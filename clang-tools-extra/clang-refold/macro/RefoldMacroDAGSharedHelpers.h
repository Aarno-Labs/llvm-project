//===--- RefoldMacroDAGSharedHelpers.h -------------------------*- C++ -*-===//
//
// Helpers shared by the DAG-chain phases of the whole-cover orchestrator.
//
// Owns the canonical implementations of `gatherArgLike`,
// `sanitizeArgLikeSpans`, `extractSpanText`, `normalizeLiftText`, and
// the small `SpanText` carrier.  Both `RefoldMacroDAGLeafDiscoveryPhase`
// and `RefoldMacroDAGLiftingPhase` use this shared header so leaf discovery
// and lifting apply the same span/text normalization policy.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDMACRODAGSHAREDHELPERS_H
#define LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDMACRODAGSHAREDHELPERS_H

#include "core/RefoldModel.h"
#include "edit/RefoldPatchTypes.h"
#include "macro/RefoldArgTextRecovery.h"
#include "macro/RefoldMacroPlannerHelpers.h"
#include "source/DiffAlgorithms.h"
#include "source/RefoldSourceMapper.h"
#include "source/RefoldToken.h"
#include "source/TokenTextHelpers.h"
#include "util/StringUtils.h"

#include "clang/Basic/SourceLocation.h"
#include "clang/Basic/TokenKinds.h"
#include "clang/Lex/Lexer.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace clang {
namespace refold {

/// Extracted argument text for a `PPArgSpan`, plus a reliability flag.
/// Reliability matters for paste spans whose token-internal byte range
/// may no longer align after the pasted token changes length; in that
/// case we fall back to clamped slicing and mark the result unreliable.
struct DAGSpanText {
  std::string text;
  bool reliable;
};

/// Proof-ranking width of a `PPArgSpan`: prefer spans that are more local
/// in PP-output bytes, falling back to token width when PP byte ranges
/// are absent.  DAG discovery and lifting phases share this ranking function
/// so leaf/root span comparisons stay deterministic.
inline uint64_t ppArgSpanProofWidth(const RefoldModel::PPArgSpan &span) {
  if (span.ppByteBegin && span.ppByteEnd && *span.ppByteEnd > *span.ppByteBegin)
    return uint64_t(*span.ppByteEnd - *span.ppByteBegin);
  if (span.end > span.begin)
    return uint64_t(span.end - span.begin);
  return ~uint64_t(0);
}

/// Collect every "argument-like" span for `mi`: standard argument spans,
/// stringify-derived spans, and paste sub-spans.  These are the only
/// spans the DAG path is willing to treat as editable arguments.
inline void
gatherArgLikeSpans(const RefoldModel::MacroInvocation &mi,
                   llvm::SmallVectorImpl<RefoldModel::PPArgSpan> &out) {
  out.clear();
  out.append(mi.argSpans.begin(), mi.argSpans.end());
  out.append(mi.stringifySpans.begin(), mi.stringifySpans.end());
  out.append(mi.pasteSpans.begin(), mi.pasteSpans.end());
}

/// Drop malformed or out-of-bounds argument-like spans before using them
/// for hunk-containment checks.  Invalid producer spans must not become
/// proof witnesses for args-only rewrite selection.
inline void
sanitizeArgLikeSpans(llvm::ArrayRef<PPTok> aToks,
                     llvm::SmallVectorImpl<RefoldModel::PPArgSpan> &spans) {
  llvm::SmallVector<RefoldModel::PPArgSpan, 8> valid;
  valid.reserve(spans.size());
  const uint64_t maxATokCount = static_cast<uint64_t>(aToks.size());
  for (const auto &sp : spans) {
    if (sp.end <= sp.begin)
      continue;
    if (sp.begin >= maxATokCount || sp.end > maxATokCount)
      continue;
    valid.push_back(sp);
  }
  spans.assign(valid.begin(), valid.end());
}

/// Extract the argument text corresponding to a `PPArgSpan`, from either
/// the A side (`fromB=false`) or the B side (`fromB=true`).
///
/// Special handling: paste spans and wrapped stringify spans may identify
/// a token-internal `[byteBegin, byteEnd)` subrange.  For B-side paste
/// spans, that range may no longer align if the pasted token changed
/// length; we attempt to re-derive the segment by preserving the
/// unchanged A-side prefix/suffix, otherwise we clamp the slice and mark
/// the result unreliable.  DAG replay callers use the reliability flag to
/// reject proof paths that would depend on guessed text.
inline std::optional<DAGSpanText>
extractDAGSpanText(const RefoldSourceMapper &sourceMapper,
                   const RefoldModel::PPArgSpan &sp, bool fromB) {
  if (sp.end <= sp.begin)
    return std::nullopt;

  // --- A-side extraction: exact bytes from the original pp token stream.
  if (!fromB) {
    llvm::StringRef a = sourceMapper.SliceASource(static_cast<size_t>(sp.begin),
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
    return DAGSpanText{a.trim().str(), /*reliable=*/true};
  }

  // --- B-side extraction: map the A-span to its B envelope and slice B.
  auto bEnv = sourceMapper.MapAToBTokenEnvelopeByPPArgSpan(sp);
  if (!bEnv)
    return std::nullopt;
  if (bEnv->second <= bEnv->first)
    return std::nullopt;
  llvm::StringRef b = sourceMapper.SliceBSource(bEnv->first, bEnv->second);
  bool reliable = true;

  if ((sp.kind == PPArgSpanKind::Paste ||
       sp.kind == PPArgSpanKind::Stringify) &&
      sp.byteBegin && sp.byteEnd) {
    if ((bEnv->second - bEnv->first) != 1)
      return std::nullopt;
    const uint64_t bb = *sp.byteBegin;
    const uint64_t be = *sp.byteEnd;

    // Token-internal byte ranges are computed from the A-side token
    // spelling.  If the B-side token changes length (for example
    // L"hello" -> L"goodbye" for wrapped stringify, or any pasted token
    // rewrite), using the raw [bb,be) slice can truncate the changed
    // segment.  Re-derive the B-side segment by peeling any unchanged
    // prefix/suffix when possible.
    llvm::StringRef aTok = sourceMapper.SliceASource(
        static_cast<size_t>(sp.begin), static_cast<size_t>(sp.end));
    if (be < bb || be > static_cast<uint64_t>(aTok.size()))
      return std::nullopt;
    llvm::StringRef aPref = aTok.take_front(static_cast<size_t>(bb));
    llvm::StringRef aSuff = aTok.drop_front(static_cast<size_t>(be));
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
  return DAGSpanText{b.trim().str(), reliable};
}

/// Normalize raw extracted text to a canonical "argument text" suitable
/// for comparison/lifting.  Stringify spans unescape the payload so
/// quote and escape choices don't create false diffs.  Paste spans are
/// usually raw token text, but when a stringified argument participates
/// in token pasting (`L## #x`) the pasted token contains quotes that the
/// invocation argument does not; only in that case do we unstringify.  The
/// normalized text is used by DAG lift and subtree proof paths before comparing
/// root-formal payloads.
inline std::optional<std::string>
normalizeDAGLiftText(const RefoldArgTextRecovery &argTextRecovery,
                     const RefoldModel::MacroInvocation *inv,
                     const RefoldModel::PPArgSpan &sp, llvm::StringRef raw0,
                     bool allowTopLevelComma) {
  llvm::StringRef raw = raw0.trim();

  // For stringify spans, compare against the de-escaped payload so that
  // quote/escape choices do not create false diffs.
  if (sp.kind == PPArgSpanKind::Stringify) {
    auto un =
        argTextRecovery.UnstringifyLiteralToArgText(raw, allowTopLevelComma);
    if (!un)
      return std::nullopt;
    return *un;
  }

  // Paste spans are normally token text (identifiers, numbers, string
  // literals, etc.).  However, when a stringified argument participates
  // in token pasting (`L## #x`), the pasted token will contain quotes
  // even though the invocation argument does not.  Only in that case
  // should we unstringify.
  if (sp.kind == PPArgSpanKind::Paste && sp.byteBegin && sp.byteEnd &&
      raw.find('"') != llvm::StringRef::npos) {
    bool invArgHasQuote = false;
    if (inv && inv->invText && inv->invB &&
        sp.argIdx < inv->invArgRanges.size()) {
      const auto &rng = inv->invArgRanges[sp.argIdx];
      if (rng.first && rng.second && *rng.first <= *rng.second &&
          *rng.first >= *inv->invB) {
        const uint64_t relB = *rng.first - *inv->invB;
        const uint64_t relE = *rng.second - *inv->invB;
        if (relE >= relB && relE <= inv->invText->size()) {
          llvm::StringRef invArg =
              llvm::StringRef(*inv->invText).slice(relB, relE);
          invArgHasQuote = invArg.find('"') != llvm::StringRef::npos;
        }
      }
    }
    if (!invArgHasQuote) {
      auto un = argTextRecovery.UnstringifyLiteralToArgText(raw);
      if (!un)
        return std::nullopt;
      return *un;
    }
  }

  return raw.str();
}

//===----------------------------------------------------------------------===//
// Small orchestrator/DAG helpers shared between the whole-cover
// orchestrator body and the DAG lifting phase.  All are pure functions
// that only touch caller-owned storage.
//===----------------------------------------------------------------------===//

/// Compare token-spelling vectors without relying on a concrete
/// container type.
inline bool tokenSpellingVectorsEqual(llvm::ArrayRef<std::string> lhs,
                                      llvm::ArrayRef<std::string> rhs) {
  if (lhs.size() != rhs.size())
    return false;
  for (size_t i = 0; i < lhs.size(); ++i)
    if (lhs[i] != rhs[i])
      return false;
  return true;
}

/// Return true when a token hunk is wholly covered by one of the supplied
/// producer PP spans.
inline bool hunkWithinPPSpans(const diffutils::Hunk &hunk,
                              llvm::ArrayRef<RefoldModel::PPSpan> spans) {
  for (const auto &span : spans)
    if (span.begin <= hunk.aStart && hunk.aEnd <= span.end &&
        span.begin < span.end)
      return true;
  return false;
}

/// Canonically order producer PP spans by token range.
inline bool ppSpanLessByTokenRange(const RefoldModel::PPSpan &lhs,
                                   const RefoldModel::PPSpan &rhs) {
  if (lhs.begin != rhs.begin)
    return lhs.begin < rhs.begin;
  return lhs.end < rhs.end;
}

/// Merge the materialized B-token envelope from one macro patch into
/// another.  The destination keeps any envelope it already had,
/// extended to cover the source envelope as well.
inline void unionMacroPatchMaterializedBTokenRange(MacroPatch &dst,
                                                   const MacroPatch &src) {
  if (!src.materialized.hasBTokenRange)
    return;
  if (!dst.materialized.hasBTokenRange) {
    dst.materialized.hasBTokenRange = true;
    dst.materialized.bTokStart = src.materialized.bTokStart;
    dst.materialized.bTokEnd = src.materialized.bTokEnd;
    return;
  }
  dst.materialized.bTokStart =
      std::min(dst.materialized.bTokStart, src.materialized.bTokStart);
  dst.materialized.bTokEnd =
      std::max(dst.materialized.bTokEnd, src.materialized.bTokEnd);
}

/// Return the sorted subset of `required` that is not present in
/// `provided`.  Used to compute missing paste-arg-idx support sets.
inline llvm::SmallVector<uint32_t, 8>
computeSortedMissingUInt32s(llvm::ArrayRef<uint32_t> required,
                            llvm::ArrayRef<uint32_t> provided) {
  llvm::SmallVector<uint32_t, 8> missing;
  for (uint32_t value : required) {
    if (!llvm::is_contained(provided, value))
      missing.push_back(value);
  }
  llvm::sort(missing);
  return missing;
}

/// Return true when any arg index in `argIdxs` participates in a pasted
/// token surface on `mi`.
inline bool anyInvocationArgTouchesPaste(const RefoldModel::MacroInvocation &mi,
                                         llvm::ArrayRef<uint32_t> argIdxs) {
  for (uint32_t argIdx : argIdxs) {
    if (invocationArgTouchesPaste(mi, argIdx))
      return true;
  }
  return false;
}

/// Return true when every touched paste arg in `argIdxs` belongs to the
/// supplied allow-list set.  Non-paste arguments are ignored.
template <typename SetLike>
bool allTouchedPasteArgsAreContained(const RefoldModel::MacroInvocation &mi,
                                     llvm::ArrayRef<uint32_t> argIdxs,
                                     const SetLike &allowedArgIdxs) {
  for (uint32_t argIdx : argIdxs) {
    if (invocationArgTouchesPaste(mi, argIdx) && !allowedArgIdxs.count(argIdx))
      return false;
  }
  return true;
}

/// Count non-overlapping occurrences of `needle` in `text`.
inline uint64_t countSubstringOccurrences(llvm::StringRef text,
                                          llvm::StringRef needle) {
  if (needle.empty())
    return 0;
  uint64_t count = 0;
  for (size_t pos = 0; (pos = text.find(needle, pos)) != llvm::StringRef::npos;
       pos += needle.size())
    ++count;
  return count;
}

/// Total ordering for paste-span pointer groups.
inline bool pasteSpanPtrLessByByteRange(const RefoldModel::PPArgSpan *a,
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

/// Format a list of uint32_t values for trace diagnostics.
inline std::string formatUInt32List(llvm::ArrayRef<uint32_t> values) {
  std::string out;
  llvm::raw_string_ostream os(out);
  os << "[";
  for (size_t i = 0; i < values.size(); ++i) {
    if (i)
      os << ", ";
    os << values[i];
  }
  os << "]";
  return os.str();
}

/// Collect the unique formal indices participating in a pasted token surface.
inline llvm::SmallVector<uint32_t, 8> collectSortedUniquePasteArgIdxs(
    llvm::ArrayRef<RefoldModel::PPArgSpan> pasteSpans) {
  llvm::SmallVector<uint32_t, 8> argIdxs;
  for (const auto &ps : pasteSpans) {
    if (!llvm::is_contained(argIdxs, ps.argIdx))
      argIdxs.push_back(ps.argIdx);
  }
  llvm::sort(argIdxs);
  return argIdxs;
}

/// One character-diff hunk derived while composing compatible string
/// rewrites against a shared base string.
struct CompatibleStringRewriteHunk {
  uint64_t oldBegin = 0;
  uint64_t oldEnd = 0;
  std::string repl;
};

inline bool
sameCompatibleStringRewriteHunk(const CompatibleStringRewriteHunk &lhs,
                                const CompatibleStringRewriteHunk &rhs) {
  return lhs.oldBegin == rhs.oldBegin && lhs.oldEnd == rhs.oldEnd &&
         lhs.repl == rhs.repl;
}

inline bool
compatibleStringRewriteHunksOverlap(const CompatibleStringRewriteHunk &lhs,
                                    const CompatibleStringRewriteHunk &rhs) {
  return lhs.oldBegin < rhs.oldEnd && rhs.oldBegin < lhs.oldEnd;
}

inline bool
compatibleStringRewriteHunkBeginsBefore(const CompatibleStringRewriteHunk &hunk,
                                        uint64_t pos) {
  return hunk.oldBegin < pos;
}

/// Merge several independently-proven replacements against the same base
/// text.  Returns nullopt when any two rewrites overlap on incompatible
/// regions; otherwise returns the deterministically-merged result.
inline std::optional<std::string> mergeCompatibleStringReplacements(
    llvm::StringRef baseOld, llvm::ArrayRef<llvm::StringRef> replacements) {
  std::vector<CompatibleStringRewriteHunk> merged;
  std::vector<llvm::StringRef> aRefs = stringutils::splitChars(baseOld);

  for (llvm::StringRef replText : replacements) {
    if (replText == baseOld)
      continue;

    std::vector<llvm::StringRef> bRefs = stringutils::splitChars(replText);
    auto steps = diffutils::diff(aRefs, bRefs);
    auto hunks = diffutils::coalesce(steps);

    for (const auto &hunk : hunks) {
      CompatibleStringRewriteHunk piece{
          hunk.aStart, hunk.aEnd,
          replText
              .slice(static_cast<size_t>(hunk.bStart),
                     static_cast<size_t>(hunk.bEnd))
              .str()};

      auto it = std::lower_bound(merged.begin(), merged.end(), piece.oldBegin,
                                 compatibleStringRewriteHunkBeginsBefore);

      if (it != merged.begin()) {
        const auto &prev = *std::prev(it);
        if (sameCompatibleStringRewriteHunk(prev, piece))
          continue;
        if (compatibleStringRewriteHunksOverlap(prev, piece))
          return std::nullopt;
      }
      if (it != merged.end()) {
        if (sameCompatibleStringRewriteHunk(*it, piece))
          continue;
        if (compatibleStringRewriteHunksOverlap(*it, piece))
          return std::nullopt;
      }

      merged.insert(it, std::move(piece));
    }
  }

  std::string out = baseOld.str();
  for (auto it = merged.rbegin(); it != merged.rend(); ++it)
    out.replace(static_cast<size_t>(it->oldBegin),
                static_cast<size_t>(it->oldEnd - it->oldBegin), it->repl);
  return out;
}

/// Emit every byte boundary in the half-open range `(begin,end]`.
inline void emitByteCutRange(size_t begin, size_t end,
                             llvm::function_ref<void(unsigned)> emitCut) {
  for (size_t cut = begin + 1; cut <= end; ++cut)
    emitCut(static_cast<unsigned>(cut));
}

/// True for string and character literal tokens whose interior bytes must
/// not be inspected for delimiters or cut points.
inline bool isOpaqueLiteralToken(clang::tok::TokenKind kind) {
  switch (kind) {
  case clang::tok::string_literal:
  case clang::tok::wide_string_literal:
  case clang::tok::utf8_string_literal:
  case clang::tok::utf16_string_literal:
  case clang::tok::utf32_string_literal:
  case clang::tok::char_constant:
  case clang::tok::wide_char_constant:
  case clang::tok::utf8_char_constant:
  case clang::tok::utf16_char_constant:
  case clang::tok::utf32_char_constant:
    return true;
  default:
    return false;
  }
}

/// Return the byte offset immediately after a complete comment token.
inline std::optional<size_t> commentCutEnd(llvm::StringRef text, size_t begin,
                                           size_t end) {
  llvm::StringRef comment = text.slice(begin, end);
  if (comment.starts_with("//")) {
    const size_t newline = text.find('\n', begin);
    if (newline == llvm::StringRef::npos)
      return std::nullopt;
    return newline + 1;
  }
  if (comment.starts_with("/*")) {
    const size_t close = text.find("*/", begin + 2);
    if (close == llvm::StringRef::npos)
      return std::nullopt;
    return close + 2;
  }
  return end;
}

/// Update delimiter nesting for raw tokens that contribute to top-level
/// comma and cut-point decisions.
inline void updateTopLevelDelimiterDepth(clang::tok::TokenKind kind,
                                         int &parenDepth, int &bracketDepth,
                                         int &braceDepth) {
  switch (kind) {
  case clang::tok::l_paren:
    ++parenDepth;
    break;
  case clang::tok::r_paren:
    if (parenDepth > 0)
      --parenDepth;
    break;
  case clang::tok::l_square:
    ++bracketDepth;
    break;
  case clang::tok::r_square:
    if (bracketDepth > 0)
      --bracketDepth;
    break;
  case clang::tok::l_brace:
    ++braceDepth;
    break;
  case clang::tok::r_brace:
    if (braceDepth > 0)
      --braceDepth;
    break;
  default:
    break;
  }
}

/// True when no tracked delimiter family is currently nested.
inline bool isAtTopLevel(int parenDepth, int bracketDepth, int braceDepth) {
  return parenDepth == 0 && bracketDepth == 0 && braceDepth == 0;
}

/// Enumerate byte cut-points that are balanced with respect to top-level
/// delimiters in `text`.
inline void enumerateTopLevelBalancedCutPointsWithLexer(
    llvm::StringRef text, const clang::LangOptions &lang,
    llvm::function_ref<void(unsigned)> emitCut) {
  emitCut(0u);
  if (text.empty())
    return;

  const clang::SourceLocation baseLoc =
      clang::SourceLocation::getFromRawEncoding(1);
  std::string lexBuf = text.str();
  lexBuf.push_back('\0');
  const char *bufStart = lexBuf.data();
  const char *bufEnd = bufStart + text.size();
  clang::Lexer lexer(baseLoc, lang, bufStart, bufStart, bufEnd);
  lexer.SetCommentRetentionState(true);

  int parenDepth = 0;
  int bracketDepth = 0;
  int braceDepth = 0;
  size_t covered = 0;
  clang::Token token;

  while (true) {
    lexer.LexFromRawLexer(token);
    if (token.is(clang::tok::eof))
      break;

    const size_t tokenBegin =
        std::min(refoldTokenOffsetFromBase(token, baseLoc), text.size());
    const size_t tokenEnd =
        std::min(refoldTokenEndOffsetFromBase(token, baseLoc), text.size());

    if (covered < tokenBegin &&
        isAtTopLevel(parenDepth, bracketDepth, braceDepth))
      emitByteCutRange(covered, tokenBegin, emitCut);

    if (token.is(clang::tok::comment)) {
      covered = tokenEnd;
      std::optional<size_t> cutEnd = commentCutEnd(text, tokenBegin, tokenEnd);
      if (cutEnd) {
        covered = std::min(*cutEnd, text.size());
        if (isAtTopLevel(parenDepth, bracketDepth, braceDepth))
          emitCut(static_cast<unsigned>(covered));
      }
      continue;
    }

    if (isOpaqueLiteralToken(token.getKind())) {
      covered = tokenEnd;
      if (isAtTopLevel(parenDepth, bracketDepth, braceDepth))
        emitCut(static_cast<unsigned>(tokenEnd));
      continue;
    }

    updateTopLevelDelimiterDepth(token.getKind(), parenDepth, bracketDepth,
                                 braceDepth);
    covered = tokenEnd;

    if (isAtTopLevel(parenDepth, bracketDepth, braceDepth))
      emitByteCutRange(tokenBegin, tokenEnd, emitCut);
  }

  if (covered < text.size() &&
      isAtTopLevel(parenDepth, bracketDepth, braceDepth))
    emitByteCutRange(covered, text.size(), emitCut);
}

} // namespace refold
} // namespace clang

#endif // LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDMACRODAGSHAREDHELPERS_H
