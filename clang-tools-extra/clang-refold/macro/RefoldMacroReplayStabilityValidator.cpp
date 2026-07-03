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

#include "core/RefoldLog.h"
#include "core/RefoldModel.h"
#include "line-control/LineDirectiveInserter.h"
#include "line-control/RefoldLineObserverLayout.h"
#include "macro/RefoldArgTextRecovery.h"
#include "macro/RefoldMacroPlannerHelpers.h"
#include "macro/RefoldMacroReplay.h"
#include "macro/RefoldMacroStateProof.h"
#include "macro/RefoldMacroSubtreeReplayValidator.h"
#include "macro/RefoldMacroTopology.h"
#include "source/RefoldSourceMapper.h"
#include "util/RefoldPathIdentity.h"
#include "util/StringUtils.h"

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

/// Split `body` into the sub-intervals that lie outside `argumentIntervals`,
/// appending them to `fixed`.  Returns false when `body` escapes `cover`.
/// This is the shared literal-body replay slicer used by args-only envelope
/// validation after argument-owned intervals have been excluded.
bool appendFixedPiecesOutsideArguments(
    SmallVectorImpl<TokenInterval> &fixed, TokenInterval body,
    std::pair<uint64_t, uint64_t> cover,
    ArrayRef<TokenInterval> argumentIntervals) {
  if (body.begin >= body.end)
    return true;
  if (body.begin < cover.first || body.end > cover.second ||
      body.end < body.begin)
    return false;

  uint64_t cursor = body.begin;
  for (const TokenInterval &arg : argumentIntervals) {
    if (arg.end <= cursor)
      continue;
    if (arg.begin >= body.end)
      break;
    if (arg.begin > cursor)
      fixed.push_back({cursor, std::min<uint64_t>(arg.begin, body.end)});
    cursor = std::max(cursor, std::min<uint64_t>(arg.end, body.end));
  }
  if (cursor < body.end)
    fixed.push_back({cursor, body.end});
  return true;
}

