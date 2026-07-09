//===--- RefoldTUAnchorProof.cpp -----------------------------*- C++ -*-===//
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
#include "proof/RefoldTheoremAudit.h"

#include "llvm/Support/FormatVariadic.h"

#include <cassert>
#include <utility>

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
  case TUAnchorEvidenceKind::CorroboratedRightNeighbor:
  case TUAnchorEvidenceKind::CorroboratedLeftNeighbor:
    return witness.hasLeftNeighbor && witness.hasRightNeighbor;
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
  case AcceptedPathKind::MacroPasteDerivedCalleeSelector:
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
    if (witness->evidence == TUAnchorEvidenceKind::CorroboratedRightNeighbor ||
        witness->evidence == TUAnchorEvidenceKind::CorroboratedLeftNeighbor) {
      discharge.Require(witness->ownerDepthStable,
                        ProofObligationKind::TUOwnerDepthStableTracked,
                        ProofFailureReason::MissingTUOwnerDepthStability);
    }
    break;
  }

  case AcceptedPathKind::Unknown:
  case AcceptedPathKind::MacroArgsOnlyStandard:
  case AcceptedPathKind::MacroArgsOnlyPasteSingle:
  case AcceptedPathKind::MacroArgsOnlyPasteMulti:
  case AcceptedPathKind::MacroArgsOnlyPurePasteOnly:
  case AcceptedPathKind::MacroArgsOnlyPairedPureInsertion:
  case AcceptedPathKind::MacroPasteDerivedCalleeSelector:
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

void configureTUAnchorProofSummary(ProofSummary &summary) {
  summary.theoremClass = TheoremProofClass::DirectivePreservingProof;
  summary.acceptedClass = AcceptedProofClass::TUAnchor;
  summary.realizationMode = RealizationMode::PreserveOriginalStructure;
  summary.preference = SelectionPreference::PreferExactAnchoring;
  summary.surfaceDisposition = SurfaceDisposition::None;
  summary.structurePreserving = true;
  summary.primaryProofClassExplicit = true;
}

GlobalSelectionLattice
buildTUAnchorSelectionLattice(const ProofSummary &summary) {
  GlobalSelectionLattice lattice;
  switch (summary.inventory.currentPath) {
  case AcceptedPathKind::TUExactSlotBoundary:
  case AcceptedPathKind::TUProvableInsertionAnchor:
    lattice.domain = LatticeConflictDomain::TUAnchorPoint;
    lattice.mergeLaw = LatticeMergeLaw::SelectSingleWitness;
    lattice.conflictLaw = LatticeConflictLaw::PreferExactAnchorWitness;
    break;
  case AcceptedPathKind::Unknown:
  case AcceptedPathKind::MacroArgsOnlyStandard:
  case AcceptedPathKind::MacroArgsOnlyPasteSingle:
  case AcceptedPathKind::MacroArgsOnlyPasteMulti:
  case AcceptedPathKind::MacroArgsOnlyPurePasteOnly:
  case AcceptedPathKind::MacroArgsOnlyPairedPureInsertion:
  case AcceptedPathKind::MacroPasteDerivedCalleeSelector:
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
  return lattice;
}

CompletenessContract
buildTUAnchorCompletenessContract(const ProofSummary &summary) {
  CompletenessContract contract;
  if (summary.inventory.futureTarget != FutureProofTarget::Unknown &&
      summary.inventory.support == AcceptanceSupportKind::ExplicitProofBacked) {
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
  }
  return contract;
}

TheoremDomainContract
buildTUAnchorTheoremDomainContract(const ProofSummary &summary) {
  TheoremDomainContract contract;
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
    contract.hasExplicitExclusion = summary.completeness.hasExplicitExclusion;
    contract.explicitExclusion = summary.completeness.explicitExclusion;
    contract.declaredTarget = summary.completeness.declaredTarget;
    break;
  case CompletenessCoverageKind::Unknown:
    break;
  }
  return contract;
}

EmittedProof
buildEmittedProofFromTUAnchorSummary(TheoremProofClass theoremClass,
                                     const ProofSummary &summary) {
  EmittedProof proof;
  proof.theoremClass = theoremClass;
  proof.discharge = summary.discharge;
  if (summary.hasTUAnchorWitness)
    proof.tuAnchor = summary.tuAnchorWitness;
  return proof;
}

