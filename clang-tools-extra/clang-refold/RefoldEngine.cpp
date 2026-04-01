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
#include "clang/Frontend/CompilerInvocation.h"
#include "clang/Lex/Lexer.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/Hashing.h"
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
#include <cstdint>
#include <filesystem>
#include <functional>
#include <limits>
#include <memory>
#include <tuple>

using namespace llvm;

namespace clang {
namespace refold {

namespace {
inline std::string resolveHeaderPath(const RefoldModel::IncludeItem &inc) {
  return (inc.resolvedPath && !inc.resolvedPath->empty())
             ? inc.resolvedPath->str()
             : stringutils::stripHeaderToken(inc.target).str();
}

inline bool HasLiteralMacroCalleeOrigin(
    const RefoldModel::MacroInvocation &M) {
  return M.calleeOrigin.kind == MacroCalleeOriginKind::LiteralMacroName;
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

static PasteRunInvertibilityCertificate
buildAdjacentPasteRunInvertibilityCertificate(
    StringRef aTok, StringRef bRun,
    ArrayRef<const RefoldModel::PPArgSpan *> runSpans) {
  PasteRunInvertibilityCertificate cert;
  if (runSpans.empty())
    return cert;

  std::vector<std::string> oldSegs;
  oldSegs.reserve(runSpans.size());
  std::string oldRun;
  oldRun.reserve(aTok.size());

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
    oldRun += oldSeg;
    prevEnd = *ps->byteEnd;
  }

  SmallVector<std::vector<std::string>, 4> candidates;
  auto addCandidate = [&](std::vector<std::string> segs) {
    for (const auto &existing : candidates)
      if (existing == segs)
        return;
    candidates.push_back(std::move(segs));
  };

  // A contiguous ## run is invertible when its edited B spelling can be split
  // into per-segment spellings such that every changed segment is isolated by
  // unchanged neighbors (or a run edge). Adjacent edited segments in the same
  // undelimited run are treated as ambiguous because no internal anchor pins
  // their boundary.
  SmallVector<std::string, 8> curSegs(oldSegs.begin(), oldSegs.end());
  auto solveRun = [&](auto &&self, size_t idx, size_t pos,
                      bool prevChanged) -> void {
    if (candidates.size() > 1 || pos > bRun.size())
      return;

    if (idx == oldSegs.size()) {
      if (pos == bRun.size())
        addCandidate(std::vector<std::string>(curSegs.begin(), curSegs.end()));
      return;
    }

    StringRef oldSeg = oldSegs[idx];

    auto tryUnchanged = [&]() {
      if (!bRun.drop_front(pos).starts_with(oldSeg))
        return;
      curSegs[idx] = oldSeg;
      self(self, idx + 1, pos + oldSeg.size(), /*prevChanged=*/false);
    };

    if (prevChanged) {
      tryUnchanged();
      return;
    }

    tryUnchanged();

    // Current segment edited. The next segment, if any, must remain unchanged
    // and therefore acts as the first available anchor for the edited piece.
    if (idx + 1 == oldSegs.size()) {
      curSegs[idx] = bRun.drop_front(pos).str();
      self(self, idx + 1, bRun.size(), /*prevChanged=*/true);
      return;
    }

    StringRef nextAnchor = oldSegs[idx + 1];
    if (nextAnchor.empty()) {
      cert.kind = PasteRunInvertibilityKind::Unsupported;
      return;
    }

    StringRef rest = bRun.drop_front(pos);
    for (size_t searchPos = 0;; ++searchPos) {
      size_t found = rest.find(nextAnchor, searchPos);
      if (found == StringRef::npos)
        break;
      curSegs[idx] = rest.take_front(found).str();
      self(self, idx + 1, pos + found, /*prevChanged=*/true);
      if (candidates.size() > 1)
        return;
    }
  };

  solveRun(solveRun, 0, 0, /*prevChanged=*/false);

  if (cert.kind == PasteRunInvertibilityKind::Unsupported)
    return cert;
  if (candidates.empty()) {
    cert.kind = PasteRunInvertibilityKind::NoMatch;
    return cert;
  }
  if (candidates.size() > 1) {
    cert.kind = PasteRunInvertibilityKind::Ambiguous;
    return cert;
  }

  cert.kind = PasteRunInvertibilityKind::Unique;
  cert.derivedSegs = std::move(candidates.front());
  return cert;
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
  DiagnosticOptions diagOpts;
  IntrusiveRefCntPtr<DiagnosticIDs> diagIDs(new DiagnosticIDs());
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

void RefoldEngine::RequestEscalation(StringRef phase, StringRef detail) const {
  escalationRequested_ = true;
  if (escalationReasons_.size() < 64) {
    escalationReasons_.push_back(
        llvm::formatv("{0}: {1}", phase, detail).str());
  }
  debug("escalate", "REQUEST escalation: {0}: {1}", phase, detail);
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
  // Multi-tier escalation ladder: retry refolding under increasingly
  // conservative (more-expanded) policies before falling back to emitting B.
  //
  //   tier 0: normal structural refold
  //   tier 1: force whole-cover macro replacement
  //   tier 2: force inlining touched includes from B slices
  //
  // If escalation is still requested after tier 2, emit the fully expanded
  // edited preprocessed stream (B).
  constexpr unsigned kMaxTier = 2;
  for (unsigned tier = 0; tier <= kMaxTier; ++tier) {
    escalationTier_ = tier;
    ResetEscalationState();
    ResetAttemptStats();

    if (tier != 0)
      debug("escalate", "ESCALATION tier={0}: retrying refold", tier);

    std::string out = RefoldOnce();
    if (!escalationRequested_) {
      EmitRefoldStats();
      return out;
    }

    debug("escalate", "ESCALATION tier={0}: requested; advancing. reasons={1}",
          tier, escalationReasons_.size());
    for (const auto &r : escalationReasons_)
      debug("escalate", "  {0}", r);
  }

  debug("escalate",
        "ESCALATION terminal: emitting fully expanded edited preprocessed "
        "stream (B). reasons={0}", escalationReasons_.size());
  for (const auto &r : escalationReasons_)
    debug("escalate", "  {0}", r);
  lastStats_ = RefoldStats{};
  lastStats_.totalIncludes = model_.GetIncludes().size();
  lastStats_.expandedIncludes = lastStats_.totalIncludes;
  for (const auto &mi : model_.GetMacroInvocations()) {
    if (!mi.callerMacroId)
      ++lastStats_.totalMacros;
  }
  lastStats_.expandedMacros = lastStats_.totalMacros;
  lastStats_.tier = kMaxTier + 1;
  lastStats_.terminalFallbackToB = true;
  EmitRefoldStats();
  return bSource_.str();
}

std::string RefoldEngine::RefoldOnce() {
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

  // 2) LCS over tokens (A → B) with owner-aware cost model. and dump A → B
  // map.
  auto a2b = diffutils::lcsMapAB(aSeq, bSeq, ownerDepthGap_);
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


  // Cache the token-level hunks for boundary-aware envelope mapping.
  abTokHunks_ = hunks;

  // Build provenance for token-level pure insertions (B-only hunks) and
  // pre-claim standalone insertions before macro patching so whole-cover
  // replacements can deterministically avoid double-emitting insertion
  // payloads.
  BuildBInsertionProvenance(hunks);
  PreclaimStandaloneInsertions(tuPath, hunks);

  // Build *raw-text* byte hunks once; this enables deterministic mapping of PP
  // byte spans from A->B, without inheriting any ambiguity from token-level
  // alignment.
  //
  // This is critical for "insert-only" edits, where token-only LCS diffing can
  // place the insertion at an arbitrary stable point, corrupting subsequent
  // A->B byte span mapping.
  abByteHunks_ = BuildByteHunksFromRawText();

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
        for (const RefoldModel::MacroInvocation *target = m; target != nullptr;) {
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

          // If found, use the existing replacement; otherwise, use the original
          // text.
          std::string currentInvText =
              (existingIt != byMacroId.end())
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
            if (hadExistingCallsitePatch) {
              if (prevCallsiteReplacement == updated->replacement)
                trace("macro", "callsite patch reused inv id={0}", target->id);
              else
                trace("macro", "callsite patch overwritten inv id={0}",
                      target->id);
            }
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
        if (replacingGap && !repl.empty() && !stringutils::isWs(repl.front()))
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
        tuEdits.push_back(TextEdit{span->first, span->second,
                                   std::move(ro.text), std::move(ro.pending)});
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
    // Deterministic behavior:
    //   * strict mode: fatal (schema deficiency or segment-construction bug)
    //   * non-strict: do not realize includes; attempt TU-only edit if a TU
    //     byte span exists
    if (strict_) {
      fatal("hunks",
            "owner unresolved for changed A-interval [{0},{1}) (segments did "
            "not classify; include guessing disabled)",
            h.aStart, h.aEnd);
    }

    debug("classify",
          "#{0} owner unresolved (not TU/macro/segment). Conservative TU-only "
          "attempt (no include realization).",
          i);

    if (auto span = TUByteSpan(h.aStart, h.aEnd, tuPath)) { // [b, e)
      std::string repl;
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

      // This patch inserts B text at a zero-width TU site: the TU span is empty,
      // but the hunk contributes one or more B tokens. Token-envelope byte ranges
      // begin at the first inserted token, so they do not include any spaces or
      // tabs that appear immediately before that token in B on the same line.
      // Preserve those preceding spaces/tabs when forming the inserted text,
      // unless equivalent spacing is already present immediately to the left of
      // the insertion point in the TU.
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
        if (replacingGap) {
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
      if (replacingGap && !repl.empty() && !stringutils::isWs(repl.front()))
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
      tuEdits.push_back(TextEdit{span->first, span->second, std::move(ro.text),
                                 std::move(ro.pending)});
      continue;
    }

    debug("classify",
          "#{0} dropping edit {1}: owner unresolved and no TU byte span "
          "available (no include guessing).",
          i, h);
    RequestEscalation(
        "classify",
        llvm::formatv("dropped edit #{0} (owner unresolved, no TU byte span)",
                      i)
            .str());
    continue;
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
                                &appliedExpandedMacroRootIds);
  }

  // Determine which include ids should count as "expanded" for this refold.
  // If forced inlining of touched includes from B is enabled, start from the
  // includes that have explicit expansion materialization and close that set
  // downward through the include tree so all nested children are marked
  // expanded too. Otherwise, count only the includes that appear directly in
  // includeExpansion. Record the final expanded-include count in stats.
  DenseSet<uint64_t> expandedIncludeIds;
  if (ForceInlineTouchedIncludesFromB()) {
    SmallVector<uint64_t, 32> stack;
    for (const auto &kv : includeExpansion)
      stack.push_back(kv.first);
    while (!stack.empty()) {
      const uint64_t id = stack.pop_back_val();
      if (!expandedIncludeIds.insert(id).second)
        continue;
      auto childIt = children.find(id);
      if (childIt == children.end())
        continue;
      for (const RefoldModel::IncludeItem *child : childIt->second)
        stack.push_back(child->id);
    }
  } else {
    for (const auto &kv : includeExpansion)
      expandedIncludeIds.insert(kv.first);
  }
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
        tuEdits.push_back(TextEdit{
            mp.invStart, mpEnd, std::move(ro.text), std::move(ro.pending),
            MacroPatchRemainsExpanded(mp)
                ? std::make_optional(GetRootMacroId(mp.macroId))
                : std::nullopt});
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
      tuEdits.push_back(
          TextEdit{siteB, siteE, std::move(wrapped), std::nullopt});
    }
  }

  // Apply TU edits in descending order of start offset.
  debug("tu/apply", "applying {0} TU edits", tuEdits.size());
  std::string tuResult = ApplyTextEditsWithPendingResync(
      tuBytes, tuEdits, &appliedExpandedMacroRootIds);

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