/// Split `body` into the sub-intervals that lie outside `excludedA`,
/// appending them to `fixed`.  Returns false when `body` escapes `cover`.
/// This is the shared root-body replay slicer used after known macro-owned or
/// argument-owned surfaces have been excluded from the fixed-body check.
bool appendFixedPiecesOutsideExcludedSurfaces(
    SmallVectorImpl<TokenInterval> &fixed, TokenInterval body,
    std::pair<uint64_t, uint64_t> cover, ArrayRef<TokenInterval> excludedA) {
  if (body.begin >= body.end)
    return true;
  if (body.begin < cover.first || body.end > cover.second ||
      body.end < body.begin)
    return false;

  uint64_t cursor = body.begin;
  for (const TokenInterval &excluded : excludedA) {
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
  if (!RefoldLineObserverLayout::InvocationSpanMatchesCallsitePrefix(
          patch.replacement, m)) {
    REFOLD_LOG_TRACE("macro/proof",
                     "suppress structure-preserving macro replay: inv id={0} "
                     "name={1} replacement no longer has a matching callsite "
                     "prefix",
                     m.id, m.name);
    return false;
  }

  auto parsedActuals =
      RefoldArgTextRecovery::LexMacroInvocationActualContentRanges(
          patch.replacement, deps_.lexLang);
  if (!parsedActuals) {
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
        const RefoldModel::MacroInvocation &m, const MacroPatch &patch) const {
  auto isArgsOnlyInvocationPreservingProof = [&]() {
    switch (patch.proof.kind) {
    case MacroPatchProofKind::ArgsOnlyStandard:
    case MacroPatchProofKind::ArgsOnlyPasteSingle:
    case MacroPatchProofKind::ArgsOnlyPasteMulti:
    case MacroPatchProofKind::ArgsOnlyPurePasteOnly:
    case MacroPatchProofKind::ArgsOnlyPairedPureInsertion:
      return true;
    case MacroPatchProofKind::PasteDerivedCalleeSelector:
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
  // macro expansion.  Those boundary insertions are not part of the
  // replacement-list replay obligation: they are emitted by the ordinary
  // boundary edit path next to the preserved invocation.  Use the
  // boundary-trimmed envelope for the literal body/formal/operator replay
  // audit, while still requiring the candidate to have claimed the wider
  // whole-cover envelope before this guard applies.
  std::pair<size_t, size_t> replayB = *wholeB;
  if (std::optional<std::pair<size_t, size_t>> trimmedB =
          deps_.sourceMapper.MapATokRangeAToBTokenEnvelope(cover->first,
                                                           cover->second)) {
    if (wholeB->first <= trimmedB->first &&
        trimmedB->first <= trimmedB->second &&
        trimmedB->second <= wholeB->second)
      replayB = *trimmedB;
  }

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
    if (!appendFixedPiecesOutsideArguments(fixedIntervals, {bs.begin, bs.end},
                                           *cover, mergedArgumentIntervals)) {
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

  // Producer body spans on a root invocation can include tokens emitted
  // by nested/generated macro calls.  Those tokens are not fixed root
  // body: they are discharged by the descendant proof that the root
  // candidate rewrites or by whole-cover realization.  Exclude every
  // descendant cover before asking whether the remaining root-owned body
  // text is still literal.
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
    const uint64_t begin =
        std::max<uint64_t>(candidate.cover.begin, cover->first);
    const uint64_t end = std::min<uint64_t>(candidate.cover.end, cover->second);
    addNonEmptyTokenInterval(excludedA, begin, end);
  }

  normalizeIntervals(excludedA);

  SmallVector<TokenInterval, 32> fixedBodyA;
  for (const auto &span : m.bodySpans) {
    const uint64_t begin = std::max<uint64_t>(span.begin, cover->first);
    const uint64_t end = std::min<uint64_t>(span.end, cover->second);
    if (!appendFixedPiecesOutsideExcludedSurfaces(fixedBodyA, {begin, end},
                                                  *cover, excludedA)) {
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
  if (!RefoldLineObserverLayout::InvocationSpanMatchesCallsitePrefix(
          patch.replacement, m))
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
    return deps_.macroStateProof.RecoverMacroStateDirectiveLineInterval(
        directive, *m.invFile, *headerBytes, m.ownerIncludeId);
  };

  // Determine whether `definition` is the active macro definition for
  // `macroName` immediately before the preserved callsite.  A later #undef
  // or #define for the same name cancels this definition for replay
  // stability purposes.
  auto activeDefinitionAtPatch =
      [&](const RefoldModel::MacroDirective &definition, StringRef macroName) {
        const RefoldModel::MacroDirective *active = nullptr;
        uint64_t activeEnd = 0;
        for (const auto &candidate : deps_.model.GetMacroDirectives()) {
          std::optional<HeaderDirectivePiece> piece =
              directiveInterval(candidate);
          if (!piece || piece->end > patch.invRange.begin)
            continue;
          if (StringRef(piece->name) != macroName)
            continue;
          if (!active || piece->end > activeEnd ||
              (piece->end == activeEnd && candidate.id > active->id)) {
            active = &candidate;
            activeEnd = piece->end;
          }
        }
        return active == &definition && definition.subkind == "#define";
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
    if (!activeDefinitionAtPatch(directive, piece->name))
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
  return StructurePreservingCallsiteHasStableFormalSyntax(patch, m) &&
         ArgsOnlyWholeEnvelopeCandidateHasLiteralBodyReplay(m, patch) &&
         RootPreservingCandidateHasLiteralFixedRootBodyReplay(
             m, ctx.rootInvocationText, patch) &&
         deps_.subtreeReplayValidator.ClaimedWholeEnvelopeIsReplaySafe(ctx,
                                                                       patch) &&
         !CallsiteReplayObservesActiveHeaderMacroState(m, patch);
}

} // namespace refold
} // namespace clang
