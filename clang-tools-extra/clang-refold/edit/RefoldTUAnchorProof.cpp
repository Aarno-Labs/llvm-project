//===--- RefoldTUAnchorProof.cpp --------------------------------*- C++ -*-===//
//
// TU-anchor accepted-result proof construction.
//
// This file intentionally contains only the TU-anchor subset of the accepted
// carrier builder.  It is not a general proof lattice: it does not compare
// candidates, assemble text edits, or decide whether an A/B hunk belongs to the
// TU.  Its job is to certify a local TUAnchorWitness into the normalized
// AcceptedResultCandidate shape used by the existing selector/audit machinery.
//
//===----------------------------------------------------------------------===//

#include "edit/RefoldTUAnchorProof.h"

#include "proof/RefoldAcceptedResultPredicates.h"
#include "proof/RefoldTheoremAudit.h"

#include "llvm/Support/FormatVariadic.h"

using namespace llvm;

namespace clang {
namespace refold {

bool tuAnchorWitnessHasProvableEvidence(const TUAnchorWitness &witness) {
  switch (witness.evidence) {
  case TUAnchorEvidenceKind::ArgLikeBegin:
    return witness.macroId != 0;
  case TUAnchorEvidenceKind::ImmediateRightNeighbor:
    return witness.hasRightNeighbor;
  case TUAnchorEvidenceKind::ImmediateLeftNeighbor:
    return witness.hasLeftNeighbor;
  case TUAnchorEvidenceKind::IncludeDirectiveBoundary:
    return witness.hasLeftNeighbor && witness.hasRightNeighbor;
  case TUAnchorEvidenceKind::ZeroTokenIncludeBoundary:
    return witness.slotId != 0 && !witness.slotKind.empty();
  case TUAnchorEvidenceKind::Unknown:
  case TUAnchorEvidenceKind::ExactSlotBoundary:
    return false;
  }
  return false;
}

namespace {

AcceptancePathInventory
buildTUAnchorAcceptancePathInventory(AcceptedPathKind currentPath) {
  AcceptancePathInventory inventory;
  inventory.currentPath = currentPath;

  switch (currentPath) {
  case AcceptedPathKind::TUExactSlotBoundary:
    inventory.support = AcceptanceSupportKind::ExplicitProofBacked;
    inventory.futureTarget = FutureProofTarget::TUExactSlotAnchor;
    break;
  case AcceptedPathKind::TUProvableInsertionAnchor:
    inventory.support = AcceptanceSupportKind::ExplicitProofBacked;
    inventory.futureTarget = FutureProofTarget::TUProvableInsertionAnchor;
    break;
  case AcceptedPathKind::Unknown:
  case AcceptedPathKind::MacroArgsOnlyStandard:
  case AcceptedPathKind::MacroArgsOnlyPasteSingle:
  case AcceptedPathKind::MacroArgsOnlyPasteMulti:
  case AcceptedPathKind::MacroArgsOnlyPurePasteOnly:
  case AcceptedPathKind::MacroArgsOnlyPairedPureInsertion:
  case AcceptedPathKind::MacroDirectCalleeSubstitution:
  case AcceptedPathKind::MacroPasteDerivedCalleeSelector:
  case AcceptedPathKind::MacroRecursiveTupleGeneratedCalleeReplay:
  case AcceptedPathKind::MacroDagSubtreeRoot:
  case AcceptedPathKind::MacroCallChainSuffix:
  case AcceptedPathKind::MacroCounterLiteral:
  case AcceptedPathKind::MacroWholeCoverRealization:
  case AcceptedPathKind::IncludePatchPendingMaterialization:
  case AcceptedPathKind::IncludeDeleteReplaceMappedHeaderTokens:
  case AcceptedPathKind::IncludeInsertSelectedConditionalBoundary:
  case AcceptedPathKind::IncludeInsertChildBoundary:
  case AcceptedPathKind::IncludeInsertRightNeighborPP:
  case AcceptedPathKind::IncludeInsertLeftNeighborPP:
  case AcceptedPathKind::IncludeInsertDeclBoundary:
  case AcceptedPathKind::IncludeRealizationInlineFromB:
  case AcceptedPathKind::IncludeMaterializedExpansion:
  case AcceptedPathKind::TUByteSpanMappedEdit:
  case AcceptedPathKind::TUByteSpanConservativeEdit:
  case AcceptedPathKind::TUIncludeClosureEdit:
  case AcceptedPathKind::TerminalEmitEditedPreprocessedStream:
    break;
  }

  return inventory;
}

} // namespace

ProofDischargeRecord
validateTUAnchorProof(AcceptedPathKind currentPath,
                      const TUAnchorWitness *witness,
                      const AcceptancePathInventory &inventory) {
  ProofDischargeAccumulator discharge;

  const bool classified =
      currentPath == AcceptedPathKind::TUExactSlotBoundary ||
      currentPath == AcceptedPathKind::TUProvableInsertionAnchor;
  discharge.Require(classified, ProofObligationKind::TUAnchorPathClassified,
                    ProofFailureReason::MissingTUAnchorClassification);
  discharge.Require(inventory.futureTarget != FutureProofTarget::Unknown,
                    ProofObligationKind::FutureTargetMapped,
                    ProofFailureReason::MissingFutureTargetMapping);

  discharge.Require(witness &&
                        witness->evidence != TUAnchorEvidenceKind::Unknown,
                    ProofObligationKind::TUAnchorWitnessTracked,
                    ProofFailureReason::MissingTUAnchorWitness);
  discharge.Require(witness && witness->hasPPGap,
                    ProofObligationKind::TUAnchorPPGapTracked,
                    ProofFailureReason::MissingTUAnchorGap);
  discharge.Require(witness && witness->hasTUByte,
                    ProofObligationKind::TUAnchorByteTracked,
                    ProofFailureReason::MissingTUAnchorByte);

  if (!witness)
    return discharge.Finish();

  switch (currentPath) {
  case AcceptedPathKind::TUExactSlotBoundary: {
    const bool exactSlotWitness =
        witness->evidence == TUAnchorEvidenceKind::ExactSlotBoundary &&
        witness->exactPPMatch && witness->slotId != 0 &&
        !witness->slotKind.empty();
    discharge.Require(exactSlotWitness,
                      ProofObligationKind::TUExactSlotWitnessTracked,
                      ProofFailureReason::MissingTUExactSlotWitness);
    break;
  }

  case AcceptedPathKind::TUProvableInsertionAnchor: {
    discharge.Require(tuAnchorWitnessHasProvableEvidence(*witness),
                      ProofObligationKind::TUProvableEvidenceTracked,
                      ProofFailureReason::MissingTUProvableAnchorWitness);
    discharge.Require(witness->outsideIncludeCoverage ||
                          witness->evidence ==
                              TUAnchorEvidenceKind::IncludeDirectiveBoundary,
                      ProofObligationKind::TUOutsideIncludeCoverageTracked,
                      ProofFailureReason::MissingTUOutsideIncludeCoverageProof);
    break;
  }

  case AcceptedPathKind::Unknown:
  case AcceptedPathKind::MacroArgsOnlyStandard:
  case AcceptedPathKind::MacroArgsOnlyPasteSingle:
  case AcceptedPathKind::MacroArgsOnlyPasteMulti:
  case AcceptedPathKind::MacroArgsOnlyPurePasteOnly:
  case AcceptedPathKind::MacroArgsOnlyPairedPureInsertion:
  case AcceptedPathKind::MacroDirectCalleeSubstitution:
  case AcceptedPathKind::MacroPasteDerivedCalleeSelector:
  case AcceptedPathKind::MacroRecursiveTupleGeneratedCalleeReplay:
  case AcceptedPathKind::MacroDagSubtreeRoot:
  case AcceptedPathKind::MacroCallChainSuffix:
  case AcceptedPathKind::MacroCounterLiteral:
  case AcceptedPathKind::MacroWholeCoverRealization:
  case AcceptedPathKind::IncludePatchPendingMaterialization:
  case AcceptedPathKind::IncludeDeleteReplaceMappedHeaderTokens:
  case AcceptedPathKind::IncludeInsertSelectedConditionalBoundary:
  case AcceptedPathKind::IncludeInsertChildBoundary:
  case AcceptedPathKind::IncludeInsertRightNeighborPP:
  case AcceptedPathKind::IncludeInsertLeftNeighborPP:
  case AcceptedPathKind::IncludeInsertDeclBoundary:
  case AcceptedPathKind::IncludeRealizationInlineFromB:
  case AcceptedPathKind::IncludeMaterializedExpansion:
  case AcceptedPathKind::TUByteSpanMappedEdit:
  case AcceptedPathKind::TUByteSpanConservativeEdit:
  case AcceptedPathKind::TUIncludeClosureEdit:
  case AcceptedPathKind::TerminalEmitEditedPreprocessedStream:
    break;
  }

  return discharge.Finish();
}

namespace {

/// Build the normalized proof summary for a TU insertion anchor.
///
/// Only TUExactSlotBoundary and TUProvableInsertionAnchor are TU-anchor paths;
/// any other path leaves the summary unconfigured, so the shared finalizer
/// records the missing primary-proof-class obligation and the summary fails
/// closed with no emitted proof.
ProofSummary
buildAcceptedTUAnchorProofSummary(const RefoldProofSummaryBuilder &builder,
                                  AcceptedPathKind currentPath,
                                  const TUAnchorWitness &witness) {
  ProofSummary summary;
  summary.inventory = buildTUAnchorAcceptancePathInventory(currentPath);

  switch (currentPath) {
  case AcceptedPathKind::TUExactSlotBoundary:
  case AcceptedPathKind::TUProvableInsertionAnchor:
    builder.ConfigureProofSummary(
        summary, TheoremProofClass::DirectivePreservingProof,
        AcceptedProofClass::TUAnchor,
        RealizationMode::PreserveOriginalStructure,
        SelectionPreference::PreferExactAnchoring, SurfaceDisposition::None,
        /*structurePreserving=*/true);
    summary.hasTUAnchorWitness = true;
    summary.tuAnchorWitness = witness;
    summary.discharge =
        validateTUAnchorProof(currentPath, &witness, summary.inventory);
    break;
  case AcceptedPathKind::Unknown:
  case AcceptedPathKind::MacroArgsOnlyStandard:
  case AcceptedPathKind::MacroArgsOnlyPasteSingle:
  case AcceptedPathKind::MacroArgsOnlyPasteMulti:
  case AcceptedPathKind::MacroArgsOnlyPurePasteOnly:
  case AcceptedPathKind::MacroArgsOnlyPairedPureInsertion:
  case AcceptedPathKind::MacroDirectCalleeSubstitution:
  case AcceptedPathKind::MacroPasteDerivedCalleeSelector:
  case AcceptedPathKind::MacroRecursiveTupleGeneratedCalleeReplay:
  case AcceptedPathKind::MacroDagSubtreeRoot:
  case AcceptedPathKind::MacroCallChainSuffix:
  case AcceptedPathKind::MacroCounterLiteral:
  case AcceptedPathKind::MacroWholeCoverRealization:
  case AcceptedPathKind::IncludePatchPendingMaterialization:
  case AcceptedPathKind::IncludeDeleteReplaceMappedHeaderTokens:
  case AcceptedPathKind::IncludeInsertSelectedConditionalBoundary:
  case AcceptedPathKind::IncludeInsertChildBoundary:
  case AcceptedPathKind::IncludeInsertRightNeighborPP:
  case AcceptedPathKind::IncludeInsertLeftNeighborPP:
  case AcceptedPathKind::IncludeInsertDeclBoundary:
  case AcceptedPathKind::IncludeRealizationInlineFromB:
  case AcceptedPathKind::IncludeMaterializedExpansion:
  case AcceptedPathKind::TUByteSpanMappedEdit:
  case AcceptedPathKind::TUByteSpanConservativeEdit:
  case AcceptedPathKind::TUIncludeClosureEdit:
  case AcceptedPathKind::TerminalEmitEditedPreprocessedStream:
    break;
  }

  builder.FinalizeProofSummary(summary);
  return summary;
}

} // namespace

RefoldTUAnchorProof::RefoldTUAnchorProof(const RefoldTheoremAudit &theoremAudit,
                                         ArrayRef<PPTok> bToks)
    : theoremAudit_(theoremAudit),
      proofSummaryBuilder_(RefoldProofSummaryBuilder::Dependencies{bToks}) {}

AcceptedResultCandidate RefoldTUAnchorProof::BuildAcceptedTUAnchorCandidate(
    AcceptedPathKind currentPath, const TUAnchorWitness &witness) const {
  AcceptedResultCandidate candidate;
  candidate.kind = AcceptedResultCandidateKind::TUAnchor;

  // TU anchors are selector candidates for insertion/frontier proofs. Preserve
  // the witness-derived gap/byte position so later diagnostics can report the
  // exact anchor that discharged the path.
  candidate.proofSummary = buildAcceptedTUAnchorProofSummary(
      proofSummaryBuilder_, currentPath, witness);

  if (witness.hasPPGap)
    candidate.begin = candidate.end = witness.ppGap;
  if (witness.hasTUByte) {
    candidate.hasAnchorByte = true;
    candidate.anchorByte = witness.tuByte;
  }

  // TU-anchor insertions are zero-token boundary witnesses when the producer
  // supplied an exact PP gap / source-byte frontier.  Preserve the existing
  // anchor proof and expose the boundary dimensions to the common witness key
  // without changing anchor selection.
  if (witness.hasPPGap && witness.hasTUByte) {
    candidate.hasZeroTokenBoundaryWitness = true;
    candidate.zeroTokenOwnerKind = "tu";
    candidate.zeroTokenOwnerId = 0;
    candidate.zeroTokenHasPPGap = true;
    candidate.zeroTokenPPGap = witness.ppGap;
    candidate.zeroTokenHasSourceAnchor = true;
    candidate.zeroTokenSourceAnchor = witness.tuByte;
    candidate.zeroTokenHasBTokenRange = true;
    candidate.zeroTokenBTokStart = witness.ppGap;
    candidate.zeroTokenBTokEnd = witness.ppGap;
    candidate.zeroTokenProducerProven =
        tuAnchorWitnessHasProvableEvidence(witness) || witness.exactPPMatch;
    candidate.zeroTokenOwnerClosed = true;
    candidate.zeroTokenLayoutStable = true;
    candidate.zeroTokenObserversStable = witness.ownerDepthStable ||
                                         witness.outsideIncludeCoverage ||
                                         witness.exactPPMatch;
    candidate.zeroTokenCounterStable = true;
    candidate.zeroTokenFromTUAnchor = true;
    candidate.zeroTokenFromIncludeBoundary =
        witness.evidence == TUAnchorEvidenceKind::ZeroTokenIncludeBoundary ||
        witness.evidence == TUAnchorEvidenceKind::IncludeDirectiveBoundary;
    candidate.zeroTokenFromDirectiveLayoutGap =
        witness.evidence == TUAnchorEvidenceKind::ExactSlotBoundary;
    candidate.zeroTokenBoundarySignature =
        llvm::formatv("tu-anchor:evidence={0}:slot={1}:{2}:pp_gap={3}:"
                      "byte={4}:left={5}:{6}:right={7}:{8}:outside_include={9}:"
                      "owner_depth={10}",
                      witness.evidence, witness.slotId, witness.slotKind,
                      witness.ppGap, witness.tuByte,
                      witness.hasLeftNeighbor ? 1 : 0, witness.leftNeighborPP,
                      witness.hasRightNeighbor ? 1 : 0, witness.rightNeighborPP,
                      witness.outsideIncludeCoverage ? 1 : 0,
                      witness.ownerDepthStable ? 1 : 0)
            .str();
  }

  // TU-anchor candidates preserve only the local zero-token boundary fields.
  // Line-control, counter, owner, and suffix-state witness facts enter through
  // proof summaries that explicitly own those facts, so this builder does not
  // synthesize additional carrier-local witness state while recertifying the
  // selector-visible TU-anchor candidate.
  RefreshAcceptedCandidateEmissionPathInventory(candidate);
  theoremAudit_.AuditAcceptedResultCandidateForLegacyAuthority(
      candidate, "BuildAcceptedTUAnchorCandidate");
  return candidate;
}

} // namespace refold
} // namespace clang