  // Note: escalation handling is performed by the outer ladder in Refold().
  if (ForceInlineTouchedIncludesFromB()) {
    for (const auto &mi : model_.GetMacroInvocations()) {
      if (mi.ownerIncludeId &&
          expandedIncludeIds.find(*mi.ownerIncludeId) != expandedIncludeIds.end())
        appliedExpandedMacroRootIds.insert(GetRootMacroId(mi.id));
    }
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

// ============================= Boundary helpers ==============================

namespace {
struct LexBoundaryToken {
  tok::TokenKind Kind = tok::unknown;
  std::string Spelling;
};

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
    Out.push_back(
        {Tok.getKind(), std::string(Text.substr(Off, Tok.getLength()))});
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

  // NOTE: MacroDirective.siteB/siteE typically covers only the first physical
  // line of the #define directive. For multi-line macro definitions ("\\\n"
  // line splices), invocations spelled in the macro body may appear after
  // siteE. We therefore compute the actual directive extent by scanning the
  // source text until we reach a newline that is NOT line-spliced.
  //
  // NOTE: This predicate may be queried extremely frequently during macro
  // selection. Avoid O(Ninvocations*Ndirectives) behavior by indexing all
  // #define directive extents once per process, keyed by absolute file path.
  //
  // IMPORTANT: intentionally avoid RefoldEngine::PathsEqual() here. The
  // canonicalization it performs can be expensive in tight loops and can
  // dominate runtime for large preprocessed streams.

  struct DefineExtent {
    uint64_t b;
    uint64_t e;
  };

  // Cache file text by absolute path to avoid repeated disk reads.
  static llvm::StringMap<std::string> fileTextCache;
  // Cache computed end offsets per directive id.
  static llvm::DenseMap<uint64_t, uint64_t> defineEndCache;
  // Index define extents per absolute path (built lazily).
  static llvm::StringMap<std::vector<DefineExtent>> definesByAbsPath;
  static bool definesIndexBuilt = false;

  if (!definesIndexBuilt) {
    definesIndexBuilt = true;

    // Build a lexical containment index for `#define` directives, keyed by the
    // directive's absolute source path. For each producer-reported `#define`,
    // widen its recorded end to the true physical end of the directive by
    // following any `\\\n` line-spliced continuation lines, then record the
    // resulting byte range `[siteB, defineEnd)` in `definesByAbsPath`.
    //
    // This is used by IsInvocationInsideDefineDirective(): a macro invocation
    // is considered "inside a define" if its spelled byte range falls within
    // one of these per-file define extents.
    //
    // To keep repeated queries cheap, we cache:
    //   - the loaded source text for each file (`fileTextCache`)
    //   - the computed widened end for each directive (`defineEndCache`)
    for (const auto &d : model_.GetMacroDirectives()) {
      if ("#define" != d.subkind)
        continue;
      if (d.sitePath.empty())
        continue;

      // Compute (or fetch) the true end of the #define directive in its source
      // file, including any "\\\n" line-spliced continuation lines.
      uint64_t defineEnd = d.siteE;
      auto itEnd = defineEndCache.find(d.id);
      if (itEnd != defineEndCache.end()) {
        defineEnd = itEnd->second;
      } else {
        std::string absPath = lineDirs_.ToAbsolutePath(d.sitePath);
        auto itTxt = fileTextCache.find(absPath);
        if (itTxt == fileTextCache.end()) {
          auto bufOrErr = llvm::MemoryBuffer::getFile(absPath);
          if (!bufOrErr) {
            // Best-effort: fall back to the producer-provided site range.
            defineEndCache[d.id] = d.siteE;
            defineEnd = d.siteE;
          } else {
            fileTextCache[absPath] = (**bufOrErr).getBuffer().str();
            itTxt = fileTextCache.find(absPath);
          }
        }

        if (itTxt != fileTextCache.end()) {
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
            // Advance past the newline.
            i = static_cast<uint64_t>(nl + 1);
            if (!stringutils::isLineSplice(bytes, nl))
              break;
          }

          defineEnd = i;
          defineEndCache[d.id] = defineEnd;
        }
      }

      const std::string absPath = lineDirs_.ToAbsolutePath(d.sitePath);
      definesByAbsPath[absPath].push_back(DefineExtent{d.siteB, defineEnd});
    }

    // Sort extents by begin offset for binary-search probing.
    for (auto &kv : definesByAbsPath) {
      auto &vec = kv.getValue();
      llvm::sort(vec, [](const DefineExtent &x, const DefineExtent &y) {
        if (x.b != y.b)
          return x.b < y.b;
        return x.e < y.e;
      });
    }
  }

  // Normalize the invocation file spelling the same way we keyed the define
  // index, then look up all #define extents recorded for that file.
  const std::string invAbs = lineDirs_.ToAbsolutePath(*m.invFile);
  auto it = definesByAbsPath.find(invAbs);
  if (it == definesByAbsPath.end())
    return false;

  // Use the invocation's start byte as the lexical containment probe.
  const uint64_t x = *m.invB;
  const auto &vec = it->second;
  if (vec.empty())
    return false;

  // Binary-search for the last define extent whose begin offset is <= x.
  // The extents are sorted by begin offset, so this gives the only candidate
  // that can still contain the probe byte.
  size_t lo = 0, hi = vec.size();
  while (lo < hi) {
    size_t mid = lo + (hi - lo) / 2;
    if (vec[mid].b <= x)
      lo = mid + 1;
    else
      hi = mid;
  }
  if (lo == 0)
    return false;

  // Containment is half-open: [b,e). If the invocation start byte falls inside
  // the candidate define extent, treat the invocation as lexically inside that
  // #define directive.
  const DefineExtent cand = vec[lo - 1];
  if (x >= cand.b && x < cand.e)
    return true;

  // Conservative fallback: in unusual cases where extents overlap, linearly
  // scan a handful of adjacent ranges.
  for (size_t i = lo; i < vec.size() && i < lo + 4; ++i) {
    if (x >= vec[i].b && x < vec[i].e)
      return true;
  }
  for (size_t i = lo; i > 0 && i + 4 > lo; --i) {
    if (x >= vec[i - 1].b && x < vec[i - 1].e)
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
RefoldEngine::FindProvableTUInsertionAnchor(uint64_t pp,
                                            StringRef tuPath) const {
  // First prefer an exact structural slot anchor recorded by the producer.
  // These anchors are the strongest evidence because they identify a specific
  // TU byte boundary corresponding to this PP gap.
  if (auto slotAnchor = AnchorToExactSlotBoundaryFromPPGap(tuPath, pp)) {
    trace("hunk",
          "    insertion gap PP={0} mapsToTU via slot boundary TU byte {1}", pp,
          slotAnchor);
    return slotAnchor;
  }

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

    trace("tu/anchor",
          "pure insertion arg-like begin anchor: ppGap={0} -> macro id={1} "
          "name='{2}' invB={3}",
          pp, best->id, best->name, *best->invB);
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
      if (PathsEqual(tuPath, right.file))
        return right.b;
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
      if (PathsEqual(tuPath, left.file))
        return left.e;
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
  if (dRight <= dLeft)
    return right->b;
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

std::optional<uint64_t>
RefoldEngine::AnchorToExactSlotBoundaryFromPPGap(StringRef tuPath,
                                                   uint64_t ppGap) const {
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
    trace("slots/anchor",
          "AnchorToExactSlotBoundaryFromPPGap: ppGap={0} -> slotId={1} "
          "kind={2} tuByte={3}",
          ppGap, best->slot->id, best->slot->kind, best->b);
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

bool RefoldEngine::MacroExpansionEnvelopeB(
    const RefoldModel::MacroInvocation &m, bool onlyInvFile, uint64_t &begin,
    uint64_t &end) const {
  const auto &tokMapByPP = model_.GetTokmapByPP();
  if (tokMapByPP.empty())
    return false;

  uint64_t lo = std::numeric_limits<uint64_t>::max();
  uint64_t hi = 0;
  bool any = false;

  // Expand the PP envelope with mapped tokens from [l,h), optionally restricted
  // to tokens that resolve to the invocation's source file.
  auto addRange = [&](uint64_t l, uint64_t h) {
    for (uint64_t pp = l; pp < h; ++pp) {
      auto it = tokMapByPP.find(pp);
      if (it == tokMapByPP.end())
        continue;

      const auto &t = it->second;

      if (onlyInvFile) {
        if (!m.invFile || !PathsEqual(t.file, *m.invFile))
          continue;
      }

      if (pp < lo)
        lo = pp;
      if (pp + 1 > hi)
        hi = pp + 1;
      any = true;
    }
  };

  // BODY spans
  for (const auto &s : m.bodySpans) {
    if (!s.IsValid())
      continue;
    addRange(s.begin, s.end);
  }

  // ARG spans
  for (const auto &s : m.argSpans) {
    if (!s.IsValid())
      continue;
    addRange(s.begin, s.end);
  }

  if (!any)
    return false;

  begin = lo;
  end = hi;
  return true;
}

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
  // right-hand separator owner takes precedence and this span must not claim the
  // insertion.
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
    bool checkPasteSpans) const {
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

  if (!hasAny) {
    for (const auto &s : m.pasteSpans) {
      if (s.argIdx == argIdx) {
        hasAny = true;
        break;
      }
    }
  }

  if (!hasAny)
    return true;

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

  // Strip trailing newlines to stabilize within-token diffs.
  StringRef aTok = stringutils::stripTrailingNewlines(aTokRaw);
  StringRef bTok = stringutils::stripTrailingNewlines(bTokRaw);

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

  // Initialize the output vector with empty strings.
  std::vector<std::string> out(spansAsc.size());

  // Call the recursive worker.
  if (!SegmentPastedTokenArgsByFixedSlicesRec(aTok, bTok, spansAsc,
                                              /*idx*/ 0, /*posA*/ 0,
                                              /*posB*/ 0, out)) {
    return std::nullopt;
  }

  return out;
}

bool RefoldEngine::SegmentPastedTokenArgsByFixedSlicesRec(
    StringRef aTok, StringRef bTok,
    ArrayRef<const RefoldModel::PPArgSpan *> spansAsc, size_t idx, size_t posA,
    size_t posB, MutableArrayRef<std::string> out) {
  // 1. Base Case: All spans processed
  if (idx >= spansAsc.size()) {
    // All spans emitted; remaining fixed tail must match exactly.
    StringRef tail = aTok.substr(posA);
    return bTok.substr(posB) == tail;
  }

  const auto *ps = spansAsc[idx];

  // 2. Validate Optionals and Range Consistency
  if (!ps->byteBegin || !ps->byteEnd || *ps->byteEnd < *ps->byteBegin)
    return false;

  const size_t bA = static_cast<size_t>(*ps->byteBegin);
  const size_t eA = static_cast<size_t>(*ps->byteEnd);

  // Ensure current span doesn't overlap backwards into previously processed A
  // text
  if (bA < posA)
    return false;

  // 3. Match the fixed text that appears before this pasted-argument run.
  StringRef fixedBefore = aTok.substr(posA, bA - posA);
  if (!bTok.substr(posB).starts_with(fixedBefore))
    return false;

  const size_t runStartB = posB + fixedBefore.size();

  // 4. Determine the maximal adjacent-span run beginning at idx. Internal
  // empty gaps correspond to touching pasted contributions; they remain
  // invertible only while unchanged neighboring segments still pin the
  // boundaries.
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
    if (!cur->byteEnd || !next->byteBegin || *next->byteBegin < *cur->byteEnd)
      return false;

    StringRef gap =
        aTok.substr(static_cast<size_t>(*cur->byteEnd),
                    static_cast<size_t>(*next->byteBegin - *cur->byteEnd));
    if (!gap.empty()) {
      fixedAfter = gap;
      break;
    }

    ++runEnd;
    if (!spansAsc[runEnd]->byteEnd)
      return false;
    nextPosA = static_cast<size_t>(*spansAsc[runEnd]->byteEnd);
  }

  auto tryRun = [&](size_t runEndB) -> bool {
    if (runEndB < runStartB || runEndB > bTok.size())
      return false;

    auto cert = buildAdjacentPasteRunInvertibilityCertificate(
        aTok, bTok.substr(runStartB, runEndB - runStartB),
        spansAsc.slice(idx, runEnd - idx + 1));
    if (cert.kind == PasteRunInvertibilityKind::Unique &&
        cert.derivedSegs.size() == runEnd - idx + 1) {
      for (size_t i = 0; i < cert.derivedSegs.size(); ++i)
        out[idx + i] = cert.derivedSegs[i];

      return SegmentPastedTokenArgsByFixedSlicesRec(aTok, bTok, spansAsc,
                                                    runEnd + 1, nextPosA,
                                                    runEndB, out);
    }

    // Legacy single-span fallback: when the current run contains only one
    // argument contribution, the fixed slices on either side already pin the
    // segment boundary. The newer adjacent-run certificate is stricter, but
    // some pure-paste cases (e.g. CONCAT-style token assembly) are still
    // structurally invertible via this simpler anchor-based split.
    if (runEnd == idx) {
      out[idx] = bTok.substr(runStartB, runEndB - runStartB).str();
      return SegmentPastedTokenArgsByFixedSlicesRec(aTok, bTok, spansAsc,
                                                    idx + 1, nextPosA,
                                                    runEndB, out);
    }

    return false;
  };

  // 5. Match the fixed anchor after the whole touching run. With no tail, the
  // run consumes the remainder of the B token.
  if (fixedAfter.empty())
    return tryRun(bTok.size());

  size_t k = bTok.find(fixedAfter, runStartB);
  while (k != StringRef::npos) {
    if (tryRun(k))
      return true;
    k = bTok.find(fixedAfter, k + 1);
  }

  return false;
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

  auto collectDenseMapKeys = [&](const auto &mapLike) {
    SmallVector<uint32_t, 8> keys;
    keys.reserve(mapLike.size());
    for (const auto &KV : mapLike)
      keys.push_back(KV.first);
    llvm::sort(keys);
    return keys;
  };

  auto collectSpanArgIdxs = [&](const std::vector<RefoldModel::PPArgSpan> &spans) {
    SmallVector<uint32_t, 8> argIdxs;
    for (const auto &ps : spans) {
      if (!llvm::is_contained(argIdxs, ps.argIdx))
        argIdxs.push_back(ps.argIdx);
    }
    llvm::sort(argIdxs);
    return argIdxs;
  };

  auto computeMissingSupportArgs = [&](ArrayRef<uint32_t> carried,
                                      ArrayRef<uint32_t> direct) {
    SmallVector<uint32_t, 8> missing;
    for (uint32_t argIdx : carried) {
      if (!llvm::is_contained(direct, argIdx))
        missing.push_back(argIdx);
    }
    llvm::sort(missing);
    return missing;
  };

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

    SmallVector<uint32_t, 8> carriedArgIdxs = collectDenseMapKeys(replByArgIdx);
    SmallVector<uint32_t, 8> directPasteArgIdxs = collectSpanArgIdxs(spans);
    SmallVector<uint32_t, 8> carriedButDirectMissingArgIdxs =
        computeMissingSupportArgs(carriedArgIdxs, directPasteArgIdxs);
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
            trace("macro/paste",
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
                " parent-level paste span has no standard occurrence bytes: inv id={0} "
                "name={1} argIdx={2} tokRange=[{3},{4}) ppBytes=[{5},{6})",
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
                "baseArg='{2}' newArg='{3}' ppTokRange=[{4},{5}) spanBytes=[{6},{7})",
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
              "  mismatch span detail: argIdx={0} tokRange=[{1},{2}) byteRange=[{3},{4}) ppBytes=[{5},{6}) kind={7}",
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

std::optional<std::vector<std::pair<size_t, size_t>>>
RefoldEngine::GetMacroInvocationFormalArgContentRanges(
    const RefoldModel::MacroInvocation &m, StringRef invText) {
  // Prefer the producer-provided per-formal invocation-argument ranges when
  // available. This is required for variadic macros where multiple "actual"
  // arguments correspond to a single formal (e.g. __VA_ARGS__).
  if (!m.invArgRanges.empty()) {
    // The producer-provided ranges are absolute byte offsets in the invocation
    // file, captured from the *original* invocation spelling. If we've already
    // applied an earlier args-only patch to this invocation (so invText differs
    // from m.invText), those absolute endpoints no longer line up. Re-parse
    // current invocation spelling and then re-map to the producer's formal
    // arity (including the variadic tail) to keep subsequent hunks stable.
    if (m.invText && invText != *m.invText) {
      auto parsedOpt =
          RefoldEngine::ParseMacroInvocationArgContentRanges(invText);
      if (!parsedOpt)
        return std::nullopt;

      const auto &parsed = *parsedOpt;
      const size_t formalN = m.invArgRanges.size();
      const size_t actualN = parsed.size();

      // Helper to return a safe "empty" range at the close-paren location.
      auto emptyAtCloseParen = [&]() -> std::pair<size_t, size_t> {
        size_t closeIdx = invText.rfind(')');
        if (closeIdx == StringRef::npos)
          closeIdx = invText.size();
        return {closeIdx, closeIdx};
      };

      if (actualN == formalN)
        return parsed;

      std::vector<std::pair<size_t, size_t>> out;
      out.reserve(formalN);

      if (actualN > formalN && formalN > 0) {
        // More actual argument ranges were parsed than there are formal
        // parameters. Treat this as a variadic-style tail: keep the first
        // (formalN - 1) actuals aligned 1:1 with the corresponding formals,
        // then make the last formal cover one contiguous source range from the
        // start of the next actual through the end of the final actual.
        out.insert(out.end(), parsed.begin(), parsed.begin() + (formalN - 1));
        out.push_back({parsed[formalN - 1].first, parsed.back().second});
        return out;
      }

      // Fewer actual ranges were parsed than there are formals. Preserve the
      // actuals that do exist, then pad the remaining trailing formals with
      // empty ranges anchored at the call's closing ')', so the result still
      // contains one range per formal parameter.
      out.insert(out.end(), parsed.begin(), parsed.end());
      for (size_t i = actualN; i < formalN; ++i)
        out.push_back(emptyAtCloseParen());
      return out;
    }

    if (!m.invB)
      return std::nullopt;

    const uint64_t invB = *m.invB;

    std::vector<std::pair<size_t, size_t>> out;
    out.reserve(m.invArgRanges.size());

    bool anyInvalid = false;
    for (const auto &R : m.invArgRanges) {
      // Each recorded invocation-argument range is stored in absolute TU byte
      // space. Convert it into a half-open byte range relative to invText so
      // later parsing can index directly into the invocation spelling.
      if (!R.first || !R.second) {
        anyInvalid = true;
        out.emplace_back(static_cast<size_t>(-1), static_cast<size_t>(-1));
        continue;
      }

      // Reject malformed absolute ranges that start before the invocation or
      // whose end precedes the begin.
      if (*R.first < invB || *R.second < *R.first) {
        anyInvalid = true;
        out.emplace_back(static_cast<size_t>(-1), static_cast<size_t>(-1));
        continue;
      }

      const uint64_t relB64 = *R.first - invB;
      const uint64_t relE64 = *R.second - invB;

      // The relative slice must remain within the invocation text.
      if (relE64 > invText.size() || relB64 > relE64) {
        anyInvalid = true;
        out.emplace_back(static_cast<size_t>(-1), static_cast<size_t>(-1));
        continue;
      }

      out.emplace_back(static_cast<size_t>(relB64),
                       static_cast<size_t>(relE64));
    }

    if (!anyInvalid)
      return out;

    // Try to fill any invalid entries using a conservative textual parse of the
    // invocation spelling, preserving the formal-parameter indexing when the
    // parsed arity differs (variadics / missing variadic tail).
    auto parsedOpt =
        RefoldEngine::ParseMacroInvocationArgContentRanges(invText);
    if (!parsedOpt)
      return std::nullopt;

    const auto &parsed = *parsedOpt;
    const size_t formalN = out.size();
    const size_t actualN = parsed.size();

    // Helper to return a safe "empty" range at the close-paren location.
    auto emptyAtCloseParen = [&]() -> std::pair<size_t, size_t> {
      size_t closeIdx = invText.rfind(')');
      if (closeIdx == StringRef::npos)
        closeIdx = invText.size();
      return {closeIdx, closeIdx};
    };

    if (actualN == formalN) {
      // Parsed actuals already line up 1:1 with the formal parameter list.
      // Only fill entries that were not already supplied by the recorded
      // invocation ranges above.
      for (size_t i = 0; i < formalN; ++i) {
        if (out[i].first == static_cast<size_t>(-1))
          out[i] = parsed[i];
      }
      return out;
    }

    if (actualN > formalN && formalN > 0) {
      // More actual argument ranges were parsed than there are formals. Treat
      // this as a variadic-style tail: keep the leading formals aligned 1:1 to
      // the corresponding parsed actuals, and let the final formal absorb one
      // contiguous range from the start of the next actual through the end of
      // the last actual.
      for (size_t i = 0; i + 1 < formalN; ++i) {
        if (out[i].first == static_cast<size_t>(-1))
          out[i] = parsed[i];
      }

      // If the last formal did not already have a valid recorded range, make it
      // span the entire remaining tail of the callsite argument text.
      if (out[formalN - 1].first == static_cast<size_t>(-1)) {
        out[formalN - 1] = {parsed[formalN - 1].first, parsed.back().second};
      }
      return out;
    }

    // Fewer actual ranges were parsed than there are formals. Copy over the
    // actuals that exist, then synthesize empty trailing ranges at the closing
    // ')' so the result still has one entry per formal parameter.
    for (size_t i = 0; i < std::min(formalN, actualN); ++i) {
      if (out[i].first == static_cast<size_t>(-1))
        out[i] = parsed[i];
    }
    for (size_t i = actualN; i < formalN; ++i) {
      if (out[i].first == static_cast<size_t>(-1))
        out[i] = emptyAtCloseParen();
    }

    return out;
  }

  // Fallback: derive ranges from the invocation spelling. This path assumes a
  // 1:1 mapping between argument index and "actual" arguments.
  return RefoldEngine::ParseMacroInvocationArgContentRanges(invText);
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
  // validate cross-occurrence consistency. In this args-only path we only need
  // visibility into the current hunk, so treat it as the only token-level edit.
  diffutils::Hunk hArgs = h;

  // Keep the raw hunk unchanged. Pure-insertion ownership is derived later
  // from exact occurrence boundaries and exact mapped B-envelope adjacency.
  const diffutils::Hunk tokenHunks[] = {hArgs};

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

        // Splice the sub-token replacement into the spelling arg
        // conservatively.
        std::string newArg = SplicePasteSegmentIntoSpellingArg(
            baseArgText, pae.oldSeg, pae.newSeg);
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
                m, argIdx, baseArgText, newArg, tokenHunks)) {
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
        return MacroPatch{*m.invB, *m.invE, std::move(newInv), 0};
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
      std::string newArg = SplicePasteSegmentIntoSpellingArg(
          baseArgText, pae->oldSeg, pae->newSeg);
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
      if (!MacroArgReplacementMatchesAllOccurrencesInB(m, argIdx, baseArgText,
                                                       newArg, tokenHunks)) {
        return std::nullopt;
      }

      std::string newInv =
          stringutils::replaceRange(baseInvText, r.first, r.second, newArg);
      trace("macro/args", "  args-only SUCCESS newInv='{0}'",
            stringutils::showWSWithClip(newInv, 200));
      return MacroPatch{*m.invB, *m.invE, std::move(newInv), 0};
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
      std::string newArg = SplicePasteSegmentIntoSpellingArg(
          baseArgText, pae.oldSeg, pae.newSeg);
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
    return MacroPatch{*m.invB, *m.invE, std::move(newInv), 0};
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

  trace("macro/args", "  touchedOcc={0}",
        stringutils::boolArrayToString(touchedOcc));
  trace("macro/args", "  touched={0}", stringutils::boolArrayToString(touched));

  // Compute argument replacements implied by each touched occurrence. Multiple
  // occurrences of the same argIdx must imply the exact same replacement,
  // otherwise the macro cannot be refolded args-only.
  DenseMap<uint32_t, std::string> replByArgIdx;
  for (size_t i = 0; i < occs.size(); ++i) {
    const auto &sp = occs[i];
    uint32_t argIdx = sp.argIdx;
    if (argIdx >= touched.size() || !touched[argIdx])
      continue;

    if (static_cast<size_t>(argIdx) >= invArgRanges.size())
      return std::nullopt;

    // Base spelling for this argument in the invocation text (used for splice
    // and consistency).
    auto r0 = invArgRanges[argIdx];
    StringRef baseArgText = baseInvText.substr(r0.first, r0.second - r0.first);

    // Map the A occurrence envelope to B using byte-level hunks derived from
    // the global alignment. This leverages producer-provided pp_byte_begin/
    // pp_byte_end when present and falls back to consumer token offsets
    // otherwise.
    auto bEnv = MapAToBTokenEnvelopeByPPArgSpan(sp);
    if (!bEnv) {
      // Conservative fallback: use the hunk's B range if the span mapping
      // fails. If the hunk doesn't have a valid B range either, we cannot
      // safely derive an args-only replacement.
      if (h.bStart >= h.bEnd)
        return std::nullopt;
      bEnv = {static_cast<size_t>(h.bStart), static_cast<size_t>(h.bEnd)};
    }

    // If this occurrence exactly owns a pure insertion, extend the mapped
    // B envelope by that owned range. This uses only exact A-side occurrence
    // structure and exact mapped B-envelope adjacency.
    if (bEnv) {
      if (auto owned =
              GetOwnedPureInsertionBRangeForArgSpan(sp, occs, *bEnv, hArgs)) {
        const size_t insB0 = owned->first;
        const size_t insB1 = owned->second;
        size_t e0 = bEnv->first;
        size_t e1 = bEnv->second;

        if (!(insB1 < e0 || e1 < insB0)) {
          e0 = std::min(e0, insB0);
          e1 = std::max(e1, insB1);
        }

        if (e0 != bEnv->first || e1 != bEnv->second) {
          trace("macro/args",
                "  extend env with owned insertion: argIdx={0} env=[{1},{2}) "
                "ins=[{3},{4}) -> [{5},{6})",
                static_cast<size_t>(argIdx), bEnv->first, bEnv->second, insB0,
                insB1, e0, e1);
          bEnv = std::make_pair(e0, e1);
        }
      }
    }

    // Slice the edited text from B corresponding to this occurrence and treat
    // it as the candidate replacement for the argument (subject to stringify
    // decoding and paste lifting below).
    StringRef bSlice = SliceBSource(bEnv->first, bEnv->second).trim();
    std::string newArg = bSlice.str();

    // For stringify occurrences, the B slice is a string literal; decode it
    // back into the argument text that would produce that literal via string-
    // ification.
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

      // The decoded payload itself still has to be canonical, but the source
      // spelling of the original argument does not. This preserves valid
      // refolds such as S(a /*c*/ + b) -> S(a - b), where comments/spacing in
      // the original raw argument should not block reconstruction from a
      // canonical stringified payload.
      newArg = std::move(*canon);
    }

    // Optional lift/paste rewrite: when an argument participates in token
    // pasting, the direct B slice might reflect only the pasted contribution
    // rather than the full argument spelling. In that case, attempt to replace
    // only the A occurrence slice within the base argument spelling.
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
            std::string cand = baseArgText.substr(0, pos).str() + bSlice.str() +
                               baseArgText.substr(pos + aSlice.size()).str();
            newArg = StringRef(cand).trim().str();
            trace("macro/args",
                  "    lift/paste argIdx={0} baseArg={1} aSlice={2} bSlice={3} "
                  "-> newArg={4}",
                  argIdx, stringutils::showWSWithClip(baseArgText, 200),
                  stringutils::showWSWithClip(aSlice, 200),
                  stringutils::showWSWithClip(bSlice, 200),
                  stringutils::showWSWithClip(newArg, 200));
          } else {
            trace("macro/args",
                  "    lift/paste FAILED argIdx={0} baseArg={1} aSlice={2} "
                  "bSlice={3}",
                  argIdx, stringutils::showWS(baseArgText),
                  stringutils::showWS(aSlice), stringutils::showWS(bSlice));
          }
        }
      }
    }

    // For non-variadic formals, reject replacements that would introduce
    // additional top-level commas in the invocation spelling (which would
    // change the invocation's arity).
    if (!isVariadicFormal(argIdx) && hasTopLevelComma(newArg))
      return std::nullopt;

    // If we've already derived a replacement for this argument index, it must
    // match exactly.
    if (replByArgIdx.count(argIdx) && replByArgIdx[argIdx] != newArg)
      return std::nullopt;

    // Final safety gate for this argument: verify that the candidate replace-
    // ment reproduces all occurrences for argIdx in B, respecting strict/non-
    // strict stringify policy and paste behavior.
    if (!MacroArgReplacementMatchesAllOccurrencesInB(m, argIdx, baseArgText,
                                                     newArg, tokenHunks)) {
      trace(
          "macro/args",
          "    consistency check FAILED for argIdx={0} newArg='{1}' -> expand",
          argIdx, stringutils::showWSWithClip(newArg, 200));
      return std::nullopt;
    }

    trace("macro/args", "    consistency OK for argIdx={0}", argIdx);
    replByArgIdx[argIdx] = std::move(newArg);
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

  return MacroPatch{*m.invB, *m.invE, std::move(finalInv), 0};
}

StringRef RefoldEngine::SliceSource(ArrayRef<size_t> tokOff, StringRef source,
                                    uint64_t startTok, uint64_t endTok) {
  if (tokOff.empty() || source.empty())
    return "";

  const size_t n = tokOff.size();
  const uint64_t maxTokIdx = static_cast<uint64_t>(n) - 1;

  // Clamp token indices to valid array bounds.
  uint64_t loTok = std::clamp(startTok, static_cast<uint64_t>(0), maxTokIdx);
  uint64_t hiTok = std::clamp(endTok, loTok, maxTokIdx);

  size_t lo = tokOff[static_cast<size_t>(loTok)];
  size_t hi = tokOff[static_cast<size_t>(hiTok)];

  // Clamp byte offsets to the actual string length.
  const size_t sourceLen = source.size();
  lo = std::clamp(lo, size_t(0), sourceLen);
  hi = std::clamp(hi, lo, sourceLen);

  return source.substr(lo, hi - lo);
}

std::optional<std::string>
RefoldEngine::UnstringifyLiteralToArgText(StringRef literalTok,
                                          bool allowTopLevelComma) {
  StringRef s = literalTok.trim();
  if (s.empty())
    return std::nullopt;

  // Find the opening quote (after any optional prefix)
  size_t q = s.find('\"');
  if (q == StringRef::npos)
    return std::nullopt;

  // Validate the prefix (L, u, U, u8)
  StringRef prefix = s.substr(0, q);
  if (!prefix.empty()) {
    if (prefix != "L" && prefix != "u" && prefix != "U" && prefix != "u8")
      return std::nullopt;
  }

  // Ensure it has a closing quote and is at least ""
  if (s.size() < q + 2 || s.back() != '\"')
    return std::nullopt;

  // Extract content between the quotes
  StringRef body = s.slice(q + 1, s.size() - 1);

  std::string out;
  out.reserve(body.size());

  for (size_t i = 0; i < body.size(); ++i) {
    const char c = body[i];
    if (c == '\\' && i + 1 < body.size()) {
      const char n = body[i + 1];
      // Stringification only escapes backslashes and quotes
      if (n == '\\' || n == '\"') {
        out.push_back(n);
        i++;
        continue;
      }
      // Preserve other escape sequences (like \n, \t) as-is
      out.push_back(c);
      out.push_back(n);
      i++;
      continue;
    }
    out.push_back(c);
  }

  // Safety check: a top-level comma would be interpreted as multiple arguments
  // in a macro invocation. Allow callers that are only normalizing for
  // comparison (not re-synthesizing invocation text) to opt out.
  if (!allowTopLevelComma && out.find(',') != std::string::npos)
    return std::nullopt;

  // Macros cannot have raw newlines in arguments unless escaped/continued
  if (out.find('\n') != std::string::npos ||
      out.find('\r') != std::string::npos)
    return std::nullopt;

  return out;
}

bool RefoldEngine::HunkFullyWithinArgSpans(
    const diffutils::Hunk &h, ArrayRef<RefoldModel::PPArgSpan> argSpans,
    MutableArrayRef<char> touched) const {
  uint64_t a0 = h.aStart;
  uint64_t a1 = h.aEnd;

  auto isCommaTok = [&](uint64_t a) -> bool {
    return a < aToks_.size() &&
           aToks_[static_cast<size_t>(a)].spelling == ",";
  };

  // Pure insertions are attributed only from exact occurrence structure.
  if (a0 == a1) {
    if (auto owner = FindExactOwningArgSpanForPureInsertion(a0, argSpans)) {
      if (*owner >= touched.size())
        return false;
      touched[*owner] = 1;
      return true;
    }
    return false;
  }

  // Replacement/deletion: every covered token must fall inside some arg span.
  // Additionally, allow a separator comma that immediately precedes an arg span
  // (common when deleting the entire variadic tail, which removes the comma
  // after the last fixed formal) to be attributed to that right-hand span.
  bool any = false;
  for (uint64_t a = a0; a < a1; ++a) {
    bool inSome = false;

    // Note: multiple arg-like spans may overlap the same token range (e.g.
    // paste spans that partition a single PP token by byte subranges). In that
    // case, mark *all* containing spans as touched so later logic can pick the
    // one(s) that actually changed.
    for (size_t i = 0; i < argSpans.size(); ++i) {
      const auto &s = argSpans[i];
      if (a >= s.begin && a < s.end) {
        if (i < touched.size())
          touched[i] = 1;
        inSome = true;
        any = true;
      }
    }

    if (!inSome && isCommaTok(a)) {
      for (size_t i = 0; i < argSpans.size(); ++i) {
        const auto &s = argSpans[i];
        if (s.begin == a + 1) {
          if (i < touched.size())
            touched[i] = 1;
          inSome = true;
          any = true;
          break;
        }
      }
    }

    if (!inSome)
      return false;
  }
  return any;
}

std::vector<RefoldEngine::ByteHunk>
RefoldEngine::BuildByteHunksFromRawText() const {
  // We still need a physical array of "elements" for ArrayRef, but now, each
  // element is just 16 bytes (pointer + length) instead of a 32-byte
  // heap-allocating std::string.
  auto ToRefVec = [](StringRef s) {
    std::vector<StringRef> v;
    v.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
      // Point to a 1-character substring within the existing aText/bText. This
      // is O(1) and performs NO heap allocation for the character.
      v.push_back(s.substr(i, 1));
    }
    return v;
  };

  std::vector<StringRef> aRefs = ToRefVec(aSource_);
  std::vector<StringRef> bRefs = ToRefVec(bSource_);

  // diff now takes ArrayRef<StringRef>
  auto steps = diffutils::diff(aRefs, bRefs);
  auto hunks = diffutils::coalesce(steps);

