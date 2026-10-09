//===--- RefoldMacroReplayStabilityValidator.cpp ---------------*- C++ -*-===//
//
// Implementation of the final-admission replay-stability validator.  See the
// public header for the service contract.  The helper routines in this file
// keep body-splitting, exclusion, and fixed-piece replay checks factored into
// named lexical operations so the public predicates read as flat sequences of
// validation steps.
//
//===----------------------------------------------------------------------===//

#include "macro/RefoldMacroReplayStabilityValidator.h"

#include "line-control/LineDirectiveInserter.h"
#include "macro/RefoldArgTextRecovery.h"
#include "macro/RefoldMacroPlannerHelpers.h"
#include "macro/RefoldMacroReplay.h"
#include "macro/RefoldMacroStateProof.h"
#include "macro/RefoldMacroSubtreeReplayValidator.h"
#include "macro/RefoldMacroTopology.h"
#include "model/RefoldModel.h"
#include "model/RefoldPathIdentity.h"
#include "source/RefoldSourceMapper.h"
#include "support/RefoldLog.h"
#include "support/StringUtils.h"

#include "clang/Lex/Lexer.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Support/FormatVariadic.h"
#include "llvm/Support/MemoryBuffer.h"

#include <algorithm>
#include <optional>
#include <string>
#include <utility>

using namespace llvm;

