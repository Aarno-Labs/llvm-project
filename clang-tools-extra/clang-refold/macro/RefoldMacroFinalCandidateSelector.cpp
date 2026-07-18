//===--- RefoldMacroFinalCandidateSelector.cpp ------------------*- C++ -*-===//
//
// Final macro candidate selector service implementation.
//
//===----------------------------------------------------------------------===//

#include "macro/RefoldMacroFinalCandidateSelector.h"

#include "core/RefoldLog.h"
#include "core/RefoldModel.h"
#include "line-control/RefoldLineObserverLayout.h"
#include "macro/RefoldMacroDAGSharedHelpers.h"
#include "macro/RefoldMacroPatchProofCertifier.h"
#include "macro/RefoldMacroPatchReusePhase.h"
#include "macro/RefoldMacroPlannerHelpers.h"
#include "macro/RefoldMacroReplayStabilityValidator.h"
#include "macro/RefoldMacroTopology.h"
#include "macro/RefoldMacroWholeCoverPlanningContext.h"
#include "proof/RefoldAcceptedResultRanker.h"
#include "proof/RefoldProofLattice.h"
#include "util/StringUtils.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>

using namespace llvm;

namespace clang {
namespace refold {

namespace {

/// Origin tag carried beside each final macro candidate so post-selection
/// certification can distinguish replay, reuse, and realization paths.
enum class FinalMacroCandidateOrigin : uint8_t {
  DirectArgsOnly,
  DagRootReplay,
  ReuseExistingCallsiteNoOp,
  ReuseExistingCallsiteSkipWholeCover,
  ReuseExistingExpanded,
  WholeCoverRealization,
};

/// Stable trace spelling for final candidate origins.
StringRef finalMacroCandidateOriginName(FinalMacroCandidateOrigin origin) {
  switch (origin) {
  case FinalMacroCandidateOrigin::DirectArgsOnly:
    return "DirectArgsOnly";
  case FinalMacroCandidateOrigin::DagRootReplay:
    return "DagRootReplay";
  case FinalMacroCandidateOrigin::ReuseExistingCallsiteNoOp:
    return "ReuseExistingCallsiteNoOp";
  case FinalMacroCandidateOrigin::ReuseExistingCallsiteSkipWholeCover:
    return "ReuseExistingCallsiteSkipWholeCover";
  case FinalMacroCandidateOrigin::ReuseExistingExpanded:
    return "ReuseExistingExpanded";
  case FinalMacroCandidateOrigin::WholeCoverRealization:
    return "WholeCoverRealization";
  }
  llvm_unreachable("invalid final macro candidate origin");
}


/// Pair the concrete macro patch with its theorem-lattice selection
/// candidate and the origin path that produced it.
struct FinalMacroCandidate {
  MacroPatch patch;
  MacroSelectionCandidate selectionCandidate;
  FinalMacroCandidateOrigin origin =
      FinalMacroCandidateOrigin::WholeCoverRealization;
};

/// Caller-owned admission state for the final macro selector.  The
/// candidate vector is owned by `Run`; the replay-stability context is
/// borrowed from the planning context.
struct FinalMacroCandidateAdmissionContext {
  const RefoldModel::MacroInvocation &invocation;
  const diffutils::Hunk &hunk;
  StringRef baseInvocationText;
  SmallVectorImpl<FinalMacroCandidate> &candidates;
  const MacroSubtreeReplayValidationContext *replayStabilityCtx = nullptr;
  bool allowNonTopLevelMacroSelectorFailure = false;