  std::vector<ByteHunk> out;
  out.reserve(hunks.size());
  for (const auto &h : hunks) {
    out.emplace_back(h.aStart, h.aEnd, h.bStart, h.bEnd);
  }
  return out;
}

size_t RefoldEngine::MapAByteToBByteLowerBound(size_t aByte) const {
  if (!abByteHunks_ || abByteHunks_->empty())
    return aByte;

  const uint64_t searchVal = static_cast<uint64_t>(aByte);

  // 1. Find the first hunk where h.aStart >= aByte
  auto it = std::lower_bound(
      abByteHunks_->begin(), abByteHunks_->end(), searchVal,
      [](const ByteHunk &h, uint64_t val) { return h.aStart < val; });

  // 2. We need to look at the hunk PRIOR to 'it' to see if aByte falls inside
  // it, or if we are in the gap after it.
  if (it != abByteHunks_->begin()) {
    auto prev = std::prev(it);

    // If the byte is within the previous hunk's range [aStart, aEnd)
    if (searchVal < prev->aEnd) {
      // It's inside an edit/deletion; map to the start of the B-side
      // equivalent.
      return static_cast<size_t>(prev->bStart);
    }
  }

  // 3. Handle the Cumulative Delta.
  int64_t delta = 0;
  for (auto current = abByteHunks_->begin(); current != it; ++current) {
    delta += (static_cast<int64_t>(current->bEnd - current->bStart) -
              static_cast<int64_t>(current->aEnd - current->aStart));
  }

  int64_t result = static_cast<int64_t>(aByte) + delta;
  return static_cast<size_t>(std::max<int64_t>(0, result));
}

size_t RefoldEngine::MapAByteToBByteUpperBound(size_t aByte) const {
  if (!abByteHunks_ || abByteHunks_->empty())
    return aByte;

  const uint64_t searchVal = static_cast<uint64_t>(aByte);

  // 1. Binary search finds the first hunk where h.aStart >= aByte
  auto it = std::lower_bound(
      abByteHunks_->begin(), abByteHunks_->end(), searchVal,
      [](const ByteHunk &h, uint64_t val) { return h.aStart < val; });

  // 3. Accumulate delta from all hunks preceding 'it'
  int64_t delta = 0;
  for (auto current = abByteHunks_->begin(); current != it; ++current) {
    delta += (static_cast<int64_t>(current->bEnd - current->bStart) -
              static_cast<int64_t>(current->aEnd - current->aStart));
  }

  // 4. Handle boundary conditions at the 'it' position
  if (it != abByteHunks_->end()) {
    // If we land exactly on the start of this hunk
    if (searchVal == it->aStart) {
      if (it->aStart == it->aEnd) {
        // Pure insertion at the boundary: Include its shift in the delta.
        delta += static_cast<int64_t>(it->bEnd - it->bStart);
      }
      // Note: We don't snap here because we are at the start of a range.
    } else if (searchVal > it->aStart && searchVal < it->aEnd) {
      // Byte is inside a deletion/replacement hunk: snap to the end.
      return static_cast<size_t>(it->bEnd);
    }
  }

  // 5. Final translation with bounds safety
  int64_t result = static_cast<int64_t>(aByte) + delta;
  return static_cast<size_t>(std::max<int64_t>(0, result));
}

size_t RefoldEngine::BTokIndexFloor(size_t bByte) const {
  if (bTokOff_.size() < 2)
    return 0;

  const int n = bTokOff_.size() - 1;

  // If the byte is at or before the start of the first token.
  if (bByte <= bTokOff_[0])
    return 0;

  // If the byte is at or after the end of the last token (the sentinel).
  if (bByte >= bTokOff_[n])
    return n;

  size_t lo = 0;
  size_t hi = n;

  // Standard floor binary search.
  while (lo < hi) {
    size_t mid = lo + (hi - lo + 1) / 2;
    size_t off = bTokOff_[mid];
    if (off <= bByte)
      lo = mid;
    else
      hi = mid - 1;
  }

  return lo;
}

size_t RefoldEngine::BTokIndexCeil(size_t bByte) const {
  if (bTokOff_.size() < 2)
    return 0;

  const int n = bTokOff_.size() - 1;

  // If the byte is at or before the start of the first token.
  if (bByte <= bTokOff_[0])
    return 0;

  // If the byte is at or after the end of the last token (the sentinel).
  if (bByte >= bTokOff_[n])
    return n;

  size_t lo = 0;
  size_t hi = n;

  // Standard ceiling binary search.
  while (lo < hi) {
    size_t mid = lo + (hi - lo) / 2;
    size_t off = bTokOff_[mid];
    if (off < bByte)
      lo = mid + 1;
    else
      hi = mid;
  }

  return lo;
}

std::pair<size_t, size_t>
RefoldEngine::MapAByteRangeToBTokenEnvelope(size_t aByteBegin,
                                            size_t aByteEnd) const {
  // Sanitize A-byte input range
  if (aByteEnd < aByteBegin)
    aByteEnd = aByteBegin;

  // Instrumentation: for non-empty ranges, detect pure insertions anchored
  // exactly at the begin/end A-byte boundaries. These can make the lower-bound
  // mapping choose a B position *before* the insertion, which in turn can
  // cause B-token envelopes to include boundary insertions (a common root cause
  // of duplicated insertion material when a later macro whole-cover patch also
  // slices B).
  const bool nonEmpty = (aByteEnd > aByteBegin);
  auto tracePureInsAt = [&](size_t aByte, llvm::StringRef which) {
    if (!nonEmpty || !abByteHunks_ || abByteHunks_->empty())
      return;

    const uint64_t val = static_cast<uint64_t>(aByte);
    auto it = std::lower_bound(
        abByteHunks_->begin(), abByteHunks_->end(), val,
        [](const ByteHunk &h, uint64_t v) { return h.aStart < v; });

    unsigned shown = 0;
    for (auto cur = it; cur != abByteHunks_->end() && cur->aStart == val &&
                       cur->aEnd == val;
         ++cur) {
      // Pure insertion (A-length 0, B-length >0).
      if (cur->bEnd > cur->bStart) {
        trace("byte/env",
              "A{0} boundary has pure-insertion ByteHunk: A@{1} -> "
              "Bbytes=[{2},{3}) (len={4})",
              which, aByte, static_cast<size_t>(cur->bStart),
              static_cast<size_t>(cur->bEnd),
              static_cast<size_t>(cur->bEnd - cur->bStart));
        if (++shown >= 3)
          break;
      }
    }
  };

  tracePureInsAt(aByteBegin, "Begin");
  tracePureInsAt(aByteEnd, "End");

  // Convert A-byte span to B-byte span using A→B mapping
  size_t bByteBegin = MapAByteToBByteLowerBound(aByteBegin);
  size_t bByteEnd = MapAByteToBByteUpperBound(aByteEnd);

  // For non-empty A spans, exclude any *pure insertion* ByteHunks that are
  // anchored exactly at the begin/end A-byte boundaries.
  //
  // Rationale:
  //   * A pure insertion has A-length 0, so it is not part of the image of a
  //     non-empty A-byte interval under the A→B mapping.
  //   * If we include these boundary insertions in the envelope, callers that
  //     also materialize the insertion as its own edit can end up duplicating
  //     the inserted bytes (notably in macro whole-cover fallback).
  //
  // We only trim boundary insertions for non-empty spans; for empty spans the
  // insertion itself is the whole point.
  if (nonEmpty && abByteHunks_ && !abByteHunks_->empty()) {
    auto trimBegin = [&]() {
      const uint64_t key = static_cast<uint64_t>(aByteBegin);
      auto it = std::lower_bound(
          abByteHunks_->begin(), abByteHunks_->end(), key,
          [](const ByteHunk &h, uint64_t v) { return h.aStart < v; });
      for (auto cur = it;
           cur != abByteHunks_->end() && cur->aStart == key && cur->aEnd == key;
           ++cur) {
        if (cur->bEnd > cur->bStart) {
          const size_t be = static_cast<size_t>(cur->bEnd);
          if (be > bByteBegin) {
            const size_t oldBegin = bByteBegin;
            bByteBegin = be;
            if (inTraceMode() && bByteBegin > oldBegin) {
              StringRef trimmed = bSource_.slice(
                  oldBegin, std::min(oldBegin + 200, bByteBegin));
              trace("byte/env",
                    "trimBegin: Abytes=[{0},{1}) hunkBbytes=[{2},{3}) "
                    "Btrim=[{4},{5}) text='{6}'",
                    aByteBegin, aByteEnd, cur->bStart, cur->bEnd, oldBegin,
                    bByteBegin, stringutils::showWSWithClip(trimmed, 220));
            }
          }
        }
      }
    };

    auto trimEnd = [&]() {
      const uint64_t key = static_cast<uint64_t>(aByteEnd);
      auto it = std::lower_bound(
          abByteHunks_->begin(), abByteHunks_->end(), key,
          [](const ByteHunk &h, uint64_t v) { return h.aStart < v; });
      for (auto cur = it;
           cur != abByteHunks_->end() && cur->aStart == key && cur->aEnd == key;
           ++cur) {
        if (cur->bEnd > cur->bStart) {
          const size_t bs = static_cast<size_t>(cur->bStart);
          if (bs < bByteEnd) {
            const size_t oldEnd = bByteEnd;
            bByteEnd = bs;
            if (inTraceMode() && oldEnd > bByteEnd) {
              StringRef trimmed =
                  bSource_.slice(bByteEnd, std::min(bByteEnd + 200, oldEnd));
              trace("byte/env",
                    "trimEnd: Abytes=[{0},{1}) hunkBbytes=[{2},{3}) "
                    "Btrim=[{4},{5}) text='{6}'",
                    aByteBegin, aByteEnd, cur->bStart, cur->bEnd, bByteEnd,
                    oldEnd, stringutils::showWSWithClip(trimmed, 220));
            }
          }
        }
      }
    };

    trimBegin();
    trimEnd();
  }

  // Clamp B-byte bounds to legal range
  if (bByteEnd < bByteBegin)
    bByteEnd = bByteBegin;
  if (bByteEnd > bSource_.size())
    bByteEnd = bSource_.size();

  // Convert B-byte bounds to B-token index span
  size_t bTokBegin = BTokIndexFloor(bByteBegin); // inclusive
  size_t bTokEnd = BTokIndexCeil(bByteEnd);      // exclusive

  // Clamp B-token bounds
  if (bTokEnd < bTokBegin)
    bTokEnd = bTokBegin;

  return {bTokBegin, bTokEnd};
}

std::pair<size_t, size_t>
RefoldEngine::MapAByteRangeToBTokenEnvelopePreserveBoundaryInsertions(
    size_t aByteBegin, size_t aByteEnd) const {
  if (aByteEnd < aByteBegin)
    aByteEnd = aByteBegin;

  size_t bByteBegin = MapAByteToBByteLowerBound(aByteBegin);
  size_t bByteEnd = MapAByteToBByteUpperBound(aByteEnd);

  if (bByteEnd < bByteBegin)
    bByteEnd = bByteBegin;
  if (bByteEnd > bSource_.size())
    bByteEnd = bSource_.size();

  size_t bTokBegin = BTokIndexFloor(bByteBegin);
  size_t bTokEnd = BTokIndexCeil(bByteEnd);
  if (bTokEnd < bTokBegin)
    bTokEnd = bTokBegin;

  return {bTokBegin, bTokEnd};
}

std::optional<std::pair<size_t, size_t>>
RefoldEngine::MapATokRangeAToBTokenEnvelopePreserveBoundaryInsertions(
    uint64_t beginTok, uint64_t endTok) const {
  const uint64_t nA = static_cast<uint64_t>(aToks_.size());
  if (nA == 0 || aTokOff_.empty())
    return std::nullopt;

  beginTok = std::clamp(beginTok, static_cast<uint64_t>(0), nA);
  endTok = std::clamp(endTok, beginTok, nA);
  if (endTok <= beginTok)
    return std::nullopt;

  size_t idxEnd = static_cast<size_t>(endTok);
  if (idxEnd >= aTokOff_.size())
    return std::nullopt;

  const size_t aByteBegin = aTokOff_[static_cast<size_t>(beginTok)];
  const size_t aByteEnd = aTokOff_[idxEnd];
  return MapAByteRangeToBTokenEnvelopePreserveBoundaryInsertions(aByteBegin,
                                                                 aByteEnd);
}

std::optional<std::pair<size_t, size_t>>
RefoldEngine::MapAToBTokenEnvelopeByPPArgSpan(
    const RefoldModel::PPArgSpan &sp) const {
  // 1. Primary path: Mapping via Preprocessor Byte Spans
  if (sp.ppByteBegin && sp.ppByteEnd) {
    size_t pp0 = static_cast<size_t>(*sp.ppByteBegin);
    size_t pp1 = static_cast<size_t>(*sp.ppByteEnd);
    auto env = MapAByteRangeToBTokenEnvelope(pp0, pp1);
    trace("byte/env",
          "PPArgSpan['{0}' arg={1} Aidx={2} PPbytes=[{3},{4})] -> "
          "Btok=[{5},{6})",
          sp.kind, sp.argIdx, sp.begin, pp0, pp1, env.first, env.second);
    return env;
  }

  // 2. Don't resort to "fallback" if in strict mode.
  if (strict_) {
    fatal("macro/pparg/span",
          "PPArgSpan missing producer ppByte span (kind='{0}' argIdx={1} "
          "A=[{2},{3}) ppByte=[4},{5}]) - cannot map without snapping",
          sp.kind, sp.argIdx, sp.begin, sp.end, sp.ppByteBegin, sp.ppByteEnd);
    return std::nullopt;
  }

  // 3. Fallback: use consumer token offsets.
  const uint64_t maxATok = static_cast<uint64_t>(aToks_.size());
  uint64_t a0Idx = std::clamp(sp.begin, static_cast<uint64_t>(0), maxATok);
  uint64_t a1Idx = std::clamp(sp.end, a0Idx, maxATok);

  size_t a0 = aTokOff_[static_cast<size_t>(a0Idx)];
  size_t a1 = aTokOff_[static_cast<size_t>(a1Idx)];

  return MapAByteRangeToBTokenEnvelope(a0, a1);
}

std::optional<std::pair<size_t, size_t>>
RefoldEngine::MapATokRangeAToBTokenEnvelope(uint64_t beginTok,
                                            uint64_t endTok) const {
  const uint64_t nA = static_cast<uint64_t>(aToks_.size());

  // If we don't have token data, we can't perform the mapping.
  if (nA == 0 || aTokOff_.empty())
    return std::nullopt;

  // Standardize the token indices.
  beginTok = std::clamp(beginTok, static_cast<uint64_t>(0), nA);
  endTok = std::clamp(endTok, beginTok, nA);

  // If the range is empty or inverted, return nullopt.
  if (endTok <= beginTok)
    return std::nullopt;

  // Ensure we don't walk off the end of the offset array.
  size_t idxEnd = static_cast<size_t>(endTok);
  if (idxEnd >= aTokOff_.size()) {
    // If we are missing the sentinel, we can't safely determine the end of the
    // last token.
    return std::nullopt;
  }

  const size_t aByteBegin = aTokOff_[static_cast<size_t>(beginTok)];
  const size_t aByteEnd = aTokOff_[idxEnd];

  // Primary mapping path.
  auto env = MapAByteRangeToBTokenEnvelope(aByteBegin, aByteEnd);

  return env;
}

std::optional<std::pair<size_t, size_t>>
RefoldEngine::MapATokRangeAToBTokenEnvelopeWholeCover(uint64_t beginTok,
                                                      uint64_t endTok) const {
  const uint64_t nA = static_cast<uint64_t>(aToks_.size());

  if (nA == 0 || aTokOff_.empty())
    return std::nullopt;

  beginTok = std::clamp(beginTok, static_cast<uint64_t>(0), nA);
  endTok = std::clamp(endTok, beginTok, nA);
  if (endTok <= beginTok)
    return std::nullopt;

  const size_t idxEnd = static_cast<size_t>(endTok);
  if (idxEnd >= aTokOff_.size())
    return std::nullopt;

  const size_t aByteBegin = aTokOff_[static_cast<size_t>(beginTok)];
  const size_t aByteEnd = aTokOff_[idxEnd];

  size_t bByteBegin = MapAByteToBByteLowerBound(aByteBegin);
  size_t bByteEnd = MapAByteToBByteUpperBound(aByteEnd);

  if (bByteEnd < bByteBegin)
    bByteEnd = bByteBegin;
  if (bByteEnd > bSource_.size())
    bByteEnd = bSource_.size();

  size_t bTokBegin = BTokIndexFloor(bByteBegin);
  size_t bTokEnd = BTokIndexCeil(bByteEnd);
  if (bTokEnd < bTokBegin)
    bTokEnd = bTokBegin;

  return std::make_pair(bTokBegin, bTokEnd);
}

std::optional<std::pair<size_t, size_t>>
RefoldEngine::MapATokRangeAToBTokenEnvelopeTrimEdgeInsertions(
    uint64_t beginTok, uint64_t endTok) const {
  const uint64_t nA = static_cast<uint64_t>(aToks_.size());

  // Mirror the clamping performed by MapATokRangeAToBTokenEnvelope so the edge
  // positions we trim are the exact A-token boundaries used for the mapping.
  beginTok = std::clamp(beginTok, static_cast<uint64_t>(0), nA);
  endTok = std::clamp(endTok, beginTok, nA);

  auto envOpt = MapATokRangeAToBTokenEnvelope(beginTok, endTok);
  if (!envOpt)
    return std::nullopt;

  if (abTokHunks_.empty())
    return envOpt;

  size_t bBegin = envOpt->first;
  size_t bEnd = envOpt->second;

  auto trimAt = [&](uint64_t aPos, bool isBegin) {
    for (const auto &h : abTokHunks_) {
      // Only consider token-level pure insertions anchored at this exact A-gap.
      if (h.aStart != aPos || h.aEnd != aPos)
        continue;
      if (h.bEnd <= h.bStart)
        continue;

      const size_t hb0 = static_cast<size_t>(h.bStart);
      const size_t hb1 = static_cast<size_t>(h.bEnd);

      // Only trim if the computed envelope overlaps the insertion payload.
      if (!(bBegin < hb1 && bEnd > hb0))
        continue;

      if (isBegin) {
        // Drop any insertion payload that lies *before* the first A token.
        bBegin = std::max(bBegin, hb1);
      } else {
        // Drop any insertion payload that lies *after* the last A token.
        bEnd = std::min(bEnd, hb0);
      }
    }
  };

  trimAt(beginTok, /*isBegin=*/true);
  trimAt(endTok, /*isBegin=*/false);

  if (bBegin > bEnd)
    bBegin = bEnd;

  return std::make_pair(bBegin, bEnd);
}


std::optional<std::vector<std::pair<size_t, size_t>>>
RefoldEngine::ParseMacroInvocationArgContentRanges(StringRef invText) {
  // Locate the start of the argument list.
  size_t open = invText.find('(');
  if (open == StringRef::npos)
    return std::nullopt;

  std::vector<std::pair<size_t, size_t>> out;
  size_t n = invText.size();

  // Special-case: empty argument list "()" (or only whitespace/comments inside)
  // means *zero* arguments. The previous logic treated this as a single empty
  // argument, which is incorrect for C/C++ macro calls and breaks args-only
  // lifting for zero-arg call chains like "PICK1()(10)".
  const size_t afterOpen = open + 1;
  const size_t firstTok = stringutils::skipWSAndComments(invText, afterOpen);
  if (firstTok < n && invText[firstTok] == ')')
    return out;

  uint32_t depth = 0; // Track nested parentheses, brackets, or braces.
  bool inS = false;   // Inside a single-quoted character literal.
  bool inD = false;   // Inside a double-quoted string literal.

  size_t argStart = open + 1;
  for (size_t i = argStart; i < n; i++) {
    char c = invText[i];

    // Handle escaping and termination inside character literals.
    if (inS) {
      if (c == '\\' && i + 1 < n) {
        i++; // Skip escaped characters.
        continue;
      }
      if (c == '\'')
        inS = false;
      continue;
    }

    // Handle escaping and termination inside string literals.
    if (inD) {
      if (c == '\\' && i + 1 < n) {
        i++; // Skip escaped characters.
        continue;
      }
      if (c == '"')
        inD = false;
      continue;
    }

    // Enter literal state if a quote is encountered.
    if (c == '\'') {
      inS = true;
      continue;
    }
    if (c == '"') {
      inD = true;
      continue;
    }

    // Increment depth for nested groups; commas inside these do not
    // separate macro arguments.
    if (c == '(' || c == '[' || c == '{') {
      depth++;
      continue;
    }

    // Handle the closing of a group or the entire argument list.
    if (c == ')') {
      if (depth == 0) {
        // Final argument reached at the closing parenthesis of the call.
        out.push_back(stringutils::trimWsRange(invText, argStart, i));
        return out;
      }
      depth--;
      continue;
    }

    // A comma at depth 0 signifies the end of one macro argument.
    if (c == ',' && depth == 0) {
      out.push_back(stringutils::trimWsRange(invText, argStart, i));
      argStart = i + 1;
    }
  }

  // If the loop finishes without hitting the final ')', the syntax is invalid.
  return std::nullopt;
}

RefoldEngine::IncludePatch
RefoldEngine::BuildIncludeInsertionPatch(const RefoldModel::IncludeItem &inc,
                                         const diffutils::Hunk &h) const {
  // Extra debug: show the raw PP hunk slices.
  DebugIncludePatch("pre", inc, h);

  std::string insertBytes;
  const size_t numOffsets = bTokOff_.size();
  const size_t sourceLen = bSource_.size();

  const size_t uBStart = static_cast<size_t>(h.bStart);
  const size_t uBEnd = static_cast<size_t>(h.bEnd);

  // Validate hunk bounds against B-token offsets.
  if (uBStart < numOffsets && uBEnd < numOffsets && uBEnd >= uBStart) {
    size_t b0 = bTokOff_[uBStart];
    size_t b1 = bTokOff_[uBEnd];

    // Clamp byte offsets to the actual length of bSource_ (defensively handle
    // huge sizes).
    b0 = std::min(b0, sourceLen);
    b1 = std::clamp(b1, b0, sourceLen);
    if (b1 > b0) {
      insertBytes = bSource_.substr(b0, b1 - b0).str();
    }
  } else {
    // Hardening: Log an error or assert if we get a hunk that points
    // outside our known token universe.
    fatal("include/patch",
          "hunk bounds exceed token offset table for inc #{0}: "
          "B[{1},{2}) requested, but bTokOff only has {3} entries",
          inc.id, uBStart, uBEnd, numOffsets);
  }

  IncludePatch patch{&inc,  std::move(insertBytes), h.aStart, h.aEnd, h.bStart,
                     h.bEnd};

  trace("include/patch",
        "built inc #{0} patch A[{1},{2})->B[{3},{4}) len(insertBytes)={5}",
        inc.id, patch.aStart, patch.aEnd, patch.bStart, patch.bEnd,
        patch.insertBytes.size());

  return patch;
}


SmallVector<RefoldEngine::ForcedMacroPatchRequest, 32>
RefoldEngine::ComputeForcedCounterPatches(StringRef tuPath,
                                          ArrayRef<int64_t> a2b) const {
  struct Occ {
    const RefoldModel::MacroInvocation *m;
    uint64_t aStart;
    uint64_t aEnd;
    std::optional<uint64_t> ownerInc;
  };

  SmallVector<Occ, 32> occs;

  // Collect __COUNTER__ occurrences in PP-token space.
  //
  // Note: when __COUNTER__ is consumed by token paste or stringification, the
  // producer may record a zero-length span (begin==end) anchored at the output
  // token index affected by the counter. Treat such anchors as a single-token
  // occurrence so edits to pasted/stringified counter materialize and trigger
  // suffix stabilization.
  auto addOcc = [&](const RefoldModel::MacroInvocation &mi, uint64_t b,
                    uint64_t e) {
    if (e > b) {
      occs.push_back(Occ{&mi, b, e, std::nullopt});
      return;
    }
    if (e == b && b < aToks_.size()) {
      occs.push_back(Occ{&mi, b, b + 1, std::nullopt});
      return;
    }
  };

  for (const auto &mi : model_.GetMacroInvocations()) {
    if (mi.name != "__COUNTER__")
      continue;
    if (!mi.invB || !mi.invE || !mi.invText)
      continue;

    if (!mi.bodySpans.empty()) {
      for (const auto &bs : mi.bodySpans)
        addOcc(mi, bs.begin, bs.end);
      continue;
    }

    // Prefer explicit spans when present, including zero-length anchors.
    if (!mi.spans.empty()) {
      for (const auto &s : mi.spans)
        addOcc(mi, s.begin, s.end);
      continue;
    }

    if (mi.cover.IsValid() && mi.cover.end > mi.cover.begin)
      occs.push_back(Occ{&mi, mi.cover.begin, mi.cover.end, std::nullopt});
  }

  if (occs.empty())
    return {};

  // Determine the true PP-owner include for each occurrence and drop any
  // producer-merged duplicates whose ownerIncludeId does not match.
  SmallVector<Occ, 32> filtered;
  filtered.reserve(occs.size());
  for (const auto &o : occs) {
    diffutils::Hunk dummy;
    dummy.aStart = o.aStart;
    dummy.aEnd = o.aEnd;
    dummy.bStart = 0;
    dummy.bEnd = 0;

    Owner owner = ClassifyOwnerWithSegments(tuPath, dummy);
    std::optional<uint64_t> ownerInc;
    if (owner.kind == OwnerKind::Include)
      ownerInc = owner.includeId;

    // If the producer says this macro belongs to an include instance, require
    // the occurrence to actually be owned by that include.
    if (o.m->ownerIncludeId && ownerInc && *o.m->ownerIncludeId != *ownerInc)
      continue;

    filtered.push_back(Occ{o.m, o.aStart, o.aEnd, ownerInc});
  }

  if (filtered.empty())
    return {};

  // Sort by PP-token order.
  llvm::sort(filtered, [](const Occ &A, const Occ &B) {
    if (A.aStart != B.aStart)
      return A.aStart < B.aStart;
    if (A.aEnd != B.aEnd)
      return A.aEnd < B.aEnd;
    if (A.ownerInc != B.ownerInc)
      return A.ownerInc < B.ownerInc;
    return A.m->id < B.m->id;
  });

  auto isEditedOcc = [&](const Occ &o) -> bool {
    for (uint64_t ai = o.aStart; ai < o.aEnd; ++ai) {
      if (static_cast<size_t>(ai) >= a2b.size() ||
          static_cast<size_t>(ai) >= aToks_.size())
        return true;
      const int64_t bj = a2b[static_cast<size_t>(ai)];
      if (bj < 0 || static_cast<size_t>(bj) >= bToks_.size())
        return true;
      if (aToks_[static_cast<size_t>(ai)].spelling !=
          bToks_[static_cast<size_t>(bj)].spelling)
        return true;
    }
    return false;
  };

  auto isEditedInvocationOutput =
      [&](const RefoldModel::MacroInvocation &m) -> bool {
    auto spanEdited = [&](uint64_t lo, uint64_t hi) -> bool {
      for (uint64_t ai = lo; ai < hi; ++ai) {
        if (static_cast<size_t>(ai) >= a2b.size() ||
            static_cast<size_t>(ai) >= aToks_.size())
          return true;
        const int64_t bj = a2b[static_cast<size_t>(ai)];
        if (bj < 0 || static_cast<size_t>(bj) >= bToks_.size())
          return true;
        if (aToks_[static_cast<size_t>(ai)].spelling !=
            bToks_[static_cast<size_t>(bj)].spelling)
          return true;
      }
      return false;
    };

    // Compare the emitted output segments directly when the producer recorded
    // them. Do not collapse discontiguous bodySpans into one merged envelope:
    // the holes between body spans are caller-formal substitution sites, and
    // edits to those sites must not be misclassified as edits to an embedded
    // __COUNTER__ occurrence.
    if (!m.bodySpans.empty()) {
      for (const auto &s : m.bodySpans) {
        if (s.begin < s.end && spanEdited(s.begin, s.end))
          return true;
      }
      return false;
    }

    if (!m.spans.empty()) {
      for (const auto &s : m.spans) {
        if (s.begin < s.end) {
          if (spanEdited(s.begin, s.end))
            return true;
          continue;
        }
        if (s.begin == s.end && s.begin < aToks_.size()) {
          if (spanEdited(s.begin, s.begin + 1))
            return true;
        }
      }
      return false;
    }

    if (m.cover.IsValid() && m.cover.end > m.cover.begin)
      return spanEdited(m.cover.begin, m.cover.end);

    return false;
  };

  int firstEditedIdx = -1;
  for (size_t i = 0; i < filtered.size(); ++i) {
    if (isEditedOcc(filtered[i])) {
      firstEditedIdx = static_cast<int>(i);
      break;
    }
  }

  // Fallback: some nested-macro scenarios record __COUNTER__ with a zero-length
  // PP span anchored at a caller site that is *not* the edited output token
  // itself. In those cases, the direct __COUNTER__ occurrence comparison above
  // can miss the edit. Recover by lifting each __COUNTER__ occurrence to a
  // patchable caller macro whose bodySpans/cover correspond to the actual
  // expanded token slice, then redo edit detection.
  if (firstEditedIdx < 0) {
    DenseMap<uint64_t, const RefoldModel::MacroInvocation *> invById;
    invById.reserve(model_.GetMacroInvocations().size());
    for (const auto &mi : model_.GetMacroInvocations())
      invById[mi.id] = &mi;

    auto findPatchableCaller = [&](const RefoldModel::MacroInvocation &mi)
        -> const RefoldModel::MacroInvocation * {
      const RefoldModel::MacroInvocation *cur = &mi;
      while (cur) {
        if (!IsInvocationInsideDefineDirective(*cur))
          return cur;
        if (!cur->callerMacroId)
          break;
        auto it = invById.find(*cur->callerMacroId);
        if (it == invById.end())
          break;
        cur = it->second;
      }
      return nullptr;
    };

    auto computeOccRange = [&](const RefoldModel::MacroInvocation &m)
        -> std::optional<std::pair<uint64_t, uint64_t>> {
      // Prefer the precise body slice when available.
      if (!m.bodySpans.empty()) {
        uint64_t lo = std::numeric_limits<uint64_t>::max();
        uint64_t hi = 0;
        for (const auto &s : m.bodySpans) {
          if (s.begin < s.end) {
            lo = std::min(lo, s.begin);
            hi = std::max(hi, s.end);
          }
        }
        if (lo != std::numeric_limits<uint64_t>::max() && lo < hi)
          return std::make_pair(lo, hi);
      }

      // Next: explicit spans (including 0-length anchors).
      if (!m.spans.empty()) {
        uint64_t lo = std::numeric_limits<uint64_t>::max();
        uint64_t hi = 0;
        for (const auto &s : m.spans) {
          if (s.begin < s.end) {
            lo = std::min(lo, s.begin);
            hi = std::max(hi, s.end);
          } else if (s.begin == s.end && s.begin < aToks_.size()) {
            lo = std::min(lo, s.begin);
            hi = std::max(hi, s.begin + 1);
          }
        }
        if (lo != std::numeric_limits<uint64_t>::max() && lo < hi)
          return std::make_pair(lo, hi);
      }

      if (m.cover.IsValid() && m.cover.end > m.cover.begin)
        return std::make_pair(m.cover.begin, m.cover.end);

      return std::nullopt;
    };

    SmallVector<Occ, 32> lifted;
    lifted.reserve(filtered.size());
    for (const auto &o : filtered) {
      const RefoldModel::MacroInvocation *root = findPatchableCaller(*o.m);
      if (!root)
        continue;

      auto r = computeOccRange(*root);
      if (!r)
        continue;

      lifted.push_back(Occ{root, r->first, r->second, root->ownerIncludeId});
    }

    if (!lifted.empty()) {
      llvm::sort(lifted, [](const Occ &A, const Occ &B) {
        if (A.aStart != B.aStart)
          return A.aStart < B.aStart;
        if (A.aEnd != B.aEnd)
          return A.aEnd < B.aEnd;
        if (A.ownerInc != B.ownerInc)
          return A.ownerInc < B.ownerInc;
        return A.m->id < B.m->id;
      });

      int idx = -1;
      for (size_t i = 0; i < lifted.size(); ++i) {
        if (isEditedInvocationOutput(*lifted[i].m)) {
          idx = static_cast<int>(i);
          break;
        }
      }

      if (idx >= 0) {
        trace("counter",
              "__COUNTER__: lifted edit detection found firstEditedIdx={0} at "
              "A=[{1},{2}) root='{3}'",
              idx, lifted[static_cast<size_t>(idx)].aStart,
              lifted[static_cast<size_t>(idx)].aEnd,
              lifted[static_cast<size_t>(idx)].m->name);
        filtered.swap(lifted);
        firstEditedIdx = idx;
      }
    }
  }

  if (firstEditedIdx < 0) {
    trace("counter", "__COUNTER__: no edited occurrences; no forced expansion");
    return {};
  }

  // Define a unique key structure for our DenseSet
  struct MacroKey {
    std::optional<uint64_t> ownerId;
    uint64_t start;
    uint64_t end;
  };

  SmallVector<ForcedMacroPatchRequest, 32> forced;
  llvm::DenseSet<llvm::hash_code> seen;

  for (const Occ &o : llvm::drop_begin(filtered, firstEditedIdx)) {
    const auto *root =
        SmallestCoveringPatchableMacro(o.aStart, o.aEnd, o.ownerInc);

    // Safety: never patch macro definitions.
    if (!root || IsInvocationInsideDefineDirective(*root))
      continue;

    const auto invStart = root->invB;
    const auto invEnd = root->invE;
    if (!invStart || !invEnd)
      continue;

    auto key = llvm::hash_combine(root->ownerIncludeId.value_or(0), *invStart,
                                  *invEnd);
    if (!seen.insert(key).second)
      continue;

    forced.push_back({root, o.aStart, o.aEnd});

    trace("counter",
          "__COUNTER__: force root id={0} name='{1}' ownerInc={2} "
          "inv=[{3},{4}) for occ A=[{5},{6})",
          root->id, root->name, root->ownerIncludeId, *invStart, *invEnd,
          o.aStart, o.aEnd);
  }

  debug("counter",
        "__COUNTER__: occurrences={0} firstEditedIdx={1} forced={2} "
        "firstOccA=[{3},{4})",
        filtered.size(), firstEditedIdx, forced.size(),
        filtered[static_cast<size_t>(firstEditedIdx)].aStart,
        filtered[static_cast<size_t>(firstEditedIdx)].aEnd);

  return forced;
}

SmallVector<RefoldEngine::ForcedMacroPatchRequest, 32>
RefoldEngine::ComputeForcedCounterPatchesFromExpandedMacros(
    StringRef tuPath,
    const DenseMap<std::optional<uint64_t>, DenseMap<uint64_t, MacroPatch>>
        &macroPatchByOwnerByMacroId) const {
  struct Occ {
    const RefoldModel::MacroInvocation *m;
    uint64_t aStart;
    uint64_t aEnd;
    std::optional<uint64_t> ownerInc;
  };

  SmallVector<Occ, 32> occs;

  auto addOcc = [&](const RefoldModel::MacroInvocation &mi, uint64_t b,
                    uint64_t e) {
    if (e > b) {
      occs.push_back(Occ{&mi, b, e, std::nullopt});
      return;
    }
    if (e == b && b < aToks_.size()) {
      occs.push_back(Occ{&mi, b, b + 1, std::nullopt});
      return;
    }
  };

  for (const auto &mi : model_.GetMacroInvocations()) {
    if (mi.name != "__COUNTER__")
      continue;
    if (!mi.invB || !mi.invE || !mi.invText)
      continue;

    if (!mi.bodySpans.empty()) {
      for (const auto &bs : mi.bodySpans)
        addOcc(mi, bs.begin, bs.end);
      continue;
    }

    if (!mi.spans.empty()) {
      for (const auto &s : mi.spans)
        addOcc(mi, s.begin, s.end);
      continue;
    }

    if (mi.cover.IsValid() && mi.cover.end > mi.cover.begin)
      occs.push_back(Occ{&mi, mi.cover.begin, mi.cover.end, std::nullopt});
  }

  if (occs.empty())
    return {};

  SmallVector<Occ, 32> filtered;
  filtered.reserve(occs.size());
  for (const auto &o : occs) {
    diffutils::Hunk dummy;
    dummy.aStart = o.aStart;
    dummy.aEnd = o.aEnd;
    dummy.bStart = 0;
    dummy.bEnd = 0;

    Owner owner = ClassifyOwnerWithSegments(tuPath, dummy);
    std::optional<uint64_t> ownerInc;
    if (owner.kind == OwnerKind::Include)
      ownerInc = owner.includeId;

    if (o.m->ownerIncludeId && ownerInc && *o.m->ownerIncludeId != *ownerInc)
      continue;

    filtered.push_back(Occ{o.m, o.aStart, o.aEnd, ownerInc});
  }

  if (filtered.empty())
    return {};

  llvm::sort(filtered, [](const Occ &A, const Occ &B) {
    if (A.aStart != B.aStart)
      return A.aStart < B.aStart;
    if (A.aEnd != B.aEnd)
      return A.aEnd < B.aEnd;
    if (A.ownerInc != B.ownerInc)
      return A.ownerInc < B.ownerInc;
    return A.m->id < B.m->id;
  });

  auto hasExpandedPatchFor = [&](const RefoldModel::MacroInvocation &root)
      -> bool {
    const auto oit = macroPatchByOwnerByMacroId.find(root.ownerIncludeId);
    if (oit == macroPatchByOwnerByMacroId.end())
      return false;

    const auto invStart = root.invB;
    const auto invEnd = root.invE;
    if (!invStart || !invEnd)
      return false;

    for (const auto &kv : oit->second) {
      const MacroPatch &p = kv.second;
      if (p.invStart != *invStart || p.invEnd != *invEnd)
        continue;
      if (MacroPatchRemainsExpanded(p))
        return true;
    }
    return false;
  };

  int firstExpandedIdx = -1;
  for (size_t i = 0; i < filtered.size(); ++i) {
    const auto *root =
        SmallestCoveringPatchableMacro(filtered[i].aStart, filtered[i].aEnd,
                                       filtered[i].ownerInc);
    if (!root || IsInvocationInsideDefineDirective(*root))
      continue;
    if (hasExpandedPatchFor(*root)) {
      firstExpandedIdx = static_cast<int>(i);
      break;
    }
  }

  if (firstExpandedIdx < 0)
    return {};

  SmallVector<ForcedMacroPatchRequest, 32> forced;
  llvm::DenseSet<llvm::hash_code> seen;
  for (const Occ &o : llvm::drop_begin(filtered, firstExpandedIdx)) {
    const auto *root =
        SmallestCoveringPatchableMacro(o.aStart, o.aEnd, o.ownerInc);
    if (!root || IsInvocationInsideDefineDirective(*root))
      continue;
    const auto invStart = root->invB;
    const auto invEnd = root->invE;
    if (!invStart || !invEnd)
      continue;

    auto key = llvm::hash_combine(root->ownerIncludeId.value_or(0), *invStart,
                                  *invEnd);
    if (!seen.insert(key).second)
      continue;

    forced.push_back({root, o.aStart, o.aEnd});
    trace("counter",
          "__COUNTER__: expanded-macro stabilization firstExpandedIdx={0} "
          "force root id={1} name='{2}' ownerInc={3} inv=[{4},{5}) for occ "
          "A=[{6},{7})",
          firstExpandedIdx, root->id, root->name, root->ownerIncludeId,
          *invStart, *invEnd, o.aStart, o.aEnd);
  }

  return forced;
}


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

std::optional<std::string>
RefoldEngine::BuildWholeCoverReplacementText(
    const RefoldModel::MacroInvocation &m) const {
  auto range = GetWholeCoverATokRange(m);
  if (!range)
    return std::nullopt;
  uint64_t covLoA = range->first;
  uint64_t covHiA = range->second;

  if (!MacroWholeCoverIsSelfContained(m)) {
    trace("macro/whole",
          "whole-cover rejected inv id={0} name='{1}': non-self-contained cover=[{2},{3})",
          m.id, m.name, covLoA, covHiA);
    return std::nullopt;
  }

  auto bEnv = MapATokRangeAToBTokenEnvelopePreserveBoundaryInsertions(
      covLoA, covHiA);
  if (!bEnv)
    return std::nullopt;

  size_t bTokStart = bEnv->first;
  size_t bTokEnd = bEnv->second;
  if (bTokEnd <= bTokStart)
    return std::nullopt;

  // Tighten the B-side envelope to the exact A-side boundary tokens when
  // possible. This mirrors the non-heuristic correction used by
  // BuildMacroInvocationPatchWholeCover's whole-cover fallback.
  if (covLoA < aToks_.size() && bTokStart < bToks_.size()) {
    StringRef want = aToks_[static_cast<size_t>(covLoA)].spelling;
    if (!want.empty()) {
      if (bToks_[bTokStart].spelling != want && bTokStart > 0 &&
          bToks_[bTokStart - 1].spelling == want) {
        bTokStart--;
      }
    }
  }

  if (covHiA > 0 && (covHiA - 1) < aToks_.size() && bTokEnd > 0 &&
      (bTokEnd - 1) < bToks_.size()) {
    StringRef want = aToks_[static_cast<size_t>(covHiA - 1)].spelling;
    if (!want.empty()) {
      if (bToks_[bTokEnd - 1].spelling != want && bTokEnd >= 2 &&
          bToks_[bTokEnd - 2].spelling == want) {
        bTokEnd--;
      }
    }
  }

  if (bTokEnd <= bTokStart)
    return std::nullopt;
  std::string clipped = SliceBSourceClippedAgainstClaims(bTokStart, bTokEnd);
  return StringRef(clipped).trim().str();
}

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
      return MacroPatch{*invStart, *invEnd, std::move(*repl), 0};
  }

  trace("macro/whole",
        "whole-cover build inv id={0} name={1} ownerIncludeId={2} hasOwner={3} "
        "inv=[{4},{5}) baseInvLen={6} hunk A[{7},{8})->B[{9},{10})",
        m.id, m.name, m.ownerIncludeId.value_or(0), (bool)m.ownerIncludeId,
        *invStart, *invEnd, baseInvText.size(), h.aStart, h.aEnd, h.bStart,
        h.bEnd);

  // Do not downgrade: if we already have a patch for this invocation and it
  // does not look like a callsite invocation anymore (i.e. we already
  // realized/expanded it), keep it. If it is still a callsite patch, we may
  // refine it across additional hunks.
  const MacroPatch *existingPatch = nullptr;
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

      const bool isCallsite =
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
        return it->second;
    }

    if (bestCallsiteId) {
      auto it = ownerIt->second.find(*bestCallsiteId);
      if (it != ownerIt->second.end()) {
        existingPatch = &it->second;
        existingIsCallsite = true;
      }
    }
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

  // Important arbitration rule: a direct root args-only patch is not returned
  // immediately. We let the DAG-chained certificate path run afterwards and
  // prefer it when it can produce a unique validated callsite rewrite, because
  // that path can preserve deeper nested macro structure that a direct root
  // argument rewrite would flatten.
  std::optional<MacroPatch> argsOnlyCandidate;
  bool reuseExistingCallsitePatch = false;

  SmallVector<RefoldModel::PPArgSpan, 16> argLikeSpans;
  argLikeSpans.append(m.argSpans.begin(), m.argSpans.end());
  argLikeSpans.append(m.stringifySpans.begin(), m.stringifySpans.end());
  argLikeSpans.append(m.pasteSpans.begin(), m.pasteSpans.end());

  SmallVector<char, 16> argTouched(argLikeSpans.size(), 0);
  if (!ForceWholeCoverMacros() && !argLikeSpans.empty() &&
      HunkFullyWithinArgSpans(hEff, argLikeSpans, argTouched)) {
    // invB/invE are offsets in the invocation file, not in the A-stream source;
    // do not slice aSource here (it can be shorter and/or refer to a different
    // logical file).
    StringRef invSpanText =
        !baseInvText.empty()
            ? baseInvText
            : (m.invText ? StringRef(*m.invText) : StringRef(""));
    if (InvocationSpanMatchesCallsitePrefix(invSpanText, m)) {
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
            existingPatch && existingIsCallsite && !baseInvText.empty();
      }
    }
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
  if (!ForceWholeCoverMacros() && m.subkind == "func" &&
      HasLiteralMacroCalleeOrigin(m)) {
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

      auto pathHasOnlyLiteralCallees =
          [&](const RefoldModel::MacroInvocation &cand) -> bool {
        const RefoldModel::MacroInvocation *cur = &cand;
        for (;;) {
          if (!HasLiteralMacroCalleeOrigin(*cur))
            return false;
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
          if (sp.kind == PPArgSpanKind::Paste) {
            // Paste spans can identify a subrange within a single pasted token.
            if (!sp.byteBegin || !sp.byteEnd)
              return std::nullopt;
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

        if (sp.kind == PPArgSpanKind::Paste) {
          if (!sp.byteBegin || !sp.byteEnd)
            return std::nullopt;
          if ((bEnv->second - bEnv->first) != 1)
            return std::nullopt;
          const uint64_t bb = *sp.byteBegin;
          const uint64_t be = *sp.byteEnd;

          // Token-internal byte ranges are computed from the A-side token
          // spelling. If the B-side pasted token changes length (e.g.
          // L\"hello\" -> L\"goodbye\"), using the raw [bb,be) slice can
          // truncate the changed segment. When possible, re-derive the B-side
          // segment by preserving the A-side prefix/suffix around the segment.
          StringRef aTok = SliceASource(static_cast<size_t>(sp.begin),
                                        static_cast<size_t>(sp.end));
          if (be < bb || be > static_cast<uint64_t>(aTok.size()))
            return std::nullopt;
          StringRef aPref = aTok.take_front(static_cast<size_t>(bb));
          StringRef aSuff = aTok.drop_front(static_cast<size_t>(be));
          if (b.starts_with(aPref) && b.ends_with(aSuff) &&
              b.size() >= aPref.size() + aSuff.size()) {
            // Best case: we can "peel" the unchanged prefix/suffix and get
            // the true edited middle segment.
            b = b.slice(aPref.size(), b.size() - aSuff.size());
          } else {
            // Fallback: clamp to the recorded offsets. This may be ambiguous,
            // so mark unreliable; later we only accept such edits if we can
            // uniquely split the token at the token level.
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

      SmallVector<LeafCandidate, 8> leafCands;
      directRootPreservationInadmissible = false;
      for (const auto &cand : model_.GetMacroInvocations()) {
        // Only consider invocations that are descendants of the root m.
        auto d = depthToRoot(cand);
        if (!d || *d == 0)
          continue;
        const bool hunkWithinCandCover =
            cand.cover.begin <= h.aStart && h.aEnd <= cand.cover.end &&
            cand.cover.begin < cand.cover.end;
        if (!pathHasOnlyLiteralCallees(cand)) {
          trace("macro/dag",
                "skip leaf id={0} name='{1}': non-literal callee origin on "
                "path to root id={2}",
                cand.id, cand.name, m.id);
          if (hunkWithinCandCover)
            directRootPreservationInadmissible = true;
          continue;
        }

        // Candidate must have argument-like spans; otherwise there's nothing
        // concrete to map an edit to.
        SmallVector<RefoldModel::PPArgSpan, 8> candArgLike;
        gatherArgLike(cand, candArgLike);
        // Filter out argument-like spans that are structurally invalid. A
        // PPArgSpan is considered invalid if its token-range is empty or
        // inverted, or if its begin/end values fall outside the range of
        // available A-side tokens. Such spans can arise from producer
        // propagation bugs or when a nested invocation was elided; using
        // them would lead to bogus touched indices or out-of-bounds
        // access. Skip them here to avoid crashes downstream (see
        // similar_expansion_paths_ambiguity_path1_path2 regression).
        SmallVector<RefoldModel::PPArgSpan, 8> validArgLike;
        validArgLike.reserve(candArgLike.size());
        const uint64_t maxATokCount = static_cast<uint64_t>(aToks_.size());
        for (const auto &sp : candArgLike) {
          if (sp.end <= sp.begin)
            continue;
          // ensure begin/end are within A-token bounds
          if (sp.begin >= maxATokCount || sp.end > maxATokCount)
            continue;
          validArgLike.push_back(sp);
        }
        candArgLike.swap(validArgLike);
        if (candArgLike.empty()) {
          if (hunkWithinCandCover) {
            trace("macro/dag",
                  "skip leaf id={0} name='{1}': no argument-like spans for "
                  "touched descendant under root id={2}",
                  cand.id, cand.name, m.id);
          }
          continue;
        }

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
        if (!HunkFullyWithinArgSpans(h, candArgLike, candTouchedBySpan))
          continue;

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
        if (!anyTouched)
          continue;

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

        if (!anyDiff)
          continue;

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
          [&](const RefoldModel::MacroInvocation &inv) -> std::optional<std::string> {
        uint64_t covLoA = inv.cover.begin;
        uint64_t covHiA = inv.cover.end;
        if (inv.subkind == "func" && inv.defParams.empty() && !inv.bodySpans.empty()) {
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
          [&](const RefoldModel::MacroInvocation &parent,
              uint32_t parentFormal) -> SmallVector<LexicalChildPlaceholder, 4> {
        SmallVector<LexicalChildPlaceholder, 8> cands;
        auto argInfo = getTrimmedInvocationArgInfo(parent, parentFormal);
        if (!argInfo || !parent.invFile)
          return SmallVector<LexicalChildPlaceholder, 4>{};

        for (const auto &cand : model_.GetMacroInvocations()) {
          if (cand.id == parent.id || !cand.invFile || !cand.invB || !cand.invE)
            continue;
          if (*cand.invFile != *parent.invFile)
            continue;
          if (*cand.invB < argInfo->absTrimBegin || *cand.invE > argInfo->absTrimEnd ||
              *cand.invE <= *cand.invB)
            continue;

          auto olds = expansionTextCandidates(cand, /*fromB=*/false);
          auto news = expansionTextCandidates(cand, /*fromB=*/true);

          SmallVector<WrapperChainCertificate, 4> forms;
          auto addObservedForm = [&](WrapperChainKind kind,
                                     WrapperObservedSource source,
                                     StringRef text,
                                     StringRef logicalInput) {
            std::string observed = text.trim().str();
            std::string logical = logicalInput.trim().str();
            if (observed.empty() || logical.empty())
              return;

            if (kind == WrapperChainKind::StringLiteral ||
                kind == WrapperChainKind::WideStringLiteral) {
              auto canon = CanonicalizeStringifyInversePayload(logical);
              if (!canon || StringRef(*canon).trim() != StringRef(logical).trim())
                return;
              logical = std::move(*canon);
            }

            for (const auto &existing : forms) {
              if (existing.kind == kind &&
                  existing.observedOldText == observed &&
                  existing.logicalInputText == logical)
                return;
            }

            forms.push_back(WrapperChainCertificate{kind, source,
                                                    std::move(observed),
                                                    std::move(logical)});
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
              addObservedForm(WrapperChainKind::WideStringLiteral,
                              WrapperObservedSource::ChildRawInvocation,
                              (Twine("L") + quoteCStringLiteral(rawInvocation))
                                  .str(),
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
              StringRef observedOld0) -> std::optional<ArgInvertibilityCertificate> {
        auto argInfo = getTrimmedInvocationArgInfo(parent, parentFormal);
        if (!argInfo)
          return std::nullopt;

        StringRef rawArg = StringRef(argInfo->text).trim();
        StringRef observedOld = observedOld0.trim();

        ArgInvertibilityCertificate cert;
        cert.rawArgText = rawArg.str();
        auto placeholders = getTopLevelLexicalChildrenInArg(parent, parentFormal);
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
          cert.literals.push_back(rawArg.slice((size_t)curPos, (size_t)ph.relBegin).str());
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
          for (unsigned choice = 0; choice < cert.slots[idx].observedForms.size();
               ++choice) {
            StringRef phOld = cert.slots[idx].observedForms[choice].observedOldText;
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
            if (i >= cert.slots.size() || a[i] >= cert.slots[i].observedForms.size() ||
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
        SlotRewriteDecisionKind kind = SlotRewriteDecisionKind::PassthroughFlatten;
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

          return StringRef(*canonDecoded).trim() == StringRef(*decoded).trim() &&
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
                      slot.newExpansionCandidates, [&](const std::string &cand) {
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
            [&](const SmallVectorImpl<SlotRewriteDecision> &sol) -> std::string {
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
            fallbackCert.decision = SlotRewriteDecision{
                SlotRewriteDecisionKind::PassthroughFlatten, piece.str(),
                piece.str()};
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
          argCert.detail =
              "new arg did not admit any structurally valid slot reconstruction";
          return argCert;
        }

        std::string rebuilt = rebuildFromSolution(newSolutions[0]);
        for (size_t i = 1; i < newSolutions.size(); ++i)
          if (rebuildFromSolution(newSolutions[i]) != rebuilt) {
            argCert.detail =
                "new arg admitted multiple non-equivalent structural reconstructions";
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
        auto invertibilityCert =
            buildArgInvertibilityCertificate(parent, parentFormal, observedOld0);
        if (!invertibilityCert) {
          ArgSemanticRewriteCertificate argCert;
          argCert.failure =
              ArgSemanticRewriteFailure::MissingStructuralTemplate;
          argCert.detail =
              "observed old arg text did not match a unique structural template";
          return argCert;
        }
        return buildArgSemanticRewriteCertificate(*invertibilityCert,
                                                  observedNew0,
                                                  preferredChildSyntax);
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

        if (!MacroArgReplacementMatchesAllOccurrencesInBIgnorePaste(
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

        for (const auto &ps : inv.pasteSpans) {
          if (ps.argIdx == argIdx) {
            cert.touchesPaste = true;
            break;
          }
        }

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

        cert.signature = buildSemanticInteractionSignature(interactions.front());
        for (size_t i = 1; i < interactions.size(); ++i) {
          auto sig = buildSemanticInteractionSignature(interactions[i]);
          if (!(sig == cert.signature)) {
            cert.valid = false;
            cert.failure =
                FormalInteractionConsistencyFailure::DivergentSemanticEvidence;
            cert.detail = formatv(
                              "{0}: inv id={1} name={2} argIdx={3} semantic "
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
              oldTrim.str(), StringRef(argRewriteCert.rawArgNewText).trim().str()});
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
        for (uint32_t parentFormal = 0; parentFormal < parent.invArgRanges.size();
             ++parentFormal) {
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

        auto collectDenseMapKeys = [&](const auto &mapLike) {
          SmallVector<uint32_t, 8> keys;
          keys.reserve(mapLike.size());
          for (const auto &KV : mapLike)
            keys.push_back(KV.first);
          llvm::sort(keys);
          return keys;
        };

        auto collectPasteArgIdxs =
            [&](const RefoldModel::MacroInvocation &MI) {
              SmallVector<uint32_t, 8> argIdxs;
              for (const auto &ps : MI.pasteSpans) {
                if (!llvm::is_contained(argIdxs, ps.argIdx))
                  argIdxs.push_back(ps.argIdx);
              }
              llvm::sort(argIdxs);
              return argIdxs;
            };

        auto computeMissingSupport = [&](ArrayRef<uint32_t> required,
                                        ArrayRef<uint32_t> carried) {
          SmallVector<uint32_t, 8> missing;
          for (uint32_t argIdx : required) {
            if (!llvm::is_contained(carried, argIdx))
              missing.push_back(argIdx);
          }
          llvm::sort(missing);
          return missing;
        };

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
          if (!cert.touchesPaste) {
            for (const auto &ps : inv.pasteSpans) {
              if (ps.argIdx == argIdx) {
                cert.touchesPaste = true;
                break;
              }
            }
          }

          if (oldText == newText)
            continue;

          cert.rewrites.push_back(
              CertifiedFormalRewrite{argIdx, oldText.str(), newText.str()});
        }

        auto providedArgIdxs = argOrder;
        auto carriedArgIdxs = collectDenseMapKeys(cert.replacementByArgIdx);
        SmallVector<uint32_t, 8> changedArgIdxs;
        changedArgIdxs.reserve(cert.rewrites.size());
        for (const auto &rewrite : cert.rewrites)
          changedArgIdxs.push_back(rewrite.argIdx);
        llvm::sort(changedArgIdxs);
        auto requiredPasteArgIdxs = collectPasteArgIdxs(inv);
        auto missingSupportArgIdxs =
            computeMissingSupport(requiredPasteArgIdxs, carriedArgIdxs);

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
          if (auto rewritten = buildRewrittenInvocationSyntax(inv, replByFormal))
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
        auto newRangesOpt = GetMacroInvocationFormalArgContentRanges(m, newText);
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

      auto tryBuildNestedPasteChainDerivation =
          [&](const RefoldModel::MacroInvocation &cur, uint32_t curFormal,
              StringRef curOld, StringRef curNew,
              StringRef traceStage)
          -> std::optional<ParentConstraintDerivationCertificate> {
        auto argText = getInvocationArgText(cur, curFormal);
        trace("macro/dag",
              "{0}: nested pasted-chain derivation enter child id={1} name={2} argIdx={3} argText='{4}' old='{5}' new='{6}'",
              traceStage, cur.id, cur.name, curFormal,
              argText ? argText->trim() : StringRef("<missing>"),
              curOld.trim(), curNew.trim());
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
                          "{0}: nested pasted-chain derivation ambiguous nested child matches parent child id={1} name={2} argIdx={3} matches={4}",
                          traceStage, cur.id, cur.name, curFormal,
                          FormatUInt32List(nestedMatches)),
                    std::nullopt);
          nested = cand;
        }
        if (!nested) {
          trace("macro/dag",
                "{0}: nested pasted-chain derivation found no exact nested child match for child id={1} name={2} argIdx={3}",
                traceStage, cur.id, cur.name, curFormal);
          return std::nullopt;
        }
        trace("macro/dag",
              "{0}: nested pasted-chain derivation matched nested child id={1} name={2} argIdx={3} nestedId={4} nestedName={5} nestedInv='{6}'",
              traceStage, cur.id, cur.name, curFormal, nested->id, nested->name,
              nested->invText ? StringRef(*nested->invText).trim() : StringRef("<none>"));

        SmallVector<std::string, 4> oldExpansionCandidates =
            expansionTextCandidates(*nested, /*fromB=*/false);
        SmallVector<std::string, 4> matchingOldCandidates;
        for (const auto &cand : oldExpansionCandidates) {
          if (StringRef(cand).trim() == curOld.trim())
            matchingOldCandidates.push_back(cand);
        }
        trace("macro/dag",
              "{0}: nested pasted-chain derivation old expansion candidates nestedId={1} total={2} matchingOld={3} curOld='{4}'",
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
              "{0}: nested pasted-chain derivation paste groups nestedId={1} pasteSpanCount={2} groupCount={3}",
              traceStage, nested->id, (uint64_t)nested->pasteSpans.size(),
              (uint64_t)pasteGroups.size());
        if (pasteGroups.size() != 1)
          return std::nullopt;

        auto &group = pasteGroups.begin()->second;
        if (group.size() < 2)
          return std::nullopt;

        llvm::sort(group, [](const RefoldModel::PPArgSpan *a,
                             const RefoldModel::PPArgSpan *b) {
          if (*a->byteBegin != *b->byteBegin)
            return *a->byteBegin < *b->byteBegin;
          if (*a->byteEnd != *b->byteEnd)
            return *a->byteEnd < *b->byteEnd;
          return a->argIdx < b->argIdx;
        });

        StringRef oldTok = curOld.trim();
        StringRef newTok = curNew.trim();
        if (oldTok.empty() || newTok.empty())
          return std::nullopt;

        const uint64_t oldLen = oldTok.size();
        for (size_t i = 0; i < group.size(); ++i) {
          const auto *sp = group[i];
          if (*sp->byteBegin > *sp->byteEnd || *sp->byteEnd > oldLen)
            return std::nullopt;
          if (i > 0 && *group[i - 1]->byteEnd > *sp->byteBegin)
            return std::nullopt;
        }

        StringRef leading = oldTok.take_front(*group.front()->byteBegin);
        StringRef trailing = oldTok.drop_front(*group.back()->byteEnd);
        if (!newTok.starts_with(leading) || !newTok.ends_with(trailing))
          return std::nullopt;

        SmallVector<StringRef, 4> oldSegs;
        SmallVector<StringRef, 4> midBodies;
        oldSegs.reserve(group.size());
        midBodies.reserve(group.size() - 1);
        for (size_t i = 0; i < group.size(); ++i) {
          const auto *sp = group[i];
          oldSegs.push_back(oldTok.slice(*sp->byteBegin, *sp->byteEnd));
          if (i + 1 < group.size()) {
            StringRef mid =
                oldTok.slice(*sp->byteEnd, *group[i + 1]->byteBegin);
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

        trace("macro/dag",
              "{0}: nested pasted-chain derivation split summary nestedId={1} groupSize={2} splitSolutions={3}",
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
                "{0}: nested pasted-chain derivation chosen split nestedId={1} split={2}",
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
        llvm::sort(cert.derivedConstraints,
                   [](const auto &a, const auto &b) {
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
                "{0}: nested pasted-chain derivation success child id={1} name={2} argIdx={3} derived={4}",
                traceStage, cur.id, cur.name, curFormal, os.str());
        }
        cert.valid = true;
        return cert;
      };


      auto tryBuildTwoParentDelimitedDerivation =
          [&](const RefoldModel::MacroInvocation &cur, uint32_t curFormal,
              StringRef curOld, StringRef curNew,
              StringRef traceStage)
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
              "{0}: two-parent delimited derivation enter child id={1} name={2} argIdx={3} deps=[{4}, {5}] old='{6}' new='{7}' oldA='{8}' oldB='{9}'",
              traceStage, cur.id, cur.name, curFormal, deps[0], deps[1],
              oldTok, newTok, oldA, oldB);
        if (oldTok.empty() || newTok.empty() || oldA.empty() || oldB.empty())
          return std::nullopt;
        if (!oldTok.starts_with(oldA) || !oldTok.ends_with(oldB) ||
            oldTok.size() < oldA.size() + oldB.size()) {
          trace("macro/dag",
                "{0}: two-parent delimited derivation rejected shape child id={1} name={2} argIdx={3} old='{4}' oldA='{5}' oldB='{6}' startsWithA={7} endsWithB={8} sizeOk={9}",
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
          if (countSubstr(newA, mid) < needA ||
              countSubstr(newB, mid) < needB)
            continue;
          splits.push_back({newA, newB});
        }

        trace("macro/dag",
              "{0}: two-parent delimited derivation split summary child id={1} name={2} argIdx={3} mid='{4}' splitCount={5}",
              traceStage, cur.id, cur.name, curFormal, mid,
              (uint64_t)splits.size());
        if (splits.size() != 1)
          return std::nullopt;

        ParentConstraintDerivationCertificate cert;
        cert.childFormal = curFormal;
        cert.valid = true;
        cert.derivedConstraints.push_back(
            {deps[0], ObservedFormalConstraint{oldA.str(), splits[0].first.trim().str()}});
        cert.derivedConstraints.push_back(
            {deps[1], ObservedFormalConstraint{oldB.str(), splits[0].second.trim().str()}});
        cert.detail = formatv(
                          "{0}: child id={1} name={2} argIdx={3} accepted via two-parent delimited derivation",
                          traceStage, cur.id, cur.name, curFormal)
                          .str();
        trace("macro/dag",
              "{0}: two-parent delimited derivation success child id={1} name={2} argIdx={3} derived={{{4}:'{5}'->'{6}', {7}:'{8}'->'{9}'}}",
              traceStage, cur.id, cur.name, curFormal,
              deps[0], oldA, splits[0].first.trim(),
              deps[1], oldB, splits[0].second.trim());
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
          trace("macro/dag", "{0}: derivation failure context: {1}",
                traceStage, formatCurFormalContext());
          cert.detail = formatv(
                            "{0}: child id={1} name={2} argIdx={3} missing "
                            "argDeps entry; lexical bridge required",
                            traceStage, cur.id, cur.name, curFormal)
                            .str();
          return cert;
        }
        ArrayRef<uint32_t> deps = cur.argDeps[curFormal];
        if (deps.empty()) {
          cert.failure = ParentConstraintDerivationFailure::EmptyArgDeps;
          trace("macro/dag", "{0}: derivation failure context: {1}",
                traceStage, formatCurFormalContext());
          cert.detail = formatv(
                            "{0}: child id={1} name={2} argIdx={3} has "
                            "empty argDeps; lexical bridge required",
                            traceStage, cur.id, cur.name, curFormal)
                            .str();
          return cert;
        }

        if (curFormal >= cur.argRefs.size()) {
          cert.failure = ParentConstraintDerivationFailure::MissingArgRefs;
          trace("macro/dag", "{0}: derivation failure context: {1}",
                traceStage, formatCurFormalContext());
          cert.detail = formatv(
                            "{0}: child id={1} name={2} argIdx={3} missing "
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
                  "{0}: derivation accepted via two-parent delimited split child id={1} name={2} argIdx={3} detail={4}",
                  traceStage, cur.id, cur.name, curFormal,
                  twoParentCert->detail);
            return *twoParentCert;
          }
          if (auto nestedCert = tryBuildNestedPasteChainDerivation(
                  cur, curFormal, curOld, curNew, traceStage)) {
            trace("macro/dag",
                  "{0}: derivation accepted via nested pasted-chain child id={1} name={2} argIdx={3} detail={4}",
                  traceStage, cur.id, cur.name, curFormal, nestedCert->detail);
            return *nestedCert;
          }
          cert.failure =
              ParentConstraintDerivationFailure::TemplateNotCertifiable;
          trace("macro/dag", "{0}: derivation failure context: {1}",
                traceStage, formatCurFormalContext());
          cert.detail = formatv(
                            "{0}: child id={1} name={2} argIdx={3} arg-ref "
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
                  "{0}: derivation accepted via two-parent delimited split child id={1} name={2} argIdx={3} detail={4}",
                  traceStage, cur.id, cur.name, curFormal,
                  twoParentCert->detail);
            return *twoParentCert;
          }
          if (auto nestedCert = tryBuildNestedPasteChainDerivation(
                  cur, curFormal, curOld, curNew, traceStage)) {
            trace("macro/dag",
                  "{0}: derivation accepted via nested pasted-chain child id={1} name={2} argIdx={3} detail={4}",
                  traceStage, cur.id, cur.name, curFormal, nestedCert->detail);
            return *nestedCert;
          }
          cert.failure = ParentConstraintDerivationFailure::InversionNotUnique;
          trace("macro/dag", "{0}: derivation failure context: {1}",
                traceStage, formatCurFormalContext());
          cert.detail = formatv(
                            "{0}: child id={1} name={2} argIdx={3} arg-ref "
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
            cert.detail = formatv(
                              "{0}: child id={1} name={2} argIdx={3} "
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
              StringRef curOld, StringRef curNew)
          -> std::optional<StructuredLiftCertificate> {
        const StringRef oldTrim = curOld.trim();
        const StringRef newTrim = curNew.trim();
        if (oldTrim.empty() || newTrim.empty())
          return std::nullopt;

        const RefoldModel::MacroInvocation *matchedSibling = nullptr;
        trace("macro/dag",
              "DAG per-hop exact sibling reroot candidate scan: child id={0} name={1} parent id={2} name={3} curFormal={4} old='{5}' new='{6}'",
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
                  "DAG per-hop exact sibling reroot ambiguous sibling match: child id={0} name={1} parent id={2} name={3} old='{4}' firstSibling={5} secondSibling={6}",
                  cur.id, cur.name, parent.id, parent.name, oldTrim,
                  matchedSibling->id, cand.id);
            return std::nullopt;
          }
          matchedSibling = &cand;
        }

        if (!matchedSibling || matchedSibling->invArgRanges.empty()) {
          trace("macro/dag",
                "DAG per-hop exact sibling reroot no usable sibling: child id={0} name={1} parent id={2} name={3} matchedSibling={4}",
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
                "DAG per-hop exact sibling reroot matched sibling expansion candidates: child id={0} name={1} sibling id={2} name={3} curFormal={4} rawOld='{5}' expansions={6}",
                cur.id, cur.name, matchedSibling->id, matchedSibling->name,
                curFormal, oldTrim, os.str());

          if (curFormal < cur.argDeps.size() && cur.argDeps[curFormal].size() == 2 &&
              siblingOldExpansionCandidates.size() == 1) {
            ArrayRef<uint32_t> deps = cur.argDeps[curFormal];
            auto oldAOpt = getInvocationArgText(parent, deps[0]);
            auto oldBOpt = getInvocationArgText(parent, deps[1]);
            if (oldAOpt && oldBOpt) {
              StringRef oldExp = StringRef(siblingOldExpansionCandidates[0]).trim();
              StringRef oldA = oldAOpt->trim();
              StringRef oldB = oldBOpt->trim();
              bool factors = oldExp.starts_with(oldA) && oldExp.ends_with(oldB) &&
                             oldExp.size() >= oldA.size() + oldB.size();
              std::string midStr;
              uint64_t splitCount = 0;
              if (factors) {
                StringRef mid = oldExp.slice(oldA.size(), oldExp.size() - oldB.size());
                midStr = mid.str();
                if (!mid.empty()) {
                  const uint64_t needA = countSubstr(oldA, mid);
                  const uint64_t needB = countSubstr(oldB, mid);
                  for (size_t pos = 0; (pos = newTrim.find(mid, pos)) != StringRef::npos; ++pos) {
                    StringRef newA = newTrim.slice(0, pos);
                    StringRef newB = newTrim.drop_front(pos + mid.size());
                    if (countSubstr(newA, mid) < needA || countSubstr(newB, mid) < needB)
                      continue;
                    ++splitCount;
                  }
                }
              }
              trace("macro/dag",
                    "DAG per-hop exact sibling reroot counterfactual sibling-output split: child id={0} name={1} sibling id={2} name={3} curFormal={4} oldExp='{5}' parentOldA='{6}' parentOldB='{7}' factors={8} mid='{9}' splitCount={10}",
                    cur.id, cur.name, matchedSibling->id, matchedSibling->name,
                    curFormal, oldExp, oldA, oldB, factors, midStr, splitCount);
            }
          }
        }

        std::optional<StructuredLiftCertificate> uniqueLift;
        std::optional<uint32_t> uniqueSiblingFormal;
        std::string uniqueSiblingOld;
        for (uint32_t siblingFormal = 0;
             siblingFormal < matchedSibling->invArgRanges.size();
             ++siblingFormal) {
          auto siblingOldArg = getInvocationArgText(*matchedSibling, siblingFormal);
          if (!siblingOldArg) {
            trace("macro/dag",
                  "DAG per-hop exact sibling reroot skip sibling formal: sibling id={0} name={1} siblingFormal={2} reason=noArgText",
                  matchedSibling->id, matchedSibling->name, siblingFormal);
            continue;
          }

          const StringRef siblingOldTrim = siblingOldArg->trim();
          if (siblingOldTrim.empty() || siblingOldTrim == newTrim) {
            trace("macro/dag",
                  "DAG per-hop exact sibling reroot skip sibling formal: sibling id={0} name={1} siblingFormal={2} siblingOld='{3}' reason=emptyOrNoChange",
                  matchedSibling->id, matchedSibling->name, siblingFormal,
                  siblingOldTrim);
            continue;
          }

          trace("macro/dag",
                "DAG per-hop exact sibling reroot try sibling formal: sibling id={0} name={1} siblingFormal={2} siblingOld='{3}' new='{4}'",
                matchedSibling->id, matchedSibling->name, siblingFormal,
                siblingOldTrim, newTrim);

          DenseMap<uint32_t, FormalTextPair> siblingFormals;
          siblingFormals[siblingFormal] =
              FormalTextPair{siblingOldTrim.str(), newTrim.str()};
          auto siblingLift =
              buildStructuredLiftCertificate(*matchedSibling, siblingFormals);
          trace("macro/dag",
                "DAG per-hop exact sibling reroot sibling formal result: sibling id={0} name={1} siblingFormal={2} stepKind={3} nextInv={4} detail={5}",
                matchedSibling->id, matchedSibling->name, siblingFormal,
                static_cast<unsigned>(siblingLift.kind),
                siblingLift.nextInv ? siblingLift.nextInv->id : 0,
                siblingLift.detail);
          if (siblingLift.kind != StructuredLiftCertificateKind::Unique ||
              siblingLift.nextInv != &parent)
            continue;

          if (uniqueLift) {
            trace("macro/dag",
                  "DAG per-hop exact sibling reroot ambiguous sibling-formal seed: sibling id={0} name={1} firstFormal={2} secondFormal={3}",
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
            [&](const DenseMap<uint32_t,
                               SmallVector<ObservedFormalConstraint, 2>>
                    &observed) -> std::string {
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

        auto collectObservedKeys =
            [&](const DenseMap<uint32_t,
                               SmallVector<ObservedFormalConstraint, 2>>
                    &observed) {
              SmallVector<uint32_t, 8> keys;
              keys.reserve(observed.size());
              for (const auto &KV : observed)
                keys.push_back(KV.first);
              llvm::sort(keys);
              return keys;
            };

        auto collectPasteArgIdxs =
            [&](const RefoldModel::MacroInvocation &MI) {
              SmallVector<uint32_t, 8> argIdxs;
              for (const auto &ps : MI.pasteSpans) {
                if (!llvm::is_contained(argIdxs, ps.argIdx))
                  argIdxs.push_back(ps.argIdx);
              }
              llvm::sort(argIdxs);
              return argIdxs;
            };

        auto collectDenseMapKeys = [&](const auto &mapLike) {
          SmallVector<uint32_t, 8> keys;
          keys.reserve(mapLike.size());
          for (const auto &KV : mapLike)
            keys.push_back(KV.first);
          llvm::sort(keys);
          return keys;
        };

        auto computeMissingSupport = [&](ArrayRef<uint32_t> required,
                                        ArrayRef<uint32_t> carried) {
          SmallVector<uint32_t, 8> missing;
          for (uint32_t argIdx : required) {
            if (!llvm::is_contained(carried, argIdx))
              missing.push_back(argIdx);
          }
          llvm::sort(missing);
          return missing;
        };

        trace("macro/dag",
              "DAG per-hop enter: child id={0} name={1} curFormals={2}",
              cur.id, cur.name, formatFormalTextPairs(curFormals));

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
        } else if (auto curSyntax = buildRewrittenInvocationSyntax(
                       cur, curFormalSyntax)) {
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
          cert.detail = formatv(
                            "DAG per-hop: child id={0} name={1} requires "
                            "lexical bridge to root",
                            cur.id, cur.name)
                            .str();
          return cert;
        }

        auto tryBuildDirectPassthroughParentFormalRewrite =
            [&](uint32_t curFormal, uint32_t parentFormal,
                StringRef curNewText)
            -> std::optional<FormalTextPair> {
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

          trace("macro/dag",
                "DAG per-hop parent formal passthrough flatten candidate: child id={0} name={1} parent id={2} name={3} sourceCurFormal={4} parentFormal={5} childObservedOld='{6}' childObservedNew='{7}' parentLogicalOld='{8}' supportLoss={9}",
                cur.id, cur.name, parent->id, parent->name, curFormal,
                parentFormal, StringRef(curFormals.lookup(curFormal).oldText).trim(),
                newTrim, oldTrim,
                StringRef(curFormals.lookup(curFormal).oldText).trim() != oldTrim);

          return FormalTextPair{oldTrim.str(), newTrim.str()};
        };

        DenseMap<uint32_t, SmallVector<ObservedFormalConstraint, 2>>
            parentObserved;
        DenseMap<uint32_t, SmallVector<uint32_t, 2>> parentObservedSources;
        SmallVector<uint32_t, 4> unresolvedChildFormals;
        SmallVector<std::string, 4> unresolvedDerivationDetails;
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
                  if (llvm::find(sources, curFormal) == sources.end())
                    sources.push_back(curFormal);
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
            if (llvm::find(sources, curFormal) == sources.end())
              sources.push_back(curFormal);
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

        auto observedParentArgIdxs = collectObservedKeys(parentObserved);
        auto requiredParentPasteArgIdxs = collectPasteArgIdxs(*parent);
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
                        sourceCurFormal, parentFormal,
                        curIt->second.newText)) {
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
            cert.detail = formatv(
                              "{0}; lexical bridge required",
                              formalCert.detail)
                              .str();
            cert.kind = StructuredLiftCertificateKind::NeedsLexicalBridge;
            cert.nextInv = parent;
            return cert;
          }
          if (formalCert.kind == FormalRewriteCertificateKind::NoChange) {
            trace("macro/dag",
                  "DAG per-hop parent formal no-change preserved in nextFormals: child id={0} name={1} parent id={2} name={3} parentFormal={4} old='{5}' new='{6}'",
                  cur.id, cur.name, parent->id, parent->name, parentFormal,
                  formalCert.oldText, formalCert.newText);
          }

          parentFormals[parentFormal] =
              FormalTextPair{formalCert.oldText, formalCert.newText};
        }

        auto carriedParentArgIdxs = collectDenseMapKeys(parentFormals);
        auto missingParentSupportArgIdxs =
            computeMissingSupport(requiredParentPasteArgIdxs, carriedParentArgIdxs);
        trace("macro/proof",
              "DAG per-hop proof ledger carried: child id={0} name={1} "
              "parent id={2} name={3} observedParentArgs={4} carriedParentArgs={5} "
              "requiredParentPasteArgs={6} missingSupport={7} unresolvedChildFormals={8}",
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
          cert.detail = unresolvedDerivationDetails.empty()
                            ? formatv("DAG per-hop: unresolved parent "
                                      "constraint derivation requires "
                                      "lexical bridge: child id={0} name={1} "
                                      "parent id={2} name={3}",
                                      cur.id, cur.name, parent->id,
                                      parent->name)
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
          cert.detail = formatv(
                            "DAG per-hop: parent invocation no-change child "
                            "id={0} name={1} parent id={2} name={3} "
                            "parentFormals={4}",
                            cur.id, cur.name, parent->id, parent->name,
                            parentFormals.size())
                            .str();
        } else {
          cert.detail = formatv(
                            "DAG per-hop: structured hop child id={0} name={1} "
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
        DeferredPasteDischargeFailure failure = DeferredPasteDischargeFailure::None;
        SmallVector<const InvocationRewriteCertificate *, 4> deferredInvocations;
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

        cert.detail = formatv(
                          "subtree interaction summary: interactions={0} "
                          "paste={1} stringify={2} wide={3} rawInvocation={4} "
                          "childSyntax={5} stringifyPaste={6} "
                          "wideStringifyPaste={7} rawInvocationPaste={8} "
                          "childSyntaxPaste={9} mixed={10}",
                          cert.interactions.size(), cert.hasPaste ? 1 : 0,
                          cert.hasStringify ? 1 : 0,
                          cert.hasWideStringify ? 1 : 0,
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
              invCert.inv && rootCert.inv &&
              invCert.inv->id == rootCert.inv->id && rootCert.pasteValidation.valid;

          trace("macro/dag",
                "subtree deferred discharge probe: deferredInv id={0} name={1} rootInv id={2} name={3} rootKind={4} rootPasteRequired={5} rootPasteValid={6} rootPasteDeferred={7} hasSemantic={8} hasAncestorReplay={9} hasAcceptedRootReplay={10}",
                invCert.inv ? invCert.inv->id : 0,
                invCert.inv ? invCert.inv->name : StringRef("<none>"),
                rootCert.inv ? rootCert.inv->id : 0,
                rootCert.inv ? rootCert.inv->name : StringRef("<none>"),
                static_cast<unsigned>(rootCert.kind),
                rootCert.pasteValidation.required ? 1 : 0,
                rootCert.pasteValidation.valid ? 1 : 0,
                rootCert.pasteValidation.deferred ? 1 : 0,
                hasSemanticDischarge ? 1 : 0,
                hasAncestorReplayPath ? 1 : 0,
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
          for (const auto &lift : semantic.liftChains)
            trace("macro/dag",
                  "  lift chain detail: leaf id={0} name={1} leafArgs={2} "
                  "usedLexicalBridge={3} detail={4}",
                  lift.leaf ? lift.leaf->id : 0,
                  lift.leaf ? lift.leaf->name : StringRef("<none>"),
                  FormatUInt32List(lift.leafArgIdxs),
                  lift.usedLexicalBridge ? 1 : 0,
                  lift.detail);
          for (const auto &step : semantic.structuredLiftCertificates)
            trace("macro/dag",
                  "  structured step detail: nextInv={0} kind={1} detail={2} "
                  "rewrittenChildSyntax='{3}'",
                  step.nextInv ? step.nextInv->id : 0,
                  static_cast<unsigned>(step.kind), step.detail,
                  step.rewrittenChildSyntax);
          cert.valid = false;
          cert.failure = SubtreeSemanticAdmissibilityFailure::
              LexicalBridgeWithStructuredSemantics;
          cert.detail =
              "subtree semantic admissibility failed: bridge-sensitive "
              "wrapper/raw-invocation/paste subtree semantics remain "
              "inadmissible";
          return cert;
        }

        cert.detail = formatv(
                          "subtree semantic admissibility: lexicalBridge={0} "
                          "mixed={1} structuredSemantics={2} "
                          "bridgeSensitiveStructuredSemantics={3} "
                          "bridgedFormals={4} bridgedInteractions={5}",
                          semantic.usesLexicalBridge ? 1 : 0,
                          semantic.interactionSummary.hasMixedInteractions ? 1
                                                                       : 0,
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
                  cert.bridgedInteractionKeys.insert(
                      makeFormalKey(interactionCert.inv, interactionCert.argIdx));
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
        cert.interactionSummary =
            buildSubtreeInteractionSummaryCertificate(cert.interactionCertificates);
        cert.interactionConsistency =
            buildSubtreeInteractionConsistencyCertificate(cert.formalCertificates);
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
        cert.detail = formatv(
                          "subtree semantic bundle: invCerts={0} formalCerts={1} "
                          "argCerts={2} slotCerts={3} interactions={4} "
                          "formalConsistency={5} derivations={6} liftSteps={7} "
                          "rootMerges={8} lexicalBridge={9} paste={10} "
                          "wrappers={11} admissible={12} deferred={13} "
                          "bridgedFormals={14} bridgedInteractions={15} "
                          "bridgeSensitiveStructuredSemantics={16}",
                          cert.invocationCertificates.size(),
                          cert.formalCertificates.size(),
                          cert.argCertificates.size(),
                          cert.slotCertificates.size(),
                          cert.interactionCertificates.size(),
                          cert.formalInteractionConsistencies.size(),
                          cert.parentDerivations.size(),
                          cert.structuredLiftCertificates.size(),
                          cert.rootMergeCertificates.size(),
                          cert.usesLexicalBridge ? 1 : 0,
                          cert.touchesPaste ? 1 : 0,
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
              bool deferLeafPasteValidation)
          -> SubtreeRewriteCertificate {
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

        cert.leafCert =
            buildWrapperPlaceholderHopInvocationCertificate(
                leaf, pendingLeafFormals, "DAG subtree leaf");
        if (cert.leafCert.pasteValidation.deferred && deferLeafPasteValidation) {
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
          cert.detail = formatv(
                            "DAG subtree: leaf invocation no-change leaf "
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
                                        FormatUInt32List(
                                            liftCert.leafArgIdxs))
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
          cert.detail = formatv(
                            "DAG subtree: no lifted root formals root id={0} "
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
                  "DAG subtree root merge no-change preserved for replay: root id={0} name={1} argIdx={2} old='{3}' new='{4}'",
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
          if (cert.rootCert.failure == InvocationRewriteFailure::PasteMismatch) {
            auto wrapperProbe = buildWrapperPlaceholderHopInvocationCertificate(
                m, pendingRootFormals, "DAG subtree root probe");
            trace("macro/dag",
                  "DAG subtree root probe: plain root cert INVALID due to paste mismatch root id={0} name={1} pendingRootFormals={2} wrapperProbeKind={3} wrapperPasteRequired={4} wrapperPasteValid={5} wrapperPasteDeferred={6} wrapperSyntax='{7}' detail={8}",
                  m.id, m.name, pendingRootFormals.size(),
                  static_cast<unsigned>(wrapperProbe.kind),
                  wrapperProbe.pasteValidation.required ? 1 : 0,
                  wrapperProbe.pasteValidation.valid ? 1 : 0,
                  wrapperProbe.pasteValidation.deferred ? 1 : 0,
                  wrapperProbe.rewrittenInvocationSyntax,
                  wrapperProbe.detail);
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
          cert.detail = formatv(
                            "DAG subtree: root invocation no-change root "
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
                "leafPasteRequired={7} leafPasteDeferred={8} rootPasteRequired={9} "
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
              "DAG subtree root cert summary before semantic: root id={0} name={1} rootCertInv={2} rootCertName={3} rootKind={4} rootPasteRequired={5} rootPasteValid={6} rootPasteDeferred={7} rootRewriteCount={8}",
              m.id, m.name,
              cert.rootCert.inv ? cert.rootCert.inv->id : 0,
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
          trace("macro/dag",
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
      // constraints for one formal collapse to the same normalized old/new text,
      // certify that exact uniform observed rewrite as the leaf seed and then
      // continue through the structured lift/root-certificate pipeline. This
      // does not accept a root patch by itself; it only certifies the leaf-side
      // semantic rewrite when direct raw leaf-formal certification is
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
          cert.detail = formatv(
                            "{0}: uniform observed leaf seed unavailable: inv "
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
            cert.detail = formatv(
                              "{0}: uniform observed leaf seed unavailable: "
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
          cert.detail = formatv(
                            "{0}: uniform observed leaf seed collapsed to "
                            "no-change inv id={1} name={2} argIdx={3}",
                            traceStage, inv.id, inv.name, argIdx)
                            .str();
          return cert;
        }

        cert.kind = UniformObservedLeafSeedCertificateKind::Unique;
        cert.detail = formatv(
                          "{0}: uniform observed leaf seed certified inv id={1} "
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
                  stringutils::showWSWithClip(baseArgText, 120), KV.second.size());
            continue;
          }

          merged.expectedRootFormals[argIdx] =
              FormalTextPair{baseArgText.str(),
                             StringRef(*mergedArgText).trim().str()};
        }

        merged.hasExpectedRootFormals = true;
        return merged;
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
              [&](uint32_t argIdx, const FormalTextPair &expected)
              -> bool {
            if (argIdx >= invArgRanges.size())
              return false;

            auto newRangesOpt = GetMacroInvocationFormalArgContentRanges(m, newText);
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

          auto newRangesOpt = GetMacroInvocationFormalArgContentRanges(m, newText);
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
                if (oldR.first <= oldR.second && oldR.second <= baseText.size() &&
                    newR.first <= newR.second && newR.second <= newText.size()) {
                  StringRef concreteOld =
                      baseText.slice((size_t)oldR.first, (size_t)oldR.second).trim();
                  StringRef concreteNew =
                      newText.slice((size_t)newR.first, (size_t)newR.second).trim();
                  if (concreteOld == concreteNew &&
                      concreteOld == StringRef(KV.second.oldText).trim() &&
                      concreteNew == StringRef(KV.second.newText).trim()) {
                    unchangedConcreteMissingArgIdxs.push_back(KV.first);
                  }
                  trace("macro/proof",
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
        SmallVector<uint32_t, 8> deferredArgs = validation.deferOccurrenceArgIdxs;
        llvm::sort(deferredArgs);
        trace("macro/proof",
              "{0}: DAG candidate proof ledger enter root id={1} name={2} "
              "expectedRootArgs={3} deferredArgs={4} bridgeSensitive={5} mixed={6}",
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
        trace("macro/proof",
              "{0}: DAG candidate proof ledger replay root id={1} name={2} "
              "replayRootArgs={3} replayPasteRequired={4} replayPasteValid={5} "
              "replayPasteDeferred={6}",
              traceStage, m.id, m.name, FormatUInt32List(replayRootArgIdxs),
              proofCert.replayInvocationCertificate.pasteValidation.required ? 1 : 0,
              proofCert.replayInvocationCertificate.pasteValidation.valid ? 1 : 0,
              proofCert.replayInvocationCertificate.pasteValidation.deferred ? 1 : 0);
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
          [&](MacroPatch candPatch, StringRef baseText,
              StringRef traceStage,
              const DagCandidateValidationMetadata *candValidation = nullptr)
          -> DagCandidateAcceptanceCertificate {
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
          auto mergedValidation = mergeDagCandidateValidationMetadata(
              uniquePatchValidation, candidateValidation);
          if (!mergedValidation) {
            cert.failure = DagCandidateAcceptanceFailure::MergedRootValidationFailed;
            cert.detail = formatv(
                              "{0}: DAG candidate patch rejected root id={1} "
                              "name={2} equivalent replacement produced "
                              "incompatible root-formal validation metadata",
                              traceStage, m.id, m.name)
                              .str();
            return cert;
          }

          if (!validateDagCandidateProof(*mergedValidation,
                                         *uniquePatchBaseText,
                                         uniquePatch->replacement,
                                         traceStage)) {
            cert.failure = DagCandidateAcceptanceFailure::MergedRootValidationFailed;
            cert.detail = formatv(
                              "{0}: DAG candidate patch rejected root id={1} "
                              "name={2} equivalent replacement failed merged "
                              "root validation",
                              traceStage, m.id, m.name)
                              .str();
            return cert;
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
          cert.detail = formatv(
                            "{0}: DAG candidate patch rejected root id={1} "
                            "name={2} base text mismatch baseLenExisting={3} "
                            "baseLenCandidate={4}",
                            traceStage, m.id, m.name,
                            uniquePatchBaseText ? uniquePatchBaseText->size() : 0,
                            baseText.size())
                            .str();
          return cert;
        }

        SmallVector<StringRef, 2> repls;
        repls.push_back(StringRef(uniquePatch->replacement));
        repls.push_back(StringRef(candPatch.replacement));
        auto merged = mergeCompatibleCallsitePatchReplacements(
            *uniquePatchBaseText, ArrayRef<StringRef>(repls));
        if (!merged) {
          cert.failure = DagCandidateAcceptanceFailure::MergeConflict;
          cert.detail = formatv(
                            "{0}: DAG candidate patch rejected root id={1} "
                            "name={2} incompatible replacement hunks",
                            traceStage, m.id, m.name)
                            .str();
          return cert;
        }

        auto mergedValidation = mergeDagCandidateValidationMetadata(
            uniquePatchValidation, candidateValidation);
        if (!mergedValidation) {
          cert.failure = DagCandidateAcceptanceFailure::MergedRootValidationFailed;
          cert.detail = formatv(
                            "{0}: DAG candidate patch rejected root id={1} "
                            "name={2} merged replacement produced "
                            "incompatible root-formal validation metadata",
                            traceStage, m.id, m.name)
                            .str();
          return cert;
        }

        if (!validateDagCandidateProof(*mergedValidation,
                                       *uniquePatchBaseText,
                                       StringRef(*merged), traceStage)) {
          cert.failure =
              DagCandidateAcceptanceFailure::MergedRootValidationFailed;
          cert.detail = formatv(
                            "{0}: DAG candidate patch rejected root id={1} "
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
        cert.detail = formatv(
                          "{0}: merged DAG candidate root patch root id={1} "
                          "name={2} distinctRootPatches={3} replLen={4}",
                          traceStage, m.id, m.name, distinctRootPatches,
                          uniquePatch->replacement.size())
                          .str();
        return cert;
      };

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
          if (group.size() < 2) {
            invalid = true;
            break;
          }

          llvm::sort(group, [](const RefoldModel::PPArgSpan *a,
                               const RefoldModel::PPArgSpan *b) {
            if (!a->byteBegin || !b->byteBegin)
              return a < b;
            if (*a->byteBegin != *b->byteBegin)
              return *a->byteBegin < *b->byteBegin;
            if (!a->byteEnd || !b->byteEnd)
              return a < b;
            if (*a->byteEnd != *b->byteEnd)
              return *a->byteEnd < *b->byteEnd;
            return a->argIdx < b->argIdx;
          });

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
          for (size_t i = 0; i < group.size(); ++i) {
            const auto *sp = group[i];
            oldSegs.push_back(oldTok.slice(*sp->byteBegin, *sp->byteEnd));
            if (i + 1 < group.size()) {
              StringRef mid = oldTok.slice(*sp->byteEnd, *group[i + 1]->byteBegin);
              if (mid.empty()) {
                groupOk = false;
                break;
              }
              midBodies.push_back(mid);
            }
          }
          if (!groupOk) {
            invalid = true;
            break;
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

          auto splitCore = [&](auto &&self, size_t delimIdx, StringRef rest) -> void {
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
              splitSolutions[0].size() != group.size()) {
            invalid = true;
            break;
          }

          auto recordLeafEdit = [&](uint32_t argIdx, StringRef oldText,
                                    StringRef newText) -> bool {
            return recordLeafObserved(argIdx, oldText, newText);
          };

          // Normalize each recovered segment and record it as a per-formal edit.
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

        // Paste-aware leaf certificate: when multiple leaf-formal rewrites
        // participate in the same pasted token, validate them as a group
        // against the B-side pasted-token spellings before attempting to lift
        // them up the caller chain.
        if (!leaf.pasteSpans.empty()) {
          DenseMap<uint32_t, std::string> leafReplByArgIdx;
          bool leafTouchesPaste = false;
          for (const auto &KV : leafEdits) {
            leafReplByArgIdx[KV.first] = KV.second.newText;
            if (!leafTouchesPaste) {
              for (const auto &ps : leaf.pasteSpans) {
                if (ps.argIdx == KV.first) {
                  leafTouchesPaste = true;
                  break;
                }
              }
            }
          }

          if (leafTouchesPaste) {
            bool allTouchedPasteArgsFromObservedLeafSeed = true;
            for (const auto &KV : leafEdits) {
              bool argTouchesPaste = false;
              for (const auto &ps : leaf.pasteSpans) {
                if (ps.argIdx == KV.first) {
                  argTouchesPaste = true;
                  break;
                }
              }
              if (argTouchesPaste && !observedLeafSeedArgIdxs.count(KV.first)) {
                allTouchedPasteArgsFromObservedLeafSeed = false;
                break;
              }
            }

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
        if (!leaf.pasteSpans.empty()) {
          bool leafTouchesPaste = false;
          bool allTouchedPasteArgsFromObservedLeafSeed = true;
          for (const auto &KV : leafEdits) {
            bool argTouchesPaste = false;
            for (const auto &ps : leaf.pasteSpans) {
              if (ps.argIdx == KV.first) {
                argTouchesPaste = true;
                leafTouchesPaste = true;
                break;
              }
            }
            if (argTouchesPaste && !observedLeafSeedArgIdxs.count(KV.first)) {
              allTouchedPasteArgsFromObservedLeafSeed = false;
              break;
            }
          }
          deferLeafPasteValidation =
              leafTouchesPaste && allTouchedPasteArgsFromObservedLeafSeed;
        }

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

                MacroPatch candPatch{*invStart, chainEnd, replText};

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
              subtreeCert.semantic.interactionSummary.hasMixedInteractions ? 1 : 0,
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
        if (rootPatchCert.kind == RootPatchConstructionCertificateKind::Invalid ||
            rootPatchCert.kind == RootPatchConstructionCertificateKind::NoChange) {
          if (!rootPatchCert.detail.empty())
            trace("macro/dag", "{0}", rootPatchCert.detail);
          trace("macro/dag",
                "DAG subtree root patch not constructible: expanding root id={0} "
                "name={1} leaf id={2} name={3} rootPatchKind={4}",
                m.id, m.name, leaf.id, leaf.name,
                static_cast<unsigned>(rootPatchCert.kind));
          continue;
        }

        trace("macro/dag", "{0}", rootPatchCert.detail);

        DagCandidateValidationMetadata subtreeValidation =
            buildDagCandidateValidationMetadataFromSubtree(subtreeCert);
        if (!validateDagCandidateProof(subtreeValidation, invSpanText,
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
                                           te.repl, std::nullopt});
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
                  return MacroPatch{minB, maxE, std::move(out)};
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

      auto baseRangesOpt = GetMacroInvocationFormalArgContentRanges(m, baseText);
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


    auto mergeCurrentRootWithExistingCallsitePatch =
        [&](MacroPatch &candidate, StringRef label) -> void {
      if (!existingPatch || !existingIsCallsite || baseInvText.empty())
        return;
      if (candidate.invStart != existingPatch->invStart ||
          candidate.invEnd != existingPatch->invEnd)
        return;
      if (candidate.replacement == existingPatch->replacement)
        return;

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
        return;
      }

      trace("macro/dag",
            "existing callsite patch merged with {0}: inv id={1} name='{2}' "
            "existing='{3}' current='{4}' merged='{5}'",
            label, m.id, m.name,
            stringutils::showWSWithClip(existingPatch->replacement, 160),
            stringutils::showWSWithClip(candidate.replacement, 160),
            stringutils::showWSWithClip(*merged, 160));
      candidate.replacement = std::move(*merged);
      if (!candidate.macroId)
        candidate.macroId = existingPatch->macroId;
    };

    auto dag = tryDAGChainedArgsOnly();
    if (dag) {
      if (argsOnlyCandidate && dag->invStart == argsOnlyCandidate->invStart &&
          dag->invEnd == argsOnlyCandidate->invEnd &&
          dag->replacement != argsOnlyCandidate->replacement &&
          !baseInvText.empty()) {
        SmallVector<StringRef, 2> repls;
        repls.push_back(StringRef(dag->replacement));
        repls.push_back(StringRef(argsOnlyCandidate->replacement));
        auto merged = mergeCompatibleRootCallsiteReplacements(
            baseInvText, ArrayRef<StringRef>(repls));
        if (merged && validateMergedDirectAndDagRootReplacement(
                          baseInvText, StringRef(*merged))) {
          trace("macro/dag",
                "DAG/direct root patches merged: root id={0} name='{1}' "
                "direct='{2}' dag='{3}' merged='{4}'",
                m.id, m.name,
                stringutils::showWSWithClip(argsOnlyCandidate->replacement, 160),
                stringutils::showWSWithClip(dag->replacement, 160),
                stringutils::showWSWithClip(*merged, 160));
          dag->replacement = std::move(*merged);
          if (!dag->macroId)
            dag->macroId = argsOnlyCandidate->macroId;
        } else {
          trace("macro/dag",
                "DAG args-only preferred over direct args-only: root id={0} "
                "name='{1}' direct='{2}' dag='{3}'",
                m.id, m.name,
                stringutils::showWSWithClip(argsOnlyCandidate->replacement, 160),
                stringutils::showWSWithClip(dag->replacement, 160));
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
      mergeCurrentRootWithExistingCallsitePatch(*dag, "dag/direct root rewrite");
      return *dag;
    }

    trace("macro/dag",
          "DAG args-only: no patch produced for root id={0} name='{1}'; "
          "will fall back to direct args-only / existing callsite / "
          "whole-cover replacement as needed",
          m.id, m.name);
    if (directRootPreservationInadmissible) {
      trace("macro/dag",
            "direct args-only/callsite preservation suppressed for root "
            "id={0} name='{1}': touched hunk lies in unsupported descendant "
            "subtree",
            m.id, m.name);
      argsOnlyCandidate.reset();
      reuseExistingCallsitePatch = false;
    }
  }

  if (argsOnlyCandidate) {
    return *argsOnlyCandidate;
  }

  if (reuseExistingCallsitePatch) {
    trace("macro", "callsite patch reused (args-only no-op) inv id={0}",
          m.id);
    return *existingPatch;
  }

  if (existingPatch && existingIsCallsite && !baseInvText.empty() &&
      InvocationSpanMatchesCallsitePrefix(baseInvText, m)) {
    trace("macro",
          "callsite patch reused (skip whole-cover expansion) inv id={0}",
          m.id);
    return *existingPatch;
  }


  // 2) Whole-cover fallback: replace invocation with the entire expansion cover
  // slice from B. cover.begin/cover.end are PP-token indices in A; map them
  // into a B-token envelope.
  //
  // This fallback is only valid when the invocation's replacement surface is
  // self-contained at its own callsite. Nested child macros whose emitted
  // tokens are interleaved with parent-owned syntax may still admit certified
  // args-only / DAG lifting, but they must never whole-cover expand directly
  // at the child callsite.
  if (!MacroWholeCoverIsSelfContained(m)) {
    trace("macro/whole",
          "whole-cover rejected inv id={0} name='{1}': non-self-contained cover=[{2},{3})",
          m.id, m.name, m.cover.begin, m.cover.end);
    return std::nullopt;
  }
  uint64_t covLoA = m.cover.begin;
  uint64_t covHiA = m.cover.end;

  // Same rationale as BuildWholeCoverReplacementText: for no-arg function-like
  // macros, prefer the precise bodySpans envelope over the conservative cover.
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

  auto bEnv = MapATokRangeAToBTokenEnvelopePreserveBoundaryInsertions(
      covLoA, covHiA);
  if (!bEnv)
    return std::nullopt;

  size_t bTokStart = bEnv->first;
  size_t bTokEnd = bEnv->second;
  if (bTokEnd <= bTokStart)
    return std::nullopt;

  // Tighten the B-side envelope to the exact A-side cover boundary tokens when
  // possible.
  //
  // The A→B alignment/byte-envelope mapping can legitimately drop or shift
  // punctuation tokens (e.g. the leading '(' in an assert() expansion) because
  // they are extremely common and thus low-information in the diff alignment.
  // When that happens, whole-cover replacement can produce syntactically wrong
  // output (missing parens) or double tokens (e.g. ';;').
  //
  // We can correct this deterministically by re-aligning the first/last B
  // tokens to the first/last A tokens of the macro cover, but only when the
  // expected token exists as an immediately-adjacent neighbor in B. This avoids
  // any heuristic scanning.
  if (covLoA < aToks_.size() && bTokStart < bToks_.size()) {
    StringRef want = aToks_[static_cast<size_t>(covLoA)].spelling;
    if (!want.empty()) {
      if (bToks_[bTokStart].spelling != want && bTokStart > 0 &&
          bToks_[bTokStart - 1].spelling == want) {
        bTokStart--;
      }
    }
  }

  if (covHiA > 0 && (covHiA - 1) < aToks_.size() && bTokEnd > 0 &&
      (bTokEnd - 1) < bToks_.size()) {
    StringRef want = aToks_[static_cast<size_t>(covHiA - 1)].spelling;
    if (!want.empty()) {
      if (bToks_[bTokEnd - 1].spelling != want && bTokEnd >= 2 &&
          bToks_[bTokEnd - 2].spelling == want) {
        bTokEnd--;
      }
    }
  }

  // Final check to ensure realignment didn't invert or empty the range.
  if (bTokEnd <= bTokStart)
    return std::nullopt;
  std::string clipped = SliceBSourceClippedAgainstClaims(bTokStart, bTokEnd);
  return MacroPatch{*invStart, *invEnd, StringRef(clipped).trim().str(), 0};
}

bool RefoldEngine::InvocationSpanMatchesCallsitePrefix(
    StringRef invSpanText, const RefoldModel::MacroInvocation &m) const {
  if (invSpanText.empty() || m.name.empty())
    return false;

  const size_t n = invSpanText.size();
  size_t i = 0;

  // Skip leading whitespace.
  while (i < n && stringutils::isWs(invSpanText[i]))
    i++;

  if (i >= n)
    return false;

  // Verify identifier start.
  const char c0 = invSpanText[i];
  if (!stringutils::isIdentStart(c0))
    return false;

  // Find end of identifier.
  size_t j = i + 1;
  while (j < n && stringutils::isIdentPart(invSpanText[j]))
    j++;

  // Check if the identifier matches the macro name.
  StringRef ident = invSpanText.slice(i, j);
  if (ident != m.name)
    return false;

  // Object-like macro: identifier match at span start is sufficient.
  if (m.subkind != "func")
    return true;

  // Function-like macro: must be followed by '(' (allowing whitespace).
  while (j < n && stringutils::isWs(invSpanText[j]))
    j++;

  return (j < n && invSpanText[j] == '(');
}

uint64_t RefoldEngine::GetRootMacroId(uint64_t macroId) const {
  const RefoldModel::MacroInvocation *cur = FindMacroInvocationById(macroId);
  if (!cur)
    return macroId;

  while (cur->callerMacroId) {
    const RefoldModel::MacroInvocation *parent =
        FindMacroInvocationById(*cur->callerMacroId);
    if (!parent)
      break;
    cur = parent;
  }
  return cur->id;
}

// ============ Include processing (normalize, materialize, apply) =============

void RefoldEngine::MaterializeIncludeExpansion(
    uint64_t includeId, const DenseMap<uint64_t, IncludeEdits> &perInclude,
    const DenseMap<std::optional<uint64_t>, std::vector<MacroPatch>>
        &macroPatchesByOwner,
    const DenseMap<uint64_t, std::vector<const RefoldModel::IncludeItem *>>
        &children,
    DenseMap<uint64_t, std::string> &includeExpansion,
    DenseSet<uint64_t> *appliedExpandedMacroRootIds) const {
  // Already materialized?
  if (includeExpansion.count(includeId)) {
    debug("include/mat", "SKIP inc#{0} (already materialized)", includeId);
    return;
  }

  const RefoldModel::IncludeItem *inc = model_.GetIncludeById(includeId);
  if (!inc)
    fatal("include/mat", "unknown includeId {0}", includeId);

  if (ForceInlineTouchedIncludesFromB()) {
    // Tier-2 escalation: bypass include text patching and inline the include's
    // fully expanded B-side slice for the include cover. This preserves edits
    // inside the include subtree, at the cost of expansion.
    auto bEnvOpt =
        MapATokRangeAToBTokenEnvelope(inc->cover.begin, inc->cover.end);
    if (!bEnvOpt) {
      RequestEscalation("include/mat",
                        llvm::formatv("tier2 inline-from-B: failed to map A "
                                      "cover [{0},{1}) for inc#{2}",
                                      inc->cover.begin, inc->cover.end,
                                      includeId)
                            .str());
      includeExpansion[includeId] = std::string();
      return;
    }
    includeExpansion[includeId] =
        SliceBSource(bEnvOpt->first, bEnvOpt->second).str();
    debug("include/mat",
          "FORCE inline from B tier={0} inc#{1} bTok=[{2},{3})",
          escalationTier_, inc->id, bEnvOpt->first, bEnvOpt->second);
    return;
  }

  debug("include/mat",
        "ENTER inc#{0} target={1} resolved={2} sitePath={3} site=[{4},{5}) "
        "cover=[{6},{7})",
        inc->id, inc->target, inc->resolvedPath, inc->sitePath, inc->siteB,
        inc->siteE, inc->cover.begin, inc->cover.end);

  const std::string headerPath = resolveHeaderPath(*inc);

  // Start from the raw header text that was preloaded into includeExpansion.
  // If it wasn’t preseeded for some reason, load deterministically by path.
  std::string bytes;
  if (auto it = includeExpansion.find(includeId); it != includeExpansion.end())
    bytes = it->second;
  if (bytes.empty()) {
    auto bufOrErr = MemoryBuffer::getFile(lineDirs_.ToAbsolutePath(headerPath));
    if (!bufOrErr) {
      fatal("include/mat", "failed to read header: {0} ({1})", headerPath,
            bufOrErr.getError().message());
    }

    // Treat as raw bytes; copy into std::string
    const MemoryBuffer &mb = **bufOrErr;
    bytes.assign(mb.getBufferStart(), mb.getBufferEnd());
  } else {
    debug("include/mat", "inc#{0} using preseeded bytes len={1}", inc->id,
          bytes.size());
  }

  auto editsIt = perInclude.find(includeId);
  auto macroIt = macroPatchesByOwner.find(includeId);
  auto childIt = children.find(includeId);

  debug(
      "include/mat",
      "inc#{0} initialHeaderLen={1} patches={2} macroPatches={3} children={4}",
      inc->id, bytes.size(),
      (editsIt != perInclude.end()) ? editsIt->second.patches.size() : 0,
      (macroIt != macroPatchesByOwner.end()) ? macroIt->second.size() : 0,
      (childIt != children.end()) ? childIt->second.size() : 0);

  // Collect byte-level edits to apply within this header.
  std::vector<TextEdit> edits;

  // 1) Macro-patch edits owned by this include (invocation byte ranges already
  // in the owner’s file space).
  if (auto it = macroPatchesByOwner.find(includeId);
      it != macroPatchesByOwner.end()) {
    for (const auto &mp : it->second) {
      uint64_t mpEnd =
          extendChainedCallEnd(StringRef(bytes), mp.invEnd, mp.replacement);
      if (mpEnd != mp.invEnd) {
        debug("macro/chain",
              "inc#{0} extend chained callsite [{1},{2}) -> [{1},{3})", inc->id,
              mp.invStart, mp.invEnd, mpEnd);
      }
      debug("include/mat", "inc#{0} macroPatch inv=[{1},{2}) replLen={3}",
            inc->id, mp.invStart, mpEnd, mp.replacement.size());
      ResyncOutcome ro = ApplyResyncOrPend(bytes, mp.invStart, mpEnd,
                                           mp.replacement, headerPath);
      edits.push_back(TextEdit{mp.invStart, mpEnd, std::move(ro.text),
                               std::move(ro.pending),
                               MacroPatchRemainsExpanded(mp)
                                   ? std::make_optional(GetRootMacroId(mp.macroId))
                                   : std::nullopt});
    }
  }

  // 2) A/B include insert/delete/replace patches that belong to this include.
  if (auto it = perInclude.find(includeId); it != perInclude.end()) {
    if (!it->second.patches.empty()) {
      debug("include/mat", "inc#{0} adding {1} include patches as TextEdits",
            inc->id, it->second.patches.size());
      auto moreEdits = ComputeIncludeTextEdits(it->second, bytes);
      for (auto &te : moreEdits) {
        ResyncOutcome ro =
            ApplyResyncOrPend(bytes, te.start, te.end, te.text, headerPath);
        edits.push_back(TextEdit{te.start, te.end, std::move(ro.text),
                                 std::move(ro.pending)});
      }
    } else {
      debug("include/mat", "inc#{0} has no include patches", inc->id);
    }
  } else {
    debug("include/mat", "inc#{0} has no include patches", inc->id);
  }

  auto hasDescendantWork = [&](auto &&self, uint64_t id) -> bool {
    // Direct work at this node?
    bool selfWork = false;
    if (auto it = perInclude.find(id); it != perInclude.end())
      selfWork = !it->second.patches.empty();
    if (!selfWork) {
      if (auto it = macroPatchesByOwner.find(id);
          it != macroPatchesByOwner.end())
        selfWork = !it->second.empty();
    }

    if (selfWork) {
      debug("include/tree", "hasWork inc#{0}: selfWork=YES", id);
      return true;
    }

    // Otherwise, recurse into children
    if (auto it = children.find(id); it != children.end()) {
      for (const auto *child : it->second) {
        if (self(self, child->id)) {
          debug("include/tree", "hasWork inc#{0}: via child#{1} => YES", id,
                child->id);
          return true;
        }
      }
    }

    debug("include/tree", "hasWork inc#{0}: NO", id);
    return false;
  };

  // 3) Recurse into child includes that have any descendant work, and
  // replace each child directive with the child’s fully materialized text.
  if (auto it = children.find(includeId); it != children.end()) {
    for (const auto *child : it->second) {
      const bool todo = hasDescendantWork(hasDescendantWork, child->id);
      debug("include/mat",
            "inc#{0} -> child#{1} target={2} resolved={3} site=[{4},{5}) "
            "expand={6}",
            inc->id, child->id, child->target, child->resolvedPath,
            child->siteB, child->siteE, todo ? "YES" : "NO");
      if (!todo) {
        continue; // leave untouched: keep the original directive as-is
      }

      // Ensure the child is materialized first (depth-first).
      MaterializeIncludeExpansion(child->id, perInclude, macroPatchesByOwner,
                                  children, includeExpansion,
                                  appliedExpandedMacroRootIds);

      // The child directive's site is recorded in the includer byte space.
      const auto &childText = includeExpansion[child->id];
      const size_t n = bytes.size();
      const uint64_t siteStart = std::clamp<uint64_t>(child->siteB, 0ULL, n);
      const uint64_t siteEnd = std::clamp<uint64_t>(child->siteE, siteStart, n);

      debug("include/mat",
            "REPLACE in inc#{0}: site=[{1},{2}) len(parent)={3} with child#{4} "
            "len(childText)={5}",
            inc->id, siteStart, siteEnd, bytes.size(), child->id,
            childText.size());

      debug("tu/replace",
            "TU replace site=[{0},{1}) with inc#{2} len={3} (target={4} "
            "resolved={5})",
            siteStart, siteEnd, child->id, childText.size(), child->target,
            child->resolvedPath);

      if (siteStart < siteEnd) {
        std::string childHeaderPath = resolveHeaderPath(*child);
        std::string wrapped = lineDirs_.WrapIncludeExpansion(
            childHeaderPath, headerPath,
            stringutils::lineAtOffset(bytes, child->siteE), childText);
        edits.push_back(
            TextEdit{siteStart, siteEnd, std::move(wrapped), std::nullopt});
      } else {
        // Defensive fallback: if site is somehow unmapped, skip replacing.
        // (This keeps behavior deterministic instead of crashing.)
        debug("include/mat",
              "inc#{0} child#{1} has degenerate site [start={2},end={3}]; "
              "skipping TextEdit",
              inc->id, child->id, siteStart, siteEnd);
      }
    }
  }

  debug("include/mat", "inc#{0} applying {1} header TextEdits", inc->id,
        edits.size());
  std::string applied = ApplyTextEditsWithPendingResync(
      bytes, edits, appliedExpandedMacroRootIds);
  includeExpansion[includeId] = std::move(applied);
  debug("include/mat", "EXIT inc#{0} resultLen={1}", inc->id,
        includeExpansion[includeId].size());
}

const RefoldModel::HeaderDecl *
RefoldEngine::FindHeaderDeclForPatch(const RefoldModel::IncludeItem &inc,
                                     const IncludePatch &p) {
  if (inc.decls.empty())
    return nullptr;

  const uint64_t aLo = p.aStart;
  const uint64_t aHi = p.aEnd;

  const RefoldModel::HeaderDecl *bestCover = nullptr;
  const RefoldModel::HeaderDecl *bestOverlap = nullptr;

  auto getSpanLen = [](const RefoldModel::HeaderDecl *d) -> uint64_t {
    // Standardize: ensure we don't underflow if a span is somehow malformed.
    if (d->span.end <= d->span.begin)
      return 0;
    return d->span.end - d->span.begin;
  };

  for (const auto &d : inc.decls) {
    const uint64_t dLo = d.span.begin;
    const uint64_t dHi = d.span.end;
    const uint64_t curSpanLen = dHi - dLo;

    // Pure insertion: treat as attached at aLo
    if (aLo == aHi) {
      // Special case: insertions at decl end are treated as inclusive
      if (aLo >= dLo && aLo <= dHi) {
        // Tie-breaker: prefer the "tightest" (smallest) declaration that covers
        // this point.
        if (!bestCover || curSpanLen < getSpanLen(bestCover))
          bestCover = &d;
      }
      continue;
    }

    // Non-empty A-interval
    const bool covers = (aLo >= dLo && aHi <= dHi);
    const bool overlaps = (aLo < dHi && aHi > dLo);

    if (covers) {
      // Prioritize "Cover": we want the smallest decl that completely contains
      // the patch.
      if (!bestCover || curSpanLen < getSpanLen(bestCover))
        bestCover = &d;
    } else if (overlaps) {
      // Fallback to "Overlap": if no decl covers it, find the smallest one that
      // touches it.
      if (!bestOverlap || curSpanLen < getSpanLen(bestOverlap))
        bestOverlap = &d;
    }
  }

  return bestCover ? bestCover : bestOverlap;
}

std::vector<RefoldEngine::TextEdit>
RefoldEngine::ComputeIncludeTextEdits(const IncludeEdits &ie,
                                      std::string headerText) const {
  const std::string file = resolveHeaderPath(*ie.include);

  const size_t fileLen = headerText.size();

  debug("include/apply",
        "ENTER computeIncludeTextEdits file={0} len={1} patches={2} "
        "cover=[{3},{4})",
        file, fileLen, ie.patches.size(), ie.include->cover.begin,
        ie.include->cover.end);

  // PP cover for this include inside the header; if not present, these will
  // already have been derived from spans when building the model.
  const uint64_t coverBegin =
      std::max(static_cast<uint64_t>(0), ie.include->cover.begin);
  const uint64_t coverEnd = std::max(coverBegin, ie.include->cover.end);

  // Single list of edits; we will apply them highest-offset-first so indices
  // remain stable as we mutate the StringBuilder.
  std::vector<TextEdit> edits;

  for (size_t idx = 0; idx < ie.patches.size(); ++idx) {
    const IncludePatch &p = ie.patches[idx];
    trace("include/patch", "computeIncludeTextEdits: patch={0}", p);

    const bool isInsert = (p.aStart == p.aEnd) && (p.bStart < p.bEnd);
    const bool isDelete = (p.aStart < p.aEnd) && (p.bStart == p.bEnd);
    const bool isReplace = (p.aStart < p.aEnd) && (p.bStart < p.bEnd);

    debug("include/apply",
          "file={0} patch[{1}] raw A=[{2},{3}) B=[{4},{5}) isInsert={6} "
          "isDelete={7} isReplace={8}",
          file, idx, p.aStart, p.aEnd, p.bStart, p.bEnd, isInsert, isDelete,
          isReplace);

    if (!isInsert && !isDelete && !isReplace) {
      // Ignore empty or malformed patches defensively.
      debug("include/apply",
            "file={0} patch[{1}] ignored (no-op classification)", file, idx);
      continue;
    }

    // Decide which logical header decl "owns" this patch, if any.
    const auto *decl = FindHeaderDeclForPatch(*ie.include, p);

    if (decl) {
      debug("include/apply",
            "file={0} patch[{1}] ownerDecl kind={2} name={3} header=[{4},{5}) "
            "pp-span=[{6},{7})",
            file, idx, decl->kind, decl->name, decl->headerB, decl->headerE,
            decl->span.begin, decl->span.end);
    } else {
      debug("include/apply", "file={0} patch[{1}] ownerDecl=<none>", file, idx);
    }

    // Effective PP coverage in this header we're allowed to touch.
    // For INSERTs we deliberately work at include scope so that inserts that
    // land exactly at a declaration boundary (for example, between
    // `int yyy(...);` and `int zzz(...);`) can anchor to the first token
    // of the following declaration rather than being forced back inside the
    // previous one.  For DELETE / REPLACE we restrict to the owning decl.
    uint64_t ppLo = coverBegin;
    uint64_t ppHi = coverEnd;
    if (!isInsert && decl) {
      ppLo = std::max(ppLo, decl->span.begin);
      ppHi = std::min(ppHi, decl->span.end);
    }
    ppHi = std::max(ppHi, ppLo);

    trace("include/apply",
          "file={0} patch[{1}] effectivePP=[{2},{3}) cover=[{4},{5})", file,
          idx, ppLo, ppHi, coverBegin, coverEnd);

    std::optional<uint64_t> startByte;
    std::optional<uint64_t> endByte;

    const auto &tokmapByPP = model_.GetTokmapByPP();
    if (isInsert) {
      // INSERT: interpret A-position as "before the next token" in this header.
      const uint64_t pos = p.aStart;
      std::optional<uint64_t> anchorPP;

      // If this INSERT gap is exactly the begin of the currently selected
      // conditional arm in this header, treat it as the *boundary before* the
      // conditional group rather than as "before the next token" inside the
      // arm body. In preprocessed token space, both positions collapse onto the
      // same PP gap because the controlling directive itself contributes no PP
      // tokens. For include-owned pure insertions, the boundary policy is to
      // keep the insertion outside the conditional unless the insertion is
      // explicitly arm-owned.
      auto selectedArmBeginBoundaryByte = [&]() -> std::optional<uint64_t> {
        auto rightArmRef = model_.FindArmRefAtPP(pos);
        if (!rightArmRef || !rightArmRef->group || !rightArmRef->arm)
          return std::nullopt;
        if (!rightArmRef->arm->selected || !rightArmRef->arm->span)
          return std::nullopt;
        if (rightArmRef->arm->span->begin != pos)
          return std::nullopt;
        if (!PathsEqual(rightArmRef->group->file, file))
          return std::nullopt;
        if (!rightArmRef->group->parentIncludeId ||
            *rightArmRef->group->parentIncludeId != ie.include->id)
          return std::nullopt;

        auto leftArmRef = (pos > 0) ? model_.FindArmRefAtPP(pos - 1)
                                    : std::optional<RefoldModel::ArmRef>{};
        if (leftArmRef && leftArmRef->arm &&
            leftArmRef->arm->id == rightArmRef->arm->id)
          return std::nullopt;

        return std::clamp<uint64_t>(rightArmRef->group->groupB, 0ULL, fileLen);
      };

      if (const std::optional<uint64_t> insertByte =
              selectedArmBeginBoundaryByte()) {
        std::string text =
            PadAtBoundaries(headerText, static_cast<size_t>(*insertByte),
                            static_cast<size_t>(*insertByte), p.insertBytes,
                            /* allowLeft */ true, /* allowRight */ true);
        edits.push_back(MakeTextEditWithResyncOrPending(
            headerText, *insertByte, *insertByte, text, file));

        trace("include/apply",
              "file={0} patch[{1}] INSERT: anchored at selected-arm boundary groupBegin={2}",
              file, idx, insertByte);
        continue;
      }

      // First, if this INSERT PP gap lies on a boundary between this header
      // and one of its direct child includes, prefer anchoring at the child's
      // include-site byte position. This avoids placing boundary insertions at
      // the next mapped PP token in the header, which is often *after* the
      // child include directive (since tokens from the included file do not map
      // back to this header).
      if (const std::optional<uint64_t> insertByte =
              ComputeChildBoundaryInsertByte(p, file)) {
        if (*insertByte <= fileLen) {
          std::string text =
              PadAtBoundaries(headerText, static_cast<size_t>(*insertByte),
                              static_cast<size_t>(*insertByte), p.insertBytes,
                              /* allowLeft */ true, /* allowRight */ true);
          edits.push_back(MakeTextEditWithResyncOrPending(
              headerText, *insertByte, *insertByte, text, file));

          trace("include/apply",
                "file={0} patch[{1}] INSERT: anchored via child boundary at "
                "byte={2}",
                file, idx, insertByte);
          continue;
        }
      }

      // 1) Prefer the right neighbor: smallest pp >= pos in [ppLo, ppHi).
      for (uint64_t pp = std::max(pos, ppLo); pp < ppHi; ++pp) {
        auto it = tokmapByPP.find(pp);
        if (it != tokmapByPP.end() && PathsEqual(it->second.file, file)) {
          anchorPP = pp;
          break;
        }
      }

      if (anchorPP) {
        // Insert immediately before the right neighbor token.
        startByte = ByteStartForPPInFile(file, *anchorPP,
                                         /* fallbackToEOF */ false, fileLen);
        trace("include/apply",
              "file={0} patch[{1}] INSERT: right-neighbor anchorPP={2} -> "
              "startByte={3}",
              file, idx, anchorPP, startByte);
        if (!startByte) {
          if (!ForceInlineTouchedIncludesFromB())
            RequestEscalation(
                "include/apply",
                llvm::formatv("INSERT: failed to map anchorPP={0} in file {1}",
                              anchorPP ? *anchorPP : 0ULL, file)
                    .str());
          continue;
        }
      } else {
        // 2) No right neighbor; fall back to the last left neighbor.
        if (pos > ppLo && ppHi > ppLo) {
          for (uint64_t pp = std::min(pos - 1, ppHi - 1);; --pp) {
            auto it = tokmapByPP.find(pp);
            if (it != tokmapByPP.end() && PathsEqual(it->second.file, file)) {
              anchorPP = pp;
              break;
            }
            if (pp == ppLo)
              break;
          }
        }

        if (anchorPP) {
          startByte = ByteEndForPPInFile(file, *anchorPP, false, fileLen);
          trace("include/apply",
                "file={0} patch[{1}] INSERT: left-neighbor anchorPP={2} -> "
                "startByte={3}",
                file, idx, anchorPP, startByte);
          if (!startByte) {
            continue;
          }
        } else if (decl) {
          // 3) No PP neighbor at all in this header, but we have an owning
          // decl: anchor at the end of its header span.
          startByte = std::clamp<uint64_t>(decl->headerE, 0ULL, fileLen);
          trace(
              "include/apply",
              "file={0} patch[{1}] INSERT: no neighbors; anchor at declEnd={2}",
              file, idx, startByte);
        } else {
          // 4) Fallback: use child '#include' sites inside this header as
          // synthetic anchors.
          const std::optional<uint64_t> insertByte =
              ComputeChildBoundaryInsertByte(p, file);
          debug("include/apply.", "inserted byte {0}", insertByte);
          if (insertByte && *insertByte <= fileLen) {
            std::string text =
                PadAtBoundaries(headerText, static_cast<size_t>(*insertByte),
                                static_cast<size_t>(*insertByte), p.insertBytes,
                                /* allowLeft */ true, /* allowRight */ true);
            edits.push_back(MakeTextEditWithResyncOrPending(
                headerText, *insertByte, *insertByte, text, file));

            debug("include/apply.",
                  "file={0} patch[{1}] INSERT: anchored via child boundary at "
                  "byte={2}",
                  file, idx, insertByte);
          } else {
            // Preserve old behavior if we still can't place it
            // deterministically.
            debug("include/apply.",
                  "file={0} patch[{1}] INSERT: no neighbors, no decl, no child "
                  "boundary; SKIP",
                  file, idx);
            if (!ForceInlineTouchedIncludesFromB())
              RequestEscalation(
                  "include/apply",
                  llvm::formatv("INSERT: cannot anchor include patch in file "
                                "{0} (no neighbors/decl/child boundary)",
                                file)
                      .str());
          }
          continue;
        }
      }

      endByte = startByte;
    } else {
      // DELETE / REPLACE: map non-empty A-range to byte range within this
      // header.
      uint64_t aLo = std::max(p.aStart, ppLo);
      uint64_t aHi = std::min(p.aEnd, ppHi);
      if (aHi <= aLo) {
        // Nothing of this patch lies in this header/declaration.
        debug("include/apply",
              "file={0} patch[{1}] DELETE/REPLACE: empty intersection; SKIP",
              file, idx);
        continue;
      }

      std::optional<uint64_t> firstPP, lastPP;
      for (uint64_t pp = aLo; pp < aHi; ++pp) {
        auto it = tokmapByPP.find(pp);
        if (it != tokmapByPP.end() && PathsEqual(it->second.file, file)) {
          if (!firstPP)
            firstPP = pp;
          lastPP = pp;
        }
      }

      if (!firstPP || !lastPP) {
        // No tokens from this patch actually map into this header file.
        debug("include/apply",
              "file={0} patch[{1}] DELETE/REPLACE: no mapped PP tokens in "
              "header; SKIP",
              file, idx);
        if (!ForceInlineTouchedIncludesFromB())
          RequestEscalation("include/apply",
                            llvm::formatv("DELETE/REPLACE: no mapped PP tokens "
                                          "for patch[{0}] in header file {1}",
                                          idx, file)
                                .str());
        continue;
      }

      startByte = ByteStartForPPInFile(file, *firstPP,
                                       /* fallbackToEOF */ false, fileLen);
      endByte =
          ByteEndForPPInFile(file, *lastPP, /* fallbackToEOF */ false, fileLen);
      trace("include/apply",
            "file={0} patch[{1}] DELETE/REPLACE: firstPP={2} lastPP={3} -> "
            "bytes=[{4},{5})",
            file, idx, firstPP, lastPP, startByte, endByte);
      if (!startByte || !endByte) {
        if (!ForceInlineTouchedIncludesFromB())
          RequestEscalation(
              "include/apply",
              llvm::formatv("DELETE/REPLACE: failed to map first/last PP "
                            "tokens to bytes in file {0}",
                            file)
                  .str());
        continue;
      }
    }

    // Clamp to the declaration’s header_span, if any, so we never cross decl
    // boundaries for DELETE/REPLACE.  For INSERTs we intentionally allow the
    // anchor to sit on the boundary between two declarations so that a new
    // declaration can be injected cleanly between them.
    if (!isInsert && decl) {
      startByte = std::clamp(*startByte, decl->headerB, decl->headerE);
      endByte = std::clamp(*endByte, decl->headerB, decl->headerE);
    }

    // Sanity clamp to file bounds.
    if (!startByte)
      continue;

    const uint64_t fLen = static_cast<uint64_t>(fileLen);
    startByte = std::clamp(*startByte, uint64_t(0), fLen);
    endByte = std::clamp(*endByte, *startByte, fLen);

    std::string replacement = isDelete ? "" : p.insertBytes;

    if (inTraceMode()) {
      StringRef opKind =
          isInsert ? "INSERT" : (isDelete ? "DELETE" : "REPLACE");

      // DEBUG: log the exact slice and replacement we are about to apply.
      const std::string originalSlice =
          headerText.substr(*startByte, *endByte - *startByte);
      std::string origDbg = originalSlice;
      std::string replDbg = replacement;
      constexpr int MAX_DBG = 120;
      if (origDbg.size() > MAX_DBG) {
        origDbg = origDbg.substr(0, MAX_DBG) + "…";
      }
      if (replDbg.size() > MAX_DBG) {
        replDbg = replDbg.substr(0, MAX_DBG) + "…";
      }

      std::string declInfo = "<none>";
      if (decl) {
        declInfo =
            formatv("kind={0} name={1} header=[{2},{3}) pp-span=[{4},{5})",
                    decl->kind, decl->name, decl->headerB, decl->headerE,
                    decl->span.begin, decl->span.end)
                .str();
      }

      trace("include/apply",
            "file={0} patch[{1}] kind={2} A=[{3},{4}) B=[{5},{6}) pp=[{7},{8}) "
            "bytes=[{9},{10}) decl={11} orig='{12}' repl='{13}'",
            file, idx, opKind, p.aStart, p.aEnd, p.bStart, p.bEnd, ppLo, ppHi,
            startByte, endByte, declInfo, stringutils::showWS(origDbg),
            stringutils::showWS(replDbg));
    }

    edits.push_back(MakeTextEditWithResyncOrPending(
        headerText, *startByte, *endByte, replacement, file));
  }

  // Apply all edits inside this header, highest offset first so earlier edits
  // do not disturb the coordinates of later ones.
  sort(edits, [](const TextEdit &lhs, const TextEdit &rhs) {
    if (lhs.start != rhs.start)
      return lhs.start > rhs.start;
    return lhs.end > rhs.end;
  });

  debug("include/apply", "file={0} computed {1} header TextEdits", file,
        edits.size());

  for (const auto &e : edits) {
    trace("include/apply",
          "file={0} header TextEdit bytes=[{1},{2}) replLen={3}", file, e.start,
          e.end, e.text.size());

    if (e.end < e.start || e.end > static_cast<uint64_t>(headerText.size())) {
      fatal("include/apply", "TextEdit out of bounds: bytes=[{0},{1}) size={2}",
            e.start, e.end, headerText.size());
    }
  }

  return edits;
}

std::optional<uint64_t>
RefoldEngine::ComputeChildBoundaryInsertByte(const IncludePatch &p,
                                             StringRef file) const {
  // Which include are we editing?
  const RefoldModel::IncludeItem *owner = p.include;
  if (!owner) {
    return std::nullopt;
  }

  // Structural boundary selection:
  //   * If the INSERT PP-gap is strictly inside a child include's PP cover,
  //     this is not a boundary insertion in the parent (it should have been a
  //     child-owned patch).
  //   * If the gap is exactly on a child cover boundary, we can anchor at that
  //     child's include-site byte span.
  //
  // Contract: boundary anchors must be unambiguous. If multiple children claim
  // the same boundary PP position, we must fail rather than guess.

  uint64_t pos = p.aStart; // PP position of the INSERT gap

  SmallVector<const RefoldModel::IncludeItem *, 4> beginMatches;
  SmallVector<const RefoldModel::IncludeItem *, 4> endMatches;

  for (const auto &child : model_.GetIncludes()) {
    // Only check direct children of the owner.
    if (!child.parent || *child.parent != owner->id)
      continue;

    // Paths must match the file currently being processed.
    if (!PathsEqual(child.sitePath, file))
      continue;

    const uint64_t cb = child.cover.begin;
    const uint64_t ce = child.cover.end;

    // If the gap is strictly inside, this is the wrong mechanism.
    if (cb < pos && pos < ce)
      return std::nullopt;

    if (cb == pos)
      beginMatches.push_back(&child);
    if (ce == pos)
      endMatches.push_back(&child);
  }

  auto dump = [](ArrayRef<const RefoldModel::IncludeItem *> v) -> std::string {
    std::string out;
    bool first = true;
    for (const auto *c : v) {
      if (!first)
        out += "; ";
      first = false;
      out +=
          formatv("child#{0} cover=[{1},{2}) site=[{3},{4}) target={5}", c->id,
                  c->cover.begin, c->cover.end, c->siteB, c->siteE, c->target)
              .str();
    }
    return out;
  };

  // Prefer anchoring before a child whose cover begins at this gap.
  if (!beginMatches.empty()) {
    if (beginMatches.size() != 1) {
      fatal("include/boundary",
            "ambiguous child include boundary: {0} children have cover.begin "
            "== {1} in file={2}; candidates: {3}",
            beginMatches.size(), pos, file, dump(beginMatches));
    }
    return beginMatches[0]->siteB;
  }

  if (!endMatches.empty()) {
    if (endMatches.size() != 1) {
      fatal("include/boundary",
            "ambiguous child include boundary: {0} children have cover.end == "
            "{1} in file={2}; candidates: {3}",
            endMatches.size(), pos, file, dump(endMatches));
    }
    return endMatches[0]->siteE;
  }

  return std::nullopt;
}

RefoldEngine::ResyncOutcome
RefoldEngine::ApplyResyncOrPend(StringRef originalFileText, uint64_t start,
                                uint64_t end, StringRef replacement,
                                StringRef fileSpellingForDirective) const {
  size_t origNl = stringutils::countNewlines(originalFileText, start, end);
  size_t replNl =
      stringutils::countNewlines(replacement, 0, replacement.size());
  if (origNl == replNl)
    return ResyncOutcome(replacement.str(), std::nullopt);

  // Drift detected: decide whether to inject a local `#line` or defer.
  size_t resumeLine = stringutils::lineAtOffset(originalFileText, end);
  trace("linedir/resync",
        "drift: span=[{0},{1}) origNl={2} replNl={3} resumeLine={4} file={5} "
        "replTail={6}",
        start, end, origNl, replNl, resumeLine, fileSpellingForDirective,
        stringutils::showWSWithClip(replacement, 100));

  // Attempt local injection first.
  std::string injected = lineDirs_.MaybeAppendResyncAfterReplacement(
      originalFileText, start, end, replacement, fileSpellingForDirective);

  // If the returned string changed, injection succeeded.
  if (injected != replacement) {
    trace("linedir/resync", "local inject succeeded: resumeLine={0} file={1}",
          resumeLine, fileSpellingForDirective);
    return ResyncOutcome(std::move(injected), std::nullopt);
  }

  trace("linedir/resync",
        "local inject failed -> PENDING: resumeLine={0} file={1}", resumeLine,
        fileSpellingForDirective);

  return ResyncOutcome{replacement.str(),
                       PendingResync{fileSpellingForDirective}};
}

std::string
RefoldEngine::ApplyTextEditsWithPendingResync(
    StringRef originalFileText, ArrayRef<TextEdit> edits,
    DenseSet<uint64_t> *appliedExpandedMacroRootIds) const {
  if (edits.empty())
    return originalFileText.str();

  // Normalize edits by span.
  //
  // Historically we deduped by (start,end) using a map, which accidentally
  // dropped legitimate *multiple insertions* at the same byte offset.
  // This happens when the diff produces adjacent insert-only hunks (e.g. when
  // comments tokenize separately), and both map to the same insertion point.
  //
  // We now:
  //  * Preserve and concatenate multiple INSERT edits with identical
  //    zero-length spans ([x,x)) in their original order.
  //  * For non-zero spans, keep the last edit (as the prior map effectively
  //    did) but retain any pending-resync from earlier edits, and emit a trace
  //    if the payloads disagree.

  struct EditRef {
    const TextEdit *e;
    size_t idx;
  };

  SmallVector<EditRef, 32> ordered;
  ordered.reserve(edits.size());
  for (size_t i = 0; i < edits.size(); ++i)
    ordered.push_back(EditRef{&edits[i], i});

  sort(ordered, [](const EditRef &a, const EditRef &b) {
    if (a.e->start != b.e->start)
      return a.e->start < b.e->start;
    if (a.e->end != b.e->end)
      return a.e->end < b.e->end;
    return a.idx < b.idx;
  });

  SmallVector<TextEdit, 32> norm;
  norm.reserve(ordered.size());

  for (size_t i = 0; i < ordered.size();) {
    const uint64_t s = ordered[i].e->start;
    const uint64_t t = ordered[i].e->end;

    size_t j = i + 1;
    while (j < ordered.size() && ordered[j].e->start == s &&
           ordered[j].e->end == t)
      ++j;

    if (j - i == 1) {
      norm.push_back(*ordered[i].e);
      i = j;
      continue;
    }

    // Multiple edits share the same byte span.
    if (s == t) {
      // INSERT at identical position: concatenate in order.
      TextEdit merged;
      merged.start = s;
      merged.end = t;

      size_t totalLen = 0;
      for (size_t k = i; k < j; ++k)
        totalLen += ordered[k].e->text.size();
      merged.text.reserve(totalLen);

      for (size_t k = i; k < j; ++k) {
        merged.text.append(ordered[k].e->text);
        if (ordered[k].e->pending)
          merged.pending = ordered[k].e->pending;
      }

      trace("edits/apply",
            "merged {0} insertion edits at [{1},{2}) into len={3}", j - i, s, t,
            merged.text.size());

      norm.push_back(std::move(merged));
      i = j;
      continue;
    }

    // Non-zero span duplicates: keep the last edit by order, but preserve any
    // pending resync request.
    const TextEdit *last = ordered[j - 1].e;
    TextEdit chosen = *last;

    for (size_t k = i; k < j; ++k) {
      if (ordered[k].e->pending)
        chosen.pending = ordered[k].e->pending;
    }

    if (inTraceMode()) {
      bool disagree = false;
      for (size_t k = i; k + 1 < j; ++k) {
        if (ordered[k].e->text != last->text) {
          disagree = true;
          break;
        }
      }
      if (disagree) {
        trace("edits/apply",
              "multiple edits share span [{0},{1}); keeping last (idx={2})", s,
              t, ordered[j - 1].idx);
      }
    }

    norm.push_back(std::move(chosen));
    i = j;
  }

  // Use SmallString for the output buffer to optimize small file edits.
  SmallString<0> out;
  out.reserve(originalFileText.size() + 128);
  std::optional<PendingResync> pending = std::nullopt;

  uint64_t cursor = 0;
  size_t n = originalFileText.size();

  for (const auto &e : norm) {
    if (e.end < e.start || e.end > n) {
      fatal("edits/apply", "bad edit bounds [{0},{1}) fileLen={2}", e.start,
            e.end, n);
    }
    if (e.start < cursor) {
      fatal("edits/apply", "overlapping edits: cursor={0} nextStart={1}",
            cursor, e.start);
    }

    // Pass the SmallString to the appender.
    pending = AppendOriginalSliceWithPending(out, originalFileText, cursor,
                                             e.start, std::move(pending));

    if (appliedExpandedMacroRootIds && e.expandedMacroRootId)
      appliedExpandedMacroRootIds->insert(*e.expandedMacroRootId);

    out.append(e.text);

    if (e.pending) {
      pending = e.pending;
    }

    cursor = e.end;
  }

  pending = AppendOriginalSliceWithPending(out, originalFileText, cursor, n,
                                           std::move(pending));

  return std::string(out.str());
}

std::optional<RefoldEngine::PendingResync>
RefoldEngine::AppendOriginalSliceWithPending(
    SmallVectorImpl<char> &out, llvm::StringRef original, uint64_t from,
    uint64_t to, std::optional<RefoldEngine::PendingResync> pending) const {
  if (!pending || !lineDirs_.Enabled()) {
    auto slice = original.slice(from, to);
    out.append(slice.begin(), slice.end());
    return std::nullopt;
  }

  uint64_t i = from;

  auto canFlushBeforeUntouchedDirectiveLine = [&](size_t pos) -> bool {
    if (pos >= original.size())
      return false;

    size_t lineStart = pos;
    while (lineStart > 0 && original[lineStart - 1] != '\n')
      --lineStart;

    if (!stringutils::isIndentOnly(original, lineStart, pos))
      return false;

    size_t lineEnd = original.find('\n', pos);
    if (lineEnd == StringRef::npos)
      lineEnd = original.size();

    size_t firstNonWs = pos;
    while (firstNonWs < lineEnd && stringutils::isWs(original[firstNonWs]) &&
           original[firstNonWs] != '\n')
      ++firstNonWs;

    return firstNonWs < lineEnd && original[firstNonWs] == '#';
  };

  // Flush immediately when output is already at BOL and the next untouched
  // bytes are either:
  // (1) at a true BOL in the original file, or
  // (2) in the leading indentation of an untouched preprocessor-directive
  //     line. Directives may legally be preceded by horizontal whitespace, so
  //     flushing before that indentation keeps the directive line's logical
  //     location accurate without relaxing the ordinary code-line safety rule.
  if (stringutils::outAtBOL(StringRef(out.data(), out.size())) &&
      (stringutils::isBOL(original, static_cast<size_t>(from)) ||
       canFlushBeforeUntouchedDirectiveLine(static_cast<size_t>(from)))) {
    size_t line = stringutils::lineAtOffset(original, from);
    std::string directive =
        lineDirs_.FormatLineDirective(line, pending->fileSpellingForDir);

    // Create a view of the current buffer for the check
    StringRef currentOut(out.data(), out.size());

    if (LineDirectiveInserter::ShouldEmitLineDirective(
            currentOut, pending->fileSpellingForDir, line, directive)) {
      trace("line/pending",
            "flush@slice-begin file={0} line={1} (pending) outTail={2}",
            pending->fileSpellingForDir, line,
            stringutils::dbgOutTail(currentOut));
      out.append(directive.begin(), directive.end());
    } else {
      trace("line/pending",
            "SKIP flush@slice-begin (no-op) file={0} line={1} outTail={2}",
            pending->fileSpellingForDir, line,
            stringutils::dbgOutTail(currentOut));
    }

    auto slice =
        original.slice(static_cast<size_t>(from), static_cast<size_t>(to));
    out.append(slice.begin(), slice.end());
    return std::nullopt;
  }

  // Otherwise, scan forward for the first safe newline boundary (not
  // line-spliced).
  while (i < to) {
    size_t nl = original.find('\n', i);
    if (nl == llvm::StringRef::npos || nl >= static_cast<size_t>(to))
      break;

    auto slice = original.slice(static_cast<size_t>(i), nl + 1);
    out.append(slice.begin(), slice.end());

    i = nl + 1;

    if (!stringutils::isLineSplice(original, nl)) {
      size_t line = stringutils::lineAtOffset(original, static_cast<size_t>(i));
      std::string directive =
          lineDirs_.FormatLineDirective(line, pending->fileSpellingForDir);

      // Update our view of the output after appending the newline
      StringRef currentOut(out.data(), out.size());
      if (LineDirectiveInserter::ShouldEmitLineDirective(
              currentOut, pending->fileSpellingForDir, line, directive)) {
        trace("line/pending",
              "flush@safe-nl file={0} line={1} atOrig={2} outTail={3}",
              pending->fileSpellingForDir, line, i,
              stringutils::dbgOutTail(currentOut));
        out.append(directive.begin(), directive.end());
      } else {
        trace("line/pending",
              "SKIP flush@safe-nl (no-op) file={0} line={1} atOrig={2} "
              "outTail={3}",
              pending->fileSpellingForDir, line, i,
              stringutils::dbgOutTail(currentOut));
      }

      pending = std::nullopt;
      break;
    }
  }

  // Clean up remaining bytes
  if (i < to) {
    auto remaining =
        original.slice(static_cast<size_t>(i), static_cast<size_t>(to));
    out.append(remaining.begin(), remaining.end());
  }
  return pending;
}

// ==================== Low-level file & mapping utilities =====================

bool RefoldEngine::PathsEqual(StringRef a, StringRef b) {
  if (a.empty() || b.empty())
    return a == b;

  std::error_code ecA, ecB;
  const auto ca =
      std::filesystem::weakly_canonical(std::filesystem::path(a.str()), ecA);
  const auto cb =
      std::filesystem::weakly_canonical(std::filesystem::path(b.str()), ecB);

  if (ecA) {
    fatal("path/canon", "failed to canonicalize '{0}': {1}", a, ecA.message());
  }
  if (ecB) {
    fatal("path/canon", "failed to canonicalize '{0}': {1}", b, ecB.message());
  }

  return ca == cb;
}

// ======================= Diagnostics & Debug Utilities =======================

void RefoldEngine::DebugIncludePatch(StringRef tag,
                                     const RefoldModel::IncludeItem &inc,
                                     const diffutils::Hunk &h) const {
  auto getByteOff = [](uint64_t tokenIdx,
                       ArrayRef<size_t> offsets) -> std::optional<uint64_t> {
    if (tokenIdx < offsets.size()) {
      return static_cast<uint64_t>(offsets[static_cast<size_t>(tokenIdx)]);
    }
    return std::nullopt;
  };

  const std::optional<uint64_t> a0 = getByteOff(h.aStart, aTokOff_);
  const std::optional<uint64_t> a1 = getByteOff(h.aEnd, aTokOff_);
  const std::optional<uint64_t> b0 = getByteOff(h.bStart, bTokOff_);
  const std::optional<uint64_t> b1 = getByteOff(h.bEnd, bTokOff_);

  StringRef aSlice = "";
  if (a0 && a1 && *a1 >= *a0 && *a1 <= aSource_.size()) {
    aSlice = aSource_.substr(static_cast<size_t>(*a0),
                             static_cast<size_t>(*a1 - *a0));
  }
  StringRef bSlice = "";
  if (b0 && b1 && *b1 >= *b0 && *b1 <= bSource_.size()) {
    bSlice = bSource_.substr(static_cast<size_t>(*b0),
                             static_cast<size_t>(*b1 - *b0));
  }

  trace("include/patch",
        "{0} inc #{1} hunk A[{2},{3})->B[{4},{5}) Abytes=[{6},{7}) "
        "Bbytes=[{8},{9}) Aslice='{10}' Bslice='{11}'",
        tag, inc.id, h.aStart, h.aEnd, h.bStart, h.bEnd, a0, a1, b0, b1,
        stringutils::showWSWithClip(aSlice, 120),
        stringutils::showWSWithClip(bSlice, 120));
}

std::string RefoldEngine::PPArgSpanToString(const RefoldModel::PPArgSpan &sp,
                                            bool isStringifyOcc) {
  std::string storage;
  llvm::raw_string_ostream os(storage);

  os << formatv("{kind='{0}', A=[{1},{2}), argIdx={3}", sp.kind, sp.begin,
                sp.end, sp.argIdx);

  if (isStringifyOcc) {
    os << ", occ=STRINGIFY";
  }

  if (sp.kind == PPArgSpanKind::Paste) {
    os << formatv(", byte=[{0},{1})", sp.byteBegin, sp.byteEnd);
  }

  os << '}';
  return os.str();
}

std::string
RefoldEngine::PPArgSpanListToString(ArrayRef<RefoldModel::PPArgSpan> spans,
                                    ArrayRef<char> isStringify) {
  std::string storage;
  llvm::raw_string_ostream os(storage);

  os << '[';
  for (size_t i = 0; i < spans.size(); ++i) {
    if (i > 0)
      os << ", ";

    bool isStr = (i < isStringify.size() && isStringify[i]);
    os << PPArgSpanToString(spans[i], isStr);
  }
  os << ']';

  return os.str();
}

} // namespace refold
} // namespace clang