std::optional<EmittedProof>
buildCanonicalEmittedTUAnchorProof(const ProofSummary &summary) {
  if (summary.inventory.currentPath == AcceptedPathKind::Unknown)
    return std::nullopt;
  if (summary.inventory.support != AcceptanceSupportKind::ExplicitProofBacked)
    return std::nullopt;
  if (summary.theoremClass != TheoremProofClass::DirectivePreservingProof ||
      !summary.primaryProofClassExplicit)
    return std::nullopt;
  if (summary.discharge.status != ProofDischargeStatus::Discharged)
    return std::nullopt;
  if (summary.completeness.coverage !=
          CompletenessCoverageKind::DeclaredProofClass ||
      summary.theoremDomain.kind != TheoremDomainKind::DeclaredInDomainClass ||
      !summary.theoremDomain.inDeclaredDomain)
    return std::nullopt;
  return buildEmittedProofFromTUAnchorSummary(summary.theoremClass, summary);
}

void finalizeTUAnchorProofSummary(ProofSummary &summary) {
  const bool theoremFacing =
      summary.inventory.currentPath != AcceptedPathKind::Unknown;
  if (theoremFacing) {
    const bool hasDeclaredPrimaryClass =
        summary.theoremClass != TheoremProofClass::Unknown &&
        summary.primaryProofClassExplicit;
    if (!hasDeclaredPrimaryClass) {
      ++summary.discharge.obligationsEvaluated;
      if (summary.discharge.failedObligation == ProofObligationKind::Unknown)
        summary.discharge.failedObligation =
            ProofObligationKind::PrimaryProofClassDeclared;
      if (summary.discharge.failureReason == ProofFailureReason::None)
        summary.discharge.failureReason =
            ProofFailureReason::MissingPrimaryProofClass;
      summary.discharge.status = ProofDischargeStatus::Rejected;
    }
  }

  summary.lattice = buildTUAnchorSelectionLattice(summary);
  summary.completeness = buildTUAnchorCompletenessContract(summary);
  summary.theoremDomain = buildTUAnchorTheoremDomainContract(summary);
  summary.emittedProof = buildCanonicalEmittedTUAnchorProof(summary);
}

ProofSummary buildAcceptedTUAnchorProofSummary(AcceptedPathKind currentPath,
                                               const TUAnchorWitness &witness) {
  ProofSummary summary;
  summary.inventory = buildTUAnchorAcceptancePathInventory(currentPath);

  switch (currentPath) {
  case AcceptedPathKind::TUExactSlotBoundary:
  case AcceptedPathKind::TUProvableInsertionAnchor:
    configureTUAnchorProofSummary(summary);
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
  case AcceptedPathKind::MacroPasteDerivedCalleeSelector:
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

  finalizeTUAnchorProofSummary(summary);
  return summary;
}

void refreshTUAnchorEmissionPathInventory(AcceptedResultCandidate &candidate) {
  EmissionPathInventory inventory;
  inventory.Add(EmissionPathKind::TUAnchor);
  candidate.emissionPaths = std::move(inventory);
}

} // namespace

RefoldTUAnchorProof::RefoldTUAnchorProof(const RefoldTheoremAudit &theoremAudit)
    : theoremAudit_(theoremAudit) {}

AcceptedResultCandidate RefoldTUAnchorProof::BuildAcceptedTUAnchorCandidate(
    AcceptedPathKind currentPath, const TUAnchorWitness &witness) const {
  AcceptedResultCandidate candidate;
  candidate.kind = AcceptedResultCandidateKind::TUAnchor;

  // TU anchors are selector candidates for insertion/frontier proofs. Preserve
  // the witness-derived gap/byte position so later diagnostics can report the
  // exact anchor that discharged the path.
  candidate.proofSummary =
      buildAcceptedTUAnchorProofSummary(currentPath, witness);

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
        witness.evidence == TUAnchorEvidenceKind::ExactSlotBoundary ||
        witness.evidence == TUAnchorEvidenceKind::CorroboratedLeftNeighbor ||
        witness.evidence == TUAnchorEvidenceKind::CorroboratedRightNeighbor;
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
  refreshTUAnchorEmissionPathInventory(candidate);
  theoremAudit_.AuditAcceptedResultCandidateForLegacyAuthority(
      candidate, "BuildAcceptedTUAnchorCandidate");
  return candidate;
}

} // namespace refold
} // namespace clang