  bool hasDirectArgsOnlyCandidate = false;
  bool hasDagRootReplayCandidate = false;
  bool hasExistingCallsiteCandidate = false;
  bool hasExistingExpandedCandidate = false;
  bool hasWholeCoverCandidate = false;
};

/// Whole-cover realization candidate before final theorem-lattice
/// admission.  The plan was already computed by the whole-cover proof
/// path; this carrier only names the source span and plan together.
struct WholeCoverCandidate {
  const RefoldModel::MacroInvocation &invocation;
  uint64_t invocationStart = 0;
  uint64_t invocationEnd = 0;
  const WholeCoverPlan &plan;
};

/// Note which final-admission path contributed a candidate.  These flags
/// are bookkeeping only; proof-lattice ranking remains authoritative.
void noteFinalMacroCandidateOrigin(FinalMacroCandidateAdmissionContext &ctx,
                                   FinalMacroCandidateOrigin origin) {
  switch (origin) {
  case FinalMacroCandidateOrigin::DirectArgsOnly:
    ctx.hasDirectArgsOnlyCandidate = true;
    break;
  case FinalMacroCandidateOrigin::DagRootReplay:
    ctx.hasDagRootReplayCandidate = true;
    break;
  case FinalMacroCandidateOrigin::ReuseExistingCallsiteNoOp:
  case FinalMacroCandidateOrigin::ReuseExistingCallsiteSkipWholeCover:
    ctx.hasExistingCallsiteCandidate = true;
    break;
  case FinalMacroCandidateOrigin::ReuseExistingExpanded:
    ctx.hasExistingExpandedCandidate = true;
    break;
  case FinalMacroCandidateOrigin::WholeCoverRealization:
    ctx.hasWholeCoverCandidate = true;
    break;
  }
}

/// Gate one already-discovered final candidate at the replay-stability
/// boundary and admit it to the caller-owned candidate vector when the
/// gate passes.  Structure-preserving replay candidates can be produced by
/// several proof paths, but they all share this same gate so unstable
/// callsite replay cannot suppress realization.
bool addFinalMacroCandidate(
    const RefoldMacroReplayStabilityValidator &replayStabilityValidator,
    RefoldProofLattice &lattice, FinalMacroCandidateAdmissionContext &ctx,
    FinalMacroCandidate candidate) {
  REFOLD_LOG_TRACE(
      "macro/final-candidate",
      "consider inv id={0} name={1} origin={2} proof={3} preserves={4} "
      "root={5} replacement='{6}' BRange={7} B=[{8},{9})",
      ctx.invocation.id, ctx.invocation.name,
      finalMacroCandidateOriginName(candidate.origin),
      toString(candidate.patch.proof.kind),
      candidate.patch.proof.preservesInvocationStructure ? 1 : 0,
      candidate.patch.proof.proofRootMacroId,
      stringutils::showWsWithClip(candidate.patch.replacement, 220),
      candidate.patch.materialized.hasBTokenRange ? 1 : 0,
      candidate.patch.materialized.bTokStart,
      candidate.patch.materialized.bTokEnd);

  if (!ctx.replayStabilityCtx) {
    REFOLD_LOG_TRACE("macro/final-candidate",
                     "reject inv id={0} name={1} origin={2} reason=no-replay-stability-context",
                     ctx.invocation.id, ctx.invocation.name,
                     finalMacroCandidateOriginName(candidate.origin));
    return false;
  }
  if (!replayStabilityValidator.MacroCandidateReplayIsStableForFinalSelection(
          *ctx.replayStabilityCtx, candidate.patch)) {
    REFOLD_LOG_TRACE("macro/final-candidate",
                     "reject inv id={0} name={1} origin={2} reason=replay-stability-failed proof={3} replacement='{4}'",
                     ctx.invocation.id, ctx.invocation.name,
                     finalMacroCandidateOriginName(candidate.origin),
                     toString(candidate.patch.proof.kind),
                     stringutils::showWsWithClip(candidate.patch.replacement, 220));
    return false;
  }

  candidate.selectionCandidate =
      lattice.AcceptedCandidateBuilder().BuildMacroSelectionCandidate(
          candidate.patch, ctx.allowNonTopLevelMacroSelectorFailure);
  ctx.candidates.push_back(std::move(candidate));
  noteFinalMacroCandidateOrigin(ctx, ctx.candidates.back().origin);
  REFOLD_LOG_TRACE(
      "macro/final-candidate",
      "admit inv id={0} name={1} origin={2} candidateIndex={3} proof={4}",
      ctx.invocation.id, ctx.invocation.name,
      finalMacroCandidateOriginName(ctx.candidates.back().origin),
      ctx.candidates.size() - 1, toString(ctx.candidates.back().patch.proof.kind));
  return true;
}

/// Return whether an already-admitted structure-preserving candidate
/// dominates a realization candidate under the theorem lattice.  The scan
/// is intentionally over the caller-owned candidate vector so this
/// preserves the existing realization-suppression rule exactly.
bool theoremLatticeStructureCandidateDominatesRealization(
    RefoldProofLattice &lattice, const FinalMacroCandidateAdmissionContext &ctx,
    const MacroPatch &realizationPatch) {
  AcceptedResultCandidate realizationCandidate =
      lattice.AcceptedCandidateBuilder().BuildAcceptedMacroCandidate(
          realizationPatch);
  if (!lattice.AcceptedResultRanker().IsSelectableAcceptedResultCandidate(
          realizationCandidate))
    return false;

  for (const FinalMacroCandidate &candidate : ctx.candidates) {
    if (!candidate.patch.proof.preservesInvocationStructure ||
        candidate.patch.proof.proofRootMacroId != ctx.invocation.id)
      continue;
    if (!lattice.AcceptedResultRanker().IsSelectableAcceptedResultCandidate(
            candidate.selectionCandidate.selectorCandidate))
      continue;
    if (lattice.AcceptedResultRanker().LatticePrefers(
            candidate.selectionCandidate.selectorCandidate.proofSummary,
            realizationCandidate.proofSummary) &&
        !lattice.AcceptedResultRanker().LatticePrefers(
            realizationCandidate.proofSummary,
            candidate.selectionCandidate.selectorCandidate.proofSummary))
      return true;
  }
  return false;
}

/// Project the admission context's candidate vector into the dense
/// selector input and run the proof-lattice ranker.  The selected index
/// continues to refer to the caller-owned final-candidate vector.
std::optional<SelectedMacroSelectionCandidate>
selectPreferredFinalMacroCandidate(
    RefoldProofLattice &lattice,
    const FinalMacroCandidateAdmissionContext &ctx) {
  SmallVector<MacroSelectionCandidate, 5> selectionCandidates;
  selectionCandidates.reserve(ctx.candidates.size());
  for (const FinalMacroCandidate &candidate : ctx.candidates)
    selectionCandidates.push_back(candidate.selectionCandidate);
  return lattice.AcceptedResultRanker().SelectPreferredMacroSelectionCandidate(
      selectionCandidates);
}

/// Materialize one whole-cover realization candidate and attach its
/// proof carrier.  The caller supplies the already-proven plan and
/// invocation span; this helper only builds the emitted patch.
MacroPatch
materializeWholeCoverPatch(const RefoldMacroPatchProofCertifier &certifier,
                           const WholeCoverCandidate &candidate) {
  MacroPatch patch{candidate.invocationStart, candidate.invocationEnd,
                   candidate.plan.clippedText, candidate.invocation.id};
  certifier.CertifyWholeCoverAcceptedCandidate(patch, candidate.plan,
                                               candidate.invocation);
  return patch;
}

} // namespace

RefoldMacroFinalCandidateSelector::RefoldMacroFinalCandidateSelector(
    Dependencies deps)
    : deps_(std::move(deps)) {}

std::optional<MacroPatch> RefoldMacroFinalCandidateSelector::Run(
    RefoldMacroWholeCoverPlanningContext &planningCtx) const {
  const RefoldModel::MacroInvocation &m = planningCtx.m;
  const diffutils::Hunk &h = planningCtx.h;
  const diffutils::Hunk &hEff = planningCtx.hEff;
  const StringRef baseInvText = planningCtx.baseInvText;
  const uint64_t invStart = planningCtx.invStart;
  const uint64_t invEnd = planningCtx.invEnd;

  std::optional<MacroPatch> &argsOnlyCandidate = planningCtx.argsOnlyCandidate;
  std::optional<MacroPatch> &dagRootCandidate = planningCtx.dagRootCandidate;
  bool &reuseExistingCallsitePatch = planningCtx.reuseExistingCallsitePatch;
  bool &existingCallsitePatchAbsorbedByDirectCandidate =
      planningCtx.existingCallsitePatchAbsorbedByDirectCandidate;
  const bool conflictingConcreteSubtreeWitnessForcesWholeCover =
      planningCtx.conflictingConcreteSubtreeWitnessForcesWholeCover;

  MacroPatchReuseAdmissionContext &reuseAdmissionCtx =
      planningCtx.reuseAdmissionCtx;
  const MacroPatch *&existingPatch = reuseAdmissionCtx.existingPatch;
  const MacroPatch *&existingExpandedPatch =
      reuseAdmissionCtx.existingExpandedPatch;
  const bool existingIsCallsite = reuseAdmissionCtx.existingIsCallsite;

  // The direct-root inadmissibility and direct-vs-existing-callsite merge
  // blocks apply only to function-like invocations with a literal macro callee
  // origin, which is the same guard used by DAG-chained args-only.  Preserve
  // that gate explicitly so non-function-like invocations remain outside this
  // arbitration path.
  const bool funcLikeWithLiteralCalleeOrigin =
      m.subkind == "func" && hasLiteralMacroCalleeOrigin(m);

  REFOLD_LOG_TRACE(
      "macro/final-candidate",
      "enter inv id={0} name={1} hEffA=[{2},{3}) hEffB=[{4},{5}) "
      "funcLikeLiteral={6} argsOnly={7} dagRoot={8} reuseCallsite={9} "
      "existingPatch={10} existingExpanded={11} existingIsCallsite={12} "
      "baseText='{13}'",
      m.id, m.name, hEff.aStart, hEff.aEnd, hEff.bStart, hEff.bEnd,
      funcLikeWithLiteralCalleeOrigin ? 1 : 0, argsOnlyCandidate ? 1 : 0,
      dagRootCandidate ? 1 : 0, reuseExistingCallsitePatch ? 1 : 0,
      existingPatch ? 1 : 0, existingExpandedPatch ? 1 : 0,
      existingIsCallsite ? 1 : 0,
      stringutils::showWsWithClip(baseInvText, 220));

  if (funcLikeWithLiteralCalleeOrigin &&
      planningCtx.directRootPreservationInadmissible) {
    // `directRootPreservationInadmissible` means that the ordinary direct
    // argument-span proof did not see the edited descendant surface.  That is
    // not enough to discard a stronger direct candidate that was built by a
    // whole-cover owner proof, such as higher-order generated-callee replay:
    // that proof deliberately explains the descendant edit through the root
    // invocation's replacement-list grammar and records the B-token envelope
    // it materializes.  Suppress only candidates that do not discharge the
    // current hunk with such an owner-level witness.
    const bool directCandidateDischargesDescendantHunk =
        argsOnlyCandidate &&
        argsOnlyCandidate->proof.preservesInvocationStructure &&
        argsOnlyCandidate->proof.proofRootMacroId == m.id &&
        argsOnlyCandidate->materialized.hasBTokenRange &&
        argsOnlyCandidate->materialized.bTokStart <= h.bStart &&
        h.bEnd <= argsOnlyCandidate->materialized.bTokEnd;

    if (!directCandidateDischargesDescendantHunk) {
      argsOnlyCandidate.reset();
      reuseExistingCallsitePatch = false;
    }
  }

  // Try to compose a direct root args-only candidate with an existing
  // structure-preserving callsite patch for the same root invocation.
  //
  // This is the direct-candidate counterpart to the DAG/callsite merge path:
  // it allows compatible same-root patches to combine, but rejects or
  // deprioritizes the direct replay when the merged text cannot be validated
  // against the root invocation envelope or when the proof lattice prefers
  // the existing structure-preserving witness. This prevents a direct
  // args-only replay from silently overriding an already accepted same-root
  // callsite witness.
  if (funcLikeWithLiteralCalleeOrigin && argsOnlyCandidate && existingPatch &&
      existingIsCallsite && existingPatch->proof.preservesInvocationStructure &&
      existingPatch->proof.proofRootMacroId == m.id && !baseInvText.empty() &&
      argsOnlyCandidate->invRange.begin == existingPatch->invRange.begin &&
      argsOnlyCandidate->invRange.end == existingPatch->invRange.end &&
      argsOnlyCandidate->replacement != existingPatch->replacement) {

    auto replacementCollapsesParenthesizedTupleFormal =
        [&](const MacroPatch &preservingPatch,
            const MacroPatch &candidatePatch) -> bool {
      auto baseRanges =
          deps_.getMacroInvocationFormalArgContentRanges(m, baseInvText);
      auto preservingRanges = deps_.getMacroInvocationFormalArgContentRanges(
          m, StringRef(preservingPatch.replacement));
      auto candidateRanges = deps_.getMacroInvocationFormalArgContentRanges(
          m, StringRef(candidatePatch.replacement));
      if (!baseRanges || !preservingRanges || !candidateRanges ||
          preservingRanges->size() != baseRanges->size() ||
          candidateRanges->size() != baseRanges->size())
        return false;

      for (size_t i = 0; i < baseRanges->size(); ++i) {
        const StringRef baseArg =
            baseInvText.slice((*baseRanges)[i].first, (*baseRanges)[i].second);
        if (!deps_.isParenthesizedTuple(baseArg))
          continue;

        const StringRef preservingArg =
            StringRef(preservingPatch.replacement)
                .slice((*preservingRanges)[i].first,
                       (*preservingRanges)[i].second);
        const StringRef candidateArg = StringRef(candidatePatch.replacement)
                                           .slice((*candidateRanges)[i].first,
                                                  (*candidateRanges)[i].second);
        if (deps_.isParenthesizedTuple(preservingArg) &&
            !deps_.isParenthesizedTuple(candidateArg))
          return true;
      }
      return false;
    };

    // A complete same-root owner proof is stronger than a later local
    // args-only repair that does not carry its own complete-envelope witness.
    // In multi-terminal tuple cases the first pass may already prove the
    // entire invocation, including sibling paste/stringify effects.  A later
    // ordinary standard-argument hunk can be rebuilt from the original
    // invocation spelling and would otherwise merge over the existing patch,
    // regressing tuple elements outside the local hunk.  Keep the complete
    // existing proof before attempting text merge; the merge helper only
    // reasons about source bytes, not about whether the fresh candidate was
    // derived from stale root spelling.
    const bool existingPatchHasCompleteEnvelope =
        existingPatch->materialized.hasBTokenRange &&
        existingPatch->materialized.bTokStart <= h.bStart &&
        h.bEnd <= existingPatch->materialized.bTokEnd &&
        existingPatch->proof.wholeEnvelopeReplay &&
        existingPatch->proof.wholeEnvelopeReplay->replayValidated;
    const bool directPatchHasCompleteEnvelope =
        argsOnlyCandidate->proof.wholeEnvelopeReplay &&
        argsOnlyCandidate->proof.wholeEnvelopeReplay->replayValidated;
    if (existingPatchHasCompleteEnvelope && !directPatchHasCompleteEnvelope) {
      argsOnlyCandidate.reset();
      reuseExistingCallsitePatch = true;
    } else {

    // If an earlier structure-preserving patch already materializes a B-token
    // envelope that covers this hunk, do not automatically freeze it.  The
    // covered-envelope fact proves that both patches talk about the same B
    // surface, but it does not prove that the older patch has already updated
    // every source-level formal affected by this hunk.  Direct stringify and
    // paste repairs such as `FOO(billy, bob) -> FOO(billy, corgan)` must
    // still be allowed to replace a stale same-root patch.
    //
    // The one case where the older patch really is the stronger owner proof
    // is the tuple/generated-callee collapse we introduced this guard for:
    // the original argument is a parenthesized tuple, the existing patch
    // preserves that tuple, and the later direct args-only candidate replaces
    // the tuple formal with its generated expansion text.  In that situation
    // merging or selecting the direct candidate would lose source structure
    // that has already been proved by the tuple owner.
    if (existingPatch->materialized.hasBTokenRange &&
        existingPatch->materialized.bTokStart <= h.bStart &&
        h.bEnd <= existingPatch->materialized.bTokEnd &&
        replacementCollapsesParenthesizedTupleFormal(*existingPatch,
                                                     *argsOnlyCandidate)) {
      argsOnlyCandidate.reset();
      reuseExistingCallsitePatch = true;
    } else {
      // First try ordinary compatible text merging. Even when the two patches
      // touch the same root span, the merge is accepted only if the resulting
      // replacement still preserves the root invocation envelope.
      SmallVector<StringRef, 2> repls;
      repls.push_back(StringRef(argsOnlyCandidate->replacement));
      repls.push_back(StringRef(existingPatch->replacement));
      auto merged = mergeCompatibleStringReplacements(
          baseInvText, ArrayRef<StringRef>(repls));
      if (merged &&
          deps_.patchReusePhase.ValidateMergedDirectAndDagRootReplacement(
              m, baseInvText, StringRef(*merged))) {
        argsOnlyCandidate->replacement = std::move(*merged);
        argsOnlyCandidate->materialized.hasOutputByteRange = false;
        if (!argsOnlyCandidate->macroId)
          argsOnlyCandidate->macroId = existingPatch->macroId;

        // The merged direct replay is the unique source-level candidate for
        // this callsite after it absorbs the existing same-root patch.
        // Keeping the stale pre-merge patch in the final selector would
        // manufacture a second, non-equivalent witness class for bytes that
        // are already covered by the merged invocation repair.
        reuseExistingCallsitePatch = false;
        existingCallsitePatchAbsorbedByDirectCandidate = true;
      } else {
        // If the older same-root callsite patch carries a complete
        // whole-envelope replay that already covers this B hunk, keep that
        // stronger proof ahead of a fresh local args-only repair.  This is the
        // sibling-terminal tuple case: the first pass can prove the complete
        // tuple rewrite from paste/stringify sibling evidence, while a later
        // ordinary standard-argument hunk can rebuild the same invocation from
        // the original tuple spelling and accidentally revert an earlier tuple
        // element.  Prefer the existing patch only when it has the stronger
        // whole-envelope witness and the fresh candidate does not; two
        // competing complete-envelope proofs still fall through to the normal
        // lattice comparison below.
        const bool existingHasCompleteEnvelope =
            existingPatch->materialized.hasBTokenRange &&
            existingPatch->materialized.bTokStart <= h.bStart &&
            h.bEnd <= existingPatch->materialized.bTokEnd &&
            existingPatch->proof.wholeEnvelopeReplay &&
            existingPatch->proof.wholeEnvelopeReplay->replayValidated;
        const bool directHasCompleteEnvelope =
            argsOnlyCandidate->proof.wholeEnvelopeReplay &&
            argsOnlyCandidate->proof.wholeEnvelopeReplay->replayValidated;
        if (existingHasCompleteEnvelope && !directHasCompleteEnvelope) {
          argsOnlyCandidate.reset();
          reuseExistingCallsitePatch = true;
        } else {
          // If the patches cannot be merged, check whether the direct candidate
          // is independently valid. An invalid direct replay is discarded so
          // the existing callsite patch remains available to the final
          // selector.
          const bool directValid =
              deps_.patchReusePhase.ValidateMergedDirectAndDagRootReplacement(
                  m, baseInvText, StringRef(argsOnlyCandidate->replacement));
          if (!directValid) {
            argsOnlyCandidate.reset();
            reuseExistingCallsitePatch = true;
          } else {
            // Both candidates are individually viable but not merge-compatible.
            // Defer to the proof lattice rather than letting the direct replay
            // win merely because it was produced in this local path.
            const bool preferDirect =
                deps_.proofLattice.AcceptedResultRanker().LatticePrefers(
                    argsOnlyCandidate->proofSummary, existingPatch->proofSummary);
            const bool preferExisting =
                deps_.proofLattice.AcceptedResultRanker().LatticePrefers(
                    existingPatch->proofSummary, argsOnlyCandidate->proofSummary);
            if (preferExisting && !preferDirect) {
              argsOnlyCandidate.reset();
              reuseExistingCallsitePatch = true;
            }
          }
        }
      }
    }
    }
  }

  // Route every macro-level candidate through the common final selector.
  //
  // This prevents nested structure-preserving artifacts from bypassing the
  // normal macro candidate competition. The only local exception is for
  // non-top-level construction sites: a nested macro carrier may be allowed
  // to fail only the top-level proof-root requirement while still
  // participating in selector-only competition. If such an artifact is
  // selected, it must be recertified onto an emission-discharged carrier
  // before any byte edit is emitted.
  const bool allowNonTopLevelMacroSelectorFailure =
      deps_.topology.GetRootMacroId(m.id) != m.id;

  // Reuse of an existing callsite patch is split into two cases:
  //
  // * `canReuseExistingCallsiteNoOp` means the current path has already
  //   decided to reuse the existing patch directly.
  // * `canReuseExistingCallsiteSkipWholeCover` means an existing
  //   structure-preserving callsite patch is strong enough to compete in
  //   the final selector without forcing a whole-cover plan.
  const bool canReuseExistingCallsiteNoOp =
      reuseExistingCallsitePatch &&
      !conflictingConcreteSubtreeWitnessForcesWholeCover && existingPatch;
  const bool canReuseExistingCallsiteSkipWholeCover =
      !canReuseExistingCallsiteNoOp &&
      !existingCallsitePatchAbsorbedByDirectCandidate &&
      !conflictingConcreteSubtreeWitnessForcesWholeCover && existingPatch &&
      existingIsCallsite && existingPatch->proof.preservesInvocationStructure &&
      existingPatch->proof.proofRootMacroId == m.id && !baseInvText.empty() &&
      RefoldLineObserverLayout::InvocationSpanMatchesCallsitePrefix(baseInvText,
                                                                    m);

  std::optional<WholeCoverPlan> wholeCoverPlan;
  bool canReuseExistingExpanded = false;

  // Expanded, non-structure-preserving patches are reusable only when they
  // are already proven for this same macro owner/root and still match the
  // current whole-cover plan. `__COUNTER__` is handled separately because
  // its literal realization proof is not a normal whole-cover realization.
  if (existingExpandedPatch &&
      !existingExpandedPatch->proof.preservesInvocationStructure &&
      deps_.macroPatchOwnerMatches(*existingExpandedPatch,
                                   planningCtx.currentPatchOwner)) {
    if (existingExpandedPatch->proof.proofRootMacroId == m.id) {
      if (existingExpandedPatch->proof.kind ==
              MacroPatchProofKind::CounterLiteral &&
          m.name == "__COUNTER__") {
        canReuseExistingExpanded = true;
      } else if (existingExpandedPatch->proof.kind ==
                 MacroPatchProofKind::WholeCoverRealization) {
        wholeCoverPlan = deps_.computeWholeCoverPlan(m);
        if (wholeCoverPlan)
          canReuseExistingExpanded = deps_.wholeCoverPatchMatchesPlan(
              *existingExpandedPatch, *wholeCoverPlan, m.id);
      }
    }
  }

  // Ensure the whole-cover plan is available for later selector logic,
  // even when no existing expanded patch was eligible for reuse.
  if (!wholeCoverPlan)
    wholeCoverPlan = deps_.computeWholeCoverPlan(m);

  // Rebuild the root ancestry index used by the final DAG/subtree
  // stability gate.  The gate only borrows this local storage and does
  // not affect final candidate ordering or proof ranking.
  DenseMap<uint64_t, const RefoldModel::MacroInvocation *> finalSubtreeInvById;
  finalSubtreeInvById.reserve(deps_.model.GetMacroInvocations().size());
  for (const RefoldModel::MacroInvocation &inv :
       deps_.model.GetMacroInvocations())
    finalSubtreeInvById[inv.id] = &inv;
  SmallVector<std::pair<size_t, size_t>, 1> finalSubtreeRootArgRanges;
  const MacroSubtreeReplayValidationContext finalSubtreeValidationCtx{
      m, baseInvText, finalSubtreeRootArgRanges, finalSubtreeInvById};

  // Final whole-cover-family selection is intentionally after all candidate
  // builders have run.  This admits only already-discovered candidates in the
  // deterministic construction order and delegates ranking to the proof
  // lattice; it does not discover new fallback paths.
  SmallVector<FinalMacroCandidate, 5> finalMacroCandidates;
  FinalMacroCandidateAdmissionContext finalAdmissionCtx{
      m,
      hEff,
      baseInvText,
      finalMacroCandidates,
      &finalSubtreeValidationCtx,
      allowNonTopLevelMacroSelectorFailure};


  if (argsOnlyCandidate) {
    addFinalMacroCandidate(
        deps_.replayStabilityValidator, deps_.proofLattice, finalAdmissionCtx,
        FinalMacroCandidate{*argsOnlyCandidate, MacroSelectionCandidate{},
                            FinalMacroCandidateOrigin::DirectArgsOnly});
  }

  if (dagRootCandidate) {
    addFinalMacroCandidate(
        deps_.replayStabilityValidator, deps_.proofLattice, finalAdmissionCtx,
        FinalMacroCandidate{*dagRootCandidate, MacroSelectionCandidate{},
                            FinalMacroCandidateOrigin::DagRootReplay});
  }

  if (canReuseExistingCallsiteNoOp && existingPatch) {
    addFinalMacroCandidate(
        deps_.replayStabilityValidator, deps_.proofLattice, finalAdmissionCtx,
        FinalMacroCandidate{
            *existingPatch, MacroSelectionCandidate{},
            FinalMacroCandidateOrigin::ReuseExistingCallsiteNoOp});
  }

  if (canReuseExistingCallsiteSkipWholeCover && existingPatch) {
    addFinalMacroCandidate(
        deps_.replayStabilityValidator, deps_.proofLattice, finalAdmissionCtx,
        FinalMacroCandidate{
            *existingPatch, MacroSelectionCandidate{},
            FinalMacroCandidateOrigin::ReuseExistingCallsiteSkipWholeCover});
  }

  if (canReuseExistingExpanded && existingExpandedPatch &&
      !theoremLatticeStructureCandidateDominatesRealization(
          deps_.proofLattice, finalAdmissionCtx, *existingExpandedPatch)) {
    // Reuse of an already-expanded same-owner patch is a realization
    // carrier.  Structure-vs-realization preference is expressed through
    // the shared lattice selector instead of a path-local origin
    // comparison.
    addFinalMacroCandidate(
        deps_.replayStabilityValidator, deps_.proofLattice, finalAdmissionCtx,
        FinalMacroCandidate{*existingExpandedPatch, MacroSelectionCandidate{},
                            FinalMacroCandidateOrigin::ReuseExistingExpanded});
  }

  bool wholeCoverCanReplaceInvocationSource = true;
  if (m.subkind == "func") {
    StringRef invocationText =
        !baseInvText.empty()
            ? baseInvText
            : (m.invText ? StringRef(*m.invText) : StringRef(""));
    wholeCoverCanReplaceInvocationSource =
        RefoldLineObserverLayout::InvocationSpanMatchesCallsitePrefix(
            invocationText, m);
  }

  // Log the final slot state after whole-cover source eligibility is known.
  // This keeps instrumentation-only diagnostics in the same scope as every
  // value reported by the trace record and avoids changing candidate ranking.
  REFOLD_LOG_TRACE(
      "macro/final-candidate",
      "slot-state inv id={0} name={1} argsOnly={2} dagRoot={3} "
      "reuseNoOp={4} reuseSkipWhole={5} reuseExpanded={6} wholeCoverPlan={7} "
      "wholeCoverCanReplaceSource={8} conflictForcesWhole={9} "
      "absorbedExisting={10}",
      m.id, m.name, argsOnlyCandidate ? 1 : 0, dagRootCandidate ? 1 : 0,
      canReuseExistingCallsiteNoOp ? 1 : 0,
      canReuseExistingCallsiteSkipWholeCover ? 1 : 0,
      canReuseExistingExpanded ? 1 : 0, wholeCoverPlan ? 1 : 0,
      wholeCoverCanReplaceInvocationSource ? 1 : 0,
      conflictingConcreteSubtreeWitnessForcesWholeCover ? 1 : 0,
      existingCallsitePatchAbsorbedByDirectCandidate ? 1 : 0);

  if (wholeCoverPlan && wholeCoverCanReplaceInvocationSource) {
    // Whole-cover realization is added as another final candidate unless
    // a selectable structure-preserving candidate for the same root
    // already wins the named lattice tie-breaker above.  Function-like
    // realization also requires a real source callsite.  Producer pseudo-
    // invocations for generated callees, such as `ADD, (1, 2)`, are not
    // syntactic callsites and cannot safely be replaced directly.
    const WholeCoverCandidate wholeCoverCandidate{m, invStart, invEnd,
                                                  *wholeCoverPlan};
    MacroPatch patch =
        materializeWholeCoverPatch(deps_.proofCertifier, wholeCoverCandidate);
    const bool structureCandidateDominatesWholeCover =
        theoremLatticeStructureCandidateDominatesRealization(
            deps_.proofLattice, finalAdmissionCtx, patch);
    REFOLD_LOG_TRACE(
        "macro/final-candidate",
        "whole-cover-candidate inv id={0} name={1} dominatedByStructure={2} replacement='{3}'",
        m.id, m.name, structureCandidateDominatesWholeCover ? 1 : 0,
        stringutils::showWsWithClip(patch.replacement, 220));
    if (!structureCandidateDominatesWholeCover)
      addFinalMacroCandidate(
          deps_.replayStabilityValidator, deps_.proofLattice, finalAdmissionCtx,
          FinalMacroCandidate{
              patch, MacroSelectionCandidate{},
              FinalMacroCandidateOrigin::WholeCoverRealization});
  }

  if (finalAdmissionCtx.candidates.empty()) {
    REFOLD_LOG_TRACE("macro/final-candidate",
                     "no-admitted-candidates inv id={0} name={1}", m.id,
                     m.name);
    return std::nullopt;
  }
  REFOLD_LOG_TRACE("macro/final-candidate",
                   "admitted-count inv id={0} name={1} count={2}", m.id,
                   m.name, finalAdmissionCtx.candidates.size());

  // The macro selector ranks MacroSelectionCandidate objects, not emitted
  // AcceptedResultCandidate objects.  This keeps selector-only nested
  // macro proofs out of the theorem-facing accepted-result selector while
  // preserving the parallel final-admission candidate array for
  // recovering the selected patch.
  const std::optional<SelectedMacroSelectionCandidate> selectedCandidate =
      selectPreferredFinalMacroCandidate(deps_.proofLattice, finalAdmissionCtx);
  if (!selectedCandidate) {
    REFOLD_LOG_TRACE(
        "macro/proof",
        "no final macro candidate survived proof-discharge gating: inv id={0} "
        "name={1} candidates={2} allowNestedSelectorOnly={3}",
        m.id, m.name, finalAdmissionCtx.candidates.size(),
        finalAdmissionCtx.allowNonTopLevelMacroSelectorFailure ? 1 : 0);
    return std::nullopt;
  }

  const FinalMacroCandidate &selected =
      finalAdmissionCtx.candidates[selectedCandidate->index];
  REFOLD_LOG_TRACE(
      "macro/final-candidate",
      "selected inv id={0} name={1} index={2} origin={3} proof={4} replacement='{5}'",
      m.id, m.name, selectedCandidate->index,
      finalMacroCandidateOriginName(selected.origin),
      toString(selected.patch.proof.kind),
      stringutils::showWsWithClip(selected.patch.replacement, 220));

  switch (selected.origin) {
  case FinalMacroCandidateOrigin::DirectArgsOnly:
    break;

  case FinalMacroCandidateOrigin::DagRootReplay:
    break;

  case FinalMacroCandidateOrigin::ReuseExistingCallsiteNoOp:
    if (selected.patch.subtree.backed)
      REFOLD_LOG_TRACE(
          "macro/proof",
          "subtree continuity probe: reused subtree-backed callsite patch "
          "without a fresh subtree winner in this pass inv id={0} name={1}",
          m.id, m.name);
    break;

  case FinalMacroCandidateOrigin::ReuseExistingCallsiteSkipWholeCover:
    if (selected.patch.subtree.backed) {
      REFOLD_LOG_TRACE(
          "macro/proof",
          "subtree continuity probe: reused subtree-backed callsite patch "
          "from skip-whole-cover path without a fresh subtree winner inv "
          "id={0} name={1}",
          m.id, m.name);
    }
    break;

  case FinalMacroCandidateOrigin::ReuseExistingExpanded:
    break;

  case FinalMacroCandidateOrigin::WholeCoverRealization:
    break;
  }

  MacroPatch selectedPatch = selected.patch;
  switch (selected.origin) {
  case FinalMacroCandidateOrigin::ReuseExistingCallsiteNoOp:
  case FinalMacroCandidateOrigin::ReuseExistingCallsiteSkipWholeCover:
  case FinalMacroCandidateOrigin::ReuseExistingExpanded:
    deps_.proofCertifier.CertifyReusedMacroPatchAcceptedCandidate(
        selectedPatch);
    break;
  case FinalMacroCandidateOrigin::DirectArgsOnly:
  case FinalMacroCandidateOrigin::DagRootReplay:
  case FinalMacroCandidateOrigin::WholeCoverRealization:
    break;
  }
  deps_.proofCertifier.CertifySelectedFinalMacroCandidate(m, *selectedCandidate,
                                                          selectedPatch);
  return selectedPatch;
}

} // namespace refold
} // namespace clang