namespace clang {
namespace refold {

namespace {

/// A token interval in A-space used by fixed-body and argument-dependent
/// replay guards.
struct TokenInterval {
  uint64_t begin = 0;
  uint64_t end = 0;
};

/// One element in the whole-envelope replay tiling: either fixed replacement
/// body surface or argument-dependent output surface.
struct WholeEnvelopeReplayElem {
  bool isArgumentDependent = false;
  uint64_t aBegin = 0;
  uint64_t aEnd = 0;
};

/// Canonical ordering for token intervals.
bool tokenIntervalLess(const TokenInterval &lhs, const TokenInterval &rhs) {
  if (lhs.begin != rhs.begin)
    return lhs.begin < rhs.begin;
  return lhs.end < rhs.end;
}

/// Append a non-empty token interval.
void addNonEmptyTokenInterval(SmallVectorImpl<TokenInterval> &out,
                              uint64_t begin, uint64_t end) {
  if (begin < end)
    out.push_back({begin, end});
}

/// Split `body` into the sub-intervals that lie outside the sorted
/// `excludedIntervals`, appending them to `fixed`.  Returns false when `body`
/// escapes `cover`.  This is the shared fixed-body replay slicer used by
/// args-only envelope validation after argument-owned intervals have been
/// excluded, and by root-body replay after known macro-owned or argument-owned
/// surfaces have been excluded.
bool appendFixedPiecesOutside(SmallVectorImpl<TokenInterval> &fixed,
                              TokenInterval body,
                              std::pair<uint64_t, uint64_t> cover,
                              ArrayRef<TokenInterval> excludedIntervals) {
  if (body.begin >= body.end)
    return true;
  if (body.begin < cover.first || body.end > cover.second ||
      body.end < body.begin)
    return false;

  uint64_t cursor = body.begin;
  for (const TokenInterval &excluded : excludedIntervals) {
    if (excluded.end <= cursor)
      continue;
    if (excluded.begin >= body.end)
      break;
    if (excluded.begin > cursor)
      fixed.push_back({cursor, std::min<uint64_t>(excluded.begin, body.end)});
    cursor = std::max(cursor, std::min<uint64_t>(excluded.end, body.end));
  }
  if (cursor < body.end)
    fixed.push_back({cursor, body.end});
  return true;
}

/// Append each non-empty token span clipped to `cover`.
///
/// The span range must carry exact producer surfaces rather than a bounding
/// cover.  Clipping is required because a validated descendant may begin or end
/// outside the root invocation currently being audited.
template <typename SpanRange>
void appendClippedTokenIntervals(SmallVectorImpl<TokenInterval> &out,
                                 const SpanRange &spans,
                                 std::pair<uint64_t, uint64_t> cover) {
  for (const auto &span : spans) {
    const uint64_t begin = std::max<uint64_t>(span.begin, cover.first);
    const uint64_t end = std::min<uint64_t>(span.end, cover.second);
    addNonEmptyTokenInterval(out, begin, end);
  }
}

/// Append the exact A-token surfaces produced by one validated descendant.
///
/// A descendant's `PPCover` is only the convex hull of its producer spans.  It
/// is suitable for containment and ranking, but it is not an ownership proof:
/// sparse child output can leave parent-owned fixed-body tokens inside that
/// hull.  Excluding the hull from a root literal-body audit would therefore
/// hide edits that the preserved root callsite cannot reproduce.
///
/// Only the producer's exact body and argument-dependent surfaces are safe to
/// delegate to descendant replay.  Generic `spans` are deliberately omitted;
/// they may carry the same conservative ancestry envelope that widened the
/// cover.  Missing exact surfaces remain fail-closed and are audited as root
/// fixed body instead of being inferred from a bounding interval.
void appendExactDescendantReplaySurfaces(
    SmallVectorImpl<TokenInterval> &out,
    const RefoldModel::MacroInvocation &descendant,
    std::pair<uint64_t, uint64_t> rootCover) {
  appendClippedTokenIntervals(out, descendant.bodySpans, rootCover);
  appendClippedTokenIntervals(out, descendant.argSpans, rootCover);
  appendClippedTokenIntervals(out, descendant.stringifySpans, rootCover);
  appendClippedTokenIntervals(out, descendant.pasteSpans, rootCover);
}

/// Merge overlapping/adjacent intervals in place.  Used by
/// `RootPreservingCandidateHasLiteralFixedRootBodyReplay` only.
void normalizeIntervals(SmallVectorImpl<TokenInterval> &spans) {
  llvm::sort(spans, tokenIntervalLess);
  SmallVector<TokenInterval, 16> merged;
  for (const TokenInterval &span : spans) {
    if (span.begin >= span.end)
      continue;
    if (!merged.empty() && span.begin <= merged.back().end) {
      merged.back().end = std::max(merged.back().end, span.end);
      continue;
    }
    merged.push_back(span);
  }
  spans.clear();
  spans.append(merged.begin(), merged.end());
}

/// Chooses the B-token envelope used by the args-only whole-envelope replay
/// audit. Boundary pure insertions are excluded when the edge surface is fixed
/// replacement-list body, because fixed macro body cannot own new edge text.
/// When the edge surface is argument-dependent, keep the untrimmed edge: the
/// changed actual/stringify/paste surface is allowed to supply new boundary
/// spelling, even if the byte diff represents that spelling as an insertion at
/// the whole expansion frontier.
std::pair<size_t, size_t> chooseArgumentAwareReplayBEnvelope(
    std::pair<size_t, size_t> wholeB,
    const std::optional<std::pair<size_t, size_t>> &trimmedB,
    ArrayRef<WholeEnvelopeReplayElem> elems) {
  if (!trimmedB || wholeB.first > trimmedB->first ||
      trimmedB->first > trimmedB->second || trimmedB->second > wholeB.second)
    return wholeB;

  std::pair<size_t, size_t> replayB = *trimmedB;
  if (!elems.empty()) {
    if (elems.front().isArgumentDependent)
      replayB.first = wholeB.first;
    if (elems.back().isArgumentDependent)
      replayB.second = wholeB.second;
  }

  if (replayB.second < replayB.first)
    replayB.second = replayB.first;
  return replayB;
}

/// One piece of the expansion an args-only rewrite claims, in replay order:
/// either the tokens the rewrite is known to produce there, or a surface whose
/// tokens the audit does not derive and which may therefore be any run of B
/// tokens.
struct ClaimedExpansionPiece {
  bool derived = false;
  SmallVector<std::string, 16> spellings;
};

/// Return the token spellings of macro actual \p text in order, comments
/// dropped as Clang drops them before substitution.  When \p namesMacro is
/// non-null it is set if an identifier among them names a macro \p model
/// records a directive for: such an actual's expansion is not its spelling.
SmallVector<std::string, 16> lexActualSpellings(StringRef text,
                                                const LangOptions &lang,
                                                const RefoldModel &model,
                                                bool *namesMacro = nullptr) {
  const SourceLocation baseLoc = SourceLocation::getFromRawEncoding(1);
  std::string lexBuf = text.str();
  lexBuf.push_back('\0');
  Lexer lexer(baseLoc, lang, lexBuf.data(), lexBuf.data(),
              lexBuf.data() + text.size());
  SmallVector<std::string, 16> spellings;
  Token token;
  while (true) {
    lexer.LexFromRawLexer(token);
    if (token.is(tok::eof))
      break;
    if (token.is(tok::comment))
      continue;
    const size_t offset =
        token.getLocation().getRawEncoding() - baseLoc.getRawEncoding();
    StringRef spelling = text.substr(offset, token.getLength());
    if (namesMacro && token.is(tok::raw_identifier) &&
        !model.GetMacroDirectivesByName(spelling).empty())
      *namesMacro = true;
    spellings.push_back(spelling.str());
  }
  return spellings;
}

/// Return the tokens argument surface \p elem expands to when the rewrite
/// determines them: the surface is exactly a run of standard occurrences of
/// formals in \p derivedActuals, and every other argument-dependent surface
/// meeting it lies inside one of those occurrences.
std::optional<SmallVector<std::string, 16>> derivedSurfaceSpellings(
    const RefoldModel::MacroInvocation &m, const WholeEnvelopeReplayElem &elem,
    ArrayRef<std::optional<SmallVector<std::string, 16>>> derivedActuals,
    ArrayRef<TokenInterval> argumentDependentIntervals) {
  SmallVector<const RefoldModel::PPArgSpan *, 4> occurrences;
  for (const RefoldModel::PPArgSpan &span : m.argSpans)
    if (span.kind == PPArgSpanKind::Standard &&
        span.argIdx < derivedActuals.size() && derivedActuals[span.argIdx] &&
        elem.aBegin <= span.begin && span.end <= elem.aEnd &&
        span.begin < span.end)
      occurrences.push_back(&span);
  llvm::sort(occurrences, [](const auto *lhs, const auto *rhs) {
    return std::tie(lhs->begin, lhs->end) < std::tie(rhs->begin, rhs->end);
  });
  occurrences.erase(std::unique(occurrences.begin(), occurrences.end(),
                                [](const auto *lhs, const auto *rhs) {
                                  return lhs->begin == rhs->begin &&
                                         lhs->end == rhs->end;
                                }),
                    occurrences.end());

  uint64_t cursor = elem.aBegin;
  for (const RefoldModel::PPArgSpan *span : occurrences) {
    if (span->begin != cursor)
      return std::nullopt;
    cursor = span->end;
  }
  if (occurrences.empty() || cursor != elem.aEnd)
    return std::nullopt;

  for (const TokenInterval &surface : argumentDependentIntervals) {
    if (surface.end <= elem.aBegin || elem.aEnd <= surface.begin)
      continue;
    if (llvm::none_of(occurrences, [&](const RefoldModel::PPArgSpan *span) {
          return span->begin <= surface.begin && surface.end <= span->end;
        }))
      return std::nullopt;
  }

  SmallVector<std::string, 16> spellings;
  for (const RefoldModel::PPArgSpan *span : occurrences)
    llvm::append_range(spellings, *derivedActuals[span->argIdx]);
  return spellings;
}

/// Return whether B tokens `[lo, hi)` split, in order, into \p pieces, the
/// split starting at one of \p starts and ending at one of \p ends: each
/// derived piece exactly its spellings, every other piece any run of tokens.
bool bTokensSplitIntoPieces(ArrayRef<PPTok> bToks, size_t lo, size_t hi,
                            ArrayRef<size_t> starts, ArrayRef<size_t> ends,
                            ArrayRef<ClaimedExpansionPiece> pieces) {
  if (lo > hi || hi > bToks.size())
    return false;
  // `reach[p]` holds when the pieces so far can end at B token `lo + p`.
  std::vector<char> reach(hi - lo + 1, 0);
  for (size_t start : starts)
    if (lo <= start && start <= hi)
      reach[start - lo] = 1;

  for (const ClaimedExpansionPiece &piece : pieces) {
    std::vector<char> next(reach.size(), 0);
    if (!piece.derived) {
      auto first = llvm::find(reach, 1);
      std::fill(next.begin() + (first - reach.begin()), next.end(), 1);
    } else {
      const size_t len = piece.spellings.size();
      for (size_t p = 0; p + len < reach.size(); ++p) {
        if (!reach[p])
          continue;
        bool matches = true;
        for (size_t k = 0; k < len && matches; ++k)
          matches = bToks[lo + p + k].spelling == piece.spellings[k];
        if (matches)
          next[p + len] = 1;
      }
    }
    reach = std::move(next);
  }
  return llvm::any_of(ends, [&](size_t end) {
    return lo <= end && end <= hi && reach[end - lo];
  });
}

/// Return whether the expansion of an args-only rewrite reproduces the image
/// of \p cover under the token diff wherever the rewrite determines it.
///
/// The rewrite determines the fixed text of the tiling \p elems and every
/// argument surface built from standard occurrences of a formal it rewrote to
/// an actual naming no macro: such an actual expands to its own tokens.  Other
/// surfaces are left unconstrained, so a tiling that attributes repeated
/// tokens differently from the audit is accepted when one exists.  The split
/// covers the whole image, so a B token there that the rewrite does not
/// produce fails.  The one exception is a pure token insertion at a cover edge
/// held by fixed text: fixed text cannot own it, and its own hunk realizes it.
bool rewrittenActualsReplayInB(
    const RefoldMacroReplayStabilityValidator::Dependencies &deps,
    const RefoldModel::MacroInvocation &m, StringRef baseInvText,
    StringRef replacement, std::pair<uint64_t, uint64_t> cover,
    ArrayRef<WholeEnvelopeReplayElem> elems,
    ArrayRef<TokenInterval> argumentDependentIntervals) {
  RefoldMacroActualLayout layout({&deps.lexLang});
  std::optional<std::vector<std::pair<size_t, size_t>>> oldActuals =
      layout.GetMacroInvocationFormalArgContentRanges(m, baseInvText);
  std::optional<std::vector<std::pair<size_t, size_t>>> newActuals =
      layout.GetMacroInvocationFormalArgContentRanges(m, replacement);
  // Without both layouts no formal is known to be rewritten, and nothing is
  // derived.
  if (!oldActuals || !newActuals || oldActuals->size() != newActuals->size())
    return true;

  SmallVector<std::optional<SmallVector<std::string, 16>>, 8> derivedActuals(
      newActuals->size());
  bool anyDerivedActual = false;
  for (size_t formal = 0; formal < newActuals->size(); ++formal) {
    const auto [oldBegin, oldEnd] = (*oldActuals)[formal];
    const auto [newBegin, newEnd] = (*newActuals)[formal];
    bool newNamesMacro = false;
    SmallVector<std::string, 16> oldSpellings = lexActualSpellings(
        baseInvText.slice(oldBegin, oldEnd), deps.lexLang, deps.model);
    SmallVector<std::string, 16> newSpellings =
        lexActualSpellings(replacement.slice(newBegin, newEnd), deps.lexLang,
                           deps.model, &newNamesMacro);
    if (newNamesMacro || newSpellings == oldSpellings)
      continue;
    derivedActuals[formal] = std::move(newSpellings);
    anyDerivedActual = true;
  }
  if (!anyDerivedActual)
    return true;

  SmallVector<ClaimedExpansionPiece, 32> pieces;
  bool anyDerivedSurface = false;
  for (const WholeEnvelopeReplayElem &elem : elems) {
    ClaimedExpansionPiece &piece = pieces.emplace_back();
    if (!elem.isArgumentDependent) {
      piece.derived = true;
      for (uint64_t aTok = elem.aBegin; aTok < elem.aEnd; ++aTok)
        piece.spellings.push_back(deps.aToks[aTok].spelling);
      continue;
    }
    if (std::optional<SmallVector<std::string, 16>> spellings =
            derivedSurfaceSpellings(m, elem, derivedActuals,
                                    argumentDependentIntervals)) {
      piece.derived = true;
      piece.spellings = std::move(*spellings);
      anyDerivedSurface = true;
    }
  }
  if (!anyDerivedSurface || pieces.empty())
    return true;

  // The expansion replaces the cover between neighbours that the hunks place,
  // so it must reproduce the cover's image under the token diff.  The byte
  // projection the patch records can disagree with it across repeated bytes.
  std::optional<std::pair<size_t, size_t>> image =
      deps.sourceMapper.MapATokRangeToBTokenEnvelopeByTokenDiff(cover.first,
                                                                cover.second);
  if (!image)
    return false;
  std::pair<size_t, size_t> withoutEdgeInsertions = *image;
  deps.sourceMapper.TrimPureBoundaryInsertionsFromTokenEnvelope(
      cover.first, cover.second, withoutEdgeInsertions);
  SmallVector<size_t, 2> starts{image->first};
  SmallVector<size_t, 2> ends{image->second};
  if (!elems.front().isArgumentDependent)
    starts.push_back(withoutEdgeInsertions.first);
  if (!elems.back().isArgumentDependent)
    ends.push_back(withoutEdgeInsertions.second);
  return bTokensSplitIntoPieces(deps.bToks, image->first, image->second, starts,
                                ends, pieces);
}

} // namespace

RefoldMacroReplayStabilityValidator::RefoldMacroReplayStabilityValidator(
    Dependencies deps)
    : deps_(std::move(deps)) {}

bool RefoldMacroReplayStabilityValidator::
    StructurePreservingCallsiteHasStableFormalSyntax(
        const MacroPatch &patch, const RefoldModel::MacroInvocation &m) const {
  // Invocation-preserving macro patches are replayed by the preprocessor as
  // function-like macro callsites.  Therefore the replacement text must itself
  // be a complete, well-formed invocation of this macro.
  if (!patch.proof.preservesInvocationStructure ||
      patch.proof.proofRootMacroId != m.id)
    return true;
  if (m.subkind != "func")
    return true;
  const bool directCalleeSubstitution =
      patch.proof.kind == MacroPatchProofKind::DirectCalleeSubstitution;
  if (!directCalleeSubstitution &&
      !invocationSpanMatchesCallsitePrefix(patch.replacement, m)) {
    REFOLD_LOG_TRACE("macro/proof",
                     "suppress structure-preserving macro replay: inv id={0} "
                     "name={1} replacement no longer has a matching callsite "
                     "prefix",
                     m.id, m.name);
    return false;
  }

  // Clang ends the invocation at the first `)` that closes its actual list.  A
  // token after it -- left there by an actual carrying an unmatched `)` -- is
  // not part of the invocation at all: the macro expands from a prefix of the
  // actuals the candidate meant, and the rest is emitted after the expansion.
  bool trailingTokens = false;
  auto parsedActuals =
      RefoldArgTextRecovery::LexMacroInvocationActualContentRanges(
          patch.replacement, deps_.lexLang, &trailingTokens);
  if (!parsedActuals || trailingTokens) {
    REFOLD_LOG_TRACE("macro/proof",
                     "suppress structure-preserving macro replay: inv id={0} "
                     "name={1} replacement is not a complete macro invocation: "
                     "'{2}'",
                     m.id, m.name,
                     stringutils::showWsWithClip(patch.replacement, 220));
    return false;
  }

  const size_t formalN = m.defParams.size();
  size_t actualN = parsedActuals->size();
  if (actualN == 0 && formalN != 0) {
    // `M()` is zero actuals for a zero-parameter macro, but for a macro with
    // parameters it is one empty actual followed by any omitted variadic tail.
    actualN = 1;
  }

  bool trailingFormalsAreVariadic = true;
  for (size_t i = actualN; i < formalN; ++i) {
    if (!isMacroInvocationVariadicFormal(m, i)) {
      trailingFormalsAreVariadic = false;
      break;
    }
  }

  bool arityCompatible = false;
  if (actualN == formalN) {
    arityCompatible = true;
  } else if (formalN != 0 && actualN > formalN) {
    // Surplus actuals are valid only for a final variadic formal; they
    // collectively form the variadic tail.
    arityCompatible = isMacroInvocationVariadicFormal(m, formalN - 1);
  } else if (actualN < formalN) {
    // Missing actuals are valid only for omitted trailing variadic formals,
    // e.g. `M(x)` for `M(x, ...)`.
    arityCompatible = trailingFormalsAreVariadic;
  }

  if (!arityCompatible) {
    REFOLD_LOG_TRACE("macro/proof",
                     "suppress structure-preserving macro replay: inv id={0} "
                     "name={1} replacement actual/formal arity is invalid: "
                     "actuals={2} formals={3} text='{4}'",
                     m.id, m.name, static_cast<unsigned>(actualN),
                     static_cast<unsigned>(formalN),
                     stringutils::showWsWithClip(patch.replacement, 220));
    return false;
  }

  return true;
}

bool RefoldMacroReplayStabilityValidator::
    ArgsOnlyWholeEnvelopeCandidateHasLiteralBodyReplay(
        const MacroSubtreeReplayValidationContext &ctx,
        const MacroPatch &patch) const {
  const RefoldModel::MacroInvocation &m = ctx.rootInvocation;
  auto isArgsOnlyInvocationPreservingProof = [&]() {
    switch (patch.proof.kind) {
    case MacroPatchProofKind::ArgsOnlyStandard:
    case MacroPatchProofKind::ArgsOnlyPasteSingle:
    case MacroPatchProofKind::ArgsOnlyPasteMulti:
    case MacroPatchProofKind::ArgsOnlyPurePasteOnly:
    case MacroPatchProofKind::ArgsOnlyPairedPureInsertion:
      return true;
    case MacroPatchProofKind::DirectCalleeSubstitution:
    case MacroPatchProofKind::PasteDerivedCalleeSelector:
    case MacroPatchProofKind::RecursiveTupleGeneratedCalleeReplay:
    case MacroPatchProofKind::DagSubtreeRoot:
    case MacroPatchProofKind::CallChainSuffix:
    case MacroPatchProofKind::CounterLiteral:
    case MacroPatchProofKind::WholeCoverRealization:
    case MacroPatchProofKind::Unknown:
      return false;
    }
    return false;
  };

  if (!isArgsOnlyInvocationPreservingProof() ||
      !patch.proof.preservesInvocationStructure ||
      patch.proof.proofRootMacroId != m.id)
    return true;
  if (!patch.materialized.hasBTokenRange)
    return true;

  // Complete replay solvers have already checked the replacement-list
  // tape against the whole edited B envelope.  Do not second-guess those
  // proofs with a direct literal-body cursor walk: repeated formals,
  // empty actual slots, VA_OPT transitions, and higher-order generated
  // callees can all produce changed downstream body tokens while still
  // preserving the root invocation soundly.  The literal-body audit below
  // is only for local formal/operator rewrites that certify a whole-envelope
  // range without such a complete replay witness.
  if (patch.proof.wholeEnvelopeReplay &&
      patch.proof.wholeEnvelopeReplay->replayValidated &&
      patch.proof.wholeEnvelopeReplay->rootMacroId ==
          patch.proof.proofRootMacroId)
    return true;

  const std::optional<std::pair<uint64_t, uint64_t>> cover =
      RefoldMacroWholeCoverProof::GetWholeCoverATokRange(m);
  if (!cover || cover->first >= cover->second)
    return true;

  const std::optional<std::pair<size_t, size_t>> wholeB =
      deps_.sourceMapper
          .MapATokRangeAToBTokenEnvelopePreserveBoundaryInsertions(
              cover->first, cover->second);
  if (!wholeB || wholeB->first >= wholeB->second)
    return true;

  // The materialized whole-cover envelope intentionally preserves
  // zero-width insertions that are anchored exactly at either edge of the
  // macro expansion.  Keep both views: the untrimmed envelope is the claimed
  // materialized target, while the trimmed envelope is useful when an edge
  // insertion would otherwise be misread as fixed replacement-list body.  The
  // final replay cursor is selected after the replay elements are known, since
  // argument-dependent edge surfaces may legitimately own new boundary text.
  std::optional<std::pair<size_t, size_t>> trimmedReplayB =
      deps_.sourceMapper.MapATokRangeAToBTokenEnvelope(cover->first,
                                                       cover->second);

  // This guard is only about whole-envelope claims.  Narrow args-only
  // materializations are checked by their local formal/paste/stringify
  // proof paths and do not compete as complete expansion replays here.
  if (patch.materialized.bTokStart != static_cast<uint64_t>(wholeB->first) ||
      patch.materialized.bTokEnd != static_cast<uint64_t>(wholeB->second))
    return true;

  using ReplayElem = WholeEnvelopeReplayElem;

  SmallVector<TokenInterval, 32> argumentDependentIntervals;
  auto addArgumentDependentInterval = [&](uint64_t begin, uint64_t end) {
    if (begin >= end)
      return;
    argumentDependentIntervals.push_back({begin, end});
  };

  // Standard substitutions, stringification results, and pasted tokens
  // are all argument-dependent output surfaces: changing the invocation
  // actual can legitimately change those tokens.  Everything else in the
  // macro's replacement-list cover is fixed body surface and must replay
  // literally in B before a structure-preserving candidate may claim the
  // whole expansion envelope.
  for (const auto &as : m.argSpans) {
    if (as.kind == PPArgSpanKind::Standard)
      addArgumentDependentInterval(as.begin, as.end);
  }
  for (const auto &span : m.stringifySpans)
    addArgumentDependentInterval(span.begin, span.end);
  for (const auto &span : m.pasteSpans)
    addArgumentDependentInterval(span.begin, span.end);

  // A root producer body span may contain exact output surfaces emitted by
  // nested macro invocations.  The subtree replay validator owns those
  // surfaces, so exclude them from this local literal-body audit while
  // retaining exact A/B envelope tiling below.  Never exclude the descendant
  // PPCover: that interval is only a convex hull and can contain intervening
  // parent-owned body tokens.  Membership is established solely from recorded
  // caller ancestry, so malformed or unrelated producer nodes remain
  // fail-closed and continue to be checked as fixed root body.
  RefoldMacroOccurrenceProofValidator occurrenceProofValidator(
      RefoldMacroOccurrenceProofValidator::Dependencies{&deps_.model,
                                                        &deps_.macroTopology});
  for (const auto &candidate : deps_.model.GetMacroInvocations()) {
    if (candidate.id == m.id || !candidate.cover.IsValid() ||
        candidate.cover.begin >= candidate.cover.end ||
        !occurrenceProofValidator.CandidateBelongsToValidatedSubtree(
            ctx, candidate))
      continue;

    appendExactDescendantReplaySurfaces(argumentDependentIntervals, candidate,
                                        *cover);
  }

  llvm::sort(argumentDependentIntervals, tokenIntervalLess);
  SmallVector<TokenInterval, 32> mergedArgumentIntervals;
  for (const TokenInterval &raw : argumentDependentIntervals) {
    if (raw.begin < cover->first || raw.end > cover->second ||
        raw.end < raw.begin) {
      REFOLD_LOG_TRACE(
          "macro/proof",
          "suppress structure-preserving macro replay: inv id={0} "
          "name={1} argument-dependent surface escapes whole cover: "
          "surface=[{2},{3}) cover=[{4},{5}) text='{6}'",
          m.id, m.name, raw.begin, raw.end, cover->first, cover->second,
          stringutils::showWsWithClip(patch.replacement, 220));
      return false;
    }

    if (!mergedArgumentIntervals.empty() &&
        raw.begin <= mergedArgumentIntervals.back().end) {
      mergedArgumentIntervals.back().end =
          std::max(mergedArgumentIntervals.back().end, raw.end);
      continue;
    }
    mergedArgumentIntervals.push_back(raw);
  }

  SmallVector<TokenInterval, 32> fixedIntervals;
  for (const auto &bs : m.bodySpans) {
    if (!appendFixedPiecesOutside(fixedIntervals, {bs.begin, bs.end}, *cover,
                                  mergedArgumentIntervals)) {
      REFOLD_LOG_TRACE("macro/proof",
                       "suppress structure-preserving macro replay: inv id={0} "
                       "name={1} body span escapes whole cover: body=[{2},{3}) "
                       "cover=[{4},{5}) text='{6}'",
                       m.id, m.name, bs.begin, bs.end, cover->first,
                       cover->second,
                       stringutils::showWsWithClip(patch.replacement, 220));
      return false;
    }
  }

  SmallVector<ReplayElem, 64> elems;
  for (const TokenInterval &fixed : fixedIntervals)
    elems.push_back({false, fixed.begin, fixed.end});
  for (const TokenInterval &arg : mergedArgumentIntervals)
    elems.push_back({true, arg.begin, arg.end});

  if (elems.empty()) {
    REFOLD_LOG_TRACE("macro/proof",
                     "suppress structure-preserving macro replay: inv id={0} "
                     "name={1} whole-envelope replay has no body or argument "
                     "surface to discharge: cover=[{2},{3}) text='{4}'",
                     m.id, m.name, cover->first, cover->second,
                     stringutils::showWsWithClip(patch.replacement, 220));
    return false;
  }

  llvm::sort(elems, [](const ReplayElem &lhs, const ReplayElem &rhs) {
    if (lhs.aBegin != rhs.aBegin)
      return lhs.aBegin < rhs.aBegin;
    if (lhs.aEnd != rhs.aEnd)
      return lhs.aEnd < rhs.aEnd;
    return lhs.isArgumentDependent < rhs.isArgumentDependent;
  });

  const std::pair<size_t, size_t> replayB =
      chooseArgumentAwareReplayBEnvelope(*wholeB, trimmedReplayB, elems);

  auto mapArgumentDependentSurfaceToReplayBEnvelope =
      [&](const ReplayElem &elem) -> std::optional<std::pair<size_t, size_t>> {
    std::optional<std::pair<size_t, size_t>> mapped =
        deps_.sourceMapper
            .MapATokRangeAToBTokenEnvelopePreserveBoundaryInsertions(
                elem.aBegin, elem.aEnd);
    if (!mapped)
      return std::nullopt;
    if (mapped->first > mapped->second || mapped->second > replayB.second)
      return std::nullopt;
    return mapped;
  };

  uint64_t aCursor = cover->first;
  size_t bCursor = replayB.first;
  for (const ReplayElem &elem : elems) {
    if (elem.aBegin != aCursor || elem.aEnd < elem.aBegin ||
        elem.aEnd > cover->second) {
      REFOLD_LOG_TRACE(
          "macro/proof",
          "suppress structure-preserving macro replay: inv id={0} "
          "name={1} whole-envelope replay is not an exact A tiling: "
          "elem=[{2},{3}) cursor={4} cover=[{5},{6}) text='{7}'",
          m.id, m.name, elem.aBegin, elem.aEnd, aCursor, cover->first,
          cover->second, stringutils::showWsWithClip(patch.replacement, 220));
      return false;
    }
    aCursor = elem.aEnd;

    if (elem.isArgumentDependent) {
      std::optional<std::pair<size_t, size_t>> argB =
          mapArgumentDependentSurfaceToReplayBEnvelope(elem);
      if (!argB) {
        REFOLD_LOG_TRACE(
            "macro/proof",
            "suppress structure-preserving macro replay: inv id={0} "
            "name={1} argument-dependent surface has no B replay "
            "envelope: A=[{2},{3}) replayB=[{4},{5}) wholeB=[{6},{7}) "
            "text='{8}'",
            m.id, m.name, elem.aBegin, elem.aEnd, replayB.first, replayB.second,
            wholeB->first, wholeB->second,
            stringutils::showWsWithClip(patch.replacement, 220));
        return false;
      }
      if (argB->first != bCursor || argB->second < argB->first ||
          argB->second > replayB.second) {
        REFOLD_LOG_TRACE(
            "macro/proof",
            "suppress structure-preserving macro replay: inv id={0} "
            "name={1} argument-dependent surface does not align with "
            "the B replay cursor: A=[{2},{3}) B=[{4},{5}) "
            "cursor={6} replayB=[{7},{8}) wholeB=[{9},{10}) text='{11}'",
            m.id, m.name, elem.aBegin, elem.aEnd, argB->first, argB->second,
            bCursor, replayB.first, replayB.second, wholeB->first,
            wholeB->second,
            stringutils::showWsWithClip(patch.replacement, 220));
        return false;
      }
      bCursor = argB->second;
      continue;
    }

    const size_t len = static_cast<size_t>(elem.aEnd - elem.aBegin);
    if (bCursor + len > replayB.second) {
      REFOLD_LOG_TRACE("macro/proof",
                       "suppress structure-preserving macro replay: inv id={0} "
                       "name={1} fixed body span overruns replay B envelope: "
                       "A=[{2},{3}) cursor={4} len={5} replayB=[{6},{7}) "
                       "wholeB=[{8},{9}) text='{10}'",
                       m.id, m.name, elem.aBegin, elem.aEnd, bCursor, len,
                       replayB.first, replayB.second, wholeB->first,
                       wholeB->second,
                       stringutils::showWsWithClip(patch.replacement, 220));
      return false;
    }

    bool fixedMatches = true;
    for (size_t i = 0; i < len; ++i) {
      if (deps_.aToks[static_cast<size_t>(elem.aBegin) + i].spelling !=
          deps_.bToks[bCursor + i].spelling) {
        fixedMatches = false;
        break;
      }
    }
    if (!fixedMatches) {
      REFOLD_LOG_TRACE(
          "macro/proof",
          "suppress structure-preserving macro replay: inv id={0} "
          "name={1} fixed replacement-list body changed inside a "
          "claimed whole B envelope: A=[{2},{3}) B=[{4},{5}) "
          "Atext='{6}' Btext='{7}' replacement='{8}'",
          m.id, m.name, elem.aBegin, elem.aEnd, bCursor, bCursor + len,
          stringutils::showWsWithClip(
              deps_.sourceMapper.SliceASource(elem.aBegin, elem.aEnd), 120),
          stringutils::showWsWithClip(
              deps_.sourceMapper.SliceBSource(bCursor, bCursor + len), 120),
          stringutils::showWsWithClip(patch.replacement, 220));
      return false;
    }
    bCursor += len;
  }

  if (aCursor != cover->second || bCursor != replayB.second) {
    REFOLD_LOG_TRACE(
        "macro/proof",
        "suppress structure-preserving macro replay: inv id={0} "
        "name={1} whole-envelope replay did not consume exact cover: "
        "Acur={2} Aend={3} Bcur={4} Bend={5} wholeB=[{6},{7}) "
        "text='{8}'",
        m.id, m.name, aCursor, cover->second, bCursor, replayB.second,
        wholeB->first, wholeB->second,
        stringutils::showWsWithClip(patch.replacement, 220));
    return false;
  }

  // The walk places every argument surface between fixed text that replays,
  // but never looks inside one, so it admitted rewrites whose actuals do not
  // expand to the B text there -- `M(+ pz - q)` for `2 * 2 * p + pz - q`
  // under `#define M(x) 2 * x`.
  if (!rewrittenActualsReplayInB(deps_, m, ctx.rootInvocationText,
                                 patch.replacement, *cover, elems,
                                 argumentDependentIntervals)) {
    REFOLD_LOG_TRACE(
        "macro/proof",
        "suppress structure-preserving macro replay: inv id={0} name={1} "
        "rewritten actuals do not replay the cover's token-diff image "
        "cover=[{2},{3}) text='{4}'",
        m.id, m.name, cover->first, cover->second,
        stringutils::showWsWithClip(patch.replacement, 220));
    return false;
  }

  return true;
}

bool RefoldMacroReplayStabilityValidator::
    RootPreservingCandidateHasLiteralFixedRootBodyReplay(
        const RefoldModel::MacroInvocation &m, StringRef baseInvText,
        const MacroPatch &patch) const {
  if (!patch.proof.preservesInvocationStructure ||
      patch.proof.proofRootMacroId != m.id)
    return true;
  if (patch.proof.kind == MacroPatchProofKind::WholeCoverRealization)
    return true;

  // Most whole-envelope witnesses are selector/template summaries: they
  // can prove an argument or generated-callee surface without proving
  // every fixed root-body token that a preserved invocation will
  // regenerate.  The definition-tape replay solver is stronger: it
  // replays the recorded replacement list itself against B, including
  // empty variadic slots and __VA_OPT__ branch choices.  Do not
  // second-guess that complete transducer proof with per-token A->B
  // mapping, because variadic erasure can legitimately leave stable
  // punctuation without a one-token diff envelope.
  if (patch.proof.wholeEnvelopeReplay &&
      patch.proof.wholeEnvelopeReplay->definitionTapeReplayValidated &&
      patch.proof.wholeEnvelopeReplay->rootMacroId ==
          patch.proof.proofRootMacroId)
    return true;

  const std::optional<std::pair<uint64_t, uint64_t>> cover =
      RefoldMacroWholeCoverProof::GetWholeCoverATokRange(m);
  if (!cover || cover->first >= cover->second)
    return true;

  SmallVector<TokenInterval, 32> excludedA;
  for (const auto &span : m.argSpans) {
    if (span.kind == PPArgSpanKind::Standard)
      addNonEmptyTokenInterval(excludedA, span.begin, span.end);
  }
  for (const auto &span : m.stringifySpans)
    addNonEmptyTokenInterval(excludedA, span.begin, span.end);
  for (const auto &span : m.pasteSpans)
    addNonEmptyTokenInterval(excludedA, span.begin, span.end);

  // Producer body spans on a root invocation can include exact surfaces
  // emitted by nested/generated macro calls.  Those surfaces are not fixed
  // root body: they are discharged by the descendant proof that the root
  // candidate rewrites or by whole-cover realization.  Exclude only those
  // exact producer surfaces before auditing the remaining root-owned body.
  // A descendant PPCover is merely a convex hull and must not hide
  // intervening parent-owned fixed-body tokens.
  DenseMap<uint64_t, const RefoldModel::MacroInvocation *> invById;
  for (const auto &candidate : deps_.model.GetMacroInvocations())
    invById[candidate.id] = &candidate;
  SmallVector<std::pair<size_t, size_t>, 1> rootBodyReplayArgRanges;
  const MacroSubtreeReplayValidationContext rootBodyReplaySubtreeCtx{
      m, baseInvText, rootBodyReplayArgRanges, invById};

  RefoldMacroOccurrenceProofValidator occurrenceProofValidator(
      RefoldMacroOccurrenceProofValidator::Dependencies{&deps_.model,
                                                        &deps_.macroTopology});
  for (const auto &candidate : deps_.model.GetMacroInvocations()) {
    if (candidate.id == m.id || !candidate.cover.IsValid() ||
        candidate.cover.begin >= candidate.cover.end ||
        !occurrenceProofValidator.CandidateBelongsToValidatedSubtree(
            rootBodyReplaySubtreeCtx, candidate))
      continue;
    appendExactDescendantReplaySurfaces(excludedA, candidate, *cover);
  }

  normalizeIntervals(excludedA);

  SmallVector<TokenInterval, 32> fixedBodyA;
  for (const auto &span : m.bodySpans) {
    const uint64_t begin = std::max<uint64_t>(span.begin, cover->first);
    const uint64_t end = std::min<uint64_t>(span.end, cover->second);
    if (!appendFixedPiecesOutside(fixedBodyA, {begin, end}, *cover,
                                  excludedA)) {
      REFOLD_LOG_TRACE("macro/proof",
                       "suppress structure-preserving macro replay: inv id={0} "
                       "name={1} fixed root body span escapes whole cover: "
                       "body=[{2},{3}) cover=[{4},{5}) replacement='{6}'",
                       m.id, m.name, span.begin, span.end, cover->first,
                       cover->second,
                       stringutils::showWsWithClip(patch.replacement, 220));
      return false;
    }
  }

  for (const TokenInterval &fixed : fixedBodyA) {
    for (uint64_t aTok = fixed.begin; aTok < fixed.end; ++aTok) {
      if (static_cast<size_t>(aTok) >= deps_.aToks.size())
        return false;

      std::optional<std::pair<size_t, size_t>> bTok =
          deps_.sourceMapper.MapATokRangeAToBTokenEnvelope(aTok, aTok + 1);
      if (!bTok || bTok->first >= bTok->second) {
        REFOLD_LOG_TRACE(
            "macro/proof",
            "suppress structure-preserving macro replay: inv id={0} "
            "name={1} fixed root body token has no B replay "
            "envelope: A=[{2},{3}) Atext='{4}' replacement='{5}'",
            m.id, m.name, aTok, aTok + 1,
            stringutils::showWsWithClip(
                deps_.sourceMapper.SliceASource(aTok, aTok + 1), 120),
            stringutils::showWsWithClip(patch.replacement, 220));
        return false;
      }

      if (bTok->second != bTok->first + 1 ||
          bTok->first >= deps_.bToks.size() ||
          deps_.aToks[static_cast<size_t>(aTok)].spelling !=
              deps_.bToks[bTok->first].spelling) {
        REFOLD_LOG_TRACE(
            "macro/proof",
            "suppress structure-preserving macro replay: inv id={0} "
            "name={1} fixed root body changed while preserving the "
            "root invocation: A=[{2},{3}) B=[{4},{5}) Atext='{6}' "
            "Btext='{7}' replacement='{8}'",
            m.id, m.name, aTok, aTok + 1, bTok->first, bTok->second,
            stringutils::showWsWithClip(
                deps_.sourceMapper.SliceASource(aTok, aTok + 1), 120),
            stringutils::showWsWithClip(
                deps_.sourceMapper.SliceBSource(bTok->first, bTok->second),
                120),
            stringutils::showWsWithClip(patch.replacement, 220));
        return false;
      }
    }
  }

  return true;
}

bool RefoldMacroReplayStabilityValidator::
    CallsiteReplayObservesActiveHeaderMacroState(
        const RefoldModel::MacroInvocation &m, const MacroPatch &patch) const {
  // Only structure-preserving callsite replay can be unstable in this
  // way.  Expanded/whole-cover candidates do not replay the callsite as a
  // macro invocation, and TU-spelled invocations are handled by the TU
  // macro-state repair paths rather than include-owner replay.
  if (!patch.proof.preservesInvocationStructure ||
      patch.proof.proofRootMacroId != m.id)
    return false;
  if (!invocationSpanMatchesCallsitePrefix(patch.replacement, m))
    return false;
  if (!m.ownerIncludeId || !m.invFile)
    return false;
  if (deps_.pathIdentity.PathsEqual(*m.invFile, deps_.model.GetSourcePath()))
    return false;

  // Decide whether this structure-preserving candidate would observe a
  // specific active header directive if replayed at the current callsite.
  // Use the shared macro-state observation proof so this header replay
  // gate remains aligned with TU carry, include materialization, and
  // unresolved-expansion fallback.

  // Exact source interval for a macro-state directive in the header
  // containing this invocation.  The interval is used only for ordering
  // and active-state reconstruction in this admission check; movement is
  // performed later by the materialization path.
  using HeaderDirectivePiece = MacroStateDirectiveLineInterval;

  // Lazily read the header bytes so directive intervals can be validated
  // against the real file.  If the file cannot be read, this proof does
  // not guess; it simply declines to suppress the candidate here and lets
  // downstream validation/fallback handle the uncertainty.
  std::optional<std::string> headerBytesStorage;
  auto getHeaderBytes = [&]() -> std::optional<StringRef> {
    if (headerBytesStorage)
      return StringRef(*headerBytesStorage);
    auto bufOrErr =
        MemoryBuffer::getFile(deps_.lineDirs.ToAbsolutePath(m.invFile->str()));
    if (!bufOrErr)
      return std::nullopt;
    const MemoryBuffer &mb = **bufOrErr;
    headerBytesStorage.emplace(mb.getBufferStart(), mb.getBufferEnd());
    return StringRef(*headerBytesStorage);
  };

  // Reconstruct the full directive interval and verify that it belongs to
  // the same include owner and header file as the callsite. The shared
  // helper owns macro-name-anchor reconstruction and exact-text
  // validation against the lazily-read header bytes.
  auto directiveInterval = [&](const RefoldModel::MacroDirective &directive)
      -> std::optional<HeaderDirectivePiece> {
    std::optional<StringRef> headerBytes = getHeaderBytes();
    if (!headerBytes)
      return std::nullopt;
    return recoverMacroStateDirectiveLineInterval(deps_.pathIdentity, directive,
                                                  *m.invFile, *headerBytes,
                                                  m.ownerIncludeId);
  };

  // If any active header-owned definition would be observed by the
  // replacement, this structure-preserving candidate is inadmissible.  It
  // is not enough that the rewritten text is token-equivalent somewhere;
  // it must be token-equivalent under the macro state at its final replay
  // position.
  for (const auto &directive : deps_.model.GetMacroDirectives()) {
    std::optional<HeaderDirectivePiece> piece = directiveInterval(directive);
    if (!piece || piece->end > patch.invRange.begin)
      continue;
    // A recovered interval means the header bytes were read, so the shared
    // liveness proof can re-read them from the memoized storage.  A later
    // `#undef` or `#define` for the same name cancels this definition.
    if (!deps_.macroStateProof.DefinitionIsLiveAtOwnerByte(
            directive, piece->name, *m.invFile, *getHeaderBytes(),
            m.ownerIncludeId, patch.invRange.begin))
      continue;
    if (deps_.macroStateProof
            .FirstMacroStateObservationOffsetInText(directive, piece->name,
                                                    patch.replacement)
            .has_value()) {
      REFOLD_LOG_TRACE(
          "macro/proof",
          "suppress structure-preserving macro replay: inv id={0} "
          "name={1} replacement observes active header macro-state "
          "directive #{2} '{3}' before callsite",
          m.id, m.name, directive.id, piece->name);
      return true;
    }
  }
  return false;
}

bool RefoldMacroReplayStabilityValidator::
    MacroCandidateReplayIsStableForFinalSelection(
        const MacroSubtreeReplayValidationContext &ctx,
        const MacroPatch &patch) const {
  const RefoldModel::MacroInvocation &m = ctx.rootInvocation;
  REFOLD_LOG_TRACE(
      "macro/replay-stability",
      "enter inv id={0} name={1} proof={2} preserves={3} root={4} replacement='{5}'",
      m.id, m.name, toString(patch.proof.kind),
      patch.proof.preservesInvocationStructure ? 1 : 0,
      patch.proof.proofRootMacroId,
      stringutils::showWsWithClip(patch.replacement, 220));

  if (!StructurePreservingCallsiteHasStableFormalSyntax(patch, m)) {
    REFOLD_LOG_TRACE("macro/replay-stability",
                     "reject inv id={0} name={1} proof={2} reason=formal-syntax-unstable",
                     m.id, m.name, toString(patch.proof.kind));
    return false;
  }
  if (!ArgsOnlyWholeEnvelopeCandidateHasLiteralBodyReplay(ctx, patch)) {
    REFOLD_LOG_TRACE("macro/replay-stability",
                     "reject inv id={0} name={1} proof={2} reason=args-only-whole-envelope-literal-body-failed",
                     m.id, m.name, toString(patch.proof.kind));
    return false;
  }
  if (!RootPreservingCandidateHasLiteralFixedRootBodyReplay(
          m, ctx.rootInvocationText, patch)) {
    REFOLD_LOG_TRACE("macro/replay-stability",
                     "reject inv id={0} name={1} proof={2} reason=fixed-root-body-replay-failed",
                     m.id, m.name, toString(patch.proof.kind));
    return false;
  }
  if (!deps_.subtreeReplayValidator.ClaimedWholeEnvelopeIsReplaySafe(ctx,
                                                                     patch)) {
    REFOLD_LOG_TRACE("macro/replay-stability",
                     "reject inv id={0} name={1} proof={2} reason=subtree-whole-envelope-replay-unsafe",
                     m.id, m.name, toString(patch.proof.kind));
    return false;
  }
  if (CallsiteReplayObservesActiveHeaderMacroState(m, patch)) {
    REFOLD_LOG_TRACE("macro/replay-stability",
                     "reject inv id={0} name={1} proof={2} reason=active-header-macro-state-observed",
                     m.id, m.name, toString(patch.proof.kind));
    return false;
  }

  REFOLD_LOG_TRACE("macro/replay-stability",
                   "accept inv id={0} name={1} proof={2}", m.id, m.name,
                   toString(patch.proof.kind));
  return true;
}

} // namespace refold
} // namespace clang
