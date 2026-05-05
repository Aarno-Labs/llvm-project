//===--- RefoldEngine.cpp ---------------------------------------*- C++ -*-===//
//
// This component implements the deterministic “refolding” engine that projects
// edits made to a raw preprocessed stream (B) back onto the original, partially
// expanded translation unit (TU) described by the refold map.
//
// Overview
// --------
// RefoldEngine consumes:
//   • A: original preprocessed bytes and tokens
//   • B: edited preprocessed bytes and tokens
//   • M: RefoldModel (parsed from the JSON refold map)
//
// It aligns A↔B token streams, derives edit hunks, classifies each hunk as
// TU-owned / include-owned / macro-invocation–owned, and materializes a new
// TU that incorporates edits while preserving original structure and semantics.
//
// Responsibilities
// ----------------
//   • Compute LCS-based A→B anchors and contiguous edit hunks.
//   • Attribute hunks to includes or macro call sites using M’s coverage data.
//   • Normalize and coalesce include insertions (line-local, boundary safe).
//   • Realize include expansions bottom-up, applying macro patches in-owner.
//   • Apply TU-level replacements with boundary hygiene (no token gluing).
//
// Determinism & Policy
// --------------------
//   • All iteration and sorting are stable; edits apply high→low to avoid
//   drift. • Boundary padding inserts at most one space locally when needed by
//     maximal-munch rules; internal whitespace is preserved verbatim.
//   • Errors are reported via the Logging subsystem (`fatal()/error()/...`).
//
// Public Surface
// --------------
//   • std::string Refold(...): orchestrates the end-to-end refolding and
//     returns the refolded TU text.
//   • Helper utilities: token/byte mapping, hunk builders, include realization,
//     macro-patch construction, and line-local boundary checks.
//
// Notes
// -----
//   • No RTTI or exceptions required; mirrors LLVM/Clang style.
//   • Paths are compared via canonicalization (see RefoldEngine::PathsEqual()).
//   • All indices are half-open where applicable: tokens [lo,hi), bytes [b,e).
//
// Author:
//   jeikenberry
//
//===----------------------------------------------------------------------===//

#include "RefoldLog.h"
#include "RefoldEngine.h"

#include "clang/Basic/SourceLocation.h"
#include "clang/Basic/TokenKinds.h"
#include "clang/Basic/Diagnostic.h"
#include "clang/Basic/DiagnosticOptions.h"
#include "clang/Basic/FileManager.h"
#include "clang/Basic/SourceManager.h"
#include "clang/Frontend/CompilerInvocation.h"
#include "clang/Lex/Lexer.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/Hashing.h"
#include "llvm/ADT/ScopeExit.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/FormatVariadic.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <set>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <tuple>

using namespace llvm;

namespace clang {
namespace refold {

namespace {
struct LexBoundaryToken {
  tok::TokenKind Kind = tok::unknown;
  std::string Spelling;
  size_t Begin = 0;
  size_t End = 0;
};

static std::optional<LexBoundaryToken> firstLexToken(StringRef Text,
                                                    const LangOptions &Lang);
static std::optional<LexBoundaryToken> lastLexToken(StringRef Text,
                                                   const LangOptions &Lang);
static bool needsLexicalSeparator(const LexBoundaryToken &Left,
                                  const LexBoundaryToken &Right,
                                  const LangOptions &Lang);
static bool isSeparatorGapReplacementPunctuation(tok::TokenKind Kind);

inline std::string resolveHeaderPath(const RefoldModel::IncludeItem &inc) {
  return (inc.resolvedPath && !inc.resolvedPath->empty())
             ? inc.resolvedPath->str()
             : stringutils::stripHeaderToken(inc.target).str();
}

inline bool HasLiteralMacroCalleeOrigin(
    const RefoldModel::MacroInvocation &M) {
  return M.calleeOrigin.kind == MacroCalleeOriginKind::LiteralMacroName;
}

/// Recover the spelled text of one invocation argument from the producer-side
/// callsite surface recorded on a macro invocation.
///
/// The producer stores invocation argument byte ranges in TU-relative byte
/// coordinates. The consumer-side `invText` string, however, is a local slice
/// covering only the invocation text itself. This helper therefore rebases the
/// recorded argument byte range through `invB` before slicing `invText`.
///
/// Returns `std::nullopt` when the producer did not record a usable invocation
/// text/range pair for `argIdx`, or when the recorded range does not rebase
/// cleanly into the local invocation surface. Callers must treat failure as a
/// proof failure and remain fail-closed.
static std::optional<StringRef>
TryGetInvocationArgText(const RefoldModel::MacroInvocation &mi,
                        unsigned argIdx) {
  if (!mi.invText)
    return std::nullopt;
  if (argIdx >= mi.invArgRanges.size())
    return std::nullopt;

  const auto &range = mi.invArgRanges[argIdx];
  if (!range.first || !range.second)
    return std::nullopt;

  uint64_t byteBegin = *range.first;
  uint64_t byteEnd = *range.second;
  if (byteEnd < byteBegin)
    return std::nullopt;

  if (mi.invB) {
    if (byteBegin < *mi.invB || byteEnd < *mi.invB)
      return std::nullopt;
    byteBegin -= *mi.invB;
    byteEnd -= *mi.invB;
  }

  if (byteEnd > mi.invText->size() || byteBegin > byteEnd)
    return std::nullopt;

  return StringRef(*mi.invText)
      .slice(static_cast<size_t>(byteBegin), static_cast<size_t>(byteEnd));
}

/// Return true only for the narrowly proved chunk-5 higher-order case:
///
///   * the callee of a descendant invocation comes from exactly one caller
///     formal slot in `parent`
///   * the spelled invocation text for that slot is exactly the parent formal
///     name itself (for example `F` in `APPLY(F, X)`)
///   * the slot forwards through exactly one `argRef`
///   * the slot has no tuple forwarding witnesses
///   * `argDeps` agrees with that single forwarded caller slot
///
/// This is intentionally narrower than "general higher-order callee closure".
/// We only admit the whole-formal forwarding shape that is explicitly proven by
/// the current producer contract. Any richer shape must continue to fail
/// closed until the producer emits stronger callee-slice provenance.
static bool IsWholeFormalCallerForwardSlot(
    const RefoldModel::MacroInvocation &parent, uint32_t slot) {
  if (slot >= parent.defParams.size())
    return false;

  auto templateText = TryGetInvocationArgText(parent, slot);
  if (!templateText)
    return false;
  if (templateText->trim() != parent.defParams[slot].name)
    return false;

  if (slot >= parent.argRefs.size())
    return false;
  ArrayRef<RefoldModel::InvArgRef> refs(parent.argRefs[slot]);
  if (refs.size() != 1)
    return false;

  if (slot < parent.argTupleRefs.size() && !parent.argTupleRefs[slot].empty())
    return false;

  if (slot >= parent.argDeps.size())
    return false;
  ArrayRef<uint32_t> deps(parent.argDeps[slot]);
  if (deps.size() != 1 || deps.front() != refs.front().callerParamIndex)
    return false;

  return true;
}

static std::string FormatUInt32List(ArrayRef<uint32_t> values) {
  std::string out;
  raw_string_ostream os(out);
  os << "[";
  for (size_t i = 0; i < values.size(); ++i) {
    if (i)
      os << ", ";
    os << values[i];
  }
  os << "]";
  return os.str();
}

static std::string
FormatInvArgRefList(ArrayRef<RefoldModel::InvArgRef> refs) {
  std::string out;
  raw_string_ostream os(out);
  os << "[";
  for (size_t i = 0; i < refs.size(); ++i) {
    if (i)
      os << ", ";
    os << "{caller=" << refs[i].callerParamIndex << ", bytes=["
       << refs[i].byteBegin << "," << refs[i].byteEnd << ")}";
  }
  os << "]";
  return os.str();
}

static std::string
FormatInvocationArgRange(const RefoldModel::MacroInvocation::OptByteRange &R) {
  std::string out;
  raw_string_ostream os(out);
  os << "[";
  if (R.first)
    os << *R.first;
  else
    os << "?";
  os << ",";
  if (R.second)
    os << *R.second;
  else
    os << "?";
  os << ")";
  return os.str();
}

/// Collect the uint32_t keys of an associative container in deterministic
/// ascending order.
template <typename MapLike>
static SmallVector<uint32_t, 8> CollectSortedUInt32Keys(const MapLike &mapLike) {
  SmallVector<uint32_t, 8> keys;
  keys.reserve(mapLike.size());
  for (const auto &KV : mapLike)
    keys.push_back(KV.first);
  llvm::sort(keys);
  return keys;
}

/// Collect the unique formal indices participating in a pasted token surface.
///
/// Producer metadata may mention the same formal more than once within one
/// pasted product, so callers that reason about support ledgers first need a
/// deduplicated arg-index view.
static SmallVector<uint32_t, 8>
CollectSortedUniquePasteArgIdxs(ArrayRef<RefoldModel::PPArgSpan> pasteSpans) {
  SmallVector<uint32_t, 8> argIdxs;
  for (const auto &ps : pasteSpans) {
    if (!llvm::is_contained(argIdxs, ps.argIdx))
      argIdxs.push_back(ps.argIdx);
  }
  llvm::sort(argIdxs);
  return argIdxs;
}

/// Return the sorted subset of `required` that is not present in `provided`.
static SmallVector<uint32_t, 8>
ComputeSortedMissingUInt32s(ArrayRef<uint32_t> required,
                            ArrayRef<uint32_t> provided) {
  SmallVector<uint32_t, 8> missing;
  for (uint32_t value : required) {
    if (!llvm::is_contained(provided, value))
      missing.push_back(value);
  }
  llvm::sort(missing);
  return missing;
}

/// Return true when the given invocation argument contributes to any pasted
/// token emitted by the invocation.
static bool InvocationArgTouchesPaste(const RefoldModel::MacroInvocation &MI,
                                      uint32_t argIdx) {
  for (const auto &ps : MI.pasteSpans) {
    if (ps.argIdx == argIdx)
      return true;
  }
  return false;
}

/// Return true when any arg index in `argIdxs` participates in a pasted token.
static bool AnyInvocationArgTouchesPaste(const RefoldModel::MacroInvocation &MI,
                                         ArrayRef<uint32_t> argIdxs) {
  for (uint32_t argIdx : argIdxs) {
    if (InvocationArgTouchesPaste(MI, argIdx))
      return true;
  }
  return false;
}

/// Return true when every touched paste arg in `argIdxs` belongs to the
/// supplied allow-list set. Non-paste arguments are ignored.
template <typename SetLike>
static bool AllTouchedPasteArgsAreContained(
    const RefoldModel::MacroInvocation &MI, ArrayRef<uint32_t> argIdxs,
    const SetLike &allowedArgIdxs) {
  for (uint32_t argIdx : argIdxs) {
    if (InvocationArgTouchesPaste(MI, argIdx) && !allowedArgIdxs.count(argIdx))
      return false;
  }
  return true;
}

/// Total ordering for paste-span pointer groups.
///
/// Some call sites already require concrete byte ranges and some are still
/// screening for missing ranges. Centralizing the ordering keeps all of those
/// paths consistent without changing their local preconditions.
static bool PasteSpanPtrLessByByteRange(const RefoldModel::PPArgSpan *a,
                                        const RefoldModel::PPArgSpan *b) {
  if (a->byteBegin != b->byteBegin)
    return a->byteBegin < b->byteBegin;
  if (a->byteEnd != b->byteEnd)
    return a->byteEnd < b->byteEnd;
  if (a->begin != b->begin)
    return a->begin < b->begin;
  if (a->end != b->end)
    return a->end < b->end;
  return a->argIdx < b->argIdx;
}

/// 
/// Stringification (`#arg`) is not a free inverse: comments disappear and
/// arbitrary runs of PP whitespace collapse to single spaces in the resulting
/// string literal. To avoid inventing a non-unique source spelling when we map
/// an edited string literal back into raw argument syntax, we only accept
/// payloads that are already in a simple canonical source form:
///   * leading/trailing PP whitespace removed,
///   * every inter-token whitespace run collapsed to a single ASCII space,
///   * no comments outside literals,
///   * balanced string/char literals.
///
/// If the trimmed payload is not already equal to that canonical form, the
/// inverse is ambiguous and we conservatively reject it.
static std::optional<std::string>
CanonicalizeStringifyInversePayload(StringRef raw0) {
  StringRef raw = raw0.trim();

  enum class LexState { Normal, String, Char };
  LexState state = LexState::Normal;

  std::string out;
  out.reserve(raw.size());

  bool pendingSpace = false;
  auto flushPendingSpace = [&]() {
    if (pendingSpace && !out.empty())
      out.push_back(' ');
    pendingSpace = false;
  };

  for (size_t i = 0; i < raw.size(); ++i) {
    const char c = raw[i];

    switch (state) {
    case LexState::Normal:
      if (stringutils::isWs(c)) {
        pendingSpace = !out.empty();
        continue;
      }

      if (c == '/' && i + 1 < raw.size()) {
        const char n = raw[i + 1];
        if (n == '/' || n == '*')
          return std::nullopt;
      }

      flushPendingSpace();
      out.push_back(c);
      if (c == '"')
        state = LexState::String;
      else if (c == '\'')
        state = LexState::Char;
      continue;

    case LexState::String:
      out.push_back(c);
      if (c == '\\') {
        if (i + 1 >= raw.size())
          return std::nullopt;
        out.push_back(raw[++i]);
        continue;
      }
      if (c == '"')
        state = LexState::Normal;
      continue;

    case LexState::Char:
      out.push_back(c);
      if (c == '\\') {
        if (i + 1 >= raw.size())
          return std::nullopt;
        out.push_back(raw[++i]);
        continue;
      }
      if (c == '\'')
        state = LexState::Normal;
      continue;
    }
  }

  if (state != LexState::Normal)
    return std::nullopt;

  return out;
}

enum class PasteRunInvertibilityKind {
  Unique,
  NoMatch,
  Ambiguous,
  Unsupported,
};

struct PasteRunInvertibilityCertificate {
  PasteRunInvertibilityKind kind = PasteRunInvertibilityKind::Unsupported;
  std::vector<std::string> derivedSegs;
};

static PasteRunInvertibilityCertificate prependDerivedSegment(
    PasteRunInvertibilityCertificate cert, StringRef seg) {
  if (cert.kind != PasteRunInvertibilityKind::Unique)
    return cert;

  cert.derivedSegs.insert(cert.derivedSegs.begin(), seg.str());
  return cert;
}

static PasteRunInvertibilityCertificate mergePasteRunCertificates(
    PasteRunInvertibilityCertificate lhs,
    const PasteRunInvertibilityCertificate &rhs) {
  if (lhs.kind == PasteRunInvertibilityKind::Unsupported ||
      rhs.kind == PasteRunInvertibilityKind::Unsupported) {
    lhs.kind = PasteRunInvertibilityKind::Unsupported;
    lhs.derivedSegs.clear();
    return lhs;
  }

  if (rhs.kind == PasteRunInvertibilityKind::NoMatch)
    return lhs;
  if (lhs.kind == PasteRunInvertibilityKind::NoMatch)
    return rhs;

  if (lhs.kind == PasteRunInvertibilityKind::Ambiguous ||
      rhs.kind == PasteRunInvertibilityKind::Ambiguous) {
    lhs.kind = PasteRunInvertibilityKind::Ambiguous;
    lhs.derivedSegs.clear();
    return lhs;
  }

  if (lhs.derivedSegs == rhs.derivedSegs)
    return lhs;

  lhs.kind = PasteRunInvertibilityKind::Ambiguous;
  lhs.derivedSegs.clear();
  return lhs;
}

static PasteRunInvertibilityCertificate
buildAdjacentPasteRunInvertibilityCertificate(
    StringRef aTok, StringRef bRun,
    ArrayRef<const RefoldModel::PPArgSpan *> runSpans) {
  PasteRunInvertibilityCertificate cert;
  if (runSpans.empty())
    return cert;

  std::vector<std::string> oldSegs;
  oldSegs.reserve(runSpans.size());

  std::optional<uint32_t> prevEnd;
  for (const auto *ps : runSpans) {
    if (!ps || !ps->byteBegin || !ps->byteEnd || *ps->byteEnd < *ps->byteBegin)
      return cert;
    if (static_cast<size_t>(*ps->byteEnd) > aTok.size())
      return cert;
    if (prevEnd && *prevEnd != *ps->byteBegin)
      return cert;

    StringRef oldSeg =
        aTok.substr(static_cast<size_t>(*ps->byteBegin),
                    static_cast<size_t>(*ps->byteEnd - *ps->byteBegin));
    oldSegs.push_back(oldSeg.str());
    prevEnd = *ps->byteEnd;
  }

  using MemoKey = std::tuple<size_t, size_t, bool>;
  std::map<MemoKey, PasteRunInvertibilityCertificate> memo;

  // A contiguous ## run is invertible when its edited B spelling can be split
  // into per-segment spellings such that every changed segment is isolated by
  // unchanged neighbors (or a run edge). Adjacent edited segments in the same
  // undelimited run are treated as ambiguous because no internal anchor pins
  // their boundary.
  auto solveRun = [&](auto &&self, size_t idx, size_t pos,
                      bool prevChanged)
      -> PasteRunInvertibilityCertificate {
    MemoKey key{idx, pos, prevChanged};
    auto it = memo.find(key);
    if (it != memo.end())
      return it->second;

    PasteRunInvertibilityCertificate result;
    result.kind = PasteRunInvertibilityKind::NoMatch;

    if (pos > bRun.size()) {
      memo.emplace(key, result);
      return result;
    }

    if (idx == oldSegs.size()) {
      if (pos == bRun.size())
        result.kind = PasteRunInvertibilityKind::Unique;
      memo.emplace(key, result);
      return result;
    }

    StringRef oldSeg = oldSegs[idx];
    StringRef rest = bRun.drop_front(pos);

    if (rest.starts_with(oldSeg)) {
      PasteRunInvertibilityCertificate unchanged =
          prependDerivedSegment(
              self(self, idx + 1, pos + oldSeg.size(), /*prevChanged=*/false),
              oldSeg);
      result = mergePasteRunCertificates(std::move(result), unchanged);
      if (result.kind == PasteRunInvertibilityKind::Unsupported ||
          result.kind == PasteRunInvertibilityKind::Ambiguous) {
        memo.emplace(key, result);
        return result;
      }
    }

    if (prevChanged) {
      memo.emplace(key, result);
      return result;
    }

    // Current segment edited. The next segment, if any, must remain unchanged
    // and therefore acts as the first available anchor for the edited piece.
    if (idx + 1 == oldSegs.size()) {
      PasteRunInvertibilityCertificate changed;
      changed.kind = PasteRunInvertibilityKind::Unique;
      changed.derivedSegs.push_back(rest.str());
      result = mergePasteRunCertificates(std::move(result), changed);
      memo.emplace(key, result);
      return result;
    }

    StringRef nextAnchor = oldSegs[idx + 1];
    if (nextAnchor.empty()) {
      result.kind = PasteRunInvertibilityKind::Unsupported;
      result.derivedSegs.clear();
      memo.emplace(key, result);
      return result;
    }

    for (size_t searchPos = 0;; ++searchPos) {
      size_t found = rest.find(nextAnchor, searchPos);
      if (found == StringRef::npos)
        break;

      PasteRunInvertibilityCertificate changed = prependDerivedSegment(
          self(self, idx + 1, pos + found, /*prevChanged=*/true),
          rest.take_front(found));
      result = mergePasteRunCertificates(std::move(result), changed);
      if (result.kind == PasteRunInvertibilityKind::Unsupported ||
          result.kind == PasteRunInvertibilityKind::Ambiguous)
        break;
    }

    memo.emplace(key, result);
    return result;
  };

  return solveRun(solveRun, 0, 0, /*prevChanged=*/false);
}
/// Classification for replaying an edited pasted token through producer-side
/// `paste_tokens` metadata.
///
/// `Unique` means the edited token has exactly one segmentation compatible
/// with the replay witness. `Ambiguous` means multiple different segmentations
/// are possible and the consumer must fail closed. `NoMatch` means this witness
/// cannot explain the edited token. `Unsupported` denotes malformed or
/// currently out-of-domain witness shapes.
enum class PasteReplaySegmentationKind { Unique, NoMatch, Ambiguous, Unsupported };

/// Result of replaying one edited pasted-token spelling through one original
/// replay witness. For a successful replay, `argSegments` contains replacement
/// text for argument-derived parts in witness order; literal parts are omitted
/// because they must match the edited token exactly.
struct PasteReplaySegmentationResult {
  PasteReplaySegmentationKind kind = PasteReplaySegmentationKind::Unsupported;
  std::vector<std::string> argSegments;
};

/// Merge two replay attempts for the same pasted token. If two distinct
/// successful segmentations exist, preserving the paste expression would invent
/// source structure, so the merged result is ambiguous.
static PasteReplaySegmentationResult mergePasteReplayResults(
    PasteReplaySegmentationResult lhs,
    const PasteReplaySegmentationResult &rhs) {
  if (lhs.kind == PasteReplaySegmentationKind::Unsupported ||
      rhs.kind == PasteReplaySegmentationKind::Unsupported) {
    lhs.kind = PasteReplaySegmentationKind::Unsupported;
    lhs.argSegments.clear();
    return lhs;
  }
  if (rhs.kind == PasteReplaySegmentationKind::NoMatch)
    return lhs;
  if (lhs.kind == PasteReplaySegmentationKind::NoMatch)
    return rhs;
  if (lhs.kind == PasteReplaySegmentationKind::Ambiguous ||
      rhs.kind == PasteReplaySegmentationKind::Ambiguous) {
    lhs.kind = PasteReplaySegmentationKind::Ambiguous;
    lhs.argSegments.clear();
    return lhs;
  }
  if (lhs.argSegments == rhs.argSegments)
    return lhs;
  lhs.kind = PasteReplaySegmentationKind::Ambiguous;
  lhs.argSegments.clear();
  return lhs;
}

/// Return the `paste_tokens[]` witness corresponding to a paste span.
///
/// `paste_spans[]` contains one entry per argument contribution, while
/// `paste_tokens[]` contains one entry per final pasted token. The producer
/// serializes both in token order, so this helper reconstructs the distinct
/// `(begin,end)` paste-token order from `paste_spans[]` and uses the same ordinal
/// to find the replay witness. The spelling check prevents accidental matches
/// if the metadata is malformed or from a different schema generation.
static const RefoldModel::PasteToken *findPasteTokenWitnessForSpan(
    const RefoldModel::MacroInvocation &M, const RefoldModel::PPArgSpan &Span,
    StringRef ATokSpelling) {
  if (M.pasteTokens.empty())
    return nullptr;

  SmallVector<std::pair<uint64_t, uint64_t>, 8> tokenOrder;
  for (const auto &PS : M.pasteSpans) {
    std::pair<uint64_t, uint64_t> key{PS.begin, PS.end};
    if (llvm::find(tokenOrder, key) == tokenOrder.end())
      tokenOrder.push_back(key);
  }

  auto it = llvm::find(tokenOrder,
                       std::pair<uint64_t, uint64_t>{Span.begin, Span.end});
  if (it == tokenOrder.end())
    return nullptr;
  const size_t index = static_cast<size_t>(std::distance(tokenOrder.begin(), it));
  if (index >= M.pasteTokens.size())
    return nullptr;

  const RefoldModel::PasteToken &witness = M.pasteTokens[index];
  if (witness.spelling != ATokSpelling)
    return nullptr;
  return &witness;
}

/// Segment a delimiter-free run of adjacent argument-derived paste parts.
///
/// When adjacent argument parts have no literal bytes between them, the edited
/// token spelling contains no internal boundary marker. The only accepted rule
/// here is shape preservation: the edited run must have the same total width as
/// the original run and is split by the original part widths. For example,
/// `ab | cd` may replay `wxyz` as `wx | yz`; a different total width remains
/// out of domain unless literal anchors elsewhere pin the boundaries.
static PasteReplaySegmentationResult segmentArgRunByReplayWidths(
    StringRef BRun, ArrayRef<RefoldModel::PastePart> parts) {
  PasteReplaySegmentationResult result;
  if (parts.empty())
    return result;

  if (parts.size() == 1) {
    result.kind = PasteReplaySegmentationKind::Unique;
    result.argSegments.push_back(BRun.str());
    return result;
  }

  size_t requiredLen = 0;
  for (const auto &part : parts) {
    if (part.kind != RefoldModel::PastePartKind::Arg ||
        part.byteEnd < part.byteBegin)
      return result;
    requiredLen += static_cast<size_t>(part.byteEnd - part.byteBegin);
  }
  if (requiredLen != BRun.size()) {
    result.kind = PasteReplaySegmentationKind::NoMatch;
    return result;
  }

  result.kind = PasteReplaySegmentationKind::Unique;
  size_t pos = 0;
  for (const auto &part : parts) {
    const size_t width = static_cast<size_t>(part.byteEnd - part.byteBegin);
    result.argSegments.push_back(BRun.substr(pos, width).str());
    pos += width;
  }
  return result;
}

/// Replay an edited pasted-token spelling through the producer witness.
///
/// Literal witness parts are exact anchors and must occur unchanged in B. Runs
/// of argument parts are segmented by `segmentArgRunByReplayWidths()`. If a
/// literal anchor can be placed in more than one way and those placements imply
/// different argument segments, the result is ambiguous and rejected by the
/// caller.
static PasteReplaySegmentationResult segmentPastedTokenByReplayWitness(
    StringRef BTokSpelling, const RefoldModel::PasteToken &witness) {
  PasteReplaySegmentationResult unsupported;
  if (witness.parts.empty())
    return unsupported;

  uint32_t expectedBegin = 0;
  for (const auto &part : witness.parts) {
    if (part.byteBegin != expectedBegin || part.byteEnd < part.byteBegin ||
        part.byteEnd > witness.spelling.size())
      return unsupported;
    if (part.spelling != witness.spelling.slice(part.byteBegin, part.byteEnd))
      return unsupported;
    if (part.kind == RefoldModel::PastePartKind::Arg && !part.argIndex)
      return unsupported;
    if (part.kind == RefoldModel::PastePartKind::Literal && part.argIndex)
      return unsupported;
    expectedBegin = part.byteEnd;
  }
  if (expectedBegin != witness.spelling.size())
    return unsupported;

  using MemoKey = std::pair<size_t, size_t>;
  std::map<MemoKey, PasteReplaySegmentationResult> memo;

  auto solve = [&](auto &&self, size_t partIdx,
                   size_t posB) -> PasteReplaySegmentationResult {
    MemoKey key{partIdx, posB};
    auto memoIt = memo.find(key);
    if (memoIt != memo.end())
      return memoIt->second;

    PasteReplaySegmentationResult result;
    result.kind = PasteReplaySegmentationKind::NoMatch;

    if (posB > BTokSpelling.size()) {
      memo.emplace(key, result);
      return result;
    }

    if (partIdx == witness.parts.size()) {
      if (posB == BTokSpelling.size())
        result.kind = PasteReplaySegmentationKind::Unique;
      memo.emplace(key, result);
      return result;
    }

    const RefoldModel::PastePart &part = witness.parts[partIdx];
    if (part.kind == RefoldModel::PastePartKind::Literal) {
      if (!BTokSpelling.substr(posB).starts_with(part.spelling)) {
        memo.emplace(key, result);
        return result;
      }
      result = self(self, partIdx + 1, posB + part.spelling.size());
      memo.emplace(key, result);
      return result;
    }

    size_t runEnd = partIdx;
    while (runEnd < witness.parts.size() &&
           witness.parts[runEnd].kind == RefoldModel::PastePartKind::Arg)
      ++runEnd;

    auto tryRun = [&](size_t runEndB) -> PasteReplaySegmentationResult {
      if (runEndB < posB || runEndB > BTokSpelling.size())
        return PasteReplaySegmentationResult{};
      ArrayRef<RefoldModel::PastePart> witnessParts(witness.parts);
      auto runSeg = segmentArgRunByReplayWidths(
          BTokSpelling.substr(posB, runEndB - posB),
          witnessParts.slice(partIdx, runEnd - partIdx));
      if (runSeg.kind != PasteReplaySegmentationKind::Unique)
        return runSeg;
      auto suffix = self(self, runEnd, runEndB);
      if (suffix.kind != PasteReplaySegmentationKind::Unique)
        return suffix;
      runSeg.argSegments.insert(runSeg.argSegments.end(),
                                suffix.argSegments.begin(),
                                suffix.argSegments.end());
      return runSeg;
    };

    if (runEnd == witness.parts.size()) {
      result = tryRun(BTokSpelling.size());
      memo.emplace(key, result);
      return result;
    }

    StringRef anchor = witness.parts[runEnd].spelling;
    if (anchor.empty()) {
      result.kind = PasteReplaySegmentationKind::Unsupported;
      memo.emplace(key, result);
      return result;
    }

    for (size_t found = BTokSpelling.find(anchor, posB);
         found != StringRef::npos;
         found = BTokSpelling.find(anchor, found + 1)) {
      result = mergePasteReplayResults(std::move(result), tryRun(found));
      if (result.kind == PasteReplaySegmentationKind::Unsupported ||
          result.kind == PasteReplaySegmentationKind::Ambiguous)
        break;
    }

    memo.emplace(key, result);
    return result;
  };

  return solve(solve, /*partIdx=*/0, /*posB=*/0);
}


/// Return true iff \p replacement has the exact chained-call shape
///   (<non-empty head>)(...)...
/// i.e. a balanced parenthesized callable head followed by one or more
/// balanced call-suffix groups, with only whitespace/comments between
/// groups and nothing trailing afterward.
///
/// This is used by chained-call patch extension to detect replacements
/// such as `(f)(x)` or `((f))(x)(y)`, where the final trailing source
/// call group should remain outside the replacement.
static bool shouldPreserveFinalCallSuffixGroup(StringRef replacement) {
  StringRef s = replacement.trim();
  if (s.empty())
    return false;

  size_t pos = stringutils::skipWSAndComments(s, 0);
  if (pos >= s.size() || s[pos] != '(')
    return false;

  const size_t headEnd = stringutils::findMatchingRParen(s, pos);
  if (headEnd == StringRef::npos)
    return false;

  const size_t firstInside = stringutils::skipWSAndComments(s, pos + 1);
  if (firstInside >= headEnd)
    return false;

  // Require one or more trailing call groups.
  pos = stringutils::skipWSAndComments(s, headEnd + 1);
  bool sawCallGroup = false;
  while (pos < s.size() && s[pos] == '(') {
    const size_t groupEnd = stringutils::findMatchingRParen(s, pos);
    if (groupEnd == StringRef::npos)
      return false;
    sawCallGroup = true;
    pos = stringutils::skipWSAndComments(s, groupEnd + 1);
  }

  return sawCallGroup && pos == s.size();
}

/// Extend a macro invocation's replacement end across trailing chained-call
/// suffix groups in the original source, when those groups should be absorbed
/// into the replacement.
///
/// Starting at \p invEnd, this scans forward through any immediately following
/// balanced `(...)` groups (skipping whitespace/comments between groups) and
/// returns the byte offset after the last group that should be consumed.
///
/// Consumption policy:
///   - If \p replacement still looks directly callable (for example `IDENT` or
///     `IDENT(...)`), do not consume any trailing source call groups.
///   - Otherwise, consume trailing source `(...)` groups as part of the patch.
///   - Exception: if \p replacement is itself a full parenthesized chained-call
///     surface such as `(f)(x)` or `((f))(x)(y)`, preserve the final trailing
///     source call group so the outermost call remains outside the replacement.
///
/// Returns \p invEnd unchanged if no trailing call-suffix groups should be
/// consumed or if no balanced group begins at/after \p invEnd.
static uint64_t extendChainedCallEnd(StringRef fileText, uint64_t invEnd,
                                     StringRef replacement) {
  if (invEnd > fileText.size())
    return invEnd;

  // Preserve chained call parens for replacements that still look
  // callable:
  //   * IDENT
  //   * IDENT(...)
  // In these cases, any trailing '(...)' sequences are likely
  // function-call suffixes that must remain.
  if (stringutils::isIdentifierOrSimpleCallExpr(replacement))
    return invEnd;

  // When the replacement is itself a parenthesized callable head
  // followed by one or more call groups, preserve the final trailing
  // source suffix group. This keeps chained-call forms such as
  // '(f)(x)' or '((f))(x)(y)' from consuming the call that should
  // remain outside the replacement.
  const bool preserveFinalSuffixGroup =
      shouldPreserveFinalCallSuffixGroup(replacement);

  size_t pos =
      stringutils::skipWSAndComments(fileText, static_cast<size_t>(invEnd));
  if (pos >= fileText.size() || fileText[pos] != '(')
    return invEnd;

  // Collect the end offsets of each consecutive balanced trailing `(...)`
  // suffix group after the invocation, skipping intervening whitespace/comments.
  SmallVector<uint64_t, 4> groupEnds;
  while (pos < fileText.size() && fileText[pos] == '(') {
    const size_t r = stringutils::findMatchingRParen(fileText, pos);
    if (r == StringRef::npos)
      break;
    groupEnds.push_back(static_cast<uint64_t>(r + 1));
    pos = stringutils::skipWSAndComments(fileText, r + 1);
  }

  if (groupEnds.empty())
    return invEnd;

  size_t consume = groupEnds.size();
  if (preserveFinalSuffixGroup && consume > 0)
    consume -= 1;
  return (consume == 0) ? invEnd : groupEnds[consume - 1];
}

// Slice the exact byte coverage of tokens [startTok,endTok): from the first
// token's start to the last token's end, excluding any inter-token whitespace
// that follows the final token and belongs to later untouched text.
static StringRef sliceExactTokenCoverage(ArrayRef<size_t> tokOff,
                                         ArrayRef<PPTok> toks, StringRef source,
                                         uint64_t startTok, uint64_t endTok) {
  if (tokOff.empty() || toks.empty() || source.empty() || endTok <= startTok)
    return "";

  const uint64_t tokCount = static_cast<uint64_t>(toks.size());
  uint64_t loTok = std::clamp(startTok, static_cast<uint64_t>(0), tokCount);
  uint64_t hiTok = std::clamp(endTok, loTok, tokCount);
  if (hiTok <= loTok || loTok >= tokCount)
    return "";

  size_t lo = tokOff[static_cast<size_t>(loTok)];
  const size_t lastTok = static_cast<size_t>(hiTok - 1);
  size_t hi = tokOff[lastTok] + toks[lastTok].spelling.size();

  const size_t sourceLen = source.size();
  lo = std::clamp(lo, size_t(0), sourceLen);
  hi = std::clamp(hi, lo, sourceLen);
  return source.substr(lo, hi - lo);
}

// Slice the full byte envelope of tokens [startTok,endTok): from the first
// token's start up to the next token boundary (or source end). Unlike
// sliceExactTokenCoverage(), this preserves any trailing whitespace or
// newlines that are part of the inserted B-side payload.
static StringRef sliceTokenEnvelope(ArrayRef<size_t> tokOff, StringRef source,
                                    uint64_t startTok, uint64_t endTok) {
  if (tokOff.empty() || source.empty() || endTok <= startTok)
    return "";

  const uint64_t tokCount = static_cast<uint64_t>(tokOff.size());
  uint64_t loTok = std::clamp(startTok, static_cast<uint64_t>(0), tokCount);
  uint64_t hiTok = std::clamp(endTok, loTok, tokCount);
  if (hiTok <= loTok || loTok >= tokCount)
    return "";

  const size_t sourceLen = source.size();
  size_t lo =
      std::clamp(tokOff[static_cast<size_t>(loTok)], size_t(0), sourceLen);
  size_t hi = sourceLen;
  if (hiTok < tokCount)
    hi = std::clamp(tokOff[static_cast<size_t>(hiTok)], lo, sourceLen);
  return source.substr(lo, hi - lo);
}
} // namespace

clang::LangOptions RefoldEngine::MakeLexLangOptions(llvm::StringRef langName) {
  IntrusiveRefCntPtr<DiagnosticIDs> diagIDs(new DiagnosticIDs());
  IntrusiveRefCntPtr<DiagnosticOptions> diagOpts(new DiagnosticOptions());
  auto *client = new IgnoringDiagConsumer();
  DiagnosticsEngine diags(diagIDs, diagOpts, client, /*ShouldOwnClient=*/true);

  auto invocation = std::make_shared<CompilerInvocation>();
  std::string lang = langName.empty() ? "c" : langName.str();
  std::vector<const char *> args = {"-x", lang.c_str()};
  CompilerInvocation::CreateFromArgs(*invocation, ArrayRef<const char *>(args),
                                     diags);
  return invocation->getLangOpts();
}

// ========================== Public entry points ==========================

Expected<std::string> RefoldEngine::Refold(
    const json::Object &rootJson, StringRef aSource, ArrayRef<PPTok> aToks,
    ArrayRef<size_t> aTokOff, StringRef bSource, ArrayRef<PPTok> bToks,
    ArrayRef<size_t> bTokOff, bool noLines, bool strict) {
  // Build the refold model based on the parsed JSON object.
  auto mOrErr = RefoldModel::FromJson(rootJson);
  if (!mOrErr)
    return mOrErr.takeError();

  // Construct an engine and run the instance pipeline.
  RefoldEngine engine(std::move(*mOrErr), aSource, aToks, aTokOff, bSource,
                      bToks, bTokOff, noLines, strict);
  return engine.Refold();
}

void RefoldEngine::RequestTerminalFallback(TerminalFallbackKind kind, StringRef phase,
                                    StringRef detail) const {
  terminalFallbackRequested_ = true;
  ++terminalFallbackRequestCount_;
  if (terminalFallbackKind_ == TerminalFallbackKind::Unknown) {
    terminalFallbackKind_ = kind;
  } else if (kind != TerminalFallbackKind::Unknown &&
             terminalFallbackKind_ != kind) {
    terminalFallbackKind_ = TerminalFallbackKind::MixedExcludedCases;
  }
  if (terminalFallbackReasons_.size() < 64) {
    terminalFallbackReasons_.push_back(
        llvm::formatv("{0}: {1}", phase, detail).str());
  }
  debug("fallback", "REQUEST terminal fallback: {0}: {1}", phase, detail);
}

void RefoldEngine::EnforceTheoremAuditInvariants() const {
  // Step 6 closes the loop between the theorem-audit counters and the run's
  // success/failure semantics, while step 7 fixes what those counters mean:
  // any emitted non-terminal result that is not declared,
  // explicit-proof-backed, locally discharged, lattice-resolved, and in-domain
  // violates the declared theorem domain and therefore forces terminal
  // fallback in strict mode.
  if (lastTheoremAudit_.selectorDirectBypasses != 0) {
    NoteTheoremAuditViolation(
        "selector resolved an emitted result through a direct bypass");
  }
  if (lastTheoremAudit_.emittedSelectorOnlyExceptionCarriers != 0) {
    NoteTheoremAuditViolation(
        "emitted edit relied on selector-only exception carrier");
  }
  if (lastTheoremAudit_.emittedTransitionalTheoremCarriers != 0) {
    NoteTheoremAuditViolation(
        "emitted edit carried transitional theorem-facing artifact");
  }
  if (lastTheoremAudit_.emittedUnknownClassCarriers != 0) {
    NoteTheoremAuditViolation(
        "emitted edit carried unknown-class theorem artifact");
  }
  if (lastTheoremAudit_.emittedUndischargedCarriers != 0) {
    NoteTheoremAuditViolation(
        "emitted edit carried undischarged theorem artifact");
  }
  if (lastTheoremAudit_.emittedOutOfDomainCarriers != 0) {
    NoteTheoremAuditViolation(
        "non-terminal emitted edit carried explicit out-of-domain theorem artifact");
  }
  if (lastTheoremAudit_.selectorUnresolvedCompetitions != 0) {
    NoteTheoremAuditViolation(
        "selector competition ended without a lattice-selected winner");
  }
  if (lastTheoremAudit_.nonExplicitTerminalExclusions != 0) {
    NoteTheoremAuditViolation(
        "terminal fallback was not classified as an explicit out-of-domain theorem result");
  }

  if (!strict_ || terminalFallbackRequested_ ||
      lastTheoremAudit_.theoremSatisfied) {
    return;
  }

  RequestTerminalFallback(TerminalFallbackKind::TheoremAuditInvariantViolation,
                          "theorem",
                          BuildTheoremAuditInvariantDetail());
}

void RefoldEngine::RecordTerminalFallbackTheoremAudit() const {
  if (!terminalFallbackRequested_)
    return;

  const TerminalFallbackWitness witness = BuildTerminalFallbackWitness();
  const AcceptedResultCandidate candidate =
      BuildAcceptedTerminalCandidate(witness);
  const ProofSummary &summary = candidate.proofSummary;

  // Step 8 requires the terminal exclusion to remain explicit all the way
  // through the normalized proof carrier. Merely requesting fallback is not
  // sufficient; the resulting terminal witness must classify as the named
  // explicit out-of-domain theorem result.
  const bool explicitTerminalCarrier =
      candidate.kind == AcceptedResultCandidateKind::TerminalOutOfDomain &&
      summary.inventory.currentPath ==
          AcceptedPathKind::TerminalEmitEditedPreprocessedStream &&
      summary.inventory.support ==
          AcceptanceSupportKind::ExplicitOutOfDomainClass &&
      summary.completeness.coverage ==
          CompletenessCoverageKind::ExplicitOutOfDomainClass &&
      summary.theoremDomain.kind ==
          TheoremDomainKind::ExplicitOutOfDomainClass &&
      summary.theoremDomain.hasExplicitExclusion &&
      summary.theoremDomain.explicitExclusion != TerminalFallbackKind::Unknown &&
      summary.theoremDomain.explicitExclusion == witness.kind &&
      summary.hasTerminalFallbackWitness &&
      summary.discharge.status == ProofDischargeStatus::Rejected &&
      summary.discharge.failedObligation ==
          ProofObligationKind::ExplicitOutOfDomainResultTracked &&
      summary.discharge.failureReason ==
          ProofFailureReason::ExplicitOutOfDomainResult;

  if (explicitTerminalCarrier) {
    ++lastTheoremAudit_.explicitTerminalExclusions;
    return;
  }

  ++lastTheoremAudit_.nonExplicitTerminalExclusions;
  NoteTheoremAuditViolation(llvm::formatv(
                               "terminal fallback escaped theorem-domain classification: witness={0} candidate={1}",
                               witness,
                               FormatAcceptedResultCandidate(candidate))
                               .str());
}

std::string RefoldEngine::BuildTheoremAuditInvariantDetail() const {
  const std::string firstViolation =
      lastTheoremAudit_.firstViolation.empty()
          ? std::string("<none>")
          : stringutils::showWSWithClip(lastTheoremAudit_.firstViolation, 200);

  return llvm::formatv(
             "strict theorem-audit invariant violation: firstViolation='{0}' "
             "selectorDirectBypasses={1} selectorOnlyExceptions={2} transitional={3} "
             "undischarged={4} unknownClass={5} outOfDomain={6} "
             "selectorUnresolved={7} nonExplicitTerminalExclusions={8} "
             "expansionFallbackWitnesses={9} expansionFallbackSynthesisSuccesses={10} "
             "expansionFallbackSynthesisRejections={11} expansionFallbackTerminalRescues={12}",
             firstViolation, lastTheoremAudit_.selectorDirectBypasses,
             lastTheoremAudit_.emittedSelectorOnlyExceptionCarriers,
             lastTheoremAudit_.emittedTransitionalTheoremCarriers,
             lastTheoremAudit_.emittedUndischargedCarriers,
             lastTheoremAudit_.emittedUnknownClassCarriers,
             lastTheoremAudit_.emittedOutOfDomainCarriers,
             lastTheoremAudit_.selectorUnresolvedCompetitions,
             lastTheoremAudit_.nonExplicitTerminalExclusions,
             lastTheoremAudit_.expansionFallbackWitnesses,
             lastTheoremAudit_.expansionFallbackSynthesisSuccesses,
             lastTheoremAudit_.expansionFallbackSynthesisRejections,
             lastTheoremAudit_.expansionFallbackTerminalRescues)
      .str();
}

void RefoldEngine::BuildBInsertionProvenance(ArrayRef<diffutils::Hunk> hunks) {
  // Build a structural provenance map for *token-level pure insertions*.
  //
  // A pure insertion hunk is one where no A tokens are deleted and the edit
  // consists entirely of B tokens:
  //   h.aStart == h.aEnd && h.bStart < h.bEnd
  //
  // We record:
  //   * a compact table of insertion segments (bInsertions_)
  //   * a per-B-token reverse index (bTokToInsertionId_) mapping each B token
  //     to the insertion segment that owns it (or -1 if not in an insertion)
  //   * a per-hunk index (hunkToInsertionId_) so later passes can quickly
  //     determine whether a diff hunk corresponds to a tracked insertion.
  //
  // Invariants enforced here:
  //   * insertion segments are within [0, bToks_.size())
  //   * insertion segments do not overlap in B-token space
  bInsertions_.clear();
  bTokToInsertionId_.assign(bToks_.size(), -1);
  hunkToInsertionId_.assign(hunks.size(), -1);

  for (size_t hi = 0; hi < hunks.size(); ++hi) {
    const auto &h = hunks[hi];
    if (!h.isInsertOnly())
      continue;

    const size_t b0 = static_cast<size_t>(h.bStart);
    const size_t b1 = static_cast<size_t>(h.bEnd);
    if (b1 > bToks_.size()) {
      // A refold map / token diff invariant violation: a hunk must never refer
      // to B token indices outside the lexed B stream.
      fatal("prov/ins",
            "insertion hunk out of B bounds: hunk#{0} b=[{1},{2}) bToks={3}",
            hi, b0, b1, bToks_.size());
    }

    const size_t insId = bInsertions_.size();
    BInsertionProv ins;
    ins.aGap = h.aStart;
    ins.hunkIndex = hi;
    ins.b0 = b0;
    ins.b1 = b1;
    bInsertions_.push_back(ins);
    hunkToInsertionId_[hi] = static_cast<int32_t>(insId);

    for (size_t bj = b0; bj < b1; ++bj) {
      if (bTokToInsertionId_[bj] != -1) {
        // Pure insertion hunks must form a disjoint partition of B-token
        // subranges. Any overlap indicates a diff/instrumentation bug.
        fatal("prov/ins",
              "overlapping insertion hunks at B tok {0}: existingIns={1} "
              "newIns={2}",
              bj, bTokToInsertionId_[bj], static_cast<int32_t>(insId));
      }
      bTokToInsertionId_[bj] = static_cast<int32_t>(insId);
    }
  }
}

void RefoldEngine::ClaimBInsertion(size_t insId, BInsertionClaim c,
                                   llvm::StringRef why) {
  // Mark a pure-insertion segment as "claimed" by a particular emission path.
  //
  // The global invariant is: any B-only insertion segment is emitted exactly
  // once. The claim table enforces that by requiring a single, consistent
  // claim for each insertion segment.
  //
  // "Standalone" means the segment must be emitted as part of boundary
  // insertion logic (TU/include/arm). Other claim kinds may be added in the
  // future (e.g. explicitly absorbed by a macro whole-cover patch).
  if (insId >= bInsertions_.size())
    return;
  BInsertionProv &ins = bInsertions_[insId];
  if (ins.claim == BInsertionClaim::Unclaimed) {
    // First claim wins.
    ins.claim = c;
    trace("prov/claim",
          "claim ins#{0} hunk#{1} AGap={2} B=[{3},{4}) kind={5} why={6}", insId,
          ins.hunkIndex, ins.aGap, ins.b0, ins.b1, static_cast<unsigned>(c),
          why);
    return;
  }
  if (ins.claim != c) {
    // Conflicting claims indicate a logic error (double-emission risk).
    fatal("prov/claim",
          "double-claim insertion ins#{0} hunk#{1} AGap={2} B=[{3},{4}) "
          "existing={5} new={6} why={7}",
          insId, ins.hunkIndex, ins.aGap, ins.b0, ins.b1,
          static_cast<unsigned>(ins.claim), static_cast<unsigned>(c), why);
  }
}

void RefoldEngine::PreclaimStandaloneInsertions(
    StringRef tuPath, ArrayRef<diffutils::Hunk> hunks) {
  if (bInsertions_.empty())
    return;

  for (size_t hi = 0; hi < hunks.size(); ++hi) {
    int32_t insIdI32 =
        (hi < hunkToInsertionId_.size()) ? hunkToInsertionId_[hi] : -1;
    if (insIdI32 < 0)
      continue;

    const auto &h = hunks[hi];

    // Macro call-sites have priority; if this insertion lies within a patchable
    // macro's cover, leave it unclaimed so the macro patch may absorb it.
    Owner owner = ClassifyOwnerWithSegments(tuPath, h);
    if (auto *m =
            SmallestCoveringPatchableMacro(h.aStart, h.aEnd, owner.includeId)) {
      if (m->invB && m->invE)
        continue;
    }

    ClaimBInsertion(static_cast<size_t>(insIdI32), BInsertionClaim::Standalone,
                    llvm::formatv("preclaim hunk#{0}", hi).str());
  }
}

llvm::SmallVector<std::pair<size_t, size_t>, 4>
RefoldEngine::ClipBTokenRangeAgainstClaims(size_t bTokStart,
                                           size_t bTokEnd) const {
  // Given a B-token range [bTokStart, bTokEnd), return a list of subranges
  // that are safe to emit for replacement text.
  //
  // We clip out tokens belonging to insertion segments that have been claimed
  // as Standalone. Those segments will be emitted via boundary insertion
  // patches and must not be re-emitted in macro whole-cover replacement text.
  //
  // The returned segments preserve order and collectively represent
  // [bTokStart,bTokEnd) with Standalone-claimed insertion subranges removed.
  llvm::SmallVector<std::pair<size_t, size_t>, 4> segs;
  if (bTokEnd <= bTokStart)
    return segs;

  const size_t bMax = bToks_.size();
  bTokStart = std::min(bTokStart, bMax);
  bTokEnd = std::min(bTokEnd, bMax);

  size_t i = bTokStart;
  while (i < bTokEnd) {
    // If we are currently inside a Standalone-claimed insertion segment,
    // skip that entire insertion run.
    int32_t insIdI32 =
        (i < bTokToInsertionId_.size()) ? bTokToInsertionId_[i] : -1;
    if (insIdI32 >= 0) {
      const BInsertionProv &ins = bInsertions_[static_cast<size_t>(insIdI32)];
      if (ins.claim == BInsertionClaim::Standalone) {
        i = std::min(bTokEnd, ins.b1);
        continue;
      }
    }

    // Otherwise, begin a kept segment at i and extend until we reach either
    // the end of the input range or the start of a Standalone insertion.
    const size_t segStart = i;
    ++i;
    while (i < bTokEnd) {
      int32_t nextId =
          (i < bTokToInsertionId_.size()) ? bTokToInsertionId_[i] : -1;
      if (nextId >= 0) {
        const BInsertionProv &ins = bInsertions_[static_cast<size_t>(nextId)];
        if (ins.claim == BInsertionClaim::Standalone)
          break;
      }
      ++i;
    }
    if (segStart < i)
      segs.push_back({segStart, i});
  }

  return segs;
}

std::string
RefoldEngine::SliceBSourceClippedAgainstClaims(size_t bTokStart,
                                               size_t bTokEnd) const {
  // Materialize a B-token range into bytes, excluding any Standalone-claimed
  // insertion segments.
  //
  // This is used by macro whole-cover replacement text and similar paths to
  // enforce the global "no double-emission" invariant for B-only segments.
  auto segs = ClipBTokenRangeAgainstClaims(bTokStart, bTokEnd);
  if (segs.empty())
    return std::string();

  std::string out;
  for (const auto &s : segs) {
    StringRef frag = SliceBSource(s.first, s.second);
    out.append(frag.begin(), frag.end());
  }
  return out;
}

std::string RefoldEngine::Refold() {
  // The engine is single-pass: it first attempts structural refolding, then
  // (if structural proof discharge requests fallback) resolves the post-
  // structural fallback choice between the proved intermediate expansion stage
  // and the explicit raw-B terminal carrier.
  ResetTerminalFallbackState();
  ResetAttemptStats();
  ResetTheoremAudit();

  std::string out = RunSinglePassRefold();

  // Step 6 makes the theorem audit authoritative in strict mode: once the
  // structural pass finishes, any surviving theorem-audit violation must be
  // converted into the one explicit terminal fallback rather than merely being
  // reported.
  EnforceTheoremAuditInvariants();

  if (terminalFallbackRequested_)
    out = ResolvePostStructuralFallback();

  EmitRefoldStats();
  EmitTheoremAudit();
  return out;
}

std::string RefoldEngine::RunSinglePassRefold() {
  // Make sure that when we re-lex the A-stream tokens that it matches the token
  // count as listed in the refold map JSON file.
  if (static_cast<size_t>(model_.GetTokensCountA()) != aToks_.size()) {
    fatal("tok",
          "A-stream token count mismatch: model reported {0} tokens, but lexed "
          "sequence (aToks) has {1} tokens.",
          model_.GetTokensCountA(), aToks_.size());
  }

  StringRef tuPath = model_.GetSourcePath();

  info("plan", "REFOLD START tuPath={0} aLen={1} bLen={2} aToks={3} bToks={4}",
       tuPath, aSource_.size(), bSource_.size(), aToks_.size(), bToks_.size());

  abTokHunks_.clear();
  abTokMapA2B_.clear();
  abTokMapB2A_.clear();

  // Read in the translation unit file / C source.
  std::unique_ptr<llvm::MemoryBuffer> tuBuffer;
  {
    const auto fullTuPath = lineDirs_.ToAbsolutePath(tuPath);
    auto bufOrErr = MemoryBuffer::getFile(fullTuPath);
    if (!bufOrErr) {
      // Fatal and stop: unreachable past this point.
      fatal("src/load", "failed to read C source: {0} ({1})", fullTuPath,
            bufOrErr.getError().message());
    }

    // Don't need a copy of the bytes here due to lifetime reasoning.
    tuBuffer = std::move(*bufOrErr);
  }
  StringRef tuBytes = tuBuffer->getBuffer();

  constexpr size_t MAX_COLS = 80;
  SmallString<MAX_COLS> sepBuf;
  sepBuf.assign(MAX_COLS, '-');
  StringRef sep = sepBuf;

  // 1) Generate both A and B token sequences.
  auto aSeq = MapLexemes(aToks_, aTokOff_);
  trace("lcs/aSeq", "aSeq:");
  trace("lcs/aSeq", "=====");
  logFormattedArray<StringRef>(aSeq, /* k */ MAX_COLS,
                               /* sameWidth */ false,
                               [](StringRef msg) { trace("lcs/aSeq", msg); });
  trace("lcs/aSeq", sep);

  auto bSeq = MapLexemes(bToks_, bTokOff_);
  trace("lcs/bSeq", "bSeq:");
  trace("lcs/bSeq", "=====");
  logFormattedArray<StringRef>(bSeq, /* k */ MAX_COLS,
                               /* sameWidth */ false,
                               [](StringRef msg) { trace("lcs/bSeq", msg); });
  trace("lcs/bSeq", sep);

  // 1b) Compute per-gap ownership depth for A's PP tokens.
  ownerDepthGap_ = ComputeOwnerDepthGapsForPP();
  trace("lcs/ownerGap", "ownerDepthGap:");
  trace("lcs/ownerGap", "==============");
  logFormattedArray<unsigned>(
      ownerDepthGap_, /* k */ MAX_COLS, /* sameWidth */ true,
      [](StringRef msg) { trace("lcs/ownerGap", msg); });
  trace("lcs/ownerGap", sep);

  // 2) LCS over tokens (A → B). The structured provenance profiles preserve
  // the scalar owner-depth cost used by the core objective, then add exact
  // include/conditional/macro identity and edited-side line-shape data so the
  // diff layer can suppress ambiguous repeated-token anchors and restore only
  // certified boundary-preserving frontiers.
  auto gapProvenance = ComputeLcsGapProvenanceForPP();
  auto bGapProvenance = ComputeLcsBGapProvenanceForPP();
  auto a2b = diffutils::lcsMapAB(aSeq, bSeq, gapProvenance, bGapProvenance);
  trace("lcs/a2b", "a2b:");
  trace("lcs/a2b", "====");
  logFormattedArray<int64_t>(a2b, /* k */ MAX_COLS, /* sameWidth */ true,
                             [](StringRef msg) { trace("lcs/a2b", msg); });
  trace("lcs/a2b", sep);

  // Sanity check: map must have a strict ordering.
  int64_t last = -1;
  for (size_t i = 0; i < a2b.size(); ++i) {
    int64_t j = a2b[i];
    if (j < 0)
      continue;
    if (j < last) {
      fatal("lcs/map", "non-monotone map at A[{0}]={1} after {2}", i, j, last);
    }
    last = j;
  }

  debug("lcs", "A={0} toks, B={1} toks", aSeq.size(), bSeq.size());
  size_t mapped = 0;
  for (int64_t v : a2b) {
    if (v >= 0)
      mapped++;
  }
  debug("lcs", "mapped A->B = {0} ({1:F1}%)", mapped,
        100.0 * mapped / std::max<size_t>(1U, aSeq.size()));

  info("plan", "TU={0} includes={1} macroInvocations={2} tokmap={3}", tuPath,
       model_.GetIncludes().size(), model_.GetMacroInvocations().size(),
       model_.GetTokmapByPP().size());

  // 3) Diff hunks (changed A-token intervals -> B-token intervals).
  auto hunks = diffutils::hunksFromMap(a2b, aSeq.size(), bSeq.size());

  // Normalize insert-only hunks by trimming any matched tokens that
  // accidentally appear on their edges under ambiguous token-LCS tie-breaks.

  // Build inverse map B->A for quick matched/unmatched checks.
  std::vector<int64_t> b2a(bSeq.size(), -1);
  for (size_t ai = 0; ai < a2b.size(); ++ai) {
    const int64_t bj = a2b[ai];
    if (bj >= 0 && static_cast<size_t>(bj) < b2a.size())
      b2a[static_cast<size_t>(bj)] = static_cast<int64_t>(ai);
  }

  abTokMapA2B_ = a2b;
  abTokMapB2A_ = b2a;

  size_t trimmedEdgeMatched = 0;
  for (auto &h : hunks) {
    if (!h.isInsertOnly())
      continue;

    // Insert-only hunks should contain only unmatched B tokens. Under ambiguous
    // token-LCS tie-breaks, a matched context token can end up stranded on the
    // front edge of such a hunk; trim those away.
    while (h.bStart < h.bEnd && static_cast<size_t>(h.bStart) < b2a.size() &&
           b2a[static_cast<size_t>(h.bStart)] >= 0) {
      ++h.bStart;
      ++trimmedEdgeMatched;
    }

    // Likewise trim any matched context tokens that leaked onto the back edge
    // of an insert-only hunk, leaving only the true inserted B-token interval.
    while (h.bStart < h.bEnd && static_cast<size_t>(h.bEnd - 1) < b2a.size() &&
           b2a[static_cast<size_t>(h.bEnd - 1)] >= 0) {
      --h.bEnd;
      ++trimmedEdgeMatched;
    }
  }

  if (trimmedEdgeMatched) {
    trace("hunks/norm",
          "trimmed {0} matched edge tokens from insert-only hunks",
          trimmedEdgeMatched);
  }

  // Coalesce adjacent insert-only hunks that share the same A insertion
  // position and have contiguous B spans. This commonly happens when comment
  // tokens are split into a separate insertion hunk.
  if (hunks.size() > 1) {
    std::vector<diffutils::Hunk> merged;
    merged.reserve(hunks.size());
    for (const auto &h : hunks) {
      const bool isIns = h.isInsertOnly();
      if (!merged.empty()) {
        diffutils::Hunk &prev = merged.back();
        const bool prevIns = prev.isInsertOnly();
        if (isIns && prevIns && prev.aStart == h.aStart &&
            prev.aEnd == h.aEnd && prev.bEnd == h.bStart) {
          prev.bEnd = h.bEnd;
          continue;
        }
      }
      merged.push_back(h);
    }
    if (merged.size() != hunks.size()) {
      trace("hunks/norm",
            "coalesced insert-only hunks: before={0} after={1}", hunks.size(),
            merged.size());
      hunks = std::move(merged);
    }
  }

  // Cache the token-level hunks before any owner-aware splitting so A->B
  // envelope mapping can trim boundary insertions consistently.
  abTokHunks_ = hunks;

  // Build *raw-text* byte hunks once; this enables deterministic mapping of PP
  // byte spans from A->B, without inheriting any ambiguity from token-level
  // alignment.
  //
  // This is critical for "insert-only" edits, where token-only LCS diffing can
  // place the insertion at an arbitrary stable point, corrupting subsequent
  // A->B byte span mapping.
  abByteHunks_ = BuildByteHunksFromRawText();
  BuildByteHunkPrefixDeltaCache();

  // Split replace hunks when a deterministic mixed-owner partition can be
  // proven.
  //
  // The old normalizer accepted only the two-piece case where one interior
  // A-token boundary also proved one exact interior B-token boundary. That was
  // sound, but incomplete for hunks containing more than two independently
  // realizable regions. Generalize the proof to an ordered partition:
  //
  //   * every segment consumes a non-empty A subrange;
  //   * every segment has a known TU/include/macro realizer;
  //   * every segment has an A->B token envelope inside the original hunk;
  //   * segment B envelopes are contiguous and exactly tile the original B
  //     hunk; and
  //   * the final partition contains at least two different realizers.
  //
  // This remains a normalization-only proof. Each emitted sub-hunk is still
  // validated later by the ordinary macro/include/TU classifier before any
  // source edit is accepted.
  if (hunks.size() > 0) {
    enum class HunkRealizerKind {
      Unknown,
      TU,
      Include,
      Macro,
    };

    struct HunkRealizer {
      HunkRealizerKind kind = HunkRealizerKind::Unknown;
      uint64_t id = 0;

      bool operator==(const HunkRealizer &other) const {
        return kind == other.kind && id == other.id;
      }

      bool operator!=(const HunkRealizer &other) const {
        return !(*this == other);
      }

      bool operator<(const HunkRealizer &other) const {
        if (kind != other.kind)
          return static_cast<unsigned>(kind) < static_cast<unsigned>(other.kind);
        return id < other.id;
      }
    };

    auto realizerToString = [](const HunkRealizer &r) -> std::string {
      switch (r.kind) {
      case HunkRealizerKind::TU:
        return "TU";
      case HunkRealizerKind::Include:
        return llvm::formatv("Include#{0}", r.id).str();
      case HunkRealizerKind::Macro:
        return llvm::formatv("Macro#{0}", r.id).str();
      case HunkRealizerKind::Unknown:
        break;
      }
      return "Unknown";
    };

    auto classifyHunkRealizer = [&](uint64_t aStart,
                                    uint64_t aEnd) -> HunkRealizer {
      if (aEnd <= aStart)
        return {};

      diffutils::Hunk probe{aStart, aEnd, 0, 0};
      Owner probeOwner = ClassifyOwnerWithSegments(tuPath, probe);
      if (auto *m =
              SmallestCoveringPatchableMacro(aStart, aEnd, probeOwner.includeId)) {
        if (m->invB && m->invE)
          return {HunkRealizerKind::Macro, m->id};
      }

      if (probeOwner.kind == OwnerKind::Include && probeOwner.includeId)
        return {HunkRealizerKind::Include, *probeOwner.includeId};

      if (HunkMapsToTU(aStart, aEnd, tuPath))
        return {HunkRealizerKind::TU, 0};

      return {};
    };

    struct PartitionEdge {
      uint64_t aStart = 0;
      uint64_t aEnd = 0;
      uint64_t bStart = 0;
      uint64_t bEnd = 0;
      HunkRealizer realizer;
    };

    struct PartitionStateKey {
      uint64_t bPos = 0;
      HunkRealizer firstRealizer;
      HunkRealizer lastRealizer;
      bool mixed = false;

      bool operator<(const PartitionStateKey &other) const {
        if (bPos != other.bPos)
          return bPos < other.bPos;
        if (firstRealizer != other.firstRealizer)
          return firstRealizer < other.firstRealizer;
        if (lastRealizer != other.lastRealizer)
          return lastRealizer < other.lastRealizer;
        return mixed < other.mixed;
      }
    };

    struct PartitionParent {
      bool valid = false;
      size_t edgeIndex = 0;
      PartitionStateKey prev;
      unsigned cost = 0;
    };

    auto tryBuildMixedOwnerPartition =
        [&](const diffutils::Hunk &h)
            -> std::optional<SmallVector<PartitionEdge, 8>> {
      if (!h.isReplace() || h.aEnd <= h.aStart || h.bEnd <= h.bStart)
        return std::nullopt;
      if (h.aEnd - h.aStart < 2)
        return std::nullopt;

      // If the whole hunk is already a patchable macro invocation, leave it as
      // one hunk. Splitting inside an already-proven whole macro candidate would
      // make this normalization pass compete with the macro lattice rather than
      // merely exposing otherwise independent owners.
      Owner wholeOwner = ClassifyOwnerWithSegments(tuPath, h);
      if (auto *wholeMacro = SmallestCoveringPatchableMacro(
              h.aStart, h.aEnd, wholeOwner.includeId)) {
        if (wholeMacro->invB && wholeMacro->invE)
          return std::nullopt;
      }

      const uint64_t aLen = h.aEnd - h.aStart;
      std::vector<PartitionEdge> edges;
      std::vector<std::vector<size_t>> edgesByAOffset(
          static_cast<size_t>(aLen) + 1);

      for (uint64_t aLo = h.aStart; aLo < h.aEnd; ++aLo) {
        // Prefer wider segments when several partitions have the same number of
        // pieces. This keeps source structure maximally coarse while remaining
        // deterministic.
        for (uint64_t aHi = h.aEnd; aHi > aLo; --aHi) {
          HunkRealizer realizer = classifyHunkRealizer(aLo, aHi);
          if (realizer.kind == HunkRealizerKind::Unknown)
            continue;

          auto env = MapATokRangeAToBTokenEnvelopeTrimEdgeInsertions(aLo, aHi);
          if (!env)
            continue;
          if (env->first < static_cast<size_t>(h.bStart) ||
              env->second > static_cast<size_t>(h.bEnd) ||
              env->first >= env->second)
            continue;

          PartitionEdge edge;
          edge.aStart = aLo;
          edge.aEnd = aHi;
          edge.bStart = static_cast<uint64_t>(env->first);
          edge.bEnd = static_cast<uint64_t>(env->second);
          edge.realizer = realizer;

          const size_t edgeIndex = edges.size();
          edges.push_back(edge);
          edgesByAOffset[static_cast<size_t>(aLo - h.aStart)].push_back(
              edgeIndex);
        }
      }

      std::vector<std::map<PartitionStateKey, PartitionParent>> dp(
          static_cast<size_t>(aLen) + 1);
      const PartitionStateKey startKey{/*bPos=*/h.bStart,
                                       /*firstRealizer=*/{},
                                       /*lastRealizer=*/{},
                                       /*mixed=*/false};
      dp[0][startKey] = PartitionParent{/*valid=*/true, /*edgeIndex=*/0,
                                        /*prev=*/{}, /*cost=*/0};

      for (uint64_t aOff = 0; aOff < aLen; ++aOff) {
        auto &states = dp[static_cast<size_t>(aOff)];
        if (states.empty())
          continue;

        for (const auto &state : states) {
          const PartitionStateKey &key = state.first;
          const unsigned curCost = state.second.cost;

          for (size_t edgeIndex : edgesByAOffset[static_cast<size_t>(aOff)]) {
            const PartitionEdge &edge = edges[edgeIndex];
            if (edge.bStart != key.bPos)
              continue;

            PartitionStateKey nextKey;
            nextKey.bPos = edge.bEnd;
            nextKey.lastRealizer = edge.realizer;

            if (key.firstRealizer.kind == HunkRealizerKind::Unknown) {
              nextKey.firstRealizer = edge.realizer;
              nextKey.mixed = false;
            } else {
              // Do not allow adjacent segments with the same realizer. Such
              // fragments should have been represented by one wider edge, and
              // permitting them can manufacture artificial partitions.
              if (key.lastRealizer == edge.realizer)
                continue;
              nextKey.firstRealizer = key.firstRealizer;
              nextKey.mixed = key.mixed || edge.realizer != key.firstRealizer;
            }

            auto &dst = dp[static_cast<size_t>(edge.aEnd - h.aStart)];
            const unsigned nextCost = curCost + 1;
            auto existing = dst.find(nextKey);
            if (existing == dst.end() || nextCost < existing->second.cost) {
              dst[nextKey] = PartitionParent{/*valid=*/true, edgeIndex,
                                             /*prev=*/key, nextCost};
            }
          }
        }
      }

      const auto &finalStates = dp[static_cast<size_t>(aLen)];
      auto bestFinal = finalStates.end();
      for (auto it = finalStates.begin(); it != finalStates.end(); ++it) {
        const PartitionStateKey &key = it->first;
        if (key.bPos != h.bEnd || !key.mixed)
          continue;
        if (bestFinal == finalStates.end() ||
            it->second.cost < bestFinal->second.cost) {
          bestFinal = it;
        }
      }
      if (bestFinal == finalStates.end())
        return std::nullopt;

      // Reconstruct the lowest-cost mixed-realizer path. The DP tracks mixedness
      // as part of the state, rather than choosing the cheapest path first and
      // checking mixedness afterwards. This prevents a coarse TU edge from
      // swallowing a smaller macro/include segment and suppressing a valid
      // structure-preserving split.
      SmallVector<PartitionEdge, 8> path;
      uint64_t aPos = h.aEnd;
      PartitionStateKey stateKey = bestFinal->first;
      while (aPos != h.aStart) {
        const uint64_t aOff = aPos - h.aStart;
        const auto stateIt = dp[static_cast<size_t>(aOff)].find(stateKey);
        if (stateIt == dp[static_cast<size_t>(aOff)].end() ||
            !stateIt->second.valid)
          return std::nullopt;

        const PartitionEdge &edge = edges[stateIt->second.edgeIndex];
        path.push_back(edge);
        aPos = edge.aStart;
        stateKey = stateIt->second.prev;
      }
      std::reverse(path.begin(), path.end());

      if (path.size() < 2)
        return std::nullopt;

      return path;
    };

    size_t ownerPartitionCount = 0;
    bool changed = true;
    while (changed) {
      changed = false;
      std::vector<diffutils::Hunk> splitHunks;
      splitHunks.reserve(hunks.size());

      for (const auto &h : hunks) {
        auto partition = tryBuildMixedOwnerPartition(h);
        if (!partition) {
          splitHunks.push_back(h);
          continue;
        }

        trace("hunks/norm",
              "partition mixed-owner replace hunk A=[{0},{1}) B=[{2},{3}) "
              "segments={4}",
              h.aStart, h.aEnd, h.bStart, h.bEnd, partition->size());

        for (const PartitionEdge &edge : *partition) {
          trace("hunks/norm",
                "  segment A=[{0},{1}) B=[{2},{3}) realizer={4}",
                edge.aStart, edge.aEnd, edge.bStart, edge.bEnd,
                realizerToString(edge.realizer));
          splitHunks.push_back(diffutils::Hunk{edge.aStart, edge.aEnd,
                                               edge.bStart, edge.bEnd});
        }

        ++ownerPartitionCount;
        changed = true;
      }

      if (changed) {
        hunks = std::move(splitHunks);
        abTokHunks_ = hunks;
      }
    }

    if (ownerPartitionCount) {
      trace("hunks/norm",
            "partitioned mixed-owner replace hunks: count={0} finalHunks={1}",
            ownerPartitionCount, hunks.size());
    }
  }

  // Refresh the token-level hunk cache after normalization.
  abTokHunks_ = hunks;

  // Build provenance for token-level pure insertions (B-only hunks) and
  // pre-claim standalone insertions before macro patching so whole-cover
  // replacements can deterministically avoid double-emitting insertion
  // payloads.
  BuildBInsertionProvenance(hunks);
  PreclaimStandaloneInsertions(tuPath, hunks);

  // DIAGNOSTICS: Output each hunk, when in debug mode, and also perform some
  // input sanitization.
  for (size_t i = 0; i < hunks.size(); ++i) {
    const auto &h = hunks[i];

    // Case A: The hunk is logically empty (e.g., a pure deletion)
    if (h.bStart >= h.bEnd) {
      debug("hunks", "#{0} {1:verbose} B=<empty/deleted>", i, h);
      continue;
    }

    // Case B: Hunk indices are out of bounds for the token-to-byte map
    if (h.bEnd >= bTokOff_.size()) {
      fatal("hunks",
            "#{0} {1:verbose} B=OUT-OF-BOUNDS: h.bEnd={2} map.size={3}", i, h,
            h.bEnd, bTokOff_.size());
      continue;
    }

    const size_t b0 = bTokOff_[static_cast<size_t>(h.bStart)];
    const size_t b1 = bTokOff_[static_cast<size_t>(h.bEnd)];

    // Case C: The token-to-byte map contains sentinels (virtual/synthetic
    // tokens)
    if (b0 == StringRef::npos || b1 == StringRef::npos) {
      fatal("hunks", "#{0} {1:verbose} B=SENTINEL: b0={2} b1={3}", i, h,
            (b0 == StringRef::npos ? "npos" : "valid"),
            (b1 == StringRef::npos ? "npos" : "valid"));
      continue;
    }

    // Case D: Byte offsets are inverted (corrupt map or out-of-order tokens)
    if (b1 < b0) {
      fatal("hunks", "#{0} {1:verbose} B=INVERTED-OFFSETS: b0={2} b1={3}", i, h,
            b0, b1);
      continue;
    }

    // Final physical safety clamp (prevents crashes if map is stale relative to
    // source)
    const size_t lo = std::min(b0, bSource_.size());
    const size_t hi = std::min(b1, bSource_.size());

    StringRef bfrag = bSource_.substr(lo, hi - lo);

    // Happy Path: Log the successfully extracted fragment
    std::string shown = stringutils::showWSWithClip(bfrag, 160);
    debug("hunks", "#{0} {1:verbose} B='{2}'", i, h, shown);
  }

  // 4) Classify hunks and collect per-target edits.
  std::vector<TextEdit> tuEdits;

  // Collect the set of root macro invocation ids that remain expanded in the
  // final chosen refold result. This is populated only by edits that survive
  // into the final applied text so the reported stats reflect the emitted
  // result rather than intermediate patch candidates.
  DenseSet<uint64_t> appliedExpandedMacroRootIds;
  DenseMap<uint64_t, IncludeEdits> perInclude; // includeId -> edits
  DenseMap<std::optional<uint64_t>, std::vector<MacroPatch>>
      macroPatchesByOwner;

  // Merge macro patches by macro-invocation id so multiple arg hunks compose
  // correctly.
  DenseMap<std::optional<uint64_t>, DenseMap<uint64_t, MacroPatch>>
      macroPatchByOwnerByMacroId;

  // Instrumentation helpers for diagnosing duplicated hunk material:
  // Compare B-envelope selection derived from byte hunks vs token-level a2b.
  auto tokEnvFromA2B =
      [&](uint64_t a0, uint64_t a1)
          -> std::optional<std::pair<size_t, size_t>> {
    if (a1 < a0)
      a1 = a0;
    const uint64_t aMax = static_cast<uint64_t>(a2b.size());
    a0 = std::min(a0, aMax);
    a1 = std::min(a1, aMax);

    size_t bMin = std::numeric_limits<size_t>::max();
    size_t bMax = 0;
    bool any = false;
    for (uint64_t ai = a0; ai < a1; ++ai) {
      int64_t bj = a2b[static_cast<size_t>(ai)];
      if (bj >= 0) {
        any = true;
        size_t b = static_cast<size_t>(bj);
        bMin = std::min(bMin, b);
        bMax = std::max(bMax, b);
      }
    }
    if (any)
      return std::make_pair(bMin, bMax + 1);

    // No matched tokens inside the interval: approximate using nearest mapped
    // neighbors (useful for diagnosing envelope drift, not for semantics).
    std::optional<size_t> left, right;
    for (uint64_t ai = a0; ai > 0; --ai) {
      int64_t bj = a2b[static_cast<size_t>(ai - 1)];
      if (bj >= 0) {
        left = static_cast<size_t>(bj) + 1;
        break;
      }
    }
    for (uint64_t ai = a1; ai < aMax; ++ai) {
      int64_t bj = a2b[static_cast<size_t>(ai)];
      if (bj >= 0) {
        right = static_cast<size_t>(bj);
        break;
      }
    }
    if (left && right)
      return std::make_pair(*left, *right);
    if (left)
      return std::make_pair(*left, *left);
    if (right)
      return std::make_pair(*right, *right);
    return std::nullopt;
  };

  auto traceBEnv = [&](StringRef tag, StringRef label,
                       std::optional<std::pair<size_t, size_t>> env) {
    if (!inTraceMode())
      return;
    if (!env) {
      trace(tag, "{0}: Btok=<none>", label);
      return;
    }
    size_t b0 = env->first;
    size_t b1 = env->second;
    const size_t bMax = bToks_.size();
    b0 = std::min(b0, bMax);
    b1 = std::min(b1, bMax);
    StringRef lead = SliceBSource(b0, std::min(b0 + 24, b1));
    trace(tag, "{0}: Btok=[{1},{2}) lead='{3}'", label, b0, b1,
          stringutils::showWSWithClip(lead, 220));
  };

  auto hasTopLevelCommaInReplacement = [&](StringRef text) -> bool {
    // Detect whether using this replacement text as a single macro argument
    // would change the invocation arity.
    //
    // This must be token-based rather than character-based: commas inside
    // comments, string literals, character literals, or nested delimiters do
    // not split a macro argument. Raw lexing gives us the same lexical
    // treatment the preprocessor would use for the replacement spelling.
    const SourceLocation baseLoc = SourceLocation::getFromRawEncoding(1);
    std::string lexBuf = text.str();
    lexBuf.push_back('\0');
    const char *bufStart = lexBuf.data();
    const char *bufEnd = bufStart + text.size();
    Lexer lex(baseLoc, lexLang_, bufStart, bufStart, bufEnd);

    int parenDepth = 0;
    int bracketDepth = 0;
    int braceDepth = 0;
    Token tok;

    for (;;) {
      lex.LexFromRawLexer(tok);
      if (tok.is(tok::eof))
        return false;
      if (tok.is(tok::comment))
        continue;

      switch (tok.getKind()) {
      case tok::l_paren:
        ++parenDepth;
        break;
      case tok::r_paren:
        if (parenDepth > 0)
          --parenDepth;
        break;
      case tok::l_square:
        ++bracketDepth;
        break;
      case tok::r_square:
        if (bracketDepth > 0)
          --bracketDepth;
        break;
      case tok::l_brace:
        ++braceDepth;
        break;
      case tok::r_brace:
        if (braceDepth > 0)
          --braceDepth;
        break;
      case tok::comma:
        // Only an undelimited comma would split the replacement into multiple
        // macro arguments if it were written back into the original invocation.
        if (parenDepth == 0 && bracketDepth == 0 && braceDepth == 0)
          return true;
        break;
      default:
        break;
      }
    }
  };

  auto shouldPreferTUArgEditOverMacroArgsOnly =
      [&](const RefoldModel::MacroInvocation &macro,
          const diffutils::Hunk &hunk,
          const MacroPatch &macroCandidate) -> bool {
    // Cross-domain lattice preference:
    //
    // The same PP hunk can sometimes be represented in two valid ways:
    //
    //   1. a macro args-only rewrite, e.g. ID(/*keep*/ 1) -> ID(2)
    //   2. an exact TU byte edit inside the original argument spelling,
    //      e.g. ID(/*keep*/ 1) -> ID(/*keep*/ 2)
    //
    // Prefer the TU edit when it is provably equivalent, because it preserves
    // callsite trivia that macro argument reconstruction does not retain. This
    // is intentionally not a general "TU beats macro" rule; it applies only to
    // a single direct argument occurrence whose source spelling can be edited
    // without changing macro arity or expansion multiplicity.

    // Only compete against an already-proven, structure-preserving args-only
    // macro candidate. Other macro proofs have their own stronger invariants
    // and should not be displaced by this narrow preference.
    if (macroCandidate.proofKind != MacroPatchProofKind::ArgsOnlyStandard ||
        !macroCandidate.proofValidated || !macroCandidate.structurePreserving)
      return false;

    // This preference rewrites existing source bytes inside an argument. Pure
    // insertions/deletions and empty B ranges need different anchoring rules.
    if (!hunk.isReplace() || hunk.aStart >= hunk.aEnd ||
        hunk.bStart >= hunk.bEnd)
      return false;

    // The invocation itself must be spelled in the main TU. If the invocation
    // is include-owned or lacks callsite provenance, rewriting its argument
    // bytes here would cross source ownership boundaries.
    if (!macro.invB || !macro.invE || !macro.invFile ||
        !PathsEqual(*macro.invFile, tuPath))
      return false;

    // The PP hunk must map back to a concrete TU token range. This prevents
    // choosing a TU-byte edit for tokens that only exist through macro body
    // spelling, include materialization, or another non-TU source.
    if (!HunkMapsToTU(hunk.aStart, hunk.aEnd, tuPath))
      return false;

    auto span = TUByteSpan(hunk.aStart, hunk.aEnd, tuPath);
    if (!span || span->first >= span->second)
      return false;

    // The concrete source bytes for the hunk must lie inside the invocation
    // callsite. Otherwise the edit is not an argument-local alternative to the
    // macro candidate.
    if (span->first < *macro.invB || span->second > *macro.invE)
      return false;

    std::optional<uint32_t> touchedArgIdx;
    for (const auto &occ : macro.argSpans) {
      if (occ.kind != PPArgSpanKind::Standard)
        continue;

      // Find the single STANDARD expansion occurrence that covers the PP hunk.
      // If more than one argument occurrence covers it, the hunk is ambiguous
      // and must stay on the existing macro proof path.
      if (occ.begin <= hunk.aStart && hunk.aEnd <= occ.end) {
        if (touchedArgIdx)
          return false;
        touchedArgIdx = occ.argIdx;
      }
    }
    if (!touchedArgIdx)
      return false;

    unsigned directStandardOccurrences = 0;
    for (const auto &occ : macro.argSpans) {
      if (occ.argIdx != *touchedArgIdx)
        continue;

      // Editing the argument source changes every expansion of that formal in
      // this invocation. Therefore the preference is sound only when the formal
      // appears exactly once, and only as a direct STANDARD substitution.
      if (occ.kind != PPArgSpanKind::Standard)
        return false;
      ++directStandardOccurrences;
    }

    // Stringify and paste uses transform the argument spelling before it reaches
    // the PP output. A direct TU argument edit is not equivalent to an args-only
    // reconstruction for those cases.
    for (const auto &occ : macro.stringifySpans)
      if (occ.argIdx == *touchedArgIdx)
        return false;
    for (const auto &occ : macro.pasteSpans)
      if (occ.argIdx == *touchedArgIdx)
        return false;

    if (directStandardOccurrences != 1)
      return false;

    if (*touchedArgIdx >= macro.invArgRanges.size())
      return false;
    const auto &argRange = macro.invArgRanges[*touchedArgIdx];
    if (!argRange.first || !argRange.second ||
        *argRange.second < *argRange.first)
      return false;

    // The source byte span must be contained in the original argument spelling,
    // not merely somewhere inside the invocation parentheses.
    if (span->first < *argRange.first || span->second > *argRange.second)
      return false;

    StringRef replacement = sliceExactTokenCoverage(
        bTokOff_, bToks_, bSource_, hunk.bStart, hunk.bEnd);

    // Replacing an argument subrange with a top-level comma would split the
    // original invocation argument list. That is a semantic arity change, so it
    // must remain on the macro reconstruction/fallback path.
    if (hasTopLevelCommaInReplacement(replacement))
      return false;

    trace("select/lattice",
          "prefer TU arg edit over macro args-only: macro id={0} name='{1}' "
          "argIdx={2} hunk A=[{3},{4}) B=[{5},{6}) src=[{7},{8})",
          macro.id, macro.name, *touchedArgIdx, hunk.aStart, hunk.aEnd,
          hunk.bStart, hunk.bEnd, span->first, span->second);
    return true;
  };

  // Iterate over all hunks:
  for (size_t i = 0; i < hunks.size(); ++i) {
    const auto &h = hunks[i];

    // Shape info (pure insert/delete/replace) – LOG ONLY
    bool isIns = h.isInsertOnly();
    bool isDel = h.isDeleteOnly();
    bool isRep = h.isReplace();
    debug("classify", "#{0} shape: isIns={1} isDel={2} isRep={3} {4}", i, isIns,
          isDel, isRep, h);

    // a) Segment-aware owner classification: this decides TU vs include vs “no
    // segment”.
    debug("classify", "#{0} -> calling classifyOwnerWithSegments {1}", i, h);
    Owner owner = ClassifyOwnerWithSegments(tuPath, h);
    debug("classify",
          "#{0} ownerFromSegments kind={1} includeId={2} condArmId={3} {4}", i,
          owner.kind, owner.includeId, owner.condArmId, h);

    // b) Macro call-site still has priority over TU/include
    if (auto *m =
            SmallestCoveringPatchableMacro(h.aStart, h.aEnd, owner.includeId)) {
      if (m->invB && m->invE) {
        debug("classify",
              "#{0} -> MACRO invText={1} owner={2} invFile={3} {4})", i,
              m->invText, m->ownerIncludeId, m->invFile, h);
        bool appliedMacroPatch = false;
        for (const RefoldModel::MacroInvocation *target = m;
             target != nullptr;) {
          auto &byMacroId = macroPatchByOwnerByMacroId[target->ownerIncludeId];

          // Coalesce callsite patches by physical callsite span
          // (inv_file/inv_b/inv_e), not by macro invocation item id.
          std::optional<uint64_t> existingKey;
          // Determinism: byMacroId is a DenseMap; if multiple entries share the
          // same invocation span, pick the smallest key.
          for (const auto &kv : byMacroId) {
            const MacroPatch &p = kv.second;
            if (p.invStart == *target->invB && p.invEnd == *target->invE) {
              if (!existingKey || kv.first < *existingKey)
                existingKey = kv.first;
            }
          }
          const uint64_t patchKey = existingKey.value_or(target->id);
          auto existingIt = byMacroId.find(patchKey);
          const bool hadExistingCallsitePatch =
              (existingIt != byMacroId.end()) &&
              InvocationSpanMatchesCallsitePrefix(
                  StringRef(existingIt->second.replacement), *target);
          const std::string prevCallsiteReplacement =
              (existingIt != byMacroId.end()) ? existingIt->second.replacement
                                              : std::string();

          // If found, use the existing callsite replacement. For an existing
          // non-callsite (expanded) patch, keep the original invocation text as
          // the preservation base so later hunks can still attempt args-only /
          // DAG reconstruction back to the callsite.
          std::string currentInvText =
              ((existingIt != byMacroId.end()) && hadExistingCallsitePatch)
                  ? existingIt->second.replacement
                  : (target->invText ? target->invText->str() : "");

          if (inTraceMode()) {
            // Envelope diagnostics: compare byte-hunk-derived envelopes vs
            // token-level a2b-derived envelopes for both the macro cover and
            // the current hunk. Disagreements or envelopes that start on
            // boundary insertions are a common root cause for duplicated
            // insertion material in whole-cover fallback.
            trace("instr/macro",
                  "macro id={0} name='{1}' coverA=[{2},{3}) hunkA=[{4},{5}) "
                  "hunkB=[{6},{7})",
                  target->id, target->name, target->cover.begin,
                  target->cover.end, h.aStart, h.aEnd, h.bStart, h.bEnd);
            traceBEnv("instr/macro", "cover byte",
                      MapATokRangeAToBTokenEnvelope(target->cover.begin,
                                                    target->cover.end));
            traceBEnv("instr/macro", "cover a2b",
                      tokEnvFromA2B(target->cover.begin, target->cover.end));
            traceBEnv("instr/macro", "hunk byte",
                      MapATokRangeAToBTokenEnvelope(h.aStart, h.aEnd));
            traceBEnv("instr/macro", "hunk a2b",
                      tokEnvFromA2B(h.aStart, h.aEnd));
            traceBEnv("instr/macro", "hunk diff",
                      std::make_optional(
                          std::make_pair(static_cast<size_t>(h.bStart),
                                         static_cast<size_t>(h.bEnd))));
          }

          // Try to build a whole-cover replacement at the current target
          // invocation. If successful, install/replace the callsite patch for
          // this macro id and stop climbing the caller chain.
          auto updated = BuildMacroInvocationPatchWholeCover(
              *target, h, currentInvText, macroPatchByOwnerByMacroId);
          if (updated) {
            if (existingIt == byMacroId.end() &&
                shouldPreferTUArgEditOverMacroArgsOnly(*target, h, *updated)) {
              trace("select/lattice",
                    "defer macro args-only patch for inv id={0} name='{1}' "
                    "so exact TU argument edit can compete",
                    target->id, target->name);
              break;
            }

            if (hadExistingCallsitePatch) {
              if (prevCallsiteReplacement == updated->replacement)
                trace("macro", "callsite patch reused inv id={0}", target->id);
              else
                trace("macro", "callsite patch overwritten inv id={0}",
                      target->id);
            }
            const Owner currentPatchOwner =
                NormalizeHunkOwnerForPatch(tuPath, h);
            if (existingIt != byMacroId.end())
              CarryMacroPatchOwnerCertificate(*updated, existingIt->second);
            StampMacroPatchOwnerWitness(*updated, currentPatchOwner);
            updated->macroId = patchKey;
            byMacroId[patchKey] = std::move(*updated);
            appliedMacroPatch = true;
            break;
          }

          // No deterministic whole-cover patch at this child invocation.
          // Climb to the immediate caller macro and retry at that enclosing
          // callsite, so the edit can be represented at a higher macro layer
          // if needed.
          if (!target->callerMacroId)
            break;
          const RefoldModel::MacroInvocation *parent =
              FindMacroInvocationById(*target->callerMacroId);
          if (!parent)
            break;
          trace("macro/select",
                "retry ancestor macro id={0} name='{1}' after child id={2} "
                "name='{3}' produced no deterministic patch",
                parent->id, parent->name, target->id, target->name);
          target = parent;
        }

        if (appliedMacroPatch)
          continue;
      }
    }

    // c) Special case: pure insertions exactly at the boundary between sibling
    // includes that share a common parent. In this case, per policy, the
    // insertion should be attached to the *parent* include so that the
    // refolded C places it between `#include` lines, not inside any child.
    if (isIns) {
      const RefoldModel::IncludeItem *parentBoundaryInc =
          BoundaryParentIncludeForPureInsertion(h);
      if (parentBoundaryInc) {
        auto [it, _] =
            perInclude.try_emplace(parentBoundaryInc->id, parentBoundaryInc);
        IncludePatch patch = BuildIncludeInsertionPatch(*parentBoundaryInc, h);
        patch.ownerHasCondArmCert = owner.condArmId.has_value();
        patch.ownerCondArmIdCert = owner.condArmId.value_or(0);
        it->second.Add(std::move(patch));
        debug("classify",
              "#{0} -> INCLUDE(parent-boundary) inc={1} ({2}) patch={3}", i,
              parentBoundaryInc->id, parentBoundaryInc->resolvedPath, patch);
        continue;
      }

      if (owner.kind == OwnerKind::Include && owner.includeId) {
        std::optional<uint64_t> firstCond =
            model_.FirstConditionalArmStartA(*owner.includeId);
        if (firstCond && h.aStart <= *firstCond) {
          const RefoldModel::IncludeItem *inc =
              model_.GetIncludeById(*owner.includeId);
          // NOTE: `inc` cannot be null if owner has an `includeId`
          auto [it, _] = perInclude.try_emplace(inc->id, inc);
          IncludePatch patch = BuildIncludeInsertionPatch(*inc, h);
          patch.ownerHasCondArmCert = owner.condArmId.has_value();
          patch.ownerCondArmIdCert = owner.condArmId.value_or(0);
          it->second.Add(std::move(patch));
          debug("classify",
                "#{0} → INCLUDE(before first cond) include={1} patch={2}", i,
                inc->id, patch);
          continue;
        }
      }
    }

    debug("classify",
          "#{0} no macro/boundary owner, proceeding with owner.kind={1} "
          "includeId={2}",
          i, owner.kind, owner.includeId);

    // d) Include-owned edit (segment policy already enforced in
    // classifyOwnerWithSegments).
    if (owner.kind == OwnerKind::Include && owner.includeId) {
      const RefoldModel::IncludeItem *inc =
          model_.GetIncludeById(*owner.includeId);
      // NOTE: `inc` cannot be null if owner has an `includeId`
      const std::string incPath = resolveHeaderPath(*inc);

      debug("classify", "#{0} -> INCLUDE id={1} path={2}  {3} (via segments)",
            i, inc->id, incPath, h);

      auto [it, _] = perInclude.try_emplace(inc->id, inc);
      IncludePatch patch = BuildIncludeInsertionPatch(*inc, h);
      patch.ownerHasCondArmCert = owner.condArmId.has_value();
      patch.ownerCondArmIdCert = owner.condArmId.value_or(0);
      debug("include/patch", "#{0} INC {1} patch={2}", i, h, patch);
      it->second.Add(std::move(patch));
      continue;
    }

    // e) TU edit? (segments + existing TU mapping cooperate here).
    // We rely on the token→file map as the source of truth for TU ownership.
    // Segment classification is only used to detect include-owned edits; it
    // should not force a hunk into the TU if any mapped token belongs to a
    // header. So only treat it as TU when the hunk map says so.
    bool mapsToTU = HunkMapsToTU(h.aStart, h.aEnd, tuPath);
    debug("classify",
          "#{0} hunkMapsToTU={1} {2} owner.kind={3} owner.includeId={4}", i,
          mapsToTU, h, owner.kind, owner.includeId);

    if (owner.kind == OwnerKind::TU && !mapsToTU) {
      // Deterministic rule: TU ownership must be supported by provenance. If
      // tokmap-based evidence does not indicate TU ownership, do not force TU
      // edits (even for insertions). Leave owner unresolved so strict mode can
      // surface the deficiency.
      owner = Owner::Unknown();
    }

    if (mapsToTU) {
      if (owner.kind == OwnerKind::Include && owner.includeId) {
        debug(
            "classify",
            "#{0} DIAGNOSTIC: tokmap says TU but segments say INCLUDE(id={1}); "
            "will still treat as TU (tokmap wins).",
            i, owner.includeId);
      }
      auto span = TUByteSpan(h.aStart, h.aEnd, tuPath); // [b,e)
      if (span) {
        debug("classify", "#{0} TU-byteSpan=[{1},{2}) for A[{3},{4})", i,
              span->first, span->second, h.aStart, h.aEnd);

        std::string repl;
        const uint64_t rawTUStart = span->first;
        const uint64_t rawTUEnd = span->second;

        if (h.bStart < h.bEnd) {
          StringRef bSlice =
              h.isInsertOnly()
                  ? sliceTokenEnvelope(bTokOff_, bSource_, h.bStart, h.bEnd)
                  : sliceExactTokenCoverage(bTokOff_, bToks_, bSource_,
                                            h.bStart, h.bEnd);
          repl.assign(bSlice.data(), bSlice.data() + bSlice.size());
          const size_t b0 = bTokOff_[static_cast<size_t>(h.bStart)];

          // Token-envelope byte ranges begin at the first inserted token, so
          // they do not include any spaces or tabs that appear immediately
          // before that token in B on the same line. For a zero-width TU
          // insertion, preserve those preceding spaces or tabs when forming
          // the inserted text, unless equivalent spacing is already present
          // immediately to the left of the insertion point in the TU.
          if (span->first == span->second && h.bStart > 0) {
            size_t p = b0;
            while (p > 0) {
              char c = bSource_[p - 1];
              if (c == ' ' || c == '\t') {
                --p;
                continue;
              }
              break;
            }
            if (p < b0) {
              bool hasSpaceLeft =
                  (span->first > 0 &&
                   (tuBytes[span->first - 1] == ' ' ||
                    tuBytes[span->first - 1] == '\t'));
              if (!hasSpaceLeft)
                repl.insert(0, std::string(bSource_.data() + p, b0 - p));
            }
          }
        }

        bool consumedSeparatorGapForPunctuation = false;

        // A zero-width PP insertion can map to the right edge of an existing
        // whitespace separator in the TU. For separator punctuation such as a
        // comma, the source edit is not "insert after the old gap"; it is
        // "replace the old separator gap with the new separator spelling".
        //
        // Only perform that span correction when every byte being consumed is
        // ordinary horizontal TU whitespace between two real tokens. Includes
        // and macro invocation spellings are excluded so this never steals
        // whitespace that belongs to a spelled artifact, and the lexer check
        // proves that attaching the punctuation to the left token preserves
        // tokenization.
        if (h.isInsertOnly() && span->first == span->second && !repl.empty() &&
            !stringutils::isWs(repl.front()) && span->first > 0 &&
            span->first < tuBytes.size() &&
            (tuBytes[span->first - 1] == ' ' ||
             tuBytes[span->first - 1] == '\t') &&
            !stringutils::isWs(tuBytes[span->first])) {
          auto intervalOverlapsSpelledArtifact =
              [&](uint64_t begin, uint64_t end) -> bool {
            for (const auto &inc : model_.GetIncludes()) {
              if (!PathsEqual(inc.sitePath, tuPath))
                continue;
              if (inc.siteB < end && begin < inc.siteE)
                return true;
            }
            for (const auto &m : model_.GetMacroInvocations()) {
              if (m.invFile && !m.invFile->empty() &&
                  !PathsEqual(*m.invFile, tuPath))
                continue;
              if (!m.invB || !m.invE)
                continue;
              if (*m.invB < end && begin < *m.invE)
                return true;
            }
            return false;
          };

          uint64_t gapBegin = span->first;
          while (gapBegin > 0 && (tuBytes[gapBegin - 1] == ' ' ||
                                  tuBytes[gapBegin - 1] == '\t'))
            --gapBegin;

          if (gapBegin < span->first &&
              !intervalOverlapsSpelledArtifact(gapBegin, span->first)) {
            std::optional<LexBoundaryToken> leftTok =
                lastLexToken(tuBytes.take_front(gapBegin), lexLang_);
            std::optional<LexBoundaryToken> rightTok =
                firstLexToken(tuBytes.drop_front(span->first), lexLang_);
            std::optional<LexBoundaryToken> replFirstTok =
                firstLexToken(StringRef(repl), lexLang_);
            std::optional<LexBoundaryToken> replLastTok =
                lastLexToken(StringRef(repl), lexLang_);

            // Consuming the original separator gap is only
            // whitespace-preserving if the replacement is lexically valid on
            // both sides of the gap: the inserted punctuation must be able to
            // attach to the left token, and the replacement text must still
            // provide any separator required before the original right token.
            if (leftTok && rightTok && replFirstTok && replLastTok &&
                leftTok->End == gapBegin && rightTok->Begin == 0 &&
                isSeparatorGapReplacementPunctuation(replFirstTok->Kind) &&
                !needsLexicalSeparator(*leftTok, *replFirstTok, lexLang_) &&
                !needsLexicalSeparator(*replLastTok, *rightTok, lexLang_)) {
              debug("edit/tu",
                    "TU insertion consumes ordinary separator gap [{0},{1}) "
                    "for punctuation '{2}'",
                    gapBegin, span->first,
                    stringutils::showWSWithClip(replFirstTok->Spelling, 40));
              span->first = gapBegin;
              consumedSeparatorGapForPunctuation = true;
            }
          }
        }

        // If our TU span stops at an identifier and is immediately followed by
        // a "(...)" chain, decide whether to consume it or preserve it based on
        // the replacement.
        if (span->first < span->second) {
          const uint64_t oldEnd = span->second;
          const uint64_t extEnd = extendChainedCallEnd(tuBytes, oldEnd, repl);
          if (extEnd != oldEnd) {
            debug("edit/tu",
                  "TU extend trailing call/arg chain [{0},{1}) -> [{0},{2})",
                  span->first, oldEnd, extEnd);
            span->second = extEnd;
          }
        }

        // Is this span replacing a TU "gap" (bytes that are all whitespace)?
        std::string original;
        if (span->second > span->first) {
          original.assign(tuBytes.data() + span->first,
                          tuBytes.data() + span->second);
        } else if (span->second < span->first) {
          fatal("tu/span", "invalid TU byte span: [{0},{1})", span->first,
                span->second);
        }

        bool replacingGap =
            !original.empty() && stringutils::isWhitespace(original);

        // If we’re replacing a non-empty TU gap and the inserted text doesn’t
        // start with WS, prefix EXACTLY ONE space from the gap to preserve
        // “return injected” (no double spaces).
        if (replacingGap && !consumedSeparatorGapForPunctuation &&
            !repl.empty() && !stringutils::isWs(repl.front()))
          repl.insert(repl.begin(), ' ');

        // Final boundary spacing fixup:
        // - On the left, only let PadAtBoundaries add a space if we did not
        //   already preserve whitespace from a replaced TU gap; otherwise we
        //   could duplicate spacing.
        // - On the right, always allow padding if the replacement would
        //   otherwise glue to the following TU text.
        std::string padded =
            PadAtBoundaries(tuBytes, static_cast<size_t>(span->first),
                            static_cast<size_t>(span->second), std::move(repl),
                            /*allowLeft*/ !replacingGap,
                            /*allowRight*/ true);

        debug("classify",
              "#{0} -> TU  bytes=[{1},{2}) rawRepl='{3}' paddedRepl='{4}'", i,
              span->first, span->second, stringutils::showWSWithClip(repl, 160),
              stringutils::showWSWithClip(padded, 160));

        ResyncOutcome ro = ApplyResyncOrPend(tuBytes, span->first, span->second,
                                             padded, tuPath);
        TextEdit edit{span->first, span->second, std::move(ro.text),
                      std::move(ro.pending), std::nullopt, {}};
        edit.isDirectTUHunkEdit = true;
        edit.directTUHunkIndex = i;
        edit.directTUHunkAStart = h.aStart;
        edit.directTUHunkAEnd = h.aEnd;
        edit.directTUHunkBStart = h.bStart;
        edit.directTUHunkBEnd = h.bEnd;
        edit.directTURawStart = rawTUStart;
        edit.directTURawEnd = rawTUEnd;
        edit.directTUFinalStart = span->first;
        edit.directTUFinalEnd = span->second;

        AttachAcceptedResultCarrier(
            edit, BuildAcceptedTUTextEditCandidate(
                      AcceptedPathKind::TUByteSpanMappedEdit, span->first,
                      span->second, StringRef(padded)));
        tuEdits.push_back(std::move(edit));
        continue;
      } else {
        debug("classify",
              "#{0} TU mapping had nullopt for span; TU edit skipped (behavior "
              "unchanged).",
              i);
      }
    }

    // f) Ownership resolution failed.
    //
    // Step 5 reclassifies this as a theorem-boundary case rather than a hard
    // internal error: once macro ownership, include ownership, and truthful
    // TU ownership all fail, the engine may still salvage the edit via a
    // declared TU byte-span class. If that last deterministic TU realization
    // also fails, the code below requests the named terminal fallback
    // OwnerUnresolvedNoTUAnchor.
    //
    // Do not abort in strict mode here. Strict mode is enforced later by the
    // theorem audit and terminal-fallback machinery, not by crashing before
    // the explicit out-of-domain boundary is recorded.
    debug("classify",
          "#{0} owner unresolved (not TU/macro/segment). Conservative TU-only "
          "attempt (no include realization).",
          i);

    if (auto span = TUByteSpan(h.aStart, h.aEnd, tuPath)) { // [b, e)
      std::string repl;
      const uint64_t rawTUStart = span->first;
      const uint64_t rawTUEnd = span->second;
      if (isDel) {
        repl = "";
      } else {
        StringRef bSlice =
            h.isInsertOnly()
                ? sliceTokenEnvelope(bTokOff_, bSource_, h.bStart, h.bEnd)
                : sliceExactTokenCoverage(bTokOff_, bToks_, bSource_, h.bStart,
                                          h.bEnd);
        repl.assign(bSlice.data(), bSlice.data() + bSlice.size());
      }

      // This patch inserts B text at a zero-width TU site: the TU span is
      // empty, but the hunk contributes one or more B tokens. Token-envelope
      // byte ranges begin at the first inserted token, so they do not include
      // any spaces or tabs that appear immediately before that token in B on
      // the same line. Preserve those preceding spaces/tabs when forming the
      // inserted text, unless equivalent spacing is already present immediately
      // to the left of the insertion point in the TU.
      if (!isDel && span->first == span->second && h.bStart < h.bEnd &&
          h.bStart > 0) {
        const size_t bTokStart = static_cast<size_t>(h.bStart);
        const size_t b0 = bTokOff_[bTokStart];
        size_t p = b0;
        while (p > 0) {
          char c = bSource_[p - 1];
          if (c == ' ' || c == '\t') {
            --p;
            continue;
          }
          break;
        }
        if (p < b0) {
          const bool tuHasSpaceLeft =
              span->first > 0 && (tuBytes[span->first - 1] == ' ' ||
                                  tuBytes[span->first - 1] == '\t');
          if (!tuHasSpaceLeft)
            repl.insert(0, std::string(bSource_.data() + p, b0 - p));
        }
      }

      bool consumedSeparatorGapForPunctuation = false;

      // A zero-width PP insertion can map to the right edge of an existing
      // whitespace separator in the TU. For separator punctuation such as a
      // comma, widen the replacement span over that exact ordinary separator
      // gap so the edit replaces the gap instead of inserting after it.
      //
      // The widening is deliberately narrow: only spaces/tabs immediately to
      // the left of the anchor may be consumed, the right side must be a real
      // non-whitespace TU byte, the gap must not overlap include/macro spelling
      // artifacts, and the lexer must prove the inserted punctuation can attach
      // to the left token without changing tokenization.
      if (!isDel && h.isInsertOnly() && span->first == span->second &&
          !repl.empty() && !stringutils::isWs(repl.front()) &&
          span->first > 0 && span->first < tuBytes.size() &&
          (tuBytes[span->first - 1] == ' ' ||
           tuBytes[span->first - 1] == '\t') &&
          !stringutils::isWs(tuBytes[span->first])) {
        auto intervalOverlapsSpelledArtifact =
            [&](uint64_t begin, uint64_t end) -> bool {
          for (const auto &inc : model_.GetIncludes()) {
            if (!PathsEqual(inc.sitePath, tuPath))
              continue;
            if (inc.siteB < end && begin < inc.siteE)
              return true;
          }
          for (const auto &m : model_.GetMacroInvocations()) {
            if (m.invFile && !m.invFile->empty() &&
                !PathsEqual(*m.invFile, tuPath))
              continue;
            if (!m.invB || !m.invE)
              continue;
            if (*m.invB < end && begin < *m.invE)
              return true;
          }
          return false;
        };

        uint64_t gapBegin = span->first;
        while (gapBegin > 0 && (tuBytes[gapBegin - 1] == ' ' ||
                                tuBytes[gapBegin - 1] == '\t'))
          --gapBegin;

        if (gapBegin < span->first &&
            !intervalOverlapsSpelledArtifact(gapBegin, span->first)) {
          std::optional<LexBoundaryToken> leftTok =
              lastLexToken(tuBytes.take_front(gapBegin), lexLang_);
          std::optional<LexBoundaryToken> rightTok =
              firstLexToken(tuBytes.drop_front(span->first), lexLang_);
          std::optional<LexBoundaryToken> replFirstTok =
              firstLexToken(StringRef(repl), lexLang_);
          std::optional<LexBoundaryToken> replLastTok =
              lastLexToken(StringRef(repl), lexLang_);

          // Consuming the original separator gap is only whitespace-preserving
          // if the replacement is lexically valid on both sides of the gap:
          // the inserted punctuation must be able to attach to the left token,
          // and the replacement text must still provide any separator required
          // before the original right token.
          if (leftTok && rightTok && replFirstTok && replLastTok &&
              leftTok->End == gapBegin && rightTok->Begin == 0 &&
              isSeparatorGapReplacementPunctuation(replFirstTok->Kind) &&
              !needsLexicalSeparator(*leftTok, *replFirstTok, lexLang_) &&
              !needsLexicalSeparator(*replLastTok, *rightTok, lexLang_)) {
            debug("edit/tu",
                  "TU conservative insertion consumes ordinary separator gap "
                  "[{0},{1}) for punctuation '{2}'",
                  gapBegin, span->first,
                  stringutils::showWSWithClip(replFirstTok->Spelling, 40));
            span->first = gapBegin;
            consumedSeparatorGapForPunctuation = true;
          }
        }
      }

      // If our TU span stops at an identifier and is immediately followed by a
      // "(...)" chain, decide whether to consume it or preserve it based on the
      // replacement.
      if (span->first < span->second) {
        const uint64_t oldEnd = span->second;
        const uint64_t extEnd = extendChainedCallEnd(tuBytes, oldEnd, repl);
        if (extEnd != oldEnd) {
          debug("edit/tu",
                "TU extend trailing call/arg chain [{0},{1}) -> [{0},{2})",
                span->first, oldEnd, extEnd);
          span->second = extEnd;
        }
      }

      // If we are replacing whitespace-only text in the TU, we prefer to
      // preserve the existing TU gap whitespace rather than introducing new
      // whitespace from B.
      bool replacingGap = false;
      if (span->first < span->second) {
        std::string original(tuBytes.data() + span->first,
                             tuBytes.data() + span->second);
        replacingGap = !original.empty() && stringutils::isWhitespace(original);
        if (replacingGap && !consumedSeparatorGapForPunctuation) {
          // Preserve exactly the gap as the replacement.
          repl = std::move(original);
        }
      } else if (span->second < span->first) {
        fatal("tu/span", "invalid TU byte span: [{0},{1})", span->first,
              span->second);
      }

      // If this patch replaces a non-empty whitespace gap in the TU, and the
      // replacement text does not already begin with whitespace, prefix a
      // single space so adjacent tokens remain separated. Add only one space,
      // even if the original gap was wider, to avoid duplicating spacing.
      if (replacingGap && !consumedSeparatorGapForPunctuation &&
          !repl.empty() && !stringutils::isWs(repl.front()))
        repl.insert(repl.begin(), ' ');

      // Keep a copy for logging; PadAtBoundaries consumes via move.
      std::string rawRepl = repl;

      std::string padded =
          PadAtBoundaries(tuBytes, static_cast<size_t>(span->first),
                          static_cast<size_t>(span->second), std::move(repl),
                          /*allowLeft*/ !replacingGap,
                          /*allowRight*/ true);

      debug("classify",
            "#{0} -> TU (conservative) bytes=[{1},{2}) rawRepl='{3}' "
            "paddedRepl='{4}'",
            i, span->first, span->second,
            stringutils::showWSWithClip(rawRepl, 160),
            stringutils::showWSWithClip(padded, 160));

      ResyncOutcome ro =
          ApplyResyncOrPend(tuBytes, span->first, span->second, padded, tuPath);
      TextEdit edit{span->first, span->second, std::move(ro.text),
                    std::move(ro.pending), std::nullopt, {}};
      edit.isDirectTUHunkEdit = true;
      edit.directTUHunkIndex = i;
      edit.directTUHunkAStart = h.aStart;
      edit.directTUHunkAEnd = h.aEnd;
      edit.directTUHunkBStart = h.bStart;
      edit.directTUHunkBEnd = h.bEnd;
      edit.directTURawStart = rawTUStart;
      edit.directTURawEnd = rawTUEnd;
      edit.directTUFinalStart = span->first;
      edit.directTUFinalEnd = span->second;

      AttachAcceptedResultCarrier(
          edit, BuildAcceptedTUTextEditCandidate(
                    AcceptedPathKind::TUByteSpanConservativeEdit, span->first,
                    span->second, StringRef(padded)));
      tuEdits.push_back(std::move(edit));
      continue;
    }

    // Before we escalate to the explicit terminal out-of-domain carrier, try
    // one last source-closure class that keeps already-proved structural work
    // alive: materialize a contiguous run of top-level TU `#include`
    // directives directly into TU source text when that include run either
    // exactly covers the unresolved PP hunk or can be widened to the full
    // include cover without absorbing another token diff.
    SmallVector<std::pair<uint64_t, uint64_t>, 8> stagedSourceIntervals;
    for (const TextEdit &edit : tuEdits)
      stagedSourceIntervals.push_back({edit.start, edit.end});
    if (auto it = macroPatchByOwnerByMacroId.find(std::nullopt);
        it != macroPatchByOwnerByMacroId.end()) {
      for (const auto &kv : it->second)
        stagedSourceIntervals.push_back(
            {kv.second.invStart, kv.second.invEnd});
    }

    if (auto closureEdit = BuildTUIncludeClosureEditForUnresolvedHunk(
            h, tuPath, tuBytes, stagedSourceIntervals)) {
      debug("classify",
            "#{0} -> TU include-closure bytes=[{1},{2}) textLen={3}", i,
            closureEdit->start, closureEdit->end, closureEdit->text.size());
      tuEdits.push_back(std::move(*closureEdit));
      continue;
    }

    debug("classify",
          "#{0} dropping edit {1}: owner unresolved and no TU byte span "
          "available (no include-closure witness).",
          i, h);
    // Step 5 chooses the honest theorem-boundary interpretation for this last
    // ownership gap. By the time control reaches this branch, the engine has
    // already failed to prove a macro owner, include owner, truthful TU-owned
    // byte span, any exact/provable TU insertion anchor, and the first hybrid
    // TU include-closure fallback class. Do not manufacture a weaker success
    // class here; terminate via the named out-of-domain boundary instead.
    RequestTerminalFallback(
        TerminalFallbackKind::OwnerUnresolvedNoTUAnchor, "classify",
        BuildOwnerUnresolvedNoTUAnchorDetail(i, h, tuPath, owner, mapsToTU));
    continue;
  }

  // Global fail-closed composition rule: once this single structural pass has
  // requested terminal fallback, do not continue composing structural
  // artifacts. The outer driver will discard the current attempt and emit B
  // directly.
  if (terminalFallbackRequested_) {
    debug("fallback", "single-pass refold aborted after classification; "
                      "terminal fallback will be emitted");
    return std::string();
  }

  // Inject forced __COUNTER__ patches after normal hunk attribution.
  // This may create macro patches even when no diff hunk touched the invocation
  // (required to prevent later __COUNTER__ values from shifting after an edit).
  auto forcedCounters = ComputeForcedCounterPatches(tuPath, a2b);
  auto forcedCountersFromExpanded =
      ComputeForcedCounterPatchesFromExpandedMacros(tuPath,
                                                    macroPatchByOwnerByMacroId);
  if (!forcedCountersFromExpanded.empty())
    forcedCounters.append(forcedCountersFromExpanded.begin(),
                          forcedCountersFromExpanded.end());
  if (!forcedCounters.empty())
    AddForcedCounterPatches(forcedCounters, macroPatchByOwnerByMacroId);

  // Materialize merged macro patches into the list buckets expected by later
  // phases.
  //
  // Determinism: macroPatchByOwnerByMacroId and its inner maps are DenseMaps,
  // so iteration order is not stable across runs. Sort owner keys and macro
  // ids.
  llvm::SmallVector<std::optional<uint64_t>, 16> OwnerKeys;
  OwnerKeys.reserve(macroPatchByOwnerByMacroId.size());
  for (const auto &outerEntry : macroPatchByOwnerByMacroId)
    OwnerKeys.push_back(outerEntry.first);

  llvm::sort(OwnerKeys, [](const std::optional<uint64_t> &a,
                           const std::optional<uint64_t> &b) {
    if (!a && b)
      return true;
    if (a && !b)
      return false;
    if (!a && !b)
      return false;
    return *a < *b;
  });

  // Finalize per-owner macro patch lists in a deterministic order.
  // Patches were accumulated in a nested map keyed by owner and then macro id;
  // here we flatten them into each owner's output vector, sorting by macro id
  // first so patch emission does not depend on map iteration order.
  for (const auto &owner : OwnerKeys) {
    auto outerIt = macroPatchByOwnerByMacroId.find(owner);
    if (outerIt == macroPatchByOwnerByMacroId.end())
      continue;

    auto &patchesById = outerIt->second;
    auto &finalPatches = macroPatchesByOwner[owner];

    llvm::SmallVector<uint64_t, 32> MacroIds;
    MacroIds.reserve(patchesById.size());
    for (const auto &kv : patchesById)
      MacroIds.push_back(kv.first);
    llvm::sort(MacroIds);

    for (uint64_t id : MacroIds) {
      auto it = patchesById.find(id);
      if (it != patchesById.end())
        finalPatches.push_back(std::move(it->second));
    }
  }

  debug("plan", "perInclude.size={0} macroOwners={1} tuEdits(initial)={2}",
        perInclude.size(), macroPatchesByOwner.size(), tuEdits.size());

  // Normalize/coalesce include-side insertions.
  OrderIncludeInsertions(perInclude);

  // 5) Materialize include expansions bottom-up (nested first). Build child
  // lists by parent include id.
  DenseMap<uint64_t, std::vector<const RefoldModel::IncludeItem *>> children;
  for (const auto &ii : model_.GetIncludes()) {
    if (ii.parent)
      children[*ii.parent].push_back(&ii);
  }

  trace("include/tree", "BEGIN include children");
  for (const auto &[parentId, items] : children) {
    std::string pName = "#" + std::to_string(parentId);
    for (const RefoldModel::IncludeItem *child : items) {
      debug("include/tree",
            "{0} -> #{1} target={2} resolved={3} sitePath={4} site=[{5},{6}) "
            "cover=[{7},{8})",
            pName, child->id, child->target, child->resolvedPath,
            child->sitePath, child->siteB, child->siteE, child->cover.begin,
            child->cover.end);
    }
  }
  trace("include/tree", "END include children");

  // Cache for realized expansion text per include id.
  DenseMap<uint64_t, std::string> includeExpansion;

  // Step 2A: every materialized include expansion also carries a normalized
  // accepted-result summary so the later TU/header emission boundary does not
  // have to reconstruct where that emitted non-terminal artifact came from.
  DenseMap<uint64_t, AcceptedResultCandidate> includeExpansionAcceptedResults;

  // Build the set of include-ids that must be realized.
  DenseSet<uint64_t> seeds;

  // (a) Direct include edits.
  auto perIncludeKeys = make_first_range(perInclude);
  seeds.insert(perIncludeKeys.begin(), perIncludeKeys.end());

  // (b) Macro-owned work INSIDE headers (ownerIncludeId != null).
  for (auto &kv : macroPatchesByOwner) {
    if (kv.first)
      seeds.insert(*kv.first);
  }

  // (c) Pull in all ancestors up to the TU.
  llvm::SmallVector<uint64_t, 32> worklist(seeds.begin(), seeds.end());
  for (size_t i = 0; i < worklist.size(); ++i) {
    const auto *cur = model_.GetIncludeById(worklist[i]);
    while (cur && cur->parent) {
      uint64_t parentId = *cur->parent;
      auto [it, inserted] = seeds.insert(parentId);
      if (!inserted)
        break;
      worklist.push_back(parentId);
      cur = model_.GetIncludeById(parentId);
    }
  }

  std::vector<uint64_t> seedsVec(seeds.begin(), seeds.end());
  trace("include/mat", "seeds:");
  trace("include/mat", "======");
  logFormattedArray<uint64_t>(seedsVec, /* k */ MAX_COLS,
                              /* sameWidth */ false,
                              [](StringRef msg) { trace("include/mat", msg); });
  trace("include/mat", sep);

  // (d) Realize each include once (memoization lives inside
  // MaterializeIncludeExpansion).
  for (uint64_t incId : seeds) {
    debug("include/mat", "materialize seed include #{0}", incId);
    MaterializeIncludeExpansion(incId, perInclude, macroPatchesByOwner,
                                children, includeExpansion,
                                includeExpansionAcceptedResults,
                                &appliedExpandedMacroRootIds);
  }

  // Global fail-closed composition rule: if include realization requested the
  // terminal fallback in this single pass, stop here rather than continuing to
  // compose or return mixed structural artifacts.
  if (terminalFallbackRequested_) {
    debug("fallback", "single-pass refold aborted after include "
                      "materialization; terminal fallback will be emitted");
    return std::string();
  }

  // Determine which include ids count as expanded in the chosen single-pass
  // result. After Step 12, include realization is selected directly in the
  // same pass, so the set is exactly the include ids that materialized an
  // expansion text.
  DenseSet<uint64_t> expandedIncludeIds;
  for (const auto &kv : includeExpansion)
    expandedIncludeIds.insert(kv.first);
  lastStats_.expandedIncludes = expandedIncludeIds.size();

  // 6a) TU macro patches (ownerIncludeId == std::nullopt) and include
  // expansions at TU sites.
  if (auto it = macroPatchesByOwner.find(std::nullopt);
      it != macroPatchesByOwner.end() && !it->second.empty()) {
    // Create a local copy to sort
    auto tuMacroPatches = it->second;

    // 1. Sort patches by start offset, then by length (descending) to ensure we
    // process the "outermost" (largest) macros first.
    std::sort(tuMacroPatches.begin(), tuMacroPatches.end(),
              [](const MacroPatch &p1, const MacroPatch &p2) {
                if (p1.invStart != p2.invStart)
                  return p1.invStart < p2.invStart;
                return p1.invEnd > p2.invEnd;
              });

    // Apply TU-owned macro patches in deterministic outermost-first order.
    // Each patch may first be extended over trailing chained call-suffix
    // groups. Keep only the outermost patch for any nested TU callsite region:
    // if a later patch is fully contained in an already accepted interval, it
    // is shadowed and skipped. Partial overlaps are invalid for macro callsites
    // here, so fail fast rather than producing order-dependent edits.
    SmallVector<std::pair<uint64_t, uint64_t>, 16> accepted;
    for (const auto &mp : tuMacroPatches) {
      uint64_t mpEnd =
          extendChainedCallEnd(StringRef(tuBytes), mp.invEnd, mp.replacement);
      if (mpEnd != mp.invEnd) {
        debug("macro/chain",
              "TU extend chained callsite [{0},{1}) -> [{0},{2})", mp.invStart,
              mp.invEnd, mpEnd);
      }

      bool isShadowed = false;
      for (const auto &acc : accepted) {
        // If this patch is contained within one we already accepted, skip it.
        if (mp.invStart >= acc.first && mpEnd <= acc.second) {
          isShadowed = true;
          break;
        }

        // Partial overlaps should never occur (macro invocation sites are
        // either disjoint or nested). If they do, fail fast rather than
        // producing order-dependent behavior.
        if (mp.invStart < acc.second && acc.first < mpEnd) {
          fatal("macro/tu",
                "overlapping TU macro patches: mp=[{0},{1}) acc=[{2},{3})",
                mp.invStart, mpEnd, acc.first, acc.second);
        }
      }

      if (!isShadowed) {
        accepted.push_back({mp.invStart, mpEnd});
        debug("macro/tu", "  TU macro patch accepted inv=[{0},{1}) replLen={2}",
              mp.invStart, mpEnd, mp.replacement.size());
        ResyncOutcome ro = ApplyResyncOrPend(tuBytes, mp.invStart, mpEnd,
                                             mp.replacement, tuPath);
        TextEdit edit{mp.invStart,
                      mpEnd,
                      std::move(ro.text),
                      std::move(ro.pending),
                      MacroPatchRemainsExpanded(mp)
                          ? std::make_optional(GetRootMacroId(mp.macroId))
                          : std::nullopt,
                      {}};
        AttachAcceptedResultCarrier(edit,
                                    BuildAcceptedEmittedMacroCandidate(mp));
        tuEdits.push_back(std::move(edit));
      } else {
        trace("macro/tu", "  TU macro patch shadowed (skipped) inv=[{0},{1})",
              mp.invStart, mpEnd);
      }
    }
  }

  // 6b) TU include expansions: includes with parent == null and site in TU,
  // only if we realized an expansion.
  //
  // Determinism: includeExpansion is a DenseMap, so iterate by sorted id.
  llvm::SmallVector<uint64_t, 32> IncludeIds;
  IncludeIds.reserve(includeExpansion.size());
  for (const auto &kv : includeExpansion)
    IncludeIds.push_back(kv.first);
  llvm::sort(IncludeIds);

  // Apply TU-level include expansions by replacing the original `#include`
  // directive with the realized expansion text. For includes whose site is in
  // the TU itself (no parent include, and sitePath == tuPath), use the
  // materialized expansion from includeExpansion, defensively extend the
  // producer-reported site range to cover the full physical directive when line
  // splices are involved, then wrap the expansion with the appropriate line-
  // directive context and emit it as a TU text edit.
  for (uint64_t incId : IncludeIds) {
    auto itExp = includeExpansion.find(incId);
    if (itExp == includeExpansion.end())
      continue;

    const auto *inc = model_.GetIncludeById(incId);
    if (!inc)
      continue;
    if (!inc->parent && PathsEqual(inc->sitePath, tuPath)) {
      const auto &expText = itExp->second;
      // The producer's [siteB, siteE) range is supposed to cover the entire
      // physical `#include` directive in the TU. In some cases involving
      // leading line splices just before the directive, that recorded end can
      // stop too early. If we replace only the truncated range, part of the
      // original `#include` can remain in the TU, and checker replay may
      // include the header again.
      const StringRef tuRef(tuBytes);
      uint64_t siteB = inc->siteB;
      uint64_t siteE = inc->siteE;
      if (siteB < tuRef.size()) {
        size_t i = static_cast<size_t>(siteB);
        while (true) {
          size_t nl = tuRef.find('\n', i);
          if (nl == StringRef::npos) {
            siteE = tuRef.size();
            break;
          }
          i = nl + 1;
          if (!stringutils::isLineSplice(tuRef, nl)) {
            uint64_t extended = static_cast<uint64_t>(i);
            if (extended > siteE)
              siteE = extended;
            break;
          }
        }
      }

      debug("include/tu", "TU include expansion inc#{0} site=[{1},{2}) len={3}",
            inc->id, siteB, siteE, expText.size());
      std::string headerPath = resolveHeaderPath(*inc);
      std::string wrapped = lineDirs_.WrapIncludeExpansion(
          headerPath, tuPath, stringutils::lineAtOffset(tuBytes, siteE),
          expText);
      TextEdit edit{siteB, siteE, std::move(wrapped), std::nullopt,
                    std::nullopt, {}};
      auto itAccepted = includeExpansionAcceptedResults.find(incId);
      if (itAccepted != includeExpansionAcceptedResults.end())
        AttachAcceptedResultCarrier(edit, itAccepted->second);
      else
        AttachAcceptedResultCarrier(
            edit, BuildAcceptedIncludeRealizationCandidate(
                      AcceptedPathKind::IncludeMaterializedExpansion, *inc));
      tuEdits.push_back(std::move(edit));
    }
  }

  // Apply TU edits in descending order of start offset.
  debug("tu/apply", "applying {0} TU edits", tuEdits.size());
  std::string tuResult = ApplyTextEditsWithPendingResync(
      tuBytes, tuEdits, &appliedExpandedMacroRootIds, tuPath);
  if (terminalFallbackRequested_)
    return std::string();

  // Preserve TU-local __FILE__ / __FILE_NAME__ semantics in checker replay.
  //
  // If the TU contains an invocation of __FILE__ or __FILE_NAME__, replaying
  // the refolded output without an initial line directive would make those
  // builtins see the refolded output path (for example "foo.c.mod") instead of
  // the TU's original spelled path. To avoid that, prepend a TU-level line
  // directive that resets the logical file to the original TU path.
  //
  // Keep this narrowly scoped: do this only when such a builtin is actually
  // invoked in the TU, line directives are enabled, and the output does not
  // already begin with a #line directive.
  if (lineDirs_.Enabled() && !tuResult.empty()) {
    auto startsWithLine = [](StringRef s) -> bool {
      size_t i = 0;
      while (i < s.size()) {
        char c = s[i];
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
          ++i;
          continue;
        }
        break;
      }
      return s.drop_front(i).starts_with("#line");
    };

    bool needsTUPrologue = false;
    for (const auto &m : model_.GetMacroInvocations()) {
      if (m.name != "__FILE__" && m.name != "__FILE_NAME__")
        continue;
      if (!m.invFile)
        continue;

      // Compare absolute normalized paths to avoid relative-spelling
      // mismatches. Producer spelling (tuPath) is preserved in the emitted
      // directive.
      if (lineDirs_.ToAbsolutePath(*m.invFile) !=
          lineDirs_.ToAbsolutePath(tuPath))
        continue;

      needsTUPrologue = true;
      break;
    }

    if (needsTUPrologue && !startsWithLine(StringRef(tuResult))) {
      std::string dir = lineDirs_.FormatLineDirective(1, tuPath);
      if (!dir.empty())
        tuResult.insert(0, dir);
    }
  }

  // Charge root macros that remain inside expanded include bodies so the final
  // statistics continue to reflect which macro structure was realized rather
  // than preserved in the single-pass result.
  for (const auto &mi : model_.GetMacroInvocations()) {
    if (mi.ownerIncludeId &&
        expandedIncludeIds.find(*mi.ownerIncludeId) != expandedIncludeIds.end())
      appliedExpandedMacroRootIds.insert(GetRootMacroId(mi.id));
  }
  lastStats_.expandedMacros = appliedExpandedMacroRootIds.size();

  debug("plan", "REFOLD DONE tuResultLen={0}", tuResult.size());
  return tuResult;
}

// ================== A ↔ B token mapping & diff utilities ===================

std::vector<StringRef> RefoldEngine::MapLexemes(ArrayRef<PPTok> toks,
                                                ArrayRef<size_t> offs) {
  std::vector<StringRef> out;
  out.reserve(toks.size());
  for (std::size_t i = 0; i < toks.size(); ++i) {
    const auto &s = toks[i].spelling;
    if (stringutils::isWhitespace(s)) {
      // We should never encounter a whitespace token
      fatal("map/lexemes", "token at index {0} is whitespace", i);
    } else {
      out.emplace_back(StringRef(s));
    }
  }
  return out;
}

std::vector<uint32_t> RefoldEngine::ComputeOwnerDepthGapsForPP() {
  // aTokOff.size() == (#tokens) + 1 (sentinel). LCS expects N == #tokens,
  // and ownerDepthGap.size() == N + 1.
  const size_t N = aTokOff_.size() - 1;
  std::vector<uint32_t> ownerDepthGap(N + 1, 0);

  for (size_t k = 0; k <= N; ++k) {
    // --------------------------- Include depth ----------------------------
    std::optional<uint64_t> leftInc;
    std::optional<uint64_t> rightInc;

    if (k > 0) {
      leftInc = model_.InnermostIncludeAtPP(k - 1);
    }
    if (k < N) {
      rightInc = model_.InnermostIncludeAtPP(k);
    }

    std::optional<uint64_t> lca =
        model_.LeastCommonAncestorInclude(leftInc, rightInc);
    uint32_t incDepth = model_.GetIncludeDepth(lca);

    // ------------------------- Conditional depth --------------------------
    std::optional<RefoldModel::ArmRef> leftArmRef;
    std::optional<RefoldModel::ArmRef> rightArmRef;

    if (k > 0) {
      leftArmRef = model_.FindArmRefAtPP(k - 1);
    }
    if (k < N) {
      rightArmRef = model_.FindArmRefAtPP(k);
    }

    uint32_t leftCondDepth =
        leftArmRef ? model_.GetCondArmDepth(leftArmRef->arm->id) : 0;
    uint32_t rightCondDepth =
        rightArmRef ? model_.GetCondArmDepth(rightArmRef->arm->id) : 0;
    uint32_t condDepth = std::min(leftCondDepth, rightCondDepth);

    ownerDepthGap[k] = incDepth + condDepth;
  }

  return ownerDepthGap;
}


std::vector<diffutils::LcsGapProvenance>
RefoldEngine::ComputeLcsGapProvenanceForPP() {
  using diffutils::LcsGapProvenance;

  // The core LCS still consumes the same scalar owner-depth array as before.
  // The remaining fields below carry identity, not extra cost: they let the
  // diff layer prove whether an ambiguous equal-token frontier preserves an
  // include, conditional, or macro boundary.

  const size_t N = aTokOff_.size() - 1;
  std::vector<LcsGapProvenance> profiles(N + 1);
  const std::vector<uint32_t> ownerDepthGap = ComputeOwnerDepthGapsForPP();

  struct MacroTokenContext {
    uint64_t rootId = LcsGapProvenance::NoId;
    uint64_t leafId = LcsGapProvenance::NoId;
    uint32_t depth = 0;
    uint32_t roleMask = 0;
  };

  auto spanContains = [](const auto &span, uint64_t pp) -> bool {
    return span.IsValid() && span.begin <= pp && pp < span.end;
  };

  auto macroDepthAndRoot = [&](const RefoldModel::MacroInvocation &m,
                               uint64_t &rootId) -> uint32_t {
    rootId = m.id;
    uint32_t depth = 1;
    const RefoldModel::MacroInvocation *cur = &m;
    SmallDenseSet<uint64_t, 8> seen;
    seen.insert(cur->id);
    while (cur->callerMacroId) {
      const uint64_t parentId = *cur->callerMacroId;
      if (seen.contains(parentId))
        break;
      seen.insert(parentId);
      const RefoldModel::MacroInvocation *parent =
          FindMacroInvocationById(parentId);
      if (!parent)
        break;
      cur = parent;
      rootId = cur->id;
      ++depth;
    }
    return depth;
  };

  auto macroRoleMaskAtPP = [&](const RefoldModel::MacroInvocation &m,
                               uint64_t pp) -> uint32_t {
    uint32_t mask = 0;
    for (const auto &span : m.argSpans) {
      if (spanContains(span, pp)) {
        mask |= 1U; // argument contribution
        break;
      }
    }
    for (const auto &span : m.stringifySpans) {
      if (spanContains(span, pp)) {
        mask |= 2U; // stringification contribution
        break;
      }
    }
    for (const auto &span : m.pasteSpans) {
      if (spanContains(span, pp)) {
        mask |= 4U; // token-paste contribution
        break;
      }
    }
    for (const auto &span : m.bodySpans) {
      if (spanContains(span, pp)) {
        mask |= 8U; // replacement-list/body contribution
        break;
      }
    }
    for (const auto &span : m.spans) {
      if (spanContains(span, pp)) {
        mask |= 16U; // general expansion coverage
        break;
      }
    }
    return mask;
  };

  auto macroContextAtPP = [&](uint64_t pp) -> MacroTokenContext {
    MacroTokenContext best;
    uint64_t bestCoverWidth = std::numeric_limits<uint64_t>::max();
    for (const auto &m : model_.GetMacroInvocations()) {
      if (!m.Covers(pp, pp + 1))
        continue;

      uint64_t rootId = m.id;
      const uint32_t depth = macroDepthAndRoot(m, rootId);
      const uint64_t coverWidth = m.cover.IsValid()
                                      ? (m.cover.end - m.cover.begin)
                                      : std::numeric_limits<uint64_t>::max();

      // Prefer the deepest invocation in the caller chain. If two records have
      // the same caller depth for this token, the narrower cover is the more
      // precise leaf owner. This ranking feeds the production LCS provenance
      // certificate; it does not directly choose an edit by token spelling.
      if (depth > best.depth ||
          (depth == best.depth && coverWidth < bestCoverWidth)) {
        best.rootId = rootId;
        best.leafId = m.id;
        best.depth = depth;
        best.roleMask = macroRoleMaskAtPP(m, pp);
        bestCoverWidth = coverWidth;
      }
    }
    return best;
  };

  auto fillSide = [&](LcsGapProvenance &profile, uint64_t pp, bool leftSide) {
    const std::optional<uint64_t> includeId = model_.InnermostIncludeAtPP(pp);
    const std::optional<RefoldModel::ArmRef> armRef = model_.FindArmRefAtPP(pp);
    const MacroTokenContext macro = macroContextAtPP(pp);

    if (leftSide) {
      profile.leftIncludeId = includeId.value_or(LcsGapProvenance::NoId);
      if (armRef) {
        profile.leftCondGroupId = armRef->group->id;
        profile.leftCondArmId = armRef->arm->id;
      }
      profile.leftMacroRootId = macro.rootId;
      profile.leftMacroLeafId = macro.leafId;
      profile.leftMacroRoleMask = macro.roleMask;
    } else {
      profile.rightIncludeId = includeId.value_or(LcsGapProvenance::NoId);
      if (armRef) {
        profile.rightCondGroupId = armRef->group->id;
        profile.rightCondArmId = armRef->arm->id;
      }
      profile.rightMacroRootId = macro.rootId;
      profile.rightMacroLeafId = macro.leafId;
      profile.rightMacroRoleMask = macro.roleMask;
    }

    profile.macroDepth = std::max(profile.macroDepth, macro.depth);
  };

  for (size_t k = 0; k <= N; ++k) {
    LcsGapProvenance profile;
    profile.ownerDepth = ownerDepthGap[k];

    std::optional<uint64_t> leftInc;
    std::optional<uint64_t> rightInc;
    std::optional<RefoldModel::ArmRef> leftArmRef;
    std::optional<RefoldModel::ArmRef> rightArmRef;

    if (k > 0) {
      const uint64_t leftPP = static_cast<uint64_t>(k - 1);
      leftInc = model_.InnermostIncludeAtPP(leftPP);
      leftArmRef = model_.FindArmRefAtPP(leftPP);
      fillSide(profile, leftPP, /*leftSide=*/true);
    }
    if (k < N) {
      const uint64_t rightPP = static_cast<uint64_t>(k);
      rightInc = model_.InnermostIncludeAtPP(rightPP);
      rightArmRef = model_.FindArmRefAtPP(rightPP);
      fillSide(profile, rightPP, /*leftSide=*/false);
    }

    const std::optional<uint64_t> lca =
        model_.LeastCommonAncestorInclude(leftInc, rightInc);
    profile.lcaIncludeId = lca.value_or(LcsGapProvenance::NoId);
    profile.includeDepth = model_.GetIncludeDepth(lca);

    const uint32_t leftCondDepth =
        leftArmRef ? model_.GetCondArmDepth(leftArmRef->arm->id) : 0;
    const uint32_t rightCondDepth =
        rightArmRef ? model_.GetCondArmDepth(rightArmRef->arm->id) : 0;
    profile.conditionalDepth = std::min(leftCondDepth, rightCondDepth);

    profiles[k] = profile;
  }

  return profiles;
}


std::vector<diffutils::LcsBGapProvenance>
RefoldEngine::ComputeLcsBGapProvenanceForPP() {
  using diffutils::LcsBGapProvenance;

  // B-side tokens have no producer ownership graph, so this records only source
  // surface facts around each edited token gap. The LCS certificate may use
  // these facts to break otherwise equivalent pure-insertion frontiers without
  // reintroducing the removed neighboring-token spelling heuristic.

  const size_t N = bToks_.size();
  std::vector<LcsBGapProvenance> profiles(N + 1);

  auto containsNewline = [&](size_t begin, size_t end) -> bool {
    begin = std::min(begin, bSource_.size());
    end = std::min(end, bSource_.size());
    if (end < begin)
      std::swap(begin, end);
    return bSource_.substr(begin, end - begin).find('\n') != StringRef::npos;
  };

  auto onlyWhitespace = [&](size_t begin, size_t end) -> bool {
    begin = std::min(begin, bSource_.size());
    end = std::min(end, bSource_.size());
    if (end < begin)
      std::swap(begin, end);
    for (char ch : bSource_.substr(begin, end - begin)) {
      if (!isHorizontalWhitespace(ch) && ch != '\n' && ch != '\r')
        return false;
    }
    return true;
  };

  auto lineStart = [&](size_t pos) -> size_t {
    pos = std::min(pos, bSource_.size());
    while (pos > 0 && bSource_[pos - 1] != '\n' && bSource_[pos - 1] != '\r')
      --pos;
    return pos;
  };

  auto lineEnd = [&](size_t pos) -> size_t {
    pos = std::min(pos, bSource_.size());
    while (pos < bSource_.size() && bSource_[pos] != '\n' &&
           bSource_[pos] != '\r')
      ++pos;
    return pos;
  };

  auto tokenBegin = [&](size_t tok) -> size_t {
    if (tok >= bTokOff_.size())
      return bSource_.size();
    return std::min(bTokOff_[tok], bSource_.size());
  };

  auto tokenEnd = [&](size_t tok) -> size_t {
    if (tok >= N)
      return bSource_.size();
    const size_t begin = tokenBegin(tok);
    const size_t spellingEnd = begin + bToks_[tok].spelling.size();
    if (tok + 1 < bTokOff_.size())
      return std::min(spellingEnd, bTokOff_[tok + 1]);
    return std::min(spellingEnd, bSource_.size());
  };

  auto beginsLine = [&](size_t begin) -> bool {
    return onlyWhitespace(lineStart(begin), begin);
  };

  auto endsLine = [&](size_t end) -> bool {
    return onlyWhitespace(end, lineEnd(end));
  };

  for (size_t gap = 0; gap <= N; ++gap) {
    LcsBGapProvenance profile;
    profile.hasLeftToken = gap > 0;
    profile.hasRightToken = gap < N;

    const size_t gapBegin = profile.hasLeftToken ? tokenEnd(gap - 1) : 0;
    const size_t gapEnd =
        profile.hasRightToken ? tokenBegin(gap) : bSource_.size();

    profile.gapBeginByte = static_cast<uint64_t>(gapBegin);
    profile.gapEndByte = static_cast<uint64_t>(gapEnd);

    profile.gapContainsNewline = containsNewline(gapBegin, gapEnd);
    profile.gapContainsOnlyWhitespace = onlyWhitespace(gapBegin, gapEnd);
    profile.gapAtLineStart = beginsLine(gapBegin);
    profile.gapAtLineEnd = endsLine(gapEnd);

    if (profile.hasLeftToken) {
      const size_t leftBegin = tokenBegin(gap - 1);
      const size_t leftEnd = tokenEnd(gap - 1);
      profile.leftTokenBeginByte = static_cast<uint64_t>(leftBegin);
      profile.leftTokenEndByte = static_cast<uint64_t>(leftEnd);
      profile.leftTokenStartsLine = beginsLine(leftBegin);
      profile.leftTokenEndsLine = endsLine(leftEnd);
    }
    if (profile.hasRightToken) {
      const size_t rightBegin = tokenBegin(gap);
      const size_t rightEnd = tokenEnd(gap);
      profile.rightTokenBeginByte = static_cast<uint64_t>(rightBegin);
      profile.rightTokenEndByte = static_cast<uint64_t>(rightEnd);
      profile.rightTokenStartsLine = beginsLine(rightBegin);
      profile.rightTokenEndsLine = endsLine(rightEnd);
    }

    profiles[gap] = profile;
  }

  return profiles;
}

// ============================= Boundary helpers ==============================

namespace {
/// Byte slice for a single top-level element inside a comma-separated tuple.
///
/// `begin`/`end` cover the full half-open byte range for the element inside the
/// caller argument text. `trimBegin`/`trimEnd` shrink that range to the
/// non-whitespace payload used for exact old/new text comparisons.
struct TupleElementSlice {
  size_t begin = 0;
  size_t end = 0;
  size_t trimBegin = 0;
  size_t trimEnd = 0;
};

static bool computeTrimmedTupleElement(StringRef Text, size_t Begin, size_t End,
                                       TupleElementSlice &Out) {
  Out.begin = Begin;
  Out.end = End;
  Out.trimBegin = Begin;
  Out.trimEnd = End;
  while (Out.trimBegin < Out.trimEnd &&
         std::isspace(static_cast<unsigned char>(Text[Out.trimBegin])))
    ++Out.trimBegin;
  while (Out.trimEnd > Out.trimBegin &&
         std::isspace(static_cast<unsigned char>(Text[Out.trimEnd - 1])))
    --Out.trimEnd;
  return Out.trimBegin != Out.trimEnd;
}

/// Split a caller tuple into top-level comma-separated elements using Clang's
/// raw lexer instead of ad hoc character scanning.
///
/// This is intentionally token-based: literals and comments arrive as single
/// tokens, so only delimiter depth (`()`, `[]`, `{}`) needs to be tracked when
/// deciding whether a comma separates tuple elements.
static bool splitTopLevelTupleElementsWithLexer(
    StringRef Text, const LangOptions &Lang,
    SmallVectorImpl<TupleElementSlice> &Out) {
  Out.clear();
  if (Text.empty())
    return false;

  const SourceLocation BaseLoc = SourceLocation::getFromRawEncoding(1);
  std::string LexBuf = Text.str();
  LexBuf.push_back('\0');
  const char *BufStart = LexBuf.data();
  const char *BufEnd = BufStart + Text.size();
  Lexer Lex(BaseLoc, Lang, BufStart, BufStart, BufEnd);

  size_t ElementBegin = 0;
  int ParenDepth = 0;
  int BracketDepth = 0;
  int BraceDepth = 0;
  Token Tok;

  while (true) {
    Lex.LexFromRawLexer(Tok);
    if (Tok.is(tok::eof))
      break;
    if (Tok.is(tok::comment))
      continue;

    const size_t TokBegin =
        Tok.getLocation().getRawEncoding() - BaseLoc.getRawEncoding();
    const size_t TokEnd = TokBegin + Tok.getLength();

    switch (Tok.getKind()) {
    case tok::l_paren:
      ++ParenDepth;
      break;
    case tok::r_paren:
      if (ParenDepth > 0)
        --ParenDepth;
      break;
    case tok::l_square:
      ++BracketDepth;
      break;
    case tok::r_square:
      if (BracketDepth > 0)
        --BracketDepth;
      break;
    case tok::l_brace:
      ++BraceDepth;
      break;
    case tok::r_brace:
      if (BraceDepth > 0)
        --BraceDepth;
      break;
    case tok::comma:
      if (ParenDepth == 0 && BracketDepth == 0 && BraceDepth == 0) {
        TupleElementSlice Elem;
        if (!computeTrimmedTupleElement(Text, ElementBegin, TokBegin, Elem))
          return false;
        Out.push_back(Elem);
        ElementBegin = TokEnd;
      }
      break;
    default:
      break;
    }
  }

  TupleElementSlice Elem;
  if (!computeTrimmedTupleElement(Text, ElementBegin, Text.size(), Elem))
    return false;
  Out.push_back(Elem);
  return true;
}

static void lexBoundaryTokens(StringRef Text, const LangOptions &Lang,
                              SmallVectorImpl<LexBoundaryToken> &Out) {
  Out.clear();
  if (Text.empty())
    return;

  const SourceLocation BaseLoc = SourceLocation::getFromRawEncoding(1);
  std::string LexBuf = Text.str();
  LexBuf.push_back('\0');
  const char *BufStart = LexBuf.data();
  const char *BufEnd = BufStart + Text.size();
  Lexer Lex(BaseLoc, Lang, BufStart, BufStart, BufEnd);
  Token Tok;

  while (true) {
    Lex.LexFromRawLexer(Tok);
    if (Tok.is(tok::eof))
      break;
    if (Tok.is(tok::comment))
      continue;

    const unsigned Off =
        Tok.getLocation().getRawEncoding() - BaseLoc.getRawEncoding();
    Out.push_back({Tok.getKind(),
                   std::string(Text.substr(Off, Tok.getLength())), Off,
                   Off + Tok.getLength()});
  }
}

static std::optional<LexBoundaryToken> firstLexToken(StringRef Text,
                                                    const LangOptions &Lang) {
  SmallVector<LexBoundaryToken, 8> Toks;
  lexBoundaryTokens(Text, Lang, Toks);
  if (Toks.empty())
    return std::nullopt;
  return Toks.front();
}

static std::optional<LexBoundaryToken> lastLexToken(StringRef Text,
                                                   const LangOptions &Lang) {
  SmallVector<LexBoundaryToken, 16> Toks;
  lexBoundaryTokens(Text, Lang, Toks);
  if (Toks.empty())
    return std::nullopt;
  return Toks.back();
}

/// Return true iff placing Left and Right adjacent with no separating
/// whitespace would change lexical tokenization compared to placing a space
/// between them.
static bool needsLexicalSeparator(const LexBoundaryToken &Left,
                                  const LexBoundaryToken &Right,
                                  const LangOptions &Lang) {
  const std::string NoSpace = Left.Spelling + Right.Spelling;
  const std::string WithSpace = Left.Spelling + " " + Right.Spelling;

  SmallVector<LexBoundaryToken, 8> NoSpaceToks;
  SmallVector<LexBoundaryToken, 8> WithSpaceToks;
  lexBoundaryTokens(NoSpace, Lang, NoSpaceToks);
  lexBoundaryTokens(WithSpace, Lang, WithSpaceToks);

  if (NoSpaceToks.size() != WithSpaceToks.size())
    return true;
  for (size_t I = 0; I < NoSpaceToks.size(); ++I) {
    if (NoSpaceToks[I].Kind != WithSpaceToks[I].Kind ||
        NoSpaceToks[I].Spelling != WithSpaceToks[I].Spelling)
      return true;
  }
  return false;
}

/// Return true iff \p Kind is separator punctuation that may legitimately
/// replace a horizontal source gap between two tokens.
///
/// This is intentionally narrower than "left-attachable punctuation": closing
/// delimiters and operators can carry context-sensitive spacing conventions, so
/// they are not treated as gap replacements here. Callers must still prove with
/// `needsLexicalSeparator()` that attaching the punctuation to the token on its
/// left preserves lexical tokenization.
static bool isSeparatorGapReplacementPunctuation(tok::TokenKind Kind) {
  switch (Kind) {
  case tok::comma:
  case tok::semi:
  case tok::colon:
    return true;
  default:
    return false;
  }
}
} // namespace

std::string RefoldEngine::PadAtBoundaries(StringRef base, size_t start,
                                          size_t end, std::string text,
                                          bool allowLeft,
                                          bool allowRight) const {
  const auto f = stringutils::firstNonWsIdx(text);
  const auto l = stringutils::lastNonWsIdx(text);

  // If text is empty or only whitespace, there is no token content whose
  // boundaries need separation.
  if (!f || !l)
    return text;

  // If the replacement already has whitespace at an edge, treat that side as
  // already separated and never add another padding space there.
  const bool hasLeadingWS = (*f > 0);
  const bool hasTrailingWS = (*l + 1 < text.size());

  std::optional<LexBoundaryToken> textFirstTok =
      firstLexToken(StringRef(text), lexLang_);
  std::optional<LexBoundaryToken> textLastTok =
      lastLexToken(StringRef(text), lexLang_);

  const std::optional<char> leftChar =
      (start > 0 && start <= base.size()) ? std::optional<char>(base[start - 1])
                                          : std::nullopt;
  const std::optional<char> rightChar =
      (end < base.size()) ? std::optional<char>(base[end]) : std::nullopt;

  bool addLeftSpace = false;
  // Add a leading space only when:
  //   - left padding is allowed,
  //   - the replacement does not already begin with whitespace,
  //   - the base text is not already separated from the replacement by
  //     immediate boundary whitespace, and
  //   - juxtaposing the left boundary token and the replacement's first token
  //     would change lexical tokenization.
  if (allowLeft && !hasLeadingWS && start > 0 && start <= base.size() &&
      textFirstTok && (!leftChar || !stringutils::isWs(*leftChar))) {
    if (std::optional<LexBoundaryToken> leftTok =
            lastLexToken(base.take_front(start), lexLang_)) {
      addLeftSpace = needsLexicalSeparator(*leftTok, *textFirstTok, lexLang_);
    }
  }

  bool addRightSpace = false;
  // Likewise on the right boundary: add a trailing space only when:
  //   - right padding is allowed,
  //   - the replacement does not already end with whitespace,
  //   - the base text is not already separated from the replacement by
  //     immediate boundary whitespace, and
  //   - juxtaposing the replacement's last token and the right boundary token
  //     would change lexical tokenization.
  if (allowRight && !hasTrailingWS && end < base.size() && textLastTok &&
      (!rightChar || !stringutils::isWs(*rightChar))) {
    if (std::optional<LexBoundaryToken> rightTok =
            firstLexToken(base.drop_front(end), lexLang_)) {
      addRightSpace = needsLexicalSeparator(*textLastTok, *rightTok, lexLang_);
    }
  }

  if (addLeftSpace)
    text.insert(0, 1, ' ');

  if (addRightSpace)
    text.push_back(' ');

  return text;
}

// ====================== Owner resolution & TU mapping ========================

RefoldEngine::Owner
RefoldEngine::ClassifyOwnerWithSegments(StringRef tuPath,
                                        const diffutils::Hunk &h) const {
  uint64_t a0 = h.aStart;
  uint64_t a1 = h.aEnd;

  debug("segments",
        "ENTER classifyOwnerWithSegments tuPath={0} A[{1},{2}) (isEmpty={3})",
        tuPath, a0, a1, a0 == a1);

  // Insertion ownership:
  //
  // If the PP gap aligns with a stable TU slot boundary (include boundary or
  // conditional-arm boundary), defer to the segment-based classification (using
  // TU byte anchoring).
  //
  // Otherwise, if both sides of the gap are unambiguously within the same
  // include's PP coverage, treat the insertion as include-owned.
  if (a0 == a1) {
    if (auto slotAnchor = AnchorToExactSlotBoundaryFromPPGap(tuPath, a0)) {
      trace("slots/anchor", "INS slotAnchor present: gapPP={0} tuByte={1}", a0,
            (std::uint64_t)*slotAnchor);
      std::optional<uint64_t> leftInc =
          (a0 > 0) ? model_.InnermostIncludeAtPP(a0 - 1) : std::nullopt;
      const uint64_t maxPP = model_.GetTokensCountA();
      std::optional<uint64_t> rightInc =
          a0 < maxPP ? model_.InnermostIncludeAtPP(a0) : std::nullopt;

      std::int64_t leftArm = -1, rightArm = -1;
      if (a0 > 0) {
        if (auto armRef = model_.FindArmRefAtPP(a0 - 1))
          leftArm = (std::int64_t)armRef->arm->id;
      }
      if (a0 < maxPP) {
        if (auto armRef = model_.FindArmRefAtPP(a0))
          rightArm = (std::int64_t)armRef->arm->id;
      }

      trace("segments/insert",
            "ClassifyOwnerWithSegments: INS gapPP={0} slotTU={1} leftInc={2} "
            "rightInc={3} leftArm={4} rightArm={5} depthGap={6}",
            a0, (std::uint64_t)*slotAnchor, leftInc, rightInc, leftArm,
            rightArm,
            (a0 < ownerDepthGap_.size() ? (int)ownerDepthGap_[a0] : -1));

      if (leftInc && rightInc && *leftInc == *rightInc) {
        trace("segments",
              "    insertion gap PP={0} classified as INCLUDE id={1} (left={2} "
              "right={3})",
              a0, rightInc, leftInc, rightInc);
        return Owner::Include(*rightInc);
      }
    }
  }

  // First, get the TU byte span for this hunk. Even when the hunk ultimately
  // belongs to a header, we still anchor via the TU span because segments for
  // includes and conditional arms in that header are projected into the TU
  // through slots.
  auto span = TUByteSpan(a0, a1, tuPath); // [b, e)

  // No truthful TU byte anchor exists for this PP segment, so choose its owner
  // using only preprocessed-token structure. This happens when the segment has
  // no TU-backed tokens in range, and a pure insertion cannot be safely tied to
  // a concrete TU byte position. In that case, recover ownership from the
  // include / conditional context at the PP boundaries:
  //   - for insertions, inspect the PP token immediately to the left and right
  //     of the insertion gap
  //   - for non-insertions, inspect the PP endpoints covered by the segment
  // If both sides live under a common include, assign the segment to that
  // least-common-ancestor include, and preserve a same-arm conditional owner
  // when both sides are in the same selected arm. If no include owner can be
  // established, fall back to TU ownership; this should be rare and typically
  // indicates a TU-boundary case without a stronger slot anchor.
  if (!span) {
    const bool isInsert = (a0 == a1);
    const size_t n = model_.GetTokensCountA();

    std::optional<uint64_t> leftInc;
    std::optional<uint64_t> rightInc;

    std::optional<RefoldModel::ArmRef> leftArmRef;
    std::optional<RefoldModel::ArmRef> rightArmRef;

    if (isInsert) {
      // Pure insertion: classify the gap from the PP token just before and just
      // after the insertion site, when those neighbors exist.
      if (a0 > 0) {
        leftInc = model_.InnermostIncludeAtPP(a0 - 1);
        leftArmRef = model_.FindArmRefAtPP(a0 - 1);
      }
      if (static_cast<size_t>(a0) < n) {
        rightInc = model_.InnermostIncludeAtPP(a0);
        rightArmRef = model_.FindArmRefAtPP(a0);
      }
    } else {
      // Non-insertion: classify from the PP endpoints actually covered by the
      // segment.
      leftInc = model_.InnermostIncludeAtPP(a0);
      rightInc = model_.InnermostIncludeAtPP(a1 - 1);
      leftArmRef = model_.FindArmRefAtPP(a0);
      rightArmRef = model_.FindArmRefAtPP(a1 - 1);
    }

    // Use the least common ancestor include of the left/right PP contexts as
    // the structural include owner, if one exists.
    std::optional<uint64_t> lcaInc =
        model_.LeastCommonAncestorInclude(leftInc, rightInc);

    // Preserve a conditional-arm owner only when both PP sides are in the same
    // selected arm.
    std::optional<uint64_t> condArmId;
    if (leftArmRef && rightArmRef && leftArmRef->arm && rightArmRef->arm &&
        leftArmRef->arm->id == rightArmRef->arm->id) {
      condArmId = leftArmRef->arm->id;
    }

    if (lcaInc) {
      trace("segments",
            "  no TU anchor for hunk [{0},{1}); PP-only owner INCLUDE id={2} "
            "(condArmId={3})",
            a0, a1, lcaInc, condArmId);
      return Owner::Include(*lcaInc, condArmId);
    }

    // No include owner could be recovered from PP structure; fall back to TU.
    trace("segments",
          "  no TU anchor for hunk [{0},{1}); PP-only owner TU (condArmId={2})",
          a0, a1, condArmId);
    return Owner::TU(condArmId);
  }

  uint64_t b = span->first, e = span->second;
  if (b > e)
    std::swap(b, e);

  trace("segments", "  TU byte span for A[{0},{1}) in {2} = [{3},{4})", a0, a1,
        tuPath, b, e);

  // Build (or fetch) all segments projected into tuPath.
  ArrayRef<RefoldModel::Segment> segs = model_.GetSegmentsForFile(tuPath);
  if (segs.empty()) {
    debug("segments", "  no segments for file={0}; owner UNKNOWN", tuPath);
    return Owner::Unknown();
  }

  // Step 1: collect all segment candidates that could own this hunk at the
  // current TU path.
  //
  // Non-insertions own real byte coverage, so any segment that intersects the
  // hunk byte range [b,e) is a candidate.
  //
  // Pure insertions usually have no byte width in TU space. For those, treat
  // the insertion site as a single probe point at `b` and collect every
  // segment that contains that point. The comparison is right-closed
  // (`s.b <= probe && probe <= s.e`) so an insertion that lands exactly on a
  // segment boundary can still be claimed by an enclosing/parent segment.
  const bool isInsert = (a0 == a1);
  const uint64_t probe = b;

  std::vector<const RefoldModel::Segment *> hits;
  for (const auto &s : segs) {
    if (!isInsert) {
      // Standard half-open interval intersection test for [b,e) vs [s.b,s.e).
      if (s.e <= b || e <= s.b)
        continue;
      hits.push_back(&s);
    } else {
      // Zero-width insertion: classify by containment of the insertion probe.
      if (s.b <= probe && probe <= s.e)
        hits.push_back(&s);
    }
  }

  if (hits.empty()) {
    debug("segments",
          "  no candidate segments for {0} span [{1},{2}) (probe={3}, "
          "isInsert={4}); owner UNKNOWN",
          tuPath, b, e, probe, isInsert);
    return Owner::Unknown();
  }

  // Step 2: choose the most specific candidate segment.
  //
  // Prefer the smallest byte span first. If multiple hits have the same size,
  // break ties deterministically by earlier start, then earlier end.
  const RefoldModel::Segment *selected = hits[0];
  for (size_t i = 1; i < hits.size(); ++i) {
    const auto *s = hits[i];
    uint64_t sLen = s->e - s->b;
    uint64_t selLen = selected->e - selected->b;

    if (sLen < selLen) {
      selected = s;
    } else if (sLen == selLen) {
      if (s->b < selected->b) {
        selected = s;
      } else if (s->b == selected->b && s->e < selected->e) {
        selected = s;
      }
    }
  }

  // Step 3: convert the selected segment's stored ownership into a concrete
  // TU/include owner result, preserving any conditional-arm owner attached to
  // that segment.
  if (!selected->ownerIncludeId) {
    trace("segments",
          "  selected segment [{0},{1}) (probe={2}) owner TU (condArmId={3}) "
          "for hunk [{4},{5})",
          selected->b, selected->e, probe, selected->ownerCondArmId, a0, a1);
    return Owner::TU(selected->ownerCondArmId);
  } else {
    trace("segments",
          "  selected segment [{0},{1}) (probe={2}) owner INCLUDE id={3} "
          "(condArmId={4}) for hunk [{5},{6})",
          selected->b, selected->e, probe, selected->ownerIncludeId,
          selected->ownerCondArmId, a0, a1);
    return Owner::Include(*selected->ownerIncludeId, selected->ownerCondArmId);
  }
}

bool RefoldEngine::IsInvocationInsideDefineDirective(
    const RefoldModel::MacroInvocation &m) const {
  if (!m.invFile || !m.invB || !m.invE)
    return false;

  // MacroDirective::siteB/siteE normally covers the first physical line of a
  // #define directive. For line-spliced macro definitions, invocations spelled
  // in the replacement list may appear after siteE, so the index widens each
  // directive extent to the first newline that is not escaped by a line splice.
  //
  // Keep this cache on the RefoldEngine instance. The directive list and
  // logical-to-absolute path resolver come from the current refold model; a
  // process-global cache can misclassify later runs that reuse the same process
  // with different model/path state.
  if (!definesIndexBuilt_) {
    definesIndexBuilt_ = true;
    defineFileTextCache_.clear();
    defineEndCache_.clear();
    definesByAbsPath_.clear();

    for (const auto &d : model_.GetMacroDirectives()) {
      if ("#define" != d.subkind)
        continue;
      if (d.sitePath.empty())
        continue;

      // Compute, or fetch, the widened physical end of this #define directive.
      uint64_t defineEnd = d.siteE;
      auto itEnd = defineEndCache_.find(d.id);
      if (itEnd != defineEndCache_.end()) {
        defineEnd = itEnd->second;
      } else {
        std::string absPath = lineDirs_.ToAbsolutePath(d.sitePath);
        auto itTxt = defineFileTextCache_.find(absPath);
        if (itTxt == defineFileTextCache_.end()) {
          auto bufOrErr = llvm::MemoryBuffer::getFile(absPath);
          if (!bufOrErr) {
            // Best effort: keep the producer-provided one-line extent if the
            // source file cannot be loaded in the current replay environment.
            defineEndCache_[d.id] = d.siteE;
            defineEnd = d.siteE;
          } else {
            defineFileTextCache_[absPath] = (**bufOrErr).getBuffer().str();
            itTxt = defineFileTextCache_.find(absPath);
          }
        }

        if (itTxt != defineFileTextCache_.end()) {
          StringRef bytes(itTxt->second);
          uint64_t i = d.siteB;
          if (i > bytes.size())
            i = bytes.size();

          while (i < bytes.size()) {
            size_t nl = bytes.find('\n', static_cast<size_t>(i));
            if (nl == StringRef::npos) {
              i = bytes.size();
              break;
            }

            i = static_cast<uint64_t>(nl + 1);
            if (!stringutils::isLineSplice(bytes, nl))
              break;
          }

          defineEnd = i;
          defineEndCache_[d.id] = defineEnd;
        }
      }

      const std::string absPath = lineDirs_.ToAbsolutePath(d.sitePath);
      definesByAbsPath_[absPath].push_back(
          DefineDirectiveExtent{d.siteB, defineEnd});
    }

    for (auto &kv : definesByAbsPath_) {
      auto &vec = kv.getValue();
      llvm::sort(vec, [](const DefineDirectiveExtent &x,
                         const DefineDirectiveExtent &y) {
        if (x.begin != y.begin)
          return x.begin < y.begin;
        return x.end < y.end;
      });
    }
  }

  const std::string invAbs = lineDirs_.ToAbsolutePath(*m.invFile);
  auto it = definesByAbsPath_.find(invAbs);
  if (it == definesByAbsPath_.end())
    return false;

  const uint64_t x = *m.invB;
  const auto &vec = it->second;
  if (vec.empty())
    return false;

  // Binary-search for the last define extent whose begin offset is <= x.  This
  // is the normal candidate in the non-overlapping case.
  size_t lo = 0;
  size_t hi = vec.size();
  while (lo < hi) {
    size_t mid = lo + (hi - lo) / 2;
    if (vec[mid].begin <= x)
      lo = mid + 1;
    else
      hi = mid;
  }

  auto contains = [&](const DefineDirectiveExtent &extent) {
    return x >= extent.begin && x < extent.end;
  };

  if (lo != 0 && contains(vec[lo - 1]))
    return true;

  // Overlapping #define extents are unusual but legal in the metadata model.
  // Scan the adjacent window around the binary-search position exactly as the
  // previous implementation did, but against this engine's private index.
  for (size_t i = lo; i < vec.size() && i < lo + 4; ++i) {
    if (contains(vec[i]))
      return true;
  }
  for (size_t i = lo; i > 0 && i + 4 > lo; --i) {
    if (contains(vec[i - 1]))
      return true;
  }

  return false;
}

/// Describes how strongly a PP range is covered by a macro invocation, from
/// most specific (`Body`) to no meaningful cover (`None`).
enum class MacroCoverRank : uint8_t {
  Body = 0,
  ArgLike = 1,
  Cover = 2,
  None = 3,
};

static inline StringRef toString(MacroCoverRank rank) {
  switch (rank) {
  case MacroCoverRank::Body:
    return "Body";
  case MacroCoverRank::ArgLike:
    return "ArgLike";
  case MacroCoverRank::Cover:
    return "Cover";
  case MacroCoverRank::None:
    return "None";
  }
  llvm_unreachable("Invalid MacroCoverRank");
}

const RefoldModel::MacroInvocation *
RefoldEngine::SmallestCoveringPatchableMacro(
    uint64_t aStart, uint64_t aEnd,
    std::optional<uint64_t> ownerIncludeId) const {
  const RefoldModel::MacroInvocation *best = nullptr;
  MacroCoverRank bestRank = MacroCoverRank::None;
  uint64_t bestLen = std::numeric_limits<uint64_t>::max();

  trace("macro/select",
        "select smallest covering patchable macro for A=[{0},{1}) ownerInc={2}",
        aStart, aEnd, ownerIncludeId);

  const bool isInsert = (aStart == aEnd);

  // Return true iff the candidate half-open span [b,e) covers the current A
  // target. For insertions, require the insertion point to lie strictly inside
  // the span (not exactly on its boundary). For non-insertions, require full
  // coverage of [aStart,aEnd).
  auto spanCovers = [&](uint64_t b, uint64_t e) -> bool {
    if (e <= b)
      return false;
    if (isInsert)
      return (b < aStart) && (aStart < e);
    return (b <= aStart) && (aEnd <= e);
  };

  // Among an arbitrary span collection, return the smallest covering span
  // length, or std::nullopt if none of the spans cover the target.
  auto minCoverLenIn = [&](auto &&spans) -> std::optional<uint64_t> {
    std::optional<uint64_t> out;
    for (const auto &sp : spans) {
      if (spanCovers(sp.begin, sp.end)) {
        const uint64_t len = sp.end - sp.begin;
        if (!out || len < *out)
          out = len;
      }
    }
    return out;
  };

  // Return the smallest covering span length among this invocation's argument-
  // derived projections: ordinary argument spans, stringify spans, and paste
  // spans. Used to prefer the tightest argument-local cover inside the macro.
  auto minCoverLenInArgs = [&](const RefoldModel::MacroInvocation &m)
      -> std::optional<uint64_t> {
    std::optional<uint64_t> out;
    for (const auto &sp : m.argSpans) {
      if (spanCovers(sp.begin, sp.end)) {
        const uint64_t len = sp.end - sp.begin;
        if (!out || len < *out)
          out = len;
      }
    }
    for (const auto &sp : m.stringifySpans) {
      if (spanCovers(sp.begin, sp.end)) {
        const uint64_t len = sp.end - sp.begin;
        if (!out || len < *out)
          out = len;
      }
    }
    for (const auto &sp : m.pasteSpans) {
      if (spanCovers(sp.begin, sp.end)) {
        const uint64_t len = sp.end - sp.begin;
        if (!out || len < *out)
          out = len;
      }
    }
    return out;
  };

  // Scan all macro invocations and choose the smallest patchable macro that
  // truthfully covers the requested A-range in the current owner context.
  // Only real callsites are eligible (never invocations spelled inside a
  // #define), and candidates are ranked by how directly they cover the target:
  // body-span cover first, then argument-derived cover, then broad cover as a
  // fallback. Ties are broken by smaller covering span, then lower macro id,
  // for deterministic selection.
  for (const auto &m : model_.GetMacroInvocations()) {
    // Owner filter (when known): avoids selecting a macro record that belongs
    // to a different include instance.
    if (ownerIncludeId) {
      if (!m.ownerIncludeId || *m.ownerIncludeId != *ownerIncludeId)
        continue;
    }

    if (!m.cover.IsValid() || m.cover.end <= m.cover.begin)
      continue;

    // Must be patchable at a real call site.
    if (!m.invB || !m.invE || !m.invText)
      continue;

    // CRITICAL: never patch invocations that are spelled inside a #define.
    if (IsInvocationInsideDefineDirective(m))
      continue;
    // Rank candidates by how directly their spans cover the requested range.
    //   Body:    body span covers the range (direct expansion token)
    //   ArgLike: argument-like span covers the range (arg/stringify/paste)
    //   Cover:   only the broad cover covers the range (fallback)
    MacroCoverRank rank = MacroCoverRank::Cover;
    uint64_t len = m.cover.end - m.cover.begin;

    if (auto bodyLen = minCoverLenIn(m.bodySpans)) {
      rank = MacroCoverRank::Body;
      len = *bodyLen;
    } else if (auto argLen = minCoverLenInArgs(m)) {
      rank = MacroCoverRank::ArgLike;
      len = *argLen;
    } else {
      if (!m.Covers(aStart, aEnd))
        continue;
      rank = MacroCoverRank::Cover;
      len = m.cover.end - m.cover.begin;
    }

    trace("macro/select",
          "candidate macro id={0} name='{1}' rank={2} len={3} cover=[{4},{5}) "
          "ownerInc={6} invFile='{7}' inv=[{8},{9})",
          m.id, m.name, rank, len, m.cover.begin, m.cover.end,
          m.ownerIncludeId,
          (m.invFile ? StringRef(*m.invFile) : StringRef("")), *m.invB,
          *m.invE);

    if (!best || static_cast<unsigned>(rank) < static_cast<unsigned>(bestRank) ||
        (rank == bestRank &&
         (len < bestLen || (len == bestLen && m.id < best->id)))) {
      best = &m;
      bestRank = rank;
      bestLen = len;
    }
  }

  if (best) {
    trace("macro/select",
          "selected macro id={0} name='{1}' rank={2} len={3} cover=[{4},{5}) "
          "ownerInc={6} invFile='{7}' inv=[{8},{9})",
          best->id, best->name, bestRank, bestLen, best->cover.begin,
          best->cover.end, best->ownerIncludeId,
          (best->invFile ? StringRef(*best->invFile) : StringRef("")),
          *best->invB, *best->invE);
  } else {
    trace("macro/select", "selected macro: <none>");
  }

  return best;
}

std::optional<uint64_t>
RefoldEngine::FindProvableTUInsertionAnchor(uint64_t pp, StringRef tuPath,
                                            TUAnchorWitness *witness) const {
  // First prefer an exact structural slot anchor recorded by the producer.
  // These anchors are the strongest evidence because they identify a specific
  // TU byte boundary corresponding to this PP gap.
  TUAnchorWitness slotWitness;
  if (auto slotAnchor =
          AnchorToExactSlotBoundaryFromPPGap(tuPath, pp, &slotWitness)) {
    if (witness)
      *witness = slotWitness;
    const AcceptedResultCandidate slotCandidate =
        BuildAcceptedTUAnchorCandidate(AcceptedPathKind::TUExactSlotBoundary,
                                       slotWitness);
    trace("hunk",
          "    insertion gap PP={0} mapsToTU via slot boundary TU byte {1} "
          "candidate={2}",
          pp, slotAnchor, FormatAcceptedResultCandidate(slotCandidate));
    return slotAnchor;
  }

  auto includeDirectiveBoundaryAnchor = [&]() -> std::optional<uint64_t> {
    // Strict consumer-side proof for the narrow include-boundary case where a
    // producer slot would normally be present.  A PP gap at the exact boundary
    // between two top-level include expansions denotes the TU source boundary
    // between the two include directive lines, but IncludeIdCoveringPPIndex(pp)
    // treats that same coordinate as being inside the right include's cover.
    // Recognize only the unambiguous form: two adjacent include directives in
    // this TU, with the left include ending at pp, the right include beginning
    // at pp, and no intervening source bytes between the directive records.
    const RefoldModel::IncludeItem *leftInc = nullptr;
    const RefoldModel::IncludeItem *rightInc = nullptr;

    for (const auto &inc : model_.GetIncludes()) {
      if (!inc.cover.IsValid() || !PathsEqual(inc.sitePath, tuPath))
        continue;
      if (inc.cover.end == pp) {
        if (!leftInc || std::tie(inc.siteE, inc.id) <
                            std::tie(leftInc->siteE, leftInc->id))
          leftInc = &inc;
      }
      if (inc.cover.begin == pp) {
        if (!rightInc || std::tie(inc.siteB, inc.id) <
                             std::tie(rightInc->siteB, rightInc->id))
          rightInc = &inc;
      }
    }

    if (!leftInc || !rightInc || leftInc->id == rightInc->id)
      return std::nullopt;

    // Do not infer through nested include structure.  This helper is only for a
    // TU insertion between two include directive lines spelled in tuPath.
    if (leftInc->parent || rightInc->parent)
      return std::nullopt;

    // If there are comments, blank lines, or any other bytes between the
    // directives, strict mode still requires an exact producer slot.  Without
    // that slot there is no canonical byte inside the wider source gap.
    if (leftInc->siteE != rightInc->siteB)
      return std::nullopt;

    TUAnchorWitness includeBoundaryWitness;
    includeBoundaryWitness.evidence =
        TUAnchorEvidenceKind::IncludeDirectiveBoundary;
    includeBoundaryWitness.hasPPGap = true;
    includeBoundaryWitness.ppGap = pp;
    includeBoundaryWitness.hasTUByte = true;
    includeBoundaryWitness.tuByte = leftInc->siteE;
    includeBoundaryWitness.hasLeftNeighbor = true;
    includeBoundaryWitness.leftNeighborPP = pp - 1;
    includeBoundaryWitness.hasRightNeighbor = true;
    includeBoundaryWitness.rightNeighborPP = pp;
    includeBoundaryWitness.outsideIncludeCoverage = false;
    includeBoundaryWitness.ownerDepthStable = true;

    if (witness)
      *witness = includeBoundaryWitness;

    const AcceptedResultCandidate includeBoundaryCandidate =
        BuildAcceptedTUAnchorCandidate(
            AcceptedPathKind::TUProvableInsertionAnchor,
            includeBoundaryWitness);
    trace("tu/anchor",
          "provable TU insertion anchor: ppGap={0} -> include boundary byte={1} "
          "leftInclude={2} rightInclude={3} candidate={4}",
          pp, leftInc->siteE, leftInc->id, rightInc->id,
          FormatAcceptedResultCandidate(includeBoundaryCandidate));
    return leftInc->siteE;
  };

  // Try the exact include-directive-boundary proof before the generic include
  // coverage wall.  The boundary coordinate is allowed to equal the first token
  // of the right include expansion even though that makes
  // IncludeIdCoveringPPIndex report the right include as covering pp.
  if (auto includeAnchor = includeDirectiveBoundaryAnchor())
    return includeAnchor;

  // A PP gap that lies inside an include expansion cannot be materialized as a
  // TU insertion. Fail closed before considering weaker local evidence.
  if (IncludeIdCoveringPPIndex(pp))
    return std::nullopt;

  // Look for an exact TU-side macro-projection begin at this PP gap and, when
  // one exists, lift it to the outermost matching caller so the returned anchor
  // is the stable callsite-begin byte for wrapper/deferred-expansion shapes.
  auto exactArgLikeBeginAnchor = [&]() -> std::optional<uint64_t> {
    SmallVector<const RefoldModel::MacroInvocation *, 8> cands;
    DenseMap<uint64_t, const RefoldModel::MacroInvocation *> invById;
    invById.reserve(model_.GetMacroInvocations().size());
    for (const auto &mi : model_.GetMacroInvocations())
      invById[mi.id] = &mi;

    auto appendIfExactBegin = [&](const RefoldModel::MacroInvocation &m,
                                  auto &&spans) {
      for (const auto &sp : spans) {
        if (sp.begin == pp) {
          cands.push_back(&m);
          break;
        }
      }
    };

    for (const auto &m : model_.GetMacroInvocations()) {
      // Only consider real TU-side invocations with stable byte-space
      // provenance. Ignore invocations inside macro definitions, since those do
      // not denote a concrete callsite insertion point in TU source.
      if (!m.invFile || !m.invB || !m.invE || !m.invText)
        continue;
      if (!PathsEqual(*m.invFile, tuPath))
        continue;
      if (IsInvocationInsideDefineDirective(m))
        continue;

      // Treat argument, stringify, and paste projection starts as "arg-like"
      // begins. If the PP gap lands exactly on one of these begins, the outer
      // callsite begin can serve as a truthful TU insertion anchor.
      appendIfExactBegin(m, m.argSpans);
      if (!cands.empty() && cands.back() == &m)
        continue;
      appendIfExactBegin(m, m.stringifySpans);
      if (!cands.empty() && cands.back() == &m)
        continue;
      appendIfExactBegin(m, m.pasteSpans);
    }

    if (cands.empty())
      return std::nullopt;

    SmallDenseSet<uint64_t, 8> candIds;
    for (const auto *m : cands)
      candIds.insert(m->id);

    auto rootmostCand = [&](const RefoldModel::MacroInvocation *m) {
      const RefoldModel::MacroInvocation *cur = m;
      while (cur && cur->callerMacroId) {
        auto idIt = candIds.find(*cur->callerMacroId);
        if (idIt == candIds.end())
          break;
        auto parentIt = invById.find(*cur->callerMacroId);
        if (parentIt == invById.end())
          break;
        cur = parentIt->second;
      }
      return cur;
    };

    const RefoldModel::MacroInvocation *best = nullptr;
    for (const auto *m : cands) {
      const auto *root = rootmostCand(m);
      if (!root || !root->invB)
        continue;

      // Prefer the outermost candidate among the matching nested invocations,
      // then break ties by earliest callsite begin. This yields the most stable
      // TU anchor for wrapper/deferred-expansion patterns.
      if (!best || std::tie(*root->invB, root->id) <
                       std::tie(*best->invB, best->id)) {
        best = root;
      }
    }

    if (!best)
      return std::nullopt;

    TUAnchorWitness argLikeWitness;
    argLikeWitness.evidence = TUAnchorEvidenceKind::ArgLikeBegin;
    argLikeWitness.hasPPGap = true;
    argLikeWitness.ppGap = pp;
    argLikeWitness.hasTUByte = true;
    argLikeWitness.tuByte = *best->invB;
    argLikeWitness.macroId = best->id;
    argLikeWitness.outsideIncludeCoverage = true;
    if (witness)
      *witness = argLikeWitness;
    const AcceptedResultCandidate argLikeCandidate =
        BuildAcceptedTUAnchorCandidate(
            AcceptedPathKind::TUProvableInsertionAnchor, argLikeWitness);
    trace("tu/anchor",
          "pure insertion arg-like begin anchor: ppGap={0} -> macro id={1} "
          "name='{2}' invB={3} candidate={4}",
          pp, best->id, best->name, *best->invB,
          FormatAcceptedResultCandidate(argLikeCandidate));
    return *best->invB;
  };

  // Next try the exact "arg-like begin" rule used for outer wrapper callsites.
  // This handles empty-gap edits that are semantically attached to the start of
  // a TU macro invocation rather than to an immediately mapped PP token.
  if (auto argAnchor = exactArgLikeBeginAnchor())
    return argAnchor;

  const auto &tokmapByPP = model_.GetTokmapByPP();

  // First try the mapped token immediately to the right of the PP gap. If it
  // belongs to the TU, anchor at that token's begin byte; if it is mapped to a
  // different file, fail closed rather than probing past contradictory local
  // evidence.
  if (pp < model_.GetTokensCountA()) {
    auto rightIt = tokmapByPP.find(pp);
    if (rightIt != tokmapByPP.end()) {
      const auto &right = rightIt->second;
      if (PathsEqual(tuPath, right.file)) {
        TUAnchorWitness rightWitness;
        rightWitness.evidence = TUAnchorEvidenceKind::ImmediateRightNeighbor;
        rightWitness.hasPPGap = true;
        rightWitness.ppGap = pp;
        rightWitness.hasTUByte = true;
        rightWitness.tuByte = right.b;
        rightWitness.hasRightNeighbor = true;
        rightWitness.rightNeighborPP = pp;
        rightWitness.outsideIncludeCoverage = true;
        if (witness)
          *witness = rightWitness;
        const AcceptedResultCandidate rightCandidate =
            BuildAcceptedTUAnchorCandidate(
                AcceptedPathKind::TUProvableInsertionAnchor, rightWitness);
        trace("tu/anchor",
              "provable TU insertion anchor: ppGap={0} -> right neighbor "
              "byte={1} candidate={2}",
              pp, right.b, FormatAcceptedResultCandidate(rightCandidate));
        return right.b;
      }
      return std::nullopt;
    }
  }

  // Otherwise try the mapped token immediately to the left. If it belongs to
  // the TU, anchor at that token's end byte; if it belongs elsewhere, fail
  // closed.
  if (pp > 0) {
    auto leftIt = tokmapByPP.find(pp - 1);
    if (leftIt != tokmapByPP.end()) {
      const auto &left = leftIt->second;
      if (PathsEqual(tuPath, left.file)) {
        TUAnchorWitness leftWitness;
        leftWitness.evidence = TUAnchorEvidenceKind::ImmediateLeftNeighbor;
        leftWitness.hasPPGap = true;
        leftWitness.ppGap = pp;
        leftWitness.hasTUByte = true;
        leftWitness.tuByte = left.e;
        leftWitness.hasLeftNeighbor = true;
        leftWitness.leftNeighborPP = pp - 1;
        leftWitness.outsideIncludeCoverage = true;
        if (witness)
          *witness = leftWitness;
        const AcceptedResultCandidate leftCandidate =
            BuildAcceptedTUAnchorCandidate(
                AcceptedPathKind::TUProvableInsertionAnchor, leftWitness);
        trace("tu/anchor",
              "provable TU insertion anchor: ppGap={0} -> left neighbor "
              "byte={1} candidate={2}",
              pp, left.e, FormatAcceptedResultCandidate(leftCandidate));
        return left.e;
      }
      return std::nullopt;
    }
  }

  // In strict mode we stop here: without an exact structural anchor, an exact
  // arg-like anchor, or an immediate TU neighbor, the gap is not provably TU.
  if (strict_)
    return std::nullopt;

  static constexpr uint64_t MAX_SNAP_DISTANCE = 64;
  const bool haveOwnerGaps =
      (ownerDepthGap_.size() == model_.GetTokensCountA() + 1);
  const uint32_t wantOwner =
      (haveOwnerGaps && pp < ownerDepthGap_.size()) ? ownerDepthGap_[pp] : 0;

  const RefoldModel::TokMapEntry *left = nullptr;
  uint64_t dLeft = std::numeric_limits<uint64_t>::max();

  // Non-strict fallback: walk leftward through nearby unmapped whitespace, but
  // stop as soon as the owner-depth context changes. This prevents the probe
  // from drifting across a structural ownership seam.
  for (uint64_t d = 2; d <= MAX_SNAP_DISTANCE; ++d) {
    if (pp < d)
      break;
    if (haveOwnerGaps) {
      const uint64_t gap = pp - (d - 1);
      if (gap < ownerDepthGap_.size() && ownerDepthGap_[gap] != wantOwner)
        break;
    }
    auto it = tokmapByPP.find(pp - d);
    if (it != tokmapByPP.end()) {
      left = &it->second;
      dLeft = d;
      break;
    }
  }

  const RefoldModel::TokMapEntry *right = nullptr;
  uint64_t dRight = std::numeric_limits<uint64_t>::max();
  const uint64_t maxPP = model_.GetTokensCountA();

  // Mirror the same bounded whitespace probe to the right, with the same
  // owner-depth guard.
  for (uint64_t d = 1; d <= MAX_SNAP_DISTANCE; ++d) {
    uint64_t ppR = pp + d;
    if (ppR >= maxPP)
      break;
    if (haveOwnerGaps && ppR < ownerDepthGap_.size() &&
        ownerDepthGap_[ppR] != wantOwner)
      break;
    auto it = tokmapByPP.find(ppR);
    if (it != tokmapByPP.end()) {
      right = &it->second;
      dRight = d;
      break;
    }
  }

  // Accept the non-strict probe only when both corroborating neighbors exist
  // and both resolve to the TU. A one-sided or mixed-file result is not strong
  // enough to prove TU ownership.
  if (!left || !right)
    return std::nullopt;
  if (!PathsEqual(tuPath, left->file) || !PathsEqual(tuPath, right->file))
    return std::nullopt;

  // Use the nearer corroborating TU boundary as the concrete zero-width anchor,
  // preferring the right side on an equal-distance tie.
  if (dRight <= dLeft) {
    TUAnchorWitness corroboratedRightWitness;
    corroboratedRightWitness.evidence =
        TUAnchorEvidenceKind::CorroboratedRightNeighbor;
    corroboratedRightWitness.hasPPGap = true;
    corroboratedRightWitness.ppGap = pp;
    corroboratedRightWitness.hasTUByte = true;
    corroboratedRightWitness.tuByte = right->b;
    corroboratedRightWitness.hasLeftNeighbor = true;
    corroboratedRightWitness.leftNeighborPP = pp - dLeft;
    corroboratedRightWitness.hasRightNeighbor = true;
    corroboratedRightWitness.rightNeighborPP = pp + dRight;
    corroboratedRightWitness.outsideIncludeCoverage = true;
    corroboratedRightWitness.ownerDepthStable = true;
    if (witness)
      *witness = corroboratedRightWitness;
    const AcceptedResultCandidate corroboratedRightCandidate =
        BuildAcceptedTUAnchorCandidate(
            AcceptedPathKind::TUProvableInsertionAnchor,
            corroboratedRightWitness);
    trace("tu/anchor",
          "provable TU insertion anchor: ppGap={0} -> corroborated right "
          "byte={1} candidate={2}",
          pp, right->b,
          FormatAcceptedResultCandidate(corroboratedRightCandidate));
    return right->b;
  }
  TUAnchorWitness corroboratedLeftWitness;
  corroboratedLeftWitness.evidence =
      TUAnchorEvidenceKind::CorroboratedLeftNeighbor;
  corroboratedLeftWitness.hasPPGap = true;
  corroboratedLeftWitness.ppGap = pp;
  corroboratedLeftWitness.hasTUByte = true;
  corroboratedLeftWitness.tuByte = left->e;
  corroboratedLeftWitness.hasLeftNeighbor = true;
  corroboratedLeftWitness.leftNeighborPP = pp - dLeft;
  corroboratedLeftWitness.hasRightNeighbor = true;
  corroboratedLeftWitness.rightNeighborPP = pp + dRight;
  corroboratedLeftWitness.outsideIncludeCoverage = true;
  corroboratedLeftWitness.ownerDepthStable = true;
  if (witness)
    *witness = corroboratedLeftWitness;
  const AcceptedResultCandidate corroboratedLeftCandidate =
      BuildAcceptedTUAnchorCandidate(
          AcceptedPathKind::TUProvableInsertionAnchor, corroboratedLeftWitness);
  trace("tu/anchor",
        "provable TU insertion anchor: ppGap={0} -> corroborated left byte={1} "
        "candidate={2}",
        pp, left->e, FormatAcceptedResultCandidate(corroboratedLeftCandidate));
  return left->e;
}

bool RefoldEngine::HunkMapsToTU(uint64_t a0, uint64_t a1,
                                StringRef tuPath) const {
  trace("tu/own", "hunkMapsToTU: check ownership for A[{0},{1}) tu={2}", a0, a1,
        tuPath);
  bool sawAnyTU = false;
  const auto &tokmapByPP = model_.GetTokmapByPP();

  // Walk the A-side PP byte range and require every mapped byte to belong to
  // the translation unit itself. Unmapped bytes (whitespace/separators) are
  // ignored; the hunk ceases to be TU-owned as soon as any mapped byte resolves
  // to a different file.
  for (uint64_t pp = a0; pp < a1; ++pp) {
    auto it = tokmapByPP.find(pp);
    if (it == tokmapByPP.end())
      continue; // ignore unmapped (spaces/tabs/newlines)
    const auto &t = it->second;
    if (!PathsEqual(t.file, tuPath)) {
      trace("tu/own",
            "hunkMapsToTU: A[{0},{1}) hits non-TU mapping at pp={2} file={3} "
            "(tu={4}) -> false",
            a0, a1, pp, t.file, tuPath);
      return false; // spans a non-TU mapping
    }
    sawAnyTU = true;
  }

  // Non-empty range: if we only saw TU mappings (or nothing but whitespace),
  // then the hunk maps to the TU. Otherwise it mapped to some header above.
  if (a0 != a1) {
    if (!sawAnyTU) {
      trace("tu/own",
            "hunkMapsToTU: A[{0},{1}) has no TU-mapped tokens "
            "(unmapped/whitespace-only) -> false",
            a0, a1);
    }
    return sawAnyTU;
  }

  // INSERTION (A gap): classify TU ownership only when we can derive a
  // truthful TU insertion anchor at that exact PP gap.
  return FindProvableTUInsertionAnchor(a0, tuPath).has_value();
}

std::optional<uint64_t> RefoldEngine::AnchorToExactSlotBoundaryFromPPGap(
    StringRef tuPath, uint64_t ppGap, TUAnchorWitness *witness) const {
  // Candidate record for potential anchor points
  struct Cand {
    uint64_t pp; // PP coordinate for the boundary
    uint64_t b;  // TU byte coordinate (possibly adjusted)
    const RefoldModel::Slot *slot;

    Cand(uint64_t pp, uint64_t b, const RefoldModel::Slot *slot)
        : pp(pp), b(b), slot(slot) {}
  };

  // Read TU text for newline-aware slot adjustment
  auto bufOrErr = MemoryBuffer::getFile(lineDirs_.ToAbsolutePath(tuPath));
  if (!bufOrErr) {
    fatal("slot/anchor", "unable to read TU: {0}", tuPath);
    // Should be unreachable!
  }
  StringRef tuText = bufOrErr.get()->getBuffer();

  // Helper to adjust slots that terminate on directive newlines
  auto adjustSlot = [&tuText, &tuPath,
                     this](const RefoldModel::Slot *s) -> uint64_t {
    uint64_t b = s->b;

    // Special case: the producer anchors the selected arm_end at the first
    // directive following the arm body (often "#else" or "#endif"). However,
    // the corresponding PP gap is observed *after* the entire conditional group
    // when replay-preprocessing. To keep pure-insertion anchoring consistent
    // with the boundary policy (outside the conditional group), re-anchor the
    // arm_end boundary to the end of the group's "#endif" line.
    if (s->kind == "arm_end" && s->pp) {
      const uint64_t pp = *s->pp;
      if (pp > 0) {
        if (auto armRef = model_.FindArmRefAtPP(pp - 1)) {
          if (armRef->group && PathsEqual(armRef->group->file, tuPath)) {
            const uint64_t ge = armRef->group->groupE;
            if (ge <= tuText.size())
              return ge;
          }
        }
      }
      // Fall back to the raw slot byte if we cannot resolve the group.
      return b;
    }

    bool needsNoNewlineAdjustment =
        StringSwitch<bool>(s->kind)
            .Cases("after_include", "after_last_include", true)
            .Default(false);
    if (needsNoNewlineAdjustment)
      return b;

    if (b >= tuText.size())
      return b;

    char c = tuText[b];
    if (c == '\n')
      return b + 1;
    if (c == '\r') {
      if ((size_t)(b + 1) < tuText.size() && tuText[b + 1] == '\n')
        return b + 2;
      return b + 1;
    }
    return b;
  };

  std::vector<Cand> cands;

  // 1) Explicit TU slots that already carry 'pp'
  for (const auto &s :
       model_.FindSlots(tuPath, std::nullopt, std::nullopt, std::nullopt)) {
    if (!s->pp)
      continue;

    bool isBoundary =
        StringSwitch<bool>(s->kind)
            .Cases("file_begin", "file_end", "before_include", "after_include",
                   "after_last_include", "arm_begin", "arm_end", true)
            .Default(false);
    if (isBoundary) {
      cands.emplace_back(*s->pp, adjustSlot(s), s);
    }
  }

  // ---------------------------------------------------------------------------
  // NOTE: Slot.pp is now produced by clang for boundary-like slots (includes,
  // arms, file boundaries). We therefore intentionally do NOT reconstruct PP
  // coordinates from include/conditional metadata on the consumer side, since
  // that can diverge from the producer's authoritative view in edge cases
  // (nested includes, re-entrant conditionals, etc.).
  // ---------------------------------------------------------------------------

  if (cands.empty())
    return std::nullopt;

  // Filter for EXACT matches to the ppGap
  std::vector<const Cand *> exact;
  for (const auto &c : cands) {
    if (c.pp == ppGap)
      exact.push_back(&c);
  }

  if (exact.empty())
    return std::nullopt;

  // Priority tie-breaking logic
  auto getPriority = [](StringRef kind) -> unsigned {
    if (kind == "before_include")
      return 0;
    if (kind == "after_include")
      return 1;
    if (kind == "after_last_include")
      return 2;
    if (kind == "arm_begin")
      return 3;
    if (kind == "arm_end")
      return 4;
    if (kind == "file_begin")
      return 5;
    if (kind == "file_end")
      return 6;
    return 100;
  };

  // Choose the strongest exact slot match, preferring higher-priority slot
  // kinds first, then earlier TU byte offsets, then smaller slot IDs.
  const Cand *best = nullptr;
  for (const auto *c : exact) {
    if (!best) {
      best = c;
      continue;
    }
    unsigned pc = getPriority(c->slot->kind);
    unsigned pb = getPriority(best->slot->kind);

    // Tie-break: Priority -> Byte Offset -> Slot ID
    if (std::tie(pc, c->b, c->slot->id) <
        std::tie(pb, best->b, best->slot->id)) {
      best = c;
    }
  }

  if (best) {
    TUAnchorWitness exactSlotWitness;
    exactSlotWitness.evidence = TUAnchorEvidenceKind::ExactSlotBoundary;
    exactSlotWitness.hasPPGap = true;
    exactSlotWitness.ppGap = ppGap;
    exactSlotWitness.hasTUByte = true;
    exactSlotWitness.tuByte = best->b;
    exactSlotWitness.exactPPMatch = true;
    exactSlotWitness.slotId = best->slot->id;
    exactSlotWitness.slotKind = best->slot->kind.str();
    if (witness)
      *witness = exactSlotWitness;
    const AcceptedResultCandidate exactSlotCandidate =
        BuildAcceptedTUAnchorCandidate(AcceptedPathKind::TUExactSlotBoundary,
                                       exactSlotWitness);
    trace("slots/anchor",
          "AnchorToExactSlotBoundaryFromPPGap: ppGap={0} -> slotId={1} "
          "kind={2} tuByte={3} candidate={4}",
          ppGap, best->slot->id, best->slot->kind, best->b,
          FormatAcceptedResultCandidate(exactSlotCandidate));
  } else {
    trace("slots/anchor",
          "AnchorToExactSlotBoundaryFromPPGap: ppGap={0} -> <none>", ppGap);
  }
  return best ? std::optional<uint64_t>(best->b) : std::nullopt;
}

std::optional<std::pair<uint64_t, uint64_t>>
RefoldEngine::TUByteSpan(uint64_t a0, uint64_t a1, StringRef tuPath) const {
  if (a0 > a1)
    std::swap(a0, a1);

  const bool isEmpty = (a0 == a1);
  const auto &tokmapByPP = model_.GetTokmapByPP();

  // For pure insertions, use the same conservative TU-anchor proof used by
  // HunkMapsToTU so classification and concrete TU realization cannot diverge.
  if (isEmpty) {
    if (auto anchor = FindProvableTUInsertionAnchor(a0, tuPath))
      return {{*anchor, *anchor}};
  }

  // Non-empty: compute min/max over TU-mapped subset only.
  uint64_t minB = std::numeric_limits<uint64_t>::max();
  uint64_t maxE = 0;
  bool foundTuToken = false;

  // Compute the minimal TU byte envelope covered by the mapped A-side tokens in
  // this hunk, ignoring unmapped PP bytes and tokens that belong to other files.
  for (uint64_t i = a0; i < a1; ++i) {
    auto it = tokmapByPP.find(i);
    if (it == tokmapByPP.end())
      continue;

    const auto &ent = it->second;
    if (!PathsEqual(ent.file, tuPath))
      continue;

    if (ent.b < minB)
      minB = ent.b;
    if (ent.e > maxE)
      maxE = ent.e;
    foundTuToken = true;
  }

  if (foundTuToken)
    return {{minB, maxE}};

  // No TU byte span: either this is an empty hunk with no safe TU insertion
  // anchor, or a non-empty hunk with no TU-owned mapped tokens.
  return std::nullopt;
}

const RefoldModel::IncludeItem *
RefoldEngine::BoundaryParentIncludeForPureInsertion(
    const diffutils::Hunk &h) const {
  // This helper only applies to a pure insertion: the hunk must consume no
  // A-side tokens, but it must insert at least one B-side token.
  if (!h.isInsertOnly()) {
    return nullptr;
  }

  const uint64_t aPos = h.aStart;

  // We deliberately avoid "nearest token" probing here. A pure insertion is
  // attributed to an include only when the PP gap lands exactly on a recorded
  // include boundary.
  //
  // Collect the narrowest include ending at this gap (immediately on the left)
  // and the narrowest include beginning at this gap (immediately on the right).
  // Preferring the narrowest match lets an exact nested boundary beat any
  // enclosing include that shares the same endpoint.
  const RefoldModel::IncludeItem *leftBest = nullptr;
  uint64_t leftWidth = std::numeric_limits<uint64_t>::max();

  const RefoldModel::IncludeItem *rightBest = nullptr;
  uint64_t rightWidth = std::numeric_limits<uint64_t>::max();

  for (const auto &inc : model_.GetIncludes()) {
    if (!inc.cover.IsValid())
      continue;

    const uint64_t width = inc.cover.end - inc.cover.begin;

    // Include immediately to the left of the insertion gap.
    if (inc.cover.end == aPos) {
      if (width < leftWidth) {
        leftBest = &inc;
        leftWidth = width;
      }
    }

    // Include immediately to the right of the insertion gap.
    if (inc.cover.begin == aPos) {
      if (width < rightWidth) {
        rightBest = &inc;
        rightWidth = width;
      }
    }
  }

  const std::optional<uint64_t> leftIncId =
      leftBest ? std::optional<uint64_t>(leftBest->id) : std::nullopt;
  const std::optional<uint64_t> rightIncId =
      rightBest ? std::optional<uint64_t>(rightBest->id) : std::nullopt;

  // If neither side hits an exact include boundary, this insertion cannot be
  // attributed to an include via boundary ownership.
  if (!leftIncId && !rightIncId)
    return nullptr;

  // When the gap sits between two include boundaries, attribute it to the
  // structural parent shared by the left and right side. This handles both
  // "between siblings" and "at one side only" cases uniformly.
  const std::optional<uint64_t> parentId =
      model_.LeastCommonAncestorInclude(leftIncId, rightIncId);
  if (!parentId)
    return nullptr;

  const RefoldModel::IncludeItem *parent = model_.GetIncludeById(*parentId);

  trace("include/boundary",
        "BoundaryParentIncludeForPureInsertion: aPos={0} leftInc={1} "
        "rightInc={2} parent={3}",
        aPos, leftIncId, rightIncId, parentId);

  return parent;
}

// ===================== Patch builders (include & macro) ======================

std::optional<size_t> RefoldEngine::FindExactOwningArgSpanForPureInsertion(
    uint64_t aPos, ArrayRef<RefoldModel::PPArgSpan> argSpans) const {
  auto isCommaTok = [&](uint64_t a) -> bool {
    return a < aToks_.size() &&
           aToks_[static_cast<size_t>(a)].spelling == ",";
  };

  // First prefer the simple containment case: if the pure-insertion gap lies
  // within an occurrence's half-open A-side span, that occurrence owns it.
  for (size_t i = 0; i < argSpans.size(); ++i) {
    const auto &s = argSpans[i];
    if (aPos >= s.begin && aPos < s.end)
      return i;
  }

  // Next handle the exact separator-before-right-occurrence case. When the gap
  // sits on a comma token, attribute it to an occurrence that begins
  // immediately after that comma.
  if (isCommaTok(aPos)) {
    for (size_t i = 0; i < argSpans.size(); ++i) {
      const auto &s = argSpans[i];
      if (s.begin == aPos + 1)
        return i;
    }
  }

  // Finally allow an exact span-end owner when no containing occurrence or
  // right-hand separator owner claimed the insertion first.
  for (size_t i = 0; i < argSpans.size(); ++i) {
    const auto &s = argSpans[i];
    if (aPos == s.end)
      return i;
  }

  // No exact structural owner exists for this pure insertion.
  return std::nullopt;
}

std::optional<std::pair<size_t, size_t>>
RefoldEngine::GetOwnedPureInsertionBRangeForArgSpan(
    const RefoldModel::PPArgSpan &span,
    ArrayRef<RefoldModel::PPArgSpan> argSpans,
    std::pair<size_t, size_t> mappedEnv,
    const diffutils::Hunk &h) const {
  // This helper only applies to pure insertions with a non-empty B-side token
  // range.
  if (h.aStart != h.aEnd || h.bStart >= h.bEnd)
    return std::nullopt;

  const uint64_t aPos = h.aStart;
  const size_t insB0 = static_cast<size_t>(h.bStart);
  const size_t insB1 = static_cast<size_t>(h.bEnd);
  const size_t envB0 = mappedEnv.first;

  // If the gap lies inside the occurrence itself, the owned B range is just the
  // raw inserted token range.
  if (aPos >= span.begin && aPos < span.end)
    return std::make_pair(insB0, insB1);

  const bool isCommaSeparator =
      aPos < aToks_.size() && aToks_[static_cast<size_t>(aPos)].spelling == ",";

  // Exact separator-before-right-occurrence case:
  //
  //   A:  ... , <span> ...
  //          ^
  //        aPos
  //
  // The raw inserted B range includes the shared leading separator. To make the
  // insertion occurrence-owned by the right-hand span, shift the owned B range
  // right by one token so that:
  //   - the shared leading comma is excluded, and
  //   - the comma that now precedes the original occurrence in B is included.
  //
  // The mapped envelope for the occurrence must begin exactly one token after
  // the raw inserted range; otherwise this structural ownership transform does
  // not hold.
  if (isCommaSeparator && span.begin == aPos + 1) {
    if (envB0 == insB1 + 1)
      return std::make_pair(insB0 + 1, insB1 + 1);
    return std::nullopt;
  }

  // Exact span-end ownership:
  //
  // If the gap is exactly at this occurrence's end, the raw inserted B range is
  // owned by this occurrence, unless the same A-side position is also the comma
  // separator immediately before some right-hand occurrence. In that case the
  // right-hand separator owner takes precedence and this span must not claim
  // the insertion.
  if (aPos == span.end) {
    if (isCommaSeparator) {
      for (const auto &s : argSpans) {
        if (s.begin == aPos + 1)
          return std::nullopt;
      }
    }
    return std::make_pair(insB0, insB1);
  }

  // This occurrence does not exactly own the pure insertion.
  return std::nullopt;
}

bool RefoldEngine::MacroArgReplacementMatchesAllOccurrencesInBImpl(
    const RefoldModel::MacroInvocation &m, uint32_t argIdx, StringRef baseArg,
    StringRef newArg, ArrayRef<diffutils::Hunk> tokenHunks,
    bool checkPasteSpans, OccurrenceSupportMode supportMode) const {
  if (newArg.data() == nullptr)
    return false;

  // Conservatism: if we cannot locate any occurrence metadata for this arg,
  // do not block args-only.
  bool hasAny = false;
  for (const auto &s : m.argSpans) {
    if (s.argIdx == argIdx) {
      hasAny = true;
      break;
    }
  }

  auto hasDirectOccurrenceSupport = [](const RefoldModel::MacroInvocation &inv,
                                       uint32_t formalIdx) -> bool {
    for (const auto &s : inv.argSpans) {
      if (s.argIdx == formalIdx)
        return true;
    }
    for (const auto &s : inv.stringifySpans) {
      if (s.argIdx == formalIdx)
        return true;
    }
    for (const auto &s : inv.pasteSpans) {
      if (s.argIdx == formalIdx)
        return true;
    }
    return false;
  };

  if (!hasAny) {
    if (supportMode == OccurrenceSupportMode::CurrentInvocationOnly) {
      if (!hasDirectOccurrenceSupport(m, argIdx))
        return false;
    } else {
      std::function<bool(const RefoldModel::MacroInvocation &, uint32_t,
                         std::set<std::pair<uint64_t, uint32_t>> &)>
          hasOccurrenceSupportThroughGraph;

    hasOccurrenceSupportThroughGraph =
        [&](const RefoldModel::MacroInvocation &inv, uint32_t formalIdx,
            std::set<std::pair<uint64_t, uint32_t>> &visiting) -> bool {
      std::pair<uint64_t, uint32_t> key{inv.id, formalIdx};
      if (!visiting.insert(key).second)
        return false;

      auto eraseOnExit = llvm::make_scope_exit([&] { visiting.erase(key); });

      if (hasDirectOccurrenceSupport(inv, formalIdx))
        return true;

      auto childIt = macroChildrenById_.find(inv.id);
      if (childIt != macroChildrenById_.end()) {
        for (const auto *child : childIt->second) {
          for (uint32_t childFormalIdx = 0;
               childFormalIdx < child->argDeps.size(); ++childFormalIdx) {
            bool dependsOnFormal = false;
            for (uint32_t dep : child->argDeps[childFormalIdx]) {
              if (dep == formalIdx) {
                dependsOnFormal = true;
                break;
              }
            }
            if (!dependsOnFormal)
              continue;
            if (hasOccurrenceSupportThroughGraph(*child, childFormalIdx,
                                                 visiting))
              return true;
          }
        }
      }

      if (inv.callerMacroId && formalIdx < inv.argDeps.size()) {
        auto parentChildrenIt = macroChildrenById_.find(*inv.callerMacroId);
        if (parentChildrenIt != macroChildrenById_.end()) {
          ArrayRef<uint32_t> deps = inv.argDeps[formalIdx];
          for (const auto *sib : parentChildrenIt->second) {
            if (sib->id == inv.id)
              continue;
            for (uint32_t sibFormalIdx = 0; sibFormalIdx < sib->argDeps.size();
                 ++sibFormalIdx) {
              bool sharesCallerDeps = false;
              for (uint32_t sibDep : sib->argDeps[sibFormalIdx]) {
                if (llvm::is_contained(deps, sibDep)) {
                  sharesCallerDeps = true;
                  break;
                }
              }
              if (!sharesCallerDeps)
                continue;
              if (hasOccurrenceSupportThroughGraph(*sib, sibFormalIdx,
                                                   visiting))
                return true;
            }
          }
        }
      }

      return false;
    };

      std::set<std::pair<uint64_t, uint32_t>> visiting;
      if (!hasOccurrenceSupportThroughGraph(m, argIdx, visiting))
        return false;
    }
  }

  const uint64_t maxTok =
      bTokOff_.empty() ? 0ULL : static_cast<uint64_t>(bTokOff_.size() - 1);
  StringRef argTrim = newArg.trim();
  StringRef baseTrim = baseArg.trim();

  // If this arg is stringified anywhere, accept args-only without enforcing
  // paste-span checks.
  bool argIsStringified = false;
  if (strict_) {
    for (const auto &s : m.stringifySpans) {
      if (s.argIdx == argIdx) {
        argIsStringified = true;
        break;
      }
    }

    // Check all STRINGIFY spans for this argument, but only in strict mode.
    if (argIsStringified) {
      auto canonArg = CanonicalizeStringifyInversePayload(argTrim);
      if (!canonArg || StringRef(*canonArg).trim() != argTrim)
        return false;

      for (const auto &s : m.stringifySpans) {
        if (s.argIdx != argIdx)
          continue;

        auto bEnv = MapAToBTokenEnvelopeByPPArgSpan(s);
        if (!bEnv)
          return false;

        // Extend the B-envelope to account for hunks that touch this
        // occurrence. This is required for insertions at the argument boundary
        // (e.g. appending tokens).
        if (!tokenHunks.empty()) {
          size_t lo = bEnv->first;
          size_t hi = bEnv->second;
          for (const auto &h : tokenHunks) {
            if (auto owned =
                    GetOwnedPureInsertionBRangeForArgSpan(s, m.stringifySpans,
                                                          *bEnv, h)) {
              lo = std::min(lo, owned->first);
              hi = std::max(hi, owned->second);
              continue;
            }

            bool touches;
            if (h.aStart == h.aEnd) {
              touches = false;
            } else {
            // The normal case: split the rewritten core around the original
            // literal delimiters and require a unique segmentation.
              touches = (h.aStart < s.end && h.aEnd > s.begin);
            }
            if (touches && h.bStart < h.bEnd) {
              lo = static_cast<size_t>(std::min<uint64_t>(lo, h.bStart));
              hi = static_cast<size_t>(std::max<uint64_t>(hi, h.bEnd));
            }
          }
          lo = static_cast<size_t>(std::clamp<uint64_t>(lo, 0ULL, maxTok));
          hi = static_cast<size_t>(std::clamp<uint64_t>(hi, lo, maxTok));
          bEnv = {lo, hi};
        }

        StringRef tok = SliceBSource(bEnv->first, bEnv->second).trim();
        if (tok.empty())
          return false;

        if (s.byteBegin && s.byteEnd) {
          if ((bEnv->second - bEnv->first) != 1)
            return false;
          StringRef aTok = SliceASource(static_cast<size_t>(s.begin),
                                        static_cast<size_t>(s.end));
          const uint64_t bb = *s.byteBegin;
          const uint64_t be = *s.byteEnd;
          if (be < bb || be > static_cast<uint64_t>(aTok.size()))
            return false;
          StringRef aPref = aTok.take_front(static_cast<size_t>(bb));
          StringRef aSuff = aTok.drop_front(static_cast<size_t>(be));
          if (tok.starts_with(aPref) && tok.ends_with(aSuff) &&
              tok.size() >= aPref.size() + aSuff.size()) {
            tok = tok.slice(aPref.size(), tok.size() - aSuff.size());
          } else {
            // The normal case: split the rewritten core around the original
            // literal delimiters and require a unique segmentation.
            const uint64_t bbC = std::min<uint64_t>(bb, tok.size());
            const uint64_t beC = std::min<uint64_t>(be, tok.size());
            if (beC < bbC)
              return false;
            tok = tok.slice(static_cast<size_t>(bbC), static_cast<size_t>(beC));
          }
          tok = tok.trim();
        }

        std::string expect = stringutils::quoteCString(argTrim);
        if (tok != expect) {
          trace("macro/consistency",
                "STRINGIFY mismatch inv id={0} name={1} argIdx={2} tok={3} "
                "expect={4}",
                m.id, m.name, argIdx, tok, expect);
          return false;
        }
      }
    }
  }

  // Determine whether token pasting consumes a prefix/suffix/whole segment of
  // this argument. 0=none/unknown, 1=prefix, 2=suffix, 3=whole, 4=ambiguous
  enum PasteType : unsigned { Unknown, Prefix, Suffix, Whole, Ambiguous };
  PasteType pasteConsume = Unknown;
  for (const auto &ps : m.pasteSpans) {
    if (ps.argIdx != argIdx)
      continue;

    StringRef aTokText = SliceASource(ps.begin, ps.end).trim();
    if (aTokText.empty() || !ps.byteBegin || *ps.byteEnd < *ps.byteBegin ||
        static_cast<size_t>(*ps.byteEnd) > aTokText.size())
      continue;

    StringRef segA =
        aTokText.substr(*ps.byteBegin, *ps.byteEnd - *ps.byteBegin);
    if (segA.empty())
      continue;

    bool starts = baseTrim.starts_with(segA);
    bool ends = baseTrim.ends_with(segA);

    PasteType dir = Unknown;
    if (baseTrim == segA)
      dir = Whole;
    else if (starts && !ends)
      dir = Prefix;
    else if (ends && !starts)
      dir = Suffix;
    else if (starts && ends)
      dir = Ambiguous;
    else
      continue;

    if (pasteConsume == Unknown)
      pasteConsume = dir;
    else if (pasteConsume != dir)
      pasteConsume = Ambiguous;
  }

  // Verify all standard (non-paste) occurrences.
  for (const auto &s : m.argSpans) {
    if (s.argIdx != argIdx || s.kind != PPArgSpanKind::Standard)
      continue;

    auto bEnv = MapAToBTokenEnvelopeByPPArgSpan(s);
    if (!bEnv)
      return false;

    // If the argument was deleted entirely in B, the mapped envelope may be
    // empty. Accept this only when the replacement is also empty after
    // trimming.
    if (bEnv->second < bEnv->first)
      return false;

    // Extend the B-envelope to account for hunks that touch this occurrence.
    // This is required for insertions at the argument boundary (e.g. appending
    // tokens).
    if (!tokenHunks.empty()) {
      size_t lo = bEnv->first;
      size_t hi = bEnv->second;
      for (const auto &h : tokenHunks) {
        if (auto owned =
                GetOwnedPureInsertionBRangeForArgSpan(s, m.argSpans, *bEnv,
                                                      h)) {
          lo = std::min(lo, owned->first);
          hi = std::max(hi, owned->second);
          continue;
        }

        bool touches;
        if (h.aStart == h.aEnd) {
          touches = false;
        } else {
          touches = (h.aStart < s.end && h.aEnd > s.begin);
        }
        if (touches && h.bStart < h.bEnd) {
          lo = static_cast<size_t>(std::min<uint64_t>(lo, h.bStart));
          hi = static_cast<size_t>(std::max<uint64_t>(hi, h.bEnd));
        }
      }
      lo = static_cast<size_t>(std::clamp<uint64_t>(lo, 0, maxTok));
      hi = static_cast<size_t>(std::clamp<uint64_t>(hi, lo, maxTok));
      bEnv = {lo, hi};
    }

    StringRef tokText = SliceBSource(bEnv->first, bEnv->second).trim();
    if (tokText.empty()) {
      if (argTrim.empty())
        continue;
      return false;
    }

    bool ok;
    if (pasteConsume == Suffix) {
      // Suffix segment is consumed by pasting; standard expansion is the
      // prefix.
      ok = argTrim.starts_with(tokText);
    } else if (pasteConsume == Prefix) {
      // Prefix segment is consumed by pasting; standard expansion is the
      // suffix.
      ok = argTrim.ends_with(tokText);
    } else {
      ok = (tokText == argTrim);
    }

    if (!ok)
      return false;
  }

  // Paste-span verification is optional for callers that validate paste-token
  // correctness as a group (e.g., multi-span paste edits). When disabled, we
  // only validate standard+stringify occurrences above.
  if (argIsStringified || !checkPasteSpans)
    return true;

  // Verify all paste-span occurrences.
  for (const auto &ps : m.pasteSpans) {
    if (ps.argIdx != argIdx)
      continue;

    // This projected occurrence must still correspond to exactly one token in B.
    auto bEnv = MapAToBTokenEnvelopeByPPArgSpan(ps);
    if (!bEnv || bEnv->second <= bEnv->first ||
        (bEnv->second - bEnv->first) != 1)
      return false;

    // Fetch the trimmed token text for this projected occurrence on both sides;
    // later checks will compare the corresponding projected subranges.
    StringRef aTokText = SliceASource(ps.begin, ps.end).trim();
    if (aTokText.empty())
      return false;
    StringRef bTokText = SliceBSource(bEnv->first, bEnv->second).trim();
    if (bTokText.empty())
      return false;

    // The projection's byte subrange must be valid within the A-side token.
    if (!ps.byteBegin || *ps.byteEnd < *ps.byteBegin ||
        static_cast<size_t>(*ps.byteEnd) > aTokText.size())
      return false;

    // Extract the original projected segment from the A-side token text.
    StringRef oldSeg =
        aTokText.substr(*ps.byteBegin, *ps.byteEnd - *ps.byteBegin);

    auto toSigned = [](std::optional<uint32_t> opt) -> int64_t {
      return static_cast<int64_t>(opt.value_or(0));
    };

    // Shift the projected byte range into B by the whole-token size delta.
    int64_t delta = static_cast<int64_t>(bTokText.size()) -
                    static_cast<int64_t>(aTokText.size());

    int64_t bb = toSigned(ps.byteBegin);
    int64_t be = toSigned(ps.byteEnd) + delta;

    // The translated byte range must remain valid within the B-side token.
    if (bb < 0 || be < bb || static_cast<uint64_t>(be) > bTokText.size())
      return false;

    uint64_t bbB = static_cast<uint64_t>(bb);
    uint64_t beB = static_cast<uint64_t>(be);

    // NOTE: Do NOT require the token outside this segment to be identical
    // between A and B. Multiple macro arguments can contribute to the same
    // pasted token, and a single edit hunk may simultaneously modify multiple
    // segments (e.g., a_b_c -> d_e_f). Full pasted-token consistency is
    // validated separately via pasteArgReplacementsMatchAllPasteTokensInB(...).

    StringRef segB = bTokText.substr(bbB, beB - bbB);

    // Determine where this pasted segment comes from within the original
    // argument spelling.
    bool starts = baseTrim.starts_with(oldSeg);
    bool ends = baseTrim.ends_with(oldSeg);

    // Accept either an exact projected replacement or the corresponding
    // prefix/suffix match when this segment represents a trimmed edge.
    bool ok = false;
    if (baseTrim == oldSeg)
      ok = (argTrim == segB);
    else if (starts && !ends)
      ok = argTrim.starts_with(segB);
    else if (ends && !starts)
      ok = argTrim.ends_with(segB);

    // Check to see if it's ambiguous or unclassified.
    if (!ok)
      return false;
  }

  return true;
}

bool RefoldEngine::HunkTouchesAnyPasteToken(
    const RefoldModel::MacroInvocation &m, const diffutils::Hunk &h) {
  if (m.pasteSpans.empty())
    return false;

  const uint64_t a0 = h.aStart;
  const uint64_t a1 = h.aEnd;

  // Insertion hunk: treat as touching if the insertion point lies "on" a paste
  // span boundary.
  if (a0 == a1) {
    for (const auto &s : m.pasteSpans) {
      if (a0 >= s.begin && a0 <= s.end)
        return true;
    }
    return false;
  }

  // Replacement/deletion hunk: interval intersection between [a0, a1) and
  // [s.begin, s.end).
  for (const auto &s : m.pasteSpans) {
    if (a0 < s.end && a1 > s.begin)
      return true;
  }

  return false;
}

std::optional<RefoldEngine::PasteArgEdit>
RefoldEngine::DerivePasteArgEdit(const RefoldModel::MacroInvocation &m,
                                 const diffutils::Hunk &h) const {
  // This helper only applies when the producer reported paste spans.
  if (m.pasteSpans.empty())
    return std::nullopt;

  // Collect all paste span occurrences that intersect the hunk in A-token
  // space.
  //
  // Each PPArgSpan in pasteSpans corresponds to one argument's contribution to
  // a single pasted token emitted in A_PP. Multiple arguments may contribute
  // disjoint (or adjacent) byte segments inside the same pasted token, and the
  // hunk may touch one of those segments.
  std::vector<const RefoldModel::PPArgSpan *> cands;
  for (const auto &ps : m.pasteSpans) {
    if (ps.begin < h.aEnd && h.aStart < ps.end)
      cands.push_back(&ps);
  }

  if (cands.empty())
    return std::nullopt;

  // All candidates must refer to the same pasted-token occurrence. In practice,
  // each candidate has the same [begin,end) A-token envelope (the pasted
  // token), but different argIdx and [byteBegin,byteEnd) describing which byte
  // subrange of the pasted token came from that argument.
  const auto *tokenSpan = cands[0];

  // Map the pasted token envelope in A to its corresponding envelope in B. For
  // paste edits we require a strict mapping: the A pasted token must map to
  // exactly one B token that we will diff against.
  auto bEnvOpt = MapAToBTokenEnvelopeByPPArgSpan(*tokenSpan);
  if (!bEnvOpt || bEnvOpt->second <= bEnvOpt->first)
    return std::nullopt;

  // Paste-aware edits handled here must stay within a single B token. If the
  // pasted token turned into multiple tokens in B, the edit is not a pure
  // within-token paste segment rewrite, so bail out.
  if (bEnvOpt->second - bEnvOpt->first != 1)
    return std::nullopt;

  // Grab the raw token spellings for the pasted token in A and B.
  // tokenSpan.begin/end are A-token indices; bEnv[0]/bEnv[1] are B-token
  // indices.
  StringRef aTokRaw = SliceASource(tokenSpan->begin, tokenSpan->end);
  StringRef bTokRaw = SliceBSource(bEnvOpt->first, bEnvOpt->second);

  // Strip trailing newlines to stabilize within-token diffs.
  StringRef aTok = stringutils::stripTrailingNewlines(aTokRaw);
  StringRef bTok = stringutils::stripTrailingNewlines(bTokRaw);

  // Compute the minimal differing region between the two token spellings:
  // aTok = [common prefix][DIFF_A][common suffix]
  // bTok = [common prefix][DIFF_B][common suffix]
  size_t pref = 0;
  size_t minLen = std::min(aTok.size(), bTok.size());
  while (pref < minLen && aTok[pref] == bTok[pref]) {
    pref++;
  }

  size_t aLen = aTok.size();
  size_t bLen = bTok.size();
  size_t suff = 0;

  // While we haven't reached the prefix on either side
  // and the characters from the back match...
  while (suff < (aLen - pref) && suff < (bLen - pref) &&
         aTok[aLen - 1 - suff] == bTok[bLen - 1 - suff]) {
    suff++;
  }

  const size_t diffStart = pref;
  const size_t diffEndA = aLen - suff;

  // If there is no difference at all, this hunk cannot be explained as a
  // paste-segment rewrite.
  if (diffStart >= diffEndA && aTok.size() == bTok.size())
    return std::nullopt;

  // Now choose exactly one candidate argument contribution whose
  // [byteBegin,byteEnd) overlaps the differing region. The producer provided
  // byteBegin/byteEnd in pasted-token text coordinates.
  //
  // We require the edit to be attributable to a single argument slice. If
  // multiple slices overlap the diff region, we cannot express it as a
  // single-arg args-only rewrite.
  const RefoldModel::PPArgSpan *chosen = nullptr;
  for (const auto *ps : cands) {
    if (!ps->byteBegin || !ps->byteEnd || *ps->byteEnd < *ps->byteBegin)
      continue;

    const size_t bBegin = *ps->byteBegin;
    const size_t bEnd = *ps->byteEnd;

    bool hit = false;
    if (diffStart == diffEndA) {
      // Pure insertion/deletion at a point (no width in A). Treat as
      // overlapping if the point lies strictly inside the candidate slice.
      hit = (bBegin <= diffStart) && (diffStart < bEnd);
    } else {
      // General overlap between [diffStart,diffEndA) and
      // [ps.byteBegin,ps.byteEnd).
      const size_t lo = std::max(bBegin, diffStart);
      const size_t hi = std::min(bEnd, diffEndA);
      hit = (hi > lo);
    }

    if (hit) {
      if (!chosen)
        chosen = ps;
      else
        return std::nullopt; // Overlaps multiple args.
    }
  }

  if (!chosen)
    return std::nullopt;

  // Extract the old contributed segment from the A pasted token.
  const int64_t bbA = static_cast<int64_t>(*chosen->byteBegin);
  const int64_t beA = static_cast<int64_t>(*chosen->byteEnd);

  if (bbA < 0 || beA < bbA || static_cast<uint64_t>(beA) > aTok.size())
    return std::nullopt;

  // Compute the corresponding segment coordinates in the B pasted token.
  //
  // We assume the token-level edit does not permute the contribution
  // boundaries; instead, the chosen segment grows/shrinks by the overall token
  // length delta (bTokLen - aTokLen). This allows us to map [bb,be) in A to
  // [bb,be+delta) in B.
  const int64_t delta =
      static_cast<int64_t>(bTok.size()) - static_cast<int64_t>(aTok.size());
  const int64_t bbB_signed = bbA; // Assumption: prefix is stable
  const int64_t beB_signed = beA + delta;
  if (bbB_signed < 0 || beB_signed < bbB_signed ||
      static_cast<uint64_t>(beB_signed) > bTok.size())
    return std::nullopt;

  const size_t bb = static_cast<size_t>(bbA);
  const size_t be = static_cast<size_t>(beA);
  const size_t bbB = static_cast<size_t>(bbB_signed);
  const size_t beB = static_cast<size_t>(beB_signed);

  // Safety gate: ensure the only edits to the pasted token are within the
  // chosen segment.
  //
  // This requires both:
  // - the prefix before bb matches exactly
  // - the suffix after be matches exactly (after shifting by delta in B)
  if (aTok.substr(0, bb) != bTok.substr(0, bbB))
    return std::nullopt;

  if (aTok.substr(be) != bTok.substr(beB))
    return std::nullopt;

  std::string oldSeg = aTok.substr(bb, be - bb).str();
  std::string newSeg = bTok.substr(bbB, beB - bbB).str();

  return PasteArgEdit(chosen->argIdx, std::move(newSeg), std::move(oldSeg));
}

std::optional<std::vector<RefoldEngine::PasteArgEdit>>
RefoldEngine::DerivePasteArgEdits(const RefoldModel::MacroInvocation &m,
                                  const diffutils::Hunk &h) const {
  if (m.pasteSpans.empty())
    return std::nullopt;

  // Gather all paste spans that intersect this hunk in the A-stream.
  std::vector<const RefoldModel::PPArgSpan *> cands;
  for (const auto &ps : m.pasteSpans) {
    if (ps.begin < h.aEnd && h.aStart < ps.end)
      cands.push_back(&ps);
  }

  if (cands.empty())
    return std::nullopt;

  // All candidates should reference the same pasted token range [begin, end) in
  // A.
  const auto *tokenSpan = cands[0];

  auto bEnvOpt = MapAToBTokenEnvelopeByPPArgSpan(*tokenSpan);
  if (!bEnvOpt || bEnvOpt->second <= bEnvOpt->first)
    return std::nullopt;

  // Paste edits are only representable as args-only when the A-span maps to
  // exactly one B token.
  if (bEnvOpt->second - bEnvOpt->first != 1)
    return std::nullopt;

  StringRef aTokRaw = SliceASource(tokenSpan->begin, tokenSpan->end);
  StringRef bTokRaw = SliceBSource(bEnvOpt->first, bEnvOpt->second);

  // Strip trailing newlines to stabilize within-token diffs for the
  // legacy source-slice segmenter. The replay-witness path below uses lexer
  // token spellings instead, because `Slice*Source()` may include trailing
  // whitespace between adjacent PP tokens.
  StringRef aTok = stringutils::stripTrailingNewlines(aTokRaw);
  StringRef bTok = stringutils::stripTrailingNewlines(bTokRaw);

  // Prefer the producer's exact paste replay witness when it is present. The
  // witness gives the original ordered arg/literal decomposition of the pasted
  // token, so we can refold adjacent changed paste parts without inventing a
  // split from raw text alone. For delimiter-free adjacent arg runs, the only
  // accepted policy is shape preservation: the edited run must split by the
  // original part widths. All literal parts remain exact anchors.
  if (tokenSpan->end == tokenSpan->begin + 1 &&
      bEnvOpt->second == bEnvOpt->first + 1 &&
      tokenSpan->begin < aToks_.size() && bEnvOpt->first < bToks_.size()) {
    StringRef aTokSpelling = aToks_[tokenSpan->begin].spelling;
    StringRef bTokSpelling = bToks_[bEnvOpt->first].spelling;
    if (const auto *witness =
            findPasteTokenWitnessForSpan(m, *tokenSpan, aTokSpelling)) {
      auto replay = segmentPastedTokenByReplayWitness(bTokSpelling, *witness);
      if (replay.kind == PasteReplaySegmentationKind::Unique) {
        std::vector<PasteArgEdit> edits;
        size_t argPartIdx = 0;
        for (const auto &part : witness->parts) {
          if (part.kind != RefoldModel::PastePartKind::Arg)
            continue;
          if (!part.argIndex || argPartIdx >= replay.argSegments.size())
            return std::nullopt;
          StringRef oldSeg = part.spelling;
          const std::string &newSeg = replay.argSegments[argPartIdx++];
          if (oldSeg == newSeg)
            continue;
          edits.emplace_back(*part.argIndex, newSeg, oldSeg.str(),
                             part.argByteBegin, part.argByteEnd);
        }
        if (argPartIdx != replay.argSegments.size())
          return std::nullopt;
        if (!edits.empty()) {
          trace("macro/paste",
                "paste replay witness derived {0} arg edit(s): inv id={1} "
                "name={2} aTok='{3}' bTok='{4}'",
                static_cast<unsigned>(edits.size()), m.id, m.name,
                aTokSpelling, bTokSpelling);
          return edits;
        }
        return std::nullopt;
      }
      if (replay.kind == PasteReplaySegmentationKind::Ambiguous ||
          replay.kind == PasteReplaySegmentationKind::Unsupported)
        return std::nullopt;
    }
  }

  // Multi-span paste edits may change the overall pasted token length (e.g.,
  // a_b_c -> foo_bar_baz). This is still safe to refold *as long as* the
  // non-arg "fixed" slices of the pasted token remain unchanged, and we can
  // deterministically segment the B token into the per-arg regions.
  //
  // We derive the new per-arg segments by walking the A token left-to-right and
  // using the fixed (non-span) substrings between paste spans as anchors. If
  // spans are adjacent (no fixed anchor) and the total length changes,
  // segmentation is ambiguous and we conservatively return std::nullopt.
  std::vector<const RefoldModel::PPArgSpan *> spans = cands;
  std::sort(spans.begin(), spans.end(), [](const auto *p1, const auto *p2) {
    // If p1 has no value, it's "greater" than anything with a value (moves to
    // end)
    if (!p1->byteBegin)
      return false;
    if (!p2->byteBegin)
      return true;

    // If both have values, compare them
    if (*p1->byteBegin != *p2->byteBegin)
      return *p1->byteBegin < *p2->byteBegin;

    // Stable tie-breaker: sort by end position if starts are equal
    uint32_t end1 = p1->byteEnd.value_or(0);
    uint32_t end2 = p2->byteEnd.value_or(0);
    return end1 < end2;
  });

  std::optional<std::vector<std::string>> newSegs =
      SegmentPastedTokenArgsByFixedSlices(aTok, bTok, spans);
  if (!newSegs)
    return std::nullopt;

  std::vector<PasteArgEdit> edits;

  // Compare each projected argument segment that contributed to the pasted A
  // token against its derived replacement segment in B, and record only the
  // argument-local edits whose projected text actually changed.
  for (size_t i = 0; i < spans.size(); ++i) {
    const auto *ps = spans[i];
    if (!ps->byteBegin || *ps->byteEnd < *ps->byteBegin)
      return std::nullopt;

    size_t bb = static_cast<size_t>(*ps->byteBegin);
    size_t be = static_cast<size_t>(*ps->byteEnd);
    if (be > aTok.size())
      return std::nullopt;

    StringRef oldSeg = aTok.substr(bb, be - bb);
    const std::string &newSeg = (*newSegs)[i];

    if (oldSeg == newSeg)
      continue;

    // Note: the same argument may contribute multiple segments to the same
    // pasted token (e.g. X##_..._##X). We allow repeated argIdx here and let
    // the caller merge implied argument replacements conservatively.
    edits.emplace_back(ps->argIdx, newSeg, oldSeg.str());
  }

  if (edits.empty())
    return std::nullopt;

  return edits;
}

std::optional<std::vector<std::string>>
RefoldEngine::SegmentPastedTokenArgsByFixedSlices(
    StringRef aTok, StringRef bTok,
    ArrayRef<const RefoldModel::PPArgSpan *> spansAsc) {
  if (spansAsc.empty())
    return std::nullopt;

  // Basic span sanity
  for (const auto *ps : spansAsc) {
    if (!ps->byteBegin || *ps->byteEnd < *ps->byteBegin)
      return std::nullopt;
    if (static_cast<size_t>(*ps->byteEnd) > aTok.size())
      return std::nullopt;
  }

  using MemoKey = std::pair<size_t, size_t>;
  std::map<MemoKey, std::optional<std::vector<std::string>>> memo;

  auto solve = [&](auto &&self, size_t idx,
                   size_t posB) -> std::optional<std::vector<std::string>> {
    MemoKey key{idx, posB};
    auto it = memo.find(key);
    if (it != memo.end())
      return it->second;

    size_t posA = 0;
    if (idx > 0) {
      if (!spansAsc[idx - 1]->byteEnd) {
        memo.emplace(key, std::nullopt);
        return std::nullopt;
      }
      posA = static_cast<size_t>(*spansAsc[idx - 1]->byteEnd);
    }

    if (idx >= spansAsc.size()) {
      StringRef tail = aTok.substr(posA);
      std::optional<std::vector<std::string>> result =
          (bTok.substr(posB) == tail) ? std::optional<std::vector<std::string>>
                                          (std::vector<std::string>())
                                      : std::nullopt;
      memo.emplace(key, result);
      return result;
    }

    const auto *ps = spansAsc[idx];
    if (!ps->byteBegin || !ps->byteEnd || *ps->byteEnd < *ps->byteBegin) {
      memo.emplace(key, std::nullopt);
      return std::nullopt;
    }

    const size_t bA = static_cast<size_t>(*ps->byteBegin);
    const size_t eA = static_cast<size_t>(*ps->byteEnd);
    if (bA < posA) {
      memo.emplace(key, std::nullopt);
      return std::nullopt;
    }

    StringRef fixedBefore = aTok.substr(posA, bA - posA);
    if (!bTok.substr(posB).starts_with(fixedBefore)) {
      memo.emplace(key, std::nullopt);
      return std::nullopt;
    }

    const size_t runStartB = posB + fixedBefore.size();

    size_t runEnd = idx;
    size_t nextPosA = eA;
    StringRef fixedAfter;
    while (true) {
      if (runEnd + 1 >= spansAsc.size()) {
        fixedAfter = aTok.substr(nextPosA);
        break;
      }

      const auto *cur = spansAsc[runEnd];
      const auto *next = spansAsc[runEnd + 1];
      if (!cur->byteEnd || !next->byteBegin ||
          *next->byteBegin < *cur->byteEnd) {
        memo.emplace(key, std::nullopt);
        return std::nullopt;
      }

      StringRef gap =
          aTok.substr(static_cast<size_t>(*cur->byteEnd),
                      static_cast<size_t>(*next->byteBegin - *cur->byteEnd));
      if (!gap.empty()) {
        fixedAfter = gap;
        break;
      }

      ++runEnd;
      if (!spansAsc[runEnd]->byteEnd) {
        memo.emplace(key, std::nullopt);
        return std::nullopt;
      }
      nextPosA = static_cast<size_t>(*spansAsc[runEnd]->byteEnd);
    }

    auto tryRun =
        [&](size_t runEndB) -> std::optional<std::vector<std::string>> {
      if (runEndB < runStartB || runEndB > bTok.size())
        return std::nullopt;

      auto cert = buildAdjacentPasteRunInvertibilityCertificate(
          aTok, bTok.substr(runStartB, runEndB - runStartB),
          spansAsc.slice(idx, runEnd - idx + 1));
      if (cert.kind == PasteRunInvertibilityKind::Unique &&
          cert.derivedSegs.size() == runEnd - idx + 1) {
        if (auto suffix = self(self, runEnd + 1, runEndB)) {
          std::vector<std::string> combined = cert.derivedSegs;
          combined.insert(combined.end(), suffix->begin(), suffix->end());
          return combined;
        }
      }

      // Legacy single-span fallback: when the current run contains only one
      // argument contribution, the fixed slices on either side already pin the
      // segment boundary. The newer adjacent-run certificate is stricter, but
      // some pure-paste cases (e.g. CONCAT-style token assembly) are still
      // structurally invertible via this simpler anchor-based split.
      if (runEnd == idx) {
        if (auto suffix = self(self, idx + 1, runEndB)) {
          std::vector<std::string> combined;
          combined.reserve(1 + suffix->size());
          combined.push_back(bTok.substr(runStartB, runEndB - runStartB).str());
          combined.insert(combined.end(), suffix->begin(), suffix->end());
          return combined;
        }
      }

      return std::nullopt;
    };

    if (fixedAfter.empty()) {
      auto result = tryRun(bTok.size());
      memo.emplace(key, result);
      return result;
    }

    for (size_t k = bTok.find(fixedAfter, runStartB); k != StringRef::npos;
         k = bTok.find(fixedAfter, k + 1)) {
      if (auto result = tryRun(k)) {
        memo.emplace(key, result);
        return result;
      }
    }

    memo.emplace(key, std::nullopt);
    return std::nullopt;
  };

  return solve(solve, /*idx=*/0, /*posB=*/0);
}

StringRef RefoldEngine::DeriveNewPasteSegmentFromSpellingReplacement(
    StringRef baseArg, StringRef newArg, StringRef oldSeg) {
  // Trim all inputs.
  baseArg = baseArg.trim();
  newArg = newArg.trim();
  oldSeg = oldSeg.trim();

  // If the segment is the entire argument, the replacement is the entire new
  // argument.
  if (oldSeg == baseArg)
    return newArg;

  // Case 1: oldSeg is a prefix of baseArg.
  // Example: base="foo_v1", oldSeg="foo_", new="bar_v1" -> returns "bar_"
  if (baseArg.starts_with(oldSeg)) {
    StringRef suffix = baseArg.substr(oldSeg.size());
    if (!newArg.ends_with(suffix)) {
      // Special case: when the pasted segment is the macro-name token of a raw
      // invocation argument used under ##, edits to the *invocation arguments*
      // do not change the contributed pasted segment. For example:
      //   baseArg = XCAT(pre_,int)
      //   newArg  = XCAT(pre_,long)
      //   oldSeg  = XCAT
      // In that situation the spelled segment participating in ## remains the
      // raw callee token "XCAT". Preserve the old segment rather than trying
      // to derive it from the full invocation text.
      size_t oldLParen = baseArg.find('(');
      size_t newLParen = newArg.find('(');
      if (oldLParen != StringRef::npos && newLParen != StringRef::npos &&
          oldSeg == baseArg.substr(0, oldLParen) &&
          oldSeg == newArg.substr(0, newLParen)) {
        trace("macro/paste",
              " derive newSeg special-case(raw callee token): baseArg='{0}' "
              "newArg='{1}' oldSeg='{2}' -> '{3}'",
              baseArg, newArg, oldSeg, oldSeg);
        return oldSeg;
      }
      trace("macro/paste",
            " derive newSeg prefix-case FAILED: baseArg='{0}' newArg='{1}' "
            "oldSeg='{2}' suffix='{3}'",
            baseArg, newArg, oldSeg, suffix);
      return StringRef();
    }

    // Return the part of newArg that precedes the suffix.
    StringRef result = newArg.substr(0, newArg.size() - suffix.size());
    trace("macro/paste",
          " derive newSeg prefix-case: baseArg='{0}' newArg='{1}' oldSeg='{2}' "
          "suffix='{3}' -> '{4}'",
          baseArg, newArg, oldSeg, suffix, result);
    return result;
  }

  // Case 2: oldSeg is a suffix of baseArg.
  // Example: base="v1_foo", oldSeg="_foo", new="v1_bar" -> returns "_bar"
  if (baseArg.ends_with(oldSeg)) {
    StringRef prefix = baseArg.substr(0, baseArg.size() - oldSeg.size());
    if (!newArg.starts_with(prefix)) {
      trace("macro/paste",
            " derive newSeg suffix-case FAILED: baseArg='{0}' newArg='{1}' "
            "oldSeg='{2}' prefix='{3}'",
            baseArg, newArg, oldSeg, prefix);
      return StringRef();
    }

    // Return the part of newArg that follows the prefix.
    StringRef result = newArg.substr(prefix.size());
    trace("macro/paste",
          " derive newSeg suffix-case: baseArg='{0}' newArg='{1}' oldSeg='{2}' "
          "prefix='{3}' -> '{4}'",
          baseArg, newArg, oldSeg, prefix, result);
    return result;
  }

  return StringRef();
}

bool RefoldEngine::PasteArgReplacementsMatchAllPasteTokensInB(
    const RefoldModel::MacroInvocation &m, StringRef baseInvText,
    ArrayRef<std::pair<size_t, size_t>> invArgRanges,
    const DenseMap<uint32_t, std::string> &replByArgIdx) const {
  if (m.pasteSpans.empty())
    return true;

  // Prefer producer-provided argument byte ranges for the raw invocation text,
  // but fall back to syntactic parsing when those ranges are absent. This is
  // required for idempotent args-only refolds where the *current* base
  // invocation already reflects earlier hunks (e.g. "int" -> "float").
  // In that case we still need a stable "original arg spelling" to recognize
  // that a paste-segment edit is equivalent to a whole-argument replacement.
  std::optional<std::vector<std::pair<size_t, size_t>>> parsedOrigArgRanges;
  if (m.invText)
    parsedOrigArgRanges = ParseMacroInvocationArgContentRanges(*m.invText);

  auto getOrigArgTrim = [&](uint32_t argIdx) -> StringRef {
    if (!m.invText)
      return StringRef();

    StringRef invText = *m.invText;

    // Producer-provided ranges (if present).
    if (argIdx < m.invArgRanges.size()) {
      const RefoldModel::MacroInvocation::OptByteRange &r =
          m.invArgRanges[argIdx];
      if (r.first && r.second) {
        uint64_t b = *r.first;
        uint64_t e = *r.second;
        if (b <= e && e <= invText.size())
          return invText.slice(b, e).trim();
      }
    }

    // Syntactic fallback (independent of the producer).
    if (!parsedOrigArgRanges || argIdx >= parsedOrigArgRanges->size())
      return StringRef();
    size_t b = (*parsedOrigArgRanges)[argIdx].first;
    size_t e = (*parsedOrigArgRanges)[argIdx].second;
    if (b > e || e > invText.size())
      return StringRef();
    return invText.slice(b, e).trim();
  };

  // Precompute the original (base) spelling text for each argument we are
  // proposing to replace. We need this to derive a stable mapping from
  // "argument replacement" -> "paste segment update".
  DenseMap<uint32_t, std::string> baseArgByIdx;
  for (const auto &entry : replByArgIdx) {
    uint32_t argIdx = entry.first;
    if (static_cast<size_t>(argIdx) >= invArgRanges.size())
      return false;

    auto range = invArgRanges[argIdx];
    StringRef rawArg =
        baseInvText.substr(range.first, range.second - range.first);
    baseArgByIdx[argIdx] = rawArg.trim().str();
  }

  // Build a per-arg set of PP-byte envelopes for *standard* occurrences of the
  // current macro's formals. A paste span whose PP-byte envelope exactly
  // matches one of these ranges is not a direct paste contribution of the
  // current macro; it is a propagated nested-child paste inside a standard
  // occurrence of this formal and must be validated at the child level rather
  // than against the parent's raw arg replacement text.
  DenseMap<uint32_t, SmallVector<std::pair<uint64_t, uint64_t>, 4>>
      standardOccByteRangesByArg;
  for (const auto &occ : m.argSpans) {
    if (occ.kind != PPArgSpanKind::Standard || !occ.ppByteBegin ||
        !occ.ppByteEnd)
      continue;
    standardOccByteRangesByArg[occ.argIdx].push_back(
        {static_cast<uint64_t>(*occ.ppByteBegin),
         static_cast<uint64_t>(*occ.ppByteEnd)});
  }

  // Group paste spans by the specific pasted-token occurrence they contribute
  // to. The grouping key is the A token interval [beginTok,endTok) of the
  // pasted token. In practice, paste spans are expected to describe a single
  // token, so (endTok - beginTok) should be 1.
  std::vector<std::pair<uint64_t, uint64_t>> tokenOrder;
  DenseMap<std::pair<uint64_t, uint64_t>, std::vector<RefoldModel::PPArgSpan>>
      spansByTok;
  for (const auto &ps : m.pasteSpans) {
    std::pair<uint64_t, uint64_t> key = {ps.begin, ps.end};
    if (spansByTok.find(key) == spansByTok.end()) {
      tokenOrder.push_back(key);
    }
    spansByTok[key].push_back(ps);
  }


  auto formatNestedDelegationCandidates =
      [&](ArrayRef<uint32_t> missingArgIdxs, uint64_t tokBegin,
          uint64_t tokEnd) -> std::string {
    std::string out;
    raw_string_ostream os(out);
    os << "[";
    bool firstEntry = true;

    auto childIt = macroChildrenById_.find(m.id);
    if (childIt != macroChildrenById_.end()) {
      for (const auto *child : childIt->second) {
        if (!child)
          continue;

        SmallVector<std::string, 4> formalSummaries;
        for (uint32_t formalIdx = 0; formalIdx < child->argDeps.size();
             ++formalIdx) {
          bool mentionsMissingArg = false;
          for (uint32_t dep : child->argDeps[formalIdx]) {
            if (llvm::is_contained(missingArgIdxs, dep)) {
              mentionsMissingArg = true;
              break;
            }
          }
          if (!mentionsMissingArg)
            continue;

          formalSummaries.push_back(
              formatv("formal={0} deps={1}", formalIdx,
                      FormatUInt32List(child->argDeps[formalIdx]))
                  .str());
        }

        if (formalSummaries.empty())
          continue;

        if (!firstEntry)
          os << ", ";
        firstEntry = false;
        os << "{childId=" << child->id << " name=" << child->name
           << " coversTok=" << (child->Covers(tokBegin, tokEnd) ? 1 : 0)
           << " hasPaste=" << (child->pasteSpans.empty() ? 0 : 1)
           << " formals=[";
        for (size_t i = 0; i < formalSummaries.size(); ++i) {
          if (i)
            os << ", ";
          os << formalSummaries[i];
        }
        os << "] inv='"
           << (child->invText ? StringRef(*child->invText).trim()
                              : StringRef("<none>"))
           << "'}";
      }
    }

    os << "]";
    return os.str();
  };

  // For each pasted-token occurrence, simulate applying the per-arg
  // replacements to its sub-token argument segments and compare against the
  // edited B token spelling.
  for (const auto &key : tokenOrder) {
    uint64_t beginTok = key.first;
    uint64_t endTok = key.second;

    // We only support pasted-token occurrences that correspond to exactly one
    // token in A.
    if (endTok != beginTok + 1)
      return false;

    // Map the A pasted-token occurrence to a single B token envelope.
    auto bEnv = MapATokRangeAToBTokenEnvelope(beginTok, endTok);
    if (!bEnv || bEnv->second != bEnv->first + 1)
      return false;

    auto stripNL = [](StringRef s) -> std::string {
      std::string result = s.str(); // Copy StringRef to a mutable string
      llvm::erase_if(result, [](char c) { return c == '\n'; });
      return result;
    };

    // Extract the pasted token text as produced in A and B. We strip newlines
    // defensively since slice helpers may include trailing '\n' depending on
    // how token ranges were formed.
    std::string aTok = stripNL(SliceASource(beginTok, endTok));
    std::string bTok = stripNL(SliceBSource(bEnv->first, bEnv->second));

    auto formatReplacementMap = [&]() -> std::string {
      std::string out;
      raw_string_ostream os(out);
      os << "{";
      bool first = true;
      for (const auto &KV : replByArgIdx) {
        if (!first)
          os << ", ";
        first = false;
        os << KV.first << ":'" << KV.second << "'";
      }
      os << "}";
      return os.str();
    };

    auto formatBaseArgMap = [&]() -> std::string {
      std::string out;
      raw_string_ostream os(out);
      os << "{";
      bool first = true;
      for (const auto &KV : baseArgByIdx) {
        if (!first)
          os << ", ";
        first = false;
        os << KV.first << ":'" << KV.second << "'";
      }
      os << "}";
      return os.str();
    };

    trace("macro/paste",
          " validate pasted token: inv id={0} name={1} tokRange=[{2},{3}) "
          "aTok='{4}' bTok='{5}' repls={6} baseArgs={7}",
          m.id, m.name, beginTok, endTok, aTok, bTok,
          formatReplacementMap(), formatBaseArgMap());

    // Paste spans for this token reference character slices inside the pasted
    // token spelling. Apply edits in descending byteBegin so earlier rewrites
    // do not shift later offsets.
    std::vector<RefoldModel::PPArgSpan> &spans = spansByTok[key];

    SmallVector<uint32_t, 8> carriedArgIdxs =
        CollectSortedUInt32Keys(replByArgIdx);
    SmallVector<uint32_t, 8> directPasteArgIdxs =
        CollectSortedUniquePasteArgIdxs(spans);
    SmallVector<uint32_t, 8> carriedButDirectMissingArgIdxs =
        ComputeSortedMissingUInt32s(carriedArgIdxs, directPasteArgIdxs);
    trace("macro/paste",
          " paste support ledger: inv id={0} name={1} tokRange=[{2},{3}) "
          "carriedArgs={4} directPasteArgs={5} carriedButDirectMissing={6} "
          "nestedDelegationCandidates={7}",
          m.id, m.name, beginTok, endTok, FormatUInt32List(carriedArgIdxs),
          FormatUInt32List(directPasteArgIdxs),
          FormatUInt32List(carriedButDirectMissingArgIdxs),
          formatNestedDelegationCandidates(carriedButDirectMissingArgIdxs,
                                           beginTok, endTok));
    for (const auto &ps : spans) {
      trace("macro/paste",
            "  token span census: inv id={0} name={1} tokRange=[{2},{3}) "
            "argIdx={4} byteRange=[{5},{6}) ppBytes=[{7},{8}) kind={9} "
            "hasReplacement={10}",
            m.id, m.name, ps.begin, ps.end, ps.argIdx,
            ps.byteBegin ? static_cast<uint32_t>(*ps.byteBegin) : 0U,
            ps.byteEnd ? static_cast<uint32_t>(*ps.byteEnd) : 0U,
            ps.ppByteBegin ? static_cast<uint64_t>(*ps.ppByteBegin) : 0ULL,
            ps.ppByteEnd ? static_cast<uint64_t>(*ps.ppByteEnd) : 0ULL,
            static_cast<unsigned>(ps.kind),
            replByArgIdx.contains(ps.argIdx) ? 1 : 0);
    }

    // Apply in descending byteBegin so replacements cannot shift the offsets
    // of later spans.
    std::sort(spans.begin(), spans.end(), [](const auto &p1, const auto &p2) {
      // Spans without byteBegin are treated as "last".
      if (!p1.byteBegin)
        return false;
      if (!p2.byteBegin)
        return true;

      // Primary key: descending start.
      if (*p1.byteBegin != *p2.byteBegin)
        return *p1.byteBegin > *p2.byteBegin;

      // Tie-breaker: descending end.
      uint32_t end1 = p1.byteEnd.value_or(0);
      uint32_t end2 = p2.byteEnd.value_or(0);
      return end1 > end2;
    });

    // Start from the A token spelling and simulate the token-paste result after
    // applying the candidate arg replacements.
    std::string expected = aTok;
    bool sawCurrentLevelDirectSpan = false;
    for (const auto &ps : spans) {
      // Only apply span updates for arguments that we are actively replacing.
      auto it = replByArgIdx.find(ps.argIdx);
      if (it == replByArgIdx.end())
        continue;

      // If this paste span's PP-byte envelope exactly matches a *standard*
      // occurrence of the same formal in the current macro expansion, this
      // span is a propagated nested-child paste contributor rather than a
      // direct paste contribution of the current macro itself. Validate those
      // at the child level, not against the parent's raw arg replacement.
      if (ps.ppByteBegin && ps.ppByteEnd) {
        auto stdIt = standardOccByteRangesByArg.find(ps.argIdx);
        if (stdIt != standardOccByteRangesByArg.end()) {
          std::pair<uint64_t, uint64_t> spanBytes = {
              static_cast<uint64_t>(*ps.ppByteBegin),
              static_cast<uint64_t>(*ps.ppByteEnd)};
          bool matchesStandardOcc = false;
          for (const auto &r : stdIt->second) {
            if (r == spanBytes) {
              matchesStandardOcc = true;
              break;
            }
          }
          if (matchesStandardOcc) {
            trace(
                "macro/paste",
                " skip propagated child paste span at parent level: inv id={0} "
                "name={1} argIdx={2} tokRange=[{3},{4}) ppBytes=[{5},{6})",
                m.id, m.name, ps.argIdx, ps.begin, ps.end,
                static_cast<uint64_t>(*ps.ppByteBegin),
                static_cast<uint64_t>(*ps.ppByteEnd));
            continue;
          }
          sawCurrentLevelDirectSpan = true;
          trace("macro/paste",
                " parent-level paste span NOT skipped: inv id={0} name={1} "
                "argIdx={2} tokRange=[{3},{4}) ppBytes=[{5},{6}) stdOccCount={7}",
                m.id, m.name, ps.argIdx, ps.begin, ps.end,
                static_cast<uint64_t>(*ps.ppByteBegin),
                static_cast<uint64_t>(*ps.ppByteEnd),
                static_cast<unsigned>(stdIt->second.size()));
          for (const auto &r : stdIt->second) {
            trace("macro/paste",
                  "  standard occurrence bytes for argIdx={0}: [{1},{2})",
                  ps.argIdx, r.first, r.second);
          }
        } else {
          sawCurrentLevelDirectSpan = true;
          trace("macro/paste",
                " parent-level paste span has no standard occurrence bytes: "
                "inv id={0} name={1} argIdx={2} tokRange=[{3},{4}) "
                "ppBytes=[{5},{6})",
                m.id, m.name, ps.argIdx, ps.begin, ps.end,
                static_cast<uint64_t>(*ps.ppByteBegin),
                static_cast<uint64_t>(*ps.ppByteEnd));
        }
      } else {
        sawCurrentLevelDirectSpan = true;
        trace("macro/paste",
              " parent-level paste span missing pp-byte envelope: inv id={0} "
              "name={1} argIdx={2} tokRange=[{3},{4})",
              m.id, m.name, ps.argIdx, ps.begin, ps.end);
      }

      StringRef newArg = it->second;

      // The segment derivation also needs the original spelling of the
      // argument.
      auto baseIt = baseArgByIdx.find(ps.argIdx);
      if (baseIt == baseArgByIdx.end())
        return false;
      StringRef baseArg = baseIt->second;

      if (!ps.byteBegin || !ps.byteEnd)
        return false;

      size_t b = static_cast<size_t>(*ps.byteBegin);
      size_t e = static_cast<size_t>(*ps.byteEnd);

      // The paste span must define a valid character slice inside the A
      // pasted-token spelling.
      if (e < b || e > aTok.size())
        return false;

      // Extract the original pasted-token segment contributed by this argument.
      StringRef oldSeg = StringRef(aTok).substr(b, e - b);

      // Derive the new pasted-token segment from the argument replacement. This
      // is intentionally conservative and must be deterministic; if we cannot
      // derive a segment safely, fail.
      StringRef newSeg =
          DeriveNewPasteSegmentFromSpellingReplacement(baseArg, newArg, oldSeg);
      if (newSeg.data() == nullptr) { // Check for "null" StringRef
        // Idempotence: later hunks may be checking a pasted-token that is
        // already consistent with an earlier spelling edit. In this common
        // case, the argument text in the current invocation equals the new
        // argument text, and the original arg text equals the old pasted
        // segment. When that holds, we can treat the paste segment as the
        // entire argument.
        uint32_t argIdx = ps.argIdx;
        StringRef origTrim = getOrigArgTrim(argIdx);
        StringRef oldTrim = oldSeg.trim();
        StringRef newTrim = newArg.trim();
        StringRef baseTrim = baseArg.trim();
        if (!origTrim.empty() && origTrim == oldTrim && baseTrim == newTrim) {
          newSeg = newTrim;
          trace("macro/paste",
                " paste newSeg fallback (whole-arg) argIdx={0} oldSeg='{1}' "
                "newSeg='{2}'",
                argIdx, oldTrim, newTrim);
        } else {
          trace("macro/paste",
                " paste newSeg derivation FAILED argIdx={0} oldSeg='{1}' "
                "baseArg='{2}' newArg='{3}' ppTokRange=[{4},{5}) "
                "spanBytes=[{6},{7})",
                argIdx, oldTrim, baseTrim, newTrim, ps.begin, ps.end,
                ps.ppByteBegin ? static_cast<uint64_t>(*ps.ppByteBegin) : 0ULL,
                ps.ppByteEnd ? static_cast<uint64_t>(*ps.ppByteEnd) : 0ULL);
          return false;
        }
      }

      // Rewrite only the identified segment region inside the synthetic pasted-
      // token spelling.
      expected = stringutils::replaceRange(expected, b, e, newSeg);
    }

    // If every span for this token was filtered out as a propagated child
    // contribution, then this token has no direct current-macro paste work to
    // validate at this level. The child invocation is responsible for it.
    if (!sawCurrentLevelDirectSpan) {
      trace("macro/paste",
            " pasted token fully delegated to child level: inv id={0} name={1} "
            "tokRange=[{2},{3}) expected='{4}' actual='{5}'",
            m.id, m.name, beginTok, endTok, expected, bTok);
      continue;
    }

    if (expected != bTok) {
      trace("macro/paste",
            " pasted-token mismatch: inv id={0} name={1} tokRange=[{2},{3}) "
            "expected='{4}' actual='{5}' rewrittenArgCount={6}",
            m.id, m.name, beginTok, endTok, expected, bTok,
            static_cast<unsigned>(replByArgIdx.size()));
      for (const auto &ps : spans) {
        trace("macro/paste",
              "  mismatch span detail: argIdx={0} tokRange=[{1},{2}) "
              "byteRange=[{3},{4}) ppBytes=[{5},{6}) kind={7}",
              ps.argIdx, ps.begin, ps.end,
              ps.byteBegin ? static_cast<uint32_t>(*ps.byteBegin) : 0U,
              ps.byteEnd ? static_cast<uint32_t>(*ps.byteEnd) : 0U,
              ps.ppByteBegin ? static_cast<uint64_t>(*ps.ppByteBegin) : 0ULL,
              ps.ppByteEnd ? static_cast<uint64_t>(*ps.ppByteEnd) : 0ULL,
              static_cast<unsigned>(ps.kind));
      }
      return false;
    }

    trace("macro/paste",
          " pasted-token match: inv id={0} name={1} tokRange=[{2},{3}) "
          "expected='{4}' actual='{5}'",
          m.id, m.name, beginTok, endTok, expected, bTok);
  }

  return true;
}

std::string RefoldEngine::SplicePasteSegmentIntoSpellingArg(StringRef baseArg,
                                                            StringRef oldSeg,
                                                            StringRef newSeg) {
  StringRef baseTrim = baseArg.trim();
  StringRef oldTrim = oldSeg.trim();
  StringRef newTrim = newSeg.trim();

  // Idempotence: later hunks may refer to the same token-paste occurrence
  // after we've already applied a previous arg edit (e.g., a standard arg
  // occurrence). In that case, 'baseTrim' already equals the target segment
  // and we should treat this splice as a no-op rather than a failure.
  if (baseTrim == newTrim) {
    trace("macro/paste",
          " splice(no-op): baseArg='{0}' oldSeg='{1}' newSeg='{2}'", baseTrim,
          oldTrim, newTrim);
    return baseTrim.str();
  }

  if (oldTrim.empty())
    return ""; // Return empty to signal failure/null

  // Only allow unambiguous boundary splices: whole arg, prefix, or suffix.
  if (baseTrim == oldTrim)
    return newTrim.str();

  bool starts = baseTrim.starts_with(oldTrim);
  bool ends = baseTrim.ends_with(oldTrim);

  // Ambiguous case: if it matches both as prefix and suffix, we can't safely
  // determine which occurrence to replace.
  if (starts && ends)
    return "";

  if (starts) {
    // Return new replacement + remaining suffix of the original arg.
    return (newTrim.str() + baseTrim.substr(oldTrim.size()).str());
  }

  if (ends) {
    // Return original prefix + new replacement.
    size_t prefixLen = baseTrim.size() - oldTrim.size();
    return (baseTrim.substr(0, prefixLen).str() + newTrim.str());
  }

  return "";
}

std::string RefoldEngine::SplicePasteSegmentIntoSpellingArgExact(
    StringRef baseArg, uint32_t argByteBegin, uint32_t argByteEnd,
    StringRef oldSeg, StringRef newSeg) {
  if (argByteEnd < argByteBegin || argByteEnd > baseArg.size())
    return "";

  // The producer-provided range is authoritative only if it still names the
  // exact old segment inside the current invocation argument spelling. If it no
  // longer matches, falling back to prefix/suffix inference would reintroduce
  // the ambiguity this metadata is meant to avoid.
  if (baseArg.slice(argByteBegin, argByteEnd) != oldSeg)
    return "";

  return stringutils::replaceRange(baseArg, argByteBegin, argByteEnd, newSeg);
}

std::optional<std::vector<std::pair<size_t, size_t>>>
RefoldEngine::GetMacroInvocationFormalArgContentRanges(
    const RefoldModel::MacroInvocation &m, StringRef invText) {
  auto emptyAtCloseParenIn = [&](StringRef text) -> std::pair<size_t, size_t> {
    size_t closeIdx = text.rfind(')');
    if (closeIdx == StringRef::npos)
      closeIdx = text.size();
    return {closeIdx, closeIdx};
  };

  auto isVariadicFormal = [&](size_t idx) -> bool {
    return idx < m.defParams.size() && m.defParams[idx].variadic;
  };

  auto trailingFormalsAreVariadic = [&](size_t beginIdx) -> bool {
    for (size_t i = beginIdx; i < m.defParams.size(); ++i) {
      if (!isVariadicFormal(i))
        return false;
    }
    return true;
  };

  auto mapParsedActualsToFormalRanges =
      [&](StringRef text,
          const std::vector<std::pair<size_t, size_t>> &parsed)
      -> std::optional<std::vector<std::pair<size_t, size_t>>> {
    const size_t formalN = m.defParams.size();
    const size_t actualN = parsed.size();

    if (formalN == 0) {
      if (actualN == 0)
        return std::vector<std::pair<size_t, size_t>>();
      return std::nullopt;
    }

    if (actualN == formalN)
      return parsed;

    std::vector<std::pair<size_t, size_t>> out;
    out.reserve(formalN);

    if (actualN > formalN) {
      if (!isVariadicFormal(formalN - 1))
        return std::nullopt;
      out.insert(out.end(), parsed.begin(), parsed.begin() + (formalN - 1));
      out.push_back({parsed[formalN - 1].first, parsed.back().second});
      return out;
    }

    if (!trailingFormalsAreVariadic(actualN))
      return std::nullopt;

    out.insert(out.end(), parsed.begin(), parsed.end());
    for (size_t i = actualN; i < formalN; ++i)
      out.push_back(emptyAtCloseParenIn(text));
    return out;
  };

  auto parseAndMapFormalRanges = [&](StringRef text)
      -> std::optional<std::vector<std::pair<size_t, size_t>>> {
    auto parsedOpt = RefoldEngine::ParseMacroInvocationArgContentRanges(text);
    if (!parsedOpt)
      return std::nullopt;
    return mapParsedActualsToFormalRanges(text, *parsedOpt);
  };

  auto tryProducerRelativeRanges = [&]()
      -> std::optional<std::vector<std::pair<size_t, size_t>>> {
    if (m.invArgRanges.empty() || !m.invB)
      return std::nullopt;

    const uint64_t invB = *m.invB;
    std::vector<std::pair<size_t, size_t>> out;
    out.reserve(m.invArgRanges.size());

    for (const auto &R : m.invArgRanges) {
      if (!R.first || !R.second)
        return std::nullopt;
      if (*R.first < invB || *R.second < *R.first)
        return std::nullopt;

      const uint64_t relB64 = *R.first - invB;
      const uint64_t relE64 = *R.second - invB;
      if (relE64 > invText.size() || relB64 > relE64)
        return std::nullopt;

      out.emplace_back(static_cast<size_t>(relB64),
                       static_cast<size_t>(relE64));
    }

    return out;
  };

  auto transportArgsOverProducerSlotsExactly =
      [&](StringRef producerText,
          const std::vector<std::pair<size_t, size_t>> &producerRanges,
          StringRef currentText,
          const std::vector<std::pair<size_t, size_t>> &currentRanges) -> bool {
    if (producerRanges.size() != currentRanges.size())
      return false;

    std::string rebuilt;
    rebuilt.reserve(currentText.size());
    size_t cur = 0;

    for (size_t i = 0; i < producerRanges.size(); ++i) {
      size_t pb = producerRanges[i].first;
      size_t pe = producerRanges[i].second;
      size_t cb = currentRanges[i].first;
      size_t ce = currentRanges[i].second;

      if (pb > pe || pe > producerText.size() || pb < cur)
        return false;
      if (cb > ce || ce > currentText.size())
        return false;

      rebuilt.append(producerText.substr(cur, pb - cur));
      rebuilt.append(currentText.substr(cb, ce - cb));
      cur = pe;
    }

    rebuilt.append(producerText.substr(cur));
    return rebuilt == currentText;
  };

  if (m.invText) {
    std::optional<std::vector<std::pair<size_t, size_t>>> producerRangesOpt =
        tryProducerRelativeRanges();
    if (!producerRangesOpt)
      producerRangesOpt = parseAndMapFormalRanges(*m.invText);

    if (invText == *m.invText)
      return producerRangesOpt;

    auto currentRangesOpt = parseAndMapFormalRanges(invText);
    if (!producerRangesOpt || !currentRangesOpt)
      return std::nullopt;

    if (!transportArgsOverProducerSlotsExactly(*m.invText, *producerRangesOpt,
                                               invText, *currentRangesOpt)) {
      return std::nullopt;
    }

    return currentRangesOpt;
  }

  return parseAndMapFormalRanges(invText);
}

std::optional<RefoldEngine::MacroPatch>
RefoldEngine::BuildMacroInvocationPatchArgsOnly(
    const RefoldModel::MacroInvocation &m, const diffutils::Hunk &h,
    StringRef baseInvText) const {
  // We can only emit an invocation patch if the producer provided a concrete
  // byte range.
  if (!m.invB || !m.invE)
    return std::nullopt;

  // tokenHunks are used by macroArgReplacementMatchesAllOccurrencesInB() to
  // validate cross-occurrence consistency. Start with the current hunk only;
  // the standard repeated-formal path may later widen this to include every
  // hunk that touches the same formal occurrences inside this invocation.
  diffutils::Hunk hArgs = h;

  // Keep the raw hunk unchanged. Pure-insertion ownership is derived later
  // from exact occurrence boundaries and exact mapped B-envelope adjacency.
  const diffutils::Hunk tokenHunksCurrent[] = {hArgs};

  if (!HasLiteralMacroCalleeOrigin(m)) {
    trace("macro/args",
          "args-only disabled: non-literal callee origin kind={0} inv id={1} "
          "name={2}",
          ::clang::refold::toString(m.calleeOrigin.kind), m.id, m.name);
    return std::nullopt;
  }

  trace("macro/args", "args-only? inv id={0} name={1} {2} baseInv={3}", m.id,
        m.name, h, stringutils::showWSWithClip(baseInvText, 200));

  // Parse the byte ranges for each argument's "content" within the invocation
  // spelling. These ranges are later used to splice per-arg replacements back
  // into the invocation text.
  auto rangesOpt = GetMacroInvocationFormalArgContentRanges(m, baseInvText);
  if (!rangesOpt)
    return std::nullopt;
  const auto &invArgRanges = *rangesOpt;

  auto isVariadicFormal = [&](uint32_t idx) -> bool {
    return idx < m.defParams.size() && m.defParams[idx].variadic;
  };

  // Detect a top-level comma in an argument replacement by lexing the
  // replacement text with Clang's raw lexer and tracking only delimiter depth.
  auto hasTopLevelComma = [&](StringRef s) -> bool {
    const SourceLocation baseLoc = SourceLocation::getFromRawEncoding(1);
    std::string lexBuf = s.str();
    lexBuf.push_back('\0');
    const char *bufStart = lexBuf.data();
    const char *bufEnd = bufStart + s.size();
    Lexer lex(baseLoc, lexLang_, bufStart, bufStart, bufEnd);

    int parenDepth = 0;
    int bracketDepth = 0;
    int braceDepth = 0;
    Token tok;

    while (true) {
      lex.LexFromRawLexer(tok);
      if (tok.is(tok::eof))
        return false;
      if (tok.is(tok::comment))
        continue;

      switch (tok.getKind()) {
      case tok::l_paren:
        ++parenDepth;
        break;
      case tok::r_paren:
        if (parenDepth > 0)
          --parenDepth;
        break;
      case tok::l_square:
        ++bracketDepth;
        break;
      case tok::r_square:
        if (bracketDepth > 0)
          --bracketDepth;
        break;
      case tok::l_brace:
        ++braceDepth;
        break;
      case tok::r_brace:
        if (braceDepth > 0)
          --braceDepth;
        break;
      case tok::comma:
        if (parenDepth == 0 && bracketDepth == 0 && braceDepth == 0)
          return true;
        break;
      default:
        break;
      }
    }
  };

  trace("macro/args", "  invArgRanges({0})={1}", invArgRanges.size(),
        stringutils::rangesToStringWithSlices(baseInvText, invArgRanges));

  auto tryTemplateSolvedArgsOnlyPatch = [&]() -> std::optional<MacroPatch> {
    // Treat the whole macro expansion as a deterministic template made of
    // fixed body tokens and argument-occurrence variables.  This proves split
    // edits that the LCS exposes as separate islands around macro-body
    // punctuation, e.g. repeated formals and tuple forwarding wrappers.
    if (!m.stringifySpans.empty() || !m.pasteSpans.empty())
      return std::nullopt;

    auto cover = GetWholeCoverATokRange(m);
    if (!cover)
      return std::nullopt;

    struct TemplateElem {
      bool isArg = false;
      uint64_t aBegin = 0;
      uint64_t aEnd = 0;
      uint32_t argIdx = 0;
      size_t occurrenceOrdinal = 0;
    };

    // Build a linear template over the macro's A-side whole cover. Body spans
    // are fixed terminals; standard argument spans are variables whose B-side
    // slices must be solved consistently across all occurrences.
    SmallVector<TemplateElem, 32> elems;
    for (const auto &bs : m.bodySpans) {
      if (bs.begin < bs.end)
        elems.push_back({false, bs.begin, bs.end, 0, 0});
    }

    size_t occurrenceCount = 0;
    for (const auto &as : m.argSpans) {
      if (as.kind != PPArgSpanKind::Standard || as.begin >= as.end)
        continue;
      if (static_cast<size_t>(as.argIdx) >= invArgRanges.size())
        return std::nullopt;
      elems.push_back({true, as.begin, as.end, as.argIdx, occurrenceCount++});
    }

    if (occurrenceCount == 0 || elems.empty())
      return std::nullopt;

    bool needsCrossOccurrenceProof = false;
    DenseMap<uint32_t, unsigned> argOccurrenceCounts;
    for (const auto &elem : elems) {
      if (!elem.isArg)
        continue;
      ++argOccurrenceCounts[elem.argIdx];
      auto r = invArgRanges[elem.argIdx];
      StringRef baseArg =
          baseInvText.substr(r.first, r.second - r.first).trim();
      StringRef occText = SliceASource(elem.aBegin, elem.aEnd).trim();
      if (occText != baseArg)
        needsCrossOccurrenceProof = true;
    }
    for (const auto &entry : argOccurrenceCounts) {
      if (entry.second > 1)
        needsCrossOccurrenceProof = true;
    }
    if (!needsCrossOccurrenceProof)
      return std::nullopt;

    llvm::sort(elems, [](const TemplateElem &a, const TemplateElem &b) {
      if (a.aBegin != b.aBegin)
        return a.aBegin < b.aBegin;
      if (a.aEnd != b.aEnd)
        return a.aEnd < b.aEnd;
      return a.isArg < b.isArg;
    });

    // Reject unless body/argument spans form an exact partition of the macro
    // cover. Any gap would be unmodelled fixed syntax, so the template would not
    // explain the whole expansion surface.
    uint64_t cursor = cover->first;
    for (const auto &elem : elems) {
      if (elem.aBegin != cursor)
        return std::nullopt;
      cursor = elem.aEnd;
    }
    if (cursor != cover->second)
      return std::nullopt;

    auto bEnv = MapATokRangeAToBTokenEnvelopePreserveBoundaryInsertions(
        cover->first, cover->second);
    if (!bEnv || bEnv->first >= bEnv->second)
      return std::nullopt;

    // Keep this proof path bounded and deterministic.  Large covers stay on
    // the existing conservative paths rather than using an expensive solver.
    if (elems.size() > 64 || (bEnv->second - bEnv->first) > 256 ||
        occurrenceCount > 32)
      return std::nullopt;

    auto bodyMatchesAt = [&](const TemplateElem &elem, size_t bPos) -> bool {
      const size_t len = static_cast<size_t>(elem.aEnd - elem.aBegin);
      if (bPos + len > bEnv->second)
        return false;
      for (size_t i = 0; i < len; ++i) {
        if (aToks_[static_cast<size_t>(elem.aBegin) + i].spelling !=
            bToks_[bPos + i].spelling)
          return false;
      }
      return true;
    };

    const std::pair<size_t, size_t> unset = {
        std::numeric_limits<size_t>::max(),
        std::numeric_limits<size_t>::max()};
    std::vector<std::pair<size_t, size_t>> curAssign(occurrenceCount, unset);
    std::vector<std::vector<std::pair<size_t, size_t>>> solutions;

    // Enumerate all bounded assignments of B-token intervals to argument
    // occurrences while requiring fixed body spans to match literally. The cap
    // keeps this a small proof search rather than an unbounded parser.
    std::function<void(size_t, size_t)> dfs = [&](size_t elemIdx,
                                                  size_t bPos) {
      if (solutions.size() > 16)
        return;
      if (elemIdx == elems.size()) {
        if (bPos == bEnv->second)
          solutions.push_back(curAssign);
        return;
      }

      const TemplateElem &elem = elems[elemIdx];
      if (!elem.isArg) {
        const size_t len = static_cast<size_t>(elem.aEnd - elem.aBegin);
        if (bodyMatchesAt(elem, bPos))
          dfs(elemIdx + 1, bPos + len);
        return;
      }

      for (size_t end = bPos + 1; end <= bEnv->second; ++end) {
        curAssign[elem.occurrenceOrdinal] = {bPos, end};
        dfs(elemIdx + 1, end);
        curAssign[elem.occurrenceOrdinal] = unset;
        if (solutions.size() > 16)
          return;
      }
    };

    dfs(0, bEnv->first);
    if (solutions.empty() || solutions.size() > 16)
      return std::nullopt;

    auto findDirectTupleRefsForArg =
        [&](uint32_t callerArgIdx,
            SmallVectorImpl<RefoldModel::TupleArgRef> &outRefs) -> bool {
      outRefs.clear();
      StringRef parentTrim =
          baseInvText.substr(invArgRanges[callerArgIdx].first,
                             invArgRanges[callerArgIdx].second -
                                 invArgRanges[callerArgIdx].first)
              .trim();
      bool matched = false;
      SmallVector<RefoldModel::TupleArgRef, 8> matchedRefs;

      for (const auto &cand : model_.GetMacroInvocations()) {
        if (!cand.callerMacroId || *cand.callerMacroId != m.id)
          continue;
        if (!cand.normalizedInvText || cand.normalizedInvArgTextRanges.empty() ||
            cand.argTupleRefs.empty() ||
            cand.normalizedInvArgTextRanges.size() != cand.argTupleRefs.size())
          continue;

        SmallVector<RefoldModel::TupleArgRef, 8> localRefs;
        bool any = false;
        for (uint32_t childArgIdx = 0; childArgIdx < cand.argTupleRefs.size();
             ++childArgIdx) {
          const auto &refs = cand.argTupleRefs[childArgIdx];
          if (refs.size() != 1)
            continue;
          const auto &ref = refs.front();
          if (ref.callerParamIndex != callerArgIdx)
            continue;
          if (ref.callerByteEnd < ref.callerByteBegin ||
              ref.callerByteEnd > parentTrim.size())
            return false;
          const auto &rng = cand.normalizedInvArgTextRanges[childArgIdx];
          if (!rng.first || !rng.second || *rng.second < *rng.first ||
              *rng.second > cand.normalizedInvText->size())
            return false;
          StringRef childText = StringRef(*cand.normalizedInvText)
                                    .slice((size_t)*rng.first,
                                           (size_t)*rng.second)
                                    .trim();
          StringRef parentSlice =
              parentTrim.slice(ref.callerByteBegin, ref.callerByteEnd).trim();
          if (childText != parentSlice)
            return false;
          localRefs.push_back(ref);
          any = true;
        }
        if (!any)
          continue;
        if (matched)
          return false;
        matched = true;
        matchedRefs = std::move(localRefs);
      }

      if (!matched)
        return false;
      llvm::sort(matchedRefs, [](const RefoldModel::TupleArgRef &a,
                                 const RefoldModel::TupleArgRef &b) {
        return a.callerByteBegin < b.callerByteBegin;
      });
      outRefs.append(matchedRefs.begin(), matchedRefs.end());
      return true;
    };

    auto buildCandidateInvocation =
        [&](ArrayRef<std::pair<size_t, size_t>> sol,
            std::string &outInv) -> bool {
      // Convert one solved expansion template back into call-site argument text.
      // Whole-argument occurrences must agree exactly; tuple/variadic cases may
      // rewrite individual top-level elements only when metadata identifies the
      // corresponding caller slices.
      DenseMap<uint32_t, SmallVector<size_t, 8>> occByArg;
      for (const auto &elem : elems) {
        if (elem.isArg)
          occByArg[elem.argIdx].push_back(elem.occurrenceOrdinal);
      }

      DenseMap<uint32_t, std::string> replByArg;
      bool changed = false;
      for (const auto &entry : occByArg) {
        const uint32_t argIdx = entry.first;
        auto argRange = invArgRanges[argIdx];
        StringRef baseArgText =
            baseInvText.substr(argRange.first, argRange.second - argRange.first);
        StringRef baseTrim = baseArgText.trim();

        SmallVector<std::string, 8> oldOccs;
        SmallVector<std::string, 8> newOccs;
        for (size_t occOrdinal : entry.second) {
          const TemplateElem *occElem = nullptr;
          for (const auto &elem : elems) {
            if (elem.isArg && elem.occurrenceOrdinal == occOrdinal) {
              occElem = &elem;
              break;
            }
          }
          if (!occElem)
            return false;
          oldOccs.push_back(SliceASource(occElem->aBegin, occElem->aEnd)
                                .trim()
                                .str());
          const auto &range = sol[occOrdinal];
          newOccs.push_back(SliceBSource(range.first, range.second)
                                .trim()
                                .str());
          if (newOccs.back().empty())
            return false;
        }

        bool allOldAreWholeArg = true;
        bool allNewSame = !newOccs.empty();
        for (size_t i = 0; i < oldOccs.size(); ++i) {
          if (StringRef(oldOccs[i]).trim() != baseTrim)
            allOldAreWholeArg = false;
          if (StringRef(newOccs[i]).trim() != StringRef(newOccs[0]).trim())
            allNewSame = false;
        }

        std::string replacement;
        if (allOldAreWholeArg && allNewSame) {
          replacement = StringRef(newOccs[0]).trim().str();
        } else if (isVariadicFormal(argIdx)) {
          SmallVector<TupleElementSlice, 8> tupleElems;
          if (!splitTopLevelTupleElementsWithLexer(baseTrim, lexLang_,
                                                   tupleElems))
            return false;
          if (tupleElems.size() != oldOccs.size())
            return false;

          replacement = baseTrim.str();
          for (size_t i = tupleElems.size(); i > 0; --i) {
            const size_t idx = i - 1;
            const auto &elem = tupleElems[idx];
            StringRef oldElem =
                baseTrim.slice(elem.trimBegin, elem.trimEnd).trim();
            if (oldElem != StringRef(oldOccs[idx]).trim())
              return false;
            replacement = stringutils::replaceRange(
                replacement, elem.trimBegin, elem.trimEnd, newOccs[idx]);
          }
        } else {
          SmallVector<RefoldModel::TupleArgRef, 8> tupleRefs;
          if (!findDirectTupleRefsForArg(argIdx, tupleRefs))
            return false;
          if (tupleRefs.size() != oldOccs.size())
            return false;

          replacement = baseTrim.str();
          for (size_t i = tupleRefs.size(); i > 0; --i) {
            const size_t idx = i - 1;
            const auto &ref = tupleRefs[idx];
            StringRef oldElem =
                baseTrim.slice(ref.callerByteBegin, ref.callerByteEnd).trim();
            if (oldElem != StringRef(oldOccs[idx]).trim())
              return false;
            replacement = stringutils::replaceRange(
                replacement, ref.callerByteBegin, ref.callerByteEnd,
                newOccs[idx]);
          }
        }

        replacement = StringRef(replacement).trim().str();
        if (replacement.empty())
          return false;
        if (!isVariadicFormal(argIdx) && hasTopLevelComma(replacement))
          return false;
        if (StringRef(replacement).trim() != baseTrim)
          changed = true;
        replByArg[argIdx] = std::move(replacement);
      }

      if (!changed || replByArg.empty())
        return false;

      outInv = baseInvText.str();
      auto keys = llvm::to_vector(
          llvm::map_range(replByArg, [](const auto &e) { return e.first; }));
      std::sort(keys.begin(), keys.end(), [&](uint32_t a, uint32_t b) {
        return invArgRanges[a].first > invArgRanges[b].first;
      });
      for (uint32_t argIdx : keys) {
        auto r = invArgRanges[argIdx];
        outInv = stringutils::replaceRange(outInv, r.first, r.second,
                                           replByArg[argIdx]);
      }
      return true;
    };

    // Multiple token-template assignments are acceptable only when they all
    // reconstruct the same invocation spelling. Otherwise the expansion surface
    // is underdetermined and this proof path fails closed.
    std::optional<std::string> uniqueInv;
    for (const auto &sol : solutions) {
      std::string candidate;
      if (!buildCandidateInvocation(sol, candidate))
        continue;
      if (!uniqueInv) {
        uniqueInv = std::move(candidate);
        continue;
      }
      if (*uniqueInv != candidate)
        return std::nullopt;
    }

    if (!uniqueInv)
      return std::nullopt;

    trace("macro/template",
          "template solver SUCCESS root id={0} name={1} coverA=[{2},{3}) "
          "coverB=[{4},{5}) newInv='{6}'",
          m.id, m.name, cover->first, cover->second, bEnv->first, bEnv->second,
          stringutils::showWSWithClip(*uniqueInv, 240));

    MacroPatch patch{*m.invB, *m.invE, std::move(*uniqueInv), m.id};
    StampMacroPatchProof(patch, MacroPatchProofKind::ArgsOnlyStandard,
                         /*validated=*/true,
                         /*structurePreserving=*/true, m.id);
    return patch;
  };

  if (auto templatePatch = tryTemplateSolvedArgsOnlyPatch())
    return templatePatch;

  // Fast path for token-paste edits. A single pasted token can embed multiple
  // argument contributions (e.g., X##_##Y##_##Z), so a single edit hunk may
  // change multiple arg segments inside that token (e.g., a_b_c -> d_e_f). In
  // that case we attempt to derive per-arg segment replacements and splice them
  // into the invocation spelling.
  if (HunkTouchesAnyPasteToken(m, h)) {
    auto edits = DerivePasteArgEdits(m, h);
    if (edits && !edits->empty()) {
      DenseMap<uint32_t, std::string> replByArgIdx;
      for (const auto &pae : *edits) {
        uint32_t argIdx = pae.argIdx;
        if (static_cast<size_t>(argIdx) >= invArgRanges.size())
          return std::nullopt;

        // A single argument may contribute multiple segments to the same
        // pasted token (e.g. X##_..._##X). We merge repeated argIdx
        // conservatively after deriving the candidate replacement below.
        auto range = invArgRanges[argIdx];
        StringRef baseArgText =
            baseInvText.substr(range.first, range.second - range.first);

        // Prefer an exact source-slice splice when the replay witness
        // provides one. Otherwise retain the existing conservative boundary
        // splice for legacy paste-span metadata.
        std::string newArg =
            (pae.argByteBegin && pae.argByteEnd)
                ? SplicePasteSegmentIntoSpellingArgExact(
                      baseArgText, *pae.argByteBegin, *pae.argByteEnd,
                      pae.oldSeg, pae.newSeg)
                : SplicePasteSegmentIntoSpellingArg(baseArgText, pae.oldSeg,
                                                    pae.newSeg);
        if (newArg.empty()) {
          // Deleting an entire argument (making it empty) is legal. Accept this
          // only when the paste-span covered the whole argument spelling.
          if (!(StringRef(pae.newSeg).trim().empty() &&
                baseArgText.trim() == StringRef(pae.oldSeg).trim()))
            return std::nullopt;
        }

        // If the argument is not the variadic formal, replacing it with a
        // text that introduces a top-level comma would change the macro
        // invocation's argument list.
        if (!isVariadicFormal(argIdx) && hasTopLevelComma(newArg))
          return std::nullopt;

        auto existing = replByArgIdx.find(argIdx);
        if (existing != replByArgIdx.end()) {
          if (existing->second != newArg)
            return std::nullopt;
          continue;
        }

        // Per-arg safety gate: validate standard + stringify occurrences for
        // this arg.
        //
        // NOTE: For multi-span paste edits where the pasted token length may
        // change, per-arg paste-span validation cannot be done reliably in
        // isolation. We validate paste tokens as a *group* below via
        // pasteArgReplacementsMatchAllPasteTokensInB(...).
        if (!MacroArgReplacementMatchesAllOccurrencesInBIgnorePaste(
                m, argIdx, baseArgText, newArg, tokenHunksCurrent)) {
          return std::nullopt;
        }

        replByArgIdx[argIdx] = std::move(newArg);
      }

      if (!replByArgIdx.empty()) {
        // Combined safety gate: applying all derived replacements must
        // reconstruct every pasted token occurrence exactly as seen in B.
        if (!PasteArgReplacementsMatchAllPasteTokensInB(
                m, baseInvText, invArgRanges, replByArgIdx)) {
          return std::nullopt;
        }

        // Apply all replacements to the invocation string (descending order).
        std::string newInv = baseInvText.str();
        auto keys = llvm::to_vector<8>(
            llvm::map_range(replByArgIdx, [](auto &e) { return e.first; }));
        std::sort(keys.begin(), keys.end(), [&](uint32_t a, uint32_t b) {
          return invArgRanges[a].first > invArgRanges[b].first;
        });

        for (uint32_t argIdx : keys) {
          auto r = invArgRanges[argIdx];
          newInv = stringutils::replaceRange(newInv, r.first, r.second,
                                             replByArgIdx[argIdx]);
        }

        trace("macro/args", "  args-only SUCCESS newInv='{0}'",
              stringutils::showWSWithClip(newInv, 200));
        {
        MacroPatch patch{*m.invB, *m.invE, std::move(newInv), m.id};
        StampMacroPatchProof(patch, MacroPatchProofKind::ArgsOnlyPasteMulti,
                             /*validated=*/true,
                             /*structurePreserving=*/true, m.id);
        // The builder already proved this rewrite by replaying the rewritten
        // invocation arguments against every pasted token occurrence in B.
        // Carry that proof source onto the accepted patch for Patch C.
        patch.pasteReplayValidated = true;
        return patch;
      }
      }
    }

    // Single-segment paste edit (existing behavior)
    //
    // This handles the common case where only one pasted segment changes (e.g.
    // X##_##Y, changing just X). The multi-span derivation above requires token
    // lengths to remain stable; when they do not, we fall back to deriving a
    // single segment edit from the token-level diff.
    auto pae = DerivePasteArgEdit(m, h);
    if (pae) {
      uint32_t argIdx = pae->argIdx;

      // HARD FAILURE: If we derived a paste edit but the index is invalid,
      // we must exit, not fall through.
      if (static_cast<size_t>(argIdx) >= invArgRanges.size())
        return std::nullopt;

      auto r = invArgRanges[argIdx];
      StringRef baseArgText = baseInvText.substr(r.first, r.second - r.first);
      std::string newArg =
          (pae->argByteBegin && pae->argByteEnd)
              ? SplicePasteSegmentIntoSpellingArgExact(
                    baseArgText, *pae->argByteBegin, *pae->argByteEnd,
                    pae->oldSeg, pae->newSeg)
              : SplicePasteSegmentIntoSpellingArg(baseArgText, pae->oldSeg,
                                                  pae->newSeg);
      if (newArg.empty()) {
        // Deleting an entire argument (making it empty) is legal. Accept this
        // only when the paste-span covered the whole argument spelling.
        if (!(StringRef(pae->newSeg).trim().empty() &&
              baseArgText.trim() == StringRef(pae->oldSeg).trim()))
          return std::nullopt;
      }

      if (!isVariadicFormal(argIdx) && hasTopLevelComma(newArg))
        return std::nullopt;

      // Safety gate: for single-segment paste edits we can directly validate
      // all occurrences, including paste-span occurrences, against the B
      // stream.
      if (!MacroArgReplacementMatchesAllOccurrencesInB(
              m, argIdx, baseArgText, newArg, tokenHunksCurrent)) {
        return std::nullopt;
      }

      std::string newInv =
          stringutils::replaceRange(baseInvText, r.first, r.second, newArg);
      trace("macro/args", "  args-only SUCCESS newInv='{0}'",
            stringutils::showWSWithClip(newInv, 200));
      {
        MacroPatch patch{*m.invB, *m.invE, std::move(newInv), m.id};
        StampMacroPatchProof(patch, MacroPatchProofKind::ArgsOnlyPasteSingle,
                             /*validated=*/true,
                             /*structurePreserving=*/true, m.id);
        // Single-segment paste rewrites are admitted only after direct replay
        // validation against all touched occurrences in B. Record that proof
        // source explicitly for the Patch C discharge gate.
        patch.pasteReplayValidated = true;
        return patch;
      }
    }

    // If we touched paste but could not safely derive a paste splice patch,
    // fall through to the standard (non-paste) args-only policy below.
  }

  // Standard (non-paste) args-only policy:
  // Collect arg-span occurrences (and stringify occurrences) and require the
  // entire hunk to be covered by those spans. Then derive per-arg replacements
  // from the B slices.
  std::vector<RefoldModel::PPArgSpan> occs;
  append_range(occs, m.argSpans);
  append_range(occs, m.stringifySpans);

  std::vector<char> occIsStringify;
  occIsStringify.resize(occs.size());
  std::fill_n(occIsStringify.begin(), m.argSpans.size(), false);
  std::fill_n(occIsStringify.begin() + m.argSpans.size(),
              m.stringifySpans.size(), true);

  trace("macro/args", "  occs={0}",
        PPArgSpanListToString(occs, occIsStringify));

  // Pure paste-only invocations have no STANDARD or STRINGIFY evidence, so the
  // normal args-only path bottoms out at occs.empty(). They are still
  // invertible when the touched pasted token can be segmented back into unique
  // per-argument replacements and those replacements reconstruct every pasted
  // token occurrence in B.
  if (occs.empty() && !m.pasteSpans.empty()) {
    trace("macro/args",
          "  pure-paste-only fallback: inv id={0} name={1} pasteSpans={2}",
          m.id, m.name, m.pasteSpans.size());

    auto edits = DerivePasteArgEdits(m, hArgs);
    if (!edits || edits->empty()) {
      trace("macro/args",
            "    pure-paste-only: no derivable paste edits for current hunk");
      return std::nullopt;
    }

    DenseMap<uint32_t, std::string> replByArgIdx;
    for (const auto &pae : *edits) {
      const uint32_t argIdx = pae.argIdx;
      if (static_cast<size_t>(argIdx) >= invArgRanges.size()) {
        trace("macro/args",
              "    pure-paste-only: argIdx {0} out of range ({1})", argIdx,
              invArgRanges.size());
        return std::nullopt;
      }

      auto range = invArgRanges[argIdx];
      StringRef baseArgText =
          baseInvText.substr(range.first, range.second - range.first);
      std::string newArg =
          (pae.argByteBegin && pae.argByteEnd)
              ? SplicePasteSegmentIntoSpellingArgExact(
                    baseArgText, *pae.argByteBegin, *pae.argByteEnd,
                    pae.oldSeg, pae.newSeg)
              : SplicePasteSegmentIntoSpellingArg(baseArgText, pae.oldSeg,
                                                  pae.newSeg);
      if (newArg.empty()) {
        if (!(StringRef(pae.newSeg).trim().empty() &&
              baseArgText.trim() == StringRef(pae.oldSeg).trim())) {
          trace("macro/args",
                "    pure-paste-only: splice failed argIdx={0} baseArg='{1}' "
                "oldSeg='{2}' newSeg='{3}'",
                argIdx, stringutils::showWSWithClip(baseArgText, 120),
                stringutils::showWSWithClip(pae.oldSeg, 120),
                stringutils::showWSWithClip(pae.newSeg, 120));
          return std::nullopt;
        }
      }

      if (!isVariadicFormal(argIdx) && hasTopLevelComma(newArg)) {
        trace("macro/args",
              "    pure-paste-only: argIdx={0} replacement introduces "
              "top-level comma: '{1}'",
              argIdx, stringutils::showWSWithClip(newArg, 120));
        return std::nullopt;
      }

      auto existing = replByArgIdx.find(argIdx);
      if (existing != replByArgIdx.end()) {
        if (existing->second != newArg) {
          trace("macro/args",
                "    pure-paste-only: conflicting replacements for argIdx={0} "
                "'{1}' vs '{2}'",
                argIdx, stringutils::showWSWithClip(existing->second, 120),
                stringutils::showWSWithClip(newArg, 120));
          return std::nullopt;
        }
        continue;
      }

      replByArgIdx[argIdx] = std::move(newArg);
    }

    if (replByArgIdx.empty()) {
      trace("macro/args",
            "    pure-paste-only: derived edits were all no-ops after merge");
      return std::nullopt;
    }

    if (!PasteArgReplacementsMatchAllPasteTokensInB(
            m, baseInvText, invArgRanges, replByArgIdx)) {
      trace("macro/args",
            "    pure-paste-only: combined paste-token validation failed");
      return std::nullopt;
    }

    std::string newInv = baseInvText.str();
    auto keys = llvm::to_vector<8>(
        llvm::map_range(replByArgIdx, [](auto &e) { return e.first; }));
    std::sort(keys.begin(), keys.end(), [&](uint32_t a, uint32_t b) {
      return invArgRanges[a].first > invArgRanges[b].first;
    });

    for (uint32_t argIdx : keys) {
      auto r = invArgRanges[argIdx];
      newInv = stringutils::replaceRange(newInv, r.first, r.second,
                                         replByArgIdx[argIdx]);
    }

    trace("macro/args", "    pure-paste-only SUCCESS newInv='{0}'",
          stringutils::showWSWithClip(newInv, 200));
    {
      MacroPatch patch{*m.invB, *m.invE, std::move(newInv), m.id};
      StampMacroPatchProof(patch,
                           MacroPatchProofKind::ArgsOnlyPurePasteOnly,
                           /*validated=*/true,
                           /*structurePreserving=*/true, m.id);
      // Pure-paste-only rewrites have no standard or stringify occurrences to
      // lean on, so successful all-paste replay is the decisive proof source.
      patch.pasteReplayValidated = true;
      return patch;
    }
  }

  if (occs.empty())
    return std::nullopt;

  std::vector<char> touchedOcc(occs.size(), 0);
  if (!HunkFullyWithinArgSpans(hArgs, occs, touchedOcc)) {
    trace("macro/args",
          "  hunk not fully within any arg spans -> fail args-only");
    return std::nullopt;
  }

  unsigned occFormalCount = static_cast<unsigned>(invArgRanges.size());
  for (const auto &sp : occs)
    occFormalCount = std::max(occFormalCount, (unsigned)sp.argIdx + 1);

  std::vector<char> touched(occFormalCount, 0);
  for (size_t i = 0; i < occs.size(); ++i) {
    if (!touchedOcc[i])
      continue;
    const auto &sp = occs[i];
    if (sp.argIdx >= touched.size())
      return std::nullopt;
    touched[sp.argIdx] = 1;
  }

  auto hunkTouchesFormalOccurrence =
      [&](const diffutils::Hunk &cand,
          const RefoldModel::PPArgSpan &sp) -> bool {
    // For replacements/deletions, touching is ordinary A-range overlap. For
    // pure insertions, the hunk has no A width, so require the existing
    // argument-span ownership helper to prove that the B insertion belongs to
    // this occurrence.
    if (cand.aStart == cand.aEnd) {
      auto bEnv = MapAToBTokenEnvelopeByPPArgSpan(sp);
      if (!bEnv)
        return false;
      return GetOwnedPureInsertionBRangeForArgSpan(sp, occs, *bEnv, cand)
          .has_value();
    }

    return cand.aStart < sp.end && cand.aEnd > sp.begin;
  };

  // A provenance-only LCS can split one logical macro-argument rewrite into
  // several pure-insertion islands around repeated punctuation.  When the
  // current seed is a pure insertion, widen the set of touched formals to every
  // occurrence in this same invocation cover that is independently touched by
  // another token hunk.  This does not move hunk boundaries and does not inspect
  // neighboring token spellings; it only lets the existing replay validator see
  // the complete split edit before deciding whether an args-only rewrite is
  // actually proven.
  if (hArgs.aStart == hArgs.aEnd && hArgs.bStart < hArgs.bEnd) {
    for (const diffutils::Hunk &cand : abTokHunks_) {
      if (cand.aStart < m.cover.begin || cand.aEnd > m.cover.end)
        continue;
      if (cand.bStart >= cand.bEnd)
        continue;

      for (const auto &sp : occs) {
        if (sp.argIdx >= touched.size())
          return std::nullopt;
        if (hunkTouchesFormalOccurrence(cand, sp))
          touched[sp.argIdx] = 1;
      }
    }
  }

  trace("macro/args", "  touchedOcc={0}",
        stringutils::boolArrayToString(touchedOcc));
  trace("macro/args", "  touched={0}", stringutils::boolArrayToString(touched));

  auto hunkTouchesTouchedFormal =
      [&](const diffutils::Hunk &cand) -> bool {
    for (const auto &sp : occs) {
      if (sp.argIdx >= touched.size() || !touched[sp.argIdx])
        continue;
      if (hunkTouchesFormalOccurrence(cand, sp))
        return true;
    }
    return false;
  };

  SmallVector<diffutils::Hunk, 8> tokenHunksForTouchedFormals;
  tokenHunksForTouchedFormals.push_back(hArgs);
  for (const auto &cand : abTokHunks_) {
    if (cand.aStart == hArgs.aStart && cand.aEnd == hArgs.aEnd &&
        cand.bStart == hArgs.bStart && cand.bEnd == hArgs.bEnd)
      continue;
    if (!hunkTouchesTouchedFormal(cand))
      continue;
    tokenHunksForTouchedFormals.push_back(cand);
  }

  auto buildCombinedInsertionEnvelope = [&](const diffutils::Hunk &left,
                                           const diffutils::Hunk &right) {
    diffutils::Hunk env;
    env.aStart = std::min(left.aStart, right.aStart);
    env.aEnd = std::max(left.aStart, right.aStart);
    env.bStart = std::min(left.bStart, right.bStart);
    env.bEnd = std::max(left.bEnd, right.bEnd);
    return env;
  };

  auto trimCommonEdgeTokensLocal = [&](diffutils::Hunk hh) {
    while (hh.aStart < hh.aEnd && hh.bStart < hh.bEnd) {
      size_t aIdx = static_cast<size_t>(hh.aStart);
      size_t bIdx = static_cast<size_t>(hh.bStart);
      if (aIdx >= aToks_.size() || bIdx >= bToks_.size())
        break;
      if (aToks_[aIdx].spelling != bToks_[bIdx].spelling)
        break;
      ++hh.aStart;
      ++hh.bStart;
    }
    while (hh.aEnd > hh.aStart && hh.bEnd > hh.bStart) {
      size_t aIdx = static_cast<size_t>(hh.aEnd - 1);
      size_t bIdx = static_cast<size_t>(hh.bEnd - 1);
      if (aIdx >= aToks_.size() || bIdx >= bToks_.size())
        break;
      if (aToks_[aIdx].spelling != bToks_[bIdx].spelling)
        break;
      --hh.aEnd;
      --hh.bEnd;
    }
    return hh;
  };

  auto sameTokHunk = [](const diffutils::Hunk &lhs,
                        const diffutils::Hunk &rhs) {
    return lhs.aStart == rhs.aStart && lhs.aEnd == rhs.aEnd &&
           lhs.bStart == rhs.bStart && lhs.bEnd == rhs.bEnd;
  };

  auto maybeAddSyntheticTouchedFormalEnvelope =
      [&](const RefoldModel::PPArgSpan &sp, const diffutils::Hunk &anchor) {
        if (anchor.aStart != anchor.aEnd)
          return;
        if (anchor.aStart < m.cover.begin || anchor.aEnd > m.cover.end)
          return;

        for (const auto &partner : abTokHunks_) {
          if (sameTokHunk(anchor, partner))
            continue;
          if (partner.aStart != partner.aEnd)
            continue;
          if (partner.aStart < m.cover.begin || partner.aEnd > m.cover.end)
            continue;

          const diffutils::Hunk env =
              buildCombinedInsertionEnvelope(anchor, partner);
          const diffutils::Hunk envTrim = trimCommonEdgeTokensLocal(env);

          if (envTrim.aStart >= envTrim.aEnd)
            continue;
          if (!(sp.begin <= envTrim.aStart && envTrim.aEnd <= sp.end))
            continue;

          SmallVector<char, 8> envTouched(occs.size(), 0);
          if (!HunkFullyWithinArgSpans(envTrim, occs, envTouched))
            continue;

          bool touchesThisExactOccurrence = false;
          for (size_t occIdx = 0; occIdx < occs.size(); ++occIdx) {
            if (!envTouched[occIdx])
              continue;
            if (occs[occIdx].argIdx != sp.argIdx)
              return;
            if (occs[occIdx].begin != sp.begin || occs[occIdx].end != sp.end)
              return;
            touchesThisExactOccurrence = true;
          }
          if (!touchesThisExactOccurrence)
            continue;

          tokenHunksForTouchedFormals.push_back(envTrim);
        }
      };

  SmallVector<diffutils::Hunk, 8> seedTokenHunks(
      tokenHunksForTouchedFormals.begin(), tokenHunksForTouchedFormals.end());
  for (size_t occIdx = 0; occIdx < occs.size(); ++occIdx) {
    const auto &sp = occs[occIdx];
    if (sp.argIdx >= touched.size() || !touched[sp.argIdx])
      continue;
    for (const auto &cand : seedTokenHunks) {
      if (cand.aStart != cand.aEnd)
        continue;
      maybeAddSyntheticTouchedFormalEnvelope(sp, cand);
    }
  }

  auto hunkLess = [](const diffutils::Hunk &lhs, const diffutils::Hunk &rhs) {
    if (lhs.aStart != rhs.aStart)
      return lhs.aStart < rhs.aStart;
    if (lhs.aEnd != rhs.aEnd)
      return lhs.aEnd < rhs.aEnd;
    if (lhs.bStart != rhs.bStart)
      return lhs.bStart < rhs.bStart;
    return lhs.bEnd < rhs.bEnd;
  };
  auto formatHunkList = [](ArrayRef<diffutils::Hunk> hunks) {
    std::string out;
    raw_string_ostream os(out);
    os << "[";
    for (size_t i = 0; i < hunks.size(); ++i) {
      if (i)
        os << ", ";
      const auto &h = hunks[i];
      os << "A[" << h.aStart << "," << h.aEnd << ")"
         << "->B[" << h.bStart << "," << h.bEnd << ")";
    }
    os << "]";
    return os.str();
  };
  llvm::sort(tokenHunksForTouchedFormals, hunkLess);
  tokenHunksForTouchedFormals.erase(
      std::unique(tokenHunksForTouchedFormals.begin(),
                  tokenHunksForTouchedFormals.end(), sameTokHunk),
      tokenHunksForTouchedFormals.end());
  ArrayRef<diffutils::Hunk> tokenHunks(tokenHunksForTouchedFormals);

  trace("macro/args", "  tokenHunksForTouchedFormals({0})={1}",
        tokenHunks.size(),
        formatHunkList(tokenHunksForTouchedFormals));

  struct OccObservation {
    StringRef oldText;
    std::string newText;
  };

  auto tryTupleForwardedCallerTupleRewrite =
      [&](uint32_t callerArgIdx, StringRef baseArgText,
          ArrayRef<OccObservation> occObservations,
          std::string &outNewArg) -> bool {
    auto formatTupleRefs = [&](ArrayRef<RefoldModel::TupleArgRef> refs) {
      std::string out;
      raw_string_ostream os(out);
      os << "[";
      for (size_t i = 0; i < refs.size(); ++i) {
        if (i)
          os << ", ";
        os << "{caller_param_index=" << refs[i].callerParamIndex
           << ", caller_byte_begin=" << refs[i].callerByteBegin
           << ", caller_byte_end=" << refs[i].callerByteEnd << "}";
      }
      os << "]";
      return os.str();
    };

    auto formatOccurrenceObservations = [&](ArrayRef<OccObservation> obs) {
      std::string out;
      raw_string_ostream os(out);
      os << "[";
      for (size_t i = 0; i < obs.size(); ++i) {
        if (i)
          os << ", ";
        os << "{old='" << stringutils::showWSWithClip(obs[i].oldText, 120)
           << "' new='" << stringutils::showWSWithClip(obs[i].newText, 120)
           << "'}";
      }
      os << "]";
      return os.str();
    };

    /// Tuple rewrite modes are ordered from strongest proof to weakest.
    ///
    /// DirectTupleRefs uses producer-supplied tuple element byte ranges in
    /// the normalized child invocation. VariadicIdentityForward is the
    /// fallback for variadic forwarding wrappers whose immediate child
    /// keeps the caller's variadic tuple intact as a single full-width
    /// forwarded argument (for example `__VA_ARGS__`).
    enum class TupleRewriteMode {
      None,
      DirectTupleRefs,
      VariadicIdentityForward,
    };

    StringRef parentTrim = baseArgText.trim();
    trace("macro/tuple",
          "tuple-forward enter root id={0} name={1} argIdx={2} "
          "baseArg='{3}' occObservations={4}",
          m.id, m.name, callerArgIdx,
          stringutils::showWSWithClip(baseArgText, 200),
          formatOccurrenceObservations(occObservations));
    if (parentTrim.empty())
      return false;

    auto getNormalizedArgText =
        [&](const RefoldModel::MacroInvocation &inv,
            uint32_t argIdx) -> std::optional<StringRef> {
      if (!inv.normalizedInvText)
        return std::nullopt;
      if (argIdx >= inv.normalizedInvArgTextRanges.size())
        return std::nullopt;
      const auto &rng = inv.normalizedInvArgTextRanges[argIdx];
      if (!rng.first || !rng.second || *rng.second < *rng.first)
        return std::nullopt;
      if (*rng.second > inv.normalizedInvText->size())
        return std::nullopt;
      return StringRef(*inv.normalizedInvText)
          .slice((size_t)*rng.first, (size_t)*rng.second)
          .trim();
    };

    auto getInvocationArgText =
        [&](const RefoldModel::MacroInvocation &inv,
            uint32_t argIdx) -> std::optional<StringRef> {
      if (!inv.invText || !inv.invB)
        return std::nullopt;
      if (argIdx >= inv.invArgRanges.size())
        return std::nullopt;
      const auto &rng = inv.invArgRanges[argIdx];
      if (!rng.first || !rng.second || *rng.second < *rng.first ||
          *rng.first < *inv.invB)
        return std::nullopt;
      const uint64_t relB = *rng.first - *inv.invB;
      const uint64_t relE = *rng.second - *inv.invB;
      if (relE < relB || relE > inv.invText->size())
        return std::nullopt;
      return StringRef(*inv.invText).slice((size_t)relB, (size_t)relE).trim();
    };

    // The variadic identity-forward mode reconstructs the caller tuple by
    // splitting the original variadic actual into top-level elements, then
    // matching those elements positionally against the observed expansion
    // occurrences. Use the Clang lexer here so comments, literals, and
    // escaped text are handled by the token stream rather than by manual
    // character parsing.

    const RefoldModel::MacroInvocation *tupleChild = nullptr;
    TupleRewriteMode rewriteMode = TupleRewriteMode::None;
    SmallVector<std::pair<uint32_t, StringRef>, 8> childArgs;
    SmallVector<RefoldModel::TupleArgRef, 8> childTupleRefs;
    std::optional<uint32_t> identityForwardChildArgIdx;

    for (const auto &cand : model_.GetMacroInvocations()) {
      if (!cand.callerMacroId || *cand.callerMacroId != m.id)
        continue;

      if (cand.normalizedInvText && !cand.normalizedInvArgTextRanges.empty() &&
          !cand.argTupleRefs.empty() &&
          cand.normalizedInvArgTextRanges.size() == cand.argTupleRefs.size()) {
        SmallVector<std::pair<uint32_t, StringRef>, 8> localChildArgs;
        SmallVector<RefoldModel::TupleArgRef, 8> localTupleRefs;
        DenseSet<StringRef> seenOldTexts;
        bool ok = false;
        for (uint32_t childArgIdx = 0; childArgIdx < cand.argTupleRefs.size();
             ++childArgIdx) {
          const auto &refs = cand.argTupleRefs[childArgIdx];
          if (refs.size() != 1)
            continue;
          const auto &ref = refs.front();
          if (ref.callerParamIndex != callerArgIdx)
            continue;

          auto oldArgText = getNormalizedArgText(cand, childArgIdx);
          if (!oldArgText)
            return false;
          if (seenOldTexts.find(*oldArgText) != seenOldTexts.end())
            return false;
          seenOldTexts.insert(*oldArgText);

          if (ref.callerByteEnd < ref.callerByteBegin ||
              ref.callerByteEnd > parentTrim.size())
            return false;
          StringRef slice =
              parentTrim.slice(ref.callerByteBegin, ref.callerByteEnd).trim();
          if (slice != oldArgText->trim())
            return false;

          localChildArgs.push_back({childArgIdx, *oldArgText});
          localTupleRefs.push_back(ref);
          ok = true;
        }
        if (ok) {
          if (tupleChild)
            return false;
          tupleChild = &cand;
          rewriteMode = TupleRewriteMode::DirectTupleRefs;
          childArgs = std::move(localChildArgs);
          childTupleRefs = std::move(localTupleRefs);
          continue;
        }
      }

      if (!isVariadicFormal(callerArgIdx) || !cand.invText || !cand.invB ||
          cand.invArgRanges.empty() || cand.argRefs.empty())
        continue;

      // Variadic forwarding wrappers may not carry tuple-specific metadata.
      // Accept a second certified shape where one child argument is a
      // full-width identity forward of the caller variadic formal. That
      // proves the caller tuple survives unchanged at the child hop, so we
      // can safely rebuild it element-by-element from the occurrence
      // observations.
      std::optional<uint32_t> localIdentityArgIdx;
      for (uint32_t childArgIdx = 0; childArgIdx < cand.invArgRanges.size() &&
                                     childArgIdx < cand.argRefs.size();
           ++childArgIdx) {
        const auto &rng = cand.invArgRanges[childArgIdx];
        if (!rng.first || !rng.second || *rng.second < *rng.first ||
            *rng.first < *cand.invB)
          continue;
        const auto &refs = cand.argRefs[childArgIdx];
        if (refs.size() != 1)
          continue;
        const auto &ref = refs.front();
        if (ref.callerParamIndex != callerArgIdx)
          continue;

        const uint64_t relB = *rng.first - *cand.invB;
        const uint64_t relE = *rng.second - *cand.invB;
        if (relE < relB || relE > cand.invText->size())
          continue;
        StringRef rawArg =
            StringRef(*cand.invText).slice((size_t)relB, (size_t)relE);
        size_t trimLead = 0;
        while (trimLead < rawArg.size() &&
               std::isspace((unsigned char)rawArg[trimLead]))
          ++trimLead;
        size_t trimEnd = rawArg.size();
        while (trimEnd > trimLead &&
               std::isspace((unsigned char)rawArg[trimEnd - 1]))
          --trimEnd;
        if (trimLead == trimEnd)
          continue;

        const uint64_t trimmedAbsBegin = relB + trimLead;
        const uint64_t trimmedAbsEnd = relB + trimEnd;
        if (ref.byteBegin != trimmedAbsBegin || ref.byteEnd != trimmedAbsEnd)
          continue;

        auto oldArgText = getInvocationArgText(cand, childArgIdx);
        if (!oldArgText || oldArgText->empty())
          continue;
        if (localIdentityArgIdx)
          return false;
        localIdentityArgIdx = childArgIdx;
      }

      if (!localIdentityArgIdx)
        continue;
      if (tupleChild)
        return false;
      tupleChild = &cand;
      rewriteMode = TupleRewriteMode::VariadicIdentityForward;
      identityForwardChildArgIdx = *localIdentityArgIdx;
    }

    if (!tupleChild)
      return false;

    std::string rebuilt;
    bool changed = false;

    if (rewriteMode == TupleRewriteMode::DirectTupleRefs) {
      if (childArgs.empty() || childArgs.size() != childTupleRefs.size())
        return false;

      StringMap<std::string> newTextByOld;
      for (const auto &obs : occObservations) {
        auto it = newTextByOld.find(obs.oldText);
        if (it == newTextByOld.end()) {
          newTextByOld[obs.oldText] = obs.newText;
          continue;
        }
        if (it->second != obs.newText)
          return false;
      }

      rebuilt = parentTrim.str();
      SmallVector<unsigned, 8> order(childTupleRefs.size());
      for (unsigned i = 0; i < childTupleRefs.size(); ++i)
        order[i] = i;
      llvm::sort(order, [&](unsigned a, unsigned b) {
        return childTupleRefs[a].callerByteBegin >
               childTupleRefs[b].callerByteBegin;
      });

      for (unsigned idx : order) {
        const auto &pair = childArgs[idx];
        StringRef oldChildText = pair.second.trim();
        auto it = newTextByOld.find(oldChildText);
        if (it == newTextByOld.end())
          continue;
        const auto &ref = childTupleRefs[idx];
        rebuilt = stringutils::replaceRange(rebuilt, ref.callerByteBegin,
                                            ref.callerByteEnd, it->second);
        if (it->second != oldChildText)
          changed = true;
      }

      if (!changed)
        return false;

      trace("macro/tuple",
            "tuple-forward rebuilt root id={0} name={1} argIdx={2} "
            "parentTrim='{3}' rebuilt='{4}' tupleRefs={5}",
            m.id, m.name, callerArgIdx,
            stringutils::showWSWithClip(parentTrim, 200),
            stringutils::showWSWithClip(rebuilt, 200),
            formatTupleRefs(childTupleRefs));
    } else if (rewriteMode == TupleRewriteMode::VariadicIdentityForward) {
      SmallVector<TupleElementSlice, 8> tupleElems;
      if (!splitTopLevelTupleElementsWithLexer(parentTrim, lexLang_,
                                               tupleElems))
        return false;

      // Identity-forward rewrites are positional: the immediate child keeps
      // the caller variadic tuple intact, so each observed occurrence must
      // correspond to exactly one top-level tuple element in order.
      if (tupleElems.size() != occObservations.size())
        return false;

      rebuilt = parentTrim.str();
      for (size_t i = tupleElems.size(); i > 0; --i) {
        const auto &elem = tupleElems[i - 1];
        StringRef oldElemText =
            parentTrim.slice(elem.trimBegin, elem.trimEnd).trim();

        // Replacements apply from right to left so earlier byte offsets
        // stay valid while we splice into the rebuilt caller tuple.
        if (oldElemText != occObservations[i - 1].oldText.trim())
          return false;
        rebuilt =
            stringutils::replaceRange(rebuilt, elem.trimBegin, elem.trimEnd,
                                      occObservations[i - 1].newText);
        if (occObservations[i - 1].newText != oldElemText)
          changed = true;
      }

      if (!changed)
        return false;

      trace("macro/tuple",
            "tuple-forward rebuilt variadic identity root id={0} name={1} "
            "argIdx={2} childId={3} childName={4} childArgIdx={5} "
            "parentTrim='{6}' rebuilt='{7}'",
            m.id, m.name, callerArgIdx, tupleChild->id, tupleChild->name,
            identityForwardChildArgIdx ? *identityForwardChildArgIdx
                                       : uint32_t(0),
            stringutils::showWSWithClip(parentTrim, 200),
            stringutils::showWSWithClip(rebuilt, 200));
    } else {
      return false;
    }

    outNewArg = StringRef(rebuilt).trim().str();
    trace("macro/args",
          "    tuple-forwarded rewrite accepted for root id={0} name={1} "
          "argIdx={2} childId={3} childName={4} baseArg='{5}' newArg='{6}' "
          "mode={7}",
          m.id, m.name, callerArgIdx, tupleChild->id, tupleChild->name,
          stringutils::showWSWithClip(baseArgText, 200),
          stringutils::showWSWithClip(outNewArg, 200),
          rewriteMode == TupleRewriteMode::DirectTupleRefs
              ? StringRef("direct_tuple_refs")
              : StringRef("variadic_identity_forward"));
    return true;
  };

  auto delimiterBalance = [&](StringRef s) {
    struct Balance {
      int paren = 0;
      int bracket = 0;
      int brace = 0;
    } bal;
    for (char c : s) {
      switch (c) {
      case '(':
        ++bal.paren;
        break;
      case ')':
        --bal.paren;
        break;
      case '[':
        ++bal.bracket;
        break;
      case ']':
        --bal.bracket;
        break;
      case '{':
        ++bal.brace;
        break;
      case '}':
        --bal.brace;
        break;
      default:
        break;
      }
    }
    return bal;
  };

  auto maybeExtendRightBoundaryClosers =
      [&](const RefoldModel::PPArgSpan &sp,
          std::pair<size_t, size_t> env,
          StringRef oldText) -> std::pair<size_t, size_t> {
        StringRef curText = SliceBSource(env.first, env.second).trim();
        auto oldBal = delimiterBalance(oldText);
        auto newBal = delimiterBalance(curText);

        int needParen = std::max(0, newBal.paren - oldBal.paren);
        int needBracket = std::max(0, newBal.bracket - oldBal.bracket);
        int needBrace = std::max(0, newBal.brace - oldBal.brace);
        if (needParen == 0 && needBracket == 0 && needBrace == 0)
          return env;

        uint64_t aPos = sp.end;
        size_t bPos = env.second;
        while ((needParen > 0 || needBracket > 0 || needBrace > 0) &&
               aPos < aToks_.size() && bPos < bToks_.size()) {
          StringRef aTok = aToks_[static_cast<size_t>(aPos)].spelling;
          StringRef bTok = bToks_[bPos].spelling;
          if (aTok != bTok)
            break;

          if (aTok == ")" && needParen > 0) {
            --needParen;
            ++aPos;
            ++bPos;
            env.second = bPos;
            continue;
          }
          if (aTok == "]" && needBracket > 0) {
            --needBracket;
            ++aPos;
            ++bPos;
            env.second = bPos;
            continue;
          }
          if (aTok == "}" && needBrace > 0) {
            --needBrace;
            ++aPos;
            ++bPos;
            env.second = bPos;
            continue;
          }
          break;
        }
        return env;
      };

  // Compute argument replacements implied by each touched occurrence. Multiple
  // occurrences of the same argIdx must imply the exact same replacement,
  // otherwise the macro cannot be refolded args-only.
  DenseMap<uint32_t, std::string> replByArgIdx;
  SmallVector<uint32_t, 8> touchedArgIdxs;
  for (const auto &sp : occs) {
    if (sp.argIdx >= touched.size() || !touched[sp.argIdx])
      continue;
    if (!llvm::is_contained(touchedArgIdxs, sp.argIdx))
      touchedArgIdxs.push_back(sp.argIdx);
  }

  for (uint32_t argIdx : touchedArgIdxs) {
    if (static_cast<size_t>(argIdx) >= invArgRanges.size())
      return std::nullopt;

    auto r0 = invArgRanges[argIdx];
    StringRef baseArgText = baseInvText.substr(r0.first, r0.second - r0.first);

    SmallVector<OccObservation, 8> occObservations;
    std::optional<std::string> unifiedNewArg;
    bool needTupleFallback = false;

    for (size_t i = 0; i < occs.size(); ++i) {
      const auto &sp = occs[i];
      if (sp.argIdx != argIdx)
        continue;

      auto bEnv = MapAToBTokenEnvelopeByPPArgSpan(sp);
      if (!bEnv) {
        if (h.bStart >= h.bEnd)
          return std::nullopt;
        bEnv = {static_cast<size_t>(h.bStart), static_cast<size_t>(h.bEnd)};
      }

      if (bEnv) {
        size_t e0 = bEnv->first;
        size_t e1 = bEnv->second;
        for (const auto &candH : tokenHunks) {
          if (auto owned = GetOwnedPureInsertionBRangeForArgSpan(
                  sp, occs, *bEnv, candH)) {
            const size_t insB0 = owned->first;
            const size_t insB1 = owned->second;
            if (!(insB1 < e0 || e1 < insB0)) {
              e0 = std::min(e0, insB0);
              e1 = std::max(e1, insB1);
            }
            continue;
          }

          if (candH.aStart == candH.aEnd)
            continue;
          if (candH.aStart < sp.end && candH.aEnd > sp.begin &&
              candH.bStart < candH.bEnd) {
            e0 = std::min(e0, static_cast<size_t>(candH.bStart));
            e1 = std::max(e1, static_cast<size_t>(candH.bEnd));
          }
        }

        if (e0 != bEnv->first || e1 != bEnv->second) {
          trace("macro/args",
                "  extend env with touched-formal hunks: argIdx={0} "
                "env=[{1},{2}) -> [{3},{4})",
                static_cast<size_t>(argIdx), bEnv->first, bEnv->second, e0, e1);
          bEnv = std::make_pair(e0, e1);
        }
      }

      StringRef oldText = SliceASource(sp.begin, sp.end).trim();
      if (sp.kind == PPArgSpanKind::Standard) {
        auto grownEnv = maybeExtendRightBoundaryClosers(sp, *bEnv, oldText);
        if (grownEnv.second != bEnv->second) {
          trace("macro/args",
                "  extend env with stable right closers: argIdx={0} "
                "env=[{1},{2}) -> [{3},{4}) old='{5}'",
                static_cast<size_t>(argIdx), bEnv->first, bEnv->second,
                grownEnv.first, grownEnv.second,
                stringutils::showWSWithClip(oldText, 120));
          bEnv = grownEnv;
        }
      }

      StringRef bSlice = SliceBSource(bEnv->first, bEnv->second).trim();
      std::string newArg = bSlice.str();

      if (occIsStringify[i]) {
        auto un = UnstringifyLiteralToArgText(bSlice, isVariadicFormal(argIdx));
        if (!un)
          return std::nullopt;

        auto canon = CanonicalizeStringifyInversePayload(*un);
        if (!canon || StringRef(*canon).trim() != StringRef(*un).trim()) {
          trace("macro/args",
                "    stringify inverse ambiguous for argIdx={0} payload='{1}'",
                argIdx, stringutils::showWSWithClip(*un, 200));
          return std::nullopt;
        }
        newArg = std::move(*canon);
        auto oldUn = UnstringifyLiteralToArgText(oldText, true);
        if (oldUn)
          oldText = StringRef(*oldUn).trim();
      }

      if (!occIsStringify[i] && !m.pasteSpans.empty()) {
        bool argHasPaste = false;
        for (const auto &ps : m.pasteSpans) {
          if (ps.argIdx == argIdx) {
            argHasPaste = true;
            break;
          }
        }

        if (argHasPaste) {
          StringRef aSlice = SliceASource(sp.begin, sp.end).trim();
          if (!aSlice.empty()) {
            size_t pos = baseArgText.find(aSlice);
            if (pos != StringRef::npos) {
              std::string cand = baseArgText.substr(0, pos).str() +
                                 bSlice.str() +
                                 baseArgText.substr(pos + aSlice.size()).str();
              newArg = StringRef(cand).trim().str();
              trace("macro/args",
                    "    lift/paste argIdx={0} baseArg={1} aSlice={2} "
                    "bSlice={3} -> newArg={4}",
                    argIdx, stringutils::showWSWithClip(baseArgText, 200),
                    stringutils::showWSWithClip(aSlice, 200),
                    stringutils::showWSWithClip(bSlice, 200),
                    stringutils::showWSWithClip(newArg, 200));
            } else {
              // The normal case: split the rewritten core around the original
              // literal delimiters and require a unique segmentation.
              trace("macro/args",
                    "    lift/paste FAILED argIdx={0} baseArg={1} aSlice={2} "
                    "bSlice={3}",
                    argIdx, stringutils::showWS(baseArgText),
                    stringutils::showWS(aSlice), stringutils::showWS(bSlice));
            }
          }
        }
      }

      occObservations.push_back(OccObservation{oldText, newArg});
      if (!unifiedNewArg)
        unifiedNewArg = newArg;
      else if (*unifiedNewArg != newArg)
        needTupleFallback = true;
    }

    std::string finalNewArg;
    bool tupleForwarded = false;
    if (needTupleFallback) {
      if (!tryTupleForwardedCallerTupleRewrite(argIdx, baseArgText,
                                               occObservations, finalNewArg)) {
        return std::nullopt;
      }
      tupleForwarded = true;
    } else if (unifiedNewArg) {
      finalNewArg = *unifiedNewArg;
    } else {
      continue;
    }

    if (!isVariadicFormal(argIdx) && hasTopLevelComma(finalNewArg))
      return std::nullopt;

    auto tupleSliceConsistencyMatchesAllOccurrencesInB = [&]() -> bool {
      llvm::StringMap<std::string> newTextByOld;
      for (const auto &obs : occObservations) {
        StringRef oldKey = StringRef(obs.oldText).trim();
        StringRef newVal = StringRef(obs.newText).trim();
        auto it = newTextByOld.find(oldKey);
        if (it == newTextByOld.end()) {
          newTextByOld[oldKey] = newVal.str();
          continue;
        }
        if (StringRef(it->second).trim() != newVal)
          return false;
      }

      for (const auto &s : m.argSpans) {
        if (s.argIdx != argIdx || s.kind != PPArgSpanKind::Standard)
          continue;
        StringRef oldSlice = SliceASource(s.begin, s.end).trim();
        auto expectedIt = newTextByOld.find(oldSlice);
        if (expectedIt == newTextByOld.end())
          return false;

        auto bEnv = MapAToBTokenEnvelopeByPPArgSpan(s);
        if (!bEnv)
          return false;

        size_t lo = bEnv->first;
        size_t hi = bEnv->second;
        for (const auto &hk : tokenHunks) {
          if (auto owned = GetOwnedPureInsertionBRangeForArgSpan(s, m.argSpans,
                                                                 *bEnv, hk)) {
            lo = std::min(lo, owned->first);
            hi = std::max(hi, owned->second);
            continue;
          }
          if (hk.aStart == hk.aEnd)
            continue;
          if (hk.aStart < s.end && hk.aEnd > s.begin && hk.bStart < hk.bEnd) {
            lo = static_cast<size_t>(std::min<uint64_t>(lo, hk.bStart));
            hi = static_cast<size_t>(std::max<uint64_t>(hi, hk.bEnd));
          }
        }

        if (s.kind == PPArgSpanKind::Standard) {
          auto grownEnv = maybeExtendRightBoundaryClosers(
              s, std::make_pair(lo, hi), oldSlice);
          lo = grownEnv.first;
          hi = grownEnv.second;
        }

        StringRef tokText = SliceBSource(lo, hi).trim();
        if (tokText != StringRef(expectedIt->second).trim())
          return false;
      }
      return true;
    };

    if (!(tupleForwarded
              ? tupleSliceConsistencyMatchesAllOccurrencesInB()
              : MacroArgReplacementMatchesAllOccurrencesInB(
                    m, argIdx, baseArgText, finalNewArg, tokenHunks))) {
      bool hasTupleChildForArg = false;
      for (const auto &cand : model_.GetMacroInvocations()) {
        if (!cand.callerMacroId || *cand.callerMacroId != m.id)
          continue;
        if (cand.argTupleRefs.empty())
          continue;
        for (const auto &refs : cand.argTupleRefs) {
          for (const auto &ref : refs) {
            if (ref.callerParamIndex == argIdx) {
              hasTupleChildForArg = true;
              break;
            }
          }
          if (hasTupleChildForArg)
            break;
        }
        if (hasTupleChildForArg)
          break;
      }
      trace(
          "macro/args",
          "    consistency check FAILED for argIdx={0} newArg='{1}' -> expand",
          argIdx, stringutils::showWSWithClip(finalNewArg, 200));
      if (hasTupleChildForArg) {
        trace("macro/tuple",
              "tuple-forward consistency failure root id={0} name={1} "
              "argIdx={2} baseArg='{3}' newArg='{4}' tokenHunks={5}",
              m.id, m.name, argIdx,
              stringutils::showWSWithClip(baseArgText, 200),
              stringutils::showWSWithClip(finalNewArg, 200),
              formatHunkList(tokenHunksForTouchedFormals));
        for (const auto &s : m.argSpans) {
          if (s.argIdx != argIdx || s.kind != PPArgSpanKind::Standard)
            continue;
          auto bEnv = MapAToBTokenEnvelopeByPPArgSpan(s);
          if (!bEnv) {
            trace("macro/tuple",
                  "  standard occurrence has no B envelope root id={0} "
                  "name={1} argIdx={2} occA=[{3},{4})",
                  m.id, m.name, argIdx, s.begin, s.end);
            continue;
          }
          size_t lo = bEnv->first;
          size_t hi = bEnv->second;
          SmallVector<std::string, 8> hunkEffects;
          for (const auto &hk : tokenHunks) {
            if (auto owned = GetOwnedPureInsertionBRangeForArgSpan(
                    s, m.argSpans, *bEnv, hk)) {
              hunkEffects.push_back(
                  formatv("owned {0} -> [{1},{2}) '{3}'", hk.ToString(),
                          owned->first, owned->second,
                          stringutils::showWSWithClip(
                              SliceBSource(owned->first, owned->second), 80))
                      .str());
              lo = std::min(lo, owned->first);
              hi = std::max(hi, owned->second);
              continue;
            }
            bool touches = false;
            if (hk.aStart != hk.aEnd)
              touches = (hk.aStart < s.end && hk.aEnd > s.begin);
            if (touches && hk.bStart < hk.bEnd) {
              hunkEffects.push_back(
                  formatv("overlap {0} -> [{1},{2}) '{3}'", hk.ToString(),
                          (uint64_t)hk.bStart, (uint64_t)hk.bEnd,
                          stringutils::showWSWithClip(
                              SliceBSource(hk.bStart, hk.bEnd), 80))
                      .str());
              lo = static_cast<size_t>(std::min<uint64_t>(lo, hk.bStart));
              hi = static_cast<size_t>(std::max<uint64_t>(hi, hk.bEnd));
            } else {
              // The normal case: split the rewritten core around the original
              // literal delimiters and require a unique segmentation.
              hunkEffects.push_back(
                  formatv("ignored {0}", hk.ToString()).str());
            }
          }
          StringRef tokText = SliceBSource(lo, hi).trim();
          trace("macro/tuple",
                "  standard occurrence root id={0} name={1} argIdx={2} "
                "occA=[{3},{4}) aSlice='{5}' baseEnv=[{6},{7}) "
                "extended=[{8},{9}) tok='{10}' expectedFull='{11}' "
                "hunkEffects={12}",
                m.id, m.name, argIdx, s.begin, s.end,
                stringutils::showWSWithClip(SliceASource(s.begin, s.end), 120),
                bEnv->first, bEnv->second, lo, hi,
                stringutils::showWSWithClip(tokText, 120),
                stringutils::showWSWithClip(StringRef(finalNewArg).trim(), 120),
                llvm::join(hunkEffects, " | "));
        }
      }
      return std::nullopt;
    }

    if (isVariadicFormal(argIdx)) {
      StringRef trimmedFinal(finalNewArg);
      trimmedFinal = trimmedFinal.trim();

      // The replay check above validates the edited expansion surface.  For the
      // invocation spelling, however, a variadic formal does not own the fixed
      // separator before it.  If deletion of the first tuple element left that
      // separator at the front of the reconstructed replacement, remove exactly
      // one leading comma so the callsite spells the shortened tuple rather than
      // an empty first variadic argument.
      if (!trimmedFinal.empty() && trimmedFinal.front() == ',') {
        trimmedFinal = trimmedFinal.drop_front();
        while (!trimmedFinal.empty() &&
               (trimmedFinal.front() == ' ' || trimmedFinal.front() == '\t'))
          trimmedFinal = trimmedFinal.drop_front();
        finalNewArg = trimmedFinal.str();
      }
    }

    trace("macro/args", "    consistency OK for argIdx={0}", argIdx);
    replByArgIdx[argIdx] = std::move(finalNewArg);
  }

  // If nothing required replacement, there is no meaningful args-only patch to
  // emit.
  if (replByArgIdx.empty()) {
    trace("macro/args", "  replByArgIdx empty -> no-op args-only");
    return std::nullopt;
  }

  // Apply replacements to the invocation string. We apply in descending argIdx
  // order so earlier replacements cannot shift the byte ranges of later ones in
  // the same baseInvText.
  std::string finalInv = baseInvText.str();
  auto finalKeys = llvm::to_vector(
      llvm::map_range(replByArgIdx, [](auto &e) { return e.first; }));
  std::sort(finalKeys.begin(), finalKeys.end(), [&](uint32_t a, uint32_t b) {
    return invArgRanges[a].first > invArgRanges[b].first;
  });
  for (uint32_t argIdx : finalKeys) {
    auto r = invArgRanges[argIdx];
    finalInv = stringutils::replaceRange(finalInv, r.first, r.second,
                                         replByArgIdx[argIdx]);
  }

  {
    MacroPatch patch{*m.invB, *m.invE, std::move(finalInv), m.id};
    StampMacroPatchProof(patch, MacroPatchProofKind::ArgsOnlyStandard,
                         /*validated=*/true,
                         /*structurePreserving=*/true, m.id);
    return patch;
  }
}

#include "RefoldEngine.SourceMapping.inc"
#include "RefoldEngine.ArgTextRecovery.inc"
#include "RefoldEngine.IncludeInsertion.inc"
#include "RefoldEngine.CounterStabilization.inc"
#include "RefoldEngine.ExpansionFallback.inc"


std::optional<std::pair<uint64_t, uint64_t>>
RefoldEngine::GetWholeCoverATokRange(
    const RefoldModel::MacroInvocation &m) const {
  uint64_t covLoA = m.cover.begin;
  uint64_t covHiA = m.cover.end;

  // For function-like macros with no formal parameters, the producer may
  // conservatively widen the macro cover to include surrounding context (e.g.
  // when the invocation occurs in a nested macro argument). In these cases,
  // bodySpans is the precise expansion slice we want to whole-cover replace.
  if (m.subkind == "func" && m.defParams.empty() && !m.bodySpans.empty()) {
    uint64_t lo = std::numeric_limits<uint64_t>::max();
    uint64_t hi = 0;
    for (const auto &s : m.bodySpans) {
      if (s.begin < s.end) {
        lo = std::min(lo, s.begin);
        hi = std::max(hi, s.end);
      }
    }
    if (lo != std::numeric_limits<uint64_t>::max() && lo < hi) {
      covLoA = lo;
      covHiA = hi;
    }
  }

  if (covLoA >= covHiA)
    return std::nullopt;
  return std::make_pair(covLoA, covHiA);
}

bool RefoldEngine::MacroWholeCoverIsSelfContained(
    const RefoldModel::MacroInvocation &m) const {
  auto range = GetWholeCoverATokRange(m);
  if (!range)
    return false;
  const uint64_t covLoA = range->first;
  const uint64_t covHiA = range->second;

  SmallVector<std::pair<uint64_t, uint64_t>, 16> spans;
  auto appendIntersecting = [&](auto &&src) {
    for (const auto &sp : src) {
      uint64_t b = std::max<uint64_t>(covLoA, sp.begin);
      uint64_t e = std::min<uint64_t>(covHiA, sp.end);
      if (b < e)
        spans.emplace_back(b, e);
    }
  };

  appendIntersecting(m.bodySpans);
  appendIntersecting(m.argSpans);
  appendIntersecting(m.stringifySpans);
  appendIntersecting(m.pasteSpans);

  if (spans.empty())
    return false;

  llvm::sort(spans, [](const auto &a, const auto &b) {
    if (a.first != b.first)
      return a.first < b.first;
    return a.second < b.second;
  });

  uint64_t cur = covLoA;
  for (const auto &sp : spans) {
    if (sp.second <= cur)
      continue;
    if (sp.first > cur)
      return false;
    cur = std::max(cur, sp.second);
    if (cur >= covHiA)
      return true;
  }
  return cur >= covHiA;
}

std::optional<RefoldEngine::WholeCoverPlan>
RefoldEngine::ComputeWholeCoverPlan(
    const RefoldModel::MacroInvocation &m) const {
  auto range = GetWholeCoverATokRange(m);
  if (!range)
    return std::nullopt;

  WholeCoverPlan plan;
  plan.covLoA = range->first;
  plan.covHiA = range->second;
  plan.usedBodyRange =
      (m.subkind == "func" && m.defParams.empty() && !m.bodySpans.empty() &&
       (plan.covLoA != m.cover.begin || plan.covHiA != m.cover.end));
  plan.selfContained = MacroWholeCoverIsSelfContained(m);
  plan.nestedSelfContained = NestedWholeCoverIsSelfContained(m);
  if (!plan.selfContained) {
    trace("macro/whole",
          "whole-cover rejected inv id={0} name='{1}': non-self-contained "
          "cover=[{2},{3}) nestedSelfContained={4}",
          m.id, m.name, plan.covLoA, plan.covHiA,
          plan.nestedSelfContained ? 1 : 0);
    return std::nullopt;
  }

  auto bEnv = MapATokRangeAToBTokenEnvelopePreserveBoundaryInsertions(
      plan.covLoA, plan.covHiA);
  if (!bEnv)
    return std::nullopt;

  plan.rawBTokStart = bEnv->first;
  plan.rawBTokEnd = bEnv->second;
  plan.bTokStart = plan.rawBTokStart;
  plan.bTokEnd = plan.rawBTokEnd;
  if (plan.bTokEnd <= plan.bTokStart)
    return std::nullopt;

  if (plan.covLoA < aToks_.size() && plan.bTokStart < bToks_.size()) {
    StringRef want = aToks_[static_cast<size_t>(plan.covLoA)].spelling;
    if (!want.empty()) {
      if (bToks_[plan.bTokStart].spelling != want && plan.bTokStart > 0 &&
          bToks_[plan.bTokStart - 1].spelling == want) {
        plan.bTokStart--;
        plan.adjustedLeft = true;
      }
    }
  }

  if (plan.covHiA > 0 && (plan.covHiA - 1) < aToks_.size() &&
      plan.bTokEnd > 0 && (plan.bTokEnd - 1) < bToks_.size()) {
    StringRef want = aToks_[static_cast<size_t>(plan.covHiA - 1)].spelling;
    if (!want.empty()) {
      // Do not contract a whole-cover replacement across a trailing B comment.
      // Comments are source trivia, not macro-body delimiter tokens; clipping one
      // out here splits an otherwise line-local insertion and forces a spurious
      // #line resynchronization before the comment text.
      const bool rightIsComment =
          bToks_[plan.bTokEnd - 1].kind == "comment";
      if (!rightIsComment && bToks_[plan.bTokEnd - 1].spelling != want &&
          plan.bTokEnd >= 2 && bToks_[plan.bTokEnd - 2].spelling == want) {
        plan.bTokEnd--;
        plan.adjustedRight = true;
      }
    }
  }

  if (plan.bTokEnd <= plan.bTokStart)
    return std::nullopt;

  std::string unclipped = SliceBSource(plan.bTokStart, plan.bTokEnd).str();
  std::string clipped =
      SliceBSourceClippedAgainstClaims(plan.bTokStart, plan.bTokEnd);
  plan.claimsClipped = (unclipped != clipped);
  plan.clippedText = StringRef(clipped).trim().str();
  return plan;
}

bool RefoldEngine::WholeCoverPatchMatchesPlan(const MacroPatch &patch,
                                              const WholeCoverPlan &plan,
                                              uint64_t rootMacroId) const {
  if (patch.proofKind != MacroPatchProofKind::WholeCoverRealization ||
      patch.structurePreserving || patch.proofRootMacroId != rootMacroId)
    return false;
  return patch.wholeCoverUsedBodyRange == plan.usedBodyRange &&
         patch.wholeCoverSelfContained == plan.selfContained &&
         patch.wholeCoverNestedSelfContained == plan.nestedSelfContained &&
         patch.wholeCoverAdjustedLeft == plan.adjustedLeft &&
         patch.wholeCoverAdjustedRight == plan.adjustedRight &&
         patch.wholeCoverClaimsClipped == plan.claimsClipped &&
         patch.wholeCoverALo == plan.covLoA &&
         patch.wholeCoverAHi == plan.covHiA &&
         patch.wholeCoverBRawLo == plan.rawBTokStart &&
         patch.wholeCoverBRawHi == plan.rawBTokEnd &&
         patch.wholeCoverBAdjLo == plan.bTokStart &&
         patch.wholeCoverBAdjHi == plan.bTokEnd;
}

RefoldEngine::Owner
RefoldEngine::NormalizeHunkOwnerForPatch(StringRef tuPath,
                                         const diffutils::Hunk &h) const {
  Owner owner = ClassifyOwnerWithSegments(tuPath, h);
  const bool mapsToTU = HunkMapsToTU(h.aStart, h.aEnd, tuPath);
  if (mapsToTU)
    return Owner::TU(owner.condArmId);
  if (owner.kind == OwnerKind::Include && owner.includeId)
    return Owner::Include(*owner.includeId, owner.condArmId);
  return Owner::Unknown();
}

bool RefoldEngine::MacroPatchOwnerMatches(const MacroPatch &patch,
                                          const Owner &owner) const {
  if (!patch.ownerCertPresent || patch.ownerMixedWitness)
    return false;
  if (owner.kind == OwnerKind::Unknown)
    return false;

  const uint8_t wantKind = (owner.kind == OwnerKind::TU)
                               ? 1
                               : (owner.kind == OwnerKind::Include ? 2 : 0);
  if (patch.ownerKindCode != wantKind)
    return false;

  const uint64_t wantInclude = owner.includeId.value_or(0);
  if (patch.ownerIncludeIdCert != wantInclude)
    return false;

  if (patch.ownerHasCondArmCert != owner.condArmId.has_value())
    return false;
  if (patch.ownerHasCondArmCert &&
      patch.ownerCondArmIdCert != owner.condArmId.value())
    return false;

  return true;
}

void RefoldEngine::CarryMacroPatchOwnerCertificate(
    MacroPatch &dst, const MacroPatch &src) const {
  dst.ownerCertPresent = src.ownerCertPresent;
  dst.ownerMixedWitness = src.ownerMixedWitness;
  dst.ownerKindCode = src.ownerKindCode;
  dst.ownerIncludeIdCert = src.ownerIncludeIdCert;
  dst.ownerHasCondArmCert = src.ownerHasCondArmCert;
  dst.ownerCondArmIdCert = src.ownerCondArmIdCert;
  dst.ownerWitnessCount = src.ownerWitnessCount;
}

void RefoldEngine::StampMacroPatchOwnerWitness(MacroPatch &patch,
                                               const Owner &owner) const {
  if (owner.kind == OwnerKind::Unknown)
    return;

  const uint8_t kindCode = (owner.kind == OwnerKind::TU)
                               ? 1
                               : (owner.kind == OwnerKind::Include ? 2 : 0);
  const uint64_t includeId = owner.includeId.value_or(0);
  const bool hasCondArm = owner.condArmId.has_value();
  const uint64_t condArmId = hasCondArm ? *owner.condArmId : 0;

  if (!patch.ownerCertPresent) {
    patch.ownerCertPresent = true;
    patch.ownerMixedWitness = false;
    patch.ownerKindCode = kindCode;
    patch.ownerIncludeIdCert = includeId;
    patch.ownerHasCondArmCert = hasCondArm;
    patch.ownerCondArmIdCert = condArmId;
    patch.ownerWitnessCount = 1;
    return;
  }

  ++patch.ownerWitnessCount;
  if (patch.ownerKindCode != kindCode ||
      patch.ownerIncludeIdCert != includeId ||
      patch.ownerHasCondArmCert != hasCondArm ||
      (hasCondArm && patch.ownerCondArmIdCert != condArmId)) {
    patch.ownerMixedWitness = true;
  }
}

RefoldEngine::AcceptancePathInventory
RefoldEngine::InventoryMacroPatchAcceptancePath(const MacroPatch &patch) const {
  switch (patch.proofKind) {
  case MacroPatchProofKind::ArgsOnlyStandard:
    return BuildAcceptancePathInventory(
        AcceptedPathKind::MacroArgsOnlyStandard);
  case MacroPatchProofKind::ArgsOnlyPasteSingle:
    return BuildAcceptancePathInventory(
        AcceptedPathKind::MacroArgsOnlyPasteSingle);
  case MacroPatchProofKind::ArgsOnlyPasteMulti:
    return BuildAcceptancePathInventory(
        AcceptedPathKind::MacroArgsOnlyPasteMulti);
  case MacroPatchProofKind::ArgsOnlyPurePasteOnly:
    return BuildAcceptancePathInventory(
        AcceptedPathKind::MacroArgsOnlyPurePasteOnly);
  case MacroPatchProofKind::ArgsOnlyPairedPureInsertion:
    // Paired pure insertion is only valid on non-paste direct arg/stringify
    // surfaces. The builder already enforces that; Step 6 records it.
    return BuildAcceptancePathInventory(
        AcceptedPathKind::MacroArgsOnlyPairedPureInsertion);
  case MacroPatchProofKind::DagSubtreeRoot:
    // DAG-preserving rewrites must carry the explicit subtree certificate that
    // Step 5 started recording on accepted root patches.
    return BuildAcceptancePathInventory(AcceptedPathKind::MacroDagSubtreeRoot);
  case MacroPatchProofKind::CallChainSuffix:
    // Call-chain suffix rewrites are emitted directly on the root callsite
    // slice, so the patch's owning macro id must already be that root.
    return BuildAcceptancePathInventory(AcceptedPathKind::MacroCallChainSuffix);
  case MacroPatchProofKind::CounterLiteral:
    return BuildAcceptancePathInventory(AcceptedPathKind::MacroCounterLiteral);
  case MacroPatchProofKind::WholeCoverRealization:
    return BuildAcceptancePathInventory(
        AcceptedPathKind::MacroWholeCoverRealization);
  case MacroPatchProofKind::Unknown:
    return BuildAcceptancePathInventory(AcceptedPathKind::Unknown);
  }

  return BuildAcceptancePathInventory(AcceptedPathKind::Unknown);
}

RefoldEngine::AcceptancePathInventory
RefoldEngine::BuildAcceptancePathInventory(AcceptedPathKind currentPath) const {
  AcceptancePathInventory inventory;
  inventory.currentPath = currentPath;

  switch (currentPath) {
  case AcceptedPathKind::MacroArgsOnlyStandard:
    inventory.support = AcceptanceSupportKind::ExplicitProofBacked;
    inventory.futureTarget = FutureProofTarget::MacroStandardArgsOnly;
    break;
  case AcceptedPathKind::MacroArgsOnlyPasteSingle:
    inventory.support = AcceptanceSupportKind::ExplicitProofBacked;
    inventory.futureTarget = FutureProofTarget::MacroPasteSingle;
    break;
  case AcceptedPathKind::MacroArgsOnlyPasteMulti:
    inventory.support = AcceptanceSupportKind::ExplicitProofBacked;
    inventory.futureTarget = FutureProofTarget::MacroPasteMultiFixedAnchor;
    break;
  case AcceptedPathKind::MacroArgsOnlyPurePasteOnly:
    inventory.support = AcceptanceSupportKind::ExplicitProofBacked;
    inventory.futureTarget = FutureProofTarget::MacroPurePasteOnly;
    break;
  case AcceptedPathKind::MacroArgsOnlyPairedPureInsertion:
    inventory.support = AcceptanceSupportKind::ExplicitProofBacked;
    inventory.futureTarget = FutureProofTarget::MacroPairedPureInsertion;
    break;
  case AcceptedPathKind::MacroDagSubtreeRoot:
    inventory.support = AcceptanceSupportKind::ExplicitProofBacked;
    inventory.futureTarget = FutureProofTarget::MacroDagLift;
    break;
  case AcceptedPathKind::MacroCallChainSuffix:
    inventory.support = AcceptanceSupportKind::ExplicitProofBacked;
    inventory.futureTarget =
        FutureProofTarget::MacroCallChainSuffixPreservation;
    break;
  case AcceptedPathKind::MacroCounterLiteral:
    inventory.support = AcceptanceSupportKind::ExplicitProofBacked;
    inventory.futureTarget =
        FutureProofTarget::MacroCounterStabilizationRealization;
    break;
  case AcceptedPathKind::MacroWholeCoverRealization:
    inventory.support = AcceptanceSupportKind::ExplicitProofBacked;
    inventory.futureTarget = FutureProofTarget::MacroRealizationWholeCover;
    break;
  case AcceptedPathKind::IncludePatchPendingMaterialization:
    // Pending include materialization is now an internal-only staging state.
    // Do not expose it as a normalized accepted path with transitional
    // support; any theorem-facing summary that still references this state
    // must fail closed and restamp onto a concrete include class first.
    inventory.currentPath = AcceptedPathKind::Unknown;
    inventory.support = AcceptanceSupportKind::Unknown;
    inventory.futureTarget = FutureProofTarget::Unknown;
    break;
  case AcceptedPathKind::IncludeDeleteReplaceMappedHeaderTokens:
    // Step 8 promotes deterministic include-preserving materialization paths
    // into explicit witness-backed proof classes.
    inventory.support = AcceptanceSupportKind::ExplicitProofBacked;
    inventory.futureTarget =
        FutureProofTarget::IncludePatchByMappedHeaderTokens;
    break;
  case AcceptedPathKind::IncludeInsertSelectedConditionalBoundary:
    inventory.support = AcceptanceSupportKind::ExplicitProofBacked;
    inventory.futureTarget =
        FutureProofTarget::IncludeConditionalArmCertifiedInsertion;
    break;
  case AcceptedPathKind::IncludeInsertChildBoundary:
    inventory.support = AcceptanceSupportKind::ExplicitProofBacked;
    inventory.futureTarget = FutureProofTarget::IncludeInsertionByChildBoundary;
    break;
  case AcceptedPathKind::IncludeInsertRightNeighborPP:
    inventory.support = AcceptanceSupportKind::ExplicitProofBacked;
    inventory.futureTarget =
        FutureProofTarget::IncludeInsertionByRightNeighborPP;
    break;
  case AcceptedPathKind::IncludeInsertLeftNeighborPP:
    inventory.support = AcceptanceSupportKind::ExplicitProofBacked;
    inventory.futureTarget =
        FutureProofTarget::IncludeInsertionByLeftNeighborPP;
    break;
  case AcceptedPathKind::IncludeInsertDeclBoundary:
    inventory.support = AcceptanceSupportKind::ExplicitProofBacked;
    inventory.futureTarget = FutureProofTarget::IncludeInsertionByDeclBoundary;
    break;
  case AcceptedPathKind::IncludeRealizationInlineFromB:
    // Step 4A narrows the declared include-realization domain explicitly:
    // this first-class path exists only when the include cover carries a
    // canonical or deterministic-consensus B-envelope witness.
    inventory.support = AcceptanceSupportKind::ExplicitProofBacked;
    inventory.futureTarget = FutureProofTarget::IncludeRealizationCover;
    break;
  case AcceptedPathKind::IncludeMaterializedExpansion:
    // Step 3 closes the remaining emitted transitional gap for recursively
    // materialized include expansions. By the time this path reaches an
    // emitted parent/TU edit, step 2A has attached an explicit normalized
    // carrier and step 2B has made its discharge record behaviorally
    // authoritative at the emission boundary.
    inventory.support = AcceptanceSupportKind::ExplicitProofBacked;
    inventory.futureTarget =
        FutureProofTarget::IncludeMaterializedExpansionRealization;
    break;
  case AcceptedPathKind::TUExactSlotBoundary:
    // Step 7 formalizes exact slot anchors as first-class TU anchor proofs.
    inventory.support = AcceptanceSupportKind::ExplicitProofBacked;
    inventory.futureTarget = FutureProofTarget::TUExactSlotAnchor;
    break;
  case AcceptedPathKind::TUProvableInsertionAnchor:
    // Step 7 likewise promotes deterministic non-slot TU insertion anchors
    // into explicit proof-backed paths once they carry a local witness.
    inventory.support = AcceptanceSupportKind::ExplicitProofBacked;
    inventory.futureTarget = FutureProofTarget::TUProvableInsertionAnchor;
    break;
  case AcceptedPathKind::TUByteSpanMappedEdit:
  case AcceptedPathKind::TUByteSpanConservativeEdit:
    // Step 3 closes the remaining emitted transitional gap for direct TU byte
    // edits. These paths now participate as explicit proof-backed TU textual
    // realizations because step 2A attaches normalized carriers and step 2B
    // enforces their discharge at the byte-edit emission boundary.
    inventory.support = AcceptanceSupportKind::ExplicitProofBacked;
    inventory.futureTarget = FutureProofTarget::TUByteSpanTextualEdit;
    break;
  case AcceptedPathKind::TerminalEmitEditedPreprocessedStream:
    inventory.support = AcceptanceSupportKind::ExplicitOutOfDomainClass;
    inventory.futureTarget =
        FutureProofTarget::EditedPreprocessedStreamFallback;
    break;
  case AcceptedPathKind::Unknown:
    break;
  }

  return inventory;
}

/// \brief Step-3 accumulator implementation for class-local obligations.
///
/// The helper is defined out of line so RefoldEngine.cpp can reuse one piece
/// of deterministic bookkeeping across macro, include, and TU proof families
/// without exposing the Step-3 discharge mechanics outside RefoldEngine.
struct RefoldEngine::ProofDischargeAccumulator {
  ProofDischargeRecord record;

  explicit ProofDischargeAccumulator(
      ProofDischargeStatus initialStatus = ProofDischargeStatus::Unknown) {
    record.status = initialStatus;
  }

  void Satisfy(ProofObligationKind obligation) {
    (void)obligation;
    ++record.obligationsEvaluated;
    ++record.obligationsSatisfied;
    if (record.status == ProofDischargeStatus::Unknown)
      record.status = ProofDischargeStatus::Discharged;
  }

  void Fail(ProofObligationKind obligation, ProofFailureReason reason) {
    ++record.obligationsEvaluated;
    if (record.failedObligation == ProofObligationKind::Unknown)
      record.failedObligation = obligation;
    if (record.failureReason == ProofFailureReason::None)
      record.failureReason = reason;
    record.status = ProofDischargeStatus::Rejected;
  }

  void Require(bool condition, ProofObligationKind obligation,
               ProofFailureReason reason) {
    if (condition)
      Satisfy(obligation);
    else
      Fail(obligation, reason);
  }

  ProofDischargeRecord Finish() {
    if (record.status == ProofDischargeStatus::Unknown)
      record.status = ProofDischargeStatus::Discharged;
    return record;
  }
};

RefoldEngine::ProofSummary
RefoldEngine::ClassifyMacroPatchProof(const MacroPatch &patch) const {
  ProofSummary summary;
  summary.validated = patch.proofValidated;
  summary.structurePreserving = patch.structurePreserving;
  summary.proofRootMacroId = patch.proofRootMacroId;
  summary.inventory = InventoryMacroPatchAcceptancePath(patch);

  switch (patch.proofKind) {
  case MacroPatchProofKind::ArgsOnlyPasteMulti:
  case MacroPatchProofKind::ArgsOnlyPasteSingle:
  case MacroPatchProofKind::ArgsOnlyPurePasteOnly:
  case MacroPatchProofKind::ArgsOnlyStandard:
  case MacroPatchProofKind::ArgsOnlyPairedPureInsertion:
    // Paired pure insertion is only valid on non-paste direct arg/stringify
    // surfaces. The builder already enforces that; Step 6 records it.
  case MacroPatchProofKind::DagSubtreeRoot:
    // DAG-preserving rewrites must carry the explicit subtree certificate that
    // Step 5 started recording on accepted root patches.
  case MacroPatchProofKind::CallChainSuffix:
    // Call-chain suffix rewrites are emitted directly on the root callsite
    // slice, so the patch's owning macro id must already be that root.
    summary.acceptedClass = AcceptedProofClass::InvocationPreserving;
    summary.realizationMode = RealizationMode::PreserveOriginalStructure;
    summary.preference = SelectionPreference::PreferStructurePreservation;
    break;

  case MacroPatchProofKind::CounterLiteral:
    summary.acceptedClass = AcceptedProofClass::InvocationRealization;
    summary.realizationMode = RealizationMode::RealizeEditedSurface;
    summary.preference = SelectionPreference::PreferSurfaceRealization;
    break;

  case MacroPatchProofKind::WholeCoverRealization:
    summary.acceptedClass = AcceptedProofClass::InvocationRealization;
    summary.realizationMode = RealizationMode::RealizeEditedSurface;
    summary.preference = SelectionPreference::PreferSurfaceRealization;
    summary.surfaceDisposition =
        SurfaceDisposition::RealizeWholeCoverMacros;
    break;

  case MacroPatchProofKind::Unknown:
    if (patch.structurePreserving) {
      summary.acceptedClass = AcceptedProofClass::InvocationPreserving;
      summary.realizationMode = RealizationMode::PreserveOriginalStructure;
      summary.preference = SelectionPreference::PreferStructurePreservation;
    } else if (patch.proofValidated || patch.proofRootMacroId) {
      summary.acceptedClass = AcceptedProofClass::InvocationRealization;
      summary.realizationMode = RealizationMode::RealizeEditedSurface;
      summary.preference = SelectionPreference::PreferSurfaceRealization;
    }
    break;
  }

  switch (summary.acceptedClass) {
  case AcceptedProofClass::InvocationPreserving:
    summary.discharge = ValidateInvocationPreservingProof(patch);
    break;
  case AcceptedProofClass::InvocationRealization:
    summary.discharge = ValidateInvocationRealizationProof(patch);
    break;
  case AcceptedProofClass::Unknown:
  case AcceptedProofClass::IncludePreserving:
  case AcceptedProofClass::IncludeRealization:
  case AcceptedProofClass::TUAnchor:
  case AcceptedProofClass::TUTextualEdit:
    break;
  }

  summary.lattice = BuildGlobalSelectionLattice(summary);
  summary.completeness = BuildCompletenessContract(summary);
  summary.theoremDomain = BuildTheoremDomainContract(summary);
  return summary;
}

void RefoldEngine::SyncMacroPatchProofSummary(MacroPatch &patch) const {
  patch.proofSummary = ClassifyMacroPatchProof(patch);
}

void RefoldEngine::StampMacroPatchProof(MacroPatch &patch,
                                        MacroPatchProofKind kind,
                                        bool validated,
                                        bool structurePreserving,
                                        uint64_t proofRootMacroId) const {
  patch.proofKind = kind;
  patch.proofValidated = validated;
  patch.structurePreserving = structurePreserving;
  patch.proofRootMacroId = proofRootMacroId;
  SyncMacroPatchProofSummary(patch);
}

void RefoldEngine::StampMacroWholeCoverRealizationPatch(
    MacroPatch &patch, const WholeCoverPlan &plan,
    uint64_t proofRootMacroId) const {
  // Step 4 promotes accepted whole-cover output into an explicit invocation
  // realization proof. The plan already carries the exact A/B token envelope
  // and containment facts, so stamping it here keeps the accepted patch
  // deterministic and fully described without changing selection behavior.
  StampMacroPatchProof(patch, MacroPatchProofKind::WholeCoverRealization,
                       /*validated=*/true,
                       /*structurePreserving=*/false, proofRootMacroId);
  patch.wholeCoverUsedBodyRange = plan.usedBodyRange;
  patch.wholeCoverSelfContained = plan.selfContained;
  patch.wholeCoverNestedSelfContained = plan.nestedSelfContained;
  patch.wholeCoverAdjustedLeft = plan.adjustedLeft;
  patch.wholeCoverAdjustedRight = plan.adjustedRight;
  patch.wholeCoverClaimsClipped = plan.claimsClipped;
  patch.wholeCoverALo = plan.covLoA;
  patch.wholeCoverAHi = plan.covHiA;
  patch.wholeCoverBRawLo = plan.rawBTokStart;
  patch.wholeCoverBRawHi = plan.rawBTokEnd;
  patch.wholeCoverBAdjLo = plan.bTokStart;
  patch.wholeCoverBAdjHi = plan.bTokEnd;
}

RefoldEngine::ProofSummary RefoldEngine::BuildAcceptedPathProofSummary(
    AcceptedPathKind currentPath, const IncludePatch *patch,
    const TUAnchorWitness *tuAnchorWitness,
    const IncludeAnchorWitness *includeAnchorWitness,
    const IncludeRealizationWitness *includeRealizationWitness,
    const TerminalFallbackWitness *terminalFallbackWitness) const {
  ProofSummary summary;

  summary.inventory = BuildAcceptancePathInventory(currentPath);

  switch (currentPath) {
  case AcceptedPathKind::IncludePatchPendingMaterialization:
    // Pending include materialization is an internal working state, not a
    // theorem-facing accepted path. Keep the switch exhaustive so enum
    // coverage remains explicit under -Wswitch, but fail closed here by
    // returning the default/empty summary rather than manufacturing a
    // transitional accepted carrier.
    return summary;
  case AcceptedPathKind::IncludeDeleteReplaceMappedHeaderTokens:
  case AcceptedPathKind::IncludeInsertSelectedConditionalBoundary:
  case AcceptedPathKind::IncludeInsertChildBoundary:
  case AcceptedPathKind::IncludeInsertRightNeighborPP:
  case AcceptedPathKind::IncludeInsertLeftNeighborPP:
  case AcceptedPathKind::IncludeInsertDeclBoundary:
    summary.acceptedClass = AcceptedProofClass::IncludePreserving;
    summary.realizationMode = RealizationMode::PreserveOriginalStructure;
    summary.preference = SelectionPreference::PreferStructurePreservation;
    summary.structurePreserving = true;
    if (includeAnchorWitness) {
      summary.hasIncludeAnchorWitness = true;
      summary.includeAnchorWitness = *includeAnchorWitness;
    }
    summary.discharge = ValidateIncludePreservingProof(currentPath, patch,
                                                       includeAnchorWitness);
    break;

  case AcceptedPathKind::IncludeRealizationInlineFromB:
    summary.acceptedClass = AcceptedProofClass::IncludeRealization;
    summary.realizationMode = RealizationMode::RealizeEditedSurface;
    summary.preference = SelectionPreference::PreferSurfaceRealization;
    summary.surfaceDisposition =
        SurfaceDisposition::RealizeInlineTouchedIncludesFromB;
    summary.structurePreserving = false;
    if (includeRealizationWitness) {
      summary.hasIncludeRealizationWitness = true;
      summary.includeRealizationWitness = *includeRealizationWitness;
    }
    summary.discharge = ValidateIncludeRealizationProof(
        currentPath, patch, includeRealizationWitness);
    break;

  case AcceptedPathKind::IncludeMaterializedExpansion: {
    summary.acceptedClass = AcceptedProofClass::IncludeRealization;
    summary.realizationMode = RealizationMode::RealizeEditedSurface;
    summary.preference = SelectionPreference::PreferSurfaceRealization;
    summary.surfaceDisposition =
        SurfaceDisposition::RealizeMaterializedIncludeExpansion;
    summary.structurePreserving = false;
    ProofDischargeAccumulator discharge;
    discharge.Require(summary.inventory.currentPath !=
                          AcceptedPathKind::Unknown,
                      ProofObligationKind::AcceptedPathClassified,
                      ProofFailureReason::MissingAcceptedPathClassification);
    discharge.Require(summary.inventory.futureTarget !=
                          FutureProofTarget::Unknown,
                      ProofObligationKind::FutureTargetMapped,
                      ProofFailureReason::MissingFutureTargetMapping);
    summary.discharge = discharge.Finish();
    break;
  }

  case AcceptedPathKind::TUExactSlotBoundary:
  case AcceptedPathKind::TUProvableInsertionAnchor:
    summary.acceptedClass = AcceptedProofClass::TUAnchor;
    summary.realizationMode = RealizationMode::PreserveOriginalStructure;
    summary.preference = SelectionPreference::PreferExactAnchoring;
    summary.structurePreserving = true;
    if (tuAnchorWitness) {
      summary.hasTUAnchorWitness = true;
      summary.tuAnchorWitness = *tuAnchorWitness;
    }
    summary.discharge = ValidateTUAnchorProof(currentPath, tuAnchorWitness);
    break;

  case AcceptedPathKind::TUByteSpanMappedEdit:
  case AcceptedPathKind::TUByteSpanConservativeEdit: {
    summary.acceptedClass = AcceptedProofClass::TUTextualEdit;
    summary.realizationMode = RealizationMode::RealizeEditedSurface;
    summary.preference = SelectionPreference::PreferSurfaceRealization;
    summary.surfaceDisposition =
        SurfaceDisposition::RealizeTranslationUnitByteEdit;
    summary.structurePreserving = false;
    ProofDischargeAccumulator discharge;
    discharge.Require(summary.inventory.currentPath !=
                          AcceptedPathKind::Unknown,
                      ProofObligationKind::AcceptedPathClassified,
                      ProofFailureReason::MissingAcceptedPathClassification);
    discharge.Require(summary.inventory.futureTarget !=
                          FutureProofTarget::Unknown,
                      ProofObligationKind::FutureTargetMapped,
                      ProofFailureReason::MissingFutureTargetMapping);
    summary.discharge = discharge.Finish();
    break;
  }

  case AcceptedPathKind::TerminalEmitEditedPreprocessedStream: {
    if (terminalFallbackWitness) {
      summary.hasTerminalFallbackWitness = true;
      summary.terminalFallbackWitness = *terminalFallbackWitness;
    }
    summary.realizationMode = RealizationMode::RealizeEditedSurface;
    summary.preference = SelectionPreference::PreferSurfaceRealization;
    summary.surfaceDisposition =
        SurfaceDisposition::EmitEditedPreprocessedStream;
    ProofDischargeAccumulator discharge;
    discharge.Require(summary.inventory.currentPath !=
                          AcceptedPathKind::Unknown,
                      ProofObligationKind::AcceptedPathClassified,
                      ProofFailureReason::MissingAcceptedPathClassification);
    discharge.Require(summary.inventory.futureTarget !=
                          FutureProofTarget::Unknown,
                      ProofObligationKind::FutureTargetMapped,
                      ProofFailureReason::MissingFutureTargetMapping);
    discharge.Fail(ProofObligationKind::ExplicitOutOfDomainResultTracked,
                   ProofFailureReason::ExplicitOutOfDomainResult);
    summary.discharge = discharge.Finish();
    break;
  }

  case AcceptedPathKind::Unknown:
  case AcceptedPathKind::MacroArgsOnlyStandard:
  case AcceptedPathKind::MacroArgsOnlyPasteSingle:
  case AcceptedPathKind::MacroArgsOnlyPasteMulti:
  case AcceptedPathKind::MacroArgsOnlyPurePasteOnly:
  case AcceptedPathKind::MacroArgsOnlyPairedPureInsertion:
  case AcceptedPathKind::MacroDagSubtreeRoot:
  case AcceptedPathKind::MacroCallChainSuffix:
  case AcceptedPathKind::MacroCounterLiteral:
  case AcceptedPathKind::MacroWholeCoverRealization:
    break;
  }

  summary.lattice = BuildGlobalSelectionLattice(summary);
  summary.completeness = BuildCompletenessContract(summary);
  summary.theoremDomain = BuildTheoremDomainContract(summary);
  return summary;
}

RefoldEngine::ProofSummary
RefoldEngine::BuildIncludePatchProofSummary(
    bool realizedSurface, AcceptedPathKind currentPath,
    const IncludePatch *patch) const {
  (void)realizedSurface;
  (void)patch;

  // IncludePatch is a pre-materialization working object. By default it does
  // not claim any normalized accepted path at all; only the later
  // witness-backed materialization step may mint theorem-facing include
  // preserving or realization summaries. If a caller explicitly provides a
  // concrete include path, reuse the accepted-path classifier for that final
  // restamped state.
  if (currentPath == AcceptedPathKind::Unknown)
    return ProofSummary{};

  ProofSummary summary = BuildAcceptedPathProofSummary(currentPath, patch);
  summary.validated = false;
  return summary;
}

RefoldEngine::GlobalSelectionLattice
RefoldEngine::BuildGlobalSelectionLattice(const ProofSummary &summary) const {
  GlobalSelectionLattice lattice;

  // The global lattice names the owner domain, compatible-merge rule, and
  // incompatible-conflict rule that accepted results obey. Converted selector
  // sites already compare candidates through this lattice directly; the
  // remaining sites are expected to converge on the same law.
  switch (summary.acceptedClass) {
  case AcceptedProofClass::InvocationPreserving:
  case AcceptedProofClass::InvocationRealization:
    lattice.domain = LatticeConflictDomain::MacroInvocationRootSpan;
    lattice.mergeLaw = LatticeMergeLaw::NestedOuterShadowsInner;
    lattice.conflictLaw =
        summary.realizationMode == RealizationMode::PreserveOriginalStructure
            ? LatticeConflictLaw::PreferStructurePreservation
            : LatticeConflictLaw::RejectPartialOverlap;
    break;

  case AcceptedProofClass::IncludePreserving:
    lattice.domain = LatticeConflictDomain::IncludeOwnerRegion;
    lattice.mergeLaw = LatticeMergeLaw::DisjointCompose;
    lattice.conflictLaw =
        LatticeConflictLaw::PreferOwnerPreservingBeforeRealization;
    break;

  case AcceptedProofClass::IncludeRealization:
    lattice.domain = LatticeConflictDomain::IncludeOwnerRegion;
    lattice.mergeLaw = LatticeMergeLaw::SelectSingleWitness;
    lattice.conflictLaw =
        LatticeConflictLaw::PreferOwnerPreservingBeforeRealization;
    break;

  case AcceptedProofClass::TUAnchor:
    lattice.domain = LatticeConflictDomain::TUAnchorPoint;
    lattice.mergeLaw = LatticeMergeLaw::SelectSingleWitness;
    lattice.conflictLaw = LatticeConflictLaw::PreferExactAnchorWitness;
    break;

  case AcceptedProofClass::TUTextualEdit:
    lattice.domain = LatticeConflictDomain::WholeTranslationUnit;
    lattice.mergeLaw = LatticeMergeLaw::DisjointCompose;
    lattice.conflictLaw = LatticeConflictLaw::RejectPartialOverlap;
    break;

  case AcceptedProofClass::Unknown:
    break;
  }

  if (summary.inventory.currentPath ==
      AcceptedPathKind::TerminalEmitEditedPreprocessedStream) {
    lattice.domain = LatticeConflictDomain::WholeTranslationUnit;
    lattice.mergeLaw = LatticeMergeLaw::TerminalReplacesAll;
    lattice.conflictLaw = LatticeConflictLaw::ExplicitOutOfDomainTerminalResult;
  }

  return lattice;
}

RefoldEngine::CompletenessContract
RefoldEngine::BuildCompletenessContract(const ProofSummary &summary) const {
  CompletenessContract contract;

  // Step 7 fixes the declared domain explicitly. For theorem-facing summaries,
  // completeness is measured only relative to carriers that belong to the
  // declared proof-class set. Internal staging states may still exist while an
  // object is being materialized, but they must not survive to emission.
  // Anything that cannot be represented as a declared, explicit-proof-backed,
  // locally discharged, lattice-resolved in-domain carrier instead becomes a
  // named explicit out-of-domain boundary.
  if (summary.inventory.currentPath ==
          AcceptedPathKind::TerminalEmitEditedPreprocessedStream ||
      summary.inventory.support ==
          AcceptanceSupportKind::ExplicitOutOfDomainClass) {
    contract.coverage = CompletenessCoverageKind::ExplicitOutOfDomainClass;
    contract.expectation =
        CompletenessExpectationKind::ExplicitlyOutsideDeclaredSet;
    if (summary.hasTerminalFallbackWitness) {
      contract.hasExplicitExclusion = true;
      contract.explicitExclusion = summary.terminalFallbackWitness.kind;
    }
    return contract;
  }

  if (summary.inventory.futureTarget != FutureProofTarget::Unknown &&
      summary.inventory.support ==
          AcceptanceSupportKind::ExplicitProofBacked) {
    contract.coverage = CompletenessCoverageKind::DeclaredProofClass;
    contract.expectation =
        CompletenessExpectationKind::MustDiscoverDeclaredOrStrongerCompatible;
    contract.declaredTarget = summary.inventory.futureTarget;
    contract.countsTowardDeclaredCoverage = true;
    return contract;
  }

  if (summary.inventory.currentPath != AcceptedPathKind::Unknown) {
    contract.coverage = CompletenessCoverageKind::TransitionalGap;
    contract.expectation =
        CompletenessExpectationKind::NoClaimPendingClassClosure;
    contract.declaredTarget = summary.inventory.futureTarget;
    return contract;
  }

  return contract;
}

RefoldEngine::TheoremDomainContract
RefoldEngine::BuildTheoremDomainContract(const ProofSummary &summary) const {
  TheoremDomainContract contract;

  // Step 7 requires theorem-domain reporting, completeness reporting, and the
  // theorem audit to say the same thing. This helper remains derived-only: it
  // does not introduce new acceptance behavior, it only restates whether the
  // summary is in-domain, transitional-internal, or an explicit named
  // out-of-domain class under the same declared-domain contract.
  switch (summary.completeness.coverage) {
  case CompletenessCoverageKind::DeclaredProofClass:
    contract.kind = TheoremDomainKind::DeclaredInDomainClass;
    contract.inDeclaredDomain = true;
    contract.countsTowardCompleteness =
        summary.completeness.countsTowardDeclaredCoverage;
    contract.declaredTarget = summary.completeness.declaredTarget;
    break;

  case CompletenessCoverageKind::TransitionalGap:
    contract.kind = TheoremDomainKind::TransitionalGap;
    contract.declaredTarget = summary.completeness.declaredTarget;
    break;

  case CompletenessCoverageKind::ExplicitOutOfDomainClass:
    contract.kind = TheoremDomainKind::ExplicitOutOfDomainClass;
    contract.hasExplicitExclusion =
        summary.completeness.hasExplicitExclusion;
    contract.explicitExclusion = summary.completeness.explicitExclusion;
    contract.declaredTarget = summary.completeness.declaredTarget;
    break;

  case CompletenessCoverageKind::Unknown:
    break;
  }

  return contract;
}

bool RefoldEngine::LatticePrefers(const ProofSummary &lhs,
                                  const ProofSummary &rhs) const {
  auto preferenceRank = [](SelectionPreference preference) -> uint8_t {
    switch (preference) {
    case SelectionPreference::PreferExactAnchoring:
      return 0;
    case SelectionPreference::PreferStructurePreservation:
      return 1;
    case SelectionPreference::PreferSurfaceRealization:
      return 2;
    case SelectionPreference::Unknown:
      return 3;
    }
    return 3;
  };

  auto surfaceDispositionRank = [](SurfaceDisposition disposition) -> uint8_t {
    switch (disposition) {
    case SurfaceDisposition::None:
      return 0;
    case SurfaceDisposition::RealizeWholeCoverMacros:
      return 1;
    case SurfaceDisposition::RealizeInlineTouchedIncludesFromB:
      return 2;
    case SurfaceDisposition::RealizeMaterializedIncludeExpansion:
      return 3;
    case SurfaceDisposition::RealizeTranslationUnitByteEdit:
      return 4;
    case SurfaceDisposition::EmitEditedPreprocessedStream:
      return 5;
    }
    return 5;
  };

  const uint8_t lhsPreference = preferenceRank(lhs.preference);
  const uint8_t rhsPreference = preferenceRank(rhs.preference);
  if (lhsPreference != rhsPreference)
    return lhsPreference < rhsPreference;

  const uint8_t lhsSurfaceDisposition =
      surfaceDispositionRank(lhs.surfaceDisposition);
  const uint8_t rhsSurfaceDisposition =
      surfaceDispositionRank(rhs.surfaceDisposition);
  if (lhsSurfaceDisposition != rhsSurfaceDisposition)
    return lhsSurfaceDisposition < rhsSurfaceDisposition;

  if (lhs.acceptedClass != rhs.acceptedClass)
    return static_cast<uint8_t>(lhs.acceptedClass) <
           static_cast<uint8_t>(rhs.acceptedClass);

  return static_cast<uint8_t>(lhs.inventory.currentPath) <
         static_cast<uint8_t>(rhs.inventory.currentPath);
}

bool RefoldEngine::IsSelectableAcceptedResultCandidate(
    const AcceptedResultCandidate &candidate) const {
  if (candidate.kind == AcceptedResultCandidateKind::Unknown)
    return false;

  const ProofDischargeRecord &discharge = candidate.proofSummary.discharge;
  if (discharge.status == ProofDischargeStatus::Discharged)
    return true;

  // Patch C makes discharge the participation gate for converted selector
  // sites, but the one explicit terminal out-of-domain result is still allowed
  // to flow through the normalized candidate carrier so the terminal theorem
  // boundary stays named and auditable.
  return candidate.kind == AcceptedResultCandidateKind::TerminalOutOfDomain &&
         discharge.status == ProofDischargeStatus::Rejected &&
         discharge.failedObligation ==
             ProofObligationKind::ExplicitOutOfDomainResultTracked &&
         discharge.failureReason ==
             ProofFailureReason::ExplicitOutOfDomainResult;
}

bool RefoldEngine::
    AcceptedResultCandidateHasOnlyNonTopLevelMacroSelectorFailure(
        const AcceptedResultCandidate &candidate) const {
  if (candidate.kind != AcceptedResultCandidateKind::MacroPatch)
    return false;

  const ProofDischargeRecord &discharge = candidate.proofSummary.discharge;
  return discharge.status == ProofDischargeStatus::Rejected &&
         discharge.failedObligation ==
             ProofObligationKind::MacroProofRootIsTopLevel &&
         discharge.failureReason ==
             ProofFailureReason::NonTopLevelMacroProofRoot;
}

bool RefoldEngine::AcceptedResultCandidatePrefers(
    const AcceptedResultCandidate &lhs,
    const AcceptedResultCandidate &rhs) const {
  if (LatticePrefers(lhs.proofSummary, rhs.proofSummary))
    return true;
  if (LatticePrefers(rhs.proofSummary, lhs.proofSummary))
    return false;

  // The lattice intentionally stays coarse. When two summaries tie, prefer the
  // candidate that is more specific about the concrete artifact it will emit.
  // This keeps the converted sites deterministic without reintroducing ad hoc
  // path-specific ordering logic.
  if (lhs.kind != rhs.kind)
    return static_cast<uint8_t>(lhs.kind) < static_cast<uint8_t>(rhs.kind);

  if (lhs.begin != rhs.begin)
    return lhs.begin < rhs.begin;
  if (lhs.end != rhs.end)
    return lhs.end < rhs.end;

  if (lhs.hasRootMacroId != rhs.hasRootMacroId)
    return lhs.hasRootMacroId;
  if (lhs.hasRootMacroId && lhs.rootMacroId != rhs.rootMacroId)
    return lhs.rootMacroId < rhs.rootMacroId;

  if (lhs.hasOwnerIncludeId != rhs.hasOwnerIncludeId)
    return lhs.hasOwnerIncludeId;
  if (lhs.hasOwnerIncludeId && lhs.ownerIncludeId != rhs.ownerIncludeId)
    return lhs.ownerIncludeId < rhs.ownerIncludeId;

  if (lhs.hasAnchorByte != rhs.hasAnchorByte)
    return lhs.hasAnchorByte;
  if (lhs.hasAnchorByte && lhs.anchorByte != rhs.anchorByte)
    return lhs.anchorByte < rhs.anchorByte;

  return false;
}

std::optional<size_t> RefoldEngine::SelectPreferredAcceptedResultCandidateIndex(
    ArrayRef<AcceptedResultCandidate> candidates,
    bool allowNonTopLevelMacroSelectorFailure) const {
  std::optional<size_t> bestIdx;
  uint64_t selectableCount = 0;

  // The caller still controls enumeration order. This selector only filters
  // that list to candidates whose proof summaries are allowed to participate,
  // then applies the normalized lattice/tie-break ordering while preserving
  // stable caller order when the candidates remain indistinguishable. One
  // internal macro-construction site may additionally admit nested
  // structure-preserving macro artifacts whose only remaining failed
  // obligation is the top-level proof-root selector rule; this removes the
  // last direct selector bypass without relaxing the theorem-facing discharge
  // gate at any emitted boundary.

  for (size_t i = 0; i < candidates.size(); ++i) {
    const bool selectable =
        IsSelectableAcceptedResultCandidate(candidates[i]) ||
        (allowNonTopLevelMacroSelectorFailure &&
         AcceptedResultCandidateHasOnlyNonTopLevelMacroSelectorFailure(
             candidates[i]));
    if (!selectable)
      continue;
    ++selectableCount;
    if (!bestIdx) {
      bestIdx = i;
      continue;
    }
    if (AcceptedResultCandidatePrefers(candidates[i], candidates[*bestIdx]))
      bestIdx = i;
  }

  if (selectableCount > 1) {
    ++lastTheoremAudit_.selectorCompetitions;
    if (bestIdx)
      ++lastTheoremAudit_.selectorResolutions;
  } else if (!bestIdx && !candidates.empty()) {
    ++lastTheoremAudit_.selectorNoSelectable;
    if (candidates.size() > 1)
      ++lastTheoremAudit_.selectorUnresolvedCompetitions;
  }

  return bestIdx;
}

RefoldEngine::AcceptedResultCandidate
RefoldEngine::BuildAcceptedMacroCandidate(const MacroPatch &patch) const {
  AcceptedResultCandidate candidate;
  candidate.kind = AcceptedResultCandidateKind::MacroPatch;
  candidate.proofSummary = ClassifyMacroPatchProof(patch);
  candidate.begin = patch.invStart;
  candidate.end = patch.invEnd;
  if (patch.proofRootMacroId) {
    candidate.hasRootMacroId = true;
    candidate.rootMacroId = patch.proofRootMacroId;
  }
  candidate.hasPayloadPreview = true;
  candidate.payloadPreview =
      stringutils::showWSWithClip(patch.replacement, 120);
  return candidate;
}

RefoldEngine::AcceptedResultCandidate
RefoldEngine::BuildAcceptedEmittedMacroCandidate(
    const MacroPatch &patch) const {
  AcceptedResultCandidate candidate = BuildAcceptedMacroCandidate(patch);

  // Step 2 removes the byte-edit boundary's selector-only nested-macro
  // exception by restamping emitted preserving macro artifacts onto the
  // emission-specific discharge rule. Selector competition still uses the
  // stronger top-level proof-root contract through
  // BuildAcceptedMacroCandidate().
  if (candidate.kind == AcceptedResultCandidateKind::MacroPatch &&
      candidate.proofSummary.acceptedClass ==
          AcceptedProofClass::InvocationPreserving &&
      AcceptedResultCandidateHasOnlyNonTopLevelMacroSelectorFailure(
          candidate)) {
    candidate.proofSummary.discharge =
        ValidateEmittedInvocationPreservingProof(patch);
    candidate.proofSummary.lattice =
        BuildGlobalSelectionLattice(candidate.proofSummary);
    candidate.proofSummary.completeness =
        BuildCompletenessContract(candidate.proofSummary);
    candidate.proofSummary.theoremDomain =
        BuildTheoremDomainContract(candidate.proofSummary);
  }

  return candidate;
}

RefoldEngine::AcceptedResultCandidate
RefoldEngine::BuildAcceptedIncludeCandidate(
    AcceptedPathKind currentPath, const IncludePatch &patch,
    const IncludeAnchorWitness *includeAnchorWitness,
    const IncludeRealizationWitness *includeRealizationWitness) const {
  AcceptedResultCandidate candidate;
  candidate.kind = AcceptedResultCandidateKind::IncludePatch;
  candidate.proofSummary = BuildAcceptedPathProofSummary(
      currentPath, &patch, /*tuAnchorWitness=*/nullptr, includeAnchorWitness,
      includeRealizationWitness);
  candidate.begin = patch.aStart;
  candidate.end = patch.aEnd;
  if (patch.include) {
    candidate.hasOwnerIncludeId = true;
    candidate.ownerIncludeId = patch.include->id;
  }
  if (includeAnchorWitness && includeAnchorWitness->hasAnchorByte) {
    candidate.hasAnchorByte = true;
    candidate.anchorByte = includeAnchorWitness->anchorByte;
  }
  if (includeRealizationWitness && includeRealizationWitness->hasIncludeId) {
    candidate.hasOwnerIncludeId = true;
    candidate.ownerIncludeId = includeRealizationWitness->includeId;
  }
  candidate.hasPayloadPreview = true;
  candidate.payloadPreview =
      stringutils::showWSWithClip(patch.insertBytes, 120);
  return candidate;
}

RefoldEngine::AcceptedResultCandidate
RefoldEngine::BuildAcceptedIncludeRealizationCandidate(
    AcceptedPathKind currentPath, const RefoldModel::IncludeItem &include,
    const IncludeRealizationWitness *includeRealizationWitness) const {
  AcceptedResultCandidate candidate;
  candidate.kind = AcceptedResultCandidateKind::IncludePatch;
  candidate.proofSummary = BuildAcceptedPathProofSummary(
      currentPath, /*patch=*/nullptr, /*tuAnchorWitness=*/nullptr,
      /*includeAnchorWitness=*/nullptr, includeRealizationWitness);
  candidate.hasOwnerIncludeId = true;
  candidate.ownerIncludeId = include.id;
  candidate.begin = include.siteB;
  candidate.end = include.siteE;
  candidate.hasPayloadPreview = true;
  candidate.payloadPreview = formatv("{0}", currentPath).str();
  return candidate;
}

RefoldEngine::AcceptedResultCandidate
RefoldEngine::BuildAcceptedTUTextEditCandidate(
    AcceptedPathKind currentPath, uint64_t begin, uint64_t end,
    StringRef payloadPreview) const {
  AcceptedResultCandidate candidate;
  candidate.kind = AcceptedResultCandidateKind::TUTextEdit;
  candidate.proofSummary = BuildAcceptedPathProofSummary(
      currentPath, /*patch=*/nullptr, /*tuAnchorWitness=*/nullptr,
      /*includeAnchorWitness=*/nullptr, /*includeRealizationWitness=*/nullptr);
  candidate.begin = begin;
  candidate.end = end;
  candidate.hasPayloadPreview = true;
  candidate.payloadPreview = payloadPreview.str();
  return candidate;
}

RefoldEngine::AcceptedResultCandidate
RefoldEngine::BuildAcceptedTUAnchorCandidate(
    AcceptedPathKind currentPath, const TUAnchorWitness &witness) const {
  AcceptedResultCandidate candidate;
  candidate.kind = AcceptedResultCandidateKind::TUAnchor;
  candidate.proofSummary = BuildAcceptedPathProofSummary(
      currentPath, /*patch=*/nullptr, &witness);
  if (witness.hasPPGap)
    candidate.begin = candidate.end = witness.ppGap;
  if (witness.hasTUByte) {
    candidate.hasAnchorByte = true;
    candidate.anchorByte = witness.tuByte;
  }
  return candidate;
}

RefoldEngine::AcceptedResultCandidate
RefoldEngine::BuildAcceptedTerminalCandidate(
    const TerminalFallbackWitness &witness) const {
  AcceptedResultCandidate candidate;
  candidate.kind = AcceptedResultCandidateKind::TerminalOutOfDomain;
  candidate.proofSummary = BuildAcceptedPathProofSummary(
      AcceptedPathKind::TerminalEmitEditedPreprocessedStream,
      /*patch=*/nullptr, /*tuAnchorWitness=*/nullptr,
      /*includeAnchorWitness=*/nullptr,
      /*includeRealizationWitness=*/nullptr, &witness);
  return candidate;
}
// Implementation extracted verbatim to keep `RefoldEngine.cpp`
// physically smaller without changing ownership or semantics.
#include "RefoldEngine.AcceptedProofs.inc"

void RefoldEngine::AddForcedCounterPatches(
    ArrayRef<ForcedMacroPatchRequest> forced,
    DenseMap<std::optional<uint64_t>, DenseMap<uint64_t, MacroPatch>>
        &macroPatchByOwnerByMacroId) const {
  auto buildOccReplacement = [&](uint64_t aStart, uint64_t aEnd)
      -> std::optional<std::string> {
    auto bEnv = MapATokRangeAToBTokenEnvelope(aStart, aEnd);
    if (!bEnv)
      return std::nullopt;
    if (bEnv->second <= bEnv->first)
      return std::nullopt;
    return SliceBSource(bEnv->first, bEnv->second).trim().str();
  };

  for (const auto &req : forced) {
    const RefoldModel::MacroInvocation *pm = req.macro;
    if (!pm)
      continue;
    const RefoldModel::MacroInvocation &m = *pm;

    // Safety: do not patch macro definitions.
    if (IsInvocationInsideDefineDirective(m))
      continue;

    const auto invStart = m.invB;
    const auto invEnd = m.invE;
    if (!invStart || !invEnd || *invEnd < *invStart)
      continue;

    std::optional<std::string> replOpt;
    if (m.name == "__COUNTER__") {
      replOpt = buildOccReplacement(req.aStart, req.aEnd);
    } else {
      replOpt = BuildWholeCoverReplacementText(m);
    }
    if (!replOpt)
      continue;

    auto &byMacroId = macroPatchByOwnerByMacroId[m.ownerIncludeId];

    // Coalesce by physical invocation span (inv_b/inv_e), matching the hunk
    // coalescing logic used during normal classification.
    std::optional<uint64_t> existingKey;
    for (const auto &kv : byMacroId) {
      const MacroPatch &p = kv.second;
      if (p.invStart == *invStart && p.invEnd == *invEnd) {
        if (!existingKey || kv.first < *existingKey)
          existingKey = kv.first;
      }
    }
    const uint64_t patchKey = existingKey.value_or(m.id);

    // Preserve an existing non-callsite (already-expanded) replacement.
    auto it = byMacroId.find(patchKey);
    if (it != byMacroId.end()) {
      const bool isCallsite =
          InvocationSpanMatchesCallsitePrefix(it->second.replacement, m);
      if (!isCallsite)
        continue;
    }

    trace("counter",
          "__COUNTER__: force patch id={0} name='{1}' ownerInc={2} "
          "inv=[{3},{4}) repl='{5}' Aocc=[{6},{7})",
          m.id, m.name, m.ownerIncludeId, *invStart, *invEnd,
          stringutils::showWSWithClip(*replOpt, 64), req.aStart, req.aEnd);

    MacroPatch patch{*invStart, *invEnd, std::move(*replOpt)};
    if (it != byMacroId.end())
      CarryMacroPatchOwnerCertificate(patch, it->second);
    StampMacroPatchOwnerWitness(
        patch, m.ownerIncludeId ? Owner::Include(*m.ownerIncludeId)
                                : Owner::TU());
    patch.macroId = patchKey;
    byMacroId[patchKey] = std::move(patch);
  }
}

bool RefoldEngine::NestedWholeCoverIsSelfContained(
    const RefoldModel::MacroInvocation &m) const {
  if (!m.callerMacroId)
    return true;
  if (!m.cover.IsValid() || m.cover.end <= m.cover.begin)
    return false;

  SmallVector<char, 64> covered(m.cover.end - m.cover.begin, 0);
  DenseSet<uint64_t> visited;
  SmallVector<const RefoldModel::MacroInvocation *, 16> stack;
  stack.push_back(&m);

  auto markSpan = [&](const RefoldModel::PPSpan &sp) {
    if (!sp.IsValid() || sp.end <= sp.begin)
      return;
    const uint64_t lo = std::max<uint64_t>(sp.begin, m.cover.begin);
    const uint64_t hi = std::min<uint64_t>(sp.end, m.cover.end);
    for (uint64_t pp = lo; pp < hi; ++pp)
      covered[pp - m.cover.begin] = 1;
  };

  while (!stack.empty()) {
    const RefoldModel::MacroInvocation *cur = stack.pop_back_val();
    if (!cur || !visited.insert(cur->id).second)
      continue;

    bool sawDetailed = false;
    auto markDetailed = [&](auto &&spans) {
      for (const auto &sp : spans) {
        if (!sp.IsValid() || sp.end <= sp.begin)
          continue;
        sawDetailed = true;
        markSpan(sp);
      }
    };

    markDetailed(cur->bodySpans);
    markDetailed(cur->argSpans);
    markDetailed(cur->stringifySpans);
    markDetailed(cur->pasteSpans);

    if (!sawDetailed) {
      for (const auto &sp : cur->spans)
        markSpan(sp);
    }

    auto it = macroChildrenById_.find(cur->id);
    if (it != macroChildrenById_.end()) {
      for (const auto *child : it->second)
        stack.push_back(child);
    }
  }

  return llvm::all_of(covered, [](char c) { return c != 0; });
}

std::optional<RefoldEngine::MacroPatch>
RefoldEngine::BuildMacroInvocationPatchWholeCover(
    const RefoldModel::MacroInvocation &m, const diffutils::Hunk &h,
    StringRef baseInvText,
    const DenseMap<std::optional<uint64_t>, DenseMap<uint64_t, MacroPatch>>
        &patchMap) const {
  // Strategy (in priority order):
  //   1) Prefer an args-only rewrite of the invocation spelling when the edit
  //      is fully contained within argument-like spans.
  //   2) For function-like macros, attempt conservative DAG-chained args-only
  //      lifting from a nested callee invocation back to this callsite.
  //   3) Fall back to whole-cover expansion: replace the invocation with its
  //      B-side expansion cover.

  // The invocation byte span in the owning file must be known.
  const auto invStart = m.invB;
  const auto invEnd = m.invE;
  if (!invStart || !invEnd || *invEnd < *invStart)
    return std::nullopt;

  // Special-case: __COUNTER__.
  //
  // The only robust representation of an edited (or forced) __COUNTER__
  // expansion is to replace the invocation spelling with the B-side literal
  // token(s) for this specific occurrence. Do NOT whole-cover expand using the
  // macro cover, which may span multiple occurrences when a header is included
  // multiple times.
  if (m.name == "__COUNTER__") {
    std::optional<std::string> repl;
    if (h.bEnd > h.bStart) {
      repl = SliceBSource(static_cast<size_t>(h.bStart),
                          static_cast<size_t>(h.bEnd))
                 .trim()
                 .str();
    } else {
      auto bEnv = MapATokRangeAToBTokenEnvelope(h.aStart, h.aEnd);
      if (bEnv && bEnv->second > bEnv->first)
        repl = SliceBSource(bEnv->first, bEnv->second).trim().str();
    }
    if (repl)
      {
        MacroPatch patch{*invStart, *invEnd, std::move(*repl), m.id};
        StampMacroPatchProof(patch, MacroPatchProofKind::CounterLiteral,
                             /*validated=*/true,
                             /*structurePreserving=*/false, m.id);
        return patch;
      }
  }

  trace("macro/whole",
        "whole-cover build inv id={0} name={1} ownerIncludeId={2} hasOwner={3} "
        "inv=[{4},{5}) baseInvLen={6} hunk A[{7},{8})->B[{9},{10})",
        m.id, m.name, m.ownerIncludeId.value_or(0), (bool)m.ownerIncludeId,
        *invStart, *invEnd, baseInvText.size(), h.aStart, h.aEnd, h.bStart,
        h.bEnd);

  const Owner currentPatchOwner =
      NormalizeHunkOwnerForPatch(model_.GetSourcePath(), h);

  // Set when two different concrete subtree-backed witnesses for the same
  // root disagree on an overlapping expected-root formal rewrite. Once this is
  // set for the current pass, we suppress same-root structure-preserving reuse
  // and let the existing whole-cover fallback logic realize the root instead of
  // collapsing incompatible witnesses into one root replay.
  bool conflictingConcreteSubtreeWitnessForcesWholeCover = false;

  // Do not downgrade: if we already have a patch for this invocation and it
  // does not look like a callsite invocation anymore (i.e. we already
  // realized/expanded it), keep it. If it is still a callsite patch, we may
  // refine it across additional hunks.
  const MacroPatch *existingPatch = nullptr;
  const MacroPatch *existingExpandedPatch = nullptr;
  bool existingIsCallsite = false;

  // Determinism: ownerIt->second is a DenseMap, so iteration order is unstable.
  // If multiple patches share this invocation span, choose a stable
  // representative (smallest macro id), preferring non-callsite patches.
  auto ownerIt = patchMap.find(m.ownerIncludeId);
  if (ownerIt != patchMap.end()) {
    std::optional<uint64_t> bestNonCallsiteId;
    std::optional<uint64_t> bestCallsiteId;

    for (const auto &kv : ownerIt->second) {
      const uint64_t id = kv.first;
      const MacroPatch &p = kv.second;
      if (p.invStart != *invStart || p.invEnd != *invEnd)
        continue;
      if (!MacroPatchOwnerMatches(p, currentPatchOwner))
        continue;

      const bool isStructurePreserving =
          p.structurePreserving && p.proofRootMacroId == m.id;
      const bool isCallsite =
          isStructurePreserving &&
          InvocationSpanMatchesCallsitePrefix(p.replacement, m);
      if (!isCallsite) {
        if (!bestNonCallsiteId || id < *bestNonCallsiteId)
          bestNonCallsiteId = id;
      } else {
        if (!bestCallsiteId || id < *bestCallsiteId)
          bestCallsiteId = id;
      }
    }

    if (bestNonCallsiteId) {
      auto it = ownerIt->second.find(*bestNonCallsiteId);
      if (it != ownerIt->second.end())
        existingExpandedPatch = &it->second;
    }

    if (bestCallsiteId) {
      auto it = ownerIt->second.find(*bestCallsiteId);
      if (it != ownerIt->second.end()) {
        existingPatch = &it->second;
        existingIsCallsite = it->second.structurePreserving &&
                             it->second.proofRootMacroId == m.id &&
                             InvocationSpanMatchesCallsitePrefix(
                                 it->second.replacement, m);
      }
    }
  }

  if (existingPatch) {
    trace("macro/proof",
          "existing callsite patch audit: inv id={0} name={1} {2}", m.id,
          m.name, FormatMacroPatchAudit(*existingPatch));
  }
  if (existingExpandedPatch) {
    trace("macro/proof",
          "existing expanded patch audit: inv id={0} name={1} {2}", m.id,
          m.name, FormatMacroPatchAudit(*existingExpandedPatch));
  }

  // Trim equal A/B token edges so args-only can run even if the diff hunk spans
  // unchanged punctuation/whitespace around the actual argument-produced
  // change.
  auto trimCommonEdgeTokens = [&](diffutils::Hunk hh) {
    while (hh.aStart < hh.aEnd && hh.bStart < hh.bEnd) {
      size_t aIdx = static_cast<size_t>(hh.aStart);
      size_t bIdx = static_cast<size_t>(hh.bStart);
      if (aIdx >= aToks_.size() || bIdx >= bToks_.size())
        break;
      if (aToks_[aIdx].spelling != bToks_[bIdx].spelling)
        break;
      ++hh.aStart;
      ++hh.bStart;
    }
    while (hh.aEnd > hh.aStart && hh.bEnd > hh.bStart) {
      size_t aIdx = static_cast<size_t>(hh.aEnd - 1);
      size_t bIdx = static_cast<size_t>(hh.bEnd - 1);
      if (aIdx >= aToks_.size() || bIdx >= bToks_.size())
        break;
      if (aToks_[aIdx].spelling != bToks_[bIdx].spelling)
        break;
      --hh.aEnd;
      --hh.bEnd;
    }
    return hh;
  };
  const diffutils::Hunk hEff = trimCommonEdgeTokens(h);

  // 1) Prefer args-only patching when safe and fully validated. Treat normal
  //    arg spans, stringify spans, and paste spans as "argument-like"
  //    occurrences.

  auto isVariadicFormalInInvocation =
      [&](const RefoldModel::MacroInvocation &inv, uint32_t idx) -> bool {
    return idx < inv.defParams.size() && inv.defParams[idx].variadic;
  };

  // Important arbitration rule: a direct root args-only patch is not returned
  // immediately. We let the DAG-chained certificate path run afterwards and
  // prefer it when it can produce a unique validated callsite rewrite, because
  // that path can preserve deeper nested macro structure that a direct root
  // argument rewrite would flatten.
  std::optional<MacroPatch> argsOnlyCandidate;
  // Keep the preferred DAG root replay alive until the shared final macro
  // selector runs. Step 1 removes the last behavioral macro bypass by carrying
  // this candidate through the same final arbitration as the other accepted
  // macro outcomes instead of returning it immediately from the local DAG
  // competition block.
  std::optional<MacroPatch> dagRootCandidate;
  bool reuseExistingCallsitePatch = false;

  SmallVector<RefoldModel::PPArgSpan, 16> argLikeSpans;
  argLikeSpans.append(m.argSpans.begin(), m.argSpans.end());
  argLikeSpans.append(m.stringifySpans.begin(), m.stringifySpans.end());
  argLikeSpans.append(m.pasteSpans.begin(), m.pasteSpans.end());

  auto tryPairedPureInsertionRootArgsOnly = [&]() -> std::optional<MacroPatch> {
    if (hEff.aStart != hEff.aEnd || hEff.bStart >= hEff.bEnd)
      return std::nullopt;
    if (argLikeSpans.empty())
      return std::nullopt;
    if (!m.pasteSpans.empty())
      return std::nullopt;

    SmallVector<char, 16> curTouched(argLikeSpans.size(), 0);
    if (HunkFullyWithinArgSpans(hEff, argLikeSpans, curTouched))
      return std::nullopt;

    StringRef invSpanText =
        !baseInvText.empty()
            ? baseInvText
            : (m.invText ? StringRef(*m.invText) : StringRef(""));
    if (!InvocationSpanMatchesCallsitePrefix(invSpanText, m))
      return std::nullopt;

    std::vector<RefoldModel::PPArgSpan> occs;
    append_range(occs, m.argSpans);
    append_range(occs, m.stringifySpans);
    if (occs.empty())
      return std::nullopt;

    auto buildCombinedInsertionEnvelope =
        [&](const diffutils::Hunk &left, const diffutils::Hunk &right)
        -> diffutils::Hunk {
      diffutils::Hunk env;
      env.aStart = std::min(left.aStart, right.aStart);
      env.aEnd = std::max(left.aStart, right.aStart);
      env.bStart = std::min(left.bStart, right.bStart);
      env.bEnd = std::max(left.bEnd, right.bEnd);
      return env;
    };

    for (const auto &partner : abTokHunks_) {
      if (partner.aStart != partner.aEnd || partner.bStart >= partner.bEnd)
        continue;
      if (partner.aStart == hEff.aStart && partner.bStart == hEff.bStart &&
          partner.bEnd == hEff.bEnd)
        continue;
      if (!(m.cover.begin <= partner.aStart && partner.aEnd <= m.cover.end))
        continue;

      auto isCanonicalLeader = [&](const diffutils::Hunk &lhs,
                                   const diffutils::Hunk &rhs) {
        if (lhs.aStart != rhs.aStart)
          return lhs.aStart < rhs.aStart;
        if (lhs.bStart != rhs.bStart)
          return lhs.bStart < rhs.bStart;
        return lhs.bEnd < rhs.bEnd;
      };
      if (!isCanonicalLeader(hEff, partner))
        continue;

      const diffutils::Hunk env = buildCombinedInsertionEnvelope(hEff, partner);
      const diffutils::Hunk envTrim = trimCommonEdgeTokens(env);

      std::vector<char> touchedOcc(occs.size(), 0);
      if (!HunkFullyWithinArgSpans(envTrim, occs, touchedOcc))
        continue;

      SmallVector<uint32_t, 4> touchedArgs;
      for (size_t i = 0; i < occs.size(); ++i) {
        if (!touchedOcc[i])
          continue;
        if (!llvm::is_contained(touchedArgs, occs[i].argIdx))
          touchedArgs.push_back(occs[i].argIdx);
      }
      if (touchedArgs.size() != 1)
        continue;

      const uint32_t argIdx = touchedArgs.front();
      trace("macro/args",
            "paired pure-insertion args-only candidate inv id={0} name={1} "
            "argIdx={2} cur={3} partner={4} envTrim={5} -> delegate to "
            "standard args-only builder",
            m.id, m.name, argIdx, hEff, partner, envTrim);

      auto patch = BuildMacroInvocationPatchArgsOnly(m, envTrim, baseInvText);
      if (!patch)
        continue;

      trace("macro/args",
            "paired pure-insertion args-only SUCCESS inv id={0} name={1} "
            "argIdx={2} cur={3} partner={4} newInv='{5}'",
            m.id, m.name, argIdx, hEff, partner,
            stringutils::showWSWithClip(patch->replacement, 200));
      StampMacroPatchProof(*patch,
                           MacroPatchProofKind::ArgsOnlyPairedPureInsertion,
                           /*validated=*/true,
                           /*structurePreserving=*/true, m.id);
      return patch;
    }

    return std::nullopt;
  };

  SmallVector<char, 16> argTouched(argLikeSpans.size(), 0);
  StringRef invSpanText =
      !baseInvText.empty()
          ? baseInvText
          : (m.invText ? StringRef(*m.invText) : StringRef(""));
  const bool rootHasDirectArgLikeSurface =
      !argLikeSpans.empty() &&
      HunkFullyWithinArgSpans(hEff, argLikeSpans, argTouched) &&
      InvocationSpanMatchesCallsitePrefix(invSpanText, m);

  if (rootHasDirectArgLikeSurface) {
    // First try to patch arguments in-place. If that can't satisfy the
    // edit, we may still be able to preserve more structure via DAG lifting
    // below, so keep the candidate around instead of returning immediately.
    argsOnlyCandidate = BuildMacroInvocationPatchArgsOnly(m, hEff, baseInvText);
    if (!argsOnlyCandidate) {
      trace("instr/macro",
            "args-only: FAIL macro id={0} name='{1}' hunkA=[{2},{3}) "
            "hunkB=[{4},{5}) (see [trace][macro/args])",
            m.id, m.name, hEff.aStart, hEff.aEnd, hEff.bStart, hEff.bEnd);
      // If we already have a callsite patch and args-only yields no
      // replacement for the trimmed hunk, then the edit is already satisfied
      // by the current callsite text. Defer reusing it until after DAG
      // chaining has had a chance to preserve deeper structure.
      reuseExistingCallsitePatch =
          existingPatch && existingIsCallsite && !baseInvText.empty() &&
          existingPatch->structurePreserving &&
          existingPatch->proofRootMacroId == m.id;
    }
  } else if (!argLikeSpans.empty() &&
             InvocationSpanMatchesCallsitePrefix(invSpanText, m)) {
    argsOnlyCandidate = tryPairedPureInsertionRootArgsOnly();
  }

  // 1b) Conservative DAG chaining: if the edited A-span lies within this
  //     invocation's cover but not within one of its direct argument-like
  //     spans, attempt to lift the edit from a nested callee invocation back
  //     to this callsite's arguments.
  //
  // Policy:
  //   * Every intermediate hop must be proven by arg_refs/template inversion.
  //   * A hop is allowed whenever the child argument text admits a unique
  //     inverse through arg_refs back to the contributing caller formals.
  //   * If the inverse is ambiguous, unsupported, or does not match the
  //     observed text, lifting fails and we conservatively keep the subtree
  //     expanded.
  if (m.subkind == "func" && HasLiteralMacroCalleeOrigin(m)) {
    bool directRootPreservationInadmissible = false;
    auto tryDAGChainedArgsOnly = [&]() -> std::optional<MacroPatch> {
      // --- Phase 0: Preconditions / root invocation parsing ------------------
      //
      // We can only "lift" edits back into the current root invocation (m) if
      // we can reliably parse *its current spelling* into formal argument byte
      // ranges. This must be the callsite text (not some expanded body text).
      StringRef invSpanText =
          !baseInvText.empty()
              ? baseInvText
              : (m.invText ? StringRef(*m.invText) : StringRef(""));

      trace("macro/dag",
            "DAG args-only: enter root id={0} name='{1}' A=[{2},{3}) "
            "invFile='{4}' inv=[{5},{6}) invSpanLen={7} baseInvLen={8}",
            m.id, m.name, hEff.aStart, hEff.aEnd,
            (m.invFile ? StringRef(*m.invFile) : StringRef("")),
            (m.invB ? *m.invB : 0ULL), (m.invE ? *m.invE : 0ULL),
            invSpanText.size(), baseInvText.size());

      if (!InvocationSpanMatchesCallsitePrefix(invSpanText, m)) {
        trace("macro/dag",
              "DAG args-only: root invocation span does not match callsite "
              "prefix; invSpanText prefix='{0}'",
              invSpanText.take_front(48));
        return std::nullopt;
      }

      // Parse the byte ranges of each *formal argument* within invSpanText.
      // This is the target surface we will rewrite if lifting succeeds.
      auto invArgRangesOpt =
          GetMacroInvocationFormalArgContentRanges(m, invSpanText);
      if (!invArgRangesOpt) {
        trace("macro/dag",
              "DAG args-only: failed to parse formal arg ranges for root "
              "id={0} name='{1}' invSpanText prefix='{2}'",
              m.id, m.name, invSpanText.take_front(48));
        return std::nullopt;
      }
      const auto &invArgRanges = *invArgRangesOpt;
      const size_t numArgs = invArgRanges.size();
      if (numArgs == 0) {
        unsigned directCallees = 0;
        unsigned directCalleesWithInvArgRanges = 0;
        for (const RefoldModel::MacroInvocation &cand :
             model_.GetMacroInvocations()) {
          if (!cand.callerMacroId || *cand.callerMacroId != m.id)
            continue;
          ++directCallees;
          if (!cand.invArgRanges.empty())
            ++directCalleesWithInvArgRanges;
        }
        trace("macro/dag",
              "DAG args-only: root has zero formal args; will compute leaf arg "
              "edits for diagnostics but cannot emit a root args-only patch. "
              "directCallees={0} "
              "directCalleesWithInvArgRanges={1}",
              directCallees, directCalleesWithInvArgRanges);
      }

      // --- Phase 1: Build lookup structures for DAG traversal ----------------
      //
      // We lift edits along caller/callee edges between MacroInvocation items.
      // Build a fast lookup map from invocation id -> invocation* so we can
      // climb parent pointers without repeated O(N) scans.
      DenseMap<uint64_t, const RefoldModel::MacroInvocation *> invById;
      invById.reserve(model_.GetMacroInvocations().size());
      for (const auto &mi : model_.GetMacroInvocations())
        invById[mi.id] = &mi;

      // Compute the number of hops from candidate invocation "cand" up to the
      // root invocation "m". Returns:
      //   * d = 1..N when cand is a descendant of m
      //   * nullopt when cand is not in m's subtree (or ancestry is broken)
      auto depthToRoot = [&](const RefoldModel::MacroInvocation &cand)
          -> std::optional<unsigned> {
        unsigned d = 0;
        std::optional<uint64_t> p = cand.callerMacroId;
        while (p) {
          ++d;
          if (*p == m.id)
            return d;
          auto it = invById.find(*p);
          if (it == invById.end())
            break;
          p = it->second->callerMacroId;
        }
        return std::nullopt;
      };

      // Accept descendant leaves whose callee ancestry is either fully literal
      // or closes transitively through the proved whole-formal caller-forwarding
      // rule above. This is the chunk-5 boundary for the current contract:
      // anything outside that proof surface remains conservatively rejected.
      auto pathHasProvableCalleeClosure =
          [&](const RefoldModel::MacroInvocation &cand) -> bool {
        const RefoldModel::MacroInvocation *cur = &cand;
        for (;;) {
          if (!HasLiteralMacroCalleeOrigin(*cur)) {
            if (cur->calleeOrigin.kind != MacroCalleeOriginKind::CallerParam ||
                !cur->callerMacroId ||
                cur->calleeOrigin.callerParamIndices.size() != 1)
              return false;

            auto parentIt = invById.find(*cur->callerMacroId);
            if (parentIt == invById.end())
              return false;

            const uint32_t slot = cur->calleeOrigin.callerParamIndices.front();
            if (!IsWholeFormalCallerForwardSlot(*parentIt->second, slot))
              return false;
          }

          if (cur->id == m.id)
            return true;
          if (!cur->callerMacroId)
            return false;
          auto it = invById.find(*cur->callerMacroId);
          if (it == invById.end())
            return false;
          cur = it->second;
        }
      };

      // Collect all "argument-like" spans for an invocation:
      //   * standard argument spans
      //   * stringify-derived spans
      //   * paste sub-spans
      //
      // These are the only spans we are willing to treat as "editable
      // arguments" when detecting a leaf edit.
      auto gatherArgLike = [&](const RefoldModel::MacroInvocation &mi,
                               SmallVectorImpl<RefoldModel::PPArgSpan> &out) {
        out.clear();
        out.append(mi.argSpans.begin(), mi.argSpans.end());
        out.append(mi.stringifySpans.begin(), mi.stringifySpans.end());
        out.append(mi.pasteSpans.begin(), mi.pasteSpans.end());
      };

      // SpanText is the extracted argument text for a span, plus a reliability
      // bit. Reliability is important for paste spans when the edited B token
      // changes length; we may have to fall back to clamped slicing which is
      // ambiguous.
      struct SpanText {
        std::string text;
        bool reliable; // true iff derived without ambiguous fallback logic
      };

      // Extract the argument text corresponding to a PPArgSpan, either from A
      // (fromB=false) or from B (fromB=true).
      //
      // Special handling:
      //   * Paste spans may refer to a token-internal [byteBegin, byteEnd)
      //   range.
      //     For B-side paste spans, that range may no longer align if the
      //     pasted token changed. We attempt to re-derive the segment using
      //     A-side prefix/suffix preservation; otherwise we clamp and mark
      //     unreliable.
      auto extractSpanText = [&](const RefoldModel::PPArgSpan &sp,
                                 bool fromB) -> std::optional<SpanText> {
        if (sp.end <= sp.begin)
          return std::nullopt;

        // --- A-side extraction: exact bytes from the original pp token stream.
        if (!fromB) {
          StringRef a = SliceASource(static_cast<size_t>(sp.begin),
                                     static_cast<size_t>(sp.end));
          if ((sp.kind == PPArgSpanKind::Paste ||
             sp.kind == PPArgSpanKind::Stringify) &&
            sp.byteBegin && sp.byteEnd) {
            // Paste spans and wrapped stringify spans can identify a subrange
            // within a single output token.
            if ((sp.end - sp.begin) != 1)
              return std::nullopt;
            const uint64_t bb = *sp.byteBegin;
            const uint64_t be = *sp.byteEnd;
            if (be < bb || be > static_cast<uint64_t>(a.size()))
              return std::nullopt;
            a = a.slice(static_cast<size_t>(bb), static_cast<size_t>(be));
          }
          return SpanText{a.trim().str(), /*reliable=*/true};
        }

        // --- B-side extraction: map the A-span to its B envelope and slice B.
        auto bEnv = MapAToBTokenEnvelopeByPPArgSpan(sp);
        if (!bEnv)
          return std::nullopt;
        if (bEnv->second <= bEnv->first)
          return std::nullopt;
        StringRef b = SliceBSource(bEnv->first, bEnv->second);
        bool reliable = true;

        if ((sp.kind == PPArgSpanKind::Paste ||
             sp.kind == PPArgSpanKind::Stringify) &&
            sp.byteBegin && sp.byteEnd) {
          if ((bEnv->second - bEnv->first) != 1)
            return std::nullopt;
          const uint64_t bb = *sp.byteBegin;
          const uint64_t be = *sp.byteEnd;

          // Token-internal byte ranges are computed from the A-side token
          // spelling. If the B-side token changes length (for example
          // L"hello" -> L"goodbye" for wrapped stringify, or any pasted
          // token rewrite), using the raw [bb,be) slice can truncate the
          // changed segment. Re-derive the B-side segment by peeling any
          // unchanged prefix/suffix when possible.
          StringRef aTok = SliceASource(static_cast<size_t>(sp.begin),
                                        static_cast<size_t>(sp.end));
          if (be < bb || be > static_cast<uint64_t>(aTok.size()))
            return std::nullopt;
          StringRef aPref = aTok.take_front(static_cast<size_t>(bb));
          StringRef aSuff = aTok.drop_front(static_cast<size_t>(be));
          if (b.starts_with(aPref) && b.ends_with(aSuff) &&
              b.size() >= aPref.size() + aSuff.size()) {
            b = b.slice(aPref.size(), b.size() - aSuff.size());
          } else {
            // The normal case: split the rewritten core around the original
            // literal delimiters and require a unique segmentation.
            reliable = false;
            const uint64_t bbC = std::min<uint64_t>(bb, b.size());
            const uint64_t beC = std::min<uint64_t>(be, b.size());
            if (beC < bbC)
              return std::nullopt;
            b = b.slice(static_cast<size_t>(bbC), static_cast<size_t>(beC));
          }
        }
        return SpanText{b.trim().str(), reliable};
      };

      // Normalize text to a canonical "argument text" for comparison/lifting.
      //
      // - Stringify spans: compare unescaped payload (so escaping/quoting
      //   choices don't create spurious diffs).
      // - Paste spans: usually raw token text; however if a stringify
      // participates
      //   in paste (e.g. L## #x), the pasted token includes quotes even though
      //   the callsite argument does not. Only then do we unstringify.
      auto normalizeLiftText =
          [&](const RefoldModel::MacroInvocation *inv,
              const RefoldModel::PPArgSpan &sp, StringRef raw0,
              bool allowTopLevelComma) -> std::optional<std::string> {
        StringRef raw = raw0.trim();

        // For stringify spans, compare against the de-escaped payload so that
        // quote/escape choices do not create false diffs.
        if (sp.kind == PPArgSpanKind::Stringify) {
          auto un = UnstringifyLiteralToArgText(raw, allowTopLevelComma);
          if (!un)
            return std::nullopt;
          return *un;
        }

        // Paste spans are normally token text (identifiers, numbers, string
        // literals, etc.). However, when a stringified argument participates in
        // token pasting (e.g. L## #x), the pasted token will contain quotes
        // even though the invocation argument does not. Only in that case
        // should we unstringify.
        if (sp.kind == PPArgSpanKind::Paste && sp.byteBegin && sp.byteEnd &&
            raw.find('"') != StringRef::npos) {
          bool invArgHasQuote = false;
          if (inv && inv->invText && inv->invB &&
              sp.argIdx < inv->invArgRanges.size()) {
            const auto &rng = inv->invArgRanges[sp.argIdx];
            if (rng.first && rng.second && *rng.first <= *rng.second &&
                *rng.first >= *inv->invB) {
              const uint64_t relB = *rng.first - *inv->invB;
              const uint64_t relE = *rng.second - *inv->invB;
              if (relE >= relB && relE <= inv->invText->size()) {
                StringRef invArg =
                    StringRef(*inv->invText).slice(relB, relE);
                invArgHasQuote = invArg.find('"') != StringRef::npos;
              }
            }
          }
          if (!invArgHasQuote) {
            auto un = UnstringifyLiteralToArgText(raw);
            if (!un)
              return std::nullopt;
            return *un;
          }
        }

        return raw.str();
      };

      auto sanitizeArgLikeSpans =
          [&](SmallVectorImpl<RefoldModel::PPArgSpan> &spans) {
            SmallVector<RefoldModel::PPArgSpan, 8> valid;
            valid.reserve(spans.size());
            const uint64_t maxATokCount = static_cast<uint64_t>(aToks_.size());
            for (const auto &sp : spans) {
              if (sp.end <= sp.begin)
                continue;
              if (sp.begin >= maxATokCount || sp.end > maxATokCount)
                continue;
              valid.push_back(sp);
            }
            spans.assign(valid.begin(), valid.end());
          };

      auto hunkWithinBodySpans =
          [&](const diffutils::Hunk &hh,
              ArrayRef<RefoldModel::PPSpan> bodySpans,
              SmallVectorImpl<uint32_t> &touchedBodyIdxs) -> bool {
            touchedBodyIdxs.clear();
            const uint64_t a0 = hh.aStart;
            const uint64_t a1 = hh.aEnd;
            if (a0 == a1) {
              for (size_t i = 0; i < bodySpans.size(); ++i) {
                const auto &s = bodySpans[i];
                if (a0 == s.begin || a0 == s.end) {
                  touchedBodyIdxs.push_back(static_cast<uint32_t>(i));
                  return true;
                }
              }
              return false;
            }

            bool any = false;
            for (uint64_t a = a0; a < a1; ++a) {
              bool inSome = false;
              for (size_t i = 0; i < bodySpans.size(); ++i) {
                const auto &s = bodySpans[i];
                if (a >= s.begin && a < s.end) {
                  if (!llvm::is_contained(touchedBodyIdxs,
                                          static_cast<uint32_t>(i)))
                    touchedBodyIdxs.push_back(static_cast<uint32_t>(i));
                  inSome = true;
                  any = true;
                }
              }
              if (!inSome)
                return false;
            }
            return any;
          };

      auto formatTokHunk = [&](const diffutils::Hunk &hh) {
        return formatv("A=[{0},{1}) B=[{2},{3})", hh.aStart, hh.aEnd,
                       hh.bStart, hh.bEnd)
            .str();
      };

      auto buildCombinedInsertionEnvelope =
          [&](const diffutils::Hunk &left, const diffutils::Hunk &right)
          -> diffutils::Hunk {
            diffutils::Hunk env;
            env.aStart = std::min(left.aStart, right.aStart);
            env.aEnd = std::max(left.aStart, right.aStart);
            env.bStart = std::min(left.bStart, right.bStart);
            env.bEnd = std::max(left.bEnd, right.bEnd);
            return env;
          };

      // --- Phase 2: Find leaf candidates touched by this hunk ----------------
      //
      // We search for descendant invocations whose *argument-like spans* are
      // fully covered by the hunk and exhibit an A->B text difference.
      //
      // We prefer deeper leaves (closest to the actual edited text), because
      // lifting from a deeper leaf tends to be more local and less ambiguous.
      struct LeafCandidate {
        const RefoldModel::MacroInvocation *inv;
        unsigned depth;             // distance from leaf to root (m): 1..N
        uint64_t smallestSpanBytes; // tie-breaker: prefer more local spans
        SmallVector<RefoldModel::PPArgSpan, 8> argLike;
        SmallVector<char, 8> touched;
      };

      struct SplitInsertionRootCandidate {
        MacroPatch patch;
        SmallVector<uint32_t, 8> deferOccurrenceArgIdxs;
      };

      SmallVector<SplitInsertionRootCandidate, 8>
          splitInsertionRootCandidates;

      auto spanBytes = [](const RefoldModel::PPArgSpan &s) -> uint64_t {
        // Prefer spans that are more local in the PP output (smaller ppByte
        // extent).
        if (s.ppByteBegin && s.ppByteEnd && *s.ppByteEnd > *s.ppByteBegin)
          return uint64_t(*s.ppByteEnd - *s.ppByteBegin);
        // Fall back to token-range width.
        if (s.end > s.begin)
          return uint64_t(s.end - s.begin);
        return ~uint64_t(0);
      };

      if (hEff.aStart == hEff.aEnd && !abTokHunks_.empty()) {
        SmallVector<char, 16> curRootTouched(argLikeSpans.size(), 0);
        const bool curRootWithinArgLike =
            !argLikeSpans.empty() &&
            HunkFullyWithinArgSpans(hEff, argLikeSpans, curRootTouched);
        SmallVector<diffutils::Hunk, 8> partnerInsertions;
        for (const auto &hh : abTokHunks_) {
          if (hh.aStart != hh.aEnd)
            continue;
          if (hh.aStart < m.cover.begin || hh.aEnd > m.cover.end)
            continue;
          if (hh.aStart == hEff.aStart && hh.bStart == hEff.bStart &&
              hh.bEnd == hEff.bEnd)
            continue;
          partnerInsertions.push_back(hh);
        }

        trace("macro/dag",
              "split insertion probe: root id={0} name='{1}' cur={2} "
              "rootCover=[{3},{4}) curRootWithinArgLike={5} "
              "curRootTouchedN={6} partnerInsertions={7}",
              m.id, m.name, formatTokHunk(hEff), m.cover.begin, m.cover.end,
              curRootWithinArgLike ? 1 : 0,
              static_cast<uint64_t>(
                  std::count(curRootTouched.begin(), curRootTouched.end(), 1)),
              static_cast<uint64_t>(partnerInsertions.size()));

        for (const auto &partner : partnerInsertions) {
          const diffutils::Hunk env =
              buildCombinedInsertionEnvelope(hEff, partner);
          const diffutils::Hunk envTrim = trimCommonEdgeTokens(env);
          SmallVector<char, 16> envRootTouched(argLikeSpans.size(), 0);
          SmallVector<char, 16> envTrimRootTouched(argLikeSpans.size(), 0);
          const bool envRootWithinArgLike =
              !argLikeSpans.empty() &&
              HunkFullyWithinArgSpans(env, argLikeSpans, envRootTouched);
          const bool envTrimRootWithinArgLike =
              !argLikeSpans.empty() &&
              HunkFullyWithinArgSpans(envTrim, argLikeSpans,
                                      envTrimRootTouched);

          std::optional<MacroPatch> pairRootPatch;
          if (!argLikeSpans.empty() && envTrimRootWithinArgLike &&
              InvocationSpanMatchesCallsitePrefix(invSpanText, m)) {
            pairRootPatch =
                BuildMacroInvocationPatchArgsOnly(m, envTrim, baseInvText);
          }

          trace("macro/dag",
                "split insertion pair probe: root id={0} name='{1}' cur={2} "
                "partner={3} env={4} envTrim={5} envRootWithinArgLike={6} "
                "envTrimRootWithinArgLike={7} pairRootPatch={8} "
                "pairRootNewInv='{9}'",
                m.id, m.name, formatTokHunk(hEff), formatTokHunk(partner),
                formatTokHunk(env), formatTokHunk(envTrim),
                envRootWithinArgLike ? 1 : 0, envTrimRootWithinArgLike ? 1 : 0,
                pairRootPatch ? 1 : 0,
                pairRootPatch ? StringRef(pairRootPatch->replacement)
                              : StringRef(""));

          if (pairRootPatch) {
            SplitInsertionRootCandidate candidate;
            // Preserve the already-constructed full root-callsite replacement
            // so it can be validated and merged later through the normal DAG
            // root candidate path.
            candidate.patch = std::move(*pairRootPatch);

            // Collect the root formal argument indices touched by the trimmed
            // combined insertion envelope. We intentionally store formal arg
            // indices here, not raw arg-like span indices, because a single
            // formal may appear multiple times at the root callsite.
            for (size_t idx = 0; idx < envTrimRootTouched.size(); ++idx) {
              // Ignore untouched spans and any defensive out-of-range cases.
              if (!envTrimRootTouched[idx] || idx >= argLikeSpans.size())
                continue;
              const uint32_t argIdx = argLikeSpans[idx].argIdx;
              if (argIdx >= numArgs)
                continue;

              // Defer occurrence-level consistency checks for each touched root
              // formal exactly once. The later DAG validation step will use
              // this set to avoid rejecting the reconstructed root patch before
              // its final root-formal replay is available.
              if (!llvm::is_contained(candidate.deferOccurrenceArgIdxs, argIdx))
                candidate.deferOccurrenceArgIdxs.push_back(argIdx);
            }

            // Keep the deferred formal set stable and deterministic so later
            // validation and tracing do not depend on discovery order.
            llvm::sort(candidate.deferOccurrenceArgIdxs);
            splitInsertionRootCandidates.push_back(std::move(candidate));
          }

          for (const auto &cand : model_.GetMacroInvocations()) {
            auto d = depthToRoot(cand);
            if (!d || *d == 0)
              continue;
            if (!(cand.cover.begin <= env.aStart && env.aEnd <= cand.cover.end))
              continue;

            SmallVector<RefoldModel::PPArgSpan, 8> candArgLikeProbe;
            gatherArgLike(cand, candArgLikeProbe);
            sanitizeArgLikeSpans(candArgLikeProbe);

            SmallVector<char, 8> candCurTouchedBySpan(candArgLikeProbe.size(),
                                                      0);
            SmallVector<char, 8> candEnvTouchedBySpan(candArgLikeProbe.size(),
                                                      0);
            SmallVector<char, 8> candEnvTrimTouchedBySpan(
                candArgLikeProbe.size(), 0);
            const bool candCurWithinArgLike =
                !candArgLikeProbe.empty() &&
                HunkFullyWithinArgSpans(hEff, candArgLikeProbe,
                                        candCurTouchedBySpan);
            const bool candEnvWithinArgLike =
                !candArgLikeProbe.empty() &&
                HunkFullyWithinArgSpans(env, candArgLikeProbe,
                                        candEnvTouchedBySpan);
            const bool candEnvTrimWithinArgLike =
                !candArgLikeProbe.empty() &&
                HunkFullyWithinArgSpans(envTrim, candArgLikeProbe,
                                        candEnvTrimTouchedBySpan);

            SmallVector<uint32_t, 8> curBodyTouched;
            SmallVector<uint32_t, 8> envBodyTouched;
            SmallVector<uint32_t, 8> envTrimBodyTouched;
            const bool candCurWithinBody =
                hunkWithinBodySpans(hEff, cand.bodySpans, curBodyTouched);
            const bool candEnvWithinBody =
                hunkWithinBodySpans(env, cand.bodySpans, envBodyTouched);
            const bool candEnvTrimWithinBody = hunkWithinBodySpans(
                envTrim, cand.bodySpans, envTrimBodyTouched);

            trace(
                "macro/dag",
                "split insertion descendant probe: root id={0} name='{1}' "
                "cand id={2} name='{3}' depth={4} cur={5} partner={6} env={7} "
                "candCover=[{8},{9}) curWithinArgLike={10} "
                "envWithinArgLike={11} "
                "envTrimWithinArgLike={12} curWithinBody={13} "
                "curBodyTouched={14} "
                "envWithinBody={15} envBodyTouched={16} envTrimWithinBody={17} "
                "envTrimBodyTouched={18} argLikeN={19} argRefN={20} "
                "invText='{21}'",
                m.id, m.name, cand.id, cand.name, *d, formatTokHunk(hEff),
                formatTokHunk(partner), formatTokHunk(env), cand.cover.begin,
                cand.cover.end, candCurWithinArgLike ? 1 : 0,
                candEnvWithinArgLike ? 1 : 0, candEnvTrimWithinArgLike ? 1 : 0,
                candCurWithinBody ? 1 : 0, FormatUInt32List(curBodyTouched),
                candEnvWithinBody ? 1 : 0, FormatUInt32List(envBodyTouched),
                candEnvTrimWithinBody ? 1 : 0,
                FormatUInt32List(envTrimBodyTouched),
                static_cast<uint64_t>(candArgLikeProbe.size()),
                static_cast<uint64_t>(cand.argRefs.size()),
                cand.invText ? StringRef(*cand.invText).trim()
                             : StringRef("<none>"));
          }
        }
      }

      SmallVector<LeafCandidate, 8> leafCands;
      directRootPreservationInadmissible = false;

      for (const auto &cand : model_.GetMacroInvocations()) {
        // Only consider invocations that are descendants of the root m.
        auto d = depthToRoot(cand);
        if (!d || *d == 0)
          continue;
        const bool hunkWithinCandCover = cand.cover.begin <= h.aStart &&
                                         h.aEnd <= cand.cover.end &&
                                         cand.cover.begin < cand.cover.end;

        SmallVector<RefoldModel::PPArgSpan, 8> candArgLikeRaw;
        gatherArgLike(cand, candArgLikeRaw);
        SmallVector<RefoldModel::PPArgSpan, 8> candArgLike = candArgLikeRaw;
        sanitizeArgLikeSpans(candArgLike);

        if (!pathHasProvableCalleeClosure(cand)) {
          trace("macro/dag",
                "skip leaf id={0} name='{1}': non-literal callee origin on "
                "path to root id={2}",
                cand.id, cand.name, m.id);
          // Unsupported descendant structure only blocks direct root replay
          // when the edit cannot already be represented by one of the root's
          // own argument-like spans. If the root has a direct args-only proof
          // surface, keep that candidate alive and let DAG lifting compete
          // normally instead of forcing whole-cover expansion.
          if (hunkWithinCandCover && !rootHasDirectArgLikeSurface)
            directRootPreservationInadmissible = true;
          continue;
        }

        // Candidate must have argument-like spans; otherwise there's nothing
        // concrete to map an edit to.
        if (candArgLike.empty())
          continue;

        // Determine how many formals this invocation "effectively" has, because
        // spans might reference argIdx beyond invArgRanges size.
        unsigned candFormalCount = (unsigned)cand.invArgRanges.size();
        for (const auto &sp : candArgLike)
          candFormalCount = std::max(candFormalCount, (unsigned)sp.argIdx + 1);

        // Mark which concrete arg-like span occurrences are touched by the
        // hunk, then compress that to a per-formal touched set. The helper
        // expects one flag per span occurrence, while the later DAG logic
        // reasons per formal arg index.
        SmallVector<char, 8> candTouchedBySpan(candArgLike.size(), 0);
        if (!HunkFullyWithinArgSpans(h, candArgLike, candTouchedBySpan)) {
          continue;
        }

        SmallVector<char, 8> candTouched(candFormalCount, 0);
        for (size_t si = 0; si < candArgLike.size(); ++si) {
          if (!candTouchedBySpan[si])
            continue;
          const auto &sp = candArgLike[si];
          if (sp.argIdx < candTouched.size())
            candTouched[sp.argIdx] = 1;
        }

        bool anyTouched = false;
        for (char t : candTouched)
          if (t) {
            anyTouched = true;
            break;
          }
        if (!anyTouched) {
          continue;
        }

        // Now verify there's an actual A->B difference within at least one
        // touched arg-like span (otherwise lifting would be a no-op).
        bool anyDiff = false;
        uint64_t bestSpan = ~uint64_t(0);
        for (const auto &sp : candArgLike) {
          if (sp.argIdx >= candTouched.size() || !candTouched[sp.argIdx])
            continue;
          bestSpan = std::min(bestSpan, spanBytes(sp));

          auto aTxt = extractSpanText(sp, /*fromB=*/false);
          auto bTxt = extractSpanText(sp, /*fromB=*/true);
          if (!aTxt || !bTxt)
            continue;

          if (bTxt->reliable) {
            // Compare normalized old/new argument text. For variadic formals,
            // allow top-level commas when unstringifying.
            auto aLift = normalizeLiftText(&cand, sp, aTxt->text,
                                           /*allowTopLevelComma=*/true);
            bool allowComma = sp.argIdx < cand.defParams.size() &&
                              cand.defParams[sp.argIdx].variadic;
            auto bLift = normalizeLiftText(&cand, sp, bTxt->text,
                                           /*allowTopLevelComma=*/allowComma);
            if (aLift && bLift && *aLift != *bLift)
              anyDiff = true;
          } else if (sp.kind == PPArgSpanKind::Paste && sp.byteBegin &&
                     sp.byteEnd) {
            // Paste-subrange extraction may be unreliable after edits (token
            // has changed). We still treat this as a potential edit; later we
            // only accept it if token-level splitting is uniquely determined.
            anyDiff = true;
          }
        }

        if (!anyDiff) {
          continue;
        }

        // Candidate leaf accepted: store its arg-like spans and which formals
        // are touched, plus depth and a locality tie-breaker.
        leafCands.push_back(LeafCandidate{&cand, *d, bestSpan,
                                          std::move(candArgLike),
                                          std::move(candTouched)});
      }

      // Order leaves from most promising to least:
      //   (1) deepest first (closest to the actual edit)
      //   (2) smaller span first (more local pp coverage)
      llvm::sort(leafCands, [](const LeafCandidate &a, const LeafCandidate &b) {
        if (a.depth != b.depth)
          return a.depth > b.depth; // deepest first
        return a.smallestSpanBytes < b.smallestSpanBytes;
      });

      // Diagnostic: if DAG chaining is considered, report the number of
      // leaf candidates that could potentially be lifted back to this root.
      debug("macro/dag",
            "DAG args-only: root inv id={0} name={1} leafCandidates={2}", m.id,
            m.name, leafCands.size());

      for (const LeafCandidate &lc : leafCands) {
        const RefoldModel::MacroInvocation &leaf = *lc.inv;

        unsigned touchedN = 0;
        for (char t : lc.touched)
          if (t)
            ++touchedN;

        debug("macro/dag",
              "DAG leaf: leafId={0} name='{1}' caller={2} depth={3} "
              "argLikeN={4} touchedN={5} invArgRangesN={6} argDepsN={7}",
              leaf.id, leaf.name,
              (leaf.callerMacroId ? *leaf.callerMacroId : 0ULL), lc.depth,
              lc.argLike.size(), touchedN, leaf.invArgRanges.size(),
              leaf.argDeps.size());

        const unsigned kMaxDump = 4;
        for (unsigned i = 0; i < lc.argLike.size() && i < kMaxDump; ++i) {
          const auto &sp = lc.argLike[i];
          std::string ppBB =
              sp.ppByteBegin ? std::to_string(*sp.ppByteBegin) : "null";
          std::string ppBE =
              sp.ppByteEnd ? std::to_string(*sp.ppByteEnd) : "null";
          debug("macro/dag",
                "  leafSpan[{0}]: kind={1} argIdx={2} ppTok=[{3},{4}) "
                "ppByte=[{5},{6}]",
                i, sp.kind, sp.argIdx, sp.begin, sp.end, ppBB, ppBE);
        }
      }

      // Cache the root invocation's *current* argument texts (trimmed). These
      // are used for final-hop two-parent splitting and for validation.
      DenseMap<uint32_t, StringRef> rootArgText;
      for (uint32_t i = 0; i < numArgs; ++i) {
        rootArgText[i] =
            invSpanText.slice(invArgRanges[i].first, invArgRanges[i].second)
                .trim();
      }

      // Count substring occurrences. Used to make "split by midBody" robust
      // when the middle delimiter repeats in the old arguments.
      auto countSubstr = [](StringRef s, StringRef pat) -> uint64_t {
        if (pat.empty())
          return 0;
        uint64_t cnt = 0;
        for (size_t pos = 0; (pos = s.find(pat, pos)) != StringRef::npos;
             pos += pat.size())
          ++cnt;
        return cnt;
      };

      // --- Phase 3: Lift a leaf edit to the root invocation ------------------
      //
      // Generalized single-parent lifting uses the recorded arg_refs slices to
      // invert a nested callee argument back to the caller's raw invocation
      // argument text. In other words, we do not merely ask "which caller
      // formal does this depend on?"; we reconstruct the child argument's
      // template, substitute abstract caller-formal variables into that
      // template, and require the observed old/new texts to match that
      // template uniquely.
      //
      // This is strictly more general than the earlier direct-pass-through
      // check. It accepts wrapper forms such as "((x), 10)" whenever the edit
      // changes only the caller-derived part, but it still rejects any hop that
      // cannot be proven by arg_refs (mixed body-owned edits, ambiguous
      // repeated-variable splits, missing provenance, etc.).
      //
      // The template inverse is now expressed as an explicit invertibility
      // certificate:
      //   * Unique       -> each contributing caller formal has exactly one
      //                     derived replacement string.
      //   * NoMatch      -> the observed text does not fit the original
      //                     arg_refs/literal template at all.
      //   * Ambiguous    -> multiple distinct inverses exist.
      //   * Unsupported  -> the template shape is outside the conservative
      //                     solver bounds.
      struct LocalArgRef {
        uint32_t callerParamIndex;
        uint32_t begin;
        uint32_t end;
      };

      struct ArgRefTemplate {
        std::string argText;
        SmallVector<LocalArgRef, 4> refs;
        SmallVector<uint32_t, 2> distinctCallerParams;
      };

      auto getInvocationArgText =
          [&](const RefoldModel::MacroInvocation &inv,
              uint32_t argIdx) -> std::optional<StringRef> {
        if (!inv.invText || !inv.invB)
          return std::nullopt;
        if (argIdx >= inv.invArgRanges.size())
          return std::nullopt;
        const auto &rng = inv.invArgRanges[argIdx];
        if (!rng.first || !rng.second || *rng.second < *rng.first ||
            *rng.first < *inv.invB)
          return std::nullopt;
        const uint64_t relB = *rng.first - *inv.invB;
        const uint64_t relE = *rng.second - *inv.invB;
        if (relE < relB || relE > inv.invText->size())
          return std::nullopt;
        return StringRef(*inv.invText).slice((size_t)relB, (size_t)relE).trim();
      };

      auto buildArgRefTemplate =
          [&](const RefoldModel::MacroInvocation &inv,
              uint32_t argIdx) -> std::optional<ArgRefTemplate> {
        if (!inv.invText || !inv.invB)
          return std::nullopt;
        if (argIdx >= inv.invArgRanges.size() || argIdx >= inv.argRefs.size())
          return std::nullopt;

        const auto &rng = inv.invArgRanges[argIdx];
        if (!rng.first || !rng.second || *rng.second < *rng.first ||
            *rng.first < *inv.invB)
          return std::nullopt;

        const uint64_t relB = *rng.first - *inv.invB;
        const uint64_t relE = *rng.second - *inv.invB;
        if (relE < relB || relE > inv.invText->size())
          return std::nullopt;

        StringRef rawArg =
            StringRef(*inv.invText).slice((size_t)relB, (size_t)relE);

        size_t trimLead = 0;
        while (trimLead < rawArg.size() &&
               std::isspace((unsigned char)rawArg[trimLead]))
          ++trimLead;
        size_t trimEnd = rawArg.size();
        while (trimEnd > trimLead &&
               std::isspace((unsigned char)rawArg[trimEnd - 1]))
          --trimEnd;

        ArgRefTemplate out;
        out.argText = rawArg.slice(trimLead, trimEnd).str();

        SmallVector<LocalArgRef, 4> refs;
        refs.reserve(inv.argRefs[argIdx].size());
        for (const auto &ref : inv.argRefs[argIdx]) {
          if (ref.byteEnd < ref.byteBegin)
            return std::nullopt;
          if (ref.byteBegin < relB || ref.byteEnd > relE)
            return std::nullopt;
          const uint64_t localBAbs = ref.byteBegin - relB;
          const uint64_t localEAbs = ref.byteEnd - relB;
          if (localEAbs < localBAbs || localEAbs > rawArg.size())
            return std::nullopt;
          if (localBAbs < trimLead || localEAbs > trimEnd)
            return std::nullopt;

          refs.push_back(LocalArgRef{ref.callerParamIndex,
                                     (uint32_t)(localBAbs - trimLead),
                                     (uint32_t)(localEAbs - trimLead)});
        }

        llvm::sort(refs, [](const LocalArgRef &a, const LocalArgRef &b) {
          if (a.begin != b.begin)
            return a.begin < b.begin;
          if (a.end != b.end)
            return a.end < b.end;
          return a.callerParamIndex < b.callerParamIndex;
        });

        uint32_t prevEnd = 0;
        bool first = true;
        for (const auto &ref : refs) {
          if (ref.end < ref.begin || ref.end > out.argText.size())
            return std::nullopt;
          if (!first && ref.begin < prevEnd)
            return std::nullopt;
          prevEnd = ref.end;
          first = false;
          if (llvm::find(out.distinctCallerParams, ref.callerParamIndex) ==
              out.distinctCallerParams.end())
            out.distinctCallerParams.push_back(ref.callerParamIndex);
        }

        out.refs = std::move(refs);
        return out;
      };

      auto sameIndexSet = [&](ArrayRef<uint32_t> a,
                              ArrayRef<uint32_t> b) -> bool {
        SmallVector<uint32_t, 4> sa(a.begin(), a.end());
        SmallVector<uint32_t, 4> sb(b.begin(), b.end());
        llvm::sort(sa);
        llvm::sort(sb);
        sa.erase(std::unique(sa.begin(), sa.end()), sa.end());
        sb.erase(std::unique(sb.begin(), sb.end()), sb.end());
        return sa == sb;
      };

      enum class ArgRefInvertibilityKind {
        Unique,
        NoMatch,
        Ambiguous,
        Unsupported,
      };

      struct ArgRefInvertibilityCertificate {
        ArgRefInvertibilityKind kind = ArgRefInvertibilityKind::Unsupported;
        DenseMap<uint32_t, std::string> derivedTextByCallerParam;
      };

      auto buildArgRefInvertibilityCertificate =
          [&](const ArgRefTemplate &tpl,
              StringRef observed) -> ArgRefInvertibilityCertificate {
        ArgRefInvertibilityCertificate cert;

        constexpr size_t MaxDistinctCallerParams = 8;
        if (tpl.refs.empty() || tpl.distinctCallerParams.empty() ||
            tpl.distinctCallerParams.size() > MaxDistinctCallerParams)
          return cert;

        SmallVector<StringRef, 8> literals;
        SmallVector<unsigned, 8> varOrdinals;
        literals.reserve(tpl.refs.size() + 1);
        varOrdinals.reserve(tpl.refs.size());

        size_t curPos = 0;
        for (const auto &ref : tpl.refs) {
          if (ref.begin < curPos || ref.end < ref.begin ||
              ref.end > tpl.argText.size()) {
            cert.kind = ArgRefInvertibilityKind::NoMatch;
            return cert;
          }
          literals.push_back(StringRef(tpl.argText).slice(curPos, ref.begin));
          auto it = llvm::find(tpl.distinctCallerParams, ref.callerParamIndex);
          if (it == tpl.distinctCallerParams.end()) {
            cert.kind = ArgRefInvertibilityKind::NoMatch;
            return cert;
          }
          varOrdinals.push_back((unsigned)std::distance(
              tpl.distinctCallerParams.begin(), it));
          curPos = ref.end;
        }
        literals.push_back(StringRef(tpl.argText).drop_front(curPos));

        StringRef obs = observed.trim();
        SmallVector<std::optional<StringRef>, MaxDistinctCallerParams> assigns(
            tpl.distinctCallerParams.size());
        SmallVector<SmallVector<std::string, MaxDistinctCallerParams>, 2>
            solutions;

        auto addSolution = [&](ArrayRef<std::optional<StringRef>> A) {
          SmallVector<std::string, MaxDistinctCallerParams> S;
          S.reserve(tpl.distinctCallerParams.size());
          for (size_t i = 0; i < tpl.distinctCallerParams.size(); ++i)
            S.push_back(A[i] ? A[i]->str() : std::string());
          for (const auto &Existing : solutions)
            if (Existing == S)
              return;
          solutions.push_back(std::move(S));
        };

        auto dfs = [&](auto &&self, size_t refIdx, size_t obsPos) -> void {
          if (solutions.size() > 1)
            return;

          const StringRef lit = literals[refIdx];
          if (obsPos > obs.size() || !obs.drop_front(obsPos).starts_with(lit))
            return;
          obsPos += lit.size();

          if (refIdx == varOrdinals.size()) {
            if (obsPos == obs.size())
              addSolution(assigns);
            return;
          }

          const unsigned varOrd = varOrdinals[refIdx];
          if (varOrd >= assigns.size())
            return;

          if (assigns[varOrd]) {
            const StringRef val = *assigns[varOrd];
            if (obsPos <= obs.size() && obs.drop_front(obsPos).starts_with(val))
              self(self, refIdx + 1, obsPos + val.size());
            return;
          }

          size_t minRemain = 0;
          for (size_t j = refIdx + 1; j < literals.size(); ++j)
            minRemain += literals[j].size();
          for (size_t j = refIdx + 1; j < varOrdinals.size(); ++j) {
            const unsigned laterOrd = varOrdinals[j];
            if (laterOrd < assigns.size() && assigns[laterOrd])
              minRemain += assigns[laterOrd]->size();
          }
          if (obsPos + minRemain > obs.size())
            return;

          const size_t maxLen = obs.size() - obsPos - minRemain;
          const StringRef rest = obs.drop_front(obsPos);
          const StringRef nextLit = literals[refIdx + 1];

          auto tryLen = [&](size_t len) {
            assigns[varOrd] = rest.take_front(len);
            self(self, refIdx + 1, obsPos + len);
            assigns[varOrd] = std::nullopt;
          };

          if (!nextLit.empty()) {
            for (size_t searchPos = 0;; ++searchPos) {
              const size_t pos = rest.find(nextLit, searchPos);
              if (pos == StringRef::npos || pos > maxLen)
                break;
              tryLen(pos);
              if (solutions.size() > 1)
                return;
            }
          } else {
            // The normal case: split the rewritten core around the original
            // literal delimiters and require a unique segmentation.
            for (size_t len = 0; len <= maxLen; ++len) {
              tryLen(len);
              if (solutions.size() > 1)
                return;
            }
          }
        };

        dfs(dfs, 0, 0);
        if (solutions.empty()) {
          cert.kind = ArgRefInvertibilityKind::NoMatch;
          return cert;
        }
        if (solutions.size() > 1) {
          cert.kind = ArgRefInvertibilityKind::Ambiguous;
          return cert;
        }

        cert.kind = ArgRefInvertibilityKind::Unique;
        for (unsigned i = 0; i < tpl.distinctCallerParams.size(); ++i)
          cert.derivedTextByCallerParam[tpl.distinctCallerParams[i]] =
              solutions[0][i];
        return cert;
      };

      struct TrimmedArgInfo {
        std::string text;
        uint64_t absTrimBegin = 0;
        uint64_t absTrimEnd = 0;
      };

      auto getTrimmedInvocationArgInfo =
          [&](const RefoldModel::MacroInvocation &inv,
              uint32_t argIdx) -> std::optional<TrimmedArgInfo> {
        if (!inv.invText || !inv.invB)
          return std::nullopt;
        if (argIdx >= inv.invArgRanges.size())
          return std::nullopt;
        const auto &rng = inv.invArgRanges[argIdx];
        if (!rng.first || !rng.second || *rng.second < *rng.first ||
            *rng.first < *inv.invB)
          return std::nullopt;

        const uint64_t relB = *rng.first - *inv.invB;
        const uint64_t relE = *rng.second - *inv.invB;
        if (relE < relB || relE > inv.invText->size())
          return std::nullopt;

        StringRef raw = StringRef(*inv.invText).slice((size_t)relB, (size_t)relE);
        size_t trimLead = 0;
        while (trimLead < raw.size() &&
               std::isspace((unsigned char)raw[trimLead]))
          ++trimLead;
        size_t trimEnd = raw.size();
        while (trimEnd > trimLead &&
               std::isspace((unsigned char)raw[trimEnd - 1]))
          --trimEnd;

        TrimmedArgInfo out;
        out.text = raw.slice(trimLead, trimEnd).str();
        out.absTrimBegin = *rng.first + trimLead;
        out.absTrimEnd = *rng.first + trimEnd;
        return out;
      };

      auto getInvocationCoverAText =
          [&](const RefoldModel::MacroInvocation &inv)
          -> std::optional<std::string> {
        uint64_t covLoA = inv.cover.begin;
        uint64_t covHiA = inv.cover.end;
        if (inv.subkind == "func" && inv.defParams.empty() &&
            !inv.bodySpans.empty()) {
          uint64_t lo = std::numeric_limits<uint64_t>::max();
          uint64_t hi = 0;
          for (const auto &s : inv.bodySpans) {
            if (s.begin < s.end) {
              lo = std::min(lo, s.begin);
              hi = std::max(hi, s.end);
            }
          }
          if (lo != std::numeric_limits<uint64_t>::max() && lo < hi) {
            covLoA = lo;
            covHiA = hi;
          }
        }
        if (covLoA >= covHiA)
          return std::nullopt;
        return SliceASource(covLoA, covHiA).trim().str();
      };

      auto hasTopLevelCommaInRefoldText = [&](StringRef s) -> bool {
        int paren = 0, bracket = 0, brace = 0;
        bool inStr = false, inChr = false, esc = false;
        for (size_t i = 0; i < s.size(); ++i) {
          char c = s[i];

          if (inStr) {
            if (esc) {
              esc = false;
              continue;
            }
            if (c == '\\') {
              esc = true;
              continue;
            }
            if (c == '"')
              inStr = false;
            continue;
          }
          if (inChr) {
            if (esc) {
              esc = false;
              continue;
            }
            if (c == '\\') {
              esc = true;
              continue;
            }
            if (c == '\'')
              inChr = false;
            continue;
          }

          if (c == '/' && i + 1 < s.size()) {
            if (s[i + 1] == '/') {
              i += 2;
              while (i < s.size() && s[i] != '\n')
                ++i;
              continue;
            }
            if (s[i + 1] == '*') {
              i += 2;
              while (i + 1 < s.size() && !(s[i] == '*' && s[i + 1] == '/'))
                ++i;
              if (i + 1 < s.size())
                ++i;
              continue;
            }
          }

          if (c == '"') {
            inStr = true;
            continue;
          }
          if (c == '\'') {
            inChr = true;
            continue;
          }
          switch (c) {
          case '(':
            ++paren;
            break;
          case ')':
            if (paren > 0)
              --paren;
            break;
          case '[':
            ++bracket;
            break;
          case ']':
            if (bracket > 0)
              --bracket;
            break;
          case '{':
            ++brace;
            break;
          case '}':
            if (brace > 0)
              --brace;
            break;
          case ',':
            if (paren == 0 && bracket == 0 && brace == 0)
              return true;
            break;
          default:
            break;
          }
        }
        return false;
      };

      auto isLikelyTokenBoundaryInRefoldText = [&](StringRef s,
                                                  size_t pos) -> bool {
        if (pos == 0 || pos >= s.size())
          return true;
        auto isIdentBody = [](char c) {
          unsigned char uc = static_cast<unsigned char>(c);
          return std::isalnum(uc) || c == '_';
        };
        return !(isIdentBody(s[pos - 1]) && isIdentBody(s[pos]));
      };

      auto enumerateTopLevelBalancedCutPoints =
          [&](StringRef s, auto &&emitCut) {
        int paren = 0, bracket = 0, brace = 0;
        bool inStr = false, inChr = false, esc = false;
        bool inLineComment = false, inBlockComment = false;

        emitCut(0u);
        for (size_t i = 0; i < s.size(); ++i) {
          const char c = s[i];

          if (inLineComment) {
            if (c == '\n')
              inLineComment = false;
          } else if (inBlockComment) {
            if (c == '*' && i + 1 < s.size() && s[i + 1] == '/') {
              inBlockComment = false;
              ++i;
            }
          } else if (inStr) {
            if (esc) {
              esc = false;
            } else if (c == '\\') {
              esc = true;
            } else if (c == '"') {
              inStr = false;
            }
          } else if (inChr) {
            if (esc) {
              esc = false;
            } else if (c == '\\') {
              esc = true;
            } else if (c == '\'') {
              inChr = false;
            }
          } else {
            // The normal case: split the rewritten core around the original
            // literal delimiters and require a unique segmentation.
            if (c == '/' && i + 1 < s.size()) {
              if (s[i + 1] == '/') {
                inLineComment = true;
                ++i;
                continue;
              }
              if (s[i + 1] == '*') {
                inBlockComment = true;
                ++i;
                continue;
              }
            }

            if (c == '"') {
              inStr = true;
            } else if (c == '\'') {
              inChr = true;
            } else {
            // The normal case: split the rewritten core around the original
            // literal delimiters and require a unique segmentation.
              switch (c) {
              case '(':
                ++paren;
                break;
              case ')':
                if (paren > 0)
                  --paren;
                break;
              case '[':
                ++bracket;
                break;
              case ']':
                if (bracket > 0)
                  --bracket;
                break;
              case '{':
                ++brace;
                break;
              case '}':
                if (brace > 0)
                  --brace;
                break;
              default:
                break;
              }
            }
          }

          if (!inStr && !inChr && !esc && !inLineComment && !inBlockComment &&
              paren == 0 && bracket == 0 && brace == 0) {
            emitCut(static_cast<unsigned>(i + 1));
          }
        }
      };

      auto isBalancedRefoldFragment = [&](StringRef s) -> bool {
        bool balancedAtEnd = false;
        enumerateTopLevelBalancedCutPoints(s, [&](unsigned cut) {
          if (cut == s.size())
            balancedAtEnd = true;
        });
        return balancedAtEnd;
      };

      auto enumerateTopLevelLiteralMatchesInRefoldText =
          [&](StringRef haystack, StringRef needle, size_t maxPos,
              auto &&emitMatch) {
        if (needle.empty())
          return;
        enumerateTopLevelBalancedCutPoints(haystack, [&](unsigned cut) {
          const size_t pos = static_cast<size_t>(cut);
          if (pos > maxPos)
            return;
          if (!isLikelyTokenBoundaryInRefoldText(haystack, pos))
            return;
          if (haystack.drop_front(pos).starts_with(needle))
            emitMatch(pos);
        });
      };

      auto buildRewrittenInvocationSyntax =
          [&](const RefoldModel::MacroInvocation &inv,
              const DenseMap<uint32_t, std::string> &replByFormal)
          -> std::optional<std::string> {
        if (!inv.invText || !inv.invB)
          return std::nullopt;

        struct LocalEdit {
          uint64_t begin = 0;
          uint64_t end = 0;
          std::string repl;
        };

        SmallVector<LocalEdit, 8> edits;
        edits.reserve(replByFormal.size());
        for (const auto &KV : replByFormal) {
          const uint32_t argIdx = KV.first;
          if (argIdx >= inv.invArgRanges.size())
            return std::nullopt;
          const auto &rng = inv.invArgRanges[argIdx];
          if (!rng.first || !rng.second || *rng.second < *rng.first ||
              *rng.first < *inv.invB)
            return std::nullopt;

          const uint64_t relB = *rng.first - *inv.invB;
          const uint64_t relE = *rng.second - *inv.invB;
          if (relE < relB || relE > inv.invText->size())
            return std::nullopt;

          StringRef newArg = StringRef(KV.second).trim();
          const bool allowComma =
              argIdx < inv.defParams.size() && inv.defParams[argIdx].variadic;
          if (!allowComma && hasTopLevelCommaInRefoldText(newArg))
            return std::nullopt;

          edits.push_back(LocalEdit{relB, relE, newArg.str()});
        }

        llvm::sort(edits, [](const LocalEdit &a, const LocalEdit &b) {
          return a.begin > b.begin;
        });

        std::string rewritten = inv.invText->str();
        for (const auto &edit : edits)
          rewritten = stringutils::replaceRange(rewritten, edit.begin, edit.end,
                                                edit.repl);
        return StringRef(rewritten).trim().str();
      };

      auto expansionTextCandidates =
          [&](const RefoldModel::MacroInvocation &inv,
              bool fromB) -> SmallVector<std::string, 4> {
        SmallVector<std::string, 4> out;
        std::optional<std::string> base =
            fromB ? BuildWholeCoverReplacementText(inv)
                  : getInvocationCoverAText(inv);
        if (!base)
          return out;

        auto addUnique = [&](StringRef s) {
          std::string cand = s.trim().str();
          if (cand.empty())
            return;
          if (llvm::find(out, cand) == out.end())
            out.push_back(std::move(cand));
        };

        auto tryAddUnstringified = [&](StringRef raw) {
          auto un =
              UnstringifyLiteralToArgText(raw, /*allowTopLevelComma=*/true);
          if (!un)
            return;
          addUnique(*un);
        };

        auto tryAddWideLiteral = [&](StringRef raw) {
          StringRef t = raw.trim();
          if (!stringutils::looksLikeStringLiteralToken(t))
            return;

          // Preserve existing wide/prefixed string literals as-is; only add a
          // widened form when the observed literal is an ordinary string.
          if (t.starts_with("L\"") || t.starts_with("u\"") ||
              t.starts_with("U\"") || t.starts_with("u8\""))
            return;

          addUnique((Twine("L") + t).str());
        };

        addUnique(*base);
        tryAddUnstringified(*base);
        tryAddWideLiteral(*base);
        return out;
      };

      auto quoteCStringLiteral = [&](StringRef raw) -> std::string {
        std::string out;
        out.reserve(raw.size() + 2);
        out.push_back('"');
        for (char c : raw) {
          if (c == '\\' || c == '"')
            out.push_back('\\');
          out.push_back(c);
        }
        out.push_back('"');
        return out;
      };

      enum class WrapperChainKind {
        Exact,
        StringLiteral,
        WideStringLiteral,
      };

      enum class WrapperObservedSource {
        ChildExpansion,
        ChildRawInvocation,
      };

      struct WrapperChainCertificate {
        WrapperChainKind kind = WrapperChainKind::Exact;
        WrapperObservedSource source = WrapperObservedSource::ChildExpansion;
        std::string observedOldText;
        std::string logicalInputText;
      };

      struct LexicalChildPlaceholder {
        uint64_t relBegin = 0;
        uint64_t relEnd = 0;
        const RefoldModel::MacroInvocation *child = nullptr;
        SmallVector<WrapperChainCertificate, 4> observedForms;
        SmallVector<std::string, 4> newExpansionCandidates;
        std::string rawInvocationText;
      };

      auto getTopLevelLexicalChildrenInArg =
          [&](const RefoldModel::MacroInvocation &parent, uint32_t parentFormal)
          -> SmallVector<LexicalChildPlaceholder, 4> {
        SmallVector<LexicalChildPlaceholder, 8> cands;
        auto argInfo = getTrimmedInvocationArgInfo(parent, parentFormal);
        if (!argInfo || !parent.invFile)
          return SmallVector<LexicalChildPlaceholder, 4>{};

        for (const auto &cand : model_.GetMacroInvocations()) {
          if (cand.id == parent.id || !cand.invFile || !cand.invB || !cand.invE)
            continue;
          if (*cand.invFile != *parent.invFile)
            continue;
          if (*cand.invB < argInfo->absTrimBegin ||
              *cand.invE > argInfo->absTrimEnd || *cand.invE <= *cand.invB)
            continue;

          auto olds = expansionTextCandidates(cand, /*fromB=*/false);
          auto news = expansionTextCandidates(cand, /*fromB=*/true);

          SmallVector<WrapperChainCertificate, 4> forms;
          auto addObservedForm = [&](WrapperChainKind kind,
                                     WrapperObservedSource source,
                                     StringRef text, StringRef logicalInput) {
            std::string observed = text.trim().str();
            std::string logical = logicalInput.trim().str();
            if (observed.empty() || logical.empty())
              return;

            if (kind == WrapperChainKind::StringLiteral ||
                kind == WrapperChainKind::WideStringLiteral) {
              auto canon = CanonicalizeStringifyInversePayload(logical);
              if (!canon ||
                  StringRef(*canon).trim() != StringRef(logical).trim())
                return;
              logical = std::move(*canon);
            }

            for (const auto &existing : forms) {
              if (existing.kind == kind &&
                  existing.observedOldText == observed &&
                  existing.logicalInputText == logical)
                return;
            }

            forms.push_back(WrapperChainCertificate{
                kind, source, std::move(observed), std::move(logical)});
          };

          for (StringRef oldText : olds) {
            StringRef trimmed = oldText.trim();
            addObservedForm(WrapperChainKind::Exact,
                            WrapperObservedSource::ChildExpansion, trimmed,
                            trimmed);
            addObservedForm(WrapperChainKind::StringLiteral,
                            WrapperObservedSource::ChildExpansion,
                            quoteCStringLiteral(trimmed), trimmed);
            addObservedForm(WrapperChainKind::WideStringLiteral,
                            WrapperObservedSource::ChildExpansion,
                            (Twine("L") + quoteCStringLiteral(trimmed)).str(),
                            trimmed);
          }
          if (cand.invText) {
            const std::string rawInvocation =
                StringRef(*cand.invText).trim().str();
            if (!rawInvocation.empty()) {
              addObservedForm(WrapperChainKind::Exact,
                              WrapperObservedSource::ChildRawInvocation,
                              rawInvocation, rawInvocation);
              addObservedForm(WrapperChainKind::StringLiteral,
                              WrapperObservedSource::ChildRawInvocation,
                              quoteCStringLiteral(rawInvocation),
                              rawInvocation);
              addObservedForm(
                  WrapperChainKind::WideStringLiteral,
                  WrapperObservedSource::ChildRawInvocation,
                  (Twine("L") + quoteCStringLiteral(rawInvocation)).str(),
                  rawInvocation);
            }
          }
          if (forms.empty())
            continue;

          LexicalChildPlaceholder ph;
          ph.relBegin = *cand.invB - argInfo->absTrimBegin;
          ph.relEnd = *cand.invE - argInfo->absTrimBegin;
          ph.child = &cand;
          ph.observedForms = std::move(forms);
          ph.newExpansionCandidates = std::move(news);
          if (cand.invText)
            ph.rawInvocationText = StringRef(*cand.invText).trim().str();
          cands.push_back(std::move(ph));
        }

        llvm::sort(cands, [](const LexicalChildPlaceholder &a,
                             const LexicalChildPlaceholder &b) {
          if (a.relBegin != b.relBegin)
            return a.relBegin < b.relBegin;
          return a.relEnd > b.relEnd;
        });

        SmallVector<LexicalChildPlaceholder, 4> top;
        for (const auto &cand : cands) {
          bool contained = false;
          for (const auto &sel : top) {
            if (cand.relBegin >= sel.relBegin && cand.relEnd <= sel.relEnd) {
              contained = true;
              break;
            }
            if (!(cand.relEnd <= sel.relBegin || cand.relBegin >= sel.relEnd)) {
              contained = true;
              break;
            }
          }
          if (!contained)
            top.push_back(cand);
        }
        return top;
      };

      enum class ArgInvertibilityKind {
        LiteralOnly,
        TemplateWithChildren,
      };

      struct ArgInvertibilityCertificate {
        ArgInvertibilityKind kind = ArgInvertibilityKind::LiteralOnly;
        std::string rawArgText;
        SmallVector<std::string, 8> literals;
        SmallVector<LexicalChildPlaceholder, 4> slots;
        SmallVector<unsigned, 4> chosenObservedFormIdx;
      };

      auto buildArgInvertibilityCertificate =
          [&](const RefoldModel::MacroInvocation &parent, uint32_t parentFormal,
              StringRef observedOld0)
          -> std::optional<ArgInvertibilityCertificate> {
        auto argInfo = getTrimmedInvocationArgInfo(parent, parentFormal);
        if (!argInfo)
          return std::nullopt;

        StringRef rawArg = StringRef(argInfo->text).trim();
        StringRef observedOld = observedOld0.trim();

        ArgInvertibilityCertificate cert;
        cert.rawArgText = rawArg.str();
        auto placeholders =
            getTopLevelLexicalChildrenInArg(parent, parentFormal);
        if (placeholders.empty()) {
          if (observedOld != rawArg)
            return std::nullopt;
          cert.kind = ArgInvertibilityKind::LiteralOnly;
          cert.literals.push_back(rawArg.str());
          return cert;
        }

        cert.kind = ArgInvertibilityKind::TemplateWithChildren;
        cert.slots = placeholders;

        uint64_t curPos = 0;
        for (const auto &ph : placeholders) {
          if (ph.relBegin < curPos || ph.relEnd < ph.relBegin ||
              ph.relEnd > rawArg.size())
            return std::nullopt;
          cert.literals.push_back(
              rawArg.slice((size_t)curPos, (size_t)ph.relBegin).str());
          curPos = ph.relEnd;
        }
        cert.literals.push_back(rawArg.drop_front((size_t)curPos).str());

        SmallVector<unsigned, 4> chosenOld;
        SmallVector<SmallVector<unsigned, 4>, 2> oldSolutions;
        auto matchOld = [&](auto &&self, size_t idx, size_t pos) -> void {
          if (oldSolutions.size() > 1)
            return;
          const StringRef lit = cert.literals[idx];
          if (pos > observedOld.size() ||
              !observedOld.drop_front(pos).starts_with(lit))
            return;
          pos += lit.size();
          if (idx == cert.slots.size()) {
            if (pos == observedOld.size())
              oldSolutions.push_back(chosenOld);
            return;
          }
          for (unsigned choice = 0;
               choice < cert.slots[idx].observedForms.size(); ++choice) {
            StringRef phOld =
                cert.slots[idx].observedForms[choice].observedOldText;
            if (observedOld.drop_front(pos).starts_with(phOld)) {
              chosenOld.push_back(choice);
              self(self, idx + 1, pos + phOld.size());
              chosenOld.pop_back();
            }
          }
        };
        matchOld(matchOld, 0, 0);
        if (oldSolutions.empty())
          return std::nullopt;

        auto semanticallyEquivalentOldSolutions =
            [&](const SmallVectorImpl<unsigned> &a,
                const SmallVectorImpl<unsigned> &b) -> bool {
          if (a.size() != b.size())
            return false;
          for (size_t i = 0; i < a.size(); ++i) {
            if (i >= cert.slots.size() ||
                a[i] >= cert.slots[i].observedForms.size() ||
                b[i] >= cert.slots[i].observedForms.size())
              return false;
            const auto &fa = cert.slots[i].observedForms[a[i]];
            const auto &fb = cert.slots[i].observedForms[b[i]];
            if (fa.kind != fb.kind || fa.source != fb.source ||
                StringRef(fa.logicalInputText).trim() !=
                    StringRef(fb.logicalInputText).trim())
              return false;
          }
          return true;
        };

        for (size_t i = 1; i < oldSolutions.size(); ++i)
          if (!semanticallyEquivalentOldSolutions(oldSolutions[0],
                                                  oldSolutions[i]))
            return std::nullopt;

        cert.chosenObservedFormIdx = oldSolutions[0];

        return cert;
      };

      enum class SlotRewriteDecisionKind {
        PreferredChildSyntax,
        PreserveRawInvocation,
        PassthroughFlatten,
      };

      struct SlotRewriteDecision {
        SlotRewriteDecisionKind kind =
            SlotRewriteDecisionKind::PassthroughFlatten;
        std::string observedText;
        std::string rebuiltText;

        bool operator==(const SlotRewriteDecision &other) const {
          return kind == other.kind && observedText == other.observedText &&
                 rebuiltText == other.rebuiltText;
        }
      };

      enum class SlotSemanticRewriteCertificateKind {
        Unique,
        Invalid,
      };

      struct SlotSemanticRewriteCertificate {
        SlotSemanticRewriteCertificateKind kind =
            SlotSemanticRewriteCertificateKind::Invalid;
        SlotRewriteDecision decision;
        WrapperChainKind wrapperKind = WrapperChainKind::Exact;
        WrapperObservedSource wrapperSource =
            WrapperObservedSource::ChildExpansion;
        std::string logicalInputText;

        bool operator==(const SlotSemanticRewriteCertificate &other) const {
          return kind == other.kind && decision == other.decision &&
                 wrapperKind == other.wrapperKind &&
                 wrapperSource == other.wrapperSource &&
                 logicalInputText == other.logicalInputText;
        }
      };

      enum class ArgSemanticRewriteCertificateKind {
        NoChange,
        Unique,
        Invalid,
      };

      enum class ArgSemanticRewriteFailure {
        None,
        MissingStructuralTemplate,
      };

      struct ArgSemanticRewriteCertificate {
        ArgSemanticRewriteCertificateKind kind =
            ArgSemanticRewriteCertificateKind::Invalid;
        ArgSemanticRewriteFailure failure = ArgSemanticRewriteFailure::None;
        std::string observedNewText;
        std::string rawArgOldText;
        std::string rawArgNewText;
        SmallVector<SlotRewriteDecision, 8> slotDecisions;
        SmallVector<SlotSemanticRewriteCertificate, 8> slotCertificates;
        std::string detail;
      };

      enum class SemanticInteractionKind {
        Plain,
        ChildSyntax,
        RawInvocation,
        Stringify,
        WideStringify,
        Paste,
        ChildSyntaxPaste,
        RawInvocationPaste,
        StringifyPaste,
        WideStringifyPaste,
        Mixed,
      };

      enum class SemanticInteractionFailure {
        None,
        NonCanonicalLogicalInput,
      };

      struct SemanticInteractionCertificate {
        bool valid = true;
        SemanticInteractionKind kind = SemanticInteractionKind::Plain;
        SemanticInteractionFailure failure = SemanticInteractionFailure::None;
        const RefoldModel::MacroInvocation *inv = nullptr;
        uint32_t argIdx = 0;
        bool touchesPaste = false;
        bool usesPreferredChildSyntax = false;
        bool usesRawInvocationPreservation = false;
        bool usesPassthroughFlatten = false;
        bool usesStringify = false;
        bool usesWideStringify = false;
        bool usesRawChildInvocationLogicalInput = false;
        SmallVector<SlotSemanticRewriteCertificate, 8> slotCertificates;
        SmallVector<std::string, 8> canonicalLogicalInputs;
        std::string detail;
      };

      struct SemanticInteractionSignature {
        bool touchesPaste = false;
        bool usesPreferredChildSyntax = false;
        bool usesRawInvocationPreservation = false;
        bool usesPassthroughFlatten = false;
        bool usesStringify = false;
        bool usesWideStringify = false;
        bool usesRawChildInvocationLogicalInput = false;
        SmallVector<std::string, 8> canonicalLogicalInputs;

        bool operator==(const SemanticInteractionSignature &other) const {
          return touchesPaste == other.touchesPaste &&
                 usesPreferredChildSyntax == other.usesPreferredChildSyntax &&
                 usesRawInvocationPreservation ==
                     other.usesRawInvocationPreservation &&
                 usesPassthroughFlatten == other.usesPassthroughFlatten &&
                 usesStringify == other.usesStringify &&
                 usesWideStringify == other.usesWideStringify &&
                 usesRawChildInvocationLogicalInput ==
                     other.usesRawChildInvocationLogicalInput &&
                 canonicalLogicalInputs == other.canonicalLogicalInputs;
        }
      };

      enum class FormalInteractionConsistencyFailure {
        None,
        DivergentSemanticEvidence,
      };

      struct FormalInteractionConsistencyCertificate {
        bool valid = true;
        FormalInteractionConsistencyFailure failure =
            FormalInteractionConsistencyFailure::None;
        const RefoldModel::MacroInvocation *inv = nullptr;
        uint32_t argIdx = 0;
        SemanticInteractionSignature signature;
        SmallVector<SemanticInteractionCertificate, 2> interactions;
        std::string detail;
      };

      enum class SubtreeInteractionConsistencyFailure {
        None,
        DivergentFormalSemantics,
      };

      struct SubtreeInteractionConsistencyCertificate {
        bool valid = true;
        SubtreeInteractionConsistencyFailure failure =
            SubtreeInteractionConsistencyFailure::None;
        SmallVector<FormalInteractionConsistencyCertificate, 16>
            formalConsistencies;
        std::string detail;
      };

      auto buildArgSemanticRewriteCertificate =
          [&](const ArgInvertibilityCertificate &cert, StringRef observedNew0,
              const DenseMap<uint64_t, std::string> *preferredChildSyntax)
          -> ArgSemanticRewriteCertificate {
        ArgSemanticRewriteCertificate argCert;
        StringRef observedNew = observedNew0.trim();
        argCert.observedNewText = observedNew.str();
        argCert.rawArgOldText = StringRef(cert.rawArgText).trim().str();
        if (cert.kind == ArgInvertibilityKind::LiteralOnly) {
          argCert.rawArgNewText = observedNew.str();
          argCert.kind = (StringRef(argCert.rawArgNewText).trim() ==
                          StringRef(argCert.rawArgOldText).trim())
                             ? ArgSemanticRewriteCertificateKind::NoChange
                             : ArgSemanticRewriteCertificateKind::Unique;
          return argCert;
        }

        SmallVector<SlotRewriteDecision, 8> newParts;
        SmallVector<SlotSemanticRewriteCertificate, 8> newPartCertificates;
        SmallVector<SmallVector<SlotRewriteDecision, 8>, 2> newSolutions;
        SmallVector<SmallVector<SlotSemanticRewriteCertificate, 8>, 2>
            newSolutionCertificates;

        auto pieceMatchesWrapperCertificate =
            [&](const WrapperChainCertificate &wrapper,
                StringRef piece0) -> bool {
          StringRef piece = piece0.trim();
          switch (wrapper.kind) {
          case WrapperChainKind::Exact:
            return true;
          case WrapperChainKind::StringLiteral:
            return stringutils::looksLikeStringLiteralToken(piece);
          case WrapperChainKind::WideStringLiteral:
            return piece.starts_with("L\"") ||
                   (piece.starts_with("L") &&
                    stringutils::looksLikeStringLiteralToken(
                        piece.drop_front(1)));
          }
          llvm_unreachable("invalid WrapperChainKind");
        };

        auto literalDecodesToCanonicalLogicalInput =
            [&](StringRef piece0, StringRef expected0) -> bool {
          auto decoded =
              UnstringifyLiteralToArgText(piece0, /*allowTopLevelComma=*/true);
          if (!decoded)
            return false;

          auto canonDecoded = CanonicalizeStringifyInversePayload(*decoded);
          auto canonExpected = CanonicalizeStringifyInversePayload(expected0);
          if (!canonDecoded || !canonExpected)
            return false;

          return StringRef(*canonDecoded).trim() ==
                     StringRef(*decoded).trim() &&
                 StringRef(*canonExpected).trim() == expected0.trim() &&
                 *canonDecoded == *canonExpected;
        };

        auto pieceMatchesWrapperLogicalInput =
            [&](const WrapperChainCertificate &wrapper,
                StringRef piece0) -> bool {
          if (!pieceMatchesWrapperCertificate(wrapper, piece0))
            return false;

          const StringRef logical = StringRef(wrapper.logicalInputText).trim();
          switch (wrapper.kind) {
          case WrapperChainKind::Exact:
            return piece0.trim() == logical;
          case WrapperChainKind::StringLiteral:
          case WrapperChainKind::WideStringLiteral:
            return literalDecodesToCanonicalLogicalInput(piece0, logical);
          }
          llvm_unreachable("invalid WrapperChainKind");
        };

        auto pieceMatchesAnyTrimmedCandidate =
            [&](StringRef piece0,
                const SmallVectorImpl<std::string> &candidates) -> bool {
          StringRef piece = piece0.trim();
          for (const auto &cand : candidates)
            if (piece == StringRef(cand).trim())
              return true;
          return false;
        };

        auto collectSlotSemanticRewriteCertificates =
            [&](const LexicalChildPlaceholder &slot,
                const WrapperChainCertificate &wrapper, StringRef piece0)
            -> SmallVector<SlotSemanticRewriteCertificate, 4> {
          SmallVector<SlotSemanticRewriteCertificate, 4> out;
          const StringRef piece = piece0.trim();

          auto addUnique = [&](SlotRewriteDecisionKind kind,
                               StringRef rebuilt0) {
            SlotSemanticRewriteCertificate cert;
            cert.kind = SlotSemanticRewriteCertificateKind::Unique;
            cert.decision.kind = kind;
            cert.decision.observedText = piece.str();
            cert.decision.rebuiltText = rebuilt0.str();
            cert.wrapperKind = wrapper.kind;
            cert.wrapperSource = wrapper.source;
            cert.logicalInputText = wrapper.logicalInputText;
            for (const auto &existing : out)
              if (existing == cert)
                return;
            out.push_back(std::move(cert));
          };

          if (preferredChildSyntax && slot.child) {
            auto it = preferredChildSyntax->find(slot.child->id);
            if (it != preferredChildSyntax->end() &&
                pieceMatchesWrapperCertificate(wrapper, piece)) {
              bool compatible = false;
              const StringRef preferredSyntax = StringRef(it->second).trim();
              switch (wrapper.source) {
              case WrapperObservedSource::ChildRawInvocation:
                switch (wrapper.kind) {
                case WrapperChainKind::Exact:
                  compatible = piece == preferredSyntax;
                  break;
                case WrapperChainKind::StringLiteral:
                case WrapperChainKind::WideStringLiteral:
                  compatible = literalDecodesToCanonicalLogicalInput(
                      piece, preferredSyntax);
                  break;
                }
                break;

              case WrapperObservedSource::ChildExpansion:
                switch (wrapper.kind) {
                case WrapperChainKind::Exact:
                  compatible = pieceMatchesAnyTrimmedCandidate(
                      piece, slot.newExpansionCandidates);
                  break;
                case WrapperChainKind::StringLiteral:
                case WrapperChainKind::WideStringLiteral:
                  compatible = llvm::any_of(
                      slot.newExpansionCandidates,
                      [&](const std::string &cand) {
                        return literalDecodesToCanonicalLogicalInput(piece,
                                                                     cand);
                      });
                  break;
                }
                break;
              }

              if (compatible)
                addUnique(SlotRewriteDecisionKind::PreferredChildSyntax,
                          preferredSyntax);
            }
          }

          if (!slot.rawInvocationText.empty() && !slot.observedForms.empty()) {
            for (const auto &form : slot.observedForms) {
              if (!pieceMatchesWrapperLogicalInput(form, piece))
                continue;
              addUnique(SlotRewriteDecisionKind::PreserveRawInvocation,
                        slot.rawInvocationText);
            }
          }

          return out;
        };

        auto addNewSolution =
            [&](const SmallVectorImpl<SlotRewriteDecision> &parts,
                const SmallVectorImpl<SlotSemanticRewriteCertificate>
                    &slotCertificates) {
              SmallVector<SlotRewriteDecision, 8> copy(parts.begin(),
                                                       parts.end());
              for (const auto &existing : newSolutions)
                if (existing == copy)
                  return;
              newSolutions.push_back(std::move(copy));
              newSolutionCertificates.emplace_back(slotCertificates.begin(),
                                                   slotCertificates.end());
            };

        auto rebuildFromSolution =
            [&](const SmallVectorImpl<SlotRewriteDecision> &sol)
            -> std::string {
          std::string rebuilt;
          for (size_t i = 0; i < cert.slots.size(); ++i) {
            rebuilt += cert.literals[i];
            rebuilt += sol[i].rebuiltText;
          }
          rebuilt += cert.literals.back();
          return rebuilt;
        };

        auto solutionsCollapseToSameRebuilt = [&]() -> bool {
          if (newSolutions.empty())
            return false;
          std::string rebuilt = rebuildFromSolution(newSolutions[0]);
          for (size_t i = 1; i < newSolutions.size(); ++i)
            if (rebuildFromSolution(newSolutions[i]) != rebuilt)
              return false;
          return true;
        };

        auto solveNew = [&](auto &&self, size_t idx, size_t pos) -> void {
          if (newSolutions.size() > 1)
            return;
          const StringRef lit = cert.literals[idx];
          if (pos > observedNew.size() ||
              !observedNew.drop_front(pos).starts_with(lit))
            return;
          pos += lit.size();
          if (idx == cert.slots.size()) {
            if (pos == observedNew.size())
              addNewSolution(newParts, newPartCertificates);
            return;
          }

          const auto &slot = cert.slots[idx];
          const WrapperChainCertificate wrapper =
              (idx < cert.chosenObservedFormIdx.size() &&
               cert.chosenObservedFormIdx[idx] < slot.observedForms.size())
                  ? slot.observedForms[cert.chosenObservedFormIdx[idx]]
                  : WrapperChainCertificate{};
          size_t minRemain = 0;
          for (size_t j = idx + 1; j < cert.literals.size(); ++j)
            minRemain += cert.literals[j].size();
          if (pos + minRemain > observedNew.size())
            return;
          const size_t maxLen = observedNew.size() - pos - minRemain;
          const StringRef rest = observedNew.drop_front(pos);
          const StringRef nextLit = cert.literals[idx + 1];

          auto enumerateSlotPieces = [&](auto &&emitPiece) {
            if (!nextLit.empty()) {
              enumerateTopLevelLiteralMatchesInRefoldText(
                  rest, nextLit, maxLen, [&](size_t found) {
                    StringRef piece = rest.take_front(found);
                    if (!isBalancedRefoldFragment(piece))
                      return;
                    emitPiece(piece);
                    if (newSolutions.size() > 1)
                      return;
                  });
            } else {
              // The normal case: split the rewritten core around the original
              // literal delimiters and require a unique segmentation.
              enumerateTopLevelBalancedCutPoints(rest, [&](unsigned cut) {
                const size_t len = static_cast<size_t>(cut);
                if (len > maxLen)
                  return;
                if (!isLikelyTokenBoundaryInRefoldText(rest, len) ||
                    !isBalancedRefoldFragment(rest.take_front(len)))
                  return;
                emitPiece(rest.take_front(len));
                if (newSolutions.size() > 1)
                  return;
              });
            }
          };

          bool triedSemanticPreserve = false;
          enumerateSlotPieces([&](StringRef piece) {
            auto semanticCerts =
                collectSlotSemanticRewriteCertificates(slot, wrapper, piece);
            if (semanticCerts.empty())
              return;
            triedSemanticPreserve = true;
            for (const auto &semanticCert : semanticCerts) {
              newParts.push_back(semanticCert.decision);
              newPartCertificates.push_back(semanticCert);
              self(self, idx + 1, pos + piece.size());
              newPartCertificates.pop_back();
              newParts.pop_back();
              if (newSolutions.size() > 1)
                return;
            }
          });
          if (triedSemanticPreserve && solutionsCollapseToSameRebuilt())
            return;

          enumerateSlotPieces([&](StringRef piece) {
            SlotSemanticRewriteCertificate fallbackCert;
            fallbackCert.kind = SlotSemanticRewriteCertificateKind::Unique;
            fallbackCert.decision =
                SlotRewriteDecision{SlotRewriteDecisionKind::PassthroughFlatten,
                                    piece.str(), piece.str()};
            fallbackCert.wrapperKind = wrapper.kind;
            fallbackCert.wrapperSource = wrapper.source;
            fallbackCert.logicalInputText = wrapper.logicalInputText;
            newParts.push_back(fallbackCert.decision);
            newPartCertificates.push_back(fallbackCert);
            self(self, idx + 1, pos + piece.size());
            newPartCertificates.pop_back();
            newParts.pop_back();
          });
        };
        solveNew(solveNew, 0, 0);
        if (newSolutions.empty()) {
          argCert.detail = "new arg did not admit any structurally valid slot "
                           "reconstruction";
          return argCert;
        }

        std::string rebuilt = rebuildFromSolution(newSolutions[0]);
        for (size_t i = 1; i < newSolutions.size(); ++i)
          if (rebuildFromSolution(newSolutions[i]) != rebuilt) {
            argCert.detail = "new arg admitted multiple non-equivalent "
                             "structural reconstructions";
            return argCert;
          }

        argCert.slotDecisions.assign(newSolutions[0].begin(),
                                     newSolutions[0].end());
        if (!newSolutionCertificates.empty())
          argCert.slotCertificates.assign(newSolutionCertificates[0].begin(),
                                          newSolutionCertificates[0].end());
        argCert.rawArgNewText = rebuilt;
        argCert.kind = (StringRef(argCert.rawArgNewText).trim() ==
                        StringRef(argCert.rawArgOldText).trim())
                           ? ArgSemanticRewriteCertificateKind::NoChange
                           : ArgSemanticRewriteCertificateKind::Unique;
        return argCert;
      };

      auto buildObservedArgRewriteCertificate =
          [&](const RefoldModel::MacroInvocation &parent, uint32_t parentFormal,
              StringRef observedOld0, StringRef observedNew0,
              const DenseMap<uint64_t, std::string> *preferredChildSyntax)
          -> ArgSemanticRewriteCertificate {
        auto invertibilityCert = buildArgInvertibilityCertificate(
            parent, parentFormal, observedOld0);
        if (!invertibilityCert) {
          ArgSemanticRewriteCertificate argCert;
          argCert.failure =
              ArgSemanticRewriteFailure::MissingStructuralTemplate;
          argCert.detail = "observed old arg text did not match a unique "
                           "structural template";
          return argCert;
        }
        return buildArgSemanticRewriteCertificate(
            *invertibilityCert, observedNew0, preferredChildSyntax);
      };

      struct FormalTextPair {
        std::string oldText;
        std::string newText;
      };

      struct ObservedFormalConstraint {
        std::string oldText;
        std::string newText;
      };

      auto formatFormalTextPairMap =
          [&](const DenseMap<uint32_t, FormalTextPair> &formals) {
            SmallVector<uint32_t, 8> argIdxs;
            argIdxs.reserve(formals.size());
            for (const auto &KV : formals)
              argIdxs.push_back(KV.first);
            llvm::sort(argIdxs);

            std::string out;
            raw_string_ostream os(out);
            os << "{";
            for (size_t i = 0; i < argIdxs.size(); ++i) {
              if (i)
                os << ", ";
              const uint32_t argIdx = argIdxs[i];
              const auto it = formals.find(argIdx);
              os << argIdx << ":'"
                 << stringutils::showWSWithClip(it->second.oldText, 80)
                 << "'->'"
                 << stringutils::showWSWithClip(it->second.newText, 80)
                 << "'";
            }
            os << "}";
            return os.str();
          };

      enum class FormalRewriteCertificateKind {
        NoChange,
        Unique,
        Invalid,
      };

      enum class FormalRewriteFailure {
        None,
        MissingArgumentText,
        MissingStructuralTemplate,
        RawRewriteNotCertifiable,
        MergeConflict,
        ArityChange,
        OccurrenceMismatch,
        InteractionConflict,
      };

      enum class RawFormalValidationFailure {
        None,
        ArityChange,
        OccurrenceMismatch,
      };

      struct RawFormalValidationCertificate {
        bool valid = false;
        RawFormalValidationFailure failure = RawFormalValidationFailure::None;
        const RefoldModel::MacroInvocation *inv = nullptr;
        uint32_t argIdx = 0;
        std::string oldText;
        std::string newText;
        std::string detail;
      };

      enum class PasteRewriteValidationFailure {
        None,
        MissingInvocationText,
        MissingArgumentRanges,
        PasteMismatch,
      };

      struct PasteRewriteValidationCertificate {
        bool required = false;
        bool valid = true;
        bool deferred = false;
        const RefoldModel::MacroInvocation *inv = nullptr;
        DenseMap<uint32_t, std::string> replacementByArgIdx;
        PasteRewriteValidationFailure failure =
            PasteRewriteValidationFailure::None;
        std::string detail;
      };

      struct FormalRewriteCertificate {
        FormalRewriteCertificateKind kind =
            FormalRewriteCertificateKind::Invalid;
        FormalRewriteFailure failure = FormalRewriteFailure::None;
        const RefoldModel::MacroInvocation *inv = nullptr;
        uint32_t argIdx = 0;
        std::string oldText;
        std::string newText;
        SmallVector<FormalTextPair, 2> candidateRewrites;
        SmallVector<ArgSemanticRewriteCertificate, 2> argRewriteCertificates;
        SmallVector<SemanticInteractionCertificate, 2> interactionCertificates;
        FormalInteractionConsistencyCertificate interactionConsistency;
        RawFormalValidationCertificate validation;
        std::string detail;
      };

      SmallVector<diffutils::Hunk, 1> tokenHunksForCheck;
      tokenHunksForCheck.push_back(h);
      ArrayRef<diffutils::Hunk> tokenHunksAR(tokenHunksForCheck);

      std::function<std::optional<std::string>(StringRef,
                                               ArrayRef<FormalTextPair>)>
          mergeCompatibleFormalRewrites;

      auto buildRawFormalValidationCertificate =
          [&](const RefoldModel::MacroInvocation &inv, uint32_t argIdx,
              StringRef oldText0, StringRef newText0,
              StringRef traceStage,
              bool skipOccurrenceConsistency = false)
          -> RawFormalValidationCertificate {
        RawFormalValidationCertificate cert;
        cert.inv = &inv;
        cert.argIdx = argIdx;
        cert.oldText = oldText0.trim().str();
        cert.newText = newText0.trim().str();

        const StringRef oldText = StringRef(cert.oldText).trim();
        const StringRef newText = StringRef(cert.newText).trim();
        if (oldText == newText) {
          cert.valid = true;
          return cert;
        }

        if (!isVariadicFormalInInvocation(inv, argIdx) &&
            hasTopLevelCommaInRefoldText(newText)) {
          cert.failure = RawFormalValidationFailure::ArityChange;
          cert.detail = formatv(
                            "{0}: inv id={1} name={2} argIdx={3} arity "
                            "safety failed",
                            traceStage, inv.id, inv.name, argIdx)
                            .str();
          return cert;
        }

        if (skipOccurrenceConsistency) {
          cert.valid = true;
          cert.detail = formatv(
                            "{0}: inv id={1} name={2} argIdx={3} occurrence "
                            "consistency deferred to semantic certificate "
                            "pipeline",
                            traceStage, inv.id, inv.name, argIdx)
                            .str();
          return cert;
        }

        if (!MacroArgReplacementMatchesAllOccurrencesInBIgnorePasteSemanticProof(
                inv, argIdx, oldText, newText, tokenHunksAR)) {
          cert.failure = RawFormalValidationFailure::OccurrenceMismatch;
          cert.detail =
              formatv("{0}: inv id={1} name={2} argIdx={3} occurrence "
                      "consistency failed",
                      traceStage, inv.id, inv.name, argIdx)
                  .str();
          return cert;
        }

        cert.valid = true;
        return cert;
      };

      auto buildPasteRewriteValidationCertificate =
          [&](const RefoldModel::MacroInvocation &inv,
              const DenseMap<uint32_t, std::string> &replacementByArgIdx,
              StringRef traceStage,
              std::optional<StringRef> callsiteTextOverride = std::nullopt,
              ArrayRef<std::pair<size_t, size_t>>
                  callsiteArgRangesOverride =
                      ArrayRef<std::pair<size_t, size_t>>())
          -> PasteRewriteValidationCertificate {
        PasteRewriteValidationCertificate cert;
        cert.inv = &inv;
        cert.replacementByArgIdx = replacementByArgIdx;

        for (const auto &KV : replacementByArgIdx) {
          const uint32_t argIdx = KV.first;
          for (const auto &ps : inv.pasteSpans) {
            if (ps.argIdx == argIdx) {
              cert.required = true;
              break;
            }
          }
          if (cert.required)
            break;
        }

        if (!cert.required)
          return cert;

        StringRef callsiteText;
        ArrayRef<std::pair<size_t, size_t>> callsiteArgRanges;
        std::optional<SmallVector<std::pair<size_t, size_t>, 8>>
            ownedArgRanges;

        if (callsiteTextOverride) {
          callsiteText = *callsiteTextOverride;
          if (callsiteArgRangesOverride.empty()) {
            cert.valid = false;
            cert.failure = PasteRewriteValidationFailure::MissingArgumentRanges;
            cert.detail =
                formatv("{0}: paste consistency unavailable: inv id={1} "
                        "name={2} arg ranges unavailable",
                        traceStage, inv.id, inv.name)
                    .str();
            return cert;
          }
          callsiteArgRanges = callsiteArgRangesOverride;
        } else {
          if (!inv.invText) {
            cert.valid = false;
            cert.failure = PasteRewriteValidationFailure::MissingInvocationText;
            cert.detail =
                formatv("{0}: paste consistency unavailable: inv id={1} "
                        "name={2} hasInvText=0",
                        traceStage, inv.id, inv.name)
                    .str();
            return cert;
          }
          callsiteText = StringRef(*inv.invText);
          auto invArgRangesOpt =
              GetMacroInvocationFormalArgContentRanges(inv, callsiteText);
          if (!invArgRangesOpt) {
            cert.valid = false;
            cert.failure = PasteRewriteValidationFailure::MissingArgumentRanges;
            cert.detail =
                formatv("{0}: paste consistency unavailable: inv id={1} "
                        "name={2} arg ranges unavailable",
                        traceStage, inv.id, inv.name)
                    .str();
            return cert;
          }
          ownedArgRanges.emplace(invArgRangesOpt->begin(),
                                 invArgRangesOpt->end());
          callsiteArgRanges = *ownedArgRanges;
        }

        if (!PasteArgReplacementsMatchAllPasteTokensInB(
                inv, callsiteText, callsiteArgRanges,
                cert.replacementByArgIdx)) {
          cert.valid = false;
          cert.failure = PasteRewriteValidationFailure::PasteMismatch;
          cert.detail =
              formatv("{0}: paste-token consistency failed: inv id={1} "
                      "name={2} touchedArgs={3}",
                      traceStage, inv.id, inv.name,
                      cert.replacementByArgIdx.size())
                  .str();
          return cert;
        }

        return cert;
      };

      auto buildSemanticInteractionCertificate =
          [&](const RefoldModel::MacroInvocation &inv, uint32_t argIdx,
              const ArgSemanticRewriteCertificate &argCert,
              StringRef traceStage) -> SemanticInteractionCertificate {
        SemanticInteractionCertificate cert;
        cert.inv = &inv;
        cert.argIdx = argIdx;
        cert.slotCertificates.assign(argCert.slotCertificates.begin(),
                                     argCert.slotCertificates.end());

        auto addLogicalInput = [&](StringRef logical0) {
          const std::string normalized = logical0.trim().str();
          if (normalized.empty())
            return;
          for (const auto &existing : cert.canonicalLogicalInputs)
            if (existing == normalized)
              return;
          cert.canonicalLogicalInputs.push_back(normalized);
        };

        cert.touchesPaste = InvocationArgTouchesPaste(inv, argIdx);

        for (const auto &slotCert : argCert.slotCertificates) {
          switch (slotCert.decision.kind) {
          case SlotRewriteDecisionKind::PreferredChildSyntax:
            cert.usesPreferredChildSyntax = true;
            break;
          case SlotRewriteDecisionKind::PreserveRawInvocation:
            cert.usesRawInvocationPreservation = true;
            break;
          case SlotRewriteDecisionKind::PassthroughFlatten:
            cert.usesPassthroughFlatten = true;
            break;
          }

          if (slotCert.wrapperSource ==
              WrapperObservedSource::ChildRawInvocation)
            cert.usesRawChildInvocationLogicalInput = true;

          switch (slotCert.wrapperKind) {
          case WrapperChainKind::Exact:
            addLogicalInput(slotCert.logicalInputText);
            break;
          case WrapperChainKind::StringLiteral:
          case WrapperChainKind::WideStringLiteral: {
            cert.usesStringify = true;
            if (slotCert.wrapperKind == WrapperChainKind::WideStringLiteral)
              cert.usesWideStringify = true;
            auto canon =
                CanonicalizeStringifyInversePayload(slotCert.logicalInputText);
            if (!canon || StringRef(*canon).trim() !=
                              StringRef(slotCert.logicalInputText).trim()) {
              cert.valid = false;
              cert.failure =
                  SemanticInteractionFailure::NonCanonicalLogicalInput;
              cert.detail = formatv(
                                "{0}: inv id={1} name={2} argIdx={3} "
                                "interaction canonical logical payload "
                                "failed",
                                traceStage, inv.id, inv.name, argIdx)
                                .str();
              return cert;
            }
            addLogicalInput(*canon);
            break;
          }
          }
        }

        const bool hasChildSyntax = cert.usesPreferredChildSyntax ||
                                    cert.usesRawInvocationPreservation;
        const bool hasStringify = cert.usesStringify;
        const bool hasPaste = cert.touchesPaste;

        if (hasPaste && hasStringify && cert.usesWideStringify) {
          cert.kind = SemanticInteractionKind::WideStringifyPaste;
        } else if (hasPaste && hasStringify) {
          cert.kind = SemanticInteractionKind::StringifyPaste;
        } else if (hasPaste && cert.usesRawInvocationPreservation) {
          cert.kind = SemanticInteractionKind::RawInvocationPaste;
        } else if (hasPaste && cert.usesPreferredChildSyntax) {
          cert.kind = SemanticInteractionKind::ChildSyntaxPaste;
        } else if (hasPaste) {
          cert.kind = SemanticInteractionKind::Paste;
        } else if (hasStringify && cert.usesWideStringify) {
          cert.kind = SemanticInteractionKind::WideStringify;
        } else if (hasStringify) {
          cert.kind = SemanticInteractionKind::Stringify;
        } else if (cert.usesRawInvocationPreservation) {
          cert.kind = SemanticInteractionKind::RawInvocation;
        } else if (cert.usesPreferredChildSyntax) {
          cert.kind = SemanticInteractionKind::ChildSyntax;
        } else {
          cert.kind = SemanticInteractionKind::Plain;
        }

        if (hasChildSyntax && hasStringify && hasPaste)
          cert.kind = SemanticInteractionKind::Mixed;

        cert.detail = formatv(
                          "semantic interaction: inv id={0} name={1} argIdx={2} "
                          "kind={3} slots={4} logicalInputs={5} paste={6} "
                          "rawChildInput={7}",
                          inv.id, inv.name, argIdx,
                          static_cast<unsigned>(cert.kind),
                          cert.slotCertificates.size(),
                          cert.canonicalLogicalInputs.size(),
                          cert.touchesPaste ? 1 : 0,
                          cert.usesRawChildInvocationLogicalInput ? 1 : 0)
                          .str();
        return cert;
      };

      auto buildSemanticInteractionSignature =
          [&](const SemanticInteractionCertificate &interaction)
          -> SemanticInteractionSignature {
        SemanticInteractionSignature sig;
        sig.touchesPaste = interaction.touchesPaste;
        sig.usesPreferredChildSyntax = interaction.usesPreferredChildSyntax;
        sig.usesRawInvocationPreservation =
            interaction.usesRawInvocationPreservation;
        sig.usesPassthroughFlatten = interaction.usesPassthroughFlatten;
        sig.usesStringify = interaction.usesStringify;
        sig.usesWideStringify = interaction.usesWideStringify;
        sig.usesRawChildInvocationLogicalInput =
            interaction.usesRawChildInvocationLogicalInput;
        sig.canonicalLogicalInputs.assign(
            interaction.canonicalLogicalInputs.begin(),
            interaction.canonicalLogicalInputs.end());
        llvm::sort(sig.canonicalLogicalInputs);
        sig.canonicalLogicalInputs.erase(
            std::unique(sig.canonicalLogicalInputs.begin(),
                        sig.canonicalLogicalInputs.end()),
            sig.canonicalLogicalInputs.end());
        return sig;
      };

      auto buildFormalInteractionConsistencyCertificate =
          [&](const RefoldModel::MacroInvocation &inv, uint32_t argIdx,
              ArrayRef<SemanticInteractionCertificate> interactions,
              StringRef traceStage) -> FormalInteractionConsistencyCertificate {
        FormalInteractionConsistencyCertificate cert;
        cert.inv = &inv;
        cert.argIdx = argIdx;
        cert.interactions.assign(interactions.begin(), interactions.end());

        if (interactions.empty()) {
          cert.detail = formatv(
                            "{0}: inv id={1} name={2} argIdx={3} semantic "
                            "interaction convergence vacuously satisfied",
                            traceStage, inv.id, inv.name, argIdx)
                            .str();
          return cert;
        }

        cert.signature =
            buildSemanticInteractionSignature(interactions.front());
        for (size_t i = 1; i < interactions.size(); ++i) {
          auto sig = buildSemanticInteractionSignature(interactions[i]);
          if (!(sig == cert.signature)) {
            cert.valid = false;
            cert.failure =
                FormalInteractionConsistencyFailure::DivergentSemanticEvidence;
            cert.detail =
                formatv("{0}: inv id={1} name={2} argIdx={3} semantic "
                        "interaction evidence diverged across "
                        "observations",
                        traceStage, inv.id, inv.name, argIdx)
                    .str();
            return cert;
          }
        }

        cert.detail = formatv(
                          "{0}: inv id={1} name={2} argIdx={3} semantic "
                          "interaction convergence satisfied "
                          "logicalInputs={4} paste={5} stringify={6} wide={7} "
                          "rawInvocation={8} childSyntax={9} passthrough={10}",
                          traceStage, inv.id, inv.name, argIdx,
                          cert.signature.canonicalLogicalInputs.size(),
                          cert.signature.touchesPaste ? 1 : 0,
                          cert.signature.usesStringify ? 1 : 0,
                          cert.signature.usesWideStringify ? 1 : 0,
                          cert.signature.usesRawInvocationPreservation ? 1 : 0,
                          cert.signature.usesPreferredChildSyntax ? 1 : 0,
                          cert.signature.usesPassthroughFlatten ? 1 : 0)
                          .str();
        return cert;
      };

      auto buildObservedFormalRewriteCertificate =
          [&](const RefoldModel::MacroInvocation &inv, uint32_t argIdx,
              ArrayRef<ObservedFormalConstraint> observedConstraints,
              const DenseMap<uint64_t, std::string> *preferredChildSyntax,
              StringRef traceStage) -> FormalRewriteCertificate {
        FormalRewriteCertificate cert;
        cert.inv = &inv;
        cert.argIdx = argIdx;

        auto argText = getInvocationArgText(inv, argIdx);
        if (!argText) {
          cert.failure = FormalRewriteFailure::MissingArgumentText;
          cert.detail =
              formatv("{0}: inv id={1} name={2} argIdx={3} text unavailable",
                      traceStage, inv.id, inv.name, argIdx)
                  .str();
          return cert;
        }

        const StringRef oldTrim = argText->trim();
        for (const auto &constraint : observedConstraints) {
          auto argRewriteCert = buildObservedArgRewriteCertificate(
              inv, argIdx, constraint.oldText, constraint.newText,
              preferredChildSyntax);
          if (argRewriteCert.kind ==
              ArgSemanticRewriteCertificateKind::Invalid) {
            cert.failure =
                argRewriteCert.failure ==
                        ArgSemanticRewriteFailure::MissingStructuralTemplate
                    ? FormalRewriteFailure::MissingStructuralTemplate
                    : FormalRewriteFailure::RawRewriteNotCertifiable;
            cert.detail = formatv(
                              "{0}: inv id={1} name={2} argIdx={3} raw "
                              "rewrite not certifiable ({4})",
                              traceStage, inv.id, inv.name, argIdx,
                              argRewriteCert.detail)
                              .str();
            return cert;
          }

          auto interactionCert = buildSemanticInteractionCertificate(
              inv, argIdx, argRewriteCert, traceStage);
          if (!interactionCert.valid) {
            cert.failure = FormalRewriteFailure::RawRewriteNotCertifiable;
            cert.detail = interactionCert.detail;
            return cert;
          }

          cert.argRewriteCertificates.push_back(argRewriteCert);
          cert.interactionCertificates.push_back(interactionCert);
          cert.candidateRewrites.push_back(FormalTextPair{
              oldTrim.str(),
              StringRef(argRewriteCert.rawArgNewText).trim().str()});
        }

        cert.interactionConsistency =
            buildFormalInteractionConsistencyCertificate(
                inv, argIdx, cert.interactionCertificates, traceStage);
        if (!cert.interactionConsistency.valid) {
          cert.failure = FormalRewriteFailure::InteractionConflict;
          cert.detail = cert.interactionConsistency.detail;
          return cert;
        }

        auto merged =
            mergeCompatibleFormalRewrites(oldTrim, cert.candidateRewrites);
        if (!merged) {
          cert.failure = FormalRewriteFailure::MergeConflict;
          cert.detail =
              formatv("{0}: inv id={1} name={2} argIdx={3} rewrite merge "
                      "conflicted",
                      traceStage, inv.id, inv.name, argIdx)
                  .str();
          return cert;
        }

        const StringRef mergedTrim = StringRef(*merged).trim();
        cert.oldText = oldTrim.str();
        cert.newText = mergedTrim.str();
        if (mergedTrim == oldTrim) {
          cert.kind = FormalRewriteCertificateKind::NoChange;
          cert.detail = formatv(
                            "{0}: inv id={1} name={2} argIdx={3} certified "
                            "rewrite collapsed to no-change old='{4}' new='{5}' "
                            "argRewriteCerts={6} interactionCerts={7}",
                            traceStage, inv.id, inv.name, argIdx, oldTrim,
                            mergedTrim,
                            cert.argRewriteCertificates.size(),
                            cert.interactionCertificates.size())
                            .str();
          return cert;
        }

        const bool deferOccurrenceConsistency =
            cert.interactionConsistency.signature.usesPreferredChildSyntax ||
            cert.interactionConsistency.signature
                .usesRawInvocationPreservation ||
            cert.interactionConsistency.signature.usesStringify ||
            cert.interactionConsistency.signature.usesWideStringify ||
            cert.interactionConsistency.signature.touchesPaste;

        cert.validation = buildRawFormalValidationCertificate(
            inv, argIdx, oldTrim, mergedTrim, traceStage,
            deferOccurrenceConsistency);
        if (!cert.validation.valid) {
          switch (cert.validation.failure) {
          case RawFormalValidationFailure::ArityChange:
            cert.failure = FormalRewriteFailure::ArityChange;
            break;
          case RawFormalValidationFailure::OccurrenceMismatch:
            cert.failure = FormalRewriteFailure::OccurrenceMismatch;
            break;
          case RawFormalValidationFailure::None:
            cert.failure = FormalRewriteFailure::None;
            break;
          }
          cert.detail = cert.validation.detail;
          return cert;
        }

        cert.kind = FormalRewriteCertificateKind::Unique;
        return cert;
      };

      auto tryLexicalChildBridge =
          [&](const RefoldModel::MacroInvocation &parent,
              const RefoldModel::MacroInvocation &child,
              const std::string &rewrittenChildSyntax)
          -> std::optional<DenseMap<uint32_t, FormalTextPair>> {
        DenseMap<uint32_t, FormalTextPair> out;
        if (rewrittenChildSyntax.empty())
          return std::nullopt;

        std::optional<uint32_t> matchedFormal;
        std::optional<LexicalChildPlaceholder> matchedSlot;
        for (uint32_t parentFormal = 0;
             parentFormal < parent.invArgRanges.size(); ++parentFormal) {
          auto argInfo = getTrimmedInvocationArgInfo(parent, parentFormal);
          if (!argInfo)
            continue;
          auto slots = getTopLevelLexicalChildrenInArg(parent, parentFormal);
          for (const auto &slot : slots) {
            if (!slot.child || slot.child->id != child.id)
              continue;
            if (matchedFormal)
              return std::nullopt;
            matchedFormal = parentFormal;
            matchedSlot = slot;
          }
        }

        if (!matchedFormal || !matchedSlot)
          return std::nullopt;

        auto argInfo = getTrimmedInvocationArgInfo(parent, *matchedFormal);
        if (!argInfo)
          return std::nullopt;

        if (matchedSlot->relEnd < matchedSlot->relBegin ||
            matchedSlot->relEnd > argInfo->text.size())
          return std::nullopt;

        std::string rewrittenArg = stringutils::replaceRange(
            argInfo->text, matchedSlot->relBegin, matchedSlot->relEnd,
            rewrittenChildSyntax);
        out[*matchedFormal] =
            FormalTextPair{StringRef(argInfo->text).trim().str(),
                           StringRef(rewrittenArg).trim().str()};
        return out;
      };

      auto toSingleCharRefs = [&](StringRef s) {
        std::vector<StringRef> refs;
        refs.reserve(s.size());
        for (size_t i = 0; i < s.size(); ++i)
          refs.push_back(s.substr(i, 1));
        return refs;
      };

      struct FormalRewriteHunk {
        uint64_t oldBegin;
        uint64_t oldEnd;
        std::string repl;
      };

      mergeCompatibleFormalRewrites =
          [&](StringRef baseOld0, ArrayRef<FormalTextPair> rewrites)
          -> std::optional<std::string> {
        const StringRef baseOld = baseOld0.trim();
        std::vector<FormalRewriteHunk> merged;

        for (const auto &rewrite : rewrites) {
          if (StringRef(rewrite.oldText).trim() != baseOld)
            return std::nullopt;

          const StringRef newText = StringRef(rewrite.newText).trim();
          if (newText == baseOld)
            continue;

          std::vector<StringRef> aRefs = toSingleCharRefs(baseOld);
          std::vector<StringRef> bRefs = toSingleCharRefs(newText);
          auto steps = diffutils::diff(aRefs, bRefs);
          auto hunks = diffutils::coalesce(steps);

          for (const auto &hunk : hunks) {
            FormalRewriteHunk piece{hunk.aStart, hunk.aEnd,
                                    newText.slice((size_t)hunk.bStart,
                                                  (size_t)hunk.bEnd)
                                        .str()};

            auto sameHunk = [&](const FormalRewriteHunk &a,
                                const FormalRewriteHunk &b) {
              return a.oldBegin == b.oldBegin && a.oldEnd == b.oldEnd &&
                     a.repl == b.repl;
            };

            auto overlaps = [&](const FormalRewriteHunk &a,
                                const FormalRewriteHunk &b) {
              return a.oldBegin < b.oldEnd && b.oldBegin < a.oldEnd;
            };

            auto it = std::lower_bound(
                merged.begin(), merged.end(), piece.oldBegin,
                [](const FormalRewriteHunk &h, uint64_t pos) {
                  return h.oldBegin < pos;
                });

            if (it != merged.begin()) {
              const auto &prev = *std::prev(it);
              if (sameHunk(prev, piece))
                continue;
              if (overlaps(prev, piece))
                return std::nullopt;
            }
            if (it != merged.end()) {
              if (sameHunk(*it, piece))
                continue;
              if (overlaps(*it, piece))
                return std::nullopt;
            }

            merged.insert(it, std::move(piece));
          }
        }

        std::string out = baseOld.str();
        for (auto it = merged.rbegin(); it != merged.rend(); ++it)
          out.replace((size_t)it->oldBegin,
                      (size_t)(it->oldEnd - it->oldBegin), it->repl);
        return out;
      };

      auto mergeCompatibleCallsitePatchReplacements =
          [&](StringRef baseOld, ArrayRef<StringRef> replacements)
          -> std::optional<std::string> {
        std::vector<FormalRewriteHunk> merged;

        for (StringRef replText : replacements) {
          if (replText == baseOld)
            continue;

          std::vector<StringRef> aRefs = toSingleCharRefs(baseOld);
          std::vector<StringRef> bRefs = toSingleCharRefs(replText);
          auto steps = diffutils::diff(aRefs, bRefs);
          auto hunks = diffutils::coalesce(steps);

          for (const auto &hunk : hunks) {
            FormalRewriteHunk piece{hunk.aStart, hunk.aEnd,
                                    replText.slice((size_t)hunk.bStart,
                                                   (size_t)hunk.bEnd)
                                        .str()};

            auto sameHunk = [&](const FormalRewriteHunk &a,
                                const FormalRewriteHunk &b) {
              return a.oldBegin == b.oldBegin && a.oldEnd == b.oldEnd &&
                     a.repl == b.repl;
            };

            auto overlaps = [&](const FormalRewriteHunk &a,
                                const FormalRewriteHunk &b) {
              return a.oldBegin < b.oldEnd && b.oldBegin < a.oldEnd;
            };

            auto it = std::lower_bound(
                merged.begin(), merged.end(), piece.oldBegin,
                [](const FormalRewriteHunk &h, uint64_t pos) {
                  return h.oldBegin < pos;
                });

            if (it != merged.begin()) {
              const auto &prev = *std::prev(it);
              if (sameHunk(prev, piece))
                continue;
              if (overlaps(prev, piece))
                return std::nullopt;
            }
            if (it != merged.end()) {
              if (sameHunk(*it, piece))
                continue;
              if (overlaps(*it, piece))
                return std::nullopt;
            }

            merged.insert(it, std::move(piece));
          }
        }

        std::string out = baseOld.str();
        for (auto it = merged.rbegin(); it != merged.rend(); ++it)
          out.replace((size_t)it->oldBegin,
                      (size_t)(it->oldEnd - it->oldBegin), it->repl);
        return out;
      };

      enum class InvocationRewriteCertificateKind {
        NoChange,
        Unique,
        Invalid,
      };

      enum class InvocationRewriteFailure {
        None,
        ArityChange,
        OccurrenceMismatch,
        MissingInvocationText,
        MissingArgumentRanges,
        PasteMismatch,
      };

      struct CertifiedFormalRewrite {
        uint32_t argIdx = 0;
        std::string oldText;
        std::string newText;
      };

      struct InvocationRewriteCertificate {
        InvocationRewriteCertificateKind kind =
            InvocationRewriteCertificateKind::Invalid;
        InvocationRewriteFailure failure = InvocationRewriteFailure::None;
        const RefoldModel::MacroInvocation *inv = nullptr;
        SmallVector<CertifiedFormalRewrite, 4> rewrites;
        DenseMap<uint32_t, std::string> replacementByArgIdx;
        SmallVector<RawFormalValidationCertificate, 4> formalValidations;
        PasteRewriteValidationCertificate pasteValidation;
        bool touchesPaste = false;
        std::string rewrittenInvocationSyntax;
        std::string detail;
      };

      auto buildInvocationRewriteCertificate =
          [&](const RefoldModel::MacroInvocation &inv,
              const DenseMap<uint32_t, FormalTextPair> &formals,
              StringRef traceStage,
              std::optional<StringRef> callsiteTextOverride = std::nullopt,
              ArrayRef<std::pair<size_t, size_t>>
                  callsiteArgRangesOverride =
                      ArrayRef<std::pair<size_t, size_t>>(),
              ArrayRef<uint32_t> deferOccurrenceArgIdxs = ArrayRef<uint32_t>())
          -> InvocationRewriteCertificate {
        InvocationRewriteCertificate cert;
        cert.inv = &inv;

        SmallVector<uint32_t, 8> argOrder;
        argOrder.reserve(formals.size());
        for (const auto &KV : formals)
          argOrder.push_back(KV.first);
        llvm::sort(argOrder);

        for (uint32_t argIdx : argOrder) {
          auto it = formals.find(argIdx);
          if (it == formals.end())
            continue;

          StringRef oldText = StringRef(it->second.oldText).trim();
          StringRef newText = StringRef(it->second.newText).trim();

          const bool deferOccurrenceConsistency =
              llvm::is_contained(deferOccurrenceArgIdxs, argIdx);
          auto validation = buildRawFormalValidationCertificate(
              inv, argIdx, oldText, newText, traceStage,
              deferOccurrenceConsistency);
          cert.formalValidations.push_back(validation);
          if (!validation.valid) {
            switch (validation.failure) {
            case RawFormalValidationFailure::ArityChange:
              cert.failure = InvocationRewriteFailure::ArityChange;
              break;
            case RawFormalValidationFailure::OccurrenceMismatch:
              cert.failure = InvocationRewriteFailure::OccurrenceMismatch;
              break;
            case RawFormalValidationFailure::None:
              cert.failure = InvocationRewriteFailure::None;
              break;
            }
            cert.detail = validation.detail;
            return cert;
          }

          cert.replacementByArgIdx[argIdx] = newText.str();
          if (!cert.touchesPaste)
            cert.touchesPaste = InvocationArgTouchesPaste(inv, argIdx);

          if (oldText == newText)
            continue;

          cert.rewrites.push_back(
              CertifiedFormalRewrite{argIdx, oldText.str(), newText.str()});
        }

        auto providedArgIdxs = argOrder;
        auto carriedArgIdxs = CollectSortedUInt32Keys(cert.replacementByArgIdx);
        SmallVector<uint32_t, 8> changedArgIdxs;
        changedArgIdxs.reserve(cert.rewrites.size());
        for (const auto &rewrite : cert.rewrites)
          changedArgIdxs.push_back(rewrite.argIdx);
        llvm::sort(changedArgIdxs);
        auto requiredPasteArgIdxs =
            CollectSortedUniquePasteArgIdxs(inv.pasteSpans);
        auto missingSupportArgIdxs =
            ComputeSortedMissingUInt32s(requiredPasteArgIdxs, carriedArgIdxs);

        trace("macro/proof",
              "{0}: invocation support ledger enter inv id={1} name={2} "
              "provided={3} changed={4} carried={5} requiredPaste={6} "
              "missingSupport={7} deferredArgs={8}",
              traceStage, inv.id, inv.name, FormatUInt32List(providedArgIdxs),
              FormatUInt32List(changedArgIdxs),
              FormatUInt32List(carriedArgIdxs),
              FormatUInt32List(requiredPasteArgIdxs),
              FormatUInt32List(missingSupportArgIdxs),
              FormatUInt32List(deferOccurrenceArgIdxs));

        if (cert.rewrites.empty()) {
          cert.kind = InvocationRewriteCertificateKind::NoChange;
          trace("macro/proof",
                "{0}: invocation support ledger no-change inv id={1} "
                "name={2} provided={3} carried={4} requiredPaste={5} "
                "missingSupport={6}",
                traceStage, inv.id, inv.name,
                FormatUInt32List(providedArgIdxs),
                FormatUInt32List(carriedArgIdxs),
                FormatUInt32List(requiredPasteArgIdxs),
                FormatUInt32List(missingSupportArgIdxs));
          return cert;
        }

        cert.pasteValidation = buildPasteRewriteValidationCertificate(
            inv, cert.replacementByArgIdx, traceStage, callsiteTextOverride,
            callsiteArgRangesOverride);
        cert.touchesPaste = cert.pasteValidation.required;
        trace("macro/proof",
              "{0}: invocation support ledger replay inv id={1} name={2} "
              "provided={3} changed={4} carried={5} requiredPaste={6} "
              "missingSupport={7} pasteRequired={8} pasteValid={9} "
              "pasteDeferred={10}",
              traceStage, inv.id, inv.name, FormatUInt32List(providedArgIdxs),
              FormatUInt32List(changedArgIdxs),
              FormatUInt32List(carriedArgIdxs),
              FormatUInt32List(requiredPasteArgIdxs),
              FormatUInt32List(missingSupportArgIdxs),
              cert.pasteValidation.required ? 1 : 0,
              cert.pasteValidation.valid ? 1 : 0,
              cert.pasteValidation.deferred ? 1 : 0);
        if (!cert.pasteValidation.valid) {
          switch (cert.pasteValidation.failure) {
          case PasteRewriteValidationFailure::MissingInvocationText:
            cert.failure = InvocationRewriteFailure::MissingInvocationText;
            break;
          case PasteRewriteValidationFailure::MissingArgumentRanges:
            cert.failure = InvocationRewriteFailure::MissingArgumentRanges;
            break;
          case PasteRewriteValidationFailure::PasteMismatch:
            cert.failure = InvocationRewriteFailure::PasteMismatch;
            break;
          case PasteRewriteValidationFailure::None:
            cert.failure = InvocationRewriteFailure::None;
            break;
          }
          cert.detail = cert.pasteValidation.detail;
          return cert;
        }

        cert.kind = InvocationRewriteCertificateKind::Unique;
        return cert;
      };

      auto buildWrapperPlaceholderHopInvocationCertificate =
          [&](const RefoldModel::MacroInvocation &inv,
              const DenseMap<uint32_t, FormalTextPair> &formals,
              StringRef traceStage,
              std::optional<StringRef> callsiteTextOverride = std::nullopt,
              ArrayRef<std::pair<size_t, size_t>>
                  callsiteArgRangesOverride =
                      ArrayRef<std::pair<size_t, size_t>>(),
              ArrayRef<uint32_t> deferOccurrenceArgIdxs =
                  ArrayRef<uint32_t>())
          -> InvocationRewriteCertificate {
        auto cert = buildInvocationRewriteCertificate(
            inv, formals, traceStage, callsiteTextOverride,
            callsiteArgRangesOverride, deferOccurrenceArgIdxs);

        DenseMap<uint32_t, std::string> replByFormal;
        for (const auto &KV : formals) {
          StringRef oldText = StringRef(KV.second.oldText).trim();
          StringRef newText = StringRef(KV.second.newText).trim();
          if (oldText == newText)
            continue;
          replByFormal[KV.first] = newText.str();
        }

        if (!replByFormal.empty()) {
          if (auto rewritten =
                  buildRewrittenInvocationSyntax(inv, replByFormal))
            cert.rewrittenInvocationSyntax = std::move(*rewritten);
        }

        if (cert.kind != InvocationRewriteCertificateKind::Invalid ||
            cert.failure != InvocationRewriteFailure::PasteMismatch ||
            cert.rewrittenInvocationSyntax.empty())
          return cert;

        cert.kind = InvocationRewriteCertificateKind::Unique;
        cert.failure = InvocationRewriteFailure::None;
        cert.pasteValidation.valid = true;
        cert.pasteValidation.deferred = true;
        cert.pasteValidation.failure = PasteRewriteValidationFailure::None;
        cert.detail = formatv(
                          "{0}: wrapper placeholder-hop paste validation "
                          "deferred: inv id={1} name={2} touchedArgs={3}",
                          traceStage, inv.id, inv.name, cert.rewrites.size())
                          .str();
        cert.pasteValidation.detail = cert.detail;
        return cert;
      };

      auto buildRootFormalRewriteMapFromCallsiteReplacement =
          [&](StringRef baseText, StringRef newText)
          -> std::optional<DenseMap<uint32_t, FormalTextPair>> {
        auto newRangesOpt =
            GetMacroInvocationFormalArgContentRanges(m, newText);
        if (!newRangesOpt || newRangesOpt->size() != invArgRanges.size())
          return std::nullopt;

        DenseMap<uint32_t, FormalTextPair> formals;
        for (uint32_t argIdx = 0; argIdx < invArgRanges.size(); ++argIdx) {
          const auto &oldR = invArgRanges[argIdx];
          const auto &newR = (*newRangesOpt)[argIdx];
          if (oldR.first > oldR.second || oldR.second > baseText.size() ||
              newR.first > newR.second || newR.second > newText.size())
            return std::nullopt;

          StringRef oldArg =
              baseText.slice((size_t)oldR.first, (size_t)oldR.second).trim();
          StringRef newArg =
              newText.slice((size_t)newR.first, (size_t)newR.second).trim();
          if (oldArg == newArg)
            continue;

          formals[argIdx] = FormalTextPair{oldArg.str(), newArg.str()};
        }

        return formals;
      };

      enum class ParentConstraintDerivationFailure {
        None,
        MissingArgDeps,
        EmptyArgDeps,
        MissingArgRefs,
        TemplateNotCertifiable,
        InversionNotUnique,
        IncompleteDerivation,
      };

      struct ParentConstraintDerivationCertificate {
        bool valid = false;
        uint32_t childFormal = 0;
        ParentConstraintDerivationFailure failure =
            ParentConstraintDerivationFailure::None;
        SmallVector<std::pair<uint32_t, ObservedFormalConstraint>, 4>
            derivedConstraints;
        std::string detail;
      };

      std::function<ParentConstraintDerivationCertificate(
          const RefoldModel::MacroInvocation &, uint32_t, StringRef, StringRef,
          StringRef)>
          buildParentConstraintDerivationCertificate;

      // Attempt to interpret a group of paste-byte ranges in the coordinate
      // space of the current observed pasted surface.
      //
      // Why this is needed:
      //   For nested paste replay, child paste spans may be recorded in one of
      //   two coordinate systems:
      //
      //   (1) Already-local coordinates:
      //       The span byte ranges are already relative to the current
      //       observed surface we are trying to replay. In that case, we can
      //       use them directly.
      //
      //   (2) Enclosing-token coordinates:
      //       The child spans are still expressed relative to the larger
      //       enclosing pasted token owned by `surfaceOwner`. In that case,
      //       we must rebase them into the local observed surface before we
      //       can derive exact-shape replay constraints.
      //
      // This helper first checks whether every span in `group` already fits
      // within `observedSurface`. If so, it returns those ranges unchanged.
      //
      // Otherwise, it looks for the enclosing paste envelope on
      // `surfaceOwner->pasteSpans` that covers the same emitted token
      // `[tokBegin, tokEnd)`. If that enclosing envelope exists and its total
      // width exactly matches `observedSurface`, then each child span is
      // rebased by subtracting the enclosing base offset.
      //
      // The function returns:
      //   - rebased/local byte ranges on success
      //   - std::nullopt if the group cannot be interpreted unambiguously in
      //     the observed-surface coordinate space
      auto tryRebasePasteGroupToObservedSurface =
          [&](const RefoldModel::MacroInvocation *surfaceOwner,
              ArrayRef<const RefoldModel::PPArgSpan *> group,
              StringRef observedSurface, StringRef traceStage)
          -> std::optional<SmallVector<std::pair<uint64_t, uint64_t>, 4>> {
        SmallVector<std::pair<uint64_t, uint64_t>, 4> rebased;
        rebased.reserve(group.size());
        const uint64_t observedLen = observedSurface.size();

        // Fast path:
        // If every span already has a valid byte range fully inside the
        // current observed surface, then the group is already expressed in the
        // local coordinate space and does not need rebasing.
        bool fitsObservedSurface = true;
        for (const auto *sp : group) {
          if (!sp->byteBegin || !sp->byteEnd || *sp->byteBegin > *sp->byteEnd ||
              *sp->byteEnd > observedLen) {
            fitsObservedSurface = false;
            break;
          }
        }
        if (fitsObservedSurface) {
          for (const auto *sp : group)
            rebased.push_back({*sp->byteBegin, *sp->byteEnd});
          return rebased;
        }

        // If the spans do not already fit the observed surface, we can only
        // recover them if we know which enclosing invocation owns the larger
        // pasted token that these spans were originally measured against.
        if (!surfaceOwner)
          return std::nullopt;

        std::optional<uint64_t> base;
        std::optional<uint64_t> limit;
        const uint64_t tokBegin = group.front()->begin;
        const uint64_t tokEnd = group.front()->end;

        // Find the enclosing paste envelope on the surface owner for the same
        // emitted token `[tokBegin, tokEnd)`.
        //
        // Multiple owner spans may contribute to that token, so we compute the
        // minimal base and maximal limit across all matching owner paste spans.
        // The resulting [base, limit) interval is the full owner-local byte
        // range for the observed pasted surface.
        for (const auto &ownerSp : surfaceOwner->pasteSpans) {
          if (!ownerSp.byteBegin || !ownerSp.byteEnd)
            continue;
          if (ownerSp.begin != tokBegin || ownerSp.end != tokEnd)
            continue;
          base = base ? std::min<uint64_t>(*base, *ownerSp.byteBegin)
                      : *ownerSp.byteBegin;
          limit = limit ? std::max<uint64_t>(*limit, *ownerSp.byteEnd)
                        : *ownerSp.byteEnd;
        }

        // The enclosing owner envelope must:
        //   - exist
        //   - be well-formed
        //   - have width exactly equal to the current observed surface
        //
        // If not, we cannot safely interpret the child spans relative to the
        // local replay surface.
        if (!base || !limit || *limit < *base ||
            (*limit - *base) != observedLen)
          return std::nullopt;

        // Rebase each child span from owner-local/full-token coordinates into
        // observed-surface-local coordinates by subtracting the enclosing base.
        //
        // Each span must lie fully inside the enclosing owner envelope;
        // otherwise the replay would be inconsistent and must be rejected.
        for (const auto *sp : group) {
          if (!sp->byteBegin || !sp->byteEnd)
            return std::nullopt;
          if (*sp->byteBegin < *base || *sp->byteEnd < *sp->byteBegin ||
              *sp->byteEnd > *limit)
            return std::nullopt;
          rebased.push_back({*sp->byteBegin - *base, *sp->byteEnd - *base});
        }

        trace("macro/dag",
              "{0}: rebased pasted span group to observed surface owner id={1} "
              "name={2} base={3} limit={4} observedLen={5}",
              traceStage, surfaceOwner->id, surfaceOwner->name, *base, *limit,
              observedLen);
        return rebased;
      };

      auto tryBuildNestedPasteChainDerivation =
          [&](const RefoldModel::MacroInvocation &cur, uint32_t curFormal,
              StringRef curOld, StringRef curNew, StringRef traceStage)
          -> std::optional<ParentConstraintDerivationCertificate> {
        auto argText = getInvocationArgText(cur, curFormal);
        trace("macro/dag",
              "{0}: nested pasted-chain derivation enter child id={1} name={2} "
              "argIdx={3} argText='{4}' old='{5}' new='{6}'",
              traceStage, cur.id, cur.name, curFormal,
              argText ? argText->trim() : StringRef("<missing>"), curOld.trim(),
              curNew.trim());
        if (!argText || argText->empty())
          return std::nullopt;

        auto childIt = macroChildrenById_.find(cur.id);
        if (childIt == macroChildrenById_.end())
          return std::nullopt;

        const RefoldModel::MacroInvocation *nested = nullptr;
        SmallVector<uint32_t, 4> nestedMatches;
        for (const auto *cand : childIt->second) {
          if (!cand || !cand->invText)
            continue;
          if (StringRef(*cand->invText).trim() != argText->trim())
            continue;
          nestedMatches.push_back(cand->id);
          if (nested)
            return (trace("macro/dag",
                          "{0}: nested pasted-chain derivation ambiguous "
                          "nested child matches parent child id={1} name={2} "
                          "argIdx={3} matches={4}",
                          traceStage, cur.id, cur.name, curFormal,
                          FormatUInt32List(nestedMatches)),
                    std::nullopt);
          nested = cand;
        }
        if (!nested) {
          trace("macro/dag",
                "{0}: nested pasted-chain derivation found no exact nested "
                "child match for child id={1} name={2} argIdx={3}",
                traceStage, cur.id, cur.name, curFormal);
          return std::nullopt;
        }
        trace("macro/dag",
              "{0}: nested pasted-chain derivation matched nested child id={1} "
              "name={2} argIdx={3} nestedId={4} nestedName={5} nestedInv='{6}'",
              traceStage, cur.id, cur.name, curFormal, nested->id, nested->name,
              nested->invText ? StringRef(*nested->invText).trim()
                              : StringRef("<none>"));

        SmallVector<std::string, 4> oldExpansionCandidates =
            expansionTextCandidates(*nested, /*fromB=*/false);
        SmallVector<std::string, 4> matchingOldCandidates;
        for (const auto &cand : oldExpansionCandidates) {
          if (StringRef(cand).trim() == curOld.trim())
            matchingOldCandidates.push_back(cand);
        }
        trace("macro/dag",
              "{0}: nested pasted-chain derivation old expansion candidates "
              "nestedId={1} total={2} matchingOld={3} curOld='{4}'",
              traceStage, nested->id, (uint64_t)oldExpansionCandidates.size(),
              (uint64_t)matchingOldCandidates.size(), curOld.trim());
        if (matchingOldCandidates.size() != 1)
          return std::nullopt;

        DenseMap<uint64_t, SmallVector<const RefoldModel::PPArgSpan *, 4>>
            pasteGroups;
        for (const auto &sp : nested->pasteSpans) {
          if (!sp.byteBegin || !sp.byteEnd)
            return std::nullopt;
          const uint64_t key = (uint64_t(sp.begin) << 32) | uint64_t(sp.end);
          pasteGroups[key].push_back(&sp);
        }
        trace("macro/dag",
              "{0}: nested pasted-chain derivation paste groups nestedId={1} "
              "pasteSpanCount={2} groupCount={3}",
              traceStage, nested->id, (uint64_t)nested->pasteSpans.size(),
              (uint64_t)pasteGroups.size());
        if (pasteGroups.size() != 1)
          return std::nullopt;

        auto &group = pasteGroups.begin()->second;
        if (group.size() < 2)
          return std::nullopt;

        llvm::sort(group, PasteSpanPtrLessByByteRange);

        StringRef oldTok = curOld.trim();
        StringRef newTok = curNew.trim();
        if (oldTok.empty() || newTok.empty())
          return std::nullopt;

        auto rebasedGroup = tryRebasePasteGroupToObservedSurface(
            &cur, ArrayRef<const RefoldModel::PPArgSpan *>(group), oldTok,
            traceStage);
        if (!rebasedGroup)
          return std::nullopt;
        for (size_t i = 1; i < rebasedGroup->size(); ++i) {
          if ((*rebasedGroup)[i - 1].second > (*rebasedGroup)[i].first)
            return std::nullopt;
        }

        StringRef leading = oldTok.take_front((*rebasedGroup).front().first);
        StringRef trailing = oldTok.drop_front((*rebasedGroup).back().second);
        if (!newTok.starts_with(leading) || !newTok.ends_with(trailing))
          return std::nullopt;

        SmallVector<StringRef, 4> oldSegs;
        SmallVector<StringRef, 4> midBodies;
        oldSegs.reserve(group.size());
        midBodies.reserve(group.size() - 1);
        for (size_t i = 0; i < group.size(); ++i) {
          const auto [segBegin, segEnd] = (*rebasedGroup)[i];
          oldSegs.push_back(oldTok.slice(segBegin, segEnd));
          if (i + 1 < group.size()) {
            StringRef mid = oldTok.slice(segEnd, (*rebasedGroup)[i + 1].first);
            if (mid.empty())
              return std::nullopt;
            midBodies.push_back(mid);
          }
        }

        StringRef core =
            newTok.slice(leading.size(), newTok.size() - trailing.size());

        auto suffixDelimiterNeed = [&](size_t delimIdx) -> uint64_t {
          const StringRef delim = midBodies[delimIdx];
          uint64_t need = 0;
          for (size_t segIdx = delimIdx + 1; segIdx < oldSegs.size(); ++segIdx)
            need += countSubstr(oldSegs[segIdx], delim);
          for (size_t later = delimIdx + 1; later < midBodies.size(); ++later)
            if (midBodies[later] == delim)
              ++need;
          return need;
        };

        SmallVector<StringRef, 4> curSegs;
        SmallVector<SmallVector<StringRef, 4>, 2> splitSolutions;
        auto addSplitSolution = [&](const SmallVectorImpl<StringRef> &parts) {
          SmallVector<StringRef, 4> copy(parts.begin(), parts.end());
          for (const auto &existing : splitSolutions)
            if (existing == copy)
              return;
          splitSolutions.push_back(std::move(copy));
        };

        auto splitCore = [&](auto &&self, size_t delimIdx,
                             StringRef rest) -> void {
          if (splitSolutions.size() > 1)
            return;
          if (delimIdx == midBodies.size()) {
            curSegs.push_back(rest);
            addSplitSolution(curSegs);
            curSegs.pop_back();
            return;
          }

          const StringRef delim = midBodies[delimIdx];
          const uint64_t needLeft = countSubstr(oldSegs[delimIdx], delim);
          const uint64_t needRight = suffixDelimiterNeed(delimIdx);

          for (size_t pos = 0; (pos = rest.find(delim, pos)) != StringRef::npos;
               ++pos) {
            StringRef left = rest.slice(0, pos);
            StringRef tail = rest.drop_front(pos + delim.size());
            if (countSubstr(left, delim) < needLeft)
              continue;
            if (countSubstr(tail, delim) < needRight)
              continue;
            curSegs.push_back(left);
            self(self, delimIdx + 1, tail);
            curSegs.pop_back();
          }
        };
        splitCore(splitCore, 0, core);

        trace("macro/dag",
              "{0}: nested pasted-chain derivation split summary nestedId={1} "
              "groupSize={2} splitSolutions={3}",
              traceStage, nested->id, (uint64_t)group.size(),
              (uint64_t)splitSolutions.size());
        if (!splitSolutions.empty()) {
          std::string splitOut;
          raw_string_ostream os(splitOut);
          os << "[";
          for (size_t i = 0; i < splitSolutions[0].size(); ++i) {
            if (i)
              os << ", ";
            os << "'" << splitSolutions[0][i] << "'";
          }
          os << "]";
          trace("macro/dag",
                "{0}: nested pasted-chain derivation chosen split nestedId={1} "
                "split={2}",
                traceStage, nested->id, os.str());
        }
        if (splitSolutions.size() != 1 ||
            splitSolutions[0].size() != group.size())
          return std::nullopt;

        ParentConstraintDerivationCertificate cert;
        cert.childFormal = curFormal;
        DenseMap<uint32_t, ObservedFormalConstraint> mergedByParentFormal;
        for (size_t i = 0; i < group.size(); ++i) {
          const uint32_t nestedFormal = group[i]->argIdx;
          auto nestedCert = buildParentConstraintDerivationCertificate(
              *nested, nestedFormal, oldSegs[i], splitSolutions[0][i],
              traceStage);
          if (!nestedCert.valid)
            return std::nullopt;
          for (const auto &derived : nestedCert.derivedConstraints) {
            auto itExisting = mergedByParentFormal.find(derived.first);
            if (itExisting == mergedByParentFormal.end()) {
              mergedByParentFormal.insert({derived.first, derived.second});
              continue;
            }
            if (itExisting->second.oldText != derived.second.oldText ||
                itExisting->second.newText != derived.second.newText)
              return std::nullopt;
          }
        }

        for (const auto &kv : mergedByParentFormal)
          cert.derivedConstraints.push_back({kv.first, kv.second});
        llvm::sort(cert.derivedConstraints, [](const auto &a, const auto &b) {
          return a.first < b.first;
        });
        if (cert.derivedConstraints.empty())
          return std::nullopt;
        {
          std::string dc;
          raw_string_ostream os(dc);
          os << "{";
          for (size_t i = 0; i < cert.derivedConstraints.size(); ++i) {
            if (i)
              os << ", ";
            os << cert.derivedConstraints[i].first << ":'"
               << cert.derivedConstraints[i].second.oldText << "'->'"
               << cert.derivedConstraints[i].second.newText << "'";
          }
          os << "}";
          trace("macro/dag",
                "{0}: nested pasted-chain derivation success child id={1} "
                "name={2} argIdx={3} derived={4}",
                traceStage, cur.id, cur.name, curFormal, os.str());
        }
        cert.valid = true;
        return cert;
      };

      auto tryBuildTwoParentDelimitedDerivation =
          [&](const RefoldModel::MacroInvocation &cur, uint32_t curFormal,
              StringRef curOld, StringRef curNew, StringRef traceStage)
          -> std::optional<ParentConstraintDerivationCertificate> {
        if (!cur.callerMacroId || curFormal >= cur.argDeps.size())
          return std::nullopt;

        auto parentIt = invById.find(*cur.callerMacroId);
        if (parentIt == invById.end())
          return std::nullopt;
        const RefoldModel::MacroInvocation *parent = parentIt->second;
        ArrayRef<uint32_t> deps = cur.argDeps[curFormal];
        if (deps.size() != 2)
          return std::nullopt;

        auto oldAOpt = getInvocationArgText(*parent, deps[0]);
        auto oldBOpt = getInvocationArgText(*parent, deps[1]);
        if (!oldAOpt || !oldBOpt)
          return std::nullopt;

        const StringRef oldTok = curOld.trim();
        const StringRef newTok = curNew.trim();
        const StringRef oldA = oldAOpt->trim();
        const StringRef oldB = oldBOpt->trim();
        trace("macro/dag",
              "{0}: two-parent delimited derivation enter child id={1} "
              "name={2} argIdx={3} deps=[{4}, {5}] old='{6}' new='{7}' "
              "oldA='{8}' oldB='{9}'",
              traceStage, cur.id, cur.name, curFormal, deps[0], deps[1], oldTok,
              newTok, oldA, oldB);
        if (oldTok.empty() || newTok.empty() || oldA.empty() || oldB.empty())
          return std::nullopt;
        if (!oldTok.starts_with(oldA) || !oldTok.ends_with(oldB) ||
            oldTok.size() < oldA.size() + oldB.size()) {
          trace("macro/dag",
                "{0}: two-parent delimited derivation rejected shape child "
                "id={1} name={2} argIdx={3} old='{4}' oldA='{5}' oldB='{6}' "
                "startsWithA={7} endsWithB={8} sizeOk={9}",
                traceStage, cur.id, cur.name, curFormal, oldTok, oldA, oldB,
                oldTok.starts_with(oldA), oldTok.ends_with(oldB),
                oldTok.size() >= oldA.size() + oldB.size());
          return std::nullopt;
        }

        StringRef mid = oldTok.slice(oldA.size(), oldTok.size() - oldB.size());
        if (mid.empty())
          return std::nullopt;

        const uint64_t needA = countSubstr(oldA, mid);
        const uint64_t needB = countSubstr(oldB, mid);

        SmallVector<std::pair<StringRef, StringRef>, 4> splits;
        for (size_t pos = 0; (pos = newTok.find(mid, pos)) != StringRef::npos;
             ++pos) {
          StringRef newA = newTok.slice(0, pos);
          StringRef newB = newTok.drop_front(pos + mid.size());
          if (countSubstr(newA, mid) < needA || countSubstr(newB, mid) < needB)
            continue;
          splits.push_back({newA, newB});
        }

        trace("macro/dag",
              "{0}: two-parent delimited derivation split summary child id={1} "
              "name={2} argIdx={3} mid='{4}' splitCount={5}",
              traceStage, cur.id, cur.name, curFormal, mid,
              (uint64_t)splits.size());
        if (splits.size() != 1)
          return std::nullopt;

        ParentConstraintDerivationCertificate cert;
        cert.childFormal = curFormal;
        cert.valid = true;
        cert.derivedConstraints.push_back(
            {deps[0], ObservedFormalConstraint{oldA.str(),
                                               splits[0].first.trim().str()}});
        cert.derivedConstraints.push_back(
            {deps[1], ObservedFormalConstraint{oldB.str(),
                                               splits[0].second.trim().str()}});
        cert.detail = formatv("{0}: child id={1} name={2} argIdx={3} accepted "
                              "via two-parent delimited derivation",
                              traceStage, cur.id, cur.name, curFormal)
                          .str();
        trace("macro/dag",
              "{0}: two-parent delimited derivation success child id={1} "
              "name={2} argIdx={3} derived={{{4}:'{5}'->'{6}', "
              "{7}:'{8}'->'{9}'}}",
              traceStage, cur.id, cur.name, curFormal, deps[0], oldA,
              splits[0].first.trim(), deps[1], oldB, splits[0].second.trim());
        return cert;
      };

      buildParentConstraintDerivationCertificate =
          [&](const RefoldModel::MacroInvocation &cur, uint32_t curFormal,
              StringRef curOld, StringRef curNew,
              StringRef traceStage) -> ParentConstraintDerivationCertificate {
        ParentConstraintDerivationCertificate cert;
        cert.childFormal = curFormal;

        auto formatCurFormalContext = [&]() -> std::string {
          std::string out;
          raw_string_ostream os(out);
          os << "child id=" << cur.id << " name=" << cur.name
             << " argIdx=" << curFormal << " old='" << curOld.trim()
             << "' new='" << curNew.trim() << "' invText='";
          if (cur.invText)
            os << *cur.invText;
          else
            os << "<none>";
          os << "' invArgRange=";
          if (curFormal < cur.invArgRanges.size())
            os << FormatInvocationArgRange(cur.invArgRanges[curFormal]);
          else
            os << "<missing>";
          os << " argDeps=";
          if (curFormal < cur.argDeps.size())
            os << FormatUInt32List(cur.argDeps[curFormal]);
          else
            os << "<missing>";
          os << " argRefs=";
          if (curFormal < cur.argRefs.size())
            os << FormatInvArgRefList(cur.argRefs[curFormal]);
          else
            os << "<missing>";
          return os.str();
        };

        if (curFormal >= cur.argDeps.size()) {
          cert.failure = ParentConstraintDerivationFailure::MissingArgDeps;
          trace("macro/dag", "{0}: derivation failure context: {1}", traceStage,
                formatCurFormalContext());
          cert.detail = formatv("{0}: child id={1} name={2} argIdx={3} missing "
                                "argDeps entry; lexical bridge required",
                                traceStage, cur.id, cur.name, curFormal)
                            .str();
          return cert;
        }
        ArrayRef<uint32_t> deps = cur.argDeps[curFormal];
        if (deps.empty()) {
          cert.failure = ParentConstraintDerivationFailure::EmptyArgDeps;
          trace("macro/dag", "{0}: derivation failure context: {1}", traceStage,
                formatCurFormalContext());
          cert.detail = formatv("{0}: child id={1} name={2} argIdx={3} has "
                                "empty argDeps; lexical bridge required",
                                traceStage, cur.id, cur.name, curFormal)
                            .str();
          return cert;
        }

        if (curFormal >= cur.argRefs.size()) {
          cert.failure = ParentConstraintDerivationFailure::MissingArgRefs;
          trace("macro/dag", "{0}: derivation failure context: {1}", traceStage,
                formatCurFormalContext());
          cert.detail = formatv("{0}: child id={1} name={2} argIdx={3} missing "
                                "argRefs entry; lexical bridge required",
                                traceStage, cur.id, cur.name, curFormal)
                            .str();
          return cert;
        }

        auto tpl = buildArgRefTemplate(cur, curFormal);
        if (!tpl || tpl->refs.empty() ||
            !sameIndexSet(deps, tpl->distinctCallerParams)) {
          if (auto twoParentCert = tryBuildTwoParentDelimitedDerivation(
                  cur, curFormal, curOld, curNew, traceStage)) {
            trace("macro/dag",
                  "{0}: derivation accepted via two-parent delimited split "
                  "child id={1} name={2} argIdx={3} detail={4}",
                  traceStage, cur.id, cur.name, curFormal,
                  twoParentCert->detail);
            return *twoParentCert;
          }
          if (auto nestedCert = tryBuildNestedPasteChainDerivation(
                  cur, curFormal, curOld, curNew, traceStage)) {
            trace("macro/dag",
                  "{0}: derivation accepted via nested pasted-chain child "
                  "id={1} name={2} argIdx={3} detail={4}",
                  traceStage, cur.id, cur.name, curFormal, nestedCert->detail);
            return *nestedCert;
          }
          cert.failure =
              ParentConstraintDerivationFailure::TemplateNotCertifiable;
          trace("macro/dag", "{0}: derivation failure context: {1}", traceStage,
                formatCurFormalContext());
          cert.detail = formatv("{0}: child id={1} name={2} argIdx={3} arg-ref "
                                "template not certifiable; lexical bridge "
                                "required",
                                traceStage, cur.id, cur.name, curFormal)
                            .str();
          return cert;
        }

        auto oldCert = buildArgRefInvertibilityCertificate(*tpl, curOld);
        auto newCert = buildArgRefInvertibilityCertificate(*tpl, curNew);
        if (oldCert.kind != ArgRefInvertibilityKind::Unique ||
            newCert.kind != ArgRefInvertibilityKind::Unique) {
          if (auto twoParentCert = tryBuildTwoParentDelimitedDerivation(
                  cur, curFormal, curOld, curNew, traceStage)) {
            trace("macro/dag",
                  "{0}: derivation accepted via two-parent delimited split "
                  "child id={1} name={2} argIdx={3} detail={4}",
                  traceStage, cur.id, cur.name, curFormal,
                  twoParentCert->detail);
            return *twoParentCert;
          }
          if (auto nestedCert = tryBuildNestedPasteChainDerivation(
                  cur, curFormal, curOld, curNew, traceStage)) {
            trace("macro/dag",
                  "{0}: derivation accepted via nested pasted-chain child "
                  "id={1} name={2} argIdx={3} detail={4}",
                  traceStage, cur.id, cur.name, curFormal, nestedCert->detail);
            return *nestedCert;
          }
          cert.failure = ParentConstraintDerivationFailure::InversionNotUnique;
          trace("macro/dag", "{0}: derivation failure context: {1}", traceStage,
                formatCurFormalContext());
          cert.detail = formatv("{0}: child id={1} name={2} argIdx={3} arg-ref "
                                "inversion not unique; lexical bridge required",
                                traceStage, cur.id, cur.name, curFormal)
                            .str();
          return cert;
        }

        for (uint32_t parentFormal : tpl->distinctCallerParams) {
          auto oldIt = oldCert.derivedTextByCallerParam.find(parentFormal);
          auto newIt = newCert.derivedTextByCallerParam.find(parentFormal);
          if (oldIt == oldCert.derivedTextByCallerParam.end() ||
              newIt == newCert.derivedTextByCallerParam.end()) {
            cert.failure =
                ParentConstraintDerivationFailure::IncompleteDerivation;
            trace("macro/dag", "{0}: derivation failure context: {1}",
                  traceStage, formatCurFormalContext());
            cert.detail =
                formatv("{0}: child id={1} name={2} argIdx={3} "
                        "parent formal derivation incomplete; lexical "
                        "bridge required",
                        traceStage, cur.id, cur.name, curFormal)
                    .str();
            cert.derivedConstraints.clear();
            return cert;
          }
          cert.derivedConstraints.push_back(
              {parentFormal,
               ObservedFormalConstraint{oldIt->second, newIt->second}});
        }

        cert.valid = true;
        return cert;
      };

      enum class StructuredLiftCertificateKind {
        Unique,
        NeedsLexicalBridge,
        Invalid,
      };

      enum class StructuredLiftFailureReason {
        None,
        CurrentInvocationInvalid,
        MissingCallerInvocation,
        RootLexicalBridgeRequired,
        ParentConstraintDerivationFailed,
        ParentFormalInvalid,
        ParentInvocationInvalid,
      };

      struct StructuredLiftCertificate {
        StructuredLiftCertificateKind kind =
            StructuredLiftCertificateKind::Invalid;
        StructuredLiftFailureReason failureReason =
            StructuredLiftFailureReason::None;
        ParentConstraintDerivationFailure derivationFailure =
            ParentConstraintDerivationFailure::None;
        FormalRewriteFailure parentFormalFailure = FormalRewriteFailure::None;
        InvocationRewriteFailure currentInvocationFailure =
            InvocationRewriteFailure::None;
        InvocationRewriteFailure parentInvocationFailure =
            InvocationRewriteFailure::None;
        const RefoldModel::MacroInvocation *nextInv = nullptr;
        DenseMap<uint32_t, FormalTextPair> nextFormals;
        DenseMap<uint32_t, SmallVector<uint32_t, 2>> parentFormalSources;
        DenseSet<uint32_t> bridgedNextFormals;
        InvocationRewriteCertificate currentCert;
        SmallVector<ParentConstraintDerivationCertificate, 4> derivations;
        SmallVector<FormalRewriteCertificate, 4> parentFormalCertificates;
        InvocationRewriteCertificate parentCert;
        std::string rewrittenChildSyntax;
        std::string detail;
      };

      std::function<StructuredLiftCertificate(
          const RefoldModel::MacroInvocation &,
          const DenseMap<uint32_t, FormalTextPair> &)>
          buildStructuredLiftCertificate;

      auto tryBuildExactSiblingRerootLift =
          [&](const RefoldModel::MacroInvocation &parent,
              const RefoldModel::MacroInvocation &cur, uint32_t curFormal,
              StringRef curOld,
              StringRef curNew) -> std::optional<StructuredLiftCertificate> {
        const StringRef oldTrim = curOld.trim();
        const StringRef newTrim = curNew.trim();
        if (oldTrim.empty() || newTrim.empty())
          return std::nullopt;

        const RefoldModel::MacroInvocation *matchedSibling = nullptr;
        trace(
            "macro/dag",
            "DAG per-hop exact sibling reroot candidate scan: child id={0} "
            "name={1} parent id={2} name={3} curFormal={4} old='{5}' new='{6}'",
            cur.id, cur.name, parent.id, parent.name, curFormal, oldTrim,
            newTrim);
        for (const auto &cand : model_.GetMacroInvocations()) {
          if (cand.id == cur.id || !cand.callerMacroId ||
              *cand.callerMacroId != parent.id || !cand.invText)
            continue;
          if (StringRef(*cand.invText).trim() != oldTrim)
            continue;
          if (matchedSibling) {
            trace("macro/dag",
                  "DAG per-hop exact sibling reroot ambiguous sibling match: "
                  "child id={0} name={1} parent id={2} name={3} old='{4}' "
                  "firstSibling={5} secondSibling={6}",
                  cur.id, cur.name, parent.id, parent.name, oldTrim,
                  matchedSibling->id, cand.id);
            return std::nullopt;
          }
          matchedSibling = &cand;
        }

        if (!matchedSibling || matchedSibling->invArgRanges.empty()) {
          trace("macro/dag",
                "DAG per-hop exact sibling reroot no usable sibling: child "
                "id={0} name={1} parent id={2} name={3} matchedSibling={4}",
                cur.id, cur.name, parent.id, parent.name,
                matchedSibling ? matchedSibling->id : 0);
          return std::nullopt;
        }

        {
          SmallVector<std::string, 4> siblingOldExpansionCandidates =
              expansionTextCandidates(*matchedSibling, /*fromB=*/false);
          std::string expansionList;
          raw_string_ostream os(expansionList);
          os << "[";
          for (size_t i = 0; i < siblingOldExpansionCandidates.size(); ++i) {
            if (i)
              os << ", ";
            os << "'" << siblingOldExpansionCandidates[i] << "'";
          }
          os << "]";
          trace("macro/dag",
                "DAG per-hop exact sibling reroot matched sibling expansion "
                "candidates: child id={0} name={1} sibling id={2} name={3} "
                "curFormal={4} rawOld='{5}' expansions={6}",
                cur.id, cur.name, matchedSibling->id, matchedSibling->name,
                curFormal, oldTrim, os.str());

          if (curFormal < cur.argDeps.size() &&
              cur.argDeps[curFormal].size() == 2 &&
              siblingOldExpansionCandidates.size() == 1) {
            ArrayRef<uint32_t> deps = cur.argDeps[curFormal];
            auto oldAOpt = getInvocationArgText(parent, deps[0]);
            auto oldBOpt = getInvocationArgText(parent, deps[1]);
            if (oldAOpt && oldBOpt) {
              StringRef oldExp =
                  StringRef(siblingOldExpansionCandidates[0]).trim();
              StringRef oldA = oldAOpt->trim();
              StringRef oldB = oldBOpt->trim();
              bool factors = oldExp.starts_with(oldA) &&
                             oldExp.ends_with(oldB) &&
                             oldExp.size() >= oldA.size() + oldB.size();
              std::string midStr;
              uint64_t splitCount = 0;
              if (factors) {
                StringRef mid =
                    oldExp.slice(oldA.size(), oldExp.size() - oldB.size());
                midStr = mid.str();
                if (!mid.empty()) {
                  const uint64_t needA = countSubstr(oldA, mid);
                  const uint64_t needB = countSubstr(oldB, mid);
                  for (size_t pos = 0;
                       (pos = newTrim.find(mid, pos)) != StringRef::npos;
                       ++pos) {
                    StringRef newA = newTrim.slice(0, pos);
                    StringRef newB = newTrim.drop_front(pos + mid.size());
                    if (countSubstr(newA, mid) < needA ||
                        countSubstr(newB, mid) < needB)
                      continue;
                    ++splitCount;
                  }
                }
              }
              trace(
                  "macro/dag",
                  "DAG per-hop exact sibling reroot counterfactual "
                  "sibling-output split: child id={0} name={1} sibling id={2} "
                  "name={3} curFormal={4} oldExp='{5}' parentOldA='{6}' "
                  "parentOldB='{7}' factors={8} mid='{9}' splitCount={10}",
                  cur.id, cur.name, matchedSibling->id, matchedSibling->name,
                  curFormal, oldExp, oldA, oldB, factors, midStr, splitCount);
            }
          }
        }

        std::optional<StructuredLiftCertificate> uniqueLift;
        std::optional<uint32_t> uniqueSiblingFormal;
        std::string uniqueSiblingOld;

        auto sameNextFormals =
            [&](const DenseMap<uint32_t, FormalTextPair> &lhs,
                const DenseMap<uint32_t, FormalTextPair> &rhs) {
              if (lhs.size() != rhs.size())
                return false;
              for (const auto &KV : lhs) {
                auto it = rhs.find(KV.first);
                if (it == rhs.end())
                  return false;
                if (it->second.oldText != KV.second.oldText ||
                    it->second.newText != KV.second.newText)
                  return false;
              }
              return true;
            };

        struct ConcreteExemplarReplayLiftResult {
          enum class State {
            None,
            Unique,
            Ambiguous,
          };

          State state = State::None;
          std::optional<StructuredLiftCertificate> lift;
        };

        auto tryBuildConcreteExemplarReplayLift =
            [&](uint32_t siblingFormal,
                StringRef siblingOldTrim) -> ConcreteExemplarReplayLiftResult {
          SmallVector<std::string, 4> parentActuals;
          for (uint32_t parentFormal = 0;
               parentFormal < parent.invArgRanges.size(); ++parentFormal) {
            if (auto parentArg = getInvocationArgText(parent, parentFormal)) {
              StringRef parentArgTrim = parentArg->trim();
              if (!parentArgTrim.empty() &&
                  !llvm::is_contained(parentActuals, parentArgTrim.str()))
                parentActuals.push_back(parentArgTrim.str());
            }
          }

          ConcreteExemplarReplayLiftResult result;
          for (const auto &exemplar : model_.GetMacroInvocations()) {
            if (exemplar.id == matchedSibling->id ||
                exemplar.name != matchedSibling->name ||
                siblingFormal >= exemplar.invArgRanges.size())
              continue;

            auto exemplarOldArg = getInvocationArgText(exemplar, siblingFormal);
            if (!exemplarOldArg)
              continue;
            StringRef exemplarOldTrim = exemplarOldArg->trim();
            if (exemplarOldTrim.empty() ||
                !llvm::is_contained(parentActuals, exemplarOldTrim.str()))
              continue;

            SmallVector<std::string, 4> projectedConcreteNews;
            for (const auto &oldExpStr :
                 expansionTextCandidates(exemplar, /*fromB=*/false)) {
              StringRef projected =
                  DeriveNewPasteSegmentFromSpellingReplacement(
                      StringRef(oldExpStr).trim(), newTrim, exemplarOldTrim);
              projected = projected.trim();
              if (projected.empty() || projected == exemplarOldTrim)
                continue;
              if (!llvm::is_contained(projectedConcreteNews, projected.str()))
                projectedConcreteNews.push_back(projected.str());
            }
            if (projectedConcreteNews.size() != 1)
              continue;

            const std::string &projectedConcreteNew =
                projectedConcreteNews.front();

            DenseMap<uint32_t, FormalTextPair> exemplarFormals;
            exemplarFormals[siblingFormal] =
                FormalTextPair{exemplarOldTrim.str(), projectedConcreteNew};
            auto exemplarInvCert =
                buildWrapperPlaceholderHopInvocationCertificate(
                    exemplar, exemplarFormals,
                    "DAG per-hop exact sibling reroot concrete exemplar");
            if (exemplarInvCert.kind ==
                InvocationRewriteCertificateKind::Invalid)
              continue;

            DenseMap<uint32_t, FormalTextPair> replayFormals;
            replayFormals[siblingFormal] =
                FormalTextPair{siblingOldTrim.str(), projectedConcreteNew};
            auto replayInvCert =
                buildWrapperPlaceholderHopInvocationCertificate(
                    *matchedSibling, replayFormals,
                    "DAG per-hop exact sibling reroot concrete replay");
            if (replayInvCert.kind == InvocationRewriteCertificateKind::Invalid)
              continue;

            auto projectedLift =
                buildStructuredLiftCertificate(*matchedSibling, replayFormals);
            if (projectedLift.kind != StructuredLiftCertificateKind::Unique ||
                projectedLift.nextInv != &parent)
              continue;

            if (result.lift) {
              if (!sameNextFormals(result.lift->nextFormals,
                                   projectedLift.nextFormals)) {
                result.state =
                    ConcreteExemplarReplayLiftResult::State::Ambiguous;
                result.lift.reset();
                return result;
              }
              result.state = ConcreteExemplarReplayLiftResult::State::Unique;
              continue;
            }

            result.state = ConcreteExemplarReplayLiftResult::State::Unique;
            result.lift = std::move(projectedLift);
          }

          return result;
        };

        for (uint32_t siblingFormal = 0;
             siblingFormal < matchedSibling->invArgRanges.size();
             ++siblingFormal) {
          auto siblingOldArg =
              getInvocationArgText(*matchedSibling, siblingFormal);
          if (!siblingOldArg) {
            trace("macro/dag",
                  "DAG per-hop exact sibling reroot skip sibling formal: "
                  "sibling id={0} name={1} siblingFormal={2} reason=noArgText",
                  matchedSibling->id, matchedSibling->name, siblingFormal);
            continue;
          }

          const StringRef siblingOldTrim = siblingOldArg->trim();
          if (siblingOldTrim.empty() || siblingOldTrim == newTrim) {
            trace("macro/dag",
                  "DAG per-hop exact sibling reroot skip sibling formal: "
                  "sibling id={0} name={1} siblingFormal={2} siblingOld='{3}' "
                  "reason=emptyOrNoChange",
                  matchedSibling->id, matchedSibling->name, siblingFormal,
                  siblingOldTrim);
            continue;
          }

          trace("macro/dag",
                "DAG per-hop exact sibling reroot try sibling formal: sibling "
                "id={0} name={1} siblingFormal={2} siblingOld='{3}' new='{4}'",
                matchedSibling->id, matchedSibling->name, siblingFormal,
                siblingOldTrim, newTrim);

          DenseMap<uint32_t, FormalTextPair> siblingFormals;
          siblingFormals[siblingFormal] =
              FormalTextPair{siblingOldTrim.str(), newTrim.str()};
          auto siblingLift =
              buildStructuredLiftCertificate(*matchedSibling, siblingFormals);
          trace("macro/dag",
                "DAG per-hop exact sibling reroot sibling formal result: "
                "sibling id={0} name={1} siblingFormal={2} stepKind={3} "
                "nextInv={4} detail={5}",
                matchedSibling->id, matchedSibling->name, siblingFormal,
                static_cast<unsigned>(siblingLift.kind),
                siblingLift.nextInv ? siblingLift.nextInv->id : 0,
                siblingLift.detail);
          if (siblingLift.kind != StructuredLiftCertificateKind::Unique ||
              siblingLift.nextInv != &parent)
            continue;

          auto siblingLiftHasCertifiedParentFormalEvidence = [&]() {
            for (const auto &derived : siblingLift.nextFormals) {
              const uint32_t parentFormal = derived.first;
              bool certified = false;
              for (const auto &formalCert :
                   siblingLift.parentFormalCertificates) {
                if (formalCert.argIdx != parentFormal)
                  continue;
                if (formalCert.kind != FormalRewriteCertificateKind::Invalid) {
                  certified = true;
                  break;
                }
              }
              if (!certified)
                return false;
            }
            return true;
          };

          if (!siblingLiftHasCertifiedParentFormalEvidence()) {
            auto replayResult = tryBuildConcreteExemplarReplayLift(
                siblingFormal, siblingOldTrim);
            if (replayResult.state ==
                    ConcreteExemplarReplayLiftResult::State::Unique &&
                replayResult.lift) {
              siblingLift = std::move(*replayResult.lift);
            } else if (replayResult.state ==
                           ConcreteExemplarReplayLiftResult::State::None &&
                       siblingLift.parentCert.kind !=
                           InvocationRewriteCertificateKind::Invalid &&
                       siblingLift.parentInvocationFailure ==
                           InvocationRewriteFailure::None) {
            } else {
              // The normal case: split the rewritten core around the original
              // literal delimiters and require a unique segmentation.
              trace("macro/dag",
                    "DAG per-hop exact sibling reroot rejected: sibling id={0} "
                    "name={1} siblingFormal={2} "
                    "reason=uncertifiedParentFormalEvidence nextFormals={3}",
                    matchedSibling->id, matchedSibling->name, siblingFormal,
                    formatFormalTextPairMap(siblingLift.nextFormals));
              continue;
            }
          }

          if (uniqueLift) {
            trace("macro/dag",
                  "DAG per-hop exact sibling reroot ambiguous sibling-formal "
                  "seed: sibling id={0} name={1} firstFormal={2} "
                  "secondFormal={3}",
                  matchedSibling->id, matchedSibling->name,
                  *uniqueSiblingFormal, siblingFormal);
            return std::nullopt;
          }

          uniqueSiblingFormal = siblingFormal;
          uniqueSiblingOld = siblingOldTrim.str();
          uniqueLift = std::move(siblingLift);
        }

        if (!uniqueLift || !uniqueSiblingFormal)
          return std::nullopt;

        trace("macro/dag",
              "DAG per-hop exact sibling reroot: child id={0} name={1} "
              "parent id={2} name={3} curFormal={4} via sibling id={5} "
              "name={6} siblingFormal={7} old='{8}' siblingOld='{9}' "
              "new='{10}'",
              cur.id, cur.name, parent.id, parent.name, curFormal,
              matchedSibling->id, matchedSibling->name, *uniqueSiblingFormal,
              oldTrim, uniqueSiblingOld, newTrim);
        return std::move(*uniqueLift);
      };

      buildStructuredLiftCertificate =
          [&](const RefoldModel::MacroInvocation &cur,
              const DenseMap<uint32_t, FormalTextPair> &curFormals)
          -> StructuredLiftCertificate {
        StructuredLiftCertificate cert;

        auto formatFormalTextPairs =
            [&](const DenseMap<uint32_t, FormalTextPair> &formals)
            -> std::string {
          std::vector<std::pair<uint32_t, const FormalTextPair *>> ordered;
          ordered.reserve(formals.size());
          for (const auto &KV : formals)
            ordered.push_back({KV.first, &KV.second});
          llvm::sort(ordered, [](const auto &L, const auto &R) {
            return L.first < R.first;
          });
          std::string out;
          raw_string_ostream os(out);
          os << "{";
          for (size_t i = 0; i < ordered.size(); ++i) {
            if (i)
              os << ", ";
            os << ordered[i].first << ":'" << ordered[i].second->oldText
               << "'->'" << ordered[i].second->newText << "'";
          }
          os << "}";
          return os.str();
        };

        auto formatObservedConstraintsMap =
            [&](const DenseMap<
                uint32_t, SmallVector<ObservedFormalConstraint, 2>> &observed)
            -> std::string {
          std::vector<uint32_t> keys;
          keys.reserve(observed.size());
          for (const auto &KV : observed)
            keys.push_back(KV.first);
          llvm::sort(keys);
          std::string out;
          raw_string_ostream os(out);
          os << "{";
          for (size_t i = 0; i < keys.size(); ++i) {
            if (i)
              os << ", ";
            os << keys[i] << ":[";
            auto found = observed.find(keys[i]);
            if (found != observed.end()) {
              const auto &constraints = found->second;
              for (size_t j = 0; j < constraints.size(); ++j) {
                if (j)
                  os << ", ";
                os << "'" << constraints[j].oldText << "'->'"
                   << constraints[j].newText << "'";
              }
            }
            os << "]";
          }
          os << "}";
          return os.str();
        };

        trace("macro/dag",
              "DAG per-hop enter: child id={0} name={1} curFormals={2}", cur.id,
              cur.name, formatFormalTextPairs(curFormals));

        auto curCert = buildWrapperPlaceholderHopInvocationCertificate(
            cur, curFormals, "DAG per-hop");
        cert.currentCert = curCert;
        if (curCert.kind == InvocationRewriteCertificateKind::Invalid) {
          cert.failureReason =
              StructuredLiftFailureReason::CurrentInvocationInvalid;
          cert.currentInvocationFailure = curCert.failure;
          cert.detail = curCert.detail;
          return cert;
        }

        DenseMap<uint32_t, std::string> curFormalSyntax;
        for (const auto &KV : curFormals)
          curFormalSyntax[KV.first] = KV.second.newText;
        if (!curCert.rewrittenInvocationSyntax.empty()) {
          cert.rewrittenChildSyntax = curCert.rewrittenInvocationSyntax;
        } else if (auto curSyntax =
                       buildRewrittenInvocationSyntax(cur, curFormalSyntax)) {
          cert.rewrittenChildSyntax = std::move(*curSyntax);
        }
        trace("macro/dag",
              "DAG per-hop child syntax: child id={0} name={1} syntax='{2}' "
              "currentCertKind={3} detail={4}",
              cur.id, cur.name, cert.rewrittenChildSyntax,
              static_cast<unsigned>(curCert.kind), curCert.detail);

        const RefoldModel::MacroInvocation *parent = nullptr;
        if (cur.callerMacroId) {
          auto parentIt = invById.find(*cur.callerMacroId);
          if (parentIt == invById.end()) {
            cert.failureReason =
                StructuredLiftFailureReason::MissingCallerInvocation;
            cert.detail =
                formatv("DAG per-hop: missing caller invocation: child id={0} "
                        "name={1} callerId={2}",
                        cur.id, cur.name, *cur.callerMacroId)
                    .str();
            return cert;
          }
          parent = parentIt->second;
        } else {
          trace("macro/dag",
                "DAG per-hop: child id={0} name={1} has no callerMacroId; "
                "forcing lexical bridge to root with syntax='{2}'",
                cur.id, cur.name, cert.rewrittenChildSyntax);
          cert.kind = StructuredLiftCertificateKind::NeedsLexicalBridge;
          cert.failureReason =
              StructuredLiftFailureReason::RootLexicalBridgeRequired;
          cert.nextInv = &m;
          cert.detail = formatv("DAG per-hop: child id={0} name={1} requires "
                                "lexical bridge to root",
                                cur.id, cur.name)
                            .str();
          return cert;
        }

        /// Try to explain an observed rewrite of a pasted surface by replaying
        /// exactly one direct paste-producing child invocation.
        ///
        /// This helper is intentionally narrow: it only succeeds when the child
        /// has a single paste group, the edited surface can be split back into
        /// a unique sequence of per-operand segments, and each segment can be
        /// lifted through the child's formal-derivation certificate. Any
        /// ambiguity means we do not have a sound inverse-paste witness.
        auto tryDeriveObservedConstraintsFromDirectPasteChild =
            [&](const RefoldModel::MacroInvocation &surfaceOwner,
                const RefoldModel::MacroInvocation &directChild,
                StringRef observedOld0, StringRef observedNew0,
                StringRef traceStage)
            -> std::optional<
                SmallVector<std::pair<uint32_t, ObservedFormalConstraint>, 4>> {
          StringRef observedOld = observedOld0.trim();
          StringRef observedNew = observedNew0.trim();
          if (observedOld.empty() || observedNew.empty())
            return std::nullopt;

          // Group spans by their common pasted result. Exact-shape replay only
          // handles a single pasted product at this hop; multiple independent
          // paste groups would require choosing between distinct replay
          // regions.
          DenseMap<uint64_t, SmallVector<const RefoldModel::PPArgSpan *, 4>>
              pasteGroups;
          for (const auto &sp : directChild.pasteSpans) {
            if (!sp.byteBegin || !sp.byteEnd)
              return std::nullopt;
            const uint64_t key = (uint64_t(sp.begin) << 32) | uint64_t(sp.end);
            pasteGroups[key].push_back(&sp);
          }
          if (pasteGroups.size() != 1)
            return std::nullopt;

          auto &group = pasteGroups.begin()->second;
          if (group.size() < 2)
            return std::nullopt;

          llvm::sort(group, PasteSpanPtrLessByByteRange);

          auto rebasedGroup = tryRebasePasteGroupToObservedSurface(
              &surfaceOwner, ArrayRef<const RefoldModel::PPArgSpan *>(group),
              observedOld, traceStage);
          if (!rebasedGroup)
            return std::nullopt;
          for (size_t i = 1; i < rebasedGroup->size(); ++i) {
            if ((*rebasedGroup)[i - 1].second > (*rebasedGroup)[i].first)
              return std::nullopt;
          }

          // The edit must preserve the non-pasted prefix/suffix verbatim.
          // Otherwise we are no longer replaying the same direct pasted child.
          StringRef leading =
              observedOld.take_front((*rebasedGroup).front().first);
          StringRef trailing =
              observedOld.drop_front((*rebasedGroup).back().second);
          if (!observedNew.starts_with(leading) ||
              !observedNew.ends_with(trailing))
            return std::nullopt;

          SmallVector<StringRef, 4> oldSegs;
          SmallVector<StringRef, 4> midBodies;
          oldSegs.reserve(group.size());
          midBodies.reserve(group.size() - 1);
          for (size_t i = 0; i < group.size(); ++i) {
            const auto [segBegin, segEnd] = (*rebasedGroup)[i];
            oldSegs.push_back(observedOld.slice(segBegin, segEnd));
            if (i + 1 < group.size()) {
              StringRef mid =
                  observedOld.slice(segEnd, (*rebasedGroup)[i + 1].first);
              if (mid.empty())
                return std::nullopt;
              midBodies.push_back(mid);
            }
          }

          StringRef core = observedNew.slice(
              leading.size(), observedNew.size() - trailing.size());

          // Count how many future occurrences of the current delimiter must be
          // reserved to make the remainder splittable. This lets the splitter
          // reject early cuts that would strand a later operand.
          auto suffixDelimiterNeed = [&](size_t delimIdx) -> uint64_t {
            const StringRef delim = midBodies[delimIdx];
            uint64_t need = 0;
            for (size_t segIdx = delimIdx + 1; segIdx < oldSegs.size();
                 ++segIdx)
              need += countSubstr(oldSegs[segIdx], delim);
            for (size_t later = delimIdx + 1; later < midBodies.size(); ++later)
              if (midBodies[later] == delim)
                ++need;
            return need;
          };

          SmallVector<StringRef, 4> curSegs;
          SmallVector<SmallVector<StringRef, 4>, 2> splitSolutions;
          auto addSplitSolution = [&](const SmallVectorImpl<StringRef> &parts) {
            SmallVector<StringRef, 4> copy(parts.begin(), parts.end());
            for (const auto &existing : splitSolutions)
              if (existing == copy)
                return;
            splitSolutions.push_back(std::move(copy));
          };

          // Split the rewritten pasted core around the original inter-operand
          // delimiters. We only accept a unique segmentation; if multiple
          // splits work, the inverse-paste explanation is ambiguous and
          // therefore not a valid replay certificate.
          auto splitCore = [&](auto &&self, size_t delimIdx,
                               StringRef rest) -> void {
            if (splitSolutions.size() > 1)
              return;
            if (delimIdx == midBodies.size()) {
              curSegs.push_back(rest);
              addSplitSolution(curSegs);
              curSegs.pop_back();
              return;
            }

            const StringRef delim = midBodies[delimIdx];
            const uint64_t needLeft = countSubstr(oldSegs[delimIdx], delim);
            const uint64_t needRight = suffixDelimiterNeed(delimIdx);

            for (size_t pos = 0;
                 (pos = rest.find(delim, pos)) != StringRef::npos; ++pos) {
              StringRef left = rest.slice(0, pos);
              StringRef tail = rest.drop_front(pos + delim.size());
              if (countSubstr(left, delim) < needLeft)
                continue;
              if (countSubstr(tail, delim) < needRight)
                continue;
              curSegs.push_back(left);
              self(self, delimIdx + 1, tail);
              curSegs.pop_back();
            }
          };
          splitCore(splitCore, 0, core);
          if (splitSolutions.size() != 1 ||
              splitSolutions[0].size() != group.size())
            return std::nullopt;

          // Lift each recovered child-operand rewrite through the child's
          // normal parent-constraint derivation, then merge the resulting
          // parent-formal constraints. Conflicting lifts mean the pasted
          // surface cannot be explained by one consistent replay of the
          // original child.
          DenseMap<uint32_t, ObservedFormalConstraint> mergedByFormal;
          for (size_t i = 0; i < group.size(); ++i) {
            const uint32_t childFormal = group[i]->argIdx;
            auto derived = buildParentConstraintDerivationCertificate(
                directChild, childFormal, oldSegs[i], splitSolutions[0][i],
                traceStage);
            if (!derived.valid)
              return std::nullopt;
            for (const auto &kv : derived.derivedConstraints) {
              auto itExisting = mergedByFormal.find(kv.first);
              if (itExisting == mergedByFormal.end()) {
                mergedByFormal.insert({kv.first, kv.second});
                continue;
              }
              if (itExisting->second.oldText != kv.second.oldText ||
                  itExisting->second.newText != kv.second.newText)
                return std::nullopt;
            }
          }

          SmallVector<std::pair<uint32_t, ObservedFormalConstraint>, 4> out;
          for (const auto &kv : mergedByFormal)
            out.push_back({kv.first, kv.second});
          llvm::sort(out, [](const auto &a, const auto &b) {
            return a.first < b.first;
          });
          if (out.empty())
            return std::nullopt;
          return out;
        };

        /// Rebuild the exact original nested paste shape for `target` when the
        /// observed edit still admits a unique, certificate-backed replay of
        /// that shape. This never invents a new decomposition; it only
        /// preserves the tree that originally existed in source.
        std::function<std::optional<std::string>(
            const RefoldModel::MacroInvocation &, StringRef, StringRef,
            StringRef)>
            tryBuildExactOriginalShapePasteReplaySyntax;

        tryBuildExactOriginalShapePasteReplaySyntax =
            [&](const RefoldModel::MacroInvocation &target,
                StringRef observedOld0, StringRef observedNew0,
                StringRef traceStage) -> std::optional<std::string> {
          StringRef observedOld = observedOld0.trim();
          StringRef observedNew = observedNew0.trim();
          if (!target.invText)
            return std::nullopt;

          StringRef rawTarget = StringRef(*target.invText).trim();
          if (rawTarget.empty())
            return std::nullopt;
          if (observedOld == observedNew)
            return rawTarget.str();

          auto childIt = macroChildrenById_.find(target.id);
          if (childIt == macroChildrenById_.end())
            return std::nullopt;

          // Stringify wrappers may record paste byte ranges relative to the
          // quoted surface rather than the raw token text, so probe both forms.
          auto tryQuoteSurface = [&](StringRef raw) -> std::string {
            return quoteCStringLiteral(raw);
          };

          const RefoldModel::MacroInvocation *directPasteChild = nullptr;
          std::optional<
              SmallVector<std::pair<uint32_t, ObservedFormalConstraint>, 4>>
              derivedConstraints;
          auto sameDerivedConstraints =
              [&](const SmallVectorImpl<
                      std::pair<uint32_t, ObservedFormalConstraint>> &lhs,
                  const SmallVectorImpl<
                      std::pair<uint32_t, ObservedFormalConstraint>> &rhs)
              -> bool {
            if (lhs.size() != rhs.size())
              return false;
            for (size_t i = 0; i < lhs.size(); ++i) {
              if (lhs[i].first != rhs[i].first)
                return false;
              if (lhs[i].second.oldText != rhs[i].second.oldText ||
                  lhs[i].second.newText != rhs[i].second.newText)
                return false;
            }
            return true;
          };

          // Find the unique direct child whose paste spans can explain the
          // observed rewrite. If more than one child derives different parent
          // constraints, the replay would be ambiguous and must be rejected.
          auto trySelectDirectChildForSurface =
              [&](StringRef surfaceOld, StringRef surfaceNew) -> bool {
            for (const auto *cand : childIt->second) {
              if (!cand || cand->pasteSpans.empty())
                continue;
              auto derived = tryDeriveObservedConstraintsFromDirectPasteChild(
                  target, *cand, surfaceOld, surfaceNew, traceStage);
              if (!derived)
                continue;
              if (directPasteChild) {
                if (directPasteChild != cand || !derivedConstraints ||
                    !sameDerivedConstraints(*derivedConstraints, *derived))
                  return false;
                continue;
              }
              directPasteChild = cand;
              derivedConstraints = std::move(derived);
            }
            return true;
          };

          if (!trySelectDirectChildForSurface(observedOld, observedNew))
            return std::nullopt;
          if (!directPasteChild) {
            std::string quotedOld = tryQuoteSurface(observedOld);
            std::string quotedNew = tryQuoteSurface(observedNew);
            if (!trySelectDirectChildForSurface(quotedOld, quotedNew))
              return std::nullopt;
          }
          if (!directPasteChild || !derivedConstraints) {
            return std::nullopt;
          }

          // Group the derived constraints by the target's formals. Each group
          // is later replayed either by recursively preserving a nested child
          // or by falling back to the normal observed-formal certificate.
          DenseMap<uint32_t, SmallVector<ObservedFormalConstraint, 2>>
              groupedObserved;
          for (const auto &kv : *derivedConstraints)
            groupedObserved[kv.first].push_back(kv.second);

          DenseMap<uint32_t, FormalTextPair> targetFormals;
          for (const auto &KV : groupedObserved) {
            const uint32_t formalIdx = KV.first;
            auto argText = getInvocationArgText(target, formalIdx);
            if (!argText)
              return std::nullopt;
            const StringRef rawOldArg = argText->trim();

            // When the target formal is exactly one nested child invocation,
            // try to preserve that nested child first. This is the step that
            // keeps a chain such as JOIN(JOIN(...), ...) instead of collapsing
            // it to the already-materialized pasted token.
            if (KV.second.size() == 1) {
              const StringRef segOld =
                  StringRef(KV.second.front().oldText).trim();
              const StringRef segNew =
                  StringRef(KV.second.front().newText).trim();
              auto placeholders =
                  getTopLevelLexicalChildrenInArg(target, formalIdx);
              if (placeholders.size() == 1 && placeholders.front().child &&
                  placeholders.front().relBegin == 0 &&
                  placeholders.front().relEnd == rawOldArg.size()) {
                const RefoldModel::MacroInvocation *nestedChild =
                    placeholders.front().child;
                if (auto nestedSyntax =
                        tryBuildExactOriginalShapePasteReplaySyntax(
                            *nestedChild, segOld, segNew,
                            "DAG per-hop exact original-shape replay")) {
                  targetFormals[formalIdx] =
                      FormalTextPair{rawOldArg.str(), std::move(*nestedSyntax)};
                  continue;
                }
              }
            }

            // Otherwise, certify the formal rewrite in the usual way and let
            // the wrapper-hop certificate rebuild the target invocation around
            // it.
            auto formalCert = buildObservedFormalRewriteCertificate(
                target, formalIdx, KV.second, /*preferredChildSyntax=*/nullptr,
                traceStage);
            if (formalCert.kind == FormalRewriteCertificateKind::Invalid)
              return std::nullopt;
            targetFormals[formalIdx] =
                FormalTextPair{formalCert.oldText, formalCert.newText};
          }

          // Finally, prove that the rewritten formals still fit the target's
          // original placeholder structure. This is the soundness gate for the
          // exact-shape replay at this invocation boundary.
          auto replayCert = buildWrapperPlaceholderHopInvocationCertificate(
              target, targetFormals, traceStage);
          if (replayCert.kind == InvocationRewriteCertificateKind::Invalid)
            return std::nullopt;
          if (!replayCert.rewrittenInvocationSyntax.empty())
            return replayCert.rewrittenInvocationSyntax;

          DenseMap<uint32_t, std::string> replByFormal;
          for (const auto &KV : targetFormals) {
            StringRef oldText = StringRef(KV.second.oldText).trim();
            StringRef newText = StringRef(KV.second.newText).trim();
            if (oldText != newText)
              replByFormal[KV.first] = newText.str();
          }
          return buildRewrittenInvocationSyntax(target, replByFormal);
        };

        /// Handle the common DAG hop where one child formal maps directly to
        /// one parent formal. Normally this is a passthrough rewrite, but if
        /// the child's observed old text is already flatter than the parent's
        /// logical old argument, probe exact-shape paste replay before
        /// accepting that flattening loss.
        auto tryBuildDirectPassthroughParentFormalRewrite =
            [&](uint32_t curFormal, uint32_t parentFormal,
                StringRef curNewText) -> std::optional<FormalTextPair> {
          if (curFormal >= cur.argDeps.size())
            return std::nullopt;
          ArrayRef<uint32_t> deps = cur.argDeps[curFormal];
          if (deps.size() != 1 || deps[0] != parentFormal)
            return std::nullopt;

          auto tpl = buildArgRefTemplate(cur, curFormal);
          if (!tpl || tpl->refs.size() != 1 ||
              tpl->distinctCallerParams.size() != 1)
            return std::nullopt;

          const auto &ref = tpl->refs[0];
          if (ref.callerParamIndex != parentFormal || ref.begin != 0 ||
              ref.end != StringRef(tpl->argText).trim().size())
            return std::nullopt;

          auto parentArgText = getInvocationArgText(*parent, parentFormal);
          if (!parentArgText)
            return std::nullopt;

          StringRef oldTrim = parentArgText->trim();
          StringRef newTrim = curNewText.trim();
          if (newTrim.empty() || oldTrim == newTrim)
            return std::nullopt;

          // `curFormals` may not carry this formal when the child rewrite was
          // derived through a different certified path, so guard the lookup.
          const auto curFormalIt = curFormals.find(curFormal);
          if (curFormalIt == curFormals.end())
            return std::nullopt;

          const StringRef childObservedOld =
              StringRef(curFormalIt->second.oldText).trim();

          trace("macro/dag",
                "DAG per-hop parent formal passthrough flatten candidate: "
                "child id={0} name={1} parent id={2} name={3} "
                "sourceCurFormal={4} parentFormal={5} childObservedOld='{6}' "
                "childObservedNew='{7}' parentLogicalOld='{8}' supportLoss={9}",
                cur.id, cur.name, parent->id, parent->name, curFormal,
                parentFormal, childObservedOld, newTrim, oldTrim,
                childObservedOld != oldTrim);
          // A mismatch here means the child has already collapsed some nested
          // structure relative to the parent's logical argument. If the parent
          // argument is exactly one lexical child, try to replay that original
          // nested shape instead of committing to the flatter replacement text.
          if (childObservedOld != oldTrim) {
            auto placeholders =
                getTopLevelLexicalChildrenInArg(*parent, parentFormal);
            if (placeholders.size() == 1 && placeholders.front().child &&
                placeholders.front().relBegin == 0 &&
                placeholders.front().relEnd == oldTrim.size()) {
              const RefoldModel::MacroInvocation *nestedChild =
                  placeholders.front().child;
              if (auto replaySyntax =
                      tryBuildExactOriginalShapePasteReplaySyntax(
                          *nestedChild, childObservedOld, newTrim,
                          "DAG per-hop exact original-shape replay")) {
                trace(
                    "macro/dag",
                    "DAG per-hop exact original-shape replay accepted: child "
                    "id={0} name={1} parent id={2} name={3} "
                    "sourceCurFormal={4} parentFormal={5} old='{6}' new='{7}'",
                    cur.id, cur.name, parent->id, parent->name, curFormal,
                    parentFormal, oldTrim, *replaySyntax);
                return FormalTextPair{oldTrim.str(), std::move(*replaySyntax)};
              }
            }
          }

          return FormalTextPair{oldTrim.str(), newTrim.str()};
        };

        DenseMap<uint32_t, SmallVector<ObservedFormalConstraint, 2>>
            parentObserved;
        DenseMap<uint32_t, SmallVector<uint32_t, 2>> parentObservedSources;
        SmallVector<uint32_t, 4> unresolvedChildFormals;
        SmallVector<std::string, 4> unresolvedDerivationDetails;

        auto recordObservedParentConstraint =
            [&](uint32_t parentFormal,
                const ObservedFormalConstraint &constraint,
                uint32_t sourceCurFormal) {
              auto &constraints = parentObserved[parentFormal];
              bool seen = false;
              for (const auto &existing : constraints) {
                if (existing.oldText == constraint.oldText &&
                    existing.newText == constraint.newText) {
                  seen = true;
                  break;
                }
              }
              if (!seen)
                constraints.push_back(constraint);

              auto &sources = parentObservedSources[parentFormal];
              if (llvm::find(sources, sourceCurFormal) == sources.end())
                sources.push_back(sourceCurFormal);
            };
        for (const auto &KV : curFormals) {
          const uint32_t curFormal = KV.first;
          StringRef curOld = KV.second.oldText;
          StringRef curNew = KV.second.newText;

          auto derivationCert = buildParentConstraintDerivationCertificate(
              cur, curFormal, curOld, curNew, "DAG per-hop");
          cert.derivations.push_back(derivationCert);
          if (!derivationCert.valid) {
            if (parent) {
              auto siblingLift = tryBuildExactSiblingRerootLift(
                  *parent, cur, curFormal, curOld, curNew);
              if (siblingLift) {
                trace("macro/dag",
                      "DAG per-hop exact sibling reroot accepted: child "
                      "id={0} name={1} parent id={2} name={3} curFormal={4} "
                      "nextFormals={5}",
                      cur.id, cur.name, parent->id, parent->name, curFormal,
                      formatFormalTextPairs(siblingLift->nextFormals));
                for (const auto &derived : siblingLift->nextFormals) {
                  const uint32_t parentFormal = derived.first;
                  const ObservedFormalConstraint constraint{
                      derived.second.oldText, derived.second.newText};
                  recordObservedParentConstraint(parentFormal, constraint,
                                                 curFormal);
                }
                continue;
              }
            }

            unresolvedChildFormals.push_back(curFormal);
            unresolvedDerivationDetails.push_back(derivationCert.detail);
            continue;
          }

          for (const auto &derived : derivationCert.derivedConstraints) {
            const uint32_t parentFormal = derived.first;
            const ObservedFormalConstraint &constraint = derived.second;
            recordObservedParentConstraint(parentFormal, constraint, curFormal);
          }
        }

        DenseMap<uint64_t, std::string> preferredChildSyntax;
        if (!cert.rewrittenChildSyntax.empty())
          preferredChildSyntax[cur.id] = cert.rewrittenChildSyntax;

        trace("macro/dag",
              "DAG per-hop derived parent observations: child id={0} name={1} "
              "parent id={2} name={3} observed={4} preferredChildSyntax='{5}'",
              cur.id, cur.name, parent->id, parent->name,
              formatObservedConstraintsMap(parentObserved),
              cert.rewrittenChildSyntax);

        auto observedParentArgIdxs = CollectSortedUInt32Keys(parentObserved);
        auto requiredParentPasteArgIdxs =
            CollectSortedUniquePasteArgIdxs(parent->pasteSpans);
        trace("macro/proof",
              "DAG per-hop proof ledger observed: child id={0} name={1} "
              "parent id={2} name={3} observedParentArgs={4} "
              "requiredParentPasteArgs={5} unresolvedChildFormals={6}",
              cur.id, cur.name, parent->id, parent->name,
              FormatUInt32List(observedParentArgIdxs),
              FormatUInt32List(requiredParentPasteArgIdxs),
              FormatUInt32List(unresolvedChildFormals));

        DenseMap<uint32_t, FormalTextPair> parentFormals;
        for (const auto &KV : parentObserved) {
          const uint32_t parentFormal = KV.first;
          auto formalCert = buildObservedFormalRewriteCertificate(
              *parent, parentFormal, KV.second,
              preferredChildSyntax.empty() ? nullptr : &preferredChildSyntax,
              "DAG per-hop");
          cert.parentFormalCertificates.push_back(formalCert);
          if (formalCert.kind == FormalRewriteCertificateKind::Invalid) {
            const bool templateMismatch =
                formalCert.failure ==
                FormalRewriteFailure::MissingStructuralTemplate;
            auto srcIt = parentObservedSources.find(parentFormal);
            if (templateMismatch && srcIt != parentObservedSources.end() &&
                srcIt->second.size() == 1) {
              const uint32_t sourceCurFormal = srcIt->second.front();
              auto curIt = curFormals.find(sourceCurFormal);
              if (curIt != curFormals.end()) {
                if (auto flatten = tryBuildDirectPassthroughParentFormalRewrite(
                        sourceCurFormal, parentFormal, curIt->second.newText)) {
                  trace("macro/dag",
                        "DAG per-hop parent formal passthrough flatten: child "
                        "id={0} name={1} parent id={2} name={3} "
                        "sourceCurFormal={4} parentFormal={5} old='{6}' "
                        "new='{7}'",
                        cur.id, cur.name, parent->id, parent->name,
                        sourceCurFormal, parentFormal, flatten->oldText,
                        flatten->newText);
                  parentFormals[parentFormal] = std::move(*flatten);
                  continue;
                }
              }
            }
            trace("macro/dag",
                  "DAG per-hop parent formal invalid: child id={0} name={1} "
                  "parent id={2} name={3} parentFormal={4} observed={5} "
                  "preferredChildSyntax='{6}' detail={7}",
                  cur.id, cur.name, parent->id, parent->name, parentFormal,
                  formatObservedConstraintsMap(parentObserved),
                  cert.rewrittenChildSyntax, formalCert.detail);
            cert.failureReason =
                StructuredLiftFailureReason::ParentFormalInvalid;
            cert.parentFormalFailure = formalCert.failure;
            cert.detail =
                formatv("{0}; lexical bridge required", formalCert.detail)
                    .str();
            cert.kind = StructuredLiftCertificateKind::NeedsLexicalBridge;
            cert.nextInv = parent;
            return cert;
          }
          if (formalCert.kind == FormalRewriteCertificateKind::NoChange) {
            trace("macro/dag",
                  "DAG per-hop parent formal no-change preserved in "
                  "nextFormals: child id={0} name={1} parent id={2} name={3} "
                  "parentFormal={4} old='{5}' new='{6}'",
                  cur.id, cur.name, parent->id, parent->name, parentFormal,
                  formalCert.oldText, formalCert.newText);
          }

          parentFormals[parentFormal] =
              FormalTextPair{formalCert.oldText, formalCert.newText};
        }

        auto carriedParentArgIdxs = CollectSortedUInt32Keys(parentFormals);
        auto missingParentSupportArgIdxs = ComputeSortedMissingUInt32s(
            requiredParentPasteArgIdxs, carriedParentArgIdxs);
        trace("macro/proof",
              "DAG per-hop proof ledger carried: child id={0} name={1} "
              "parent id={2} name={3} observedParentArgs={4} "
              "carriedParentArgs={5} "
              "requiredParentPasteArgs={6} missingSupport={7} "
              "unresolvedChildFormals={8}",
              cur.id, cur.name, parent->id, parent->name,
              FormatUInt32List(observedParentArgIdxs),
              FormatUInt32List(carriedParentArgIdxs),
              FormatUInt32List(requiredParentPasteArgIdxs),
              FormatUInt32List(missingParentSupportArgIdxs),
              FormatUInt32List(unresolvedChildFormals));

        auto unresolvedDerivationsDischargedByBodySpace = [&]() {
          if (unresolvedChildFormals.empty())
            return true;
          for (const auto &formalCert : cert.parentFormalCertificates) {
            const auto &sig = formalCert.interactionConsistency.signature;
            if (sig.usesPreferredChildSyntax ||
                sig.usesRawInvocationPreservation ||
                sig.usesRawChildInvocationLogicalInput)
              return true;
          }
          return false;
        };

        if (!unresolvedDerivationsDischargedByBodySpace()) {
          cert.failureReason =
              StructuredLiftFailureReason::ParentConstraintDerivationFailed;
          cert.derivationFailure =
              ParentConstraintDerivationFailure::InversionNotUnique;
          cert.detail =
              unresolvedDerivationDetails.empty()
                  ? formatv("DAG per-hop: unresolved parent "
                            "constraint derivation requires "
                            "lexical bridge: child id={0} name={1} "
                            "parent id={2} name={3}",
                            cur.id, cur.name, parent->id, parent->name)
                        .str()
                  : unresolvedDerivationDetails.front();
          trace("macro/dag",
                "DAG per-hop unresolved derivation not discharged: child "
                "id={0} name={1} parent id={2} name={3} unresolved={4} "
                "parentFormals={5} reason={6}",
                cur.id, cur.name, parent->id, parent->name,
                FormatUInt32List(unresolvedChildFormals),
                formatFormalTextPairs(parentFormals), cert.detail);
          cert.kind = StructuredLiftCertificateKind::NeedsLexicalBridge;
          cert.nextInv = parent;
          return cert;
        }

        if (!unresolvedChildFormals.empty()) {
          trace("macro/dag",
                "DAG per-hop unresolved derivation discharged by body-space "
                "semantics: child id={0} name={1} parent id={2} name={3} "
                "unresolved={4} parentFormals={5}",
                cur.id, cur.name, parent->id, parent->name,
                FormatUInt32List(unresolvedChildFormals),
                formatFormalTextPairs(parentFormals));
        }

        auto parentCert = buildWrapperPlaceholderHopInvocationCertificate(
            *parent, parentFormals, "DAG per-hop");
        cert.parentCert = parentCert;
        if (parentCert.kind == InvocationRewriteCertificateKind::Invalid) {
          trace("macro/dag",
                "DAG per-hop parent invocation invalid: child id={0} name={1} "
                "parent id={2} name={3} parentFormals={4} detail={5}",
                cur.id, cur.name, parent->id, parent->name,
                formatFormalTextPairs(parentFormals), parentCert.detail);
          cert.failureReason =
              StructuredLiftFailureReason::ParentInvocationInvalid;
          cert.parentInvocationFailure = parentCert.failure;
          cert.detail = parentCert.detail;
          cert.kind = StructuredLiftCertificateKind::NeedsLexicalBridge;
          cert.nextInv = parent;
          return cert;
        }
        if (parentCert.kind == InvocationRewriteCertificateKind::NoChange) {
          cert.detail =
              formatv("DAG per-hop: parent invocation no-change child "
                      "id={0} name={1} parent id={2} name={3} "
                      "parentFormals={4}",
                      cur.id, cur.name, parent->id, parent->name,
                      parentFormals.size())
                  .str();
        } else {
          cert.detail =
              formatv("DAG per-hop: structured hop child id={0} name={1} "
                      "parent id={2} name={3} parentFormals={4}",
                      cur.id, cur.name, parent->id, parent->name,
                      parentFormals.size())
                  .str();
        }

        trace("macro/dag",
              "DAG per-hop UNIQUE: child id={0} name={1} parent id={2} "
              "name={3} nextFormals={4} detail={5}",
              cur.id, cur.name, parent->id, parent->name,
              formatFormalTextPairs(parentFormals), cert.detail);
        cert.kind = StructuredLiftCertificateKind::Unique;
        cert.nextInv = parent;
        cert.nextFormals = std::move(parentFormals);
        cert.parentFormalSources = std::move(parentObservedSources);
        return cert;
      };

      enum class LiftChainCertificateKind {
        Unique,
        Invalid,
      };

      struct LiftChainCertificate {
        LiftChainCertificateKind kind = LiftChainCertificateKind::Invalid;
        const RefoldModel::MacroInvocation *leaf = nullptr;
        SmallVector<uint32_t, 4> leafArgIdxs;
        DenseMap<uint32_t, FormalTextPair> leafFormals;
        SmallVector<StructuredLiftCertificate, 4> steps;
        bool usedLexicalBridge = false;
        DenseSet<uint32_t> bridgedRootArgIdxs;
        DenseMap<uint32_t, FormalTextPair> rootFormals;
        std::string detail;
      };

      auto buildLiftChainCertificate =
          [&](const RefoldModel::MacroInvocation &leaf,
              const DenseMap<uint32_t, FormalTextPair> &leafFormals)
          -> LiftChainCertificate {
        LiftChainCertificate cert;
        cert.leaf = &leaf;

        for (const auto &KV : leafFormals) {
          StringRef oldText = StringRef(KV.second.oldText).trim();
          StringRef newText = StringRef(KV.second.newText).trim();
          if (oldText == newText)
            continue;
          cert.leafArgIdxs.push_back(KV.first);
          cert.leafFormals[KV.first] =
              FormalTextPair{oldText.str(), newText.str()};
        }
        llvm::sort(cert.leafArgIdxs);

        if (cert.leafFormals.empty()) {
          cert.detail = formatv(
                            "DAG lift chain: leaf id={0} name={1} has no "
                            "distinct leaf formals to lift",
                            leaf.id, leaf.name)
                            .str();
          return cert;
        }

        const RefoldModel::MacroInvocation *cur = &leaf;
        DenseMap<uint32_t, FormalTextPair> curFormals;
        DenseSet<uint32_t> bridgedCurFormals;
        curFormals = cert.leafFormals;

        while (cur->id != m.id) {
          auto step = buildStructuredLiftCertificate(*cur, curFormals);
          cert.steps.push_back(step);
          trace("macro/dag",
                "DAG lift-chain step: current id={0} name={1} stepKind={2} "
                "detail={3}",
                cur->id, cur->name, static_cast<unsigned>(step.kind),
                step.detail);
          if (step.kind == StructuredLiftCertificateKind::Invalid) {
            cert.detail = step.detail;
            return cert;
          }

          if (step.kind == StructuredLiftCertificateKind::Unique) {
            if (!step.nextInv) {
              cert.detail = formatv(
                                "DAG lift chain: missing next invocation "
                                "after structured hop child id={0} name={1}",
                                cur->id, cur->name)
                                .str();
              return cert;
            }

            DenseSet<uint32_t> nextBridgedCurFormals;
            if (!bridgedCurFormals.empty()) {
              for (const auto &KV : step.nextFormals) {
                auto srcIt = step.parentFormalSources.find(KV.first);
                if (srcIt == step.parentFormalSources.end())
                  continue;
                for (uint32_t sourceCurFormal : srcIt->second) {
                  if (bridgedCurFormals.contains(sourceCurFormal)) {
                    nextBridgedCurFormals.insert(KV.first);
                    break;
                  }
                }
              }
            }

            cert.steps.back().bridgedNextFormals = nextBridgedCurFormals;
            cur = step.nextInv;
            curFormals = std::move(step.nextFormals);
            bridgedCurFormals = std::move(nextBridgedCurFormals);
            continue;
          }

          if (!step.nextInv || step.rewrittenChildSyntax.empty()) {
            cert.detail = formatv(
                              "DAG lift chain: lexical bridge unavailable "
                              "child id={0} name={1}",
                              cur->id, cur->name)
                              .str();
            return cert;
          }

          trace("macro/dag",
                "DAG lift-chain lexical bridge attempt: parent id={0} "
                "name={1} child id={2} name={3} rewrittenChildSyntax='{4}'",
                step.nextInv->id, step.nextInv->name, cur->id, cur->name,
                step.rewrittenChildSyntax);
          auto bridged = tryLexicalChildBridge(*step.nextInv, *cur,
                                               step.rewrittenChildSyntax);
          if (!bridged) {
            cert.detail = formatv(
                              "DAG lift chain: lexical bridge failed "
                              "parent id={0} name={1} child id={2} name={3}",
                              step.nextInv->id, step.nextInv->name, cur->id,
                              cur->name)
                              .str();
            return cert;
          }

          {
            std::string bridgedDesc;
            raw_string_ostream os(bridgedDesc);
            os << "{";
            bool first = true;
            for (const auto &KV : *bridged) {
              if (!first)
                os << ", ";
              first = false;
              os << KV.first << ":'" << KV.second.oldText << "'->'"
                 << KV.second.newText << "'";
            }
            os << "}";
            trace("macro/dag",
                  "DAG lift-chain lexical bridge success: parent id={0} "
                  "name={1} bridgedFormals={2}",
                  step.nextInv->id, step.nextInv->name, os.str());
          }
          cert.usedLexicalBridge = true;
          cur = step.nextInv;
          curFormals = std::move(*bridged);
          bridgedCurFormals.clear();
          for (const auto &KV : curFormals)
            bridgedCurFormals.insert(KV.first);
          cert.steps.back().bridgedNextFormals = bridgedCurFormals;
        }

        for (const auto &KV : curFormals) {
          cert.rootFormals[KV.first] = KV.second;
          if (bridgedCurFormals.contains(KV.first))
            cert.bridgedRootArgIdxs.insert(KV.first);
        }
        cert.kind = LiftChainCertificateKind::Unique;
        cert.detail = formatv(
                          "DAG lift chain: leaf id={0} name={1} leafArgs={2} "
                          "steps={3} lexicalBridge={4} rootFormals={5}",
                          leaf.id, leaf.name,
                          FormatUInt32List(cert.leafArgIdxs), cert.steps.size(),
                          cert.usedLexicalBridge ? 1 : 0,
                          cert.rootFormals.size())
                          .str();
        return cert;
      };

      auto mergeCompatibleRootFormalRewrites =
          [&](StringRef baseOld0, ArrayRef<FormalTextPair> rewrites)
          -> std::optional<std::string> {
        return mergeCompatibleFormalRewrites(baseOld0, rewrites);
      };

      enum class RootFormalMergeCertificateKind {
        NoChange,
        Unique,
        Invalid,
      };

      enum class RootFormalMergeFailure {
        None,
        ArgIndexOutOfBounds,
        InvalidArgRange,
        MergeConflict,
      };

      struct RootFormalMergeCertificate {
        RootFormalMergeCertificateKind kind =
            RootFormalMergeCertificateKind::Invalid;
        RootFormalMergeFailure failure = RootFormalMergeFailure::None;
        uint32_t argIdx = 0;
        SmallVector<FormalTextPair, 2> observedRewrites;
        std::string baseArgText;
        std::string mergedArgText;
        std::string detail;
      };

      auto buildRootFormalMergeCertificate =
          [&](uint32_t argIdx, ArrayRef<FormalTextPair> rewrites,
              StringRef traceStage) -> RootFormalMergeCertificate {
        RootFormalMergeCertificate cert;
        cert.argIdx = argIdx;
        cert.observedRewrites.assign(rewrites.begin(), rewrites.end());

        if (argIdx >= invArgRanges.size()) {
          cert.failure = RootFormalMergeFailure::ArgIndexOutOfBounds;
          cert.detail = formatv(
                            "{0}: root arg index out of bounds root id={1} "
                            "name={2} argIdx={3} numArgs={4}",
                            traceStage, m.id, m.name, argIdx,
                            invArgRanges.size())
                            .str();
          return cert;
        }

        const size_t begin = invArgRanges[argIdx].first;
        const size_t end = invArgRanges[argIdx].second;
        if (begin > end || end > invSpanText.size()) {
          cert.failure = RootFormalMergeFailure::InvalidArgRange;
          cert.detail =
              formatv("{0}: root arg range invalid root id={1} name={2} "
                      "argIdx={3} range=[{4},{5}) spanLen={6}",
                      traceStage, m.id, m.name, argIdx, begin, end,
                      invSpanText.size())
                  .str();
          return cert;
        }

        const StringRef baseArgText = invSpanText.slice(begin, end).trim();
        cert.baseArgText = baseArgText.str();

        auto mergedNewArg =
            mergeCompatibleRootFormalRewrites(baseArgText, rewrites);
        if (!mergedNewArg) {
          cert.failure = RootFormalMergeFailure::MergeConflict;
          cert.detail = formatv(
                            "{0}: root rewrite merge conflicted root id={1} "
                            "name={2} argIdx={3}",
                            traceStage, m.id, m.name, argIdx)
                            .str();
          return cert;
        }

        cert.mergedArgText = StringRef(*mergedNewArg).trim().str();
        cert.kind = cert.baseArgText == cert.mergedArgText
                        ? RootFormalMergeCertificateKind::NoChange
                        : RootFormalMergeCertificateKind::Unique;
        return cert;
      };

      // Small utility structs for accumulating per-formal old/new and then
      // converting them into root-level byte edits in invSpanText.
      struct OldNewText {
        std::string oldText;
        std::string newText;
      };

      enum class SubtreeRewriteCertificateKind {
        NoChange,
        Unique,
        Invalid,
      };

      struct SubtreeInteractionSummaryCertificate {
        bool valid = true;
        bool hasPaste = false;
        bool hasStringify = false;
        bool hasWideStringify = false;
        bool hasRawInvocation = false;
        bool hasPreferredChildSyntax = false;
        bool hasMixedInteractions = false;
        bool hasStringifyPaste = false;
        bool hasWideStringifyPaste = false;
        bool hasRawInvocationPaste = false;
        bool hasChildSyntaxPaste = false;
        SmallVector<SemanticInteractionCertificate, 16> interactions;
        std::string detail;
      };

      enum class SubtreeSemanticAdmissibilityFailure {
        None,
        MixedSemanticInteractions,
        LexicalBridgeWithStructuredSemantics,
        DeferredPasteNotDischarged,
        PasteWithPassthroughFlatten,
      };

      struct SubtreeSemanticAdmissibilityCertificate {
        bool valid = true;
        SubtreeSemanticAdmissibilityFailure failure =
            SubtreeSemanticAdmissibilityFailure::None;
        std::string detail;
      };

      enum class DeferredPasteDischargeFailure {
        None,
        MissingSemanticDischarge,
        MissingAncestorPasteValidation,
      };

      struct DeferredPasteDischargeCertificate {
        bool valid = true;
        DeferredPasteDischargeFailure failure =
            DeferredPasteDischargeFailure::None;
        SmallVector<const InvocationRewriteCertificate *, 4>
            deferredInvocations;
        std::string detail;
      };

      struct SubtreeSemanticCertificate {
        bool valid = false;
        bool usesLexicalBridge = false;
        bool touchesPaste = false;
        bool hasWrapperSemantics = false;
        bool hasStringifySemantics = false;
        bool hasWideStringifySemantics = false;
        bool hasPreferredChildSyntax = false;
        bool hasRawInvocationPreservation = false;
        bool hasPassthroughFlatten = false;
        bool hasBridgeSensitiveStructuredSemantics = false;
        bool hasAcceptedRootPlaceholderReplay = false;
        bool rootReplayFlattensOnlyWholeChildArgs = false;
        const RefoldModel::MacroInvocation *acceptedRootReplayInv = nullptr;
        SmallVector<InvocationRewriteCertificate, 8> invocationCertificates;
        SmallVector<FormalRewriteCertificate, 16> formalCertificates;
        SmallVector<ArgSemanticRewriteCertificate, 16> argCertificates;
        SmallVector<SlotSemanticRewriteCertificate, 32> slotCertificates;
        SmallVector<SemanticInteractionCertificate, 32> interactionCertificates;
        SmallVector<FormalInteractionConsistencyCertificate, 16>
            formalInteractionConsistencies;
        SmallVector<RawFormalValidationCertificate, 16> rawFormalValidations;
        SmallVector<PasteRewriteValidationCertificate, 8> pasteValidations;
        SmallVector<ParentConstraintDerivationCertificate, 16>
            parentDerivations;
        SmallVector<StructuredLiftCertificate, 8> structuredLiftCertificates;
        SmallVector<LiftChainCertificate, 4> liftChains;
        SmallVector<RootFormalMergeCertificate, 4> rootMergeCertificates;
        StringSet<> bridgedFormalKeys;
        StringSet<> bridgedInteractionKeys;
        SubtreeInteractionSummaryCertificate interactionSummary;
        SubtreeInteractionConsistencyCertificate interactionConsistency;
        DeferredPasteDischargeCertificate deferredPasteDischarge;
        SubtreeSemanticAdmissibilityCertificate admissibility;
        std::string detail;
      };

      struct SubtreeRewriteCertificate {
        SubtreeRewriteCertificateKind kind =
            SubtreeRewriteCertificateKind::Invalid;
        const RefoldModel::MacroInvocation *leaf = nullptr;
        const RefoldModel::MacroInvocation *root = nullptr;
        DenseMap<uint32_t, FormalTextPair> leafFormals;
        DenseMap<uint32_t, FormalTextPair> rootFormals;
        SmallVector<uint32_t, 8> deferRootOccurrenceArgIdxs;
        InvocationRewriteCertificate leafCert;
        InvocationRewriteCertificate rootCert;
        SmallVector<LiftChainCertificate, 4> liftCertificates;
        SmallVector<RootFormalMergeCertificate, 4> rootMergeCertificates;
        SubtreeSemanticCertificate semantic;
        std::string detail;
      };

      auto buildSubtreeInteractionSummaryCertificate =
          [&](ArrayRef<SemanticInteractionCertificate> interactions)
          -> SubtreeInteractionSummaryCertificate {
        SubtreeInteractionSummaryCertificate cert;
        cert.interactions.assign(interactions.begin(), interactions.end());

        for (const auto &interaction : interactions) {
          cert.hasPaste |= interaction.touchesPaste;
          cert.hasStringify |= interaction.usesStringify;
          cert.hasWideStringify |= interaction.usesWideStringify;
          cert.hasRawInvocation |= interaction.usesRawInvocationPreservation;
          cert.hasPreferredChildSyntax |= interaction.usesPreferredChildSyntax;
          switch (interaction.kind) {
          case SemanticInteractionKind::Plain:
          case SemanticInteractionKind::ChildSyntax:
          case SemanticInteractionKind::RawInvocation:
          case SemanticInteractionKind::Stringify:
          case SemanticInteractionKind::WideStringify:
          case SemanticInteractionKind::Paste:
            break;
          case SemanticInteractionKind::ChildSyntaxPaste:
            cert.hasChildSyntaxPaste = true;
            break;
          case SemanticInteractionKind::RawInvocationPaste:
            cert.hasRawInvocationPaste = true;
            break;
          case SemanticInteractionKind::StringifyPaste:
            cert.hasStringifyPaste = true;
            break;
          case SemanticInteractionKind::WideStringifyPaste:
            cert.hasStringifyPaste = true;
            cert.hasWideStringifyPaste = true;
            break;
          case SemanticInteractionKind::Mixed:
            cert.hasMixedInteractions = true;
            break;
          }
        }

        cert.detail =
            formatv("subtree interaction summary: interactions={0} "
                    "paste={1} stringify={2} wide={3} rawInvocation={4} "
                    "childSyntax={5} stringifyPaste={6} "
                    "wideStringifyPaste={7} rawInvocationPaste={8} "
                    "childSyntaxPaste={9} mixed={10}",
                    cert.interactions.size(), cert.hasPaste ? 1 : 0,
                    cert.hasStringify ? 1 : 0, cert.hasWideStringify ? 1 : 0,
                    cert.hasRawInvocation ? 1 : 0,
                    cert.hasPreferredChildSyntax ? 1 : 0,
                    cert.hasStringifyPaste ? 1 : 0,
                    cert.hasWideStringifyPaste ? 1 : 0,
                    cert.hasRawInvocationPaste ? 1 : 0,
                    cert.hasChildSyntaxPaste ? 1 : 0,
                    cert.hasMixedInteractions ? 1 : 0)
                .str();
        return cert;
      };

      auto buildSubtreeInteractionConsistencyCertificate =
          [&](ArrayRef<FormalRewriteCertificate> formalCertificates)
          -> SubtreeInteractionConsistencyCertificate {
        SubtreeInteractionConsistencyCertificate cert;
        StringMap<size_t> keyToIndex;

        auto makeKey = [&](const RefoldModel::MacroInvocation *inv,
                           uint32_t argIdx) -> std::string {
          return formatv("{0}#{1}", inv ? inv->id : 0, argIdx).str();
        };

        for (const auto &formalCert : formalCertificates) {
          cert.formalConsistencies.push_back(formalCert.interactionConsistency);
          const auto &consistency = cert.formalConsistencies.back();
          if (!consistency.valid) {
            cert.valid = false;
            cert.failure =
                SubtreeInteractionConsistencyFailure::DivergentFormalSemantics;
            cert.detail = consistency.detail;
            return cert;
          }

          std::string key = makeKey(consistency.inv, consistency.argIdx);
          auto it = keyToIndex.find(key);
          if (it == keyToIndex.end()) {
            keyToIndex[key] = cert.formalConsistencies.size() - 1;
            continue;
          }

          const auto &existing = cert.formalConsistencies[it->second];
          if (!(existing.signature == consistency.signature)) {
            cert.valid = false;
            cert.failure =
                SubtreeInteractionConsistencyFailure::DivergentFormalSemantics;
            cert.detail = formatv(
                              "subtree semantic interaction consistency "
                              "failed: inv id={0} argIdx={1} formal semantic "
                              "evidence diverged across lift/root paths",
                              consistency.inv ? consistency.inv->id : 0,
                              consistency.argIdx)
                              .str();
            return cert;
          }
        }

        cert.detail = formatv(
                          "subtree interaction consistency: formals={0} "
                          "uniqueFormals={1}",
                          cert.formalConsistencies.size(), keyToIndex.size())
                          .str();
        return cert;
      };

      auto buildSubtreeDeferredPasteDischargeCertificate =
          [&](const SubtreeSemanticCertificate &semantic,
              const InvocationRewriteCertificate &rootCert)
          -> DeferredPasteDischargeCertificate {
        DeferredPasteDischargeCertificate cert;

        auto isAncestorOrSame =
            [&](const RefoldModel::MacroInvocation *ancestor,
                const RefoldModel::MacroInvocation *descendant) -> bool {
          if (!ancestor || !descendant)
            return false;
          const RefoldModel::MacroInvocation *cur = descendant;
          while (cur) {
            if (cur->id == ancestor->id)
              return true;
            if (!cur->callerMacroId)
              break;
            auto it = invById.find(*cur->callerMacroId);
            if (it == invById.end())
              break;
            cur = it->second;
          }
          return false;
        };

        auto rootReplayMatchesLexicalBridgeChain =
            [&](const InvocationRewriteCertificate &deferredInvCert) -> bool {
          if (!deferredInvCert.inv || !rootCert.inv ||
              !rootCert.pasteValidation.valid)
            return false;

          for (const auto &liftCert : semantic.liftChains) {
            if (!liftCert.leaf)
              continue;
            if (!isAncestorOrSame(deferredInvCert.inv, liftCert.leaf))
              continue;
            if (!liftCert.usedLexicalBridge ||
                liftCert.bridgedRootArgIdxs.empty())
              continue;

            bool allBridgedRootFormalsMatched = true;
            for (uint32_t rootArgIdx : liftCert.bridgedRootArgIdxs) {
              auto rootFormalIt = liftCert.rootFormals.find(rootArgIdx);
              if (rootFormalIt == liftCert.rootFormals.end()) {
                allBridgedRootFormalsMatched = false;
                break;
              }

              bool matchedRewrite = false;
              for (const auto &rewrite : rootCert.rewrites) {
                if (rewrite.argIdx != rootArgIdx)
                  continue;
                if (rewrite.oldText == rootFormalIt->second.oldText &&
                    rewrite.newText == rootFormalIt->second.newText) {
                  matchedRewrite = true;
                  break;
                }
              }
              if (!matchedRewrite) {
                allBridgedRootFormalsMatched = false;
                break;
              }
            }

            if (allBridgedRootFormalsMatched)
              return true;
          }
          return false;
        };

        for (const auto &invCert : semantic.invocationCertificates) {
          if (!invCert.pasteValidation.deferred)
            continue;
          cert.deferredInvocations.push_back(&invCert);

          bool hasSemanticDischarge = false;
          for (const auto &formalCert : semantic.formalCertificates) {
            if (!isAncestorOrSame(formalCert.inv, invCert.inv))
              continue;
            const auto &sig = formalCert.interactionConsistency.signature;
            if (sig.usesPreferredChildSyntax ||
                sig.usesRawInvocationPreservation || sig.usesStringify ||
                sig.usesWideStringify ||
                sig.usesRawChildInvocationLogicalInput) {
              hasSemanticDischarge = true;
              break;
            }
          }

          bool hasAncestorReplayPath = false;
          for (const auto &candidate : semantic.invocationCertificates) {
            if (!candidate.pasteValidation.valid)
              continue;
            if (!isAncestorOrSame(candidate.inv, invCert.inv))
              continue;
            if (candidate.inv && invCert.inv &&
                candidate.inv->id == invCert.inv->id)
              continue;
            hasAncestorReplayPath = true;
            break;
          }

          const bool hasAcceptedRootReplayCandidate =
              (invCert.inv && rootCert.inv &&
               invCert.inv->id == rootCert.inv->id &&
               rootCert.pasteValidation.valid) ||
              rootReplayMatchesLexicalBridgeChain(invCert);

          trace("macro/dag",
                "subtree deferred discharge probe: deferredInv id={0} name={1} "
                "rootInv id={2} name={3} rootKind={4} rootPasteRequired={5} "
                "rootPasteValid={6} rootPasteDeferred={7} hasSemantic={8} "
                "hasAncestorReplay={9} hasAcceptedRootReplay={10}",
                invCert.inv ? invCert.inv->id : 0,
                invCert.inv ? invCert.inv->name : StringRef("<none>"),
                rootCert.inv ? rootCert.inv->id : 0,
                rootCert.inv ? rootCert.inv->name : StringRef("<none>"),
                static_cast<unsigned>(rootCert.kind),
                rootCert.pasteValidation.required ? 1 : 0,
                rootCert.pasteValidation.valid ? 1 : 0,
                rootCert.pasteValidation.deferred ? 1 : 0,
                hasSemanticDischarge ? 1 : 0, hasAncestorReplayPath ? 1 : 0,
                hasAcceptedRootReplayCandidate ? 1 : 0);

          if (!(hasSemanticDischarge || hasAncestorReplayPath ||
                hasAcceptedRootReplayCandidate)) {
            cert.valid = false;
            cert.failure = hasSemanticDischarge
                               ? DeferredPasteDischargeFailure::
                                     MissingAncestorPasteValidation
                               : DeferredPasteDischargeFailure::
                                     MissingSemanticDischarge;
            cert.detail = formatv(
                              "subtree deferred paste discharge failed: inv "
                              "id={0} name={1} has deferred paste validation "
                              "without ancestor semantic discharge, "
                              "accepted ancestor replay path, or accepted "
                              "root replay candidate",
                              invCert.inv ? invCert.inv->id : 0,
                              invCert.inv ? invCert.inv->name
                                          : StringRef("<none>"))
                              .str();
            return cert;
          }
        }

        cert.detail = formatv(
                          "subtree deferred paste discharge: deferredInvs={0}",
                          cert.deferredInvocations.size())
                          .str();
        return cert;
      };

      auto buildSubtreeSemanticAdmissibilityCertificate =
          [&](const SubtreeSemanticCertificate &semantic)
          -> SubtreeSemanticAdmissibilityCertificate {
        SubtreeSemanticAdmissibilityCertificate cert;

        if (!semantic.deferredPasteDischarge.valid) {
          trace("macro/dag",
                "subtree admissibility reject(deferred): lexicalBridge={0} "
                "paste={1} wrappers={2} preferredChildSyntax={3} "
                "rawInvocation={4} deferredDetail={5}",
                semantic.usesLexicalBridge ? 1 : 0,
                semantic.touchesPaste ? 1 : 0,
                semantic.hasWrapperSemantics ? 1 : 0,
                semantic.hasPreferredChildSyntax ? 1 : 0,
                semantic.hasRawInvocationPreservation ? 1 : 0,
                semantic.deferredPasteDischarge.detail);
          cert.valid = false;
          cert.failure =
              SubtreeSemanticAdmissibilityFailure::DeferredPasteNotDischarged;
          cert.detail = semantic.deferredPasteDischarge.detail;
          return cert;
        }

        if (semantic.interactionSummary.hasMixedInteractions) {
          trace("macro/dag",
                "subtree admissibility reject(mixed): lexicalBridge={0} "
                "paste={1} wrappers={2} preferredChildSyntax={3} "
                "rawInvocation={4} summary={5}",
                semantic.usesLexicalBridge ? 1 : 0,
                semantic.touchesPaste ? 1 : 0,
                semantic.hasWrapperSemantics ? 1 : 0,
                semantic.hasPreferredChildSyntax ? 1 : 0,
                semantic.hasRawInvocationPreservation ? 1 : 0,
                semantic.interactionSummary.detail);
          cert.valid = false;
          cert.failure =
              SubtreeSemanticAdmissibilityFailure::MixedSemanticInteractions;
          cert.detail =
              "subtree semantic admissibility failed: mixed wrapper/"
              "stringify/paste interactions are not structurally admissible";
          return cert;
        }

        const bool hasInteractionScopedPasteSemantics =
            semantic.interactionSummary.hasPaste ||
            semantic.interactionSummary.hasStringifyPaste ||
            semantic.interactionSummary.hasWideStringifyPaste ||
            semantic.interactionSummary.hasRawInvocationPaste ||
            semantic.interactionSummary.hasChildSyntaxPaste;
        const bool hasStructuredSemantics =
            semantic.hasWrapperSemantics || semantic.hasPreferredChildSyntax ||
            semantic.hasRawInvocationPreservation ||
            hasInteractionScopedPasteSemantics;
        const bool hasBridgeSensitiveStructuredSemantics =
            semantic.hasBridgeSensitiveStructuredSemantics;

        // Strict mode must fail closed when a paste-bearing subtree only
        // reaches its parent through passthrough flatten. That rewrite path
        // intentionally drops interior structural boundaries, which makes
        // nested pasted-token edits underdetermined: multiple replay candidates
        // can survive even though they share the same final pasted spelling.
        //
        // One narrow proof class is still admissible: if the accepted root
        // replay itself is a deferred wrapper-placeholder replay whose
        // rewritten syntax is known, and every rewritten root formal
        // corresponds to exactly one whole-child placeholder, then the parent
        // invocation is certified while only the child subtree is flattened. In
        // that case we are not inventing interior child structure; we are
        // preserving only the ancestor syntax that has already been proven
        // replayable.
        const bool allowRootPlaceholderFlattenReplay =
            semantic.hasAcceptedRootPlaceholderReplay &&
            semantic.rootReplayFlattensOnlyWholeChildArgs &&
            semantic.acceptedRootReplayInv &&
            semantic.deferredPasteDischarge.valid &&
            !semantic.deferredPasteDischarge.deferredInvocations.empty() &&
            llvm::all_of(
                semantic.deferredPasteDischarge.deferredInvocations,
                [&](const InvocationRewriteCertificate *invCert) {
                  return invCert && invCert->inv &&
                         invCert->inv->id == semantic.acceptedRootReplayInv->id;
                });
        if (semantic.touchesPaste && semantic.hasPassthroughFlatten &&
            !allowRootPlaceholderFlattenReplay) {
          trace("macro/dag",
                "subtree admissibility reject(lossy pasted flatten): "
                "lexicalBridge={0} structuredSemantics={1} paste={2} "
                "interactionPaste={3} passthroughFlatten={4} wrappers={5} "
                "preferredChildSyntax={6} rawInvocation={7} summary={8}",
                semantic.usesLexicalBridge ? 1 : 0,
                hasStructuredSemantics ? 1 : 0,
                semantic.touchesPaste ? 1 : 0,
                hasInteractionScopedPasteSemantics ? 1 : 0,
                semantic.hasPassthroughFlatten ? 1 : 0,
                semantic.hasWrapperSemantics ? 1 : 0,
                semantic.hasPreferredChildSyntax ? 1 : 0,
                semantic.hasRawInvocationPreservation ? 1 : 0,
                semantic.interactionSummary.detail);
          cert.valid = false;
          cert.failure =
              SubtreeSemanticAdmissibilityFailure::PasteWithPassthroughFlatten;
          cert.detail =
              "subtree semantic admissibility failed: paste-bearing subtree "
              "relies on passthrough flatten and therefore does not have a "
              "unique structure-preserving witness";
          return cert;
        }
        if (semantic.touchesPaste && semantic.hasPassthroughFlatten &&
            allowRootPlaceholderFlattenReplay) {
          trace("macro/dag",
                "subtree admissibility accept(root placeholder flatten): "
                "lexicalBridge={0} structuredSemantics={1} paste={2} "
                "interactionPaste={3} passthroughFlatten={4} wrappers={5} "
                "preferredChildSyntax={6} rawInvocation={7} rootReplayInv={8} "
                "summary={9}",
                semantic.usesLexicalBridge ? 1 : 0,
                hasStructuredSemantics ? 1 : 0, semantic.touchesPaste ? 1 : 0,
                hasInteractionScopedPasteSemantics ? 1 : 0,
                semantic.hasPassthroughFlatten ? 1 : 0,
                semantic.hasWrapperSemantics ? 1 : 0,
                semantic.hasPreferredChildSyntax ? 1 : 0,
                semantic.hasRawInvocationPreservation ? 1 : 0,
                semantic.acceptedRootReplayInv
                    ? semantic.acceptedRootReplayInv->id
                    : 0,
                semantic.interactionSummary.detail);
        }

        if (hasBridgeSensitiveStructuredSemantics) {
          trace("macro/dag",
                "subtree admissibility reject(bridge-sensitive semantics): "
                "lexicalBridge={0} structuredSemantics={1} "
                "bridgeSensitiveStructuredSemantics={2} subtreePaste={3} "
                "interactionPaste={4} wrappers={5} preferredChildSyntax={6} "
                "rawInvocation={7} summary={8}",
                semantic.usesLexicalBridge ? 1 : 0,
                hasStructuredSemantics ? 1 : 0,
                hasBridgeSensitiveStructuredSemantics ? 1 : 0,
                semantic.touchesPaste ? 1 : 0,
                hasInteractionScopedPasteSemantics ? 1 : 0,
                semantic.hasWrapperSemantics ? 1 : 0,
                semantic.hasPreferredChildSyntax ? 1 : 0,
                semantic.hasRawInvocationPreservation ? 1 : 0,
                semantic.interactionSummary.detail);
          for (const auto &lift : semantic.liftChains) {
            trace("macro/dag",
                  "  lift chain detail: leaf id={0} name={1} leafArgs={2} "
                  "usedLexicalBridge={3} detail={4}",
                  lift.leaf ? lift.leaf->id : 0,
                  lift.leaf ? lift.leaf->name : StringRef("<none>"),
                  FormatUInt32List(lift.leafArgIdxs),
                  lift.usedLexicalBridge ? 1 : 0, lift.detail);
          }
          for (const auto &step : semantic.structuredLiftCertificates) {
            trace("macro/dag",
                  "  structured step detail: nextInv={0} kind={1} detail={2} "
                  "rewrittenChildSyntax='{3}'",
                  step.nextInv ? step.nextInv->id : 0,
                  static_cast<unsigned>(step.kind), step.detail,
                  step.rewrittenChildSyntax);
          }
          cert.valid = false;
          cert.failure = SubtreeSemanticAdmissibilityFailure::
              LexicalBridgeWithStructuredSemantics;
          cert.detail =
              "subtree semantic admissibility failed: bridge-sensitive "
              "wrapper/raw-invocation/paste subtree semantics remain "
              "inadmissible";
          return cert;
        }

        cert.detail =
            formatv("subtree semantic admissibility: lexicalBridge={0} "
                    "mixed={1} structuredSemantics={2} "
                    "bridgeSensitiveStructuredSemantics={3} "
                    "bridgedFormals={4} bridgedInteractions={5}",
                    semantic.usesLexicalBridge ? 1 : 0,
                    semantic.interactionSummary.hasMixedInteractions ? 1 : 0,
                    hasStructuredSemantics ? 1 : 0,
                    semantic.hasBridgeSensitiveStructuredSemantics ? 1 : 0,
                    semantic.bridgedFormalKeys.size(),
                    semantic.bridgedInteractionKeys.size())
                .str();
        return cert;
      };

      auto buildSubtreeSemanticCertificate =
          [&](const InvocationRewriteCertificate &leafCert,
              ArrayRef<LiftChainCertificate> liftCertificates,
              ArrayRef<RootFormalMergeCertificate> rootMergeCertificates,
              const InvocationRewriteCertificate &rootCert)
          -> SubtreeSemanticCertificate {
        SubtreeSemanticCertificate cert;

        auto makeFormalKey = [&](const RefoldModel::MacroInvocation *inv,
                                 uint32_t argIdx) -> std::string {
          return formatv("{0}#{1}", inv ? inv->id : 0, argIdx).str();
        };

        // Returns true iff the original root formal is exactly one top-level
        // child placeholder and nothing else. This is the structural predicate
        // for preserving the parent while flattening only that child.
        auto rootFormalIsWholeSingleChildPlaceholder =
            [&](const InvocationRewriteCertificate &invCert,
                uint32_t argIdx) -> bool {
          if (!invCert.inv)
            return false;
          auto rawArg = getInvocationArgText(*invCert.inv, argIdx);
          if (!rawArg)
            return false;
          auto placeholders =
              getTopLevelLexicalChildrenInArg(*invCert.inv, argIdx);
          return placeholders.size() == 1 && placeholders.front().child &&
                 placeholders.front().relBegin == 0 &&
                 placeholders.front().relEnd == rawArg->size();
        };

        auto recordSlotCertificate =
            [&](const SlotSemanticRewriteCertificate &slotCert) {
              cert.slotCertificates.push_back(slotCert);
              switch (slotCert.decision.kind) {
              case SlotRewriteDecisionKind::PreferredChildSyntax:
                cert.hasPreferredChildSyntax = true;
                break;
              case SlotRewriteDecisionKind::PreserveRawInvocation:
                cert.hasRawInvocationPreservation = true;
                break;
              case SlotRewriteDecisionKind::PassthroughFlatten:
                cert.hasPassthroughFlatten = true;
                break;
              }

              switch (slotCert.wrapperKind) {
              case WrapperChainKind::Exact:
                break;
              case WrapperChainKind::StringLiteral:
                cert.hasWrapperSemantics = true;
                cert.hasStringifySemantics = true;
                break;
              case WrapperChainKind::WideStringLiteral:
                cert.hasWrapperSemantics = true;
                cert.hasStringifySemantics = true;
                cert.hasWideStringifySemantics = true;
                break;
              }
            };

        auto recordArgCertificate =
            [&](const ArgSemanticRewriteCertificate &argCert) {
              cert.argCertificates.push_back(argCert);
              for (const auto &slotCert : argCert.slotCertificates)
                recordSlotCertificate(slotCert);
            };

        auto recordFormalCertificate =
            [&](const FormalRewriteCertificate &formalCert,
                bool bridgeSensitive) {
              cert.formalCertificates.push_back(formalCert);
              cert.formalInteractionConsistencies.push_back(
                  formalCert.interactionConsistency);
              if (bridgeSensitive)
                cert.bridgedFormalKeys.insert(
                    makeFormalKey(formalCert.inv, formalCert.argIdx));
              if (formalCert.validation.valid ||
                  formalCert.validation.failure !=
                      RawFormalValidationFailure::None)
                cert.rawFormalValidations.push_back(formalCert.validation);
              for (const auto &interactionCert :
                   formalCert.interactionCertificates) {
                cert.interactionCertificates.push_back(interactionCert);
                if (bridgeSensitive) {
                  cert.bridgedInteractionKeys.insert(makeFormalKey(
                      interactionCert.inv, interactionCert.argIdx));
                  const auto &sig = formalCert.interactionConsistency.signature;
                  if (sig.touchesPaste || sig.usesPreferredChildSyntax ||
                      sig.usesRawInvocationPreservation ||
                      sig.usesPassthroughFlatten || sig.usesStringify ||
                      sig.usesWideStringify ||
                      sig.usesRawChildInvocationLogicalInput)
                    cert.hasBridgeSensitiveStructuredSemantics = true;
                }
              }
              for (const auto &argCert : formalCert.argRewriteCertificates)
                recordArgCertificate(argCert);
            };

        auto recordInvocationCertificate =
            [&](const InvocationRewriteCertificate &invCert) {
              cert.invocationCertificates.push_back(invCert);
              for (const auto &validation : invCert.formalValidations)
                cert.rawFormalValidations.push_back(validation);
              if (invCert.pasteValidation.required ||
                  !invCert.pasteValidation.valid) {
                cert.pasteValidations.push_back(invCert.pasteValidation);
                cert.touchesPaste = true;
              }
            };

        recordInvocationCertificate(leafCert);
        for (const auto &liftCert : liftCertificates) {
          cert.liftChains.push_back(liftCert);
          cert.usesLexicalBridge |= liftCert.usedLexicalBridge;
          for (const auto &step : liftCert.steps) {
            cert.structuredLiftCertificates.push_back(step);
            recordInvocationCertificate(step.currentCert);
            for (const auto &derivation : step.derivations)
              cert.parentDerivations.push_back(derivation);
            for (const auto &formalCert : step.parentFormalCertificates)
              recordFormalCertificate(
                  formalCert,
                  step.bridgedNextFormals.contains(formalCert.argIdx));
            if (step.parentCert.kind !=
                InvocationRewriteCertificateKind::Invalid)
              recordInvocationCertificate(step.parentCert);
          }
        }

        for (const auto &mergeCert : rootMergeCertificates)
          cert.rootMergeCertificates.push_back(mergeCert);

        recordInvocationCertificate(rootCert);

        // Track whether the accepted root replay qualifies for the narrow
        // "preserve parent / flatten child" proof class. We only admit root
        // replays that are already uniquely certified deferred paste replays
        // with concrete rewritten syntax, and only when every rewritten formal
        // corresponds to one whole child placeholder in the original root.
        if (rootCert.kind == InvocationRewriteCertificateKind::Unique &&
            rootCert.pasteValidation.required &&
            rootCert.pasteValidation.valid &&
            rootCert.pasteValidation.deferred &&
            !rootCert.rewrittenInvocationSyntax.empty() && rootCert.inv) {
          cert.hasAcceptedRootPlaceholderReplay = true;
          cert.acceptedRootReplayInv = rootCert.inv;
          cert.rootReplayFlattensOnlyWholeChildArgs =
              !rootCert.rewrites.empty();
          for (const auto &rewrite : rootCert.rewrites) {
            if (!rootFormalIsWholeSingleChildPlaceholder(rootCert,
                                                         rewrite.argIdx)) {
              cert.rootReplayFlattensOnlyWholeChildArgs = false;
              break;
            }
          }
        }
        cert.interactionSummary = buildSubtreeInteractionSummaryCertificate(
            cert.interactionCertificates);
        cert.interactionConsistency =
            buildSubtreeInteractionConsistencyCertificate(
                cert.formalCertificates);
        if (!cert.interactionConsistency.valid) {
          cert.valid = false;
          cert.detail = cert.interactionConsistency.detail;
          return cert;
        }
        cert.deferredPasteDischarge =
            buildSubtreeDeferredPasteDischargeCertificate(cert, rootCert);
        if (!cert.deferredPasteDischarge.valid) {
          cert.valid = false;
          cert.detail = cert.deferredPasteDischarge.detail;
          return cert;
        }
        cert.admissibility = buildSubtreeSemanticAdmissibilityCertificate(cert);
        if (!cert.admissibility.valid) {
          cert.valid = false;
          cert.detail = cert.admissibility.detail;
          return cert;
        }
        cert.valid = true;
        cert.detail =
            formatv("subtree semantic bundle: invCerts={0} formalCerts={1} "
                    "argCerts={2} slotCerts={3} interactions={4} "
                    "formalConsistency={5} derivations={6} liftSteps={7} "
                    "rootMerges={8} lexicalBridge={9} paste={10} "
                    "wrappers={11} admissible={12} deferred={13} "
                    "bridgedFormals={14} bridgedInteractions={15} "
                    "bridgeSensitiveStructuredSemantics={16}",
                    cert.invocationCertificates.size(),
                    cert.formalCertificates.size(), cert.argCertificates.size(),
                    cert.slotCertificates.size(),
                    cert.interactionCertificates.size(),
                    cert.formalInteractionConsistencies.size(),
                    cert.parentDerivations.size(),
                    cert.structuredLiftCertificates.size(),
                    cert.rootMergeCertificates.size(),
                    cert.usesLexicalBridge ? 1 : 0, cert.touchesPaste ? 1 : 0,
                    cert.hasWrapperSemantics ? 1 : 0,
                    cert.admissibility.valid ? 1 : 0,
                    cert.deferredPasteDischarge.deferredInvocations.size(),
                    cert.bridgedFormalKeys.size(),
                    cert.bridgedInteractionKeys.size(),
                    cert.hasBridgeSensitiveStructuredSemantics ? 1 : 0)
                .str();
        return cert;
      };

      auto buildLeafFormalLiftGroups =
          [&](const RefoldModel::MacroInvocation &leaf,
              const DenseMap<uint32_t, FormalTextPair> &leafFormals)
          -> SmallVector<DenseMap<uint32_t, FormalTextPair>, 4> {
        SmallVector<DenseMap<uint32_t, FormalTextPair>, 4> groups;
        if (leafFormals.empty())
          return groups;

        SmallVector<uint32_t, 8> argOrder;
        argOrder.reserve(leafFormals.size());
        for (const auto &KV : leafFormals)
          argOrder.push_back(KV.first);
        llvm::sort(argOrder);

        DenseMap<uint32_t, SmallVector<uint32_t, 4>> adjacency;
        for (uint32_t argIdx : argOrder)
          adjacency[argIdx];

        StringMap<SmallVector<uint32_t, 4>> tokenArgs;
        for (const auto &ps : leaf.pasteSpans) {
          auto it = leafFormals.find(ps.argIdx);
          if (it == leafFormals.end())
            continue;

          std::string key = formatv("{0}:{1}", ps.begin, ps.end).str();
          auto &args = tokenArgs[key];
          if (llvm::find(args, ps.argIdx) == args.end())
            args.push_back(ps.argIdx);
        }

        for (const auto &KV : tokenArgs) {
          ArrayRef<uint32_t> args = KV.second;
          if (args.size() < 2)
            continue;
          for (size_t i = 0; i < args.size(); ++i) {
            for (size_t j = i + 1; j < args.size(); ++j) {
              if (llvm::find(adjacency[args[i]], args[j]) ==
                  adjacency[args[i]].end())
                adjacency[args[i]].push_back(args[j]);
              if (llvm::find(adjacency[args[j]], args[i]) ==
                  adjacency[args[j]].end())
                adjacency[args[j]].push_back(args[i]);
            }
          }
        }

        DenseSet<uint32_t> visited;
        for (uint32_t rootArgIdx : argOrder) {
          if (!visited.insert(rootArgIdx).second)
            continue;

          SmallVector<uint32_t, 8> stack{rootArgIdx};
          DenseMap<uint32_t, FormalTextPair> groupFormals;
          while (!stack.empty()) {
            uint32_t argIdx = stack.pop_back_val();
            auto it = leafFormals.find(argIdx);
            if (it != leafFormals.end())
              groupFormals[argIdx] = it->second;

            auto adjIt = adjacency.find(argIdx);
            if (adjIt == adjacency.end())
              continue;
            for (uint32_t nextArgIdx : adjIt->second) {
              if (visited.insert(nextArgIdx).second)
                stack.push_back(nextArgIdx);
            }
          }

          if (!groupFormals.empty())
            groups.push_back(std::move(groupFormals));
        }

        return groups;
      };

      auto buildSubtreeRewriteCertificate =
          [&](const RefoldModel::MacroInvocation &leaf,
              const DenseMap<uint32_t, OldNewText> &leafEdits,
              bool deferLeafPasteValidation) -> SubtreeRewriteCertificate {
        SubtreeRewriteCertificate cert;
        cert.leaf = &leaf;
        cert.root = &m;

        DenseMap<uint32_t, FormalTextPair> pendingLeafFormals;
        for (const auto &KV : leafEdits) {
          StringRef oldText = StringRef(KV.second.oldText).trim();
          StringRef newText = StringRef(KV.second.newText).trim();
          if (oldText == newText)
            continue;
          pendingLeafFormals[KV.first] =
              FormalTextPair{oldText.str(), newText.str()};
        }

        if (pendingLeafFormals.empty()) {
          cert.kind = SubtreeRewriteCertificateKind::NoChange;
          cert.detail = formatv(
                            "DAG subtree: leaf id={0} name={1} pending leaf "
                            "formals empty",
                            leaf.id, leaf.name)
                            .str();
          return cert;
        }

        cert.leafCert = buildWrapperPlaceholderHopInvocationCertificate(
            leaf, pendingLeafFormals, "DAG subtree leaf");
        if (cert.leafCert.pasteValidation.deferred &&
            deferLeafPasteValidation) {
          trace("macro/dag",
                "DAG subtree leaf certificate: wrapper placeholder-hop "
                "paste validation deferred leaf id={0} name={1} "
                "touchedArgs={2}",
                leaf.id, leaf.name, cert.leafCert.rewrites.size());
        }
        if (cert.leafCert.kind == InvocationRewriteCertificateKind::Invalid) {
          cert.detail = cert.leafCert.detail;
          return cert;
        }
        if (cert.leafCert.kind == InvocationRewriteCertificateKind::NoChange) {
          cert.kind = SubtreeRewriteCertificateKind::NoChange;
          cert.detail =
              formatv("DAG subtree: leaf invocation no-change leaf "
                      "id={0} name={1} pendingLeafFormals={2} detail={3}",
                      leaf.id, leaf.name, pendingLeafFormals.size(),
                      cert.leafCert.detail)
                  .str();
          return cert;
        }

        cert.leafFormals.clear();
        for (const auto &rewrite : cert.leafCert.rewrites) {
          cert.leafFormals[rewrite.argIdx] =
              FormalTextPair{rewrite.oldText, rewrite.newText};
        }

        DenseMap<uint32_t, SmallVector<FormalTextPair, 2>> rootRewrites;
        DenseSet<uint32_t> deferredRootOccurrenceArgIdxSet;
        auto liftGroups = buildLeafFormalLiftGroups(leaf, cert.leafFormals);
        for (const auto &groupLeafFormals : liftGroups) {
          auto liftCert = buildLiftChainCertificate(leaf, groupLeafFormals);
          cert.liftCertificates.push_back(liftCert);
          if (liftCert.kind == LiftChainCertificateKind::Invalid) {
            cert.detail = !liftCert.detail.empty()
                              ? liftCert.detail
                              : formatv("DAG subtree: lift failed root id={0} "
                                        "name={1} leaf id={2} name={3} "
                                        "groupArgs={4}",
                                        m.id, m.name, leaf.id, leaf.name,
                                        FormatUInt32List(liftCert.leafArgIdxs))
                                    .str();
            return cert;
          }

          for (const auto &RK : liftCert.rootFormals) {
            auto &rewrites = rootRewrites[RK.first];
            bool seen = false;
            for (const auto &existing : rewrites) {
              if (existing.oldText == RK.second.oldText &&
                  existing.newText == RK.second.newText) {
                seen = true;
                break;
              }
            }
            if (!seen)
              rewrites.push_back(RK.second);
          }
          for (uint32_t rootArgIdx : liftCert.bridgedRootArgIdxs)
            deferredRootOccurrenceArgIdxSet.insert(rootArgIdx);
        }

        if (rootRewrites.empty()) {
          cert.kind = SubtreeRewriteCertificateKind::NoChange;
          cert.detail =
              formatv("DAG subtree: no lifted root formals root id={0} "
                      "name={1} leaf id={2} name={3} leafCertRewrites={4} "
                      "liftCertificates={5}",
                      m.id, m.name, leaf.id, leaf.name,
                      cert.leafCert.rewrites.size(),
                      cert.liftCertificates.size())
                  .str();
          return cert;
        }

        DenseMap<uint32_t, FormalTextPair> pendingRootFormals;
        for (const auto &KV : rootRewrites) {
          const uint32_t argIdx = KV.first;
          auto mergeCert = buildRootFormalMergeCertificate(
              argIdx, KV.second, "DAG subtree root merge");
          cert.rootMergeCertificates.push_back(mergeCert);
          if (mergeCert.kind == RootFormalMergeCertificateKind::Invalid) {
            cert.detail = mergeCert.detail;
            return cert;
          }
          if (mergeCert.kind == RootFormalMergeCertificateKind::NoChange) {
            pendingRootFormals[argIdx] =
                FormalTextPair{mergeCert.baseArgText, mergeCert.mergedArgText};
            trace("macro/dag",
                  "DAG subtree root merge no-change preserved for replay: root "
                  "id={0} name={1} argIdx={2} old='{3}' new='{4}'",
                  m.id, m.name, argIdx, mergeCert.baseArgText,
                  mergeCert.mergedArgText);
            continue;
          }

          pendingRootFormals[argIdx] =
              FormalTextPair{mergeCert.baseArgText, mergeCert.mergedArgText};
        }

        cert.deferRootOccurrenceArgIdxs.clear();
        cert.deferRootOccurrenceArgIdxs.reserve(pendingRootFormals.size());
        for (const auto &KV : pendingRootFormals) {
          if (deferredRootOccurrenceArgIdxSet.contains(KV.first))
            cert.deferRootOccurrenceArgIdxs.push_back(KV.first);
        }
        llvm::sort(cert.deferRootOccurrenceArgIdxs);

        cert.rootCert = buildInvocationRewriteCertificate(
            m, pendingRootFormals, "DAG subtree root", invSpanText,
            invArgRanges, cert.deferRootOccurrenceArgIdxs);
        if (cert.rootCert.kind == InvocationRewriteCertificateKind::Invalid) {
          if (cert.rootCert.failure ==
              InvocationRewriteFailure::PasteMismatch) {
            auto wrapperProbe = buildWrapperPlaceholderHopInvocationCertificate(
                m, pendingRootFormals, "DAG subtree root probe");
            trace("macro/dag",
                  "DAG subtree root probe: plain root cert INVALID due to "
                  "paste mismatch root id={0} name={1} pendingRootFormals={2} "
                  "wrapperProbeKind={3} wrapperPasteRequired={4} "
                  "wrapperPasteValid={5} wrapperPasteDeferred={6} "
                  "wrapperSyntax='{7}' detail={8}",
                  m.id, m.name, pendingRootFormals.size(),
                  static_cast<unsigned>(wrapperProbe.kind),
                  wrapperProbe.pasteValidation.required ? 1 : 0,
                  wrapperProbe.pasteValidation.valid ? 1 : 0,
                  wrapperProbe.pasteValidation.deferred ? 1 : 0,
                  wrapperProbe.rewrittenInvocationSyntax, wrapperProbe.detail);
            if (wrapperProbe.kind == InvocationRewriteCertificateKind::Unique) {
              cert.rootCert = std::move(wrapperProbe);
            }
          }
          if (cert.rootCert.kind == InvocationRewriteCertificateKind::Invalid) {
            cert.detail = cert.rootCert.detail;
            return cert;
          }
        }
        if (cert.rootCert.kind == InvocationRewriteCertificateKind::NoChange) {
          cert.kind = SubtreeRewriteCertificateKind::NoChange;
          cert.detail =
              formatv("DAG subtree: root invocation no-change root "
                      "id={0} name={1} pendingRootFormals={2} detail={3}",
                      m.id, m.name, pendingRootFormals.size(),
                      cert.rootCert.detail)
                  .str();
          return cert;
        }

        cert.rootFormals = pendingRootFormals;

        {
          SmallVector<uint32_t, 8> rootFormalArgIdxs;
          rootFormalArgIdxs.reserve(cert.rootFormals.size());
          for (const auto &KV : cert.rootFormals)
            rootFormalArgIdxs.push_back(KV.first);
          llvm::sort(rootFormalArgIdxs);
          trace("macro/proof",
                "DAG subtree proof ledger: root id={0} name={1} leaf id={2} "
                "name={3} rootFormals={4} deferredRootArgs={5} liftChains={6} "
                "leafPasteRequired={7} leafPasteDeferred={8} "
                "rootPasteRequired={9} "
                "rootPasteDeferred={10}",
                m.id, m.name, leaf.id, leaf.name,
                FormatUInt32List(rootFormalArgIdxs),
                FormatUInt32List(cert.deferRootOccurrenceArgIdxs),
                cert.liftCertificates.size(),
                cert.leafCert.pasteValidation.required ? 1 : 0,
                cert.leafCert.pasteValidation.deferred ? 1 : 0,
                cert.rootCert.pasteValidation.required ? 1 : 0,
                cert.rootCert.pasteValidation.deferred ? 1 : 0);
        }

        trace("macro/dag",
              "DAG subtree root cert summary before semantic: root id={0} "
              "name={1} rootCertInv={2} rootCertName={3} rootKind={4} "
              "rootPasteRequired={5} rootPasteValid={6} rootPasteDeferred={7} "
              "rootRewriteCount={8}",
              m.id, m.name, cert.rootCert.inv ? cert.rootCert.inv->id : 0,
              cert.rootCert.inv ? cert.rootCert.inv->name : StringRef("<none>"),
              static_cast<unsigned>(cert.rootCert.kind),
              cert.rootCert.pasteValidation.required ? 1 : 0,
              cert.rootCert.pasteValidation.valid ? 1 : 0,
              cert.rootCert.pasteValidation.deferred ? 1 : 0,
              cert.rootCert.rewrites.size());
        cert.semantic = buildSubtreeSemanticCertificate(
            cert.leafCert, cert.liftCertificates, cert.rootMergeCertificates,
            cert.rootCert);
        if (!cert.semantic.valid) {
          trace(
              "macro/dag",
              "DAG subtree semantic INVALID: root id={0} name={1} leaf id={2} "
              "name={3} detail={4}",
              m.id, m.name, leaf.id, leaf.name, cert.semantic.detail);
          cert.detail = cert.semantic.detail;
          return cert;
        }
        cert.kind = SubtreeRewriteCertificateKind::Unique;
        return cert;
      };

      struct ArgEdit {
        uint64_t begin;
        uint64_t end;
        std::string repl;
      };

      enum class UniformObservedLeafSeedCertificateKind {
        NoChange,
        Unique,
        Invalid,
      };

      enum class UniformObservedLeafSeedFailure {
        None,
        EmptyConstraints,
        DivergentConstraints,
      };

      struct UniformObservedLeafSeedCertificate {
        UniformObservedLeafSeedCertificateKind kind =
            UniformObservedLeafSeedCertificateKind::Invalid;
        UniformObservedLeafSeedFailure failure =
            UniformObservedLeafSeedFailure::None;
        const RefoldModel::MacroInvocation *inv = nullptr;
        uint32_t argIdx = 0;
        std::string oldText;
        std::string newText;
        std::string detail;
      };

      // Nested leaf invocations are recorded in macro-body space, so their
      // invocation text is often placeholder syntax such as STR1(x) or CAT(a,b)
      // rather than source-spelled actual arguments. When all observed leaf
      // constraints for one formal collapse to the same normalized old/new
      // text, certify that exact uniform observed rewrite as the leaf seed and
      // then continue through the structured lift/root-certificate pipeline.
      // This does not accept a root patch by itself; it only certifies the
      // leaf-side semantic rewrite when direct raw leaf-formal certification is
      // unavailable.
      auto buildUniformObservedLeafSeedCertificate =
          [&](const RefoldModel::MacroInvocation &inv, uint32_t argIdx,
              ArrayRef<ObservedFormalConstraint> constraints,
              StringRef traceStage) -> UniformObservedLeafSeedCertificate {
        UniformObservedLeafSeedCertificate cert;
        cert.inv = &inv;
        cert.argIdx = argIdx;

        if (constraints.empty()) {
          cert.failure = UniformObservedLeafSeedFailure::EmptyConstraints;
          cert.detail =
              formatv("{0}: uniform observed leaf seed unavailable: inv "
                      "id={1} name={2} argIdx={3} has no observed "
                      "constraints",
                      traceStage, inv.id, inv.name, argIdx)
                  .str();
          return cert;
        }

        const StringRef oldTrim = StringRef(constraints[0].oldText).trim();
        const StringRef newTrim = StringRef(constraints[0].newText).trim();
        for (const auto &constraint : constraints) {
          if (StringRef(constraint.oldText).trim() != oldTrim ||
              StringRef(constraint.newText).trim() != newTrim) {
            cert.failure = UniformObservedLeafSeedFailure::DivergentConstraints;
            cert.detail =
                formatv("{0}: uniform observed leaf seed unavailable: "
                        "inv id={1} name={2} argIdx={3} observed "
                        "constraints diverged",
                        traceStage, inv.id, inv.name, argIdx)
                    .str();
            return cert;
          }
        }

        cert.oldText = oldTrim.str();
        cert.newText = newTrim.str();
        if (oldTrim == newTrim) {
          cert.kind = UniformObservedLeafSeedCertificateKind::NoChange;
          cert.detail = formatv("{0}: uniform observed leaf seed collapsed to "
                                "no-change inv id={1} name={2} argIdx={3}",
                                traceStage, inv.id, inv.name, argIdx)
                            .str();
          return cert;
        }

        cert.kind = UniformObservedLeafSeedCertificateKind::Unique;
        cert.detail =
            formatv("{0}: uniform observed leaf seed certified inv id={1} "
                    "name={2} argIdx={3} old='{4}' new='{5}'",
                    traceStage, inv.id, inv.name, argIdx, cert.oldText,
                    cert.newText)
                .str();
        return cert;
      };

      enum class RootPatchConstructionCertificateKind {
        NoChange,
        Unique,
        Invalid,
      };

      enum class RootPatchConstructionFailure {
        None,
        ArgIndexOutOfBounds,
        InvalidArgRange,
        OverlappingEdits,
      };

      struct RootPatchConstructionCertificate {
        RootPatchConstructionCertificateKind kind =
            RootPatchConstructionCertificateKind::Invalid;
        RootPatchConstructionFailure failure =
            RootPatchConstructionFailure::None;
        SmallVector<ArgEdit, 8> edits;
        std::optional<MacroPatch> patch;
        std::string detail;
      };

      enum class DagCandidateAcceptanceFailure {
        None,
        DifferentSpan,
        DifferentBaseText,
        MergeConflict,
        MergedRootValidationFailed,
      };

      struct DagCandidateAcceptanceCertificate {
        bool accepted = false;
        bool merged = false;
        DagCandidateAcceptanceFailure failure =
            DagCandidateAcceptanceFailure::None;
        std::string detail;
      };

      struct DagCandidateValidationMetadata {
        SmallVector<uint32_t, 8> deferOccurrenceArgIdxs;
        DenseMap<uint32_t, FormalTextPair> expectedRootFormals;
        StringMap<SemanticInteractionSignature> bridgeSensitiveFormalSignatures;
        bool hasExpectedRootFormals = false;
        bool hasBridgeSensitiveStructuredSemantics = false;
        bool hasMixedSemanticInteractions = false;
      };

      auto formatBridgeSensitiveFormalSignatureMap =
          [&](const StringMap<SemanticInteractionSignature> &sigs) {
            SmallVector<StringRef, 8> keys;
            keys.reserve(sigs.size());
            for (const auto &KV : sigs)
              keys.push_back(KV.getKey());
            llvm::sort(keys);

            std::string out;
            raw_string_ostream os(out);
            os << "{";
            for (size_t i = 0; i < keys.size(); ++i) {
              if (i)
                os << ", ";
              StringRef key = keys[i];
              const auto it = sigs.find(key);
              os << key << ":(paste="
                 << (it->second.touchesPaste ? 1 : 0)
                 << ", stringify="
                 << (it->second.usesStringify ? 1 : 0)
                 << ", wide="
                 << (it->second.usesWideStringify ? 1 : 0)
                 << ", wrapper="
                 << (it->second.usesPassthroughFlatten ? 1 : 0)
                 << ", childSyntax="
                 << (it->second.usesPreferredChildSyntax ? 1 : 0)
                 << ", raw="
                 << (it->second.usesRawInvocationPreservation ? 1 : 0)
                 << ")";
            }
            os << "}";
            return os.str();
          };

      // --- Phase 4: Try leaves, lift, validate, and ensure uniqueness --------
      //
      // We scan leaf candidates (deepest-first) and attempt to produce a root
      // invocation patch. We accept only if:
      //   * all lifted edits validate against B (occurrence matching), and
      //   * the resulting root patch is unique (no second distinct patch).
      std::optional<MacroPatch> uniquePatch;
      std::optional<std::string> uniquePatchBaseText;
      DagCandidateValidationMetadata uniquePatchValidation;
      unsigned leavesExamined = 0;
      unsigned distinctRootPatches = 0;

      auto buildRootPatchConstructionCertificate =
          [&](const InvocationRewriteCertificate &rootCert,
              StringRef traceStage)
          -> RootPatchConstructionCertificate {
        RootPatchConstructionCertificate cert;
        if (rootCert.kind == InvocationRewriteCertificateKind::NoChange ||
            rootCert.rewrites.empty()) {
          cert.kind = RootPatchConstructionCertificateKind::NoChange;
          cert.detail = formatv(
                            "{0}: root patch construction no-op root id={1} "
                            "name={2}",
                            traceStage, m.id, m.name)
                            .str();
          return cert;
        }

        cert.edits.reserve(rootCert.rewrites.size());
        for (const auto &rewrite : rootCert.rewrites) {
          const uint32_t argIdx = rewrite.argIdx;
          if (argIdx >= invArgRanges.size()) {
            cert.failure = RootPatchConstructionFailure::ArgIndexOutOfBounds;
            cert.detail = formatv(
                              "{0}: root patch construction failed root id={1} "
                              "name={2} argIdx={3} out of bounds argCount={4}",
                              traceStage, m.id, m.name, argIdx,
                              invArgRanges.size())
                              .str();
            return cert;
          }

          const uint64_t begin = (uint64_t)invArgRanges[argIdx].first;
          const uint64_t end = (uint64_t)invArgRanges[argIdx].second;
          if (begin > end || end > (uint64_t)invSpanText.size()) {
            cert.failure = RootPatchConstructionFailure::InvalidArgRange;
            cert.detail = formatv(
                              "{0}: root patch construction failed root id={1} "
                              "name={2} argIdx={3} invalid range=[{4},{5}) "
                              "spanLen={6}",
                              traceStage, m.id, m.name, argIdx, begin, end,
                              invSpanText.size())
                              .str();
            return cert;
          }

          cert.edits.push_back(ArgEdit{begin, end, rewrite.newText});
        }

        llvm::sort(cert.edits, [](const ArgEdit &a, const ArgEdit &b) {
          return a.begin < b.begin;
        });

        uint64_t cur = 0;
        for (const auto &e : cert.edits) {
          if (e.begin < cur || e.end < e.begin) {
            cert.failure = RootPatchConstructionFailure::OverlappingEdits;
            cert.detail = formatv(
                              "{0}: root patch construction failed root id={1} "
                              "name={2} overlapping edits range=[{3},{4}) "
                              "prevEnd={5}",
                              traceStage, m.id, m.name, e.begin, e.end, cur)
                              .str();
            return cert;
          }
          cur = e.end;
        }

        std::string replText;
        replText.reserve(invSpanText.size());
        cur = 0;
        for (const auto &e : cert.edits) {
          auto mid = invSpanText.slice((size_t)cur, (size_t)e.begin);
          replText.append(mid.begin(), mid.end());
          replText.append(e.repl);
          cur = e.end;
        }
        auto tail = invSpanText.drop_front((size_t)cur);
        replText.append(tail.begin(), tail.end());

        MacroPatch patch{*invStart, *invEnd, std::move(replText), 0};
        cert.patch = std::move(patch);
        cert.kind = RootPatchConstructionCertificateKind::Unique;
        cert.detail = formatv(
                          "{0}: root patch construction succeeded root id={1} "
                          "name={2} edits={3} replLen={4}",
                          traceStage, m.id, m.name, cert.edits.size(),
                          cert.patch ? cert.patch->replacement.size() : 0)
                          .str();
        return cert;
      };

      auto mergeDagCandidateValidationMetadata =
          [&](const DagCandidateValidationMetadata &lhs,
              const DagCandidateValidationMetadata &rhs)
          -> std::optional<DagCandidateValidationMetadata> {
        DagCandidateValidationMetadata merged;
        merged.hasBridgeSensitiveStructuredSemantics =
            lhs.hasBridgeSensitiveStructuredSemantics ||
            rhs.hasBridgeSensitiveStructuredSemantics;
        merged.hasMixedSemanticInteractions =
            lhs.hasMixedSemanticInteractions ||
            rhs.hasMixedSemanticInteractions;

        for (const auto &KV : lhs.bridgeSensitiveFormalSignatures)
          merged.bridgeSensitiveFormalSignatures[KV.getKey()] = KV.getValue();
        for (const auto &KV : rhs.bridgeSensitiveFormalSignatures) {
          auto it = merged.bridgeSensitiveFormalSignatures.find(KV.getKey());
          if (it == merged.bridgeSensitiveFormalSignatures.end()) {
            merged.bridgeSensitiveFormalSignatures[KV.getKey()] = KV.getValue();
            continue;
          }
          if (!(it->second == KV.getValue()))
            return std::nullopt;
        }

        auto addDeferredArgIdxs = [&](ArrayRef<uint32_t> argIdxs) {
          for (uint32_t argIdx : argIdxs) {
            if (!llvm::is_contained(merged.deferOccurrenceArgIdxs, argIdx))
              merged.deferOccurrenceArgIdxs.push_back(argIdx);
          }
        };
        addDeferredArgIdxs(lhs.deferOccurrenceArgIdxs);
        addDeferredArgIdxs(rhs.deferOccurrenceArgIdxs);
        llvm::sort(merged.deferOccurrenceArgIdxs);

        if (!lhs.hasExpectedRootFormals && !rhs.hasExpectedRootFormals)
          return merged;

        DenseMap<uint32_t, SmallVector<FormalTextPair, 2>> rewritesByArg;
        auto collect = [&](const DagCandidateValidationMetadata &meta) {
          if (!meta.hasExpectedRootFormals)
            return;
          for (const auto &KV : meta.expectedRootFormals)
            rewritesByArg[KV.first].push_back(KV.second);
        };
        collect(lhs);
        collect(rhs);

        for (const auto &KV : rewritesByArg) {
          const uint32_t argIdx = KV.first;
          if (argIdx >= invArgRanges.size())
            return std::nullopt;

          const size_t begin = invArgRanges[argIdx].first;
          const size_t end = invArgRanges[argIdx].second;
          if (begin > end || end > invSpanText.size())
            return std::nullopt;

          const StringRef baseArgText = invSpanText.slice(begin, end).trim();
          auto mergedArgText =
              mergeCompatibleFormalRewrites(baseArgText, KV.second);
          if (!mergedArgText)
            return std::nullopt;
          if (StringRef(*mergedArgText).trim() == baseArgText) {
            trace("macro/proof",
                  "merge DAG validation metadata root id={0} name={1} "
                  "argIdx={2} collapsed to base text base='{3}' variants={4}",
                  m.id, m.name, argIdx,
                  stringutils::showWSWithClip(baseArgText, 120),
                  KV.second.size());
            continue;
          }

          merged.expectedRootFormals[argIdx] =
              FormalTextPair{baseArgText.str(),
                             StringRef(*mergedArgText).trim().str()};
        }

        merged.hasExpectedRootFormals = true;
        return merged;
      };

      struct InvocationHeadShape {
        std::string callee;
        size_t argCount = 0;
      };

      auto getInvocationHeadShape = [&](StringRef text)
          -> std::optional<InvocationHeadShape> {
        StringRef trimmed = text.trim();
        auto argRangesOpt = ParseMacroInvocationArgContentRanges(trimmed);
        if (!argRangesOpt)
          return std::nullopt;

        size_t open = trimmed.find('(');
        if (open == StringRef::npos)
          return std::nullopt;

        StringRef callee = trimmed.take_front(open).trim();
        if (callee.empty())
          return std::nullopt;

        return InvocationHeadShape{callee.str(), argRangesOpt->size()};
      };

      auto preservesRootInvocationHead =
          [&](const FormalTextPair &rewrite) -> bool {
        auto oldShape = getInvocationHeadShape(rewrite.oldText);
        auto newShape = getInvocationHeadShape(rewrite.newText);
        if (!oldShape || !newShape)
          return false;
        return oldShape->callee == newShape->callee &&
               oldShape->argCount == newShape->argCount;
      };

      std::function<unsigned(StringRef, StringRef)>
          countPreservedInvocationHeads =
              [&](StringRef oldText, StringRef newText) -> unsigned {
        auto oldShape = getInvocationHeadShape(oldText);
        auto newShape = getInvocationHeadShape(newText);
        if (!oldShape || !newShape)
          return 0;
        if (oldShape->callee != newShape->callee ||
            oldShape->argCount != newShape->argCount)
          return 0;

        auto oldArgRangesOpt =
            ParseMacroInvocationArgContentRanges(oldText.trim());
        auto newArgRangesOpt =
            ParseMacroInvocationArgContentRanges(newText.trim());
        if (!oldArgRangesOpt || !newArgRangesOpt ||
            oldArgRangesOpt->size() != newArgRangesOpt->size())
          return 0;

        unsigned score = 1;
        for (size_t i = 0; i < oldArgRangesOpt->size(); ++i) {
          const auto &oldArgRange = (*oldArgRangesOpt)[i];
          const auto &newArgRange = (*newArgRangesOpt)[i];
          score += countPreservedInvocationHeads(
              oldText.trim().slice((size_t)oldArgRange.first,
                                   (size_t)oldArgRange.second),
              newText.trim().slice((size_t)newArgRange.first,
                                   (size_t)newArgRange.second));
        }
        return score;
      };

      auto choosePreferredStructuredDagCandidate =
          [&](const DagCandidateValidationMetadata &existingValidation,
              const DagCandidateValidationMetadata &candidateValidation)
          -> int {
        if (!existingValidation.hasExpectedRootFormals ||
            !candidateValidation.hasExpectedRootFormals)
          return 0;

        if (existingValidation.expectedRootFormals.size() !=
            candidateValidation.expectedRootFormals.size())
          return 0;

        bool existingPreferred = false;
        bool candidatePreferred = false;

        for (const auto &KV : existingValidation.expectedRootFormals) {
          auto it = candidateValidation.expectedRootFormals.find(KV.first);
          if (it == candidateValidation.expectedRootFormals.end())
            return 0;

          const FormalTextPair &existingRewrite = KV.second;
          const FormalTextPair &candidateRewrite = it->second;
          if (StringRef(existingRewrite.oldText).trim() !=
              StringRef(candidateRewrite.oldText).trim())
            return 0;

          const bool existingPreserves =
              preservesRootInvocationHead(existingRewrite);
          const bool candidatePreserves =
              preservesRootInvocationHead(candidateRewrite);
          const unsigned existingStructureScore = countPreservedInvocationHeads(
              existingRewrite.oldText, existingRewrite.newText);
          const unsigned candidateStructureScore =
              countPreservedInvocationHeads(candidateRewrite.oldText,
                                            candidateRewrite.newText);

          if (existingStructureScore != candidateStructureScore) {
            if (existingStructureScore > candidateStructureScore)
              existingPreferred = true;
            if (candidateStructureScore > existingStructureScore)
              candidatePreferred = true;
            continue;
          }

          if (existingPreserves == candidatePreserves) {
            if (StringRef(existingRewrite.newText).trim() !=
                StringRef(candidateRewrite.newText).trim())
              return 0;
            continue;
          }

          if (existingPreserves)
            existingPreferred = true;
          if (candidatePreserves)
            candidatePreferred = true;
        }

        if (existingPreferred == candidatePreferred)
          return 0;
        return existingPreferred ? -1 : 1;
      };

      struct RootProofValidationCertificate {
        bool valid = false;
        DenseMap<uint32_t, FormalTextPair> replayRootFormals;
        InvocationRewriteCertificate replayInvocationCertificate;
        std::string detail;
      };

      auto buildRootProofValidationCertificate =
          [&](StringRef baseText, StringRef newText,
              ArrayRef<uint32_t> deferOccurrenceArgIdxs,
              const DenseMap<uint32_t, FormalTextPair> *expectedRootFormals,
              StringRef traceStage) -> RootProofValidationCertificate {
        RootProofValidationCertificate cert;

        if (baseText == newText) {
          cert.valid = true;
          cert.detail = formatv(
                            "{0}: root proof validation no-op root id={1} "
                            "name='{2}'",
                            traceStage, m.id, m.name)
                            .str();
          return cert;
        }

        auto replayRootFormals =
            buildRootFormalRewriteMapFromCallsiteReplacement(baseText, newText);
        if (!replayRootFormals) {
          cert.detail = formatv(
                            "{0}: root proof validation failed root id={1} "
                            "name='{2}' could not derive replay root-formal "
                            "rewrite map baseLen={3} newLen={4}",
                            traceStage, m.id, m.name, baseText.size(),
                            newText.size())
                            .str();
          return cert;
        }

        if (expectedRootFormals) {
          auto concreteArgMatchesExpectedUnchanged =
              [&](uint32_t argIdx, const FormalTextPair &expected) -> bool {
            if (argIdx >= invArgRanges.size())
              return false;

            auto newRangesOpt =
                GetMacroInvocationFormalArgContentRanges(m, newText);
            if (!newRangesOpt || argIdx >= newRangesOpt->size())
              return false;

            const auto &oldR = invArgRanges[argIdx];
            const auto &newR = (*newRangesOpt)[argIdx];
            if (oldR.first > oldR.second || oldR.second > baseText.size() ||
                newR.first > newR.second || newR.second > newText.size())
              return false;

            StringRef concreteOld =
                baseText.slice((size_t)oldR.first, (size_t)oldR.second).trim();
            StringRef concreteNew =
                newText.slice((size_t)newR.first, (size_t)newR.second).trim();
            StringRef expectedOld = StringRef(expected.oldText).trim();
            StringRef expectedNew = StringRef(expected.newText).trim();
            return concreteOld == concreteNew && concreteOld == expectedOld &&
                   concreteNew == expectedNew;
          };

          SmallVector<uint32_t, 8> replayAugmentedSupportOnlyArgIdxs;
          for (const auto &KV : *expectedRootFormals) {
            if (replayRootFormals->contains(KV.first))
              continue;
            if (StringRef(KV.second.oldText).trim() !=
                StringRef(KV.second.newText).trim())
              continue;
            if (!concreteArgMatchesExpectedUnchanged(KV.first, KV.second))
              continue;
            (*replayRootFormals)[KV.first] = KV.second;
            replayAugmentedSupportOnlyArgIdxs.push_back(KV.first);
          }
          llvm::sort(replayAugmentedSupportOnlyArgIdxs);

          trace("macro/proof",
                "{0}: root proof replay-vs-expected root id={1} name={2} "
                "replay={3} expected={4} augmentedSupportOnly={5}",
                traceStage, m.id, m.name,
                formatFormalTextPairMap(*replayRootFormals),
                formatFormalTextPairMap(*expectedRootFormals),
                FormatUInt32List(replayAugmentedSupportOnlyArgIdxs));

          auto newRangesOpt =
              GetMacroInvocationFormalArgContentRanges(m, newText);
          SmallVector<uint32_t, 8> missingExpectedArgIdxs;
          SmallVector<uint32_t, 8> unchangedConcreteMissingArgIdxs;
          SmallVector<uint32_t, 8> supportOnlyMissingArgIdxs;
          SmallVector<uint32_t, 8> mismatchedExpectedArgIdxs;
          SmallVector<uint32_t, 8> unexpectedReplayArgIdxs;

          for (const auto &KV : *expectedRootFormals) {
            auto it = replayRootFormals->find(KV.first);
            if (it == replayRootFormals->end()) {
              missingExpectedArgIdxs.push_back(KV.first);
              if (KV.second.oldText == KV.second.newText)
                supportOnlyMissingArgIdxs.push_back(KV.first);

              if (newRangesOpt && KV.first < invArgRanges.size() &&
                  KV.first < newRangesOpt->size()) {
                const auto &oldR = invArgRanges[KV.first];
                const auto &newR = (*newRangesOpt)[KV.first];
                if (oldR.first <= oldR.second &&
                    oldR.second <= baseText.size() &&
                    newR.first <= newR.second &&
                    newR.second <= newText.size()) {
                  StringRef concreteOld =
                      baseText.slice((size_t)oldR.first, (size_t)oldR.second)
                          .trim();
                  StringRef concreteNew =
                      newText.slice((size_t)newR.first, (size_t)newR.second)
                          .trim();
                  if (concreteOld == concreteNew &&
                      concreteOld == StringRef(KV.second.oldText).trim() &&
                      concreteNew == StringRef(KV.second.newText).trim()) {
                    unchangedConcreteMissingArgIdxs.push_back(KV.first);
                  }
                  trace(
                      "macro/proof",
                      "{0}: root proof missing expected arg root id={1} "
                      "name={2} argIdx={3} concreteOld='{4}' concreteNew='{5}' "
                      "expectedOld='{6}' expectedNew='{7}'",
                      traceStage, m.id, m.name, KV.first,
                      stringutils::showWSWithClip(concreteOld, 120),
                      stringutils::showWSWithClip(concreteNew, 120),
                      stringutils::showWSWithClip(KV.second.oldText, 120),
                      stringutils::showWSWithClip(KV.second.newText, 120));
                }
              }
              continue;
            }

            if (it->second.oldText != KV.second.oldText ||
                it->second.newText != KV.second.newText) {
              mismatchedExpectedArgIdxs.push_back(KV.first);
              trace("macro/proof",
                    "{0}: root proof mismatched expected arg root id={1} "
                    "name={2} argIdx={3} derivedOld='{4}' derivedNew='{5}' "
                    "expectedOld='{6}' expectedNew='{7}'",
                    traceStage, m.id, m.name, KV.first,
                    stringutils::showWSWithClip(it->second.oldText, 120),
                    stringutils::showWSWithClip(it->second.newText, 120),
                    stringutils::showWSWithClip(KV.second.oldText, 120),
                    stringutils::showWSWithClip(KV.second.newText, 120));
            }
          }

          for (const auto &KV : *replayRootFormals) {
            if (!expectedRootFormals->contains(KV.first))
              unexpectedReplayArgIdxs.push_back(KV.first);
          }

          llvm::sort(missingExpectedArgIdxs);
          llvm::sort(unchangedConcreteMissingArgIdxs);
          llvm::sort(supportOnlyMissingArgIdxs);
          llvm::sort(mismatchedExpectedArgIdxs);
          llvm::sort(unexpectedReplayArgIdxs);
          trace("macro/proof",
                "{0}: root proof mismatch analysis root id={1} name={2} "
                "missingExpectedArgs={3} unchangedConcreteMissingArgs={4} "
                "supportOnlyMissingArgs={5} mismatchedExpectedArgs={6} "
                "unexpectedReplayArgs={7}",
                traceStage, m.id, m.name,
                FormatUInt32List(missingExpectedArgIdxs),
                FormatUInt32List(unchangedConcreteMissingArgIdxs),
                FormatUInt32List(supportOnlyMissingArgIdxs),
                FormatUInt32List(mismatchedExpectedArgIdxs),
                FormatUInt32List(unexpectedReplayArgIdxs));

          if (replayRootFormals->size() != expectedRootFormals->size()) {
            cert.detail = formatv(
                              "{0}: root proof validation failed root id={1} "
                              "name='{2}' replay-derived root formal count "
                              "mismatch derived={3} expected={4}",
                              traceStage, m.id, m.name,
                              replayRootFormals->size(),
                              expectedRootFormals->size())
                              .str();
            return cert;
          }

          for (const auto &KV : *expectedRootFormals) {
            auto it = replayRootFormals->find(KV.first);
            if (it == replayRootFormals->end() ||
                it->second.oldText != KV.second.oldText ||
                it->second.newText != KV.second.newText) {
              cert.detail = formatv(
                                "{0}: root proof validation failed root "
                                "id={1} name='{2}' replay-derived root "
                                "formal mismatch argIdx={3} derivedOld='{4}' "
                                "derivedNew='{5}' expectedOld='{6}' "
                                "expectedNew='{7}'",
                                traceStage, m.id, m.name, KV.first,
                                it == replayRootFormals->end()
                                    ? StringRef("")
                                    : StringRef(it->second.oldText),
                                it == replayRootFormals->end()
                                    ? StringRef("")
                                    : StringRef(it->second.newText),
                                KV.second.oldText, KV.second.newText)
                                .str();
              return cert;
            }
          }
        }

        cert.replayInvocationCertificate = buildInvocationRewriteCertificate(
            m, *replayRootFormals, traceStage, baseText, invArgRanges,
            deferOccurrenceArgIdxs);
        if (cert.replayInvocationCertificate.kind ==
            InvocationRewriteCertificateKind::Invalid) {
          if (cert.replayInvocationCertificate.failure ==
              InvocationRewriteFailure::PasteMismatch) {
            auto wrapperReplayCert =
                buildWrapperPlaceholderHopInvocationCertificate(
                    m, *replayRootFormals, traceStage, baseText, invArgRanges,
                    deferOccurrenceArgIdxs);
            if (wrapperReplayCert.kind ==
                    InvocationRewriteCertificateKind::Unique &&
                !wrapperReplayCert.rewrittenInvocationSyntax.empty() &&
                StringRef(wrapperReplayCert.rewrittenInvocationSyntax).trim() ==
                    newText.trim()) {
              trace("macro/proof",
                    "{0}: root proof validation accepted wrapper replay "
                    "candidate root id={1} name={2} syntax='{3}' "
                    "pasteDeferred={4}",
                    traceStage, m.id, m.name,
                    wrapperReplayCert.rewrittenInvocationSyntax,
                    wrapperReplayCert.pasteValidation.deferred ? 1 : 0);
              cert.replayInvocationCertificate = std::move(wrapperReplayCert);
            }
          }
        }
        if (cert.replayInvocationCertificate.kind ==
            InvocationRewriteCertificateKind::Invalid) {
          cert.detail = cert.replayInvocationCertificate.detail;
          return cert;
        }

        cert.replayRootFormals = std::move(*replayRootFormals);
        cert.valid = true;
        cert.detail = formatv(
                          "{0}: root proof validation succeeded root id={1} "
                          "name='{2}' replayFormals={3} deferredArgs={4}",
                          traceStage, m.id, m.name,
                          cert.replayRootFormals.size(),
                          deferOccurrenceArgIdxs.size())
                          .str();
        return cert;
      };

      auto validateDagCandidateProof =
          [&](const DagCandidateValidationMetadata &validation,
              StringRef baseText, StringRef newText,
              StringRef traceStage) -> bool {
        SmallVector<uint32_t, 8> expectedRootArgIdxs;
        expectedRootArgIdxs.reserve(validation.expectedRootFormals.size());
        for (const auto &KV : validation.expectedRootFormals)
          expectedRootArgIdxs.push_back(KV.first);
        llvm::sort(expectedRootArgIdxs);
        SmallVector<uint32_t, 8> deferredArgs =
            validation.deferOccurrenceArgIdxs;
        llvm::sort(deferredArgs);
        trace("macro/proof",
              "{0}: DAG candidate proof ledger enter root id={1} name={2} "
              "expectedRootArgs={3} deferredArgs={4} bridgeSensitive={5} "
              "mixed={6}",
              traceStage, m.id, m.name, FormatUInt32List(expectedRootArgIdxs),
              FormatUInt32List(deferredArgs),
              validation.hasBridgeSensitiveStructuredSemantics ? 1 : 0,
              validation.hasMixedSemanticInteractions ? 1 : 0);
        if (validation.hasMixedSemanticInteractions) {
          trace("macro/dag",
                "{0}: DAG candidate patch rejected root id={1} name={2} "
                "merged semantic metadata contains mixed interactions",
                traceStage, m.id, m.name);
          return false;
        }
        if (validation.hasBridgeSensitiveStructuredSemantics) {
          trace("macro/dag",
                "{0}: DAG candidate patch rejected root id={1} name={2} "
                "merged semantic metadata contains bridge-sensitive "
                "structured semantics formals={3}",
                traceStage, m.id, m.name,
                validation.bridgeSensitiveFormalSignatures.size());
          return false;
        }

        auto proofCert = buildRootProofValidationCertificate(
            baseText, newText, validation.deferOccurrenceArgIdxs,
            validation.hasExpectedRootFormals ? &validation.expectedRootFormals
                                              : nullptr,
            traceStage);
        if (!proofCert.valid) {
          if (!proofCert.detail.empty())
            trace("macro/dag", "{0}", proofCert.detail);
          return false;
        }
        if (!proofCert.detail.empty())
          trace("macro/dag", "{0}", proofCert.detail);
        SmallVector<uint32_t, 8> replayRootArgIdxs;
        replayRootArgIdxs.reserve(proofCert.replayRootFormals.size());
        for (const auto &KV : proofCert.replayRootFormals)
          replayRootArgIdxs.push_back(KV.first);
        llvm::sort(replayRootArgIdxs);
        trace(
            "macro/proof",
            "{0}: DAG candidate proof ledger replay root id={1} name={2} "
            "replayRootArgs={3} replayPasteRequired={4} replayPasteValid={5} "
            "replayPasteDeferred={6}",
            traceStage, m.id, m.name, FormatUInt32List(replayRootArgIdxs),
            proofCert.replayInvocationCertificate.pasteValidation.required ? 1
                                                                           : 0,
            proofCert.replayInvocationCertificate.pasteValidation.valid ? 1 : 0,
            proofCert.replayInvocationCertificate.pasteValidation.deferred ? 1
                                                                           : 0);
        return true;
      };

      auto buildDagCandidateValidationMetadataFromSubtree =
          [&](const SubtreeRewriteCertificate &subtreeCert)
          -> DagCandidateValidationMetadata {
        DagCandidateValidationMetadata validation;
        validation.hasExpectedRootFormals = true;
        validation.hasBridgeSensitiveStructuredSemantics =
            subtreeCert.semantic.hasBridgeSensitiveStructuredSemantics;
        validation.hasMixedSemanticInteractions =
            subtreeCert.semantic.interactionSummary.hasMixedInteractions;
        for (const auto &formalConsistency :
             subtreeCert.semantic.formalInteractionConsistencies) {
          std::string formalKey =
              formatv("{0}#{1}",
                      formalConsistency.inv ? formalConsistency.inv->id : 0,
                      formalConsistency.argIdx)
                  .str();
          if (subtreeCert.semantic.bridgedFormalKeys.contains(formalKey))
            validation.bridgeSensitiveFormalSignatures[formalKey] =
                formalConsistency.signature;
        }
        for (const auto &KV : subtreeCert.rootFormals)
          validation.expectedRootFormals[KV.first] = KV.second;
        validation.deferOccurrenceArgIdxs.assign(
            subtreeCert.deferRootOccurrenceArgIdxs.begin(),
            subtreeCert.deferRootOccurrenceArgIdxs.end());
        return validation;
      };

      auto acceptOrMergeDAGCandidatePatch =
          [&](MacroPatch candPatch, StringRef baseText, StringRef traceStage,
              const DagCandidateValidationMetadata *candValidation =
                  nullptr) -> DagCandidateAcceptanceCertificate {
        DagCandidateAcceptanceCertificate cert;
        DagCandidateValidationMetadata candidateValidation;
        if (candValidation)
          candidateValidation = *candValidation;

        if (!uniquePatch) {
          if (!validateDagCandidateProof(candidateValidation, baseText,
                                         candPatch.replacement, traceStage)) {
            cert.failure =
                DagCandidateAcceptanceFailure::MergedRootValidationFailed;
            cert.detail = formatv(
                              "{0}: DAG candidate patch rejected root id={1} "
                              "name={2} semantic validation metadata failed",
                              traceStage, m.id, m.name)
                              .str();
            return cert;
          }
          if (!candPatch.macroId)
            candPatch.macroId = m.id;
          if (!candPatch.proofRootMacroId) {
            candPatch.proofRootMacroId = m.id;
            SyncMacroPatchProofSummary(candPatch);
          }
          trace("macro/proof",
                "DAG candidate accepted as unique root patch: root id={0} "
                "name={1} stage={2} {3}",
                m.id, m.name, traceStage, FormatMacroPatchAudit(candPatch));
          uniquePatch = std::move(candPatch);
          uniquePatchBaseText = baseText.str();
          uniquePatchValidation = std::move(candidateValidation);
          distinctRootPatches = 1;
          cert.accepted = true;
          cert.detail = formatv(
                            "{0}: accepted first DAG candidate root patch "
                            "root id={1} name={2} span=[{3},{4}) replLen={5}",
                            traceStage, m.id, m.name, uniquePatch->invStart,
                            uniquePatch->invEnd,
                            uniquePatch->replacement.size())
                            .str();
          return cert;
        }

        if (uniquePatch->invStart != candPatch.invStart ||
            uniquePatch->invEnd != candPatch.invEnd) {
          cert.failure = DagCandidateAcceptanceFailure::DifferentSpan;
          cert.detail = formatv(
                            "{0}: DAG candidate patch rejected root id={1} "
                            "name={2} span mismatch existing=[{3},{4}) "
                            "candidate=[{5},{6})",
                            traceStage, m.id, m.name, uniquePatch->invStart,
                            uniquePatch->invEnd, candPatch.invStart,
                            candPatch.invEnd)
                            .str();
          return cert;
        }

        if (uniquePatch->replacement == candPatch.replacement) {
          trace("macro/proof",
                "DAG equivalent root patch audit: root id={0} name={1} "
                "stage={2} existing[{3}] candidate[{4}]",
                m.id, m.name, traceStage, FormatMacroPatchAudit(*uniquePatch),
                FormatMacroPatchAudit(candPatch));
          auto mergedValidation = mergeDagCandidateValidationMetadata(
              uniquePatchValidation, candidateValidation);
          if (uniquePatch->subtreeCertBacked || candPatch.subtreeCertBacked) {
            trace("macro/proof",
                  "DAG equivalent subtree-plan probe: root id={0} name={1} "
                  "stage={2} existingExpRoot={3} candidateExpRoot={4} "
                  "mergedExpRoot(pending) currentDeferredArgs={5} "
                  "candidateDeferredArgs={6}",
                  m.id, m.name, traceStage,
                  stringutils::showWSWithClip(
                      uniquePatch->subtreeExpectedRootFormalSummary, 160),
                  stringutils::showWSWithClip(
                      candPatch.subtreeExpectedRootFormalSummary, 160),
                  stringutils::showWSWithClip(
                      uniquePatch->subtreeDeferredRootArgSummary, 160),
                  stringutils::showWSWithClip(
                      candPatch.subtreeDeferredRootArgSummary, 160));
          }
          if (!mergedValidation) {
            cert.failure =
                DagCandidateAcceptanceFailure::MergedRootValidationFailed;
            cert.detail =
                formatv("{0}: DAG candidate patch rejected root id={1} "
                        "name={2} equivalent replacement produced "
                        "incompatible root-formal validation metadata",
                        traceStage, m.id, m.name)
                    .str();
            return cert;
          }

          if (!validateDagCandidateProof(
                  *mergedValidation, *uniquePatchBaseText,
                  uniquePatch->replacement, traceStage)) {
            cert.failure =
                DagCandidateAcceptanceFailure::MergedRootValidationFailed;
            cert.detail =
                formatv("{0}: DAG candidate patch rejected root id={1} "
                        "name={2} equivalent replacement failed merged "
                        "root validation",
                        traceStage, m.id, m.name)
                    .str();
            return cert;
          }

          if (uniquePatch->subtreeCertBacked || candPatch.subtreeCertBacked) {
            trace(
                "macro/proof",
                "DAG equivalent subtree-plan merged: root id={0} name={1} "
                "stage={2} mergedExpRoot={3} mergedDeferredArgs={4} "
                "mergedBridgeFormals={5}",
                m.id, m.name, traceStage,
                formatFormalTextPairMap(mergedValidation->expectedRootFormals),
                FormatUInt32List(mergedValidation->deferOccurrenceArgIdxs),
                formatBridgeSensitiveFormalSignatureMap(
                    mergedValidation->bridgeSensitiveFormalSignatures));
          }
          uniquePatchValidation = std::move(*mergedValidation);
          cert.accepted = true;
          cert.detail = formatv(
                            "{0}: DAG candidate patch equivalent to existing "
                            "root patch root id={1} name={2} span=[{3},{4})",
                            traceStage, m.id, m.name, uniquePatch->invStart,
                            uniquePatch->invEnd)
                            .str();
          return cert;
        }

        if (!uniquePatchBaseText || *uniquePatchBaseText != baseText) {
          cert.failure = DagCandidateAcceptanceFailure::DifferentBaseText;
          cert.detail =
              formatv("{0}: DAG candidate patch rejected root id={1} "
                      "name={2} base text mismatch baseLenExisting={3} "
                      "baseLenCandidate={4}",
                      traceStage, m.id, m.name,
                      uniquePatchBaseText ? uniquePatchBaseText->size() : 0,
                      baseText.size())
                  .str();
          return cert;
        }

        int preferredStructured = choosePreferredStructuredDagCandidate(
            uniquePatchValidation, candidateValidation);
        if (preferredStructured < 0) {
          trace("macro/proof",
                "DAG structured-choice kept existing patch: root id={0} "
                "name={1} stage={2} existing[{3}] candidate[{4}]",
                m.id, m.name, traceStage, FormatMacroPatchAudit(*uniquePatch),
                FormatMacroPatchAudit(candPatch));
          if (uniquePatch->subtreeCertBacked || candPatch.subtreeCertBacked) {
            trace("macro/proof",
                  "DAG structured subtree-choice kept existing: root id={0} "
                  "name={1} stage={2} existingExpRoot={3} candidateExpRoot={4}",
                  m.id, m.name, traceStage,
                  stringutils::showWSWithClip(
                      uniquePatch->subtreeExpectedRootFormalSummary, 160),
                  stringutils::showWSWithClip(
                      candPatch.subtreeExpectedRootFormalSummary, 160));
          }
          cert.accepted = true;
          cert.detail =
              formatv("{0}: kept existing structured DAG candidate root "
                      "patch root id={1} name={2} over flatter rival",
                      traceStage, m.id, m.name)
                  .str();
          return cert;
        }
        if (preferredStructured > 0) {
          if (!candPatch.macroId)
            candPatch.macroId = m.id;
          if (!candPatch.proofRootMacroId) {
            candPatch.proofRootMacroId = m.id;
            SyncMacroPatchProofSummary(candPatch);
          }
          trace("macro/proof",
                "DAG structured-choice replaced existing patch: root id={0} "
                "name={1} stage={2} existing[{3}] candidate[{4}]",
                m.id, m.name, traceStage, FormatMacroPatchAudit(*uniquePatch),
                FormatMacroPatchAudit(candPatch));
          if (uniquePatch->subtreeCertBacked || candPatch.subtreeCertBacked) {
            trace(
                "macro/proof",
                "DAG structured subtree-choice replaced existing: root id={0} "
                "name={1} stage={2} existingExpRoot={3} candidateExpRoot={4}",
                m.id, m.name, traceStage,
                stringutils::showWSWithClip(
                    uniquePatch->subtreeExpectedRootFormalSummary, 160),
                stringutils::showWSWithClip(
                    candPatch.subtreeExpectedRootFormalSummary, 160));
          }
          uniquePatch = std::move(candPatch);
          uniquePatchBaseText = baseText.str();
          uniquePatchValidation = std::move(candidateValidation);
          cert.accepted = true;
          cert.detail =
              formatv("{0}: replaced existing DAG candidate root patch "
                      "root id={1} name={2} with more structured rival",
                      traceStage, m.id, m.name)
                  .str();
          return cert;
        }

        trace("macro/proof",
              "DAG merge-candidate patch audit: root id={0} name={1} "
              "stage={2} existing[{3}] candidate[{4}]",
              m.id, m.name, traceStage, FormatMacroPatchAudit(*uniquePatch),
              FormatMacroPatchAudit(candPatch));
        SmallVector<StringRef, 2> repls;
        repls.push_back(StringRef(uniquePatch->replacement));
        repls.push_back(StringRef(candPatch.replacement));
        auto merged = mergeCompatibleCallsitePatchReplacements(
            *uniquePatchBaseText, ArrayRef<StringRef>(repls));
        if (!merged) {
          cert.failure = DagCandidateAcceptanceFailure::MergeConflict;
          cert.detail = formatv("{0}: DAG candidate patch rejected root id={1} "
                                "name={2} incompatible replacement hunks",
                                traceStage, m.id, m.name)
                            .str();
          return cert;
        }

        auto mergedValidation = mergeDagCandidateValidationMetadata(
            uniquePatchValidation, candidateValidation);
        if (uniquePatch->subtreeCertBacked || candPatch.subtreeCertBacked) {
          trace("macro/proof",
                "DAG merge subtree-plan probe: root id={0} name={1} stage={2} "
                "existingExpRoot={3} candidateExpRoot={4}",
                m.id, m.name, traceStage,
                stringutils::showWSWithClip(
                    uniquePatch->subtreeExpectedRootFormalSummary, 160),
                stringutils::showWSWithClip(
                    candPatch.subtreeExpectedRootFormalSummary, 160));
        }
        if (!mergedValidation) {
          cert.failure =
              DagCandidateAcceptanceFailure::MergedRootValidationFailed;
          cert.detail = formatv("{0}: DAG candidate patch rejected root id={1} "
                                "name={2} merged replacement produced "
                                "incompatible root-formal validation metadata",
                                traceStage, m.id, m.name)
                            .str();
          return cert;
        }

        if (uniquePatch->subtreeCertBacked || candPatch.subtreeCertBacked) {
          trace("macro/proof",
                "DAG merge subtree-plan merged: root id={0} name={1} stage={2} "
                "mergedExpRoot={3} mergedDeferredArgs={4} "
                "mergedBridgeFormals={5}",
                m.id, m.name, traceStage,
                formatFormalTextPairMap(mergedValidation->expectedRootFormals),
                FormatUInt32List(mergedValidation->deferOccurrenceArgIdxs),
                formatBridgeSensitiveFormalSignatureMap(
                    mergedValidation->bridgeSensitiveFormalSignatures));
        }
        if (!validateDagCandidateProof(*mergedValidation, *uniquePatchBaseText,
                                       StringRef(*merged), traceStage)) {
          cert.failure =
              DagCandidateAcceptanceFailure::MergedRootValidationFailed;
          cert.detail = formatv("{0}: DAG candidate patch rejected root id={1} "
                                "name={2} merged replacement failed root "
                                "validation",
                                traceStage, m.id, m.name)
                            .str();
          return cert;
        }

        uniquePatch->replacement = std::move(*merged);
        uniquePatchValidation = std::move(*mergedValidation);
        if (!uniquePatch->macroId)
          uniquePatch->macroId = candPatch.macroId;
        ++distinctRootPatches;
        cert.accepted = true;
        cert.merged = true;
        cert.detail =
            formatv("{0}: merged DAG candidate root patch root id={1} "
                    "name={2} distinctRootPatches={3} replLen={4}",
                    traceStage, m.id, m.name, distinctRootPatches,
                    uniquePatch->replacement.size())
                .str();
        return cert;
      };

      // Replay any root-level split-insertion candidates that were proven while
      // scanning paired pure-insertion envelopes.
      //
      // These candidates already carry a concrete replacement for the full root
      // callsite text (for example, a reconstructed `WRAP(INC, (1) + 3)`), but
      // they still need to pass through the normal DAG candidate validation and
      // merge path so they compete consistently with any other root patches.
      for (const SplitInsertionRootCandidate &candidate :
           splitInsertionRootCandidates) {
        // Re-derive the expected root-formal rewrite map directly from the
        // candidate's replacement text. This gives the DAG validator the same
        // root-formal expectations it would have had if this candidate had been
        // produced through the ordinary replay path.
        auto replayRootFormals =
            buildRootFormalRewriteMapFromCallsiteReplacement(
                invSpanText, StringRef(candidate.patch.replacement));
        if (!replayRootFormals) {
          trace("macro/dag",
                "DAG split insertion root patch rejected: root id={0} "
                "name='{1}' could not derive replay root formals from "
                "replacement='{2}'",
                m.id, m.name,
                stringutils::showWSWithClip(candidate.patch.replacement, 160));
          continue;
        }

        // Validate this split-root candidate against the exact set of root
        // formals implied by the reconstructed replacement. Also defer
        // occurrence-level consistency checks for the touched root formals so
        // the validator can discharge them using the final reconstructed root
        // callsite text rather than rejecting too early.
        DagCandidateValidationMetadata splitValidation;
        splitValidation.hasExpectedRootFormals = true;
        splitValidation.expectedRootFormals = *replayRootFormals;
        splitValidation.deferOccurrenceArgIdxs.assign(
            candidate.deferOccurrenceArgIdxs.begin(),
            candidate.deferOccurrenceArgIdxs.end());

        // Materialize a normal root patch from the queued split candidate.
        // Mark it as already proof-backed: the split-insertion logic has
        // already established that this is a structure-preserving args-only
        // root rewrite.
        MacroPatch splitRootPatch = candidate.patch;
        StampMacroPatchProof(splitRootPatch,
                             MacroPatchProofKind::ArgsOnlyPairedPureInsertion,
                             /*validated=*/true,
                             /*structurePreserving=*/true, m.id);

        // Feed the candidate through the shared DAG acceptance/merge logic so
        // it is deduplicated and checked for incompatibility exactly the same
        // way as other DAG-derived root patches.
        auto acceptCert = acceptOrMergeDAGCandidatePatch(
            std::move(splitRootPatch), invSpanText,
            "DAG split insertion root patch", &splitValidation);
        if (!acceptCert.detail.empty())
          trace("macro/dag", "{0}", acceptCert.detail);
        if (!acceptCert.accepted) {
          debug("macro/dag",
                "DAG args-only ambiguous: incompatible split-insertion root "
                "patches (root inv id={0} name={1} leafCandidates={2} "
                "leavesExamined={3} distinctRootPatches={4})",
                m.id, m.name, leafCands.size(), leavesExamined,
                distinctRootPatches + 1);
          return std::nullopt;
        }
      }

      for (const LeafCandidate &cand : leafCands) {
        ++leavesExamined;
        const RefoldModel::MacroInvocation &leaf = *cand.inv;

        DenseMap<uint32_t, SmallVector<ObservedFormalConstraint, 2>>
            leafObserved;
        DenseMap<uint32_t, OldNewText> leafEdits;
        DenseMap<uint64_t, SmallVector<const RefoldModel::PPArgSpan *, 4>>
            unreliPaste;
        bool invalid = false;

        auto recordLeafObserved = [&](uint32_t argIdx, StringRef oldText,
                                      StringRef newText) -> bool {
          auto &constraints = leafObserved[argIdx];
          for (const auto &existing : constraints) {
            if (existing.oldText == oldText && existing.newText == newText)
              return true;
          }
          constraints.push_back(
              ObservedFormalConstraint{oldText.str(), newText.str()});
          return true;
        };

        // --- Pass 1: collect reliable per-formal edits -----------------------
        //
        // For each touched arg-like span:
        //   * extract A and B text
        //   * normalize it (stringify/paste rules)
        //   * record old/new per formal
        //
        // If B extraction for a paste subrange is unreliable, defer it to
        // pass 2.
        for (const RefoldModel::PPArgSpan &sp : cand.argLike) {
          if (sp.argIdx >= cand.touched.size() || !cand.touched[sp.argIdx])
            continue;

          auto aTxt = extractSpanText(sp, /*fromB=*/false);
          auto bTxt = extractSpanText(sp, /*fromB=*/true);
          if (!aTxt || !bTxt)
            continue;

          if (sp.kind == PPArgSpanKind::Paste && sp.byteBegin && sp.byteEnd &&
              !bTxt->reliable) {
            uint64_t key = (uint64_t(sp.begin) << 32) | uint64_t(sp.end);
            unreliPaste[key].push_back(&sp);
            continue;
          }

          if (!bTxt->reliable) {
            invalid = true;
            break;
          }

          auto oldLift = normalizeLiftText(cand.inv, sp, aTxt->text,
                                           /*allowTopLevelComma=*/true);
          bool allowComma = sp.argIdx < cand.inv->defParams.size() &&
                            cand.inv->defParams[sp.argIdx].variadic;
          auto newLift = normalizeLiftText(cand.inv, sp, bTxt->text,
                                           /*allowTopLevelComma=*/allowComma);
          if (!oldLift || !newLift) {
            invalid = true;
            break;
          }

          if (*oldLift == *newLift)
            continue;

          if (!recordLeafObserved(sp.argIdx, *oldLift, *newLift)) {
            invalid = true;
            break;
          }
        }

        if (invalid)
          continue;

        // --- Pass 2: resolve unreliable paste subranges ----------------------
        //
        // When paste subrange extraction is unreliable on B (token length
        // changed), try to split the edited token using the unchanged "midBody"
        // delimiter that lies between the two subranges in the A token. Accept
        // only if the split is unique.
        for (auto &kv : unreliPaste) {
          auto &group = kv.second;
          if (group.size() < 2)
            continue;

          llvm::sort(group, PasteSpanPtrLessByByteRange);

          bool groupOk = true;
          for (const RefoldModel::PPArgSpan *sp : group) {
            if (!sp->byteBegin || !sp->byteEnd) {
              groupOk = false;
              break;
            }
          }
          if (!groupOk) {
            invalid = true;
            break;
          }

          // Extract full token texts in A and B for this token envelope.
          RefoldModel::PPArgSpan whole = *group.front();
          whole.kind = PPArgSpanKind::Standard;
          whole.argIdx = 0;
          whole.byteBegin = std::nullopt;
          whole.byteEnd = std::nullopt;

          auto aTok = extractSpanText(whole, /*fromB=*/false);
          auto bTok = extractSpanText(whole, /*fromB=*/true);
          if (!aTok || !bTok || !bTok->reliable) {
            invalid = true;
            break;
          }

          StringRef oldTok = aTok->text;
          StringRef newTok = bTok->text;
          const uint64_t oldLen = oldTok.size();

          for (size_t i = 0; i < group.size(); ++i) {
            const auto *sp = group[i];
            if (*sp->byteBegin > *sp->byteEnd || *sp->byteEnd > oldLen) {
              groupOk = false;
              break;
            }
            if (i > 0 && *group[i - 1]->byteEnd > *sp->byteBegin) {
              groupOk = false;
              break;
            }
          }
          if (!groupOk) {
            invalid = true;
            break;
          }

          StringRef leading = oldTok.take_front(*group.front()->byteBegin);
          StringRef trailing = oldTok.drop_front(*group.back()->byteEnd);
          if (!newTok.starts_with(leading) || !newTok.ends_with(trailing)) {
            invalid = true;
            break;
          }

          SmallVector<StringRef, 4> oldSegs;
          SmallVector<StringRef, 4> midBodies;
          oldSegs.reserve(group.size());
          midBodies.reserve(group.size() - 1);
          bool hasEmptyInternalSeparator = false;
          bool hasNonEmptyInternalSeparator = false;
          for (size_t i = 0; i < group.size(); ++i) {
            const auto *sp = group[i];
            oldSegs.push_back(oldTok.slice(*sp->byteBegin, *sp->byteEnd));
            if (i + 1 < group.size()) {
              StringRef mid =
                  oldTok.slice(*sp->byteEnd, *group[i + 1]->byteBegin);
              if (mid.empty()) {
                hasEmptyInternalSeparator = true;
                continue;
              }
              hasNonEmptyInternalSeparator = true;
              midBodies.push_back(mid);
            }
          }
          if (hasEmptyInternalSeparator && hasNonEmptyInternalSeparator) {
            invalid = true;
            break;
          }

          StringRef core =
              newTok.slice(leading.size(), newTok.size() - trailing.size());

          SmallVector<StringRef, 4> curSegs;
          SmallVector<SmallVector<StringRef, 4>, 2> splitSolutions;
          auto addSplitSolution = [&](const SmallVectorImpl<StringRef> &parts) {
            SmallVector<StringRef, 4> copy(parts.begin(), parts.end());
            for (const auto &existing : splitSolutions)
              if (existing == copy)
                return;
            splitSolutions.push_back(std::move(copy));
          };

          if (hasEmptyInternalSeparator) {
            // No literal delimiter survives between adjacent pasted operands.
            // In that case the only sound split witness is an unchanged operand
            // that still appears verbatim in the edited pasted core. Accept the
            // group only when those unchanged anchors induce exactly one
            // segmentation of the rewritten core back into per-operand pieces.
            SmallVector<size_t, 4> anchoredIdxs;
            SmallVector<SmallVector<size_t, 4>, 4> anchorStartsByIdx;
            anchoredIdxs.reserve(group.size());
            anchorStartsByIdx.reserve(group.size());

            for (size_t i = 0; i < oldSegs.size(); ++i) {
              const StringRef anchor = oldSegs[i];
              if (anchor.empty())
                continue;

              SmallVector<size_t, 4> starts;
              for (size_t pos = 0;
                   (pos = core.find(anchor, pos)) != StringRef::npos; ++pos)
                starts.push_back(pos);
              if (starts.empty())
                continue;

              anchoredIdxs.push_back(i);
              anchorStartsByIdx.push_back(std::move(starts));
            }

            if (anchoredIdxs.empty()) {
              invalid = true;
              break;
            }

            SmallVector<size_t, 4> curAnchorStarts;
            auto addZeroDelimiterAnchoredSolution =
                [&](ArrayRef<size_t> anchorStarts) {
                  SmallVector<StringRef, 4> parts(group.size());
                  size_t prevConsumed = 0;

                  for (size_t anchorPos = 0; anchorPos < anchoredIdxs.size();
                       ++anchorPos) {
                    const size_t anchorIdx = anchoredIdxs[anchorPos];
                    const size_t anchorBegin = anchorStarts[anchorPos];
                    const size_t anchorEnd =
                        anchorBegin + oldSegs[anchorIdx].size();
                    if (anchorBegin < prevConsumed || anchorEnd > core.size())
                      return;

                    if (anchorPos == 0) {
                      if (anchorIdx > 1)
                        return;
                      if (anchorIdx == 0) {
                        if (anchorBegin != 0)
                          return;
                      } else {
                        // The normal case: split the rewritten core around the
                        // original literal delimiters and require a unique
                        // segmentation.
                        parts[0] = core.slice(0, anchorBegin);
                      }
                    } else {
                      // The normal case: split the rewritten core around the
                      // original literal delimiters and require a unique
                      // segmentation.
                      const size_t prevAnchorIdx = anchoredIdxs[anchorPos - 1];
                      const size_t gapSegments = anchorIdx - prevAnchorIdx - 1;
                      if (gapSegments > 1)
                        return;
                      if (gapSegments == 1)
                        parts[prevAnchorIdx + 1] =
                            core.slice(prevConsumed, anchorBegin);
                      else if (anchorBegin != prevConsumed)
                        return;
                    }

                    parts[anchorIdx] = oldSegs[anchorIdx];
                    prevConsumed = anchorEnd;
                  }

                  const size_t trailingGapSegments =
                      group.size() - anchoredIdxs.back() - 1;
                  if (trailingGapSegments > 1)
                    return;
                  if (trailingGapSegments == 0) {
                    if (prevConsumed != core.size())
                      return;
                  } else {
                    // The normal case: split the rewritten core around the
                    // original literal delimiters and require a unique
                    // segmentation.
                    parts[anchoredIdxs.back() + 1] =
                        core.drop_front(prevConsumed);
                  }

                  addSplitSolution(parts);
                };

            auto enumerateZeroDelimiterAnchors =
                [&](auto &&self, size_t anchorPos, size_t minStart) -> void {
              if (splitSolutions.size() > 1)
                return;
              if (anchorPos == anchoredIdxs.size()) {
                addZeroDelimiterAnchoredSolution(curAnchorStarts);
                return;
              }

              const size_t anchorIdx = anchoredIdxs[anchorPos];
              const StringRef anchor = oldSegs[anchorIdx];
              for (size_t start : anchorStartsByIdx[anchorPos]) {
                if (start < minStart)
                  continue;
                curAnchorStarts.push_back(start);
                self(self, anchorPos + 1, start + anchor.size());
                curAnchorStarts.pop_back();
              }
            };
            enumerateZeroDelimiterAnchors(enumerateZeroDelimiterAnchors, 0, 0);
          } else {
            // The normal case: split the rewritten core around the original
            // literal delimiters and require a unique segmentation.
            auto suffixDelimiterNeed = [&](size_t delimIdx) -> uint64_t {
              const StringRef delim = midBodies[delimIdx];
              uint64_t need = 0;
              for (size_t segIdx = delimIdx + 1; segIdx < oldSegs.size();
                   ++segIdx)
                need += countSubstr(oldSegs[segIdx], delim);
              for (size_t later = delimIdx + 1; later < midBodies.size();
                   ++later)
                if (midBodies[later] == delim)
                  ++need;
              return need;
            };

            auto splitCore = [&](auto &&self, size_t delimIdx,
                                 StringRef rest) -> void {
              if (splitSolutions.size() > 1)
                return;
              if (delimIdx == midBodies.size()) {
                curSegs.push_back(rest);
                addSplitSolution(curSegs);
                curSegs.pop_back();
                return;
              }

              const StringRef delim = midBodies[delimIdx];
              const uint64_t needLeft = countSubstr(oldSegs[delimIdx], delim);
              const uint64_t needRight = suffixDelimiterNeed(delimIdx);

              for (size_t pos = 0;
                   (pos = rest.find(delim, pos)) != StringRef::npos; ++pos) {
                StringRef left = rest.slice(0, pos);
                StringRef tail = rest.drop_front(pos + delim.size());
                if (countSubstr(left, delim) < needLeft)
                  continue;
                if (countSubstr(tail, delim) < needRight)
                  continue;
                curSegs.push_back(left);
                self(self, delimIdx + 1, tail);
                curSegs.pop_back();
              }
            };
            splitCore(splitCore, 0, core);
          }

          if (splitSolutions.size() != 1 ||
              splitSolutions[0].size() != group.size()) {
            invalid = true;
            break;
          }

          auto recordLeafEdit = [&](uint32_t argIdx, StringRef oldText,
                                    StringRef newText) -> bool {
            return recordLeafObserved(argIdx, oldText, newText);
          };

          // Normalize each recovered segment and record it as a per-formal
          // edit.
          for (size_t i = 0; i < group.size(); ++i) {
            const auto *sp = group[i];
            auto oldSeg = normalizeLiftText(&leaf, *sp, oldSegs[i],
                                            /*allowTopLevelComma=*/true);
            auto newSeg = normalizeLiftText(&leaf, *sp, splitSolutions[0][i],
                                            /*allowTopLevelComma=*/false);
            if (!oldSeg || !newSeg) {
              groupOk = false;
              break;
            }
            if (*oldSeg == *newSeg)
              continue;
            if (!recordLeafEdit(sp->argIdx, *oldSeg, *newSeg)) {
              groupOk = false;
              break;
            }
          }
          if (!groupOk) {
            invalid = true;
            break;
          }
        }

        if (invalid)
          continue;

        // Convert each touched leaf formal's observed old/new expansion text
        // into a certified leaf rewrite before lifting it toward the root.
        // Prefer the normal formal-rewrite certificate. If that fails solely
        // because the leaf lives in macro-body space and its observed text does
        // not match a unique raw structural template, allow one narrower seed:
        // all observed constraints for that formal must collapse to the same
        // normalized old/new text. That seed still must pass the structured
        // lift/root pipeline below.
        DenseSet<uint32_t> observedLeafSeedArgIdxs;
        for (const auto &KV : leafObserved) {
          const uint32_t argIdx = KV.first;
          auto formalCert = buildObservedFormalRewriteCertificate(
              leaf, argIdx, KV.second, /*preferredChildSyntax=*/nullptr,
              "DAG subtree leaf formal");
          if (formalCert.kind == FormalRewriteCertificateKind::Invalid) {
            const bool uniformObservedSeedAllowed =
                formalCert.failure ==
                FormalRewriteFailure::MissingStructuralTemplate;
            if (uniformObservedSeedAllowed) {
              auto observedSeed = buildUniformObservedLeafSeedCertificate(
                  leaf, argIdx, KV.second, "DAG subtree leaf formal");
              if (observedSeed.kind ==
                  UniformObservedLeafSeedCertificateKind::Unique) {
                trace("macro/dag", "{0}", observedSeed.detail);
                observedLeafSeedArgIdxs.insert(argIdx);
                leafEdits[argIdx] = OldNewText{std::move(observedSeed.oldText),
                                               std::move(observedSeed.newText)};
                continue;
              }
              if (!observedSeed.detail.empty())
                trace("macro/dag", "{0}", observedSeed.detail);
            }
            if (!formalCert.detail.empty())
              trace("macro/dag",
                    "DAG subtree leaf formal: inv id={0} name={1} argIdx={2} "
                    "INVALID detail={3}",
                    leaf.id, leaf.name, argIdx, formalCert.detail);
            invalid = true;
            break;
          }
          if (formalCert.kind == FormalRewriteCertificateKind::NoChange) {
            if (!formalCert.detail.empty())
              trace("macro/dag",
                    "DAG subtree leaf formal: inv id={0} name={1} argIdx={2} "
                    "NOCHANGE detail={3}",
                    leaf.id, leaf.name, argIdx, formalCert.detail);
            continue;
          }

          leafEdits[argIdx] = OldNewText{std::move(formalCert.oldText),
                                         std::move(formalCert.newText)};
        }

        if (invalid)
          continue;
        if (leafEdits.empty()) {
          trace("macro/dag",
                "DAG subtree leaf: no certifiable leaf edits root id={0} "
                "name={1} leaf id={2} name={3} observedLeafFormals={4}",
                m.id, m.name, leaf.id, leaf.name, leafObserved.size());
          continue;
        }

        SmallVector<uint32_t, 8> leafEditArgIdxs;
        leafEditArgIdxs.reserve(leafEdits.size());
        for (const auto &KV : leafEdits)
          leafEditArgIdxs.push_back(KV.first);

        const bool leafTouchesPaste =
            !leaf.pasteSpans.empty() &&
            AnyInvocationArgTouchesPaste(leaf, leafEditArgIdxs);
        const bool allTouchedPasteArgsFromObservedLeafSeed =
            leafTouchesPaste &&
            AllTouchedPasteArgsAreContained(leaf, leafEditArgIdxs,
                                           observedLeafSeedArgIdxs);

        // Paste-aware leaf certificate: when multiple leaf-formal rewrites
        // participate in the same pasted token, validate them as a group
        // against the B-side pasted-token spellings before attempting to lift
        // them up the caller chain.
        if (!leaf.pasteSpans.empty()) {
          DenseMap<uint32_t, std::string> leafReplByArgIdx;
          for (const auto &KV : leafEdits)
            leafReplByArgIdx[KV.first] = KV.second.newText;

          if (leafTouchesPaste) {
            if (allTouchedPasteArgsFromObservedLeafSeed) {
              trace("macro/dag",
                    "DAG subtree leaf paste validation: inv id={0} name={1} "
                    "deferred all touched paste args use uniform observed leaf "
                    "seed",
                    leaf.id, leaf.name);
            } else if (!leaf.invText) {
              trace("macro/dag",
                    "DAG subtree leaf paste validation: inv id={0} name={1} "
                    "FAILED missing invocation text",
                    leaf.id, leaf.name);
              invalid = true;
            } else {
            // The normal case: split the rewritten core around the original
            // literal delimiters and require a unique segmentation.
              auto leafRangesOpt = GetMacroInvocationFormalArgContentRanges(
                  leaf, StringRef(*leaf.invText));
              if (!leafRangesOpt) {
                trace("macro/dag",
                      "DAG subtree leaf paste validation: inv id={0} name={1} "
                      "FAILED missing argument ranges",
                      leaf.id, leaf.name);
                invalid = true;
              } else if (!PasteArgReplacementsMatchAllPasteTokensInB(
                             leaf, StringRef(*leaf.invText), *leafRangesOpt,
                             leafReplByArgIdx)) {
                trace("macro/dag",
                      "DAG subtree leaf paste validation: inv id={0} name={1} "
                      "FAILED pasted-token mismatch replacements={2}",
                      leaf.id, leaf.name, leafReplByArgIdx.size());
                invalid = true;
              }
            }
          }
        }

        if (invalid)
          continue;

        bool deferLeafPasteValidation = false;
        if (!leaf.pasteSpans.empty())
          deferLeafPasteValidation = allTouchedPasteArgsFromObservedLeafSeed;

        // --- Special-case: chained call suffix arguments --------------------
        //
        // If the root invocation expands to an identifier that is immediately
        // called (e.g. PICK1()(10)), the callee's arguments are spelled in the
        // source as a chained call suffix following the root invocation. In
        // this situation, the leaf edit cannot be lifted to the root via
        // argDeps because the root has no formal parameters. Preserve the call
        // chain by patching the chained suffix argument ranges directly in the
        // invocation file text.
        if (numArgs == 0 && leaf.callerMacroId && *leaf.callerMacroId == m.id &&
            m.invFile && leaf.invFile && *leaf.invFile == *m.invFile) {
          const std::string absPath = lineDirs_.ToAbsolutePath(*m.invFile);
          auto bufOrErr = llvm::MemoryBuffer::getFile(absPath);
          if (bufOrErr) {
            StringRef fileText = bufOrErr.get()->getBuffer();

            // Compute the chained call end in the same way as the application
            // phase: consume any trailing "(...)" groups after the root
            // invocation.
            const uint64_t chainEnd =
                extendChainedCallEnd(fileText, *invEnd, "((x)+1)");
            if (chainEnd > *invEnd && chainEnd <= (uint64_t)fileText.size()) {
              struct LocalEdit {
                uint64_t begin; // relative to invStart
                uint64_t end;   // relative to invStart
                std::string repl;
              };

              SmallVector<LocalEdit, 4> localEdits;
              bool ok = true;

              for (auto &kv : leafEdits) {
                const uint32_t argIdx = kv.first;
                if (argIdx >= leaf.invArgRanges.size()) {
                  ok = false;
                  break;
                }

                const auto &rng = leaf.invArgRanges[argIdx];
                if (!rng.first || !rng.second) {
                  ok = false;
                  break;
                }

                const uint64_t bAbs = *rng.first;
                const uint64_t eAbs = *rng.second;
                if (bAbs > eAbs || eAbs > (uint64_t)fileText.size() ||
                    bAbs < *invStart || eAbs > chainEnd) {
                  ok = false;
                  break;
                }

                // Ensure the "old" text actually matches the invocation file at
                // the recorded byte range, so we don't patch unrelated text.
                StringRef oldInFile =
                    fileText.slice((size_t)bAbs, (size_t)eAbs).trim();
                if (oldInFile != StringRef(kv.second.oldText).trim()) {
                  ok = false;
                  break;
                }

                localEdits.push_back(LocalEdit{
                    bAbs - *invStart,
                    eAbs - *invStart,
                    StringRef(kv.second.newText).trim().str(),
                });
              }

              if (ok && !localEdits.empty()) {
                llvm::sort(localEdits,
                           [](const LocalEdit &a, const LocalEdit &b) {
                             return a.begin < b.begin;
                           });

                uint64_t curB = 0;
                for (const auto &e : localEdits) {
                  if (e.begin < curB || e.end < e.begin) {
                    ok = false;
                    break;
                  }
                  curB = e.end;
                }
              }

              if (ok && !localEdits.empty()) {
                std::string replText =
                    fileText.slice((size_t)*invStart, (size_t)chainEnd).str();

                // Apply edits back-to-front to keep byte indices stable.
                for (auto it = localEdits.rbegin(); it != localEdits.rend();
                     ++it) {
                  replText.replace((size_t)it->begin,
                                   (size_t)(it->end - it->begin), it->repl);
                }

                trace("macro/dag",
                      "DAG chained-call suffix patch: root id={0} leaf id={1} "
                      "inv=[{2},{3}) chainEnd={4} edits={5} replLen={6}",
                      m.id, leaf.id, *invStart, *invEnd, chainEnd,
                      localEdits.size(), replText.size());

                MacroPatch candPatch{*invStart, chainEnd, replText, m.id};
                StampMacroPatchProof(candPatch,
                                     MacroPatchProofKind::CallChainSuffix,
                                     /*validated=*/true,
                                     /*structurePreserving=*/true, m.id);

                auto acceptCert = acceptOrMergeDAGCandidatePatch(
                    std::move(candPatch),
                    fileText.slice((size_t)*invStart, (size_t)chainEnd),
                    "DAG chained-call suffix patch");
                if (!acceptCert.detail.empty())
                  trace("macro/dag", "{0}", acceptCert.detail);
                if (!acceptCert.accepted) {
                  debug("macro/dag",
                        "DAG args-only ambiguous: incompatible root patch "
                        "candidates (root inv id={0} name={1} "
                        "leafCandidates={2} leavesExamined={3} "
                        "distinctRootPatches={4})",
                        m.id, m.name, leafCands.size(), leavesExamined,
                        distinctRootPatches + 1);
                  return std::nullopt;
                }
                continue;
              }
            }
          }
        }

        // --- Build one explicit subtree certificate --------------------------
        //
        // The leaf rewrite, caller-chain lifting, root-formal merge, and final
        // root validation are now treated as one bottom-up subtree certificate
        // instead of several ad hoc stages.
        auto subtreeCert = buildSubtreeRewriteCertificate(
            leaf, leafEdits, deferLeafPasteValidation);
        if (subtreeCert.kind == SubtreeRewriteCertificateKind::Invalid ||
            subtreeCert.kind == SubtreeRewriteCertificateKind::NoChange) {
          if (!subtreeCert.detail.empty())
            trace("macro/dag", "{0}", subtreeCert.detail);
          trace("macro/dag",
                "DAG subtree rewrite not certifiable: expanding root id={0} "
                "name={1} leaf id={2} name={3} kind={4} leafEdits={5}",
                m.id, m.name, leaf.id, leaf.name,
                static_cast<unsigned>(subtreeCert.kind), leafEdits.size());
          continue;
        }

        trace("macro/dag",
              "DAG subtree semantic summary: root id={0} name={1} leaf id={2} "
              "name={3} invCerts={4} formalCerts={5} argCerts={6} "
              "slotCerts={7} interactions={8} formalConsistency={9} "
              "derivations={10} liftSteps={11} rootMerges={12} "
              "lexicalBridge={13} paste={14} wrappers={15} mixed={16} "
              "admissible={17}",
              m.id, m.name, leaf.id, leaf.name,
              subtreeCert.semantic.invocationCertificates.size(),
              subtreeCert.semantic.formalCertificates.size(),
              subtreeCert.semantic.argCertificates.size(),
              subtreeCert.semantic.slotCertificates.size(),
              subtreeCert.semantic.interactionCertificates.size(),
              subtreeCert.semantic.formalInteractionConsistencies.size(),
              subtreeCert.semantic.parentDerivations.size(),
              subtreeCert.semantic.structuredLiftCertificates.size(),
              subtreeCert.semantic.rootMergeCertificates.size(),
              subtreeCert.semantic.usesLexicalBridge ? 1 : 0,
              subtreeCert.semantic.touchesPaste ? 1 : 0,
              subtreeCert.semantic.hasWrapperSemantics ? 1 : 0,
              subtreeCert.semantic.interactionSummary.hasMixedInteractions ? 1
                                                                           : 0,
              subtreeCert.semantic.admissibility.valid ? 1 : 0);

        if (!subtreeCert.semantic.interactionSummary.detail.empty())
          trace("macro/dag", "{0}",
                subtreeCert.semantic.interactionSummary.detail);
        if (!subtreeCert.semantic.interactionConsistency.detail.empty())
          trace("macro/dag", "{0}",
                subtreeCert.semantic.interactionConsistency.detail);
        if (!subtreeCert.semantic.admissibility.detail.empty())
          trace("macro/dag", "{0}", subtreeCert.semantic.admissibility.detail);

        auto rootPatchCert = buildRootPatchConstructionCertificate(
            subtreeCert.rootCert, "DAG subtree root patch");
        if (rootPatchCert.kind ==
                RootPatchConstructionCertificateKind::Invalid ||
            rootPatchCert.kind ==
                RootPatchConstructionCertificateKind::NoChange) {
          if (!rootPatchCert.detail.empty())
            trace("macro/dag", "{0}", rootPatchCert.detail);
          trace(
              "macro/dag",
              "DAG subtree root patch not constructible: expanding root id={0} "
              "name={1} leaf id={2} name={3} rootPatchKind={4}",
              m.id, m.name, leaf.id, leaf.name,
              static_cast<unsigned>(rootPatchCert.kind));
          continue;
        }

        trace("macro/dag", "{0}", rootPatchCert.detail);

        DagCandidateValidationMetadata subtreeValidation =
            buildDagCandidateValidationMetadataFromSubtree(subtreeCert);
        if (!validateDagCandidateProof(
                subtreeValidation, invSpanText,
                StringRef(rootPatchCert.patch->replacement),
                "DAG subtree root patch")) {
          trace("macro/dag",
                "DAG subtree root proof validation failed: rejecting root "
                "id={0} name={1} leaf id={2} name={3} repl='{4}'",
                m.id, m.name, leaf.id, leaf.name,
                stringutils::showWSWithClip(rootPatchCert.patch->replacement,
                                            160));
          continue;
        }

        rootPatchCert.patch->macroId = m.id;
        StampMacroPatchProof(*rootPatchCert.patch,
                             MacroPatchProofKind::DagSubtreeRoot,
                             /*validated=*/true,
                             /*structurePreserving=*/true, m.id);
        rootPatchCert.patch->subtreeCertBacked = true;
        rootPatchCert.patch->subtreeLeafMacroId = leaf.id;
        rootPatchCert.patch->subtreeWitnessCount = 1;
        rootPatchCert.patch->subtreeInvocationCertCount = static_cast<uint32_t>(
            subtreeCert.semantic.invocationCertificates.size());
        rootPatchCert.patch->subtreeFormalCertCount = static_cast<uint32_t>(
            subtreeCert.semantic.formalCertificates.size());
        rootPatchCert.patch->subtreeArgCertCount =
            static_cast<uint32_t>(subtreeCert.semantic.argCertificates.size());
        rootPatchCert.patch->subtreeLiftChainCount =
            static_cast<uint32_t>(subtreeCert.semantic.liftChains.size());
        rootPatchCert.patch->subtreeLiftStepCount = static_cast<uint32_t>(
            subtreeCert.semantic.structuredLiftCertificates.size());
        rootPatchCert.patch->subtreeRootMergeCount = static_cast<uint32_t>(
            subtreeCert.semantic.rootMergeCertificates.size());
        rootPatchCert.patch->subtreeUsesLexicalBridge =
            subtreeCert.semantic.usesLexicalBridge;
        rootPatchCert.patch->subtreeTouchesPaste =
            subtreeCert.semantic.touchesPaste;
        rootPatchCert.patch->subtreeHasWrapperSemantics =
            subtreeCert.semantic.hasWrapperSemantics;
        rootPatchCert.patch->subtreeHasStringifySemantics =
            subtreeCert.semantic.hasStringifySemantics;
        rootPatchCert.patch->subtreeHasWideStringifySemantics =
            subtreeCert.semantic.hasWideStringifySemantics;
        rootPatchCert.patch->subtreeHasPreferredChildSyntax =
            subtreeCert.semantic.hasPreferredChildSyntax;
        rootPatchCert.patch->subtreeHasRawInvocationPreservation =
            subtreeCert.semantic.hasRawInvocationPreservation;
        rootPatchCert.patch->subtreeHasPassthroughFlatten =
            subtreeCert.semantic.hasPassthroughFlatten;
        rootPatchCert.patch->subtreeHasBridgeSensitiveStructuredSemantics =
            subtreeCert.semantic.hasBridgeSensitiveStructuredSemantics;
        rootPatchCert.patch->subtreeDeferredPasteDischarged =
            subtreeCert.semantic.deferredPasteDischarge.valid;
        rootPatchCert.patch->subtreeAdmissible =
            subtreeCert.semantic.admissibility.valid;
        rootPatchCert.patch->subtreeExpectedRootFormalCount =
            static_cast<uint32_t>(subtreeValidation.expectedRootFormals.size());
        rootPatchCert.patch->subtreeDeferredRootArgCount =
            static_cast<uint32_t>(
                subtreeValidation.deferOccurrenceArgIdxs.size());
        rootPatchCert.patch->subtreeBridgeSensitiveFormalCount =
            static_cast<uint32_t>(
                subtreeValidation.bridgeSensitiveFormalSignatures.size());
        rootPatchCert.patch->subtreeExpectedRootFormalSummary =
            formatFormalTextPairMap(subtreeValidation.expectedRootFormals);
        rootPatchCert.patch->subtreeDeferredRootArgSummary =
            FormatUInt32List(subtreeValidation.deferOccurrenceArgIdxs);
        rootPatchCert.patch->subtreeBridgeSensitiveFormalSummary =
            formatBridgeSensitiveFormalSignatureMap(
                subtreeValidation.bridgeSensitiveFormalSignatures);
        trace("macro/proof",
              "DAG subtree root patch audit: root id={0} leaf id={1} {2}", m.id,
              leaf.id, FormatMacroPatchAudit(*rootPatchCert.patch));
        auto acceptCert = acceptOrMergeDAGCandidatePatch(
            std::move(*rootPatchCert.patch), invSpanText,
            "DAG subtree root patch", &subtreeValidation);
        if (!acceptCert.detail.empty())
          trace("macro/dag", "{0}", acceptCert.detail);
        if (!acceptCert.accepted) {
          debug("macro/dag",
                "DAG args-only ambiguous: incompatible root patches (root "
                "inv id={0} name={1} leafCandidates={2} leavesExamined={3} "
                "distinctRootPatches={4})",
                m.id, m.name, leafCands.size(), leavesExamined,
                distinctRootPatches + 1);
          return std::nullopt;
        }
        continue;
      }

      // Summary diagnostics: how many leaves we considered, how many distinct
      // root patches survived validation, and whether we produced a unique
      // patch.
      debug(
          "macro/dag",
          "DAG args-only summary: root inv id={0} name={1} leafCandidates={2} "
          "leavesExamined={3} distinctRootPatches={4} result={5} "
          "directRootInadmissible={6}",
          m.id, m.name, leafCands.size(), leavesExamined, distinctRootPatches,
          uniquePatch.has_value(), directRootPreservationInadmissible);

      if (!uniquePatch && directRootPreservationInadmissible) {
        trace("macro/dag",
              "DAG args-only: direct root preservation inadmissible for root "
              "id={0} name='{1}' because the touched hunk lies in an "
              "unsupported descendant subtree",
              m.id, m.name);
      }
      return uniquePatch;
    };

    // Call-chain suffix patch: if this hunk's A-side PP tokens map to source
    // bytes in the chained-call suffix immediately following this invocation
    // (e.g. currying-style chains like GET_MATH(ADD)(10)(20)), patch those
    // bytes directly. This preserves the call chain and avoids whole-cover
    // expansion.
    if (HasLiteralMacroCalleeOrigin(m) && m.invFile && m.invB && m.invE) {
      std::string invAbs = lineDirs_.ToAbsolutePath(*m.invFile);
      auto bufOrErr = MemoryBuffer::getFile(invAbs);
      if (bufOrErr) {
        std::unique_ptr<MemoryBuffer> buf = std::move(*bufOrErr);
        StringRef invFileText = buf->getBuffer();
        const uint64_t n = invFileText.size();
        const uint64_t invEndAbs = *m.invE;
        if (invEndAbs <= n) {
          const uint64_t chainEndAbs =
              extendChainedCallEnd(invFileText, invEndAbs, StringRef());
          if (chainEndAbs > invEndAbs) {
            const uint64_t aLen = h.aEnd - h.aStart;
            const uint64_t bLen = h.bEnd - h.bStart;
            if (aLen == bLen && aLen > 0) {
              struct TokEdit {
                uint64_t bAbs;
                uint64_t eAbs;
                std::string repl;
              };
              SmallVector<TokEdit, 8> tokEdits;
              tokEdits.reserve(aLen);
              uint64_t minB = std::numeric_limits<uint64_t>::max();
              uint64_t maxE = 0;
              bool ok = true;

              const auto &tokmapByPP = model_.GetTokmapByPP();
              for (uint64_t i = 0; i < aLen; ++i) {
                const uint64_t ppIdx = h.aStart + i;
                const uint64_t bTok = h.bStart + i;
                auto it = tokmapByPP.find(ppIdx);
                if (it == tokmapByPP.end()) {
                  ok = false;
                  break;
                }
                const RefoldModel::TokMapEntry &tm = it->second;
                if (lineDirs_.ToAbsolutePath(tm.file) != invAbs) {
                  ok = false;
                  break;
                }
                if (tm.b < invEndAbs || tm.e > chainEndAbs) {
                  ok = false;
                  break;
                }
                if (tm.b > tm.e || tm.e > n) {
                  ok = false;
                  break;
                }
                StringRef repl = SliceBSource(bTok, bTok + 1);
                tokEdits.push_back(TokEdit{tm.b, tm.e, repl.str()});
                minB = std::min(minB, tm.b);
                maxE = std::max(maxE, tm.e);
              }

              if (ok && minB < maxE && maxE <= n) {
                std::string covered = invFileText.slice(minB, maxE).str();

                SmallVector<TextEdit, 8> edits;
                edits.reserve(tokEdits.size());
                for (const auto &te : tokEdits)
                  edits.push_back(TextEdit{te.bAbs - minB, te.eAbs - minB,
                                           te.repl, std::nullopt,
                                           std::nullopt, {}});
                llvm::sort(edits, [](const TextEdit &a, const TextEdit &b) {
                  return a.start < b.start;
                });

                // Apply edits (no line-directive resync needed inside this
                // local slice).
                std::string out;
                out.reserve(covered.size());
                uint64_t cur = 0;
                for (const TextEdit &e : edits) {
                  if (e.start < cur || e.end > covered.size()) {
                    ok = false;
                    break;
                  }
                  out.append(covered, cur, e.start - cur);
                  out.append(e.text);
                  cur = e.end;
                }
                if (ok) {
                  out.append(covered, cur, covered.size() - cur);
                  {
                    MacroPatch patch{minB, maxE, std::move(out), m.id};
                    StampMacroPatchProof(patch,
                                         MacroPatchProofKind::CallChainSuffix,
                                         /*validated=*/true,
                                         /*structurePreserving=*/true, m.id);
                    return patch;
                  }
                }
              }
            }
          }
        }
      }
    }

    // Prefer the DAG result over a direct root args-only rewrite when both are
    // available: the DAG path has already proved a structure-preserving nested
    // inverse, while the direct root patch only proves expansion equality at
    // this callsite.
    auto mergeCompatibleRootCallsiteReplacements =
        [&](StringRef baseOld, ArrayRef<StringRef> replacements)
        -> std::optional<std::string> {
      struct RootCallsiteRewriteHunk {
        uint64_t oldBegin;
        uint64_t oldEnd;
        std::string repl;
      };

      auto toSingleCharRefsLocal = [&](StringRef S) {
        std::vector<StringRef> refs;
        refs.reserve(S.size());
        for (size_t i = 0; i < S.size(); ++i)
          refs.push_back(S.slice(i, i + 1));
        return refs;
      };

      std::vector<RootCallsiteRewriteHunk> merged;
      for (StringRef replText : replacements) {
        if (replText == baseOld)
          continue;

        std::vector<StringRef> aRefs = toSingleCharRefsLocal(baseOld);
        std::vector<StringRef> bRefs = toSingleCharRefsLocal(replText);
        auto steps = diffutils::diff(aRefs, bRefs);
        auto hunks = diffutils::coalesce(steps);

        for (const auto &hunk : hunks) {
          RootCallsiteRewriteHunk piece{
              hunk.aStart, hunk.aEnd,
              replText.slice((size_t)hunk.bStart, (size_t)hunk.bEnd).str()};

          auto sameHunk = [&](const RootCallsiteRewriteHunk &a,
                              const RootCallsiteRewriteHunk &b) {
            return a.oldBegin == b.oldBegin && a.oldEnd == b.oldEnd &&
                   a.repl == b.repl;
          };

          auto overlaps = [&](const RootCallsiteRewriteHunk &a,
                              const RootCallsiteRewriteHunk &b) {
            return a.oldBegin < b.oldEnd && b.oldBegin < a.oldEnd;
          };

          auto it = std::lower_bound(
              merged.begin(), merged.end(), piece.oldBegin,
              [](const RootCallsiteRewriteHunk &h, uint64_t pos) {
                return h.oldBegin < pos;
              });

          if (it != merged.begin()) {
            const auto &prev = *std::prev(it);
            if (sameHunk(prev, piece))
              continue;
            if (overlaps(prev, piece))
              return std::nullopt;
          }
          if (it != merged.end()) {
            if (sameHunk(*it, piece))
              continue;
            if (overlaps(*it, piece))
              return std::nullopt;
          }

          merged.insert(it, std::move(piece));
        }
      }

      std::string out = baseOld.str();
      for (auto it = merged.rbegin(); it != merged.rend(); ++it)
        out.replace((size_t)it->oldBegin,
                    (size_t)(it->oldEnd - it->oldBegin), it->repl);
      return out;
    };

    auto validateMergedDirectAndDagRootReplacement =
        [&](StringRef baseText, StringRef newText) -> bool {
      if (baseText == newText) {
        trace("macro/dag",
              "DAG/direct merged root patch: root proof validation no-op "
              "root id={0} name='{1}'",
              m.id, m.name);
        return true;
      }

      auto baseRangesOpt =
          GetMacroInvocationFormalArgContentRanges(m, baseText);
      auto newRangesOpt = GetMacroInvocationFormalArgContentRanges(m, newText);
      if (!baseRangesOpt || !newRangesOpt ||
          newRangesOpt->size() != baseRangesOpt->size()) {
        trace("macro/dag",
              "DAG/direct merged root patch: root proof validation failed "
              "root id={0} name='{1}' could not derive replay root-formal "
              "rewrite map baseLen={2} newLen={3}",
              m.id, m.name, baseText.size(), newText.size());
        return false;
      }

      const auto &baseRanges = *baseRangesOpt;
      const auto &newRanges = *newRangesOpt;

      auto fixedSpansMatch = [&]() -> bool {
        size_t oldCursor = 0;
        size_t newCursor = 0;
        for (size_t argIdx = 0; argIdx < baseRanges.size(); ++argIdx) {
          const auto &oldR = baseRanges[argIdx];
          const auto &newR = newRanges[argIdx];
          if (oldR.first > oldR.second || oldR.second > baseText.size() ||
              newR.first > newR.second || newR.second > newText.size())
            return false;

          if (baseText.slice(oldCursor, oldR.first) !=
              newText.slice(newCursor, newR.first))
            return false;

          oldCursor = oldR.second;
          newCursor = newR.second;
        }
        return baseText.drop_front(oldCursor) == newText.drop_front(newCursor);
      };

      if (!fixedSpansMatch()) {
        trace("macro/dag",
              "DAG/direct merged root patch: root proof validation failed "
              "root id={0} name='{1}' fixed invocation syntax changed",
              m.id, m.name);
        return false;
      }

      unsigned replayFormalCount = 0;
      for (size_t argIdx = 0; argIdx < baseRanges.size(); ++argIdx) {
        const auto &oldR = baseRanges[argIdx];
        const auto &newR = newRanges[argIdx];
        StringRef oldArg =
            baseText.slice((size_t)oldR.first, (size_t)oldR.second).trim();
        StringRef newArg =
            newText.slice((size_t)newR.first, (size_t)newR.second).trim();
        if (oldArg != newArg)
          ++replayFormalCount;
      }

      trace("macro/dag",
            "DAG/direct merged root patch: root proof validation succeeded "
            "root id={0} name='{1}' replayFormals={2} deferredArgs=0",
            m.id, m.name, replayFormalCount);
      return true;
    };

    struct LocalFormalTextPair {
      std::string oldText;
      std::string newText;
    };

    // The patch audit stores expected-root rewrites in a compact summary form
    // such as {0:'bill'->'bill', 1:'y'->'z'}. We only need enough structure to
    // compare same-root witness cohorts, so parse that summary with the raw
    // lexer instead of hand-scanning punctuation.
    auto parseExpectedRootFormalSummary = [&](StringRef summary) {
      DenseMap<uint32_t, LocalFormalTextPair> out;
      StringRef s = summary.trim();
      if (s.empty() || s == "{}")
        return out;

      const SourceLocation BaseLoc = SourceLocation::getFromRawEncoding(1);
      std::string LexBuf = s.str();
      LexBuf.push_back('\0');
      const char *BufStart = LexBuf.data();
      const char *BufEnd = BufStart + s.size();
      Lexer Lex(BaseLoc, lexLang_, BufStart, BufStart, BufEnd);

      auto nextNonCommentToken = [&]() {
        Token Tok;
        while (true) {
          Lex.LexFromRawLexer(Tok);
          if (!Tok.is(tok::comment))
            return Tok;
        }
      };

      auto tokenText = [&](const Token &Tok) -> StringRef {
        const unsigned offset =
            Tok.getLocation().getRawEncoding() - BaseLoc.getRawEncoding();
        return StringRef(BufStart + offset, Tok.getLength());
      };

      auto parseQuotedPayload = [&](const Token &Tok)
          -> std::optional<std::string> {
        switch (Tok.getKind()) {
        case tok::char_constant:
        case tok::wide_char_constant:
        case tok::utf8_char_constant:
        case tok::utf16_char_constant:
        case tok::utf32_char_constant:
          break;
        default:
          return std::nullopt;
        }

        StringRef text = tokenText(Tok);
        if (text.size() < 2 || text.front() != '\'' || text.back() != '\'')
          return std::nullopt;
        return text.drop_front().drop_back().str();
      };

      Token Tok = nextNonCommentToken();
      if (!Tok.is(tok::l_brace))
        return DenseMap<uint32_t, LocalFormalTextPair>{};

      while (true) {
        Tok = nextNonCommentToken();
        if (Tok.is(tok::r_brace) || Tok.is(tok::eof))
          break;
        if (!Tok.is(tok::numeric_constant))
          return DenseMap<uint32_t, LocalFormalTextPair>{};

        uint32_t argIdx = 0;
        if (tokenText(Tok).getAsInteger(10, argIdx))
          return DenseMap<uint32_t, LocalFormalTextPair>{};

        Tok = nextNonCommentToken();
        if (!Tok.is(tok::colon))
          return DenseMap<uint32_t, LocalFormalTextPair>{};

        Tok = nextNonCommentToken();
        auto oldText = parseQuotedPayload(Tok);
        if (!oldText)
          return DenseMap<uint32_t, LocalFormalTextPair>{};

        Tok = nextNonCommentToken();
        if (!Tok.is(tok::arrow))
          return DenseMap<uint32_t, LocalFormalTextPair>{};

        Tok = nextNonCommentToken();
        auto newText = parseQuotedPayload(Tok);
        if (!newText)
          return DenseMap<uint32_t, LocalFormalTextPair>{};

        out[argIdx] = LocalFormalTextPair{std::move(*oldText),
                                          std::move(*newText)};

        Tok = nextNonCommentToken();
        if (Tok.is(tok::r_brace) || Tok.is(tok::eof))
          break;
        if (!Tok.is(tok::comma))
          return DenseMap<uint32_t, LocalFormalTextPair>{};
      }
      return out;
    };

    // Only compare witnesses that are already at the same concrete discharge
    // level. Deferred bridge-backed and passthrough-backed candidates are valid
    // subtree witnesses too, but they intentionally represent the same root at
    // a different abstraction level and must not be treated as conflicting
    // concrete cohorts.
    auto isConcreteSubtreeWitnessCohort = [&](const MacroPatch &patch) {
      if (!patch.subtreeCertBacked)
        return false;
      if (patch.subtreeUsesLexicalBridge)
        return false;
      if (patch.subtreeHasPassthroughFlatten)
        return false;
      if (patch.subtreeDeferredRootArgCount != 0)
        return false;
      return true;
    };

    // Two different subtree-backed leaves for the same root only force
    // whole-cover fallback when they disagree on an overlapping concrete root
    // formal rewrite at the same concrete discharge level. This keeps safe
    // deferred wrapper/stringify cohorts from being misclassified as conflicts
    // merely because they preserve the same root through different bridge or
    // deferred-discharge evidence.
    auto conflictingConcreteSubtreeWitnesses =
        [&](const MacroPatch &existing, const MacroPatch &candidate) {
          if (!existing.subtreeCertBacked || !candidate.subtreeCertBacked)
            return false;
          if (existing.proofRootMacroId != m.id ||
              candidate.proofRootMacroId != m.id)
            return false;
          if (!existing.subtreeLeafMacroId || !candidate.subtreeLeafMacroId)
            return false;
          if (existing.subtreeLeafMacroId == candidate.subtreeLeafMacroId)
            return false;
          if (!isConcreteSubtreeWitnessCohort(existing) ||
              !isConcreteSubtreeWitnessCohort(candidate))
            return false;

          const auto existingFormals = parseExpectedRootFormalSummary(
              existing.subtreeExpectedRootFormalSummary);
          const auto candidateFormals = parseExpectedRootFormalSummary(
              candidate.subtreeExpectedRootFormalSummary);
          for (const auto &KV : existingFormals) {
            auto it = candidateFormals.find(KV.first);
            if (it == candidateFormals.end())
              continue;
            if (KV.second.oldText != it->second.oldText ||
                KV.second.newText != it->second.newText)
              return true;
          }
          return false;
        };

    // Merge a freshly constructed root candidate with an already-tracked
    // structure-preserving callsite patch for the same invocation span. Before
    // any text merge happens, reject incompatible concrete subtree cohorts so a
    // later leaf witness cannot silently rewrite an earlier same-root patch.
    auto mergeCurrentRootWithExistingCallsitePatch =
        [&](MacroPatch &candidate, StringRef label) -> void {
      if (!existingPatch || !existingIsCallsite || baseInvText.empty() ||
          !existingPatch->structurePreserving ||
          existingPatch->proofRootMacroId != m.id)
        return;
      if (candidate.invStart != existingPatch->invStart ||
          candidate.invEnd != existingPatch->invEnd)
        return;
      if (candidate.replacement == existingPatch->replacement)
        return;

      if (conflictingConcreteSubtreeWitnesses(*existingPatch, candidate)) {
        conflictingConcreteSubtreeWitnessForcesWholeCover = true;
        trace("macro/proof",
              "subtree continuity probe: concrete same-root witness conflict "
              "forces whole-cover existing[{0}] candidate[{1}]",
              FormatMacroPatchAudit(*existingPatch),
              FormatMacroPatchAudit(candidate));
        return;
      }

      SmallVector<StringRef, 2> repls;
      repls.push_back(StringRef(candidate.replacement));
      repls.push_back(StringRef(existingPatch->replacement));
      auto merged = mergeCompatibleRootCallsiteReplacements(
          baseInvText, ArrayRef<StringRef>(repls));
      if (!merged || !validateMergedDirectAndDagRootReplacement(
                         baseInvText, StringRef(*merged))) {
        trace("macro/dag",
              "existing callsite patch not merged with {0}: inv id={1} "
              "name='{2}' existing='{3}' current='{4}'",
              label, m.id, m.name,
              stringutils::showWSWithClip(existingPatch->replacement, 160),
              stringutils::showWSWithClip(candidate.replacement, 160));
        if (existingPatch->subtreeCertBacked ||
            candidate.proofKind == MacroPatchProofKind::DagSubtreeRoot) {
          trace("macro/proof",
                "subtree continuity probe: merge-rejected existing[{0}] "
                "candidate[{1}]",
                FormatMacroPatchAudit(*existingPatch),
                FormatMacroPatchAudit(candidate));
        }
        return;
      }

      trace("macro/dag",
            "existing callsite patch merged with {0}: inv id={1} name='{2}' "
            "existing='{3}' current='{4}' merged='{5}'",
            label, m.id, m.name,
            stringutils::showWSWithClip(existingPatch->replacement, 160),
            stringutils::showWSWithClip(candidate.replacement, 160),
            stringutils::showWSWithClip(*merged, 160));
      if (existingPatch->subtreeCertBacked ||
          candidate.proofKind == MacroPatchProofKind::DagSubtreeRoot) {
        trace("macro/proof",
              "subtree continuity probe: merge-accepted existing[{0}] "
              "candidate[{1}]",
              FormatMacroPatchAudit(*existingPatch),
              FormatMacroPatchAudit(candidate));
      }
      candidate.replacement = std::move(*merged);
      if (!candidate.macroId)
        candidate.macroId = existingPatch->macroId;
    };

    // Keep the preferred DAG root replay as an explicit final candidate
    // instead of returning it immediately. The storage for that candidate is
    // owned by the outer final-selection frame so the same accepted DAG root
    // patch can participate in the common selector later in the function.
    auto dag = tryDAGChainedArgsOnly();
    if (dag) {
      bool preferDirectRootCandidate = false;
      if (argsOnlyCandidate && dag->invStart == argsOnlyCandidate->invStart &&
          dag->invEnd == argsOnlyCandidate->invEnd &&
          dag->replacement != argsOnlyCandidate->replacement &&
          !baseInvText.empty()) {
        const bool directValid = validateMergedDirectAndDagRootReplacement(
            baseInvText, StringRef(argsOnlyCandidate->replacement));
        const bool dagValid = validateMergedDirectAndDagRootReplacement(
            baseInvText, StringRef(dag->replacement));

        // Step 12 finalization makes the normalized lattice comparator
        // authoritative for same-root root-level competition. Once both
        // candidates are individually valid, choose the stronger compatible
        // proof class by the explicit lattice law rather than by an ad hoc
        // direct-vs-DAG heuristic.
        if (directValid && dagValid) {
          const bool preferDirect = LatticePrefers(
              argsOnlyCandidate->proofSummary, dag->proofSummary);
          const bool preferDag = LatticePrefers(
              dag->proofSummary, argsOnlyCandidate->proofSummary);
          preferDirectRootCandidate = preferDirect || !preferDag;
          trace(
              "macro/dag",
              "lattice-selected {0} over {1} for same-root root rewrite "
              "competition: root id={2} name='{3}' direct='{4}' dag='{5}'",
              preferDirectRootCandidate ? "direct args-only" : "DAG root",
              preferDirectRootCandidate ? "DAG root" : "direct args-only", m.id,
              m.name,
              stringutils::showWSWithClip(argsOnlyCandidate->replacement, 160),
              stringutils::showWSWithClip(dag->replacement, 160));
        } else {
          trace(
              "macro/dag",
              "DAG args-only preferred over direct args-only: root id={0} "
              "name='{1}' direct='{2}' dag='{3}' directValid={4} dagValid={5}",
              m.id, m.name,
              stringutils::showWSWithClip(argsOnlyCandidate->replacement, 160),
              stringutils::showWSWithClip(dag->replacement, 160),
              directValid ? 1 : 0, dagValid ? 1 : 0);
        }
      } else if (argsOnlyCandidate &&
                 dag->replacement != argsOnlyCandidate->replacement) {
        trace("macro/dag",
              "DAG args-only preferred over direct args-only: root id={0} "
              "name='{1}' direct='{2}' dag='{3}'",
              m.id, m.name,
              stringutils::showWSWithClip(argsOnlyCandidate->replacement, 160),
              stringutils::showWSWithClip(dag->replacement, 160));
      }
      if (!preferDirectRootCandidate) {
        mergeCurrentRootWithExistingCallsitePatch(*dag,
                                                  "dag/direct root rewrite");
        if (!conflictingConcreteSubtreeWitnessForcesWholeCover) {
          // The DAG candidate has already won the local same-root competition
          // against the direct args-only replay. Preserve that decision by
          // carrying only the DAG root candidate into the final selector; the
          // shared selector still arbitrates it against whole-cover and any
          // reusable already-tracked patch for the same invocation span.
          dagRootCandidate = *dag;
          argsOnlyCandidate.reset();
        } else {
          trace(
              "macro/dag",
              "same-root concrete subtree witness conflict suppresses DAG root "
              "replay: root id={0} name='{1}'; falling back to whole-cover "
              "expansion",
              m.id, m.name);
          argsOnlyCandidate.reset();
          reuseExistingCallsitePatch = false;
        }
      }
    }

    if (!dagRootCandidate)
      trace("macro/dag",
            "DAG args-only: no surviving DAG root candidate for root id={0} "
            "name='{1}'; will fall back to direct args-only / existing "
            "callsite / whole-cover replacement as needed",
            m.id, m.name);
    if (directRootPreservationInadmissible) {
      trace("macro/dag",
            "direct args-only/callsite preservation suppressed for root "
            "id={0} name='{1}': touched hunk lies in unsupported descendant "
            "subtree and the root has no direct argument-like replay surface",
            m.id, m.name);
      argsOnlyCandidate.reset();
      reuseExistingCallsitePatch = false;
    }

    // Direct args-only replay is still allowed to compete with an existing
    // structure-preserving patch, but we trace that comparison explicitly
    // because mixed_stringify_and_paste was previously slipping through this
    // path even after the DAG side had already identified a same-root witness
    // conflict.
    if (argsOnlyCandidate && existingPatch && existingIsCallsite &&
        existingPatch->structurePreserving &&
        existingPatch->proofRootMacroId == m.id && !baseInvText.empty() &&
        argsOnlyCandidate->invStart == existingPatch->invStart &&
        argsOnlyCandidate->invEnd == existingPatch->invEnd &&
        argsOnlyCandidate->replacement != existingPatch->replacement) {
      if (existingPatch->subtreeCertBacked) {
        trace("macro/proof",
              "subtree continuity probe: direct root args-only candidate "
              "examined "
              "against existing subtree-backed callsite patch existing[{0}] "
              "candidate[{1}]",
              FormatMacroPatchAudit(*existingPatch),
              FormatMacroPatchAudit(*argsOnlyCandidate));
      }

      SmallVector<StringRef, 2> repls;
      repls.push_back(StringRef(argsOnlyCandidate->replacement));
      repls.push_back(StringRef(existingPatch->replacement));
      auto merged = mergeCompatibleRootCallsiteReplacements(
          baseInvText, ArrayRef<StringRef>(repls));
      if (merged && validateMergedDirectAndDagRootReplacement(
                        baseInvText, StringRef(*merged))) {
        trace("macro/dag",
              "existing callsite patch merged with direct root args-only "
              "rewrite: inv id={0} name='{1}' existing='{2}' current='{3}' "
              "merged='{4}'",
              m.id, m.name,
              stringutils::showWSWithClip(existingPatch->replacement, 160),
              stringutils::showWSWithClip(argsOnlyCandidate->replacement, 160),
              stringutils::showWSWithClip(*merged, 160));
        argsOnlyCandidate->replacement = std::move(*merged);
        if (!argsOnlyCandidate->macroId)
          argsOnlyCandidate->macroId = existingPatch->macroId;
      } else {
        const bool directValid = validateMergedDirectAndDagRootReplacement(
            baseInvText, StringRef(argsOnlyCandidate->replacement));
        trace("macro/dag",
              "existing callsite patch not merged with direct root args-only "
              "rewrite: inv id={0} name='{1}' existing='{2}' current='{3}' "
              "directValid={4}",
              m.id, m.name,
              stringutils::showWSWithClip(existingPatch->replacement, 160),
              stringutils::showWSWithClip(argsOnlyCandidate->replacement, 160),
              directValid ? 1 : 0);
        if (!directValid) {
          trace(
              "macro/dag",
              "discarding invalid direct root args-only rewrite in favor of "
              "existing callsite patch: inv id={0} name='{1}' existing='{2}' "
              "current='{3}'",
              m.id, m.name,
              stringutils::showWSWithClip(existingPatch->replacement, 160),
              stringutils::showWSWithClip(argsOnlyCandidate->replacement, 160));
          argsOnlyCandidate.reset();
          reuseExistingCallsitePatch = true;
        } else {
          const bool preferDirect = LatticePrefers(
              argsOnlyCandidate->proofSummary, existingPatch->proofSummary);
          const bool preferExisting = LatticePrefers(
              existingPatch->proofSummary, argsOnlyCandidate->proofSummary);
          if (preferExisting && !preferDirect) {
            trace("macro/dag",
                  "lattice-selected existing callsite patch over direct root "
                  "args-only rewrite after merge rejection: inv id={0} "
                  "name='{1}' existing='{2}' current='{3}'",
                  m.id, m.name,
                  stringutils::showWSWithClip(existingPatch->replacement, 160),
                  stringutils::showWSWithClip(argsOnlyCandidate->replacement,
                                              160));
            argsOnlyCandidate.reset();
            reuseExistingCallsitePatch = true;
          }
        }
      }
    }
  }

  // Step 1 removes the last direct macro-selector bypass by letting the final
  // macro candidate competition handle nested preserving artifacts too. The
  // one extra local rule is that non-top-level construction sites may admit
  // selector-only nested macro carriers whose only failed obligation is the
  // top-level proof-root requirement. Step 2 then restamps any such selected
  // emitted artifact onto an emission-discharged carrier before it reaches the
  // byte-edit boundary.
  const bool allowNonTopLevelMacroSelectorFailure =
      GetRootMacroId(m.id) != m.id;

  const bool canReuseExistingCallsiteNoOp =
      reuseExistingCallsitePatch &&
      !conflictingConcreteSubtreeWitnessForcesWholeCover && existingPatch;
  const bool canReuseExistingCallsiteSkipWholeCover =
      !canReuseExistingCallsiteNoOp &&
      !conflictingConcreteSubtreeWitnessForcesWholeCover && existingPatch &&
      existingIsCallsite && existingPatch->structurePreserving &&
      existingPatch->proofRootMacroId == m.id && !baseInvText.empty() &&
      InvocationSpanMatchesCallsitePrefix(baseInvText, m);

  std::optional<WholeCoverPlan> wholeCoverPlan;
  bool canReuseExistingExpanded = false;
  if (existingExpandedPatch && !existingExpandedPatch->structurePreserving &&
      MacroPatchOwnerMatches(*existingExpandedPatch, currentPatchOwner)) {
    if (existingExpandedPatch->proofRootMacroId == m.id) {
      if (existingExpandedPatch->proofKind ==
              MacroPatchProofKind::CounterLiteral &&
          m.name == "__COUNTER__") {
        canReuseExistingExpanded = true;
      } else if (existingExpandedPatch->proofKind ==
                 MacroPatchProofKind::WholeCoverRealization) {
        wholeCoverPlan = ComputeWholeCoverPlan(m);
        if (wholeCoverPlan)
          canReuseExistingExpanded = WholeCoverPatchMatchesPlan(
              *existingExpandedPatch, *wholeCoverPlan, m.id);
      }
    }
  }

  if (!wholeCoverPlan)
    wholeCoverPlan = ComputeWholeCoverPlan(m);

  enum class FinalMacroCandidateOrigin : uint8_t {
    DirectArgsOnly,
    DagRootReplay,
    ReuseExistingCallsiteNoOp,
    ReuseExistingCallsiteSkipWholeCover,
    ReuseExistingExpanded,
    WholeCoverRealization,
  };

  struct FinalMacroCandidate {
    MacroPatch patch;
    AcceptedResultCandidate acceptedCandidate;
    FinalMacroCandidateOrigin origin =
        FinalMacroCandidateOrigin::WholeCoverRealization;
  };

  auto formatFinalMacroCandidateOrigin =
      [](FinalMacroCandidateOrigin origin) -> StringRef {
    switch (origin) {
    case FinalMacroCandidateOrigin::DirectArgsOnly:
      return "direct-args-only";
    case FinalMacroCandidateOrigin::DagRootReplay:
      return "dag-root-replay";
    case FinalMacroCandidateOrigin::ReuseExistingCallsiteNoOp:
      return "reuse-existing-callsite-no-op";
    case FinalMacroCandidateOrigin::ReuseExistingCallsiteSkipWholeCover:
      return "reuse-existing-callsite-skip-whole-cover";
    case FinalMacroCandidateOrigin::ReuseExistingExpanded:
      return "reuse-existing-expanded";
    case FinalMacroCandidateOrigin::WholeCoverRealization:
      return "whole-cover-realization";
    }
    return "unknown";
  };

  // Patch B moved the final macro-return site onto explicit accepted
  // candidates rather than a chain of early returns. Patch C keeps the
  // existing candidate discovery logic above and still uses proof discharge as
  // the default participation gate. Step 1 adds the one scoped exception
  // needed to remove the final direct bypass: non-top-level macro
  // construction sites may also admit selector-only nested preserving
  // artifacts whose sole failed obligation is the top-level proof-root rule.
  SmallVector<FinalMacroCandidate, 5> finalMacroCandidates;
  auto addFinalMacroCandidate = [&](const MacroPatch &patch,
                                   FinalMacroCandidateOrigin origin) {
    FinalMacroCandidate entry;
    entry.patch = patch;
    entry.acceptedCandidate = BuildAcceptedMacroCandidate(entry.patch);
    entry.origin = origin;
    trace("macro/proof",
          "final macro candidate: inv id={0} name={1} origin={2} {3}",
          m.id, m.name, formatFinalMacroCandidateOrigin(origin),
          FormatAcceptedResultCandidate(entry.acceptedCandidate));
    finalMacroCandidates.push_back(std::move(entry));
  };

  if (argsOnlyCandidate)
    addFinalMacroCandidate(*argsOnlyCandidate,
                           FinalMacroCandidateOrigin::DirectArgsOnly);

  if (dagRootCandidate)
    addFinalMacroCandidate(*dagRootCandidate,
                           FinalMacroCandidateOrigin::DagRootReplay);

  if (canReuseExistingCallsiteNoOp) {
    addFinalMacroCandidate(
        *existingPatch, FinalMacroCandidateOrigin::ReuseExistingCallsiteNoOp);
  }

  if (canReuseExistingCallsiteSkipWholeCover) {
    addFinalMacroCandidate(
        *existingPatch,
        FinalMacroCandidateOrigin::ReuseExistingCallsiteSkipWholeCover);
  }

  if (canReuseExistingExpanded) {
    addFinalMacroCandidate(*existingExpandedPatch,
                           FinalMacroCandidateOrigin::ReuseExistingExpanded);
  }

  if (wholeCoverPlan) {
    MacroPatch patch{*invStart, *invEnd, wholeCoverPlan->clippedText, m.id};
    StampMacroWholeCoverRealizationPatch(patch, *wholeCoverPlan, m.id);
    addFinalMacroCandidate(patch,
                           FinalMacroCandidateOrigin::WholeCoverRealization);
  }

  if (finalMacroCandidates.empty())
    return std::nullopt;

  SmallVector<AcceptedResultCandidate, 5> acceptedCandidates;
  acceptedCandidates.reserve(finalMacroCandidates.size());
  for (const FinalMacroCandidate &candidate : finalMacroCandidates)
    acceptedCandidates.push_back(candidate.acceptedCandidate);

  const std::optional<size_t> selectedIdx =
      SelectPreferredAcceptedResultCandidateIndex(
          acceptedCandidates, allowNonTopLevelMacroSelectorFailure);
  if (!selectedIdx) {
    trace(
        "macro/proof",
        "no final macro candidate survived proof-discharge gating: inv id={0} "
        "name={1} candidates={2} allowNestedSelectorOnly={3}",
        m.id, m.name, finalMacroCandidates.size(),
        allowNonTopLevelMacroSelectorFailure ? 1 : 0);
    return std::nullopt;
  }

  const FinalMacroCandidate &selected = finalMacroCandidates[*selectedIdx];
  trace("macro/proof",
        "lattice-selected final macro candidate: inv id={0} name={1} "
        "origin={2} {3}",
        m.id, m.name, formatFinalMacroCandidateOrigin(selected.origin),
        FormatAcceptedResultCandidate(selected.acceptedCandidate));

  switch (selected.origin) {
  case FinalMacroCandidateOrigin::DirectArgsOnly:
    trace("macro/proof",
          "returning direct args-only candidate: inv id={0} name={1} {2}",
          m.id, m.name,
          FormatAcceptedResultCandidate(selected.acceptedCandidate));
    break;

  case FinalMacroCandidateOrigin::DagRootReplay:
    trace("macro/proof",
          "returning DAG root replay candidate through final selector: inv "
          "id={0} name={1} {2}",
          m.id, m.name,
          FormatAcceptedResultCandidate(selected.acceptedCandidate));
    break;

  case FinalMacroCandidateOrigin::ReuseExistingCallsiteNoOp:
    trace("macro", "callsite patch reused (args-only no-op) inv id={0}",
          m.id);
    trace("macro/proof",
          "reused existing callsite patch audit: inv id={0} name={1} {2}",
          m.id, m.name,
          FormatAcceptedResultCandidate(selected.acceptedCandidate));
    if (selected.patch.subtreeCertBacked)
      trace("macro/proof",
            "subtree continuity probe: reused subtree-backed callsite patch "
            "without a fresh subtree winner in this pass inv id={0} name={1}",
            m.id, m.name);
    break;

  case FinalMacroCandidateOrigin::ReuseExistingCallsiteSkipWholeCover:
    trace("macro",
          "callsite patch reused (skip whole-cover expansion) inv id={0}",
          m.id);
    trace("macro/proof",
          "reused existing callsite patch audit: inv id={0} name={1} {2}",
          m.id, m.name,
          FormatAcceptedResultCandidate(selected.acceptedCandidate));
    if (selected.patch.subtreeCertBacked) {
      trace("macro/proof",
            "subtree continuity probe: reused subtree-backed callsite patch "
            "from skip-whole-cover path without a fresh subtree winner inv "
            "id={0} "
            "name={1}",
            m.id, m.name);
    }
    break;

  case FinalMacroCandidateOrigin::ReuseExistingExpanded:
    trace("macro",
          "expanded patch reused after preservation attempts failed inv id={0}",
          m.id);
    trace("macro/proof",
          "reused existing expanded patch audit: inv id={0} name={1} {2}",
          m.id, m.name,
          FormatAcceptedResultCandidate(selected.acceptedCandidate));
    break;

  case FinalMacroCandidateOrigin::WholeCoverRealization:
    trace("macro/proof",
          "constructed whole-cover realization patch audit: inv id={0} name={1} {2}",
          m.id, m.name,
          FormatAcceptedResultCandidate(selected.acceptedCandidate));
    break;
  }

  return selected.patch;
}

// Implementation extracted verbatim to keep `RefoldEngine.cpp`
// physically smaller without changing ownership or semantics.
#include "RefoldEngine.TailUtilities.inc"

} // namespace refold
} // namespace clang
