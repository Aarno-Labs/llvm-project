//===--- RefoldProofLattice.cpp ---------------------------------*- C++ -*-===//
//
// This file implements clang-refold's accepted-result proof lattice.  The code
// here is intentionally side-effect-free with respect to source emission: it
// classifies proof summaries, normalizes emitted theorem carriers, ranks
// accepted candidates, and builds proof/audit carriers.  RefoldEngine remains
// responsible for deciding which edits to attempt and for emitting source text.
//
//===----------------------------------------------------------------------===//

#include "proof/RefoldProofLattice.h"

#include "edit/RefoldTUAnchorProof.h"
#include "edit/RefoldTUEditPlanner.h"
#include "macro/RefoldArgTextRecovery.h"
#include "proof/RefoldAcceptedResultPredicates.h"
#include "proof/RefoldOwnerStateProof.h"
#include "proof/RefoldProofVocabulary.h"
#include "proof/RefoldTheoremAudit.h"
#include "proof/RefoldWitnessClassifier.h"

using namespace llvm;

namespace clang {
namespace refold {

RefoldProofLattice::RefoldProofLattice(
    const RefoldModel &model, StringRef bSource, ArrayRef<PPTok> bToks,
    RefoldSourceMapper &sourceMapper, const RefoldTokenTextAnalysis &tokenText,
    const RefoldArgTextRecovery &argTextRecovery,
    const RefoldMacroTopology &macroTopology,
    const RefoldOwnerStateProof &ownerStateProof,
    const RefoldTerminalProofSink &terminalSink,
    const RefoldTUEditPlanner &tuEdits, const RefoldTheoremAudit &theoremAudit,
    TheoremAuditStats &lastTheoremAudit, bool strict,
    ProofAuditMode &proofAuditMode,
    std::vector<MixedOwnerTilingSegmentBinding>
        &mixedOwnerTilingSegmentBindings,
    std::vector<MixedOwnerTilingWitness> &mixedOwnerTilingWitnesses,
    Hooks hooks)
    : hooks_(std::move(hooks)), model_(model), bSource_(bSource), bToks_(bToks),
      witnessTrace_(strict, proofAuditMode),
      equivalenceKeyBuilder_(RefoldWitnessEquivalenceKeyBuilder::Dependencies{
          sourceMapper, bSource, bToks}),
      proofSummaryBuilder_(RefoldProofSummaryBuilder::Dependencies{bToks}),
      witnessResolver_(RefoldWitnessResolver::Dependencies{
          witnessTrace_, equivalenceKeyBuilder_, theoremAudit}),
      acceptedResultRanker_(RefoldAcceptedResultRanker::Dependencies{
          witnessTrace_, witnessResolver_, theoremAudit, lastTheoremAudit,
          // Forward proof-summary normalization to the dedicated builder
          // service so ranking and summary construction stay separate.
          [this](const AcceptedResultCandidate &candidate)
              -> std::optional<TheoremProofClass> {
            return proofSummaryBuilder_.NormalizeAcceptedProof(candidate);
          }}),
      ownerRealizationProofBuilder_(
          RefoldOwnerRealizationProofBuilder::Dependencies{
              model, ownerStateProof, proofSummaryBuilder_,
              acceptedResultRanker_, mixedOwnerTilingSegmentBindings,
              mixedOwnerTilingWitnesses,
              // The starter-summary callback is called only from
              // `BuildOwnerRealizationProofSummary`, which fires per
              // include/TU realization (not per macro invocation).  It
              // stays as a `std::function` to break the mutual construction
              // dependency between the two services.  Hot-path classifier
              // dependencies are direct references instead of callbacks.
              [this](AcceptedPathKind currentPath, const IncludePatch *patch,
                     const TUAnchorWitness *tuAnchorWitness,
                     const IncludeAnchorWitness *includeAnchorWitness,
                     const TerminalFallbackWitness *terminalFallbackWitness) {
                return acceptancePathClassifier_.BuildAcceptedPathProofSummary(
                    currentPath, patch, tuAnchorWitness, includeAnchorWitness,
                    terminalFallbackWitness);
              }}),
      acceptancePathClassifier_(RefoldAcceptancePathClassifier::Dependencies{
          proofSummaryBuilder_, ownerRealizationProofBuilder_,
          // The last two lattice-side path validators
          // (`ValidateIncludePreservingProof`,
          // `ValidateTUAnchorProof`) stay on the lattice because they are
          // include/TU-specific and are not on the macro hot path.
          [this](AcceptedPathKind currentPath, const IncludePatch *patch,
                 const IncludeAnchorWitness *witness) {
            return ValidateIncludePreservingProof(currentPath, patch, witness);
          },
          [this](AcceptedPathKind currentPath, const TUAnchorWitness *witness) {
            return ValidateTUAnchorProof(currentPath, witness);
          }}),
      macroPatchProofClassifier_(RefoldMacroPatchProofClassifier::Dependencies{
          macroTopology, ownerStateProof, proofSummaryBuilder_,
          ownerRealizationProofBuilder_, acceptancePathClassifier_}),
      acceptedCandidateBuilder_(RefoldAcceptedCandidateBuilder::Dependencies{
          model, tokenText, argTextRecovery, macroTopology, sourceMapper, bToks,
          theoremAudit, witnessTrace_, witnessResolver_, acceptedResultRanker_,
          proofSummaryBuilder_, ownerRealizationProofBuilder_,
          macroPatchProofClassifier_, acceptancePathClassifier_}),
      terminalSink_(terminalSink), tuEdits_(tuEdits) {}

::clang::refold::MacroPatchProof
RefoldProofLattice::MakeMacroPatchProof(MacroPatchProofKind kind,
                                        bool preservesInvocationStructure,
                                        uint64_t proofRootMacroId) const {
  MacroPatchProof proof;
  proof.kind = kind;
  proof.proofRootMacroId = proofRootMacroId;
  proof.preservesInvocationStructure = preservesInvocationStructure;
  return proof;
}

void RefoldProofLattice::SetMacroPatchProof(MacroPatch &patch,
                                            MacroPatchProof proof) const {
  // MacroPatchProof is the only MacroPatch-local proof carrier.
  // Install the caller-provided primary proof facts directly, then enrich the
  // carrier with any paste/subtree/call-chain witnesses derived from durable
  // patch metadata before rebuilding the normalized ProofSummary.
  patch.proof = std::move(proof);
  patch.selectedAcceptedCandidate.reset();
  macroPatchProofClassifier_.SyncMacroPatchProofSummary(patch);
}

void RefoldProofLattice::CertifyMacroWholeCoverRealizationPatch(
    MacroPatch &patch, const WholeCoverPlan &plan,
    const RefoldModel::MacroInvocation &macro) const {
  // Promote accepted whole-cover output into an explicit invocation
  // realization proof. The plan already carries the exact A/B token envelope
  // and containment facts, so certifying it here keeps the accepted patch
  // deterministic and fully described without changing selection behavior.
  patch.wholeCover.usedBodyRange = plan.usedBodyRange;
  patch.wholeCover.selfContained = plan.selfContained;
  patch.wholeCover.adjustedLeft = plan.adjustedLeft;
  patch.wholeCover.adjustedRight = plan.adjustedRight;
  patch.wholeCover.claimsClipped = plan.claimsClipped;
  patch.wholeCover.aLo = plan.covLoA;
  patch.wholeCover.aHi = plan.covHiA;
  patch.wholeCover.bRawLo = plan.rawBTokStart;
  patch.wholeCover.bRawHi = plan.rawBTokEnd;
  patch.wholeCover.bAdjLo = plan.bTokStart;
  patch.wholeCover.bAdjHi = plan.bTokEnd;
  patch.materialized.hasBTokenRange = true;
  patch.materialized.bTokStart = plan.bTokStart;
  patch.materialized.bTokEnd = plan.bTokEnd;
  const OwnerRealizationResult ownerRealization =
      ownerRealizationProofBuilder_.BuildMacroWholeCoverOwnerRealization(macro,
                                                                         plan);
  MacroPatchProof proof =
      MakeMacroPatchProof(MacroPatchProofKind::WholeCoverRealization,
                          /*preservesInvocationStructure=*/false, macro.id);
  if (ownerRealization.accepted)
    proof.ownerRealization = ownerRealization.witness;
  SetMacroPatchProof(patch, std::move(proof));
}

//===----------------------------------------------------------------------===//
// Accepted-result proof helpers
//===----------------------------------------------------------------------===//
//
// Macro invocation proof helpers.
//
// These routines validate producer paste/stringification witnesses and
// normalize macro-local proof facts before the accepted-result lattice ranks
// candidate patches.  They do not emit edits or mutate source text.
//

::clang::refold::ProofDischargeRecord
RefoldProofLattice::ValidateIncludePreservingProof(
    AcceptedPathKind currentPath, const IncludePatch *patch,
    const IncludeAnchorWitness *witness) const {
  if (currentPath == AcceptedPathKind::IncludePatchPendingMaterialization) {
    // Pending include materialization is internal-only. If some caller still
    // tries to validate it as a theorem-facing include path, fail closed
    // immediately rather than reporting a live transitional discharge state.
    ProofDischargeAccumulator discharge;
    discharge.Fail(ProofObligationKind::IncludePendingMaterializationClassified,
                   ProofFailureReason::PendingMaterialization);
    return discharge.Finish();
  }

  ProofDischargeAccumulator discharge;
  const AcceptancePathInventory inventory =
      acceptancePathClassifier_.BuildAcceptancePathInventory(currentPath);

  // Common include-preserving baseline: the accepted path must be classified,
  // mapped to a future proof target, and backed by an actual include patch
  // shape before path-specific witness obligations are checked below.
  acceptancePathClassifier_.RequireAcceptedPathBaseline(discharge, inventory);
  discharge.Require(patch != nullptr,
                    ProofObligationKind::IncludePatchShapeTracked,
                    ProofFailureReason::MissingIncludePatchShape);
  if (!patch)
    return discharge.Finish();

  // Every include-preserving path needs an anchor witness. The switch below
  // refines this generic requirement into the exact witness kind and fields
  // required by each accepted include path.
  discharge.Require(witness &&
                        witness->evidence != IncludeAnchorEvidenceKind::Unknown,
                    ProofObligationKind::IncludeAnchorWitnessTracked,
                    ProofFailureReason::MissingIncludeAnchorWitness);

  switch (currentPath) {
  case AcceptedPathKind::IncludeDeleteReplaceMappedHeaderTokens:
    // Replacement/deletion over an existing include must be tied to concrete
    // mapped header tokens and the corresponding source byte range.
    discharge.Require(patch->aStart < patch->aEnd,
                      ProofObligationKind::IncludeMappedHeaderRangeTracked,
                      ProofFailureReason::MissingMappedHeaderRange);
    discharge.Require(witness &&
                          witness->evidence ==
                              IncludeAnchorEvidenceKind::MappedHeaderTokens,
                      ProofObligationKind::IncludeAnchorWitnessTracked,
                      ProofFailureReason::MissingIncludeAnchorWitness);
    discharge.Require(witness && witness->hasFirstPP && witness->hasLastPP &&
                          witness->firstPP <= witness->lastPP,
                      ProofObligationKind::IncludeMappedHeaderRangeTracked,
                      ProofFailureReason::MissingMappedHeaderRange);
    discharge.Require(witness && witness->hasByteRange &&
                          witness->startByte <= witness->endByte,
                      ProofObligationKind::IncludeMappedHeaderByteRangeTracked,
                      ProofFailureReason::MissingMappedHeaderByteRange);
    break;

  case AcceptedPathKind::IncludeInsertSelectedConditionalBoundary:
    // Conditional-boundary insertion is zero-width in A and must be anchored to
    // the selected conditional arm boundary byte.
    acceptancePathClassifier_.RequireIncludeZeroWidthAnchor(
        discharge, *patch, witness,
        IncludeAnchorEvidenceKind::SelectedConditionalBoundary,
        ProofObligationKind::IncludeSelectedConditionalBoundaryWitnessTracked,
        ProofFailureReason::MissingIncludeSelectedConditionalBoundaryWitness);
    discharge.Require(
        witness && witness->hasCondArmId,
        ProofObligationKind::IncludeSelectedConditionalBoundaryWitnessTracked,
        ProofFailureReason::MissingIncludeSelectedConditionalBoundaryWitness);
    break;

  case AcceptedPathKind::IncludeInsertChildBoundary: {
    // IncludeInsertionByChildBoundary is a declared include-preserving proof,
    // not a legacy fallback.  The accepted patch must be a zero-width insertion
    // owned by a concrete parent include, and the witness must name a direct
    // child include whose spelled directive boundary is exactly the recorded
    // anchor byte.  This keeps the theorem obligation owner-local and prevents
    // an arbitrary child id from masquerading as a stable parent-header
    // insertion point.
    acceptancePathClassifier_.RequireIncludeZeroWidthAnchor(
        discharge, *patch, witness, IncludeAnchorEvidenceKind::ChildBoundary,
        ProofObligationKind::IncludeChildBoundaryWitnessTracked,
        ProofFailureReason::MissingIncludeChildBoundaryWitness);
    discharge.Require(patch->include != nullptr,
                      ProofObligationKind::IncludeChildBoundaryWitnessTracked,
                      ProofFailureReason::MissingIncludeChildBoundaryWitness);
    discharge.Require(witness && witness->hasChildIncludeId,
                      ProofObligationKind::IncludeChildBoundaryWitnessTracked,
                      ProofFailureReason::MissingIncludeChildBoundaryWitness);

    bool childBoundaryMatchesParent = false;
    if (patch->include && witness && witness->hasChildIncludeId &&
        witness->hasAnchorByte) {
      for (const auto &child : model_.GetIncludes()) {
        if (child.id != witness->childIncludeId)
          continue;
        const bool directChildOfPatchOwner =
            child.parent && *child.parent == patch->include->id;
        const bool anchorIsSpelledChildBoundary =
            witness->anchorByte == child.siteB ||
            witness->anchorByte == child.siteE;
        childBoundaryMatchesParent =
            directChildOfPatchOwner && anchorIsSpelledChildBoundary;
        break;
      }
    }
    discharge.Require(childBoundaryMatchesParent,
                      ProofObligationKind::IncludeChildBoundaryWitnessTracked,
                      ProofFailureReason::MissingIncludeChildBoundaryWitness);
    break;
  }

  case AcceptedPathKind::IncludeInsertRightNeighborPP:
    // Right-neighbor insertion anchors before a known preprocessor token. The
    // anchor byte and neighbor PP token together identify the stable insertion
    // point.
    acceptancePathClassifier_.RequireIncludeZeroWidthAnchor(
        discharge, *patch, witness, IncludeAnchorEvidenceKind::RightNeighborPP,
        ProofObligationKind::IncludeRightNeighborWitnessTracked,
        ProofFailureReason::MissingIncludeRightNeighborWitness);
    discharge.Require(witness && witness->hasNeighborPP,
                      ProofObligationKind::IncludeRightNeighborWitnessTracked,
                      ProofFailureReason::MissingIncludeRightNeighborWitness);
    break;

  case AcceptedPathKind::IncludeInsertLeftNeighborPP:
    // Left-neighbor insertion anchors after a known preprocessor token. As with
    // the right-neighbor case, require both the byte anchor and the neighbor PP
    // identity so the insertion point is theorem-facing.
    acceptancePathClassifier_.RequireIncludeZeroWidthAnchor(
        discharge, *patch, witness, IncludeAnchorEvidenceKind::LeftNeighborPP,
        ProofObligationKind::IncludeLeftNeighborWitnessTracked,
        ProofFailureReason::MissingIncludeLeftNeighborWitness);
    discharge.Require(witness && witness->hasNeighborPP,
                      ProofObligationKind::IncludeLeftNeighborWitnessTracked,
                      ProofFailureReason::MissingIncludeLeftNeighborWitness);
    break;

  case AcceptedPathKind::IncludeInsertDeclBoundary:
    // Declaration-boundary insertion is anchored at the end of the declaration
    // header range. Requiring anchorByte == declHeaderE prevents a witness from
    // naming the right range but anchoring at a different byte.
    acceptancePathClassifier_.RequireIncludeZeroWidthAnchor(
        discharge, *patch, witness, IncludeAnchorEvidenceKind::DeclBoundary,
        ProofObligationKind::IncludeDeclBoundaryWitnessTracked,
        ProofFailureReason::MissingIncludeDeclBoundaryWitness);
    discharge.Require(witness && witness->hasDeclHeaderRange &&
                          witness->anchorByte == witness->declHeaderE,
                      ProofObligationKind::IncludeDeclBoundaryWitnessTracked,
                      ProofFailureReason::MissingIncludeDeclBoundaryWitness);
    break;

  case AcceptedPathKind::IncludePatchPendingMaterialization:
  case AcceptedPathKind::IncludeRealizationInlineFromB:
  case AcceptedPathKind::IncludeMaterializedExpansion:
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
  case AcceptedPathKind::MacroPasteDerivedCalleeSelector:
  case AcceptedPathKind::TUExactSlotBoundary:
  case AcceptedPathKind::TUProvableInsertionAnchor:
  case AcceptedPathKind::TUByteSpanMappedEdit:
  case AcceptedPathKind::TUByteSpanConservativeEdit:
  case AcceptedPathKind::TUIncludeClosureEdit:
  case AcceptedPathKind::TerminalEmitEditedPreprocessedStream:
    // Non-include-preserving paths have no additional obligations in this
    // validator. The common path-classification checks above still record any
    // mismatch if such a path reaches this function unexpectedly.
    break;
  }

  return discharge.Finish();
}

::clang::refold::ProofDischargeRecord RefoldProofLattice::ValidateTUAnchorProof(
    AcceptedPathKind currentPath, const TUAnchorWitness *witness) const {
  return validateTUAnchorProof(
      currentPath, witness,
      acceptancePathClassifier_.BuildAcceptancePathInventory(currentPath));
}

TerminalFallbackWitness
RefoldProofLattice::BuildTerminalFallbackWitness() const {
  TerminalFallbackWitness witness;

  // Ordered typed requests are the single source of truth.  There is no
  // branch-local fallback boolean, no primary failure scalar, and no separate
  // proof-failure vector mirror.  The primary failed obligation is simply the
  // first classified request failure copied into this witness.
  witness.proofFailures.reserve(terminalSink_.Requests().size());
  for (const TerminalFallbackRequest &request : terminalSink_.Requests()) {
    if (IsClassifiedTerminalFallbackProofFailure(request.failure))
      witness.proofFailures.push_back(request.failure);
  }

  if (witness.proofFailures.empty()) {
    // Do not derive the failed obligation from an aggregate terminal kind.  If
    // this state is ever reached, the bug is the missing caller-supplied proof
    // failure itself, so report a theorem-audit invariant violation rather than
    // inventing an owner/state reason here.
    witness.proofFailures.push_back(MakeTerminalFallbackProofFailure(
        TerminalFallbackObligationKind::TheoremAuditInvariantSatisfied,
        TerminalFallbackFailureReason::TheoremAuditInvariantViolation,
        TerminalFallbackFailureContext::ForStateComponent(
            "terminalFallbackWitness")));
  }

  return witness;
}

bool RefoldProofLattice::IsOwnerUnresolvedNoTUAnchorOutOfDomain(
    const diffutils::Hunk &h, StringRef tuPath, const Owner &owner,
    bool mapsToTU) const {
  // This is the explicit out-of-domain predicate for hunks whose owner could
  // not be resolved and whose TU-level fallback witnesses are also unavailable.
  // It must remain derived-only: by the time this helper returns true, all
  // deterministic owner, include-boundary, TU-anchor, and TU-byte-span searches
  // have already failed. Do not infer ownership from proximity here.
  if (owner.kind != OwnerKind::Unknown)
    return false;

  if (h.isInsertOnly()) {
    // Pure insertions have extra deterministic witnesses: they may belong to a
    // parent include boundary, a provable TU insertion anchor, or a zero-width
    // TU byte span. Only if all of those are absent is the insertion outside
    // the declared refolding domain.
    if (tuEdits_.FindBoundaryParentIncludeForPureInsertion(h))
      return false;
    if (mapsToTU)
      return false;
    if (tuEdits_.FindProvableTUInsertionAnchor(h.aStart, tuPath))
      return false;
    if (tuEdits_.PlanTUByteSpan(h.aStart, h.aEnd, tuPath))
      return false;
    return true;
  }

  // Non-insertion hunks are in-domain if the token map or TU byte-span mapping
  // can still witness the affected range. Without either, there is no declared
  // theorem-facing owner or TU anchor for this hunk.
  if (mapsToTU)
    return false;
  if (tuEdits_.PlanTUByteSpan(h.aStart, h.aEnd, tuPath))
    return false;
  return true;
}

std::string RefoldProofLattice::BuildOwnerUnresolvedNoTUAnchorDetail(
    size_t hunkIndex, const diffutils::Hunk &h, StringRef tuPath,
    const Owner &owner, bool mapsToTU) const {
  const bool isInsertion = h.aStart == h.aEnd;

  // This detail string is emitted only after normal owner resolution has failed
  // to produce a macro/include/TU witness. Record those exhausted search spaces
  // explicitly so the terminal fallback explains the domain wall in proof
  // terms, not merely as `OwnerKind::Unknown`.
  const bool macroOwnerExhausted = true;
  const bool includeOwnerExhausted = owner.kind != OwnerKind::Include;

  // For pure insertions, check whether the insertion could still be explained
  // by an include-boundary parent. If present, this is a deterministic include
  // witness, not an out-of-domain owner-unresolved case.
  const RefoldModel::IncludeItem *boundaryInc = nullptr;
  if (isInsertion) {
    if (auto boundaryPlan =
            tuEdits_.FindBoundaryParentIncludeForPureInsertion(h))
      boundaryInc = model_.GetIncludeById(boundaryPlan->includeId);
  }
  const bool hasBoundaryInclude = boundaryInc != nullptr;

  // Also test the TU insertion-anchor path. A provable TU anchor keeps an
  // otherwise ownerless insertion inside the declared refolding domain.
  std::optional<TUInsertionAnchor> provableInsertionAnchor;
  if (isInsertion)
    provableInsertionAnchor =
        tuEdits_.FindProvableTUInsertionAnchor(h.aStart, tuPath);
  const bool hasProvableInsertionAnchor = provableInsertionAnchor.has_value();

  // Finally, check whether the hunk can be represented directly as a TU byte
  // span. This is the last generic TU witness before the edit is declared
  // outside the owner/TU-anchor domain.
  std::optional<TUByteSpanPlan> tuSpan =
      tuEdits_.PlanTUByteSpan(h.aStart, h.aEnd, tuPath);
  const bool hasTUByteSpan = tuSpan.has_value();
  const bool declaredDomainWall =
      IsOwnerUnresolvedNoTUAnchorOutOfDomain(h, tuPath, owner, mapsToTU);

  // These insertion-only fields make the diagnostic precise about which
  // insertion witnesses were attempted and whether each one was absent.
  std::string exactSlot = "n/a";
  std::string insertionAnchor = "n/a";
  std::string boundaryParent = "n/a";
  if (isInsertion) {
    if (auto slot = tuEdits_.FindExactSlotBoundaryFromPPGap(tuPath, h.aStart))
      exactSlot = llvm::formatv("{0}", *slot).str();
    else
      exactSlot = "none";

    if (provableInsertionAnchor)
      insertionAnchor =
          llvm::formatv("{0}", provableInsertionAnchor->tuByteOffset).str();
    else
      insertionAnchor = "none";

    if (boundaryInc)
      boundaryParent = llvm::formatv("inc#{0}", boundaryInc->id).str();
    else
      boundaryParent = "none";
  }

  const std::string tuSpanStr =
      tuSpan
          ? llvm::formatv("[{0},{1})", tuSpan->tuByteBegin, tuSpan->tuByteEnd)
                .str()
          : std::string("none");

  return llvm::formatv("edit #{0} A=[{1},{2}) insert={3} owner={4} "
                       "macroOwnerExhausted={5} "
                       "includeOwnerExhausted={6} mapsToTU={7} TUByteSpan={8} "
                       "hasTUByteSpan={9} "
                       "exactSlot={10} insertionAnchor={11} "
                       "hasProvableInsertionAnchor={12} "
                       "boundaryParentInclude={13} hasBoundaryInclude={14} "
                       "declaredDomainWall={15}",
                       hunkIndex, h.aStart, h.aEnd, isInsertion ? "yes" : "no",
                       owner.kind, macroOwnerExhausted ? "yes" : "no",
                       includeOwnerExhausted ? "yes" : "no",
                       mapsToTU ? "yes" : "no", tuSpanStr,
                       hasTUByteSpan ? "yes" : "no", exactSlot, insertionAnchor,
                       hasProvableInsertionAnchor ? "yes" : "no",
                       boundaryParent, hasBoundaryInclude ? "yes" : "no",
                       declaredDomainWall ? "yes" : "no")
      .str();
}

std::optional<std::string> RefoldProofLattice::BuildWholeCoverReplacementText(
    const RefoldModel::MacroInvocation &m) const {
  // Reuse the same whole-cover planning path used by patch construction so the
  // returned replacement text obeys the same clipping/envelope policy.
  auto plan = hooks_.computeWholeCoverPlan(m);
  if (!plan)
    return std::nullopt;
  return plan->clippedText;
}

//===----------------------------------------------------------------------===//
} // namespace refold
} // namespace clang
