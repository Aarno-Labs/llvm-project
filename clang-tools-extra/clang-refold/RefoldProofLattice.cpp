//===--- RefoldProofLattice.cpp --------------------------------*- C++ -*-===//
//
// This file implements clang-refold's accepted-result proof lattice.  The code
// here is intentionally side-effect-free with respect to source emission: it
// classifies proof summaries, normalizes emitted theorem carriers, ranks
// accepted candidates, and builds proof/audit carriers.  RefoldEngine remains
// responsible for deciding which edits to attempt and for emitting source text.
//
//===----------------------------------------------------------------------===//

#include "RefoldProofLattice.h"

using namespace llvm;

namespace clang {
namespace refold {

RefoldEngine::AcceptancePathInventory
RefoldEngine::InventoryMacroPatchProofAcceptancePath(
    const MacroPatchProof &proof) const {
  switch (proof.kind) {
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
    // surfaces. The builder already enforces that; the proof record makes the
    // requirement explicit.
    return BuildAcceptancePathInventory(
        AcceptedPathKind::MacroArgsOnlyPairedPureInsertion);
  case MacroPatchProofKind::PasteDerivedCalleeSelector:
    return BuildAcceptancePathInventory(
        AcceptedPathKind::MacroPasteDerivedCalleeSelector);
  case MacroPatchProofKind::DagSubtreeRoot:
    // DAG-preserving rewrites must carry the explicit subtree certificate
    // recorded on accepted root patches.
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
RefoldEngine::InventoryMacroPatchAcceptancePath(const MacroPatch &patch) const {
  // Keep the full-patch overload as a convenience shim only. The classification
  // decision itself is made from MacroPatchProof so the mapping has one source
  // of truth and cannot drift from the theorem-facing carrier.
  return InventoryMacroPatchProofAcceptancePath(patch.proof);
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
  case AcceptedPathKind::MacroPasteDerivedCalleeSelector:
    inventory.support = AcceptanceSupportKind::ExplicitProofBacked;
    inventory.futureTarget = FutureProofTarget::MacroPasteDerivedCalleeSelector;
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
    // Deterministic include-preserving materialization paths
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
    // This first-class include-realization path exists only when the include
    // cover carries a
    // canonical or deterministic-consensus B-envelope witness.
    inventory.support = AcceptanceSupportKind::ExplicitProofBacked;
    inventory.futureTarget = FutureProofTarget::IncludeRealizationCover;
    break;
  case AcceptedPathKind::IncludeMaterializedExpansion:
    // Recursively materialized include expansions must reach emission through
    // an explicit normalized carrier whose discharge record is authoritative at
    // the emission boundary.
    inventory.support = AcceptanceSupportKind::ExplicitProofBacked;
    inventory.futureTarget =
        FutureProofTarget::IncludeMaterializedExpansionRealization;
    break;
  case AcceptedPathKind::TUExactSlotBoundary:
    // Exact slot anchors are first-class TU anchor proofs.
    inventory.support = AcceptanceSupportKind::ExplicitProofBacked;
    inventory.futureTarget = FutureProofTarget::TUExactSlotAnchor;
    break;
  case AcceptedPathKind::TUProvableInsertionAnchor:
    // Deterministic non-slot TU insertion anchors become explicit proof-backed
    // paths once they carry a local witness.
    inventory.support = AcceptanceSupportKind::ExplicitProofBacked;
    inventory.futureTarget = FutureProofTarget::TUProvableInsertionAnchor;
    break;
  case AcceptedPathKind::TUByteSpanMappedEdit:
  case AcceptedPathKind::TUByteSpanConservativeEdit:
    // Direct TU byte edits participate as explicit proof-backed TU textual
    // realizations once they carry normalized carriers whose discharge is
    // enforced at the byte-edit emission boundary.
    inventory.support = AcceptanceSupportKind::ExplicitProofBacked;
    inventory.futureTarget = FutureProofTarget::TUByteSpanTextualEdit;
    break;
  case AcceptedPathKind::TUIncludeClosureEdit:
    // This is not a legacy unresolved-hunk fallback.  It is a declared TU
    // textual realization whose local builder has proved a
    // closed source interval spanning top-level include directives and any
    // adjacent TU material consumed by the same hunk.  Keep it separate from
    // ordinary byte-span edits so audit output can distinguish the hybrid
    // include-closure theorem from a direct TU span mapping.
    inventory.support = AcceptanceSupportKind::ExplicitProofBacked;
    inventory.futureTarget = FutureProofTarget::TUIncludeClosureEdit;
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

RefoldEngine::TheoremProofClass
RefoldEngine::BuildTheoremProofClassForAcceptedPath(
    AcceptedPathKind currentPath) const {
  // centralizes the one-way bridge from construction provenance to
  // theorem authority. Builders may still remember which implementation path
  // produced a candidate, but after this point selectors and emitters consume
  // only ProofSummary::theoremClass / ProofSummary::emittedProof.
  switch (currentPath) {
  case AcceptedPathKind::MacroArgsOnlyStandard:
  case AcceptedPathKind::MacroArgsOnlyPasteSingle:
  case AcceptedPathKind::MacroArgsOnlyPasteMulti:
  case AcceptedPathKind::MacroArgsOnlyPurePasteOnly:
  case AcceptedPathKind::MacroArgsOnlyPairedPureInsertion:
  case AcceptedPathKind::MacroPasteDerivedCalleeSelector:
  case AcceptedPathKind::MacroDagSubtreeRoot:
  case AcceptedPathKind::MacroCallChainSuffix:
    return TheoremProofClass::InvocationPreservingProof;

  case AcceptedPathKind::MacroCounterLiteral:
    return TheoremProofClass::SuffixStabilizationProof;

  case AcceptedPathKind::MacroWholeCoverRealization:
  case AcceptedPathKind::IncludeRealizationInlineFromB:
  case AcceptedPathKind::IncludeMaterializedExpansion:
  case AcceptedPathKind::TUByteSpanMappedEdit:
  case AcceptedPathKind::TUByteSpanConservativeEdit:
  case AcceptedPathKind::TUIncludeClosureEdit:
    return TheoremProofClass::OwnerRealizationProof;

  case AcceptedPathKind::IncludeDeleteReplaceMappedHeaderTokens:
  case AcceptedPathKind::IncludeInsertSelectedConditionalBoundary:
  case AcceptedPathKind::IncludeInsertChildBoundary:
  case AcceptedPathKind::IncludeInsertRightNeighborPP:
  case AcceptedPathKind::IncludeInsertLeftNeighborPP:
  case AcceptedPathKind::IncludeInsertDeclBoundary:
  case AcceptedPathKind::TUExactSlotBoundary:
  case AcceptedPathKind::TUProvableInsertionAnchor:
    return TheoremProofClass::DirectivePreservingProof;

  case AcceptedPathKind::TerminalEmitEditedPreprocessedStream:
    return TheoremProofClass::TerminalOutOfDomainProof;

  case AcceptedPathKind::Unknown:
  case AcceptedPathKind::IncludePatchPendingMaterialization:
    return TheoremProofClass::Unknown;
  }

  return TheoremProofClass::Unknown;
}

/// \brief Accumulator implementation for class-local obligations.
///
/// The helper is defined out of line so RefoldEngine.cpp can reuse one piece
/// of deterministic bookkeeping across macro, include, and TU proof families
/// without exposing the discharge mechanics outside RefoldEngine.
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

void RefoldEngine::RequireAcceptedPathBaseline(
    ProofDischargeAccumulator &discharge,
    const AcceptancePathInventory &inventory) const {
  discharge.Require(inventory.currentPath != AcceptedPathKind::Unknown,
                    ProofObligationKind::AcceptedPathClassified,
                    ProofFailureReason::MissingAcceptedPathClassification);
  discharge.Require(inventory.futureTarget != FutureProofTarget::Unknown,
                    ProofObligationKind::FutureTargetMapped,
                    ProofFailureReason::MissingFutureTargetMapping);
}

RefoldEngine::ProofDischargeRecord
RefoldEngine::BuildAcceptedPathBaselineDischarge(
    const AcceptancePathInventory &inventory, bool explicitOutOfDomain) const {
  ProofDischargeAccumulator discharge;
  RequireAcceptedPathBaseline(discharge, inventory);
  if (explicitOutOfDomain) {
    discharge.Fail(ProofObligationKind::ExplicitOutOfDomainResultTracked,
                   ProofFailureReason::ExplicitOutOfDomainResult);
  }
  return discharge.Finish();
}

void RefoldEngine::ConfigureProofSummary(
    ProofSummary &summary, TheoremProofClass theoremClass,
    AcceptedProofClass acceptedClass, RealizationMode realizationMode,
    SelectionPreference preference,
    SurfaceDisposition surfaceDisposition, bool structurePreserving) const {
  summary.theoremClass = theoremClass;
  summary.acceptedClass = acceptedClass;
  summary.realizationMode = realizationMode;
  summary.preference = preference;
  summary.surfaceDisposition = surfaceDisposition;
  summary.structurePreserving = structurePreserving;

  // A configured theorem-facing summary must name its final theorem class
  // directly. AcceptedProofClass is now construction provenance only; this flag
  // therefore tracks whether the builder declared a final TheoremProofClass,
  // not whether it stamped an implementation-local accepted class.
  summary.primaryProofClassExplicit = theoremClass != TheoremProofClass::Unknown;
}

bool RefoldEngine::ProofSummaryRequiresOwnerRealizationWitness(
    const ProofSummary &summary) const {
  // audit guard: these accepted paths are no longer independent
  // proof families.  Their theorem-facing proof is the generic
  // OwnerRealizationProof, represented concretely by OwnerRealizationWitness.
  // Keep this list path-specific so unrelated realization classes, such as the
  // counter-literal stabilization path, are not over-constrained.
  switch (summary.inventory.currentPath) {
  case AcceptedPathKind::MacroWholeCoverRealization:
  case AcceptedPathKind::IncludeRealizationInlineFromB:
  case AcceptedPathKind::IncludeMaterializedExpansion:
  case AcceptedPathKind::TUByteSpanMappedEdit:
  case AcceptedPathKind::TUByteSpanConservativeEdit:
  case AcceptedPathKind::TUIncludeClosureEdit:
    return true;

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
  case AcceptedPathKind::IncludePatchPendingMaterialization:
  case AcceptedPathKind::IncludeDeleteReplaceMappedHeaderTokens:
  case AcceptedPathKind::IncludeInsertSelectedConditionalBoundary:
  case AcceptedPathKind::IncludeInsertChildBoundary:
  case AcceptedPathKind::IncludeInsertRightNeighborPP:
  case AcceptedPathKind::IncludeInsertLeftNeighborPP:
  case AcceptedPathKind::IncludeInsertDeclBoundary:
  case AcceptedPathKind::TUExactSlotBoundary:
  case AcceptedPathKind::TUProvableInsertionAnchor:
  case AcceptedPathKind::TerminalEmitEditedPreprocessedStream:
    return false;
  }
  return false;
}

void RefoldEngine::FinalizeProofSummary(ProofSummary &summary) const {
  const bool theoremFacing =
      summary.inventory.currentPath != AcceptedPathKind::Unknown;
  const bool terminalOutOfDomainPath =
      summary.inventory.currentPath ==
      AcceptedPathKind::TerminalEmitEditedPreprocessedStream;

  // proof closure: a normalized emitted-result carrier may not reach
  // the theorem boundary with an implicit or missing primary proof class.  This
  // check is intentionally local and monotone: it does not choose a different
  // candidate, it merely marks the current summary as locally undischarged so
  // the existing strict-mode emission audit can fail closed to the explicit
  // terminal result.
  if (theoremFacing) {
    const bool hasDeclaredPrimaryClass =
        summary.theoremClass != TheoremProofClass::Unknown &&
        summary.primaryProofClassExplicit;
    const bool terminalClassMatches =
        !terminalOutOfDomainPath ||
        summary.theoremClass == TheoremProofClass::TerminalOutOfDomainProof;
    if (!hasDeclaredPrimaryClass || !terminalClassMatches) {
      ++summary.discharge.obligationsEvaluated;
      if (summary.discharge.failedObligation == ProofObligationKind::Unknown) {
        summary.discharge.failedObligation =
            ProofObligationKind::PrimaryProofClassDeclared;
      }
      if (summary.discharge.failureReason == ProofFailureReason::None)
        summary.discharge.failureReason =
            ProofFailureReason::MissingPrimaryProofClass;
      summary.discharge.status = ProofDischargeStatus::Rejected;
    }
  }

  summary.lattice = BuildGlobalSelectionLattice(summary);
  summary.completeness = BuildCompletenessContract(summary);
  summary.theoremDomain = BuildTheoremDomainContract(summary);

  // ProofSummary owns the theorem-facing proof.  The construction
  // inventory above remains useful for diagnostics and path-local builders, but
  // every FinalizeProofSummary() call refreshes this canonical carrier from
  // summary.theoremClass rather than re-normalizing AcceptedProofClass.
  summary.emittedProof = BuildCanonicalEmittedProofFromSummary(summary);
}

void RefoldEngine::RequireIncludeZeroWidthAnchor(
    ProofDischargeAccumulator &discharge, const IncludePatch &patch,
    const IncludeAnchorWitness *witness, IncludeAnchorEvidenceKind evidence,
    ProofObligationKind witnessObligation,
    ProofFailureReason witnessFailure) const {
  discharge.Require(patch.aStart == patch.aEnd,
                    ProofObligationKind::IncludePatchShapeTracked,
                    ProofFailureReason::MissingIncludePatchShape);
  discharge.Require(witness && witness->evidence == evidence, witnessObligation,
                    witnessFailure);
  discharge.Require(witness && witness->hasAnchorByte,
                    ProofObligationKind::IncludeAnchorByteTracked,
                    ProofFailureReason::MissingIncludeAnchorByte);
}

bool RefoldEngine::TUAnchorWitnessHasProvableEvidence(
    const TUAnchorWitness &witness) const {
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

RefoldEngine::ProofSummary
RefoldEngine::ClassifyMacroPatchProof(const MacroPatch &patch) const {
  ProofSummary summary;
  const MacroPatchProof &proof = patch.proof;

  // MacroPatchProof is the sole proof-facing input to classification.  This
  // function deliberately does not read path-local construction metadata for
  // primary proof identity; otherwise old side bits could become theorem
  // authority by another name.
  summary.structurePreserving = proof.preservesInvocationStructure;
  summary.proofRootMacroId = proof.proofRootMacroId;
  if (proof.ownerRealization) {
    summary.hasOwnerRealizationWitness = true;
    summary.ownerRealizationWitness = *proof.ownerRealization;
  }
  if (proof.suffixStability) {
    summary.hasSuffixStabilityWitness = true;
    summary.suffixStabilityWitness = *proof.suffixStability;
  }
  summary.inventory = InventoryMacroPatchProofAcceptancePath(proof);

  switch (proof.kind) {
  case MacroPatchProofKind::ArgsOnlyPasteMulti:
  case MacroPatchProofKind::ArgsOnlyPasteSingle:
  case MacroPatchProofKind::ArgsOnlyPurePasteOnly:
  case MacroPatchProofKind::ArgsOnlyStandard:
  case MacroPatchProofKind::ArgsOnlyPairedPureInsertion:
    // Paired pure insertion is only valid on non-paste direct arg/stringify
    // surfaces. The builder already enforces that; the proof record makes the
    // requirement explicit.
  case MacroPatchProofKind::PasteDerivedCalleeSelector:
    // Paste-derived callee selector substitution is structure-preserving: the
    // emitted edit rewrites only a root invocation argument, after proving that
    // a unique existing pasted callee macro exactly explains the edited B
    // expansion under the same non-selector arguments.
  case MacroPatchProofKind::DagSubtreeRoot:
    // DAG-preserving rewrites must carry the explicit subtree certificate
    // recorded on accepted root patches.
  case MacroPatchProofKind::CallChainSuffix:
    // Call-chain suffix rewrites are emitted directly on the root callsite
    // slice, so the patch's owning macro id must already be that root.
    ConfigureProofSummary(
        summary, BuildTheoremProofClassForAcceptedPath(
                     summary.inventory.currentPath),
        AcceptedProofClass::InvocationPreserving,
        RealizationMode::PreserveOriginalStructure,
        SelectionPreference::PreferStructurePreservation, SurfaceDisposition::None,
                         /*preservesInvocationStructure=*/true);
    break;

  case MacroPatchProofKind::CounterLiteral:
    ConfigureProofSummary(
        summary, BuildTheoremProofClassForAcceptedPath(
                     summary.inventory.currentPath),
        AcceptedProofClass::InvocationRealization,
        RealizationMode::RealizeEditedSurface,
        SelectionPreference::PreferSurfaceRealization, SurfaceDisposition::None,
                         /*preservesInvocationStructure=*/false);
    break;

  case MacroPatchProofKind::WholeCoverRealization:
    ConfigureProofSummary(
        summary, BuildTheoremProofClassForAcceptedPath(
                     summary.inventory.currentPath),
        AcceptedProofClass::InvocationRealization,
        RealizationMode::RealizeEditedSurface,
        SelectionPreference::PreferSurfaceRealization,
        SurfaceDisposition::RealizeWholeCoverMacros,
                         /*preservesInvocationStructure=*/false);
    break;

  case MacroPatchProofKind::Unknown:
    // Unknown macro proof kind is no longer promoted from legacy side bits.
    // A patch that reaches emission with this value lacks the required primary
    // proof-class witness and will be rejected by FinalizeProofSummary().
    break;
  }

  summary.structurePreserving = proof.preservesInvocationStructure;

  switch (summary.theoremClass) {
  case TheoremProofClass::InvocationPreservingProof:
    summary.discharge = ValidateInvocationPreservingProof(patch);
    break;
  case TheoremProofClass::OwnerRealizationProof:
  case TheoremProofClass::SuffixStabilizationProof:
    summary.discharge = ValidateInvocationRealizationProof(patch);
    break;
  case TheoremProofClass::Unknown:
  case TheoremProofClass::IdentityPreservingProof:
    // Identity proofs have no macro-local validator here; they are accepted only
    // through the generic summary/discharge gate below.
  case TheoremProofClass::DirectivePreservingProof:
  case TheoremProofClass::StateRepairProof:
  case TheoremProofClass::MixedOwnerTilingProof:
  case TheoremProofClass::TerminalOutOfDomainProof:
    break;
  }

  FinalizeProofSummary(summary);
  return summary;
}

void RefoldEngine::RefreshMacroPatchDerivedProofWitnesses(
    MacroPatch &patch) const {
  // MacroPatch's old proof mirrors are gone, so this helper is no
  // longer allowed to reconstruct primary proof identity from side fields.  It
  // only enriches the canonical carrier with witnesses whose raw evidence is
  // still stored as patch-local construction metadata: paste replay status, DAG
  // subtree certificates, and root/callsite call-chain identity.
  MacroPatchProof proof = patch.proof;

  const bool proofKindRequiresPasteWitness =
      proof.kind == MacroPatchProofKind::ArgsOnlyPasteSingle ||
      proof.kind == MacroPatchProofKind::ArgsOnlyPasteMulti ||
      proof.kind == MacroPatchProofKind::ArgsOnlyPurePasteOnly;
  if (proofKindRequiresPasteWitness || patch.pasteReplayValidated) {
    PasteWitness witness;
    witness.rootMacroId = proof.proofRootMacroId;
    witness.requiresProducerPasteSpans = proofKindRequiresPasteWitness;
    witness.replayValidated = patch.pasteReplayValidated;
    proof.paste = std::move(witness);
  }

  if (proof.kind == MacroPatchProofKind::DagSubtreeRoot ||
      patch.subtreeCertBacked) {
    SubtreeCertificate cert;
    cert.backed = patch.subtreeCertBacked;
    cert.leafMacroId = patch.subtreeLeafMacroId;
    cert.witnessCount = patch.subtreeWitnessCount;
    cert.invocationCertCount = patch.subtreeInvocationCertCount;
    cert.formalCertCount = patch.subtreeFormalCertCount;
    cert.argCertCount = patch.subtreeArgCertCount;
    cert.liftChainCount = patch.subtreeLiftChainCount;
    cert.liftStepCount = patch.subtreeLiftStepCount;
    cert.rootMergeCount = patch.subtreeRootMergeCount;
    cert.usesLexicalBridge = patch.subtreeUsesLexicalBridge;
    cert.touchesPaste = patch.subtreeTouchesPaste;
    cert.hasWrapperSemantics = patch.subtreeHasWrapperSemantics;
    cert.hasStringifySemantics = patch.subtreeHasStringifySemantics;
    cert.hasWideStringifySemantics = patch.subtreeHasWideStringifySemantics;
    cert.hasPreferredChildSyntax = patch.subtreeHasPreferredChildSyntax;
    cert.hasRawInvocationPreservation =
        patch.subtreeHasRawInvocationPreservation;
    cert.hasPassthroughFlatten = patch.subtreeHasPassthroughFlatten;
    cert.hasBridgeSensitiveStructuredSemantics =
        patch.subtreeHasBridgeSensitiveStructuredSemantics;
    cert.deferredPasteDischarged = patch.subtreeDeferredPasteDischarged;
    cert.admissible = patch.subtreeAdmissible;
    cert.expectedRootFormalCount = patch.subtreeExpectedRootFormalCount;
    cert.deferredRootArgCount = patch.subtreeDeferredRootArgCount;
    cert.bridgeSensitiveFormalCount = patch.subtreeBridgeSensitiveFormalCount;
    cert.expectedRootFormalSummary = patch.subtreeExpectedRootFormalSummary;
    cert.deferredRootArgSummary = patch.subtreeDeferredRootArgSummary;
    cert.bridgeSensitiveFormalSummary =
        patch.subtreeBridgeSensitiveFormalSummary;
    proof.subtree = std::move(cert);
  }

  if (proof.kind == MacroPatchProofKind::CallChainSuffix) {
    CallChainWitness witness;
    witness.rootMacroId = proof.proofRootMacroId;
    witness.callsiteMacroId = patch.macroId;
    proof.callChain = witness;
  }

  patch.proof = std::move(proof);
}

void RefoldEngine::SyncMacroPatchProofSummary(MacroPatch &patch) const {
  RefreshMacroPatchDerivedProofWitnesses(patch);
  patch.proofSummary = ClassifyMacroPatchProof(patch);
}

RefoldEngine::MacroPatchProof RefoldEngine::MakeMacroPatchProof(
    MacroPatchProofKind kind, bool preservesInvocationStructure,
    uint64_t proofRootMacroId) const {
  MacroPatchProof proof;
  proof.kind = kind;
  proof.proofRootMacroId = proofRootMacroId;
  proof.preservesInvocationStructure = preservesInvocationStructure;
  return proof;
}

void RefoldEngine::SetMacroPatchProof(MacroPatch &patch,
                                      MacroPatchProof proof) const {
  // MacroPatchProof is the only MacroPatch-local proof carrier.
  // Install the caller-provided primary proof facts directly, then enrich the
  // carrier with any paste/subtree/call-chain witnesses derived from durable
  // patch metadata before rebuilding the normalized ProofSummary.
  patch.proof = std::move(proof);
  patch.selectedAcceptedCandidate.reset();
  SyncMacroPatchProofSummary(patch);
}

void RefoldEngine::StampSelectedMacroPatchCandidate(
    MacroPatch &patch, const AcceptedResultCandidate &candidate,
    llvm::StringRef role) const {
  // records the accepted-result carrier at the selector boundary.
  // This is report-only/provenance-bearing for now: emission still restamps
  // macro carriers onto the emitted-source discharge rule, but no selected
  // macro result is allowed to survive as only a raw MacroPatch.
  if (candidate.kind != AcceptedResultCandidateKind::MacroPatch) {
    ReportNoLegacyAuditFinding(MakeLegacyAuditEvidence(
        LegacyPathKind::PathSpecificProofMirror, role,
        llvm::formatv(
            "selected macro patch bytes=[{0},{1}) was stamped with "
            "non-macro AcceptedResultCandidate kind={2}",
            patch.invStart, patch.invEnd, candidate.kind)
            .str()));
    return;
  }

  AuditAcceptedResultCandidateForLegacyAuthority(
      candidate, "StampSelectedMacroPatchCandidate");
  patch.selectedAcceptedCandidate = candidate;
}

RefoldEngine::WitnessProofFamily
RefoldEngine::WitnessFamilyForAcceptedPath(AcceptedPathKind path) {
  switch (path) {
  case AcceptedPathKind::MacroArgsOnlyStandard:
  case AcceptedPathKind::MacroArgsOnlyPairedPureInsertion:
    return WitnessProofFamily::MacroActualRepair;
  case AcceptedPathKind::MacroArgsOnlyPasteSingle:
  case AcceptedPathKind::MacroArgsOnlyPasteMulti:
  case AcceptedPathKind::MacroArgsOnlyPurePasteOnly:
  case AcceptedPathKind::MacroPasteDerivedCalleeSelector:
    return WitnessProofFamily::TokenPaste;
  case AcceptedPathKind::MacroDagSubtreeRoot:
  case AcceptedPathKind::MacroCallChainSuffix:
    return WitnessProofFamily::GeneratedCalleeReplay;
  case AcceptedPathKind::MacroCounterLiteral:
    return WitnessProofFamily::CounterState;
  case AcceptedPathKind::MacroWholeCoverRealization:
    return WitnessProofFamily::OwnerRealization;
  case AcceptedPathKind::IncludePatchPendingMaterialization:
  case AcceptedPathKind::IncludeDeleteReplaceMappedHeaderTokens:
  case AcceptedPathKind::IncludeInsertSelectedConditionalBoundary:
  case AcceptedPathKind::IncludeInsertChildBoundary:
  case AcceptedPathKind::IncludeInsertRightNeighborPP:
  case AcceptedPathKind::IncludeInsertLeftNeighborPP:
  case AcceptedPathKind::IncludeInsertDeclBoundary:
    return WitnessProofFamily::IncludePreservation;
  case AcceptedPathKind::IncludeRealizationInlineFromB:
  case AcceptedPathKind::IncludeMaterializedExpansion:
    return WitnessProofFamily::IncludeRealization;
  case AcceptedPathKind::TUExactSlotBoundary:
  case AcceptedPathKind::TUProvableInsertionAnchor:
    return WitnessProofFamily::TUAnchor;
  case AcceptedPathKind::TUByteSpanMappedEdit:
  case AcceptedPathKind::TUByteSpanConservativeEdit:
  case AcceptedPathKind::TUIncludeClosureEdit:
    return WitnessProofFamily::TUTextEdit;
  case AcceptedPathKind::TerminalEmitEditedPreprocessedStream:
    return WitnessProofFamily::TerminalFallback;
  case AcceptedPathKind::Unknown:
    return WitnessProofFamily::Unknown;
  }
  return WitnessProofFamily::Unknown;
}


bool RefoldEngine::IsResolverAuthoritativeWitnessFamily(
    WitnessProofFamily family) {
  switch (family) {
  case WitnessProofFamily::MacroActualRepair:
  case WitnessProofFamily::DefinitionTapeReplay:
  case WitnessProofFamily::GeneratedCalleeReplay:
  case WitnessProofFamily::Stringification:
  case WitnessProofFamily::TokenPaste:
  case WitnessProofFamily::VariadicComma:
  case WitnessProofFamily::ZeroTokenBoundary:
  case WitnessProofFamily::LineControlObserver:
  case WitnessProofFamily::CounterState:
  case WitnessProofFamily::MixedOwnerTiling:
    return true;
  case WitnessProofFamily::Unknown:
  case WitnessProofFamily::AcceptedResult:
  case WitnessProofFamily::IncludePreservation:
  case WitnessProofFamily::IncludeRealization:
  case WitnessProofFamily::TUAnchor:
  case WitnessProofFamily::TUTextEdit:
  case WitnessProofFamily::OwnerRealization:
  case WitnessProofFamily::TerminalFallback:
    return false;
  }
  return false;
}

bool RefoldEngine::IsResolverAuthoritativeWitness(
    const RefoldWitness &witness) {
  if (IsResolverAuthoritativeWitnessFamily(witness.family))
    return true;

  // Macro whole-cover realization now carries the same theorem-facing
  // owner-closure, target-token, suffix-state, observer, counter, diagnostic,
  // and composition keys as the already converted families.  Keep the authority
  // gate witness-specific so include/TU realization carriers remain probe-only
  // until their own families are audited.
  if (witness.family != WitnessProofFamily::OwnerRealization)
    return false;
  if (witness.sourceFamily != "MacroWholeCoverRealization")
    return false;
  if (witness.key.boundaryClass != WitnessBoundaryClass::RootInvocation)
    return false;
  if (witness.key.compositionClass != WitnessCompositionClass::OwnerClosed)
    return false;
  if (witness.key.diagnosticClass !=
      WitnessDiagnosticClass::RealizesEditedSurface)
    return false;

  return true;
}

RefoldEngine::WitnessProducerKind
RefoldEngine::WitnessProducerKindForAcceptedPath(AcceptedPathKind path) {
  switch (path) {
  case AcceptedPathKind::MacroArgsOnlyStandard:
  case AcceptedPathKind::MacroArgsOnlyPairedPureInsertion:
    return WitnessProducerKind::Forward;
  case AcceptedPathKind::MacroArgsOnlyPasteSingle:
  case AcceptedPathKind::MacroArgsOnlyPasteMulti:
  case AcceptedPathKind::MacroArgsOnlyPurePasteOnly:
  case AcceptedPathKind::MacroPasteDerivedCalleeSelector:
    return WitnessProducerKind::PasteResult;
  case AcceptedPathKind::MacroDagSubtreeRoot:
  case AcceptedPathKind::MacroCallChainSuffix:
    return WitnessProducerKind::GeneratedCallee;
  case AcceptedPathKind::MacroCounterLiteral:
    return WitnessProducerKind::BuiltinMaterialization;
  case AcceptedPathKind::IncludePatchPendingMaterialization:
  case AcceptedPathKind::IncludeDeleteReplaceMappedHeaderTokens:
  case AcceptedPathKind::IncludeInsertSelectedConditionalBoundary:
  case AcceptedPathKind::IncludeInsertChildBoundary:
  case AcceptedPathKind::IncludeInsertRightNeighborPP:
  case AcceptedPathKind::IncludeInsertLeftNeighborPP:
  case AcceptedPathKind::IncludeInsertDeclBoundary:
  case AcceptedPathKind::TUExactSlotBoundary:
  case AcceptedPathKind::TUProvableInsertionAnchor:
    return WitnessProducerKind::Forward;
  case AcceptedPathKind::MacroWholeCoverRealization:
  case AcceptedPathKind::IncludeRealizationInlineFromB:
  case AcceptedPathKind::IncludeMaterializedExpansion:
  case AcceptedPathKind::TUByteSpanMappedEdit:
  case AcceptedPathKind::TUByteSpanConservativeEdit:
  case AcceptedPathKind::TUIncludeClosureEdit:
    return WitnessProducerKind::OwnerRealization;
  case AcceptedPathKind::TerminalEmitEditedPreprocessedStream:
    return WitnessProducerKind::TerminalMaterialization;
  case AcceptedPathKind::Unknown:
    return WitnessProducerKind::Unknown;
  }
  return WitnessProducerKind::Unknown;
}

RefoldEngine::WitnessBoundaryClass
RefoldEngine::WitnessBoundaryClassForAcceptedCandidate(
    const AcceptedResultCandidate &candidate) {
  switch (candidate.kind) {
  case AcceptedResultCandidateKind::MacroPatch:
    return WitnessBoundaryClass::RootInvocation;
  case AcceptedResultCandidateKind::IncludePatch:
    return WitnessBoundaryClass::IncludeBoundary;
  case AcceptedResultCandidateKind::TUAnchor:
    return WitnessBoundaryClass::TUAnchorBoundary;
  case AcceptedResultCandidateKind::TUTextEdit:
    return WitnessBoundaryClass::OwnerRealizationBoundary;
  case AcceptedResultCandidateKind::TerminalOutOfDomain:
    return WitnessBoundaryClass::TerminalBoundary;
  case AcceptedResultCandidateKind::Unknown:
    return WitnessBoundaryClass::Unknown;
  }
  return WitnessBoundaryClass::Unknown;
}

std::string RefoldEngine::FormatWitnessTraceHash(StringRef text) {
  // Fixed FNV-1a keeps traces deterministic without relying on
  // implementation-specific pointer identity or process-local hash seeds.
  uint64_t hash = 14695981039346656037ULL;
  for (char ch : text) {
    hash ^= static_cast<unsigned char>(ch);
    hash *= 1099511628211ULL;
  }
  return std::string("0x") + llvm::utohexstr(hash, /*LowerCase=*/true);
}

void RefoldEngine::AttachLineControlObserverWitness(
    AcceptedResultCandidate &candidate) const {
  candidate.hasLineControlObserverWitness = false;
  candidate.lineControlObserverWitness = LineControlObserverWitness{};

  LineControlObserverWitness witness;
  const ProofSummary &summary = candidate.proofSummary;

  auto appendSig = [](std::string &dst, llvm::StringRef part) {
    if (!dst.empty())
      dst += ";";
    dst += part.str();
  };

  auto observeSummary = [&](const OwnerObserverSummary &observers,
                            llvm::StringRef role) {
    if (observers.observesLineNumber) {
      witness.observesLineNumber = true;
      appendSig(witness.observerSignature,
                llvm::formatv("{0}:observes-line", role).str());
    }
    if (observers.observesFileState) {
      witness.observesFileState = true;
      appendSig(witness.observerSignature,
                llvm::formatv("{0}:observes-file", role).str());
    }
    if (observers.observesFileName) {
      witness.observesFileName = true;
      appendSig(witness.observerSignature,
                llvm::formatv("{0}:observes-filename", role).str());
    }
  };

  auto recordLineControlEvent = [&](const LineControlStateIdentity &event,
                                    llvm::StringRef bucket) {
    witness.hasSourceLineControlState = true;
    ++witness.lineControlEventCount;
    if (event.active)
      ++witness.activeLineControlEventCount;
    else
      ++witness.inactiveLineControlEventCount;
    if (event.producerProven)
      ++witness.producerProvenLineControlEventCount;
    if (event.operandProvenance ==
        LineDirectiveOperandProvenance::MissingProducerOperands)
      ++witness.missingOperandLineControlEventCount;
    if (event.operandProvenance == LineDirectiveOperandProvenance::Unknown)
      ++witness.unknownOperandLineControlEventCount;

    // Producer line-control events are source-authored logical-state mutations
    // in the refold map.  Synthetic final-stream directives are represented by
    // final-line-control candidates instead and are not inferred here.
    ++witness.sourceAuthoredLineDirectiveCount;
    appendSig(witness.stateSignature,
              llvm::formatv("{0}:{1}", bucket,
                            FormatLineControlEventForWitness(event))
                  .str());
  };

  auto recordBuiltinObservation = [&](const BuiltinLocationObservation &obs,
                                      llvm::StringRef bucket) {
    witness.hasBuiltinLocationObservers = true;
    ++witness.builtinLocationObservationCount;
    switch (obs.kind) {
    case BuiltinLocationObservationKind::LineState:
      witness.observesLineNumber = true;
      ++witness.builtinLineObservationCount;
      break;
    case BuiltinLocationObservationKind::FileState:
      witness.observesFileState = true;
      ++witness.builtinFileObservationCount;
      break;
    case BuiltinLocationObservationKind::FileNameState:
      witness.observesFileName = true;
      ++witness.builtinFileNameObservationCount;
      break;
    }
    appendSig(witness.observerSignature,
              llvm::formatv("{0}:{1}", bucket,
                            FormatBuiltinLocationObservationForWitness(obs))
                  .str());
  };

  auto collectFacts = [&](const OwnerStateFacts &facts,
                          llvm::StringRef bucket) {
    for (const LineControlStateIdentity &event : facts.lineControlEvents)
      recordLineControlEvent(event, bucket);
    for (const BuiltinLocationObservation &obs :
         facts.builtinLocationObservations)
      recordBuiltinObservation(obs, bucket);
    for (const MissingStateFact &fact : facts.missingStateFacts) {
      if (fact.kind == MissingStateFactKind::MissingLineControlFacts) {
        witness.hasSourceLineControlState = true;
        appendSig(witness.stateSignature,
                  llvm::formatv("{0}:missing-line-control-fact:{1}",
                                bucket, fact.detail)
                      .str());
      }
    }
  };

  auto collectDelta = [&](const OwnerStateDelta &delta, llvm::StringRef role) {
    collectFacts(delta.Entry, llvm::formatv("{0}.entry", role).str());
    collectFacts(delta.Observes, llvm::formatv("{0}.observes", role).str());
    collectFacts(delta.Mutates, llvm::formatv("{0}.mutates", role).str());
    collectFacts(delta.Exit, llvm::formatv("{0}.exit", role).str());
  };

  if (summary.hasOwnerRealizationWitness &&
      summary.ownerRealizationWitness.closure.IsComplete()) {
    const OwnerClosure &closure = summary.ownerRealizationWitness.closure;
    collectDelta(closure.stateIn, "owner.state_in");
    collectDelta(closure.stateOut, "owner.state_out");
    observeSummary(closure.observers, "owner.suffix");
  }

  if (summary.hasMixedOwnerTilingWitness) {
    const MixedOwnerTilingWitness &tiling = summary.mixedOwnerTilingWitness;
    for (const MixedOwnerTilingSegmentWitness &segment : tiling.segments) {
      collectDelta(segment.ownerTransitionProof.before, "mixed.segment.before");
      collectDelta(segment.ownerTransitionProof.after, "mixed.segment.after");
      for (const SuffixStabilityWitness &suffix :
           segment.ownerTransitionProof.suffixWitnesses) {
        const OwnerStateComponent component =
            ComponentNamedBySuffixStabilityWitness(suffix);
        if (!IsLineControlStateComponent(component))
          continue;
        witness.hasSuffixLineControlDischarge = true;
        appendSig(witness.stateSignature,
                  llvm::formatv("mixed.segment.suffix:{0}:component={1}",
                                suffix.kind, component)
                      .str());
        appendSig(
            witness.observerSignature,
            llvm::formatv("mixed.segment.suffix-discharge:{0}:component={1}",
                          suffix.kind, component)
                .str());
      }
    }
  }

  if (summary.hasSuffixStabilityWitness) {
    const SuffixStabilityWitness &suffix = summary.suffixStabilityWitness;
    const OwnerStateComponent component =
        ComponentNamedBySuffixStabilityWitness(suffix);
    if (IsLineControlStateComponent(component)) {
      witness.hasSuffixLineControlDischarge = true;
      switch (component) {
      case OwnerStateComponent::LineNumber:
        witness.observesLineNumber = true;
        break;
      case OwnerStateComponent::FileState:
        witness.observesFileState = true;
        break;
      case OwnerStateComponent::FileName:
        witness.observesFileName = true;
        break;
      case OwnerStateComponent::MacroState:
      case OwnerStateComponent::DefinedOperator:
      case OwnerStateComponent::ConditionalState:
      case OwnerStateComponent::Counter:
      case OwnerStateComponent::PragmaState:
      case OwnerStateComponent::IncludeGuardState:
      case OwnerStateComponent::IncludeState:
      case OwnerStateComponent::UnmodeledState:
      case OwnerStateComponent::Unknown:
        break;
      }
      appendSig(
          witness.stateSignature,
          llvm::formatv("suffix:{0}:component={1}", suffix.kind, component)
              .str());
      appendSig(witness.observerSignature,
                llvm::formatv("suffix-discharge:{0}:component={1}", suffix.kind,
                              component)
                    .str());
    }
  }

  // does not reverse-engineer final #line text from ordinary payloads.
  // It records layout only when an earlier proof has already carried a typed
  // zero-width/layout witness.  Unknown layout is kept witness-specific in the
  // signature so it cannot collapse unrelated line-control candidates.
  if (candidate.hasZeroTokenBoundaryWitness) {
    witness.physicalLayoutKnown = true;
    witness.physicalLayoutStable = candidate.zeroTokenLayoutStable;
    if (candidate.zeroTokenFromIncludeBoundary)
      witness.includeReturnResyncCount += 1;
    if (candidate.zeroTokenFromDirectiveLayoutGap)
      witness.syntheticResyncCount += 1;
  }

  if (witness.stateSignature.empty())
    witness.stateSignature = "none";
  if (witness.observerSignature.empty())
    witness.observerSignature = "none";

  if (witness.physicalLayoutKnown) {
    witness.layoutSignature =
        llvm::formatv("layout:known:stable={0}:include_return={1}:"
                      "synthetic_resync={2}",
                      witness.physicalLayoutStable ? 1 : 0,
                      witness.includeReturnResyncCount,
                      witness.syntheticResyncCount)
            .str();
  } else {
    witness.layoutSignature =
        llvm::formatv("layout:unknown:candidate=[{0},{1})", candidate.begin,
                      candidate.end)
            .str();
  }

  if (witness.Empty())
    return;

  candidate.hasLineControlObserverWitness = true;
  candidate.lineControlObserverWitness = std::move(witness);
}

void RefoldEngine::AttachCounterStateWitness(
    AcceptedResultCandidate &candidate) const {
  CounterStateWitness witness = candidate.hasCounterStateWitness
                                    ? candidate.counterStateWitness
                                    : CounterStateWitness{};
  candidate.hasCounterStateWitness = false;
  candidate.counterStateWitness = CounterStateWitness{};

  const ProofSummary &summary = candidate.proofSummary;

  auto appendSig = [](std::string &dst, llvm::StringRef part) {
    if (!dst.empty())
      dst += ";";
    dst += part.str();
  };

  std::vector<CounterEventIdentity> orderedEvents;
  auto rememberEvent = [&](const CounterEventIdentity &event) {
    if (llvm::none_of(orderedEvents, [&](const CounterEventIdentity &existing) {
          return existing == event;
        }))
      orderedEvents.push_back(event);
  };

  auto recordCounterEvent = [&](const CounterEventIdentity &event,
                                llvm::StringRef bucket, bool observation,
                                bool mutation) {
    witness.hasCounterEvents = true;
    rememberEvent(event);
    if (observation)
      ++witness.counterObservationCount;
    if (mutation)
      ++witness.counterMutationCount;
    if (event.expectedBValue) {
      witness.hasExpectedBValues = true;
      ++witness.expectedBValueCount;
      appendSig(witness.suffixValueSignature,
                llvm::formatv("{0}:expected={1}", bucket,
                              FormatWitnessTraceHash(*event.expectedBValue))
                    .str());
    } else {
      witness.hasMissingExpectedBValues = true;
      ++witness.missingExpectedBValueCount;
    }
    appendSig(witness.consumptionSignature,
              llvm::formatv("{0}:{1}", bucket,
                            FormatCounterEventForWitness(event))
                  .str());
  };

  auto collectFacts = [&](const OwnerStateFacts &facts, llvm::StringRef bucket,
                          bool observation, bool mutation) {
    for (const CounterEventIdentity &event : facts.counterEvents)
      recordCounterEvent(event, bucket, observation, mutation);
    for (const MissingStateFact &fact : facts.missingStateFacts) {
      if (fact.kind == MissingStateFactKind::MissingCounterFacts) {
        witness.hasMissingExpectedBValues = true;
        appendSig(witness.suffixObserverSignature,
                  llvm::formatv("{0}:missing-counter-facts:{1}", bucket,
                                FormatWitnessTraceHash(fact.detail))
                      .str());
      }
    }
  };

  auto collectDelta = [&](const OwnerStateDelta &delta,
                          llvm::StringRef bucket) {
    collectFacts(delta.Entry, llvm::formatv("{0}.entry", bucket).str(),
                 /*observation=*/true, /*mutation=*/false);
    collectFacts(delta.Observes, llvm::formatv("{0}.observes", bucket).str(),
                 /*observation=*/true, /*mutation=*/false);
    collectFacts(delta.Mutates, llvm::formatv("{0}.mutates", bucket).str(),
                 /*observation=*/false, /*mutation=*/true);
    collectFacts(delta.Exit, llvm::formatv("{0}.exit", bucket).str(),
                 /*observation=*/false, /*mutation=*/true);
  };

  auto recordSuffixWitness = [&](const SuffixStabilityWitness &suffix,
                                 llvm::StringRef bucket) {
    if (!SuffixStabilityWitnessNamesComponent(suffix,
                                              OwnerStateComponent::Counter))
      return;

    witness.suffixStateStable =
        suffix.kind != SuffixStabilityWitnessKind::TerminalStateFailure &&
        suffix.kind != SuffixStabilityWitnessKind::None;
    if (suffix.kind == SuffixStabilityWitnessKind::SuffixUnobserved)
      witness.suffixUnobserved = true;
    if (suffix.kind == SuffixStabilityWitnessKind::Literalization)
      witness.literalizationStable = true;
    if (suffix.kind == SuffixStabilityWitnessKind::OwnerMaterialization)
      witness.materializationStable = true;
    if (suffix.kind == SuffixStabilityWitnessKind::ClosureWidening)
      witness.coversAllAffectedObservers = true;

    ++witness.preservedSuffixObserverCount;
    appendSig(witness.suffixObserverSignature,
              llvm::formatv("{0}:suffix={1}:component={2}", bucket, suffix.kind,
                            ComponentNamedBySuffixStabilityWitness(suffix))
                  .str());
  };

  if (summary.hasSuffixStabilityWitness)
    recordSuffixWitness(summary.suffixStabilityWitness, "summary");

  if (summary.hasOwnerRealizationWitness) {
    const OwnerRealizationWitness &owner = summary.ownerRealizationWitness;
    collectDelta(owner.closure.stateIn, "owner.state_in");
    collectDelta(owner.closure.stateOut, "owner.state_out");
    if (owner.closure.observers.observesCounter) {
      witness.observesCounter = true;
      ++witness.preservedSuffixObserverCount;
      appendSig(witness.suffixObserverSignature,
                "owner.closure.observes-counter");
    }
    for (const SuffixStabilityWitness &suffix : owner.stateWitnesses)
      recordSuffixWitness(suffix, "owner.state_witness");
  }

  if (summary.hasMixedOwnerTilingWitness) {
    const MixedOwnerTilingWitness &tiling = summary.mixedOwnerTilingWitness;
    for (const MixedOwnerTilingSegmentWitness &segment : tiling.segments) {
      collectDelta(segment.ownerTransitionProof.before,
                   llvm::formatv("mixed.segment{0}.before",
                                 segment.segmentIndex)
                       .str());
      collectDelta(segment.ownerTransitionProof.after,
                   llvm::formatv("mixed.segment{0}.after",
                                 segment.segmentIndex)
                       .str());
      for (const SuffixStabilityWitness &suffix :
           segment.ownerTransitionProof.suffixWitnesses)
        recordSuffixWitness(
            suffix,
            llvm::formatv("mixed.segment{0}.suffix", segment.segmentIndex)
                .str());
    }
  }

  if (candidate.proofSummary.inventory.currentPath ==
      AcceptedPathKind::MacroCounterLiteral) {
    witness.suffixStateStable =
        witness.suffixStateStable || summary.hasSuffixStabilityWitness;
    if (summary.hasSuffixStabilityWitness) {
      witness.literalizationStable =
          witness.literalizationStable ||
          summary.suffixStabilityWitness.kind ==
              SuffixStabilityWitnessKind::Literalization;
      witness.materializationStable =
          witness.materializationStable ||
          summary.suffixStabilityWitness.kind ==
              SuffixStabilityWitnessKind::OwnerMaterialization;
    }
    appendSig(witness.suffixObserverSignature, "macro-counter-literal-path");
  }

  if (candidate.hasZeroTokenBoundaryWitness && candidate.zeroTokenCounterStable)
    appendSig(witness.suffixObserverSignature, "zero-token-counter-stable");

  if (!orderedEvents.empty()) {
    witness.counterConsumptionCount =
        static_cast<uint32_t>(orderedEvents.size());
    witness.counterOrderKnown = true;
    for (const CounterEventIdentity &event : orderedEvents)
      appendSig(witness.orderSignature,
                llvm::formatv("ordinal={0}:macro={1}:A=[{2},{3})",
                              event.occurrenceOrdinal, event.macroInvocationId,
                              event.aTokenBegin, event.aTokenEnd)
                    .str());
  } else if (!witness.orderSignature.empty()) {
    witness.counterOrderKnown = true;
  }

  // A suffix value is known only when every recorded event carries an expected
  // B value.  Absence of typed counter events means the candidate may still be
  // counter-stable through an existing suffix witness, but it must not merge
  // with a value-specific counter witness.
  if (witness.hasCounterEvents)
    witness.hasMissingExpectedBValues =
        witness.hasMissingExpectedBValues ||
        witness.expectedBValueCount != witness.counterConsumptionCount;

  if (witness.suffixStateStable && !witness.observesCounter &&
      witness.preservedSuffixObserverCount == 0)
    witness.suffixUnobserved = true;

  if (witness.Empty())
    return;

  candidate.hasCounterStateWitness = true;
  candidate.counterStateWitness = std::move(witness);
}

RefoldEngine::WitnessEquivalenceKey RefoldEngine::BuildWitnessEquivalenceKey(
    const AcceptedResultCandidate &candidate) const {
  WitnessEquivalenceKey key;
  const ProofSummary &summary = candidate.proofSummary;
  const AcceptedPathKind path = summary.inventory.currentPath;

  auto knownHash = [&](llvm::StringRef label, llvm::StringRef text) {
    return WitnessEquivalenceDimension::Known(
        llvm::formatv("{0}:{1}", label, FormatWitnessTraceHash(text)).str());
  };

  auto knownRangeHash = [&](llvm::StringRef label, uint64_t begin,
                            uint64_t end) {
    return WitnessEquivalenceDimension::Known(
        llvm::formatv("{0}:[{1},{2}):{3}", label, begin, end,
                      FormatWitnessTraceHash(SliceBSource(begin, end)))
            .str());
  };

  auto pathIsMacroRepairReplayStateNeutralCandidate =
      [](AcceptedPathKind path) {
        switch (path) {
        case AcceptedPathKind::MacroArgsOnlyStandard:
        case AcceptedPathKind::MacroDagSubtreeRoot:
        case AcceptedPathKind::MacroCallChainSuffix:
        case AcceptedPathKind::MacroPasteDerivedCalleeSelector:
          return true;
        case AcceptedPathKind::Unknown:
        case AcceptedPathKind::MacroArgsOnlyPasteSingle:
        case AcceptedPathKind::MacroArgsOnlyPasteMulti:
        case AcceptedPathKind::MacroArgsOnlyPurePasteOnly:
        case AcceptedPathKind::MacroArgsOnlyPairedPureInsertion:
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
        case AcceptedPathKind::TUExactSlotBoundary:
        case AcceptedPathKind::TUProvableInsertionAnchor:
        case AcceptedPathKind::TUByteSpanMappedEdit:
        case AcceptedPathKind::TUByteSpanConservativeEdit:
        case AcceptedPathKind::TUIncludeClosureEdit:
        case AcceptedPathKind::TerminalEmitEditedPreprocessedStream:
          return false;
        }
        return false;
      };

  auto counterWitnessProvesSuffixCounterStability = [&]() {
    if (!candidate.hasCounterStateWitness)
      return false;

    const CounterStateWitness &counter = candidate.counterStateWitness;
    return counter.suffixStateStable && counter.counterOrderKnown &&
           !counter.hasMissingExpectedBValues;
  };

  auto lineControlWitnessIsInvocationNeutral = [&]() {
    if (!candidate.hasLineControlObserverWitness)
      return true;

    const LineControlObserverWitness &line =
        candidate.lineControlObserverWitness;

    // Macro repair/replay witnesses may carry observer/layout neutrality for
    // invocation-preserving macro repairs, but it must not silently discharge
    // source-authored #line mutations, producer-emitted resyncs, or unknown
    // directive operands. Those remain line-control obligations.
    // Builtin-location observations are allowed because they read the
    // already-proven invocation location; they do not mutate suffix
    // preprocessing state.
    if (line.hasSourceLineControlState || line.lineControlEventCount != 0 ||
        line.sourceAuthoredLineDirectiveCount != 0 ||
        line.producerEmittedLineDirectiveCount != 0 ||
        line.includeReturnResyncCount != 0 || line.syntheticResyncCount != 0 ||
        line.missingOperandLineControlEventCount != 0 ||
        line.unknownOperandLineControlEventCount != 0)
      return false;

    if (line.physicalLayoutKnown && !line.physicalLayoutStable)
      return false;

    return true;
  };

  auto canUseMacroRepairReplayStateNeutrality = [&]() {
    if (!pathIsMacroRepairReplayStateNeutralCandidate(path))
      return false;
    if (candidate.kind != AcceptedResultCandidateKind::MacroPatch)
      return false;
    if (summary.theoremClass != TheoremProofClass::InvocationPreservingProof ||
        summary.realizationMode != RealizationMode::PreserveOriginalStructure ||
        !summary.structurePreserving)
      return false;
    // Nested macro selector candidates may intentionally carry the one local
    // discharge failure that final emission later restamps away: the proof root
    // is producer-proven but not top-level.  That failure is not a
    // suffix-state, observer, diagnostic, or layout instability; it only says
    // this candidate is selector-local.  Allow the neutral state key for that
    // exact selector certificate while continuing to reject every other
    // undischarged proof.
    const bool proofIsLocallyDischarged =
        summary.discharge.status == ProofDischargeStatus::Discharged;
    const bool proofIsSelectorOnlyNestedMacro =
        AcceptedResultCandidateHasOnlyNonTopLevelMacroSelectorFailure(
            candidate);
    if (!proofIsLocallyDischarged && !proofIsSelectorOnlyNestedMacro)
      return false;
    if (!candidate.hasTargetBTokenRange ||
        candidate.targetBTokStart > candidate.targetBTokEnd ||
        candidate.targetBTokEnd > bToks_.size())
      return false;
    if (!counterWitnessProvesSuffixCounterStability())
      return false;
    if (!lineControlWitnessIsInvocationNeutral())
      return false;

    return true;
  };

  auto macroRepairReplayStateNeutralityValue = [&]() {
    const uint64_t rootId = candidate.hasRootMacroId ? candidate.rootMacroId
                                                     : summary.proofRootMacroId;
    const CounterStateWitness &counter = candidate.counterStateWitness;
    return llvm::formatv(
               "macro_repair_replay_neutral:path={0}:root={1}:"
               "target=[{2},{3}):target_hash={4}:"
               "counter_suffix={5}:counter_order={6}:"
               "counter_events={7}:counter_suffix_observers={8}:"
               "line_control_neutral={9}:diagnostics=preserve-invocation",
               path, rootId, candidate.targetBTokStart, candidate.targetBTokEnd,
               FormatWitnessTraceHash(SliceBSource(candidate.targetBTokStart,
                                                   candidate.targetBTokEnd)),
               counter.suffixStateStable ? 1 : 0,
               counter.counterOrderKnown ? 1 : 0,
               counter.counterConsumptionCount,
               counter.preservedSuffixObserverCount,
               lineControlWitnessIsInvocationNeutral() ? 1 : 0)
        .str();
  };

  // Whole-cover macro realization often observes macro-definition state
  // without mutating it.  Observing the definition used for expansion is a
  // producer/provenance fact, not a suffix-state change: if every carried state
  // fact is a known macro observation/requirement and neither the entry nor
  // exit delta mutates any state, the suffix state is KnownEqual for the
  // resolver. Keep the exact producer identities in the key so two
  // observation-only witnesses with different macro-definition evidence do not
  // collapse through an undifferentiated "state neutral" bucket.
  auto formatOptionalU64 = [](std::optional<uint64_t> value) {
    return value ? llvm::formatv("{0}", *value).str() : std::string("none");
  };
  auto formatOptionalU32 = [](std::optional<uint32_t> value) {
    return value ? llvm::formatv("{0}", *value).str() : std::string("none");
  };
  auto appendMacroIdentity = [&](llvm::raw_ostream &os, llvm::StringRef label,
                                 const MacroStateIdentity &identity) {
    os << label << "{name=" << identity.macroName
       << ":def=" << formatOptionalU64(identity.definitionDirectiveId)
       << ":undef=" << formatOptionalU64(identity.undefDirectiveId)
       << ":function_like=" << (identity.functionLike ? 1 : 0)
       << ":arity=" << formatOptionalU32(identity.arity)
       << ":variadic=" << (identity.variadic ? 1 : 0) << ":replacement="
       << FormatWitnessTraceHash(identity.replacementTokenHash) << "};";
  };
  auto appendMacroObservation = [&](llvm::raw_ostream &os,
                                    llvm::StringRef label,
                                    const MacroStateObservation &observation) {
    os << label << "{kind=" << toString(observation.kind) << ':';
    appendMacroIdentity(os, "identity=", observation.identity);
    os << "};";
  };
  auto factsAreKnownMacroObservationOnly = [](const OwnerStateFacts &facts) {
    return !facts.HasMissingStateFacts() && !facts.HasMacroDefinitions() &&
           !facts.HasMacroUndefinitions() && !facts.HasLineControlEvents() &&
           !facts.HasBuiltinLocationObservations() &&
           !facts.HasCounterEvents() && !facts.HasPragmaStateEvents() &&
           !facts.HasIncludeStateEvents() &&
           !facts.HasIncludeGuardStateEvents() &&
           !facts.HasConditionalStateEvents();
  };
  auto factsAreKnownBuiltinLocationObservationOnly =
      [](const OwnerStateFacts &facts) {
        // Builtin-location observations read line/file/file-name state but do
        // not mutate preprocessing state.  They may coexist with
        // producer-proven macro observations required to explain the macro
        // expansion that produced the builtin, but they must not be used to
        // discharge source-authored #line mutations, counters,
        // include/conditional/pragma effects, or any missing producer fact.
        return !facts.HasMissingStateFacts() && !facts.HasMacroDefinitions() &&
               !facts.HasMacroUndefinitions() &&
               !facts.HasLineControlEvents() && !facts.HasCounterEvents() &&
               !facts.HasPragmaStateEvents() &&
               !facts.HasIncludeStateEvents() &&
               !facts.HasIncludeGuardStateEvents() &&
               !facts.HasConditionalStateEvents();
      };
  auto factsHaveBuiltinLocationObservation = [](const OwnerStateFacts &facts) {
    return facts.HasBuiltinLocationObservations();
  };
  auto appendMacroObservationBucket = [&](llvm::raw_ostream &os,
                                          llvm::StringRef bucket,
                                          const OwnerStateFacts &facts) {
    for (const MacroStateIdentity &identity : facts.macroRequirements)
      appendMacroIdentity(os, llvm::formatv("{0}.requirement=", bucket).str(),
                          identity);
    for (const MacroStateObservation &observation :
         facts.macroExpansionObservations)
      appendMacroObservation(os, llvm::formatv("{0}.expansion=", bucket).str(),
                             observation);
    for (const MacroStateObservation &observation :
         facts.definedOperatorObservations)
      appendMacroObservation(os, llvm::formatv("{0}.defined=", bucket).str(),
                             observation);
    for (const MacroStateObservation &observation :
         facts.conditionalMacroObservations)
      appendMacroObservation(
          os, llvm::formatv("{0}.conditional=", bucket).str(), observation);
  };

  auto formatOptionalString = [](const std::optional<std::string> &value) {
    return value ? *value : std::string("none");
  };

  auto appendIncludeIdentity = [&](llvm::raw_ostream &os, llvm::StringRef label,
                                   const IncludeStateIdentity &identity) {
    os << label << "{id=" << identity.includeId
       << ":kind=" << identity.directiveKind << ":site=" << identity.sitePath
       << '[' << identity.siteBegin << ',' << identity.siteEnd << ")"
       << ":target=" << identity.target
       << ":resolved=" << formatOptionalString(identity.resolvedPath)
       << ":angled=" << (identity.angled ? 1 : 0)
       << ":parent=" << formatOptionalU64(identity.parentIncludeId)
       << ":tokens=" << (identity.hasTokenMaterialization ? 1 : 0) << "};";
  };

  auto appendIncludeGuardIdentity =
      [&](llvm::raw_ostream &os, llvm::StringRef label,
          const IncludeGuardStateIdentity &identity) {
        os << label << "{kind=" << toString(identity.kind)
           << ":include=" << identity.includeId
           << ":header=" << identity.headerPath
           << ":guard=" << formatOptionalString(identity.guardMacroName)
           << ":parent=" << formatOptionalU64(identity.parentIncludeId)
           << ":producer=" << (identity.producerProvenGuard ? 1 : 0) << "};";
      };

  auto appendLineControlIdentity =
      [&](llvm::raw_ostream &os, llvm::StringRef label,
          const LineControlStateIdentity &identity) {
        os << label << "{id=" << identity.eventId
           << ":file=" << identity.physicalFile << ":site=["
           << formatOptionalU64(identity.siteBegin) << ','
           << formatOptionalU64(identity.siteEnd) << ')'
           << ":active=" << (identity.active ? 1 : 0)
           << ":producer=" << (identity.producerProven ? 1 : 0)
           << ":line_after=" << identity.logicalLineAfter
           << ":file_after=" << identity.logicalFileAfter
           << ":owner_include=" << formatOptionalU64(identity.ownerIncludeId)
           << ":operands=" << toString(identity.operandProvenance) << "};";
      };

  auto appendBuiltinLocationObservation =
      [&](llvm::raw_ostream &os, llvm::StringRef label,
          const BuiltinLocationObservation &observation) {
        os << label << "{kind=" << toString(observation.kind)
           << ":owner_include=" << formatOptionalU64(observation.ownerIncludeId)
           << ":source=[" << formatOptionalU64(observation.sourceBegin) << ','
           << formatOptionalU64(observation.sourceEnd) << ')' << ":atok=["
           << formatOptionalU64(observation.aTokenBegin) << ','
           << formatOptionalU64(observation.aTokenEnd) << ")};";
      };

  auto appendCounterIdentity = [&](llvm::raw_ostream &os, llvm::StringRef label,
                                   const CounterEventIdentity &identity) {
    os << label << "{macro=" << identity.macroInvocationId
       << ":ordinal=" << identity.occurrenceOrdinal
       << ":owner_include=" << formatOptionalU64(identity.ownerIncludeId)
       << ":caller=" << formatOptionalU64(identity.callerMacroId) << ":atok=["
       << identity.aTokenBegin << ',' << identity.aTokenEnd << ')'
       << ":site=" << identity.expansionSiteFile << '['
       << formatOptionalU64(identity.expansionSiteBegin) << ','
       << formatOptionalU64(identity.expansionSiteEnd) << ')'
       << ":avalue=" << identity.aValue << ":bvalue="
       << (identity.expectedBValue ? *identity.expectedBValue
                                   : std::string("none"))
       << ":literal=" << (identity.canStabilizeByLiteralization ? 1 : 0)
       << ":materialize=" << (identity.canStabilizeByMaterialization ? 1 : 0)
       << "};";
  };

  auto appendPragmaIdentity = [&](llvm::raw_ostream &os, llvm::StringRef label,
                                  const PragmaStateIdentity &identity) {
    os << label << "{id=" << identity.pragmaId << ":site=" << identity.sitePath
       << '[' << identity.siteBegin << ',' << identity.siteEnd << ')'
       << ":owner_include=" << formatOptionalU64(identity.ownerIncludeId)
       << ":class=" << toString(identity.classification) << ":fingerprint="
       << FormatWitnessTraceHash(identity.directiveFingerprint) << "};";
  };

  auto appendConditionalIdentity =
      [&](llvm::raw_ostream &os, llvm::StringRef label,
          const ConditionalStateIdentity &identity) {
        os << label << "{role=" << toString(identity.role)
           << ":group=" << identity.groupId
           << ":arm=" << formatOptionalU64(identity.armId)
           << ":file=" << identity.file << '[' << identity.groupBegin << ','
           << identity.groupEnd << ')'
           << ":parent_arm=" << formatOptionalU64(identity.parentArmId)
           << ":parent_include=" << formatOptionalU64(identity.parentIncludeId)
           << ":arm_kind=" << identity.armKind
           << ":cond=" << formatOptionalString(identity.conditionText)
           << ":selected=" << (identity.selected ? 1 : 0) << ":atok=["
           << formatOptionalU64(identity.aTokenBegin) << ','
           << formatOptionalU64(identity.aTokenEnd) << ')' << ":producer_truth="
           << (identity.conditionTruthProducerProven ? 1 : 0)
           << ":reverse_solved="
           << (identity.reverseSolvedDirectiveRequired ? 1 : 0) << "};";
      };

  auto appendMissingFact = [&](llvm::raw_ostream &os, llvm::StringRef label,
                               const MissingStateFact &fact) {
    os << label << "{kind=" << toString(fact.kind)
       << ":detail=" << FormatWitnessTraceHash(fact.detail) << "};";
  };

  auto appendFullStateFacts = [&](llvm::raw_ostream &os, llvm::StringRef bucket,
                                  const OwnerStateFacts &facts) {
    for (const MacroStateIdentity &identity : facts.macroDefinitions)
      appendMacroIdentity(os, llvm::formatv("{0}.macro_define=", bucket).str(),
                          identity);
    for (const MacroStateIdentity &identity : facts.macroUndefinitions)
      appendMacroIdentity(os, llvm::formatv("{0}.macro_undef=", bucket).str(),
                          identity);
    appendMacroObservationBucket(os, bucket, facts);
    for (const LineControlStateIdentity &identity : facts.lineControlEvents)
      appendLineControlIdentity(
          os, llvm::formatv("{0}.line_control=", bucket).str(), identity);
    for (const BuiltinLocationObservation &observation :
         facts.builtinLocationObservations)
      appendBuiltinLocationObservation(
          os, llvm::formatv("{0}.builtin=", bucket).str(), observation);
    for (const CounterEventIdentity &identity : facts.counterEvents)
      appendCounterIdentity(os, llvm::formatv("{0}.counter=", bucket).str(),
                            identity);
    for (const PragmaStateIdentity &identity : facts.pragmaStateEvents)
      appendPragmaIdentity(os, llvm::formatv("{0}.pragma=", bucket).str(),
                           identity);
    for (const IncludeStateIdentity &identity : facts.includeStateEvents)
      appendIncludeIdentity(os, llvm::formatv("{0}.include=", bucket).str(),
                            identity);
    for (const IncludeGuardStateIdentity &identity :
         facts.includeGuardStateEvents)
      appendIncludeGuardIdentity(
          os, llvm::formatv("{0}.include_guard=", bucket).str(), identity);
    for (const ConditionalStateIdentity &identity :
         facts.conditionalStateEvents)
      appendConditionalIdentity(
          os, llvm::formatv("{0}.conditional=", bucket).str(), identity);
    for (const MissingStateFact &fact : facts.missingStateFacts)
      appendMissingFact(os, llvm::formatv("{0}.missing=", bucket).str(), fact);
  };

  auto hasIncludeFacts = [](const OwnerStateFacts &facts) {
    return facts.HasIncludeStateEvents() || facts.HasIncludeGuardStateEvents();
  };

  auto hasUnknownIncludeGuardFacts = [](const OwnerStateFacts &facts) {
    return llvm::any_of(
        facts.includeGuardStateEvents,
        [](const IncludeGuardStateIdentity &identity) {
          return identity.kind ==
                 IncludeGuardObservationKind::UnknownGuardEffect;
        });
  };

  auto hasConditionalFacts = [](const OwnerStateFacts &facts) {
    return facts.HasConditionalStateEvents();
  };

  auto hasUnknownConditionalFacts = [](const OwnerStateFacts &facts) {
    return llvm::any_of(facts.conditionalStateEvents,
                        [](const ConditionalStateIdentity &identity) {
                          return !identity.conditionTruthProducerProven ||
                                 identity.reverseSolvedDirectiveRequired;
                        });
  };

  auto factsAreKnownConditionalPathState = [&](const OwnerStateFacts &facts) {
    // Conditional-state proof may carry producer-selected branch events
    // and the macro observations needed to evaluate those conditions.  It must
    // not silently discharge unrelated line/include/counter/pragma effects, and
    // it must reject inactive-arm reverse solving or unproven branch truth.
    return !facts.HasMissingStateFacts() && !facts.HasMacroDefinitions() &&
           !facts.HasMacroUndefinitions() && !facts.HasLineControlEvents() &&
           !facts.HasBuiltinLocationObservations() &&
           !facts.HasCounterEvents() && !facts.HasPragmaStateEvents() &&
           !facts.HasIncludeStateEvents() &&
           !facts.HasIncludeGuardStateEvents() &&
           !hasUnknownConditionalFacts(facts);
  };

  auto deltaHasAnyMissingFacts = [](const OwnerStateDelta &delta) {
    return delta.Entry.HasMissingStateFacts() ||
           delta.Observes.HasMissingStateFacts() ||
           delta.Mutates.HasMissingStateFacts() ||
           delta.Exit.HasMissingStateFacts();
  };

  auto deltaHasIncludeFacts = [&](const OwnerStateDelta &delta) {
    return hasIncludeFacts(delta.Entry) || hasIncludeFacts(delta.Observes) ||
           hasIncludeFacts(delta.Mutates) || hasIncludeFacts(delta.Exit);
  };

  auto deltaHasUnknownIncludeGuardFacts = [&](const OwnerStateDelta &delta) {
    return hasUnknownIncludeGuardFacts(delta.Entry) ||
           hasUnknownIncludeGuardFacts(delta.Observes) ||
           hasUnknownIncludeGuardFacts(delta.Mutates) ||
           hasUnknownIncludeGuardFacts(delta.Exit);
  };

  auto deltaHasConditionalFacts = [&](const OwnerStateDelta &delta) {
    return hasConditionalFacts(delta.Entry) ||
           hasConditionalFacts(delta.Observes) ||
           hasConditionalFacts(delta.Mutates) ||
           hasConditionalFacts(delta.Exit);
  };

  auto deltaHasUnknownConditionalFacts = [&](const OwnerStateDelta &delta) {
    return hasUnknownConditionalFacts(delta.Entry) ||
           hasUnknownConditionalFacts(delta.Observes) ||
           hasUnknownConditionalFacts(delta.Mutates) ||
           hasUnknownConditionalFacts(delta.Exit);
  };

  auto appendStateDeltaSignature = [&](llvm::raw_ostream &os,
                                       llvm::StringRef prefix,
                                       const OwnerStateDelta &delta) {
    appendFullStateFacts(os, llvm::formatv("{0}.entry", prefix).str(),
                         delta.Entry);
    appendFullStateFacts(os, llvm::formatv("{0}.observes", prefix).str(),
                         delta.Observes);
    appendFullStateFacts(os, llvm::formatv("{0}.mutates", prefix).str(),
                         delta.Mutates);
    appendFullStateFacts(os, llvm::formatv("{0}.exit", prefix).str(),
                         delta.Exit);
  };

  auto ownerStateDeltaSignature = [&](const OwnerRealizationWitness &owner) {
    std::string storage;
    llvm::raw_string_ostream os(storage);
    os << "owner=" << toString(owner.closure.owner.kind)
       << ":evidence=" << toString(owner.evidence);
    if (owner.closure.source.HasPath())
      os << ":source=" << owner.closure.source.path << '['
         << owner.closure.source.begin << ',' << owner.closure.source.end
         << ')';
    if (owner.closure.aTokens.IsValid())
      os << ":A=[" << owner.closure.aTokens.begin << ','
         << owner.closure.aTokens.end << ')';
    if (owner.closure.bTokens.IsValid())
      os << ":B=[" << owner.closure.bTokens.begin << ','
         << owner.closure.bTokens.end << ')';
    appendStateDeltaSignature(os, "in", owner.closure.stateIn);
    appendStateDeltaSignature(os, "out", owner.closure.stateOut);
    for (const SuffixStabilityWitness &suffix : owner.stateWitnesses)
      os << "state_witness{" << toString(suffix.kind) << ':'
         << toString(ComponentNamedBySuffixStabilityWitness(suffix)) << "};";
    os.flush();
    return FormatWitnessTraceHash(storage);
  };

  auto mixedOwnerTilingSignature = [&](const MixedOwnerTilingWitness &tiling) {
    std::string storage;
    llvm::raw_string_ostream os(storage);
    os << "tiling=" << tiling.witnessId << ":A=[" << tiling.originalAStart
       << ',' << tiling.originalAEnd << ")"
       << ":B=[" << tiling.originalBStart << ',' << tiling.originalBEnd
       << "):tokens=" << tiling.tokenSegmentCount
       << ":state_gaps=" << tiling.stateGapCount
       << ":state_composed=" << (tiling.stateSummariesComposed ? 1 : 0)
       << ":owners_composed=" << (tiling.ownerBoundariesComposed ? 1 : 0)
       << ":target_composed=" << (tiling.targetTokenStreamComposed ? 1 : 0)
       << ":edges_proven=" << (tiling.compositionEdgesProven ? 1 : 0)
       << ":target=" << tiling.globalTargetPPTokenSignature
       << ":composition=" << tiling.globalCompositionSignature;
    for (const MixedOwnerTilingSegmentWitness &segment : tiling.segments) {
      os << ";segment" << segment.segmentIndex
         << "{kind=" << toString(segment.kind) << ":A=[" << segment.aStart
         << ',' << segment.aEnd << "):B=[" << segment.bStart << ','
         << segment.bEnd << "):zero_gap=" << (segment.zeroTokenStateGap ? 1 : 0)
         << ":empty_b=" << (segment.allowEmptyBEnvelope ? 1 : 0)
         << ":closure=" << (segment.ownerClosureComplete ? 1 : 0)
         << ":owner=" << segment.ownerSignature
         << ":source=" << segment.sourceSignature
         << ":producer=" << segment.producerPathSignature
         << ":target=" << segment.targetPPTokenSignature;
      appendStateDeltaSignature(os, "before",
                                segment.ownerTransitionProof.before);
      appendStateDeltaSignature(os, "after",
                                segment.ownerTransitionProof.after);
      for (const SuffixStabilityWitness &suffix :
           segment.ownerTransitionProof.suffixWitnesses) {
        os << "suffix{" << toString(suffix.kind) << ':'
           << toString(ComponentNamedBySuffixStabilityWitness(suffix)) << "};";
      }
      os << '}';
    }
    os.flush();
    return FormatWitnessTraceHash(storage);
  };

  auto stateFactsHaveCounterProofObligation = [](const OwnerStateFacts &facts) {
    return facts.HasCounterEvents() ||
           facts.HasMissingFactKind(MissingStateFactKind::MissingCounterFacts);
  };

  auto deltaHasCounterProofObligation = [&](const OwnerStateDelta &delta) {
    return stateFactsHaveCounterProofObligation(delta.Entry) ||
           stateFactsHaveCounterProofObligation(delta.Observes) ||
           stateFactsHaveCounterProofObligation(delta.Mutates) ||
           stateFactsHaveCounterProofObligation(delta.Exit);
  };

  auto deltaHasMissingCounterProof = [](const OwnerStateDelta &delta) {
    return delta.Entry.HasMissingFactKind(
               MissingStateFactKind::MissingCounterFacts) ||
           delta.Observes.HasMissingFactKind(
               MissingStateFactKind::MissingCounterFacts) ||
           delta.Mutates.HasMissingFactKind(
               MissingStateFactKind::MissingCounterFacts) ||
           delta.Exit.HasMissingFactKind(
               MissingStateFactKind::MissingCounterFacts);
  };

  auto transitionHasStableCounterWitness =
      [](const StateTransitionProof &proof) {
        return llvm::any_of(
            proof.suffixWitnesses, [](const SuffixStabilityWitness &suffix) {
              return RefoldEngine::SuffixStabilityWitnessNamesComponent(
                         suffix, OwnerStateComponent::Counter) &&
                     suffix.kind != SuffixStabilityWitnessKind::None &&
                     suffix.kind !=
                         SuffixStabilityWitnessKind::TerminalStateFailure;
            });
      };

  auto mixedOwnerTilingHasKnownCounterState =
      [&](const MixedOwnerTilingWitness &tiling) {
        if (!tiling.stateSummariesComposed || tiling.segments.empty())
          return false;

        bool sawCounterObligation = false;
        for (const MixedOwnerTilingSegmentWitness &segment : tiling.segments) {
          const StateTransitionProof &proof = segment.ownerTransitionProof;
          if (deltaHasMissingCounterProof(proof.before) ||
              deltaHasMissingCounterProof(proof.after))
            return false;

          const bool segmentHasCounterObligation =
              deltaHasCounterProofObligation(proof.before) ||
              deltaHasCounterProofObligation(proof.after);
          if (!segmentHasCounterObligation)
            continue;

          sawCounterObligation = true;
          if (!transitionHasStableCounterWitness(proof))
            return false;
        }

        return !sawCounterObligation || tiling.compositionEdgesProven;
      };

  auto ownerRealizationHasKnownIncludeState =
      [&](const OwnerRealizationWitness &owner) {
        const bool includeRealization =
            owner.evidence == OwnerRealizationEvidenceKind::IncludeBEnvelope ||
            owner.evidence ==
                OwnerRealizationEvidenceKind::IncludeMaterializedExpansion;
        if (!includeRealization || !owner.closure.IsComplete())
          return false;

        // Include-state realization is resolver-comparable only when the
        // producer supplied complete include/include-guard facts.  Missing
        // include ordering/guard facts, unknown pragma state, or an unknown
        // include-guard effect remain fail-closed theorem obligations; this
        // deliberately does not weaken the existing pragma/include-gap
        // ExplicitOutOfDomain cases.
        if (deltaHasAnyMissingFacts(owner.closure.stateIn) ||
            deltaHasAnyMissingFacts(owner.closure.stateOut) ||
            OwnerStateDeltaHasUnknownPragmaState(owner.closure.stateIn) ||
            OwnerStateDeltaHasUnknownPragmaState(owner.closure.stateOut) ||
            deltaHasUnknownIncludeGuardFacts(owner.closure.stateIn) ||
            deltaHasUnknownIncludeGuardFacts(owner.closure.stateOut))
          return false;

        return deltaHasIncludeFacts(owner.closure.stateIn) ||
               deltaHasIncludeFacts(owner.closure.stateOut);
      };

  auto ownerRealizationHasKnownConditionalState =
      [&](const OwnerRealizationWitness &owner) {
        if (!owner.closure.IsComplete())
          return false;

        // Active conditional-path state is known only when every carried
        // conditional event was producer-selected/evaluated and no inactive arm
        // is being used as a reverse-solved source repair.  This branch is
        // deliberately limited to conditional + macro-observation facts; line,
        // counter, include, pragma, and directive mutations remain governed by
        // their own component-specific proof families.
        if (deltaHasAnyMissingFacts(owner.closure.stateIn) ||
            deltaHasAnyMissingFacts(owner.closure.stateOut) ||
            deltaHasUnknownConditionalFacts(owner.closure.stateIn) ||
            deltaHasUnknownConditionalFacts(owner.closure.stateOut) ||
            OwnerStateDeltaHasUnknownPragmaState(owner.closure.stateIn) ||
            OwnerStateDeltaHasUnknownPragmaState(owner.closure.stateOut))
          return false;

        return (deltaHasConditionalFacts(owner.closure.stateIn) ||
                deltaHasConditionalFacts(owner.closure.stateOut)) &&
               factsAreKnownConditionalPathState(owner.closure.stateIn.Entry) &&
               factsAreKnownConditionalPathState(
                   owner.closure.stateIn.Observes) &&
               factsAreKnownConditionalPathState(
                   owner.closure.stateIn.Mutates) &&
               factsAreKnownConditionalPathState(owner.closure.stateIn.Exit) &&
               factsAreKnownConditionalPathState(
                   owner.closure.stateOut.Entry) &&
               factsAreKnownConditionalPathState(
                   owner.closure.stateOut.Observes) &&
               factsAreKnownConditionalPathState(
                   owner.closure.stateOut.Mutates) &&
               factsAreKnownConditionalPathState(owner.closure.stateOut.Exit);
      };
  auto ownerRealizationHasKnownMacroObservationOnlyState =
      [&](const OwnerRealizationWitness &owner) {
        const OwnerStateDelta &in = owner.closure.stateIn;
        const OwnerStateDelta &out = owner.closure.stateOut;
        return !OwnerStateDeltaMutatesAnyState(in) &&
               !OwnerStateDeltaMutatesAnyState(out) &&
               factsAreKnownMacroObservationOnly(in.Entry) &&
               factsAreKnownMacroObservationOnly(in.Observes) &&
               in.Mutates.Empty() && in.Exit.Empty() &&
               factsAreKnownMacroObservationOnly(out.Entry) &&
               factsAreKnownMacroObservationOnly(out.Observes) &&
               out.Mutates.Empty() && out.Exit.Empty();
      };
  auto ownerRealizationHasKnownBuiltinLocationObservationState =
      [&](const OwnerRealizationWitness &owner) {
        if (!owner.closure.IsComplete())
          return false;

        const OwnerStateDelta &in = owner.closure.stateIn;
        const OwnerStateDelta &out = owner.closure.stateOut;
        if (OwnerStateDeltaMutatesAnyState(in) ||
            OwnerStateDeltaMutatesAnyState(out))
          return false;

        const bool observesBuiltinLocation =
            factsHaveBuiltinLocationObservation(in.Entry) ||
            factsHaveBuiltinLocationObservation(in.Observes) ||
            factsHaveBuiltinLocationObservation(out.Entry) ||
            factsHaveBuiltinLocationObservation(out.Observes);
        if (!observesBuiltinLocation)
          return false;

        return factsAreKnownBuiltinLocationObservationOnly(in.Entry) &&
               factsAreKnownBuiltinLocationObservationOnly(in.Observes) &&
               in.Mutates.Empty() && in.Exit.Empty() &&
               factsAreKnownBuiltinLocationObservationOnly(out.Entry) &&
               factsAreKnownBuiltinLocationObservationOnly(out.Observes) &&
               out.Mutates.Empty() && out.Exit.Empty();
      };
  auto macroObservationOnlyStateSignature =
      [&](const OwnerRealizationWitness &owner) {
        std::string storage;
        llvm::raw_string_ostream os(storage);
        appendMacroObservationBucket(os, "in.entry",
                                     owner.closure.stateIn.Entry);
        appendMacroObservationBucket(os, "in.observes",
                                     owner.closure.stateIn.Observes);
        appendMacroObservationBucket(os, "out.entry",
                                     owner.closure.stateOut.Entry);
        appendMacroObservationBucket(os, "out.observes",
                                     owner.closure.stateOut.Observes);
        os.flush();
        return FormatWitnessTraceHash(storage);
      };

  // keeps the target-preprocessed-token dimension conservative.  It is
  // known only when an accepted proof already carries a B-token envelope or a
  // zero-token B anchor.  Source-spelling previews are intentionally not
  // treated as token-stream proof, because later resolver passes must not merge
  // two witnesses by comparing edited source bytes in place of produced PP
  // tokens.
  if (candidate.hasTargetBTokenRange &&
      candidate.targetBTokStart <= candidate.targetBTokEnd &&
      candidate.targetBTokEnd <= bToks_.size()) {
    const char *label =
        candidate.hasZeroTokenBoundaryWitness &&
                !candidate.hasGeneratedCalleeReplayWitness &&
                !candidate.hasVariadicCommaWitness &&
                !candidate.hasTokenPasteWitness &&
                !candidate.hasStringificationWitness
            ? "zero_token_b_tokens"
            : (candidate.hasGeneratedCalleeReplayWitness
                   ? "generated_callee_b_tokens"
                   : (candidate.hasVariadicCommaWitness
                          ? "variadic_b_tokens"
                          : (candidate.hasTokenPasteWitness
                                 ? "token_paste_b_tokens"
                                 : (candidate.hasStringificationWitness
                                        ? "stringification_b_tokens"
                                        : (candidate.hasMacroActualRepairWitness
                                               ? "macro_actual_b_tokens"
                                               : "candidate_b_tokens")))));
    key.targetPPTokens = knownRangeHash(label, candidate.targetBTokStart,
                                        candidate.targetBTokEnd);
  } else if (candidate.hasZeroTokenBoundaryWitness &&
             candidate.zeroTokenHasBTokenRange &&
             candidate.zeroTokenBTokStart <= candidate.zeroTokenBTokEnd &&
             candidate.zeroTokenBTokEnd <= bToks_.size()) {
    // Zero-token include/boundary witnesses already carry the exact B-token
    // gap/envelope from the modified preprocessed stream.  Promote that
    // producer-recorded envelope into the target-PP equivalence dimension
    // instead of treating the absence of replacement text as unknown output.
    key.targetPPTokens =
        knownRangeHash("zero_token_b_tokens", candidate.zeroTokenBTokStart,
                       candidate.zeroTokenBTokEnd);
  } else if (summary.hasOwnerRealizationWitness &&
             summary.ownerRealizationWitness.closure.IsComplete()) {
    const OwnerTokenRange bTokens =
        summary.ownerRealizationWitness.closure.bTokens;
    key.targetPPTokens = knownRangeHash("b_tokens", bTokens.begin, bTokens.end);
  } else if (summary.hasMixedOwnerTilingWitness) {
    const MixedOwnerTilingWitness &tiling = summary.mixedOwnerTilingWitness;
    if (tiling.originalBStart <= tiling.originalBEnd)
      key.targetPPTokens = knownRangeHash(
          "mixed_owner_b_tokens", tiling.originalBStart, tiling.originalBEnd);
  } else if (summary.hasIncludeAnchorWitness &&
             summary.includeAnchorWitness.hasFirstPP &&
             summary.includeAnchorWitness.hasLastPP &&
             summary.includeAnchorWitness.firstPP <=
                 summary.includeAnchorWitness.lastPP) {
    key.targetPPTokens = knownRangeHash(
        "include_anchor_b_tokens", summary.includeAnchorWitness.firstPP,
        summary.includeAnchorWitness.lastPP + 1);
  } else if (summary.hasTUAnchorWitness && summary.tuAnchorWitness.hasPPGap) {
    key.targetPPTokens = WitnessEquivalenceDimension::Known(
        llvm::formatv("empty_b_gap:{0}", summary.tuAnchorWitness.ppGap).str());
  } else if (path == AcceptedPathKind::TerminalEmitEditedPreprocessedStream) {
    key.targetPPTokens = knownHash("terminal_full_b", bSource_);
  }

  if (candidate.hasGeneratedCalleeReplayWitness) {
    key.suffixState = WitnessEquivalenceDimension::Known(
        llvm::formatv(
            "generated_callee_replay:root={0}:final_def={1}:depth={2}:"
            "aliases={3}:chain={4}:replay={5}:mapped={6}:"
            "decoded_payload_evidence_only={7}",
            candidate.generatedCalleeRootMacroId,
            candidate.generatedCalleeFinalDirectiveId,
            candidate.generatedCalleeDepth,
            candidate.generatedCalleeObjectAliasHops,
            candidate.generatedCalleeChainDeterministic ? 1 : 0,
            candidate.generatedCalleeReplacementReplayValidated ? 1 : 0,
            candidate.generatedCalleeSolvedActualsMappedToRoot ? 1 : 0,
            candidate.generatedCalleeDecodedStringLiteralEvidenceOnly ? 1 : 0)
            .str());
  } else if (candidate.hasVariadicCommaWitness) {
    key.suffixState = WitnessEquivalenceDimension::Known(
        llvm::formatv("variadic:root={0}:formal={1}:arity={2}:orig_missing={3}:"
                      "orig_empty={4}:orig_nonempty={5}:result_missing={6}:"
                      "result_empty={7}:result_nonempty={8}:comma_inserted={9}:"
                      "comma_deleted={10}:gnu_elision={11}:vaopt={12}:"
                      "vaopt_orig={13}:vaopt_result={14}:vaopt_comma_ins={15}:"
                      "vaopt_comma_del={16}:producer={17}:pack={18}",
                      candidate.variadicRootMacroId,
                      candidate.variadicFormalIndex,
                      candidate.variadicArityStable ? 1 : 0,
                      candidate.variadicOriginalMissing ? 1 : 0,
                      candidate.variadicOriginalExplicitEmpty ? 1 : 0,
                      candidate.variadicOriginalNonEmpty ? 1 : 0,
                      candidate.variadicResultMissing ? 1 : 0,
                      candidate.variadicResultExplicitEmpty ? 1 : 0,
                      candidate.variadicResultNonEmpty ? 1 : 0,
                      candidate.variadicCommaInserted ? 1 : 0,
                      candidate.variadicCommaDeleted ? 1 : 0,
                      candidate.variadicGnuCommaElision ? 1 : 0,
                      candidate.variadicVaOptPresent ? 1 : 0,
                      candidate.variadicVaOptOriginallyActive ? 1 : 0,
                      candidate.variadicVaOptResultActive ? 1 : 0,
                      candidate.variadicVaOptCommaIntroduced ? 1 : 0,
                      candidate.variadicVaOptCommaDeleted ? 1 : 0,
                      candidate.variadicProducerSignature,
                      candidate.variadicPackStateSignature)
            .str());
  } else if (candidate.hasTokenPasteWitness) {
    key.suffixState = WitnessEquivalenceDimension::Known(
        llvm::formatv(
            "token_paste:root={0}:spans={1}:tokens={2}:parts={3}:"
            "arg_parts={4}:literal_parts={5}:left={6}:right={7}:"
            "result_validated={8}:diagnostic_safe={9}:producer={10}:result={"
            "11}",
            candidate.tokenPasteRootMacroId, candidate.tokenPasteSpanCount,
            candidate.tokenPasteTokenCount, candidate.tokenPastePartCount,
            candidate.tokenPasteArgPartCount,
            candidate.tokenPasteLiteralPartCount,
            candidate.tokenPasteHasLeftProducer ? 1 : 0,
            candidate.tokenPasteHasRightProducer ? 1 : 0,
            candidate.tokenPasteResultValidated ? 1 : 0,
            candidate.tokenPasteDiagnosticSafe ? 1 : 0,
            candidate.tokenPasteProducerSignature,
            candidate.tokenPasteResultSignature)
            .str());
  } else if (candidate.hasStringificationWitness) {
    key.suffixState = WitnessEquivalenceDimension::Known(
        llvm::formatv(
            "stringification:root={0}:spans={1}:args={2}:"
            "ws_normalized={3}:escaped_stable={4}:producer={5}:payload={6}",
            candidate.stringificationRootMacroId,
            candidate.stringificationSpanCount,
            candidate.stringificationArgCount,
            candidate.stringificationWhitespaceNormalized ? 1 : 0,
            candidate.stringificationEscapedSpellingStable ? 1 : 0,
            candidate.stringificationProducerSignature,
            candidate.stringificationCanonicalPayloadSignature)
            .str());
  } else if (candidate.hasZeroTokenBoundaryWitness) {
    key.suffixState = WitnessEquivalenceDimension::Known(
        llvm::formatv("zero_token:owner={0}:{1}:pp_gap={2}:{3}:anchor={4}:{5}:"
                      "b=[{6},{7}):producer={8}:owner_closed={9}:layout={10}:"
                      "observers={11}:counter={12}:empty_actual={13}:"
                      "replacement_gap={14}:"
                      "paired={15}:tu_anchor={16}:include_boundary={17}:"
                      "directive_gap={18}:"
                      "signature={19}",
                      candidate.zeroTokenOwnerKind, candidate.zeroTokenOwnerId,
                      candidate.zeroTokenHasPPGap ? 1 : 0,
                      candidate.zeroTokenPPGap,
                      candidate.zeroTokenHasSourceAnchor ? 1 : 0,
                      candidate.zeroTokenSourceAnchor,
                      candidate.zeroTokenBTokStart, candidate.zeroTokenBTokEnd,
                      candidate.zeroTokenProducerProven ? 1 : 0,
                      candidate.zeroTokenOwnerClosed ? 1 : 0,
                      candidate.zeroTokenLayoutStable ? 1 : 0,
                      candidate.zeroTokenObserversStable ? 1 : 0,
                      candidate.zeroTokenCounterStable ? 1 : 0,
                      candidate.zeroTokenFromEmptyActual ? 1 : 0,
                      candidate.zeroTokenFromReplacementGap ? 1 : 0,
                      candidate.zeroTokenFromPairedInsertion ? 1 : 0,
                      candidate.zeroTokenFromTUAnchor ? 1 : 0,
                      candidate.zeroTokenFromIncludeBoundary ? 1 : 0,
                      candidate.zeroTokenFromDirectiveLayoutGap ? 1 : 0,
                      candidate.zeroTokenBoundarySignature)
            .str());
  } else if (candidate.hasMacroActualRepairWitness) {
    key.suffixState = WitnessEquivalenceDimension::Known(
        llvm::formatv("macro_actual_replay:root={0}:whole_envelope={1}:"
                      "definition_tape={2}:arity_stable={3}",
                      candidate.rootMacroId,
                      candidate.macroActualWholeEnvelopeReplayValidated ? 1 : 0,
                      candidate.macroActualDefinitionTapeReplayValidated ? 1
                                                                         : 0,
                      candidate.macroActualArityStable ? 1 : 0)
            .str());
  } else if (canUseMacroRepairReplayStateNeutrality()) {
    key.suffixState = WitnessEquivalenceDimension::Known(
        llvm::formatv("macro_repair_replay_state_neutral:{0}",
                      macroRepairReplayStateNeutralityValue())
            .str());
  } else if (summary.hasSuffixStabilityWitness) {
    const SuffixStabilityWitness &suffix = summary.suffixStabilityWitness;
    key.suffixState = WitnessEquivalenceDimension::Known(
        llvm::formatv("suffix_witness:{0}:component={1}", suffix.kind,
                      ComponentNamedBySuffixStabilityWitness(suffix))
            .str());
  } else if (summary.hasOwnerRealizationWitness) {
    const OwnerRealizationWitness &owner = summary.ownerRealizationWitness;
    if (owner.closure.IsStateNeutral()) {
      key.suffixState = WitnessEquivalenceDimension::Known(
          llvm::formatv("owner_realization:evidence={0}:state_neutral",
                        owner.evidence)
              .str());
    } else if (ownerRealizationHasKnownMacroObservationOnlyState(owner)) {
      key.suffixState = WitnessEquivalenceDimension::Known(
          llvm::formatv(
              "owner_realization:evidence={0}:macro_observation_only:{1}",
              owner.evidence, macroObservationOnlyStateSignature(owner))
              .str());
    } else if (ownerRealizationHasKnownBuiltinLocationObservationState(owner)) {
      key.suffixState = WitnessEquivalenceDimension::Known(
          llvm::formatv(
              "owner_realization:evidence={0}:builtin_location_observation:{1}",
              owner.evidence, ownerStateDeltaSignature(owner))
              .str());
    } else if (ownerRealizationHasKnownIncludeState(owner)) {
      key.suffixState = WitnessEquivalenceDimension::Known(
          llvm::formatv("owner_realization:evidence={0}:include_state:{1}",
                        owner.evidence, ownerStateDeltaSignature(owner))
              .str());
    } else if (ownerRealizationHasKnownConditionalState(owner)) {
      key.suffixState = WitnessEquivalenceDimension::Known(
          llvm::formatv("owner_realization:evidence={0}:conditional_state:{1}",
                        owner.evidence, ownerStateDeltaSignature(owner))
              .str());
    } else if (!owner.stateWitnesses.empty()) {
      std::string stateSummary =
          llvm::formatv(
              "owner_realization:evidence={0}:state_witnesses={1}:state={2}",
              owner.evidence, owner.stateWitnesses.size(),
              ownerStateDeltaSignature(owner))
              .str();
      for (const SuffixStabilityWitness &suffix : owner.stateWitnesses) {
        stateSummary +=
            llvm::formatv(":{0}/{1}", suffix.kind,
                          ComponentNamedBySuffixStabilityWitness(suffix))
                .str();
      }
      key.suffixState = WitnessEquivalenceDimension::Known(stateSummary);
    } else {
      key.suffixState = WitnessEquivalenceDimension::Unknown(
          "owner-realization-state-delta-not-summarized");
    }
  } else if (summary.hasMixedOwnerTilingWitness &&
             summary.mixedOwnerTilingWitness.stateSummariesComposed) {
    const MixedOwnerTilingWitness &tiling = summary.mixedOwnerTilingWitness;
    key.suffixState = WitnessEquivalenceDimension::Known(
        llvm::formatv("mixed_owner_composed:id={0}:segments={1}:state_gaps={2}:"
                      "state={3}",
                      tiling.witnessId, tiling.tokenSegmentCount,
                      tiling.stateGapCount, mixedOwnerTilingSignature(tiling))
            .str());
  }

  if (candidate.hasLineControlObserverWitness) {
    const LineControlObserverWitness &line =
        candidate.lineControlObserverWitness;
    const std::string stateValue =
        llvm::formatv(
            "line_control_state:events={0}:active={1}:inactive={2}:"
            "producer={3}:missing_operands={4}:unknown_operands={5}:"
            "source_directives={6}:producer_directives={7}:"
            "include_return={8}:synthetic_resync={9}:state={10}:layout={11}",
            line.lineControlEventCount, line.activeLineControlEventCount,
            line.inactiveLineControlEventCount,
            line.producerProvenLineControlEventCount,
            line.missingOperandLineControlEventCount,
            line.unknownOperandLineControlEventCount,
            line.sourceAuthoredLineDirectiveCount,
            line.producerEmittedLineDirectiveCount,
            line.includeReturnResyncCount, line.syntheticResyncCount,
            FormatWitnessTraceHash(line.stateSignature),
            FormatWitnessTraceHash(line.layoutSignature))
            .str();

    if (key.suffixState.known) {
      key.suffixState = WitnessEquivalenceDimension::Known(
          llvm::formatv("{0}|{1}", key.suffixState.value, stateValue).str());
    } else if (line.hasSourceLineControlState ||
               line.hasSuffixLineControlDischarge) {
      key.suffixState = WitnessEquivalenceDimension::Unknown(
          llvm::formatv("line-control-suffix-state-partial:{0}",
                        FormatWitnessTraceHash(stateValue))
              .str());
    }
  }

  if (candidate.hasGeneratedCalleeReplayWitness) {
    key.preservedObservers = WitnessEquivalenceDimension::Known(
        llvm::formatv(
            "generated_callee_replay:root={0}:final_def={1}:target=[{2},{3}):"
            "reexpanded-target-pp",
            candidate.generatedCalleeRootMacroId,
            candidate.generatedCalleeFinalDirectiveId,
            candidate.targetBTokStart, candidate.targetBTokEnd)
            .str());
  } else if (candidate.hasVariadicCommaWitness) {
    key.preservedObservers = WitnessEquivalenceDimension::Known(
        llvm::formatv(
            "variadic:root={0}:target=[{1},{2}):formal={3}:"
            "missing={4}:empty={5}:nonempty={6}:literal_comma={7}:"
            "vaopt_result={8}:vaopt_included={9}:reexpanded-target-pp",
            candidate.variadicRootMacroId, candidate.targetBTokStart,
            candidate.targetBTokEnd, candidate.variadicFormalIndex,
            candidate.variadicResultMissing ? 1 : 0,
            candidate.variadicResultExplicitEmpty ? 1 : 0,
            candidate.variadicResultNonEmpty ? 1 : 0,
            candidate.variadicLiteralCommaInActual ? 1 : 0,
            candidate.variadicVaOptResultActive ? 1 : 0,
            candidate.variadicVaOptIncludedCount)
            .str());
  } else if (candidate.hasTokenPasteWitness) {
    key.preservedObservers = WitnessEquivalenceDimension::Known(
        llvm::formatv(
            "token_paste:root={0}:target=[{1},{2}):"
            "valid-token-formation={3}:diagnostic-safe={4}:result={5}",
            candidate.tokenPasteRootMacroId, candidate.targetBTokStart,
            candidate.targetBTokEnd,
            candidate.tokenPasteResultValidated ? 1 : 0,
            candidate.tokenPasteDiagnosticSafe ? 1 : 0,
            candidate.tokenPasteResultSignature)
            .str());
  } else if (candidate.hasStringificationWitness) {
    key.preservedObservers = WitnessEquivalenceDimension::Known(
        llvm::formatv("stringification:root={0}:target=[{1},{2}):"
                      "escaped-spelling={3}:whitespace={4}:payload={5}",
                      candidate.stringificationRootMacroId,
                      candidate.targetBTokStart, candidate.targetBTokEnd,
                      candidate.stringificationEscapedSpellingStable ? 1 : 0,
                      candidate.stringificationWhitespaceNormalized ? 1 : 0,
                      candidate.stringificationCanonicalPayloadSignature)
            .str());
  } else if (candidate.hasZeroTokenBoundaryWitness) {
    key.preservedObservers = WitnessEquivalenceDimension::Known(
        llvm::formatv(
            "zero_token:owner={0}:{1}:target=[{2},{3}):pp_gap={4}:{5}:"
            "source_anchor={6}:{7}:layout={8}:observers={9}:counter={10}:"
            "boundary={11}",
            candidate.zeroTokenOwnerKind, candidate.zeroTokenOwnerId,
            candidate.zeroTokenBTokStart, candidate.zeroTokenBTokEnd,
            candidate.zeroTokenHasPPGap ? 1 : 0, candidate.zeroTokenPPGap,
            candidate.zeroTokenHasSourceAnchor ? 1 : 0,
            candidate.zeroTokenSourceAnchor,
            candidate.zeroTokenLayoutStable ? 1 : 0,
            candidate.zeroTokenObserversStable ? 1 : 0,
            candidate.zeroTokenCounterStable ? 1 : 0,
            candidate.zeroTokenBoundarySignature)
            .str());
  } else if (candidate.hasMacroActualRepairWitness) {
    key.preservedObservers = WitnessEquivalenceDimension::Known(
        llvm::formatv("macro_actual_replay:root={0}:target=[{1},{2}):"
                      "reexpanded-target-pp",
                      candidate.rootMacroId, candidate.targetBTokStart,
                      candidate.targetBTokEnd)
            .str());
  } else if (canUseMacroRepairReplayStateNeutrality()) {
    key.preservedObservers = WitnessEquivalenceDimension::Known(
        llvm::formatv("macro_repair_replay_observers_neutral:{0}",
                      macroRepairReplayStateNeutralityValue())
            .str());
  } else if (summary.hasTUAnchorWitness) {
    const TUAnchorWitness &w = summary.tuAnchorWitness;
    key.preservedObservers = WitnessEquivalenceDimension::Known(
        llvm::formatv("tu_anchor:evidence={0}:pp_gap={1}:{2}:tu_byte={3}:{4}:"
                      "slot={5}:{6}:macro={7}:left={8}:{9}:right={10}:{11}:"
                      "outside_include={12}:owner_depth_stable={13}",
                      w.evidence, w.hasPPGap, w.ppGap, w.hasTUByte, w.tuByte,
                      w.slotId, w.slotKind, w.macroId, w.hasLeftNeighbor,
                      w.leftNeighborPP, w.hasRightNeighbor, w.rightNeighborPP,
                      w.outsideIncludeCoverage, w.ownerDepthStable)
            .str());
  } else if (summary.hasIncludeAnchorWitness) {
    const IncludeAnchorWitness &w = summary.includeAnchorWitness;
    key.preservedObservers = WitnessEquivalenceDimension::Known(
        llvm::formatv(
            "include_anchor:evidence={0}:anchor={1}:{2}:range={3}:[{4},{5}):"
            "pp_first={6}:{7}:pp_last={8}:{9}:neighbor={10}:{11}:"
            "cond={12}:{13}:child={14}:{15}:decl={16}:[{17},{18})",
            w.evidence, w.hasAnchorByte, w.anchorByte, w.hasByteRange,
            w.startByte, w.endByte, w.hasFirstPP, w.firstPP, w.hasLastPP,
            w.lastPP, w.hasNeighborPP, w.neighborPP, w.hasCondArmId,
            w.condArmId, w.hasChildIncludeId, w.childIncludeId,
            w.hasDeclHeaderRange, w.declHeaderB, w.declHeaderE)
            .str());
  } else if (summary.hasMixedOwnerTilingWitness &&
             summary.mixedOwnerTilingWitness.stateSummariesComposed) {
    const MixedOwnerTilingWitness &tiling = summary.mixedOwnerTilingWitness;
    key.preservedObservers = WitnessEquivalenceDimension::Known(
        llvm::formatv(
            "mixed_owner_observers:id={0}:segments={1}:state_gaps={2}:"
            "state={3}",
            tiling.witnessId, tiling.tokenSegmentCount, tiling.stateGapCount,
            mixedOwnerTilingSignature(tiling))
            .str());
  } else if (summary.hasOwnerRealizationWitness) {
    const OwnerObserverSummary &observers =
        summary.ownerRealizationWitness.closure.observers;
    key.preservedObservers = WitnessEquivalenceDimension::Known(
        llvm::formatv(
            "owner_observers:macro={0}:defined={1}:cond={2}:line={3}:"
            "file={4}:filename={5}:counter={6}:pragma={7}:include_guard={8}:"
            "include={9}",
            observers.observesMacroExpansion, observers.observesDefinedOperator,
            observers.observesConditionalEvaluation,
            observers.observesLineNumber, observers.observesFileState,
            observers.observesFileName, observers.observesCounter,
            observers.observesPragmaState, observers.observesIncludeGuardState,
            observers.observesIncludeState)
            .str());
  } else if (summary.hasSuffixStabilityWitness) {
    const SuffixStabilityWitness &suffix = summary.suffixStabilityWitness;
    key.preservedObservers = WitnessEquivalenceDimension::Known(
        llvm::formatv("suffix_observer_discharge:{0}:component={1}",
                      suffix.kind,
                      ComponentNamedBySuffixStabilityWitness(suffix))
            .str());
  }

  if (candidate.hasLineControlObserverWitness) {
    const LineControlObserverWitness &line =
        candidate.lineControlObserverWitness;
    const std::string observerValue =
        llvm::formatv(
            "line_control_observers:line={0}:file={1}:filename={2}:"
            "builtin={3}:builtin_line={4}:builtin_file={5}:"
            "builtin_filename={6}:suffix_discharge={7}:"
            "obs={8}:layout={9}",
            line.observesLineNumber ? 1 : 0, line.observesFileState ? 1 : 0,
            line.observesFileName ? 1 : 0, line.builtinLocationObservationCount,
            line.builtinLineObservationCount, line.builtinFileObservationCount,
            line.builtinFileNameObservationCount,
            line.hasSuffixLineControlDischarge ? 1 : 0,
            FormatWitnessTraceHash(line.observerSignature),
            FormatWitnessTraceHash(line.layoutSignature))
            .str();

    if (key.preservedObservers.known) {
      key.preservedObservers = WitnessEquivalenceDimension::Known(
          llvm::formatv("{0}|{1}", key.preservedObservers.value, observerValue)
              .str());
    } else {
      key.preservedObservers =
          WitnessEquivalenceDimension::Known(observerValue);
    }
  }

  if (candidate.hasCounterStateWitness) {
    const CounterStateWitness &counter = candidate.counterStateWitness;
    const std::string value =
        llvm::formatv(
            "counter_state:consumes={0}:order_known={1}:observations={2}:"
            "mutations={3}:suffix_observers={4}:suffix_stable={5}:"
            "covers_all={6}:literalized={7}:materialized={8}:"
            "suffix_unobserved={9}:expected_values={10}:missing_values={11}:"
            "order={12}:values={13}:observers={14}",
            counter.counterConsumptionCount, counter.counterOrderKnown ? 1 : 0,
            counter.counterObservationCount, counter.counterMutationCount,
            counter.preservedSuffixObserverCount,
            counter.suffixStateStable ? 1 : 0,
            counter.coversAllAffectedObservers ? 1 : 0,
            counter.literalizationStable ? 1 : 0,
            counter.materializationStable ? 1 : 0,
            counter.suffixUnobserved ? 1 : 0, counter.expectedBValueCount,
            counter.missingExpectedBValueCount,
            FormatWitnessTraceHash(counter.orderSignature),
            FormatWitnessTraceHash(counter.suffixValueSignature),
            FormatWitnessTraceHash(counter.suffixObserverSignature))
            .str();
    key.counterState = WitnessEquivalenceDimension::Known(value);
  } else if (candidate.hasGeneratedCalleeReplayWitness) {
    key.counterState = WitnessEquivalenceDimension::Known(
        llvm::formatv("generated_callee_replay:root={0}:counter-stable",
                      candidate.generatedCalleeRootMacroId)
            .str());
  } else if (candidate.hasVariadicCommaWitness) {
    key.counterState = WitnessEquivalenceDimension::Known(
        llvm::formatv("variadic:root={0}:counter-stable",
                      candidate.variadicRootMacroId)
            .str());
  } else if (candidate.hasTokenPasteWitness) {
    key.counterState = WitnessEquivalenceDimension::Known(
        llvm::formatv("token_paste:root={0}:counter-stable",
                      candidate.tokenPasteRootMacroId)
            .str());
  } else if (candidate.hasStringificationWitness) {
    key.counterState = WitnessEquivalenceDimension::Known(
        llvm::formatv("stringification:root={0}:counter-stable",
                      candidate.stringificationRootMacroId)
            .str());
  } else if (candidate.hasZeroTokenBoundaryWitness) {
    key.counterState =
        candidate.zeroTokenCounterStable
            ? WitnessEquivalenceDimension::Known(
                  llvm::formatv("zero_token:owner={0}:{1}:counter-stable",
                                candidate.zeroTokenOwnerKind,
                                candidate.zeroTokenOwnerId)
                      .str())
            : WitnessEquivalenceDimension::Unknown(
                  "zero-token-counter-stability-not-proven");
  } else if (summary.hasMixedOwnerTilingWitness) {
    const MixedOwnerTilingWitness &tiling = summary.mixedOwnerTilingWitness;
    key.counterState =
        mixedOwnerTilingHasKnownCounterState(tiling)
            ? WitnessEquivalenceDimension::Known(
                  llvm::formatv("mixed_owner_counter_state:id={0}:state={1}",
                                tiling.witnessId,
                                mixedOwnerTilingSignature(tiling))
                      .str())
            : WitnessEquivalenceDimension::Unknown(
                  "mixed-owner-counter-state-not-proven");
  } else if (candidate.hasMacroActualRepairWitness) {
    key.counterState = WitnessEquivalenceDimension::Known(
        llvm::formatv("macro_actual_replay:root={0}:counter-stable",
                      candidate.rootMacroId)
            .str());
  } else if (path == AcceptedPathKind::MacroCounterLiteral) {
    key.counterState = summary.hasSuffixStabilityWitness
                           ? WitnessEquivalenceDimension::Known(
                                 "counter_literalized_with_suffix_stability")
                           : WitnessEquivalenceDimension::Unknown(
                                 "counter-literal-without-suffix-witness");
  } else if (summary.hasOwnerRealizationWitness &&
             summary.ownerRealizationWitness.closure.IsComplete() &&
             !summary.ownerRealizationWitness.closure.observers
                  .observesCounter) {
    key.counterState = WitnessEquivalenceDimension::Known(
        "owner-realization-no-preserved-counter-observer");
  }

  if (candidate.hasGeneratedCalleeReplayWitness) {
    WitnessProducerKindSet producers;
    producers.Add(WitnessProducerKind::GeneratedCallee);
    if (candidate.generatedCalleeUsesForwarding)
      producers.Add(WitnessProducerKind::Forward);
    if (candidate.generatedCalleeUsesStringification)
      producers.Add(WitnessProducerKind::Stringify);
    if (candidate.generatedCalleeUsesPaste) {
      producers.Add(WitnessProducerKind::PasteLeft);
      producers.Add(WitnessProducerKind::PasteRight);
      producers.Add(WitnessProducerKind::PasteResult);
    }
    if (candidate.generatedCalleeUsesVariadicForwarding)
      producers.Add(WitnessProducerKind::VariadicForward);
    if (candidate.generatedCalleeUsesObjectAlias)
      producers.Add(WitnessProducerKind::ObjectAlias);
    if (candidate.hasZeroTokenBoundaryWitness)
      producers.Add(WitnessProducerKind::ZeroTokenAnchor);
    key.producerKinds = std::move(producers);
  } else if (candidate.hasVariadicCommaWitness) {
    WitnessProducerKindSet producers;
    producers.Add(WitnessProducerKind::Forward);
    producers.Add(WitnessProducerKind::VariadicForward);
    if (candidate.variadicResultMissing)
      producers.Add(WitnessProducerKind::VariadicMissing);
    if (candidate.variadicResultExplicitEmpty)
      producers.Add(WitnessProducerKind::VariadicEmpty);
    if (candidate.variadicCommaInserted ||
        candidate.variadicVaOptCommaIntroduced)
      producers.Add(WitnessProducerKind::VariadicCommaInsertion);
    if (candidate.variadicCommaDeleted || candidate.variadicGnuCommaElision ||
        candidate.variadicVaOptCommaDeleted)
      producers.Add(WitnessProducerKind::VariadicCommaElision);
    if (candidate.variadicVaOptPresent) {
      if (candidate.variadicVaOptResultActive)
        producers.Add(WitnessProducerKind::VaOptActivation);
      else
        producers.Add(WitnessProducerKind::VaOptErasure);
    }
    if (candidate.hasZeroTokenBoundaryWitness)
      producers.Add(WitnessProducerKind::ZeroTokenAnchor);
    key.producerKinds = std::move(producers);
  } else if (candidate.hasStringificationWitness ||
             candidate.hasTokenPasteWitness) {
    WitnessProducerKindSet producers;
    if (candidate.hasStringificationWitness)
      producers.Add(WitnessProducerKind::Stringify);
    if (candidate.hasTokenPasteWitness) {
      if (candidate.tokenPasteHasLeftProducer)
        producers.Add(WitnessProducerKind::PasteLeft);
      if (candidate.tokenPasteHasRightProducer)
        producers.Add(WitnessProducerKind::PasteRight);
      producers.Add(WitnessProducerKind::PasteResult);
    }
    if (candidate.hasZeroTokenBoundaryWitness)
      producers.Add(WitnessProducerKind::ZeroTokenAnchor);
    key.producerKinds = std::move(producers);
  } else if (candidate.hasZeroTokenBoundaryWitness) {
    WitnessProducerKindSet producers;
    producers.Add(WitnessProducerKind::Forward);
    producers.Add(WitnessProducerKind::ZeroTokenAnchor);
    key.producerKinds = std::move(producers);
  } else {
    key.producerKinds = WitnessProducerKindSet::KnownSingle(
        WitnessProducerKindForAcceptedPath(path));
  }

  if (candidate.hasLineControlObserverWitness && key.producerKinds.known) {
    const LineControlObserverWitness &line =
        candidate.lineControlObserverWitness;
    if (line.lineControlEventCount || line.sourceAuthoredLineDirectiveCount ||
        line.producerEmittedLineDirectiveCount)
      key.producerKinds.Add(WitnessProducerKind::DirectiveMaterialization);
    if (line.builtinLocationObservationCount)
      key.producerKinds.Add(WitnessProducerKind::BuiltinMaterialization);
  }

  if (candidate.hasCounterStateWitness && key.producerKinds.known) {
    const CounterStateWitness &counter = candidate.counterStateWitness;
    if (counter.hasCounterEvents || counter.counterConsumptionCount != 0)
      key.producerKinds.Add(WitnessProducerKind::CounterConsumption);
    if (counter.literalizationStable || counter.materializationStable ||
        counter.hasExpectedBValues)
      key.producerKinds.Add(WitnessProducerKind::BuiltinMaterialization);
  }

  key.boundaryClass = candidate.hasZeroTokenBoundaryWitness
                          ? WitnessBoundaryClass::ZeroTokenBoundary
                          : WitnessBoundaryClassForAcceptedCandidate(candidate);

  if (path == AcceptedPathKind::TerminalEmitEditedPreprocessedStream) {
    key.diagnosticClass = WitnessDiagnosticClass::TerminalOutOfDomain;
    key.compositionClass = WitnessCompositionClass::Terminal;
  } else if (summary.realizationMode == RealizationMode::RealizeEditedSurface) {
    key.diagnosticClass = WitnessDiagnosticClass::RealizesEditedSurface;
  } else if (summary.realizationMode ==
             RealizationMode::PreserveOriginalStructure) {
    key.diagnosticClass = WitnessDiagnosticClass::PreservesDiagnostics;
  }

  if (summary.hasMixedOwnerTilingWitness ||
      summary.theoremClass == TheoremProofClass::MixedOwnerTilingProof)
    key.compositionClass = WitnessCompositionClass::MixedOwnerTile;
  else if (summary.hasOwnerRealizationWitness ||
           summary.theoremClass == TheoremProofClass::OwnerRealizationProof)
    key.compositionClass = WitnessCompositionClass::OwnerClosed;
  else if (key.compositionClass == WitnessCompositionClass::Unknown &&
           candidate.kind != AcceptedResultCandidateKind::Unknown)
    key.compositionClass = WitnessCompositionClass::LocalOnly;

  return key;
}

RefoldEngine::WitnessCanonicalCost RefoldEngine::BuildWitnessCanonicalCost(
    const AcceptedResultCandidate &candidate) const {
  WitnessCanonicalCost cost;
  cost.preserveOriginalPenalty =
      candidate.proofSummary.structurePreserving ? 0 : 1;
  cost.sourceRangeBytes =
      candidate.end >= candidate.begin ? candidate.end - candidate.begin : 0;
  cost.ownerBoundaryChangePenalty =
      candidate.kind == AcceptedResultCandidateKind::TerminalOutOfDomain ? 1
                                                                         : 0;
  cost.spellingChangePenalty =
      candidate.hasPayloadPreview ? candidate.payloadPreview.size() : 0;
  cost.sourceOrder = candidate.begin;
  return cost;
}

RefoldEngine::RefoldWitness
RefoldEngine::BuildRefoldWitness(const AcceptedResultCandidate &candidate,
                                 llvm::StringRef role,
                                 uint64_t witnessId) const {
  RefoldWitness witness;
  witness.witnessId = witnessId;
  witness.family = WitnessFamilyForAcceptedPath(
      candidate.proofSummary.inventory.currentPath);
  if (candidate.hasGeneratedCalleeReplayWitness)
    witness.family = WitnessProofFamily::GeneratedCalleeReplay;
  else if (candidate.hasVariadicCommaWitness)
    witness.family = WitnessProofFamily::VariadicComma;
  else if (candidate.hasTokenPasteWitness)
    witness.family = WitnessProofFamily::TokenPaste;
  else if (candidate.hasStringificationWitness)
    witness.family = WitnessProofFamily::Stringification;
  else if (candidate.hasZeroTokenBoundaryWitness)
    witness.family = WitnessProofFamily::ZeroTokenBoundary;
  if (witness.family == WitnessProofFamily::Unknown &&
      candidate.kind != AcceptedResultCandidateKind::Unknown)
    witness.family = WitnessProofFamily::AcceptedResult;
  if (candidate.hasCounterStateWitness &&
      (witness.family == WitnessProofFamily::AcceptedResult ||
       candidate.proofSummary.inventory.currentPath ==
           AcceptedPathKind::MacroCounterLiteral))
    witness.family = WitnessProofFamily::CounterState;
  if (candidate.hasLineControlObserverWitness &&
      witness.family == WitnessProofFamily::AcceptedResult)
    witness.family = WitnessProofFamily::LineControlObserver;
  if ((candidate.proofSummary.hasMixedOwnerTilingWitness ||
       candidate.proofSummary.theoremClass ==
           TheoremProofClass::MixedOwnerTilingProof) &&
      (witness.family == WitnessProofFamily::Unknown ||
       witness.family == WitnessProofFamily::AcceptedResult ||
       witness.family == WitnessProofFamily::TUTextEdit ||
       witness.family == WitnessProofFamily::OwnerRealization))
    witness.family = WitnessProofFamily::MixedOwnerTiling;

  if (candidate.hasRootMacroId)
    witness.owner = llvm::formatv("macro#{0}", candidate.rootMacroId).str();
  else if (candidate.hasOwnerIncludeId)
    witness.owner =
        llvm::formatv("include#{0}", candidate.ownerIncludeId).str();
  else if (candidate.hasAnchorByte)
    witness.owner = llvm::formatv("anchor@{0}", candidate.anchorByte).str();
  else
    witness.owner =
        llvm::formatv("range=[{0},{1})", candidate.begin, candidate.end).str();

  witness.selector = role.str();
  witness.sourceFamily =
      toString(candidate.proofSummary.inventory.currentPath).str();
  witness.candidateKind = toString(candidate.kind).str();
  witness.theoremClass = toString(candidate.proofSummary.theoremClass).str();

  witness.detail = llvm::formatv("role={0} kind={1} path={2} theorem={3}", role,
                                 candidate.kind,
                                 candidate.proofSummary.inventory.currentPath,
                                 candidate.proofSummary.theoremClass)
                       .str();
  if (candidate.hasLineControlObserverWitness) {
    const LineControlObserverWitness &line =
        candidate.lineControlObserverWitness;
    witness.detail +=
        llvm::formatv(" line_control=line:{0}/file:{1}/filename:{2}/"
                      "events:{3}/builtin:{4}",
                      line.observesLineNumber ? 1 : 0,
                      line.observesFileState ? 1 : 0,
                      line.observesFileName ? 1 : 0, line.lineControlEventCount,
                      line.builtinLocationObservationCount)
            .str();
  }
  if (candidate.hasCounterStateWitness) {
    const CounterStateWitness &counter = candidate.counterStateWitness;
    witness.detail +=
        llvm::formatv(
            " counter=consumes:{0}/order:{1}/suffix:{2}/"
            "expected_values:{3}/missing_values:{4}",
            counter.counterConsumptionCount, counter.counterOrderKnown ? 1 : 0,
            counter.suffixStateStable ? 1 : 0, counter.expectedBValueCount,
            counter.missingExpectedBValueCount)
            .str();
  }
  witness.key = BuildWitnessEquivalenceKey(candidate);
  witness.cost = BuildWitnessCanonicalCost(candidate);
  if (candidate.hasPayloadPreview)
    witness.payloadPreview = candidate.payloadPreview;
  return witness;
}

RefoldEngine::WitnessResolverMode RefoldEngine::GetWitnessResolverMode() const {
  if (strict_)
    return WitnessResolverMode::Strict;

  switch (proofAuditMode_) {
  case ProofAuditMode::Default:
  case ProofAuditMode::Off:
    return WitnessResolverMode::Off;
  case ProofAuditMode::Probe:
    return WitnessResolverMode::Probe;
  case ProofAuditMode::Strict:
    return WitnessResolverMode::Strict;
  }

  return WitnessResolverMode::Off;
}

bool RefoldEngine::ShouldEmitProofLog() const {
  return GetWitnessResolverMode() != WitnessResolverMode::Off;
}

static std::string formatOptionalIndex(std::optional<size_t> index) {
  if (!index)
    return "none";
  return llvm::formatv("{0}", *index).str();
}

static std::string formatWitnessClosureAtom(StringRef value) {
  if (value.empty())
    return "<none>";

  std::string out;
  for (char raw : value) {
    const unsigned char c = static_cast<unsigned char>(raw);
    if (std::isalnum(c) || c == '_' || c == '-' || c == '.' || c == ':' ||
        c == '/' || c == '#' || c == '@' || c == '[' || c == ']' || c == '(' ||
        c == ')' || c == ',' || c == '=') {
      out.push_back(static_cast<char>(c));
      continue;
    }

    out += "%";
    const std::string hex = llvm::utohexstr(c, /*LowerCase=*/true);
    if (hex.size() == 1)
      out += "0";
    out += hex;
  }
  return out;
}

template <typename FormatObject>
static void LogProofLine(const FormatObject &line) {
  std::string text = line.str();
  while (!text.empty() && text.back() == '\n')
    text.pop_back();
  REFOLD_LOG_INFO("proof", "{0}", text);
}

RefoldEngine::WitnessFallbackClass
RefoldEngine::ClassifyTerminalFallbackFailure(
    const TerminalFallbackProofFailure &failure) {
  using FailureReason = TerminalFallbackFailureReason;
  using FallbackClass = WitnessFallbackClass;

  switch (failure.reason) {
  case FailureReason::NoOwnerClosedCover:
  case FailureReason::NoTUAnchorForUnresolvedOwner:
    return FallbackClass::NoOwnerClosedWitness;
  case FailureReason::AmbiguousMixedOwnerTiling:
    return FallbackClass::MultipleNonEquivalentWitnessClasses;
  case FailureReason::NoDeterministicMixedOwnerTiling:
  case FailureReason::UncomposableEmissionEditSet:
    return FallbackClass::CompositionFailure;
  case FailureReason::StateTransitionConsumedAndObserved:
  case FailureReason::MacroStateNotStabilizable:
  case FailureReason::IncludeGuardStateNotStabilizable:
  case FailureReason::ConditionalStateNotStabilizable:
  case FailureReason::NoCanonicalSuffixOrder:
    return FallbackClass::UnknownSuffixState;
  case FailureReason::UnknownPragmaCrossesBoundary:
  case FailureReason::ReverseSolvedDirectiveRequired:
    return FallbackClass::UnsupportedDirectiveInteraction;
  case FailureReason::LineControlStateNotProducerProven:
    return FallbackClass::LineControlObserverMismatch;
  case FailureReason::CounterStateNotStabilizable:
    return FallbackClass::CounterStateMismatch;
  case FailureReason::MalformedInvocationPreservation:
    return FallbackClass::InvalidMacroInvocation;
  case FailureReason::MissingProducerFacts:
  case FailureReason::UnmappableIncludeBEnvelope:
  case FailureReason::UndischargedEmissionArtifact:
    return FallbackClass::UnprovenProducerKind;
  case FailureReason::ValidationFailure:
  case FailureReason::TheoremAuditInvariantViolation:
    return FallbackClass::ValidationFailure;
  case FailureReason::Unknown:
  case FailureReason::UnclassifiedTerminalFallback:
    return FallbackClass::Unknown;
  }
  return FallbackClass::Unknown;
}

RefoldEngine::WitnessFallbackClass RefoldEngine::ClassifyIncompleteWitnessKeys(
    ArrayRef<std::pair<size_t, RefoldWitness>> witnesses) {
  using FallbackClass = WitnessFallbackClass;

  bool unknownTargetPP = false;
  bool unknownSuffix = false;
  bool unknownObservers = false;
  bool unknownCounter = false;
  bool unknownProducer = false;
  bool unknownBoundary = false;
  bool unknownDiagnostics = false;
  bool unknownComposition = false;

  for (const std::pair<size_t, RefoldWitness> &entry : witnesses) {
    const WitnessEquivalenceKey &key = entry.second.key;
    unknownTargetPP |= !key.targetPPTokens.known;
    unknownSuffix |= !key.suffixState.known;
    unknownObservers |= !key.preservedObservers.known;
    unknownCounter |= !key.counterState.known;
    unknownProducer |= !key.producerKinds.known;
    unknownBoundary |= key.boundaryClass == WitnessBoundaryClass::Unknown;
    unknownDiagnostics |=
        key.diagnosticClass == WitnessDiagnosticClass::Unknown;
    unknownComposition |=
        key.compositionClass == WitnessCompositionClass::Unknown;
  }

  // Prefer the first failed theorem obligation that prevents comparing witness
  // classes.  This ordering is conservative: it does not assert a mismatch; it
  // states which proof dimension is still missing strongly enough to require
  // terminal classification instead of resolver authority.
  if (unknownBoundary)
    return FallbackClass::NoOwnerClosedWitness;
  if (unknownTargetPP)
    return FallbackClass::UnknownTargetPreprocessedTokens;
  if (unknownProducer)
    return FallbackClass::UnprovenProducerKind;
  if (unknownSuffix)
    return FallbackClass::UnknownSuffixState;
  if (unknownObservers)
    return FallbackClass::LineControlObserverMismatch;
  if (unknownCounter)
    return FallbackClass::CounterStateMismatch;
  if (unknownComposition)
    return FallbackClass::CompositionFailure;
  if (unknownDiagnostics)
    return FallbackClass::ValidationFailure;
  return FallbackClass::IncompleteWitnessKey;
}

void RefoldEngine::PopulateWitnessClosureLedger(
    WitnessResolverDecision &decision,
    ArrayRef<std::pair<size_t, RefoldWitness>> selectableWitnesses) {
  using DomainClass = WitnessStrictDomainClass;
  using Obligation = WitnessStrictDomainObligation;

  decision.closureLedger.clear();
  if (decision.strictDomain.domainClass !=
      DomainClass::PotentiallyInDomainMissingProof)
    return;

  auto ownerKindForWitness = [](const RefoldWitness &witness) {
    StringRef owner(witness.owner);
    if (owner.starts_with("macro#"))
      return std::string("macro");
    if (owner.starts_with("include#"))
      return std::string("include");
    if (owner.starts_with("anchor@"))
      return std::string("anchor");
    if (owner.starts_with("range="))
      return std::string("token_range");
    if (owner.empty())
      return std::string("unknown");
    return std::string("other");
  };

  auto addEntry = [&](const RefoldWitness &witness, size_t candidateIndex,
                      StringRef missingDimension, StringRef missingReason,
                      Obligation obligation) {
    WitnessClosureLedgerEntry entry;
    entry.selector = decision.role;
    entry.testRegion = witness.owner.empty() ? std::string("<none>")
                                             : witness.owner;
    entry.witnessId = witness.witnessId;
    entry.candidateIndex = candidateIndex;
    entry.family = witness.family;
    entry.sourceFamily = witness.sourceFamily.empty() ? std::string("<none>")
                                                      : witness.sourceFamily;
    entry.candidateKind = witness.candidateKind.empty()
                              ? std::string("<none>")
                              : witness.candidateKind;
    entry.theoremClass = witness.theoremClass.empty() ? std::string("<none>")
                                                      : witness.theoremClass;
    entry.sourceOwnerKind = ownerKindForWitness(witness);
    entry.owner = witness.owner.empty() ? std::string("<none>")
                                        : witness.owner;
    entry.missingDimension = missingDimension.str();
    entry.missingReason = missingReason.empty() ? std::string("<none>")
                                                : missingReason.str();
    entry.obligation = obligation;
    entry.fallbackClass = decision.fallbackClass;
    entry.resolverReason = decision.failureReason.empty()
                               ? std::string("<none>")
                               : decision.failureReason;
    decision.closureLedger.push_back(std::move(entry));
  };

  auto addUnknownKeyDimensions = [&](const RefoldWitness &witness,
                                     size_t candidateIndex) {
    const WitnessEquivalenceKey &key = witness.key;
    if (!key.targetPPTokens.known)
      addEntry(witness, candidateIndex, "target_pp",
               key.targetPPTokens.unknownReason,
               Obligation::TargetPreprocessedTokens);
    if (!key.suffixState.known)
      addEntry(witness, candidateIndex, "suffix_state",
               key.suffixState.unknownReason, Obligation::StateEquivalence);
    if (!key.preservedObservers.known)
      addEntry(witness, candidateIndex, "observers",
               key.preservedObservers.unknownReason,
               Obligation::LineControlObserverEquivalence);
    if (!key.counterState.known)
      addEntry(witness, candidateIndex, "counter",
               key.counterState.unknownReason, Obligation::CounterEquivalence);
    if (!key.producerKinds.known)
      addEntry(witness, candidateIndex, "producer",
               key.producerKinds.unknownReason,
               Obligation::ProducerProvenSourceWitness);
    if (key.boundaryClass == WitnessBoundaryClass::Unknown)
      addEntry(witness, candidateIndex, "owner_closure",
               "boundary-class-unknown", Obligation::OwnerClosure);
    if (key.diagnosticClass == WitnessDiagnosticClass::Unknown)
      addEntry(witness, candidateIndex, "diagnostics",
               "diagnostic-class-unknown", Obligation::FinalValidation);
    if (key.compositionClass == WitnessCompositionClass::Unknown)
      addEntry(witness, candidateIndex, "composition",
               "composition-class-unknown", Obligation::Composition);
  };

  for (const std::pair<size_t, RefoldWitness> &entry : selectableWitnesses) {
    const RefoldWitness &witness = entry.second;
    if (witness.key.HasUnknownDimensions())
      addUnknownKeyDimensions(witness, entry.first);
  }

  if (decision.closureLedger.empty() &&
      decision.fallbackClass == WitnessFallbackClass::UnconvertedProofFamily) {
    for (const std::pair<size_t, RefoldWitness> &entry : selectableWitnesses) {
      if (!IsResolverAuthoritativeWitness(entry.second))
        addEntry(entry.second, entry.first, "converted_family",
                 "proof-family-not-resolver-authoritative",
                 Obligation::ConvertedProofFamily);
    }
  }

  if (decision.closureLedger.empty() &&
      decision.fallbackClass == WitnessFallbackClass::CompositionFailure) {
    const std::string reason = decision.composition.reason.empty()
                                   ? std::string("composition-not-proven")
                                   : decision.composition.reason;
    for (const std::pair<size_t, RefoldWitness> &entry : selectableWitnesses)
      addEntry(entry.second, entry.first, "composition", reason,
               Obligation::Composition);
  }

  if (decision.closureLedger.empty() && selectableWitnesses.empty()) {
    RefoldWitness synthetic;
    synthetic.witnessId = 0;
    synthetic.family = WitnessProofFamily::Unknown;
    synthetic.owner.clear();
    synthetic.selector = decision.role;
    synthetic.sourceFamily = "<none>";
    synthetic.candidateKind = "<none>";
    synthetic.theoremClass = "<none>";
    addEntry(synthetic, 0, "producer",
             decision.failureReason.empty() ? StringRef("no-selectable-witness")
                                            : StringRef(decision.failureReason),
             Obligation::ProducerProvenSourceWitness);
  }

  if (decision.closureLedger.empty()) {
    for (const std::pair<size_t, RefoldWitness> &entry : selectableWitnesses)
      addEntry(entry.second, entry.first, "unknown",
               decision.failureReason.empty()
                   ? StringRef("missing-proof")
                   : StringRef(decision.failureReason),
               decision.strictDomain.obligation);
  }
}

RefoldEngine::WitnessFallbackClass RefoldEngine::ClassifyResolverFallbackReason(
    llvm::StringRef reason, const WitnessCompositionDecision &composition) {
  using FallbackClass = WitnessFallbackClass;

  if (reason.empty() || reason == "<none>")
    return FallbackClass::Unknown;
  if (reason == "no-selectable-witness")
    return FallbackClass::NoSelectableWitness;
  if (reason == "multiple-non-equivalent-classes")
    return FallbackClass::MultipleNonEquivalentWitnessClasses;
  if (reason == "unconverted-proof-family")
    return FallbackClass::UnconvertedProofFamily;
  if (reason == "composition-not-proven" ||
      reason == "composition-incomplete-witness-key" ||
      reason == "composition-incompatible-terminal-tuple" ||
      reason == "multiple-non-equivalent-composition-classes" ||
      composition.failureIsFatal)
    return FallbackClass::CompositionFailure;
  if (reason == "incomplete-witness-key")
    return FallbackClass::IncompleteWitnessKey;
  if (reason == "single-equivalence-class" ||
      reason == "single-source-repair-multiple-proof-classes")
    return FallbackClass::Unknown;

  return FallbackClass::Unknown;
}

RefoldEngine::WitnessStrictDomainObligation
RefoldEngine::StrictDomainObligationForFallbackClass(
    WitnessFallbackClass fallbackClass) {
  using FallbackClass = WitnessFallbackClass;
  using Obligation = WitnessStrictDomainObligation;

  switch (fallbackClass) {
  case FallbackClass::NoOwnerClosedWitness:
    return Obligation::OwnerClosure;
  case FallbackClass::MultipleNonEquivalentWitnessClasses:
    return Obligation::FiniteDeterministicTiling;
  case FallbackClass::UnknownTargetPreprocessedTokens:
    return Obligation::TargetPreprocessedTokens;
  case FallbackClass::UnknownSuffixState:
    return Obligation::StateEquivalence;
  case FallbackClass::UnprovenProducerKind:
    return Obligation::ProducerProvenSourceWitness;
  case FallbackClass::CounterStateMismatch:
    return Obligation::CounterEquivalence;
  case FallbackClass::LineControlObserverMismatch:
    return Obligation::LineControlObserverEquivalence;
  case FallbackClass::InvalidMacroInvocation:
    return Obligation::ValidSourceRepair;
  case FallbackClass::InvalidPasteResult:
  case FallbackClass::UnsupportedDirectiveInteraction:
    return Obligation::ModeledProducerSemantics;
  case FallbackClass::CompositionFailure:
    return Obligation::Composition;
  case FallbackClass::ValidationFailure:
    return Obligation::FinalValidation;
  case FallbackClass::UnconvertedProofFamily:
    return Obligation::ConvertedProofFamily;
  case FallbackClass::IncompleteWitnessKey:
    return Obligation::CompleteWitnessKey;
  case FallbackClass::NoSelectableWitness:
    return Obligation::ProducerProvenSourceWitness;
  case FallbackClass::Unknown:
    return Obligation::Unknown;
  }
  return Obligation::Unknown;
}

RefoldEngine::WitnessStrictDomainDecision
RefoldEngine::ClassifyStrictDomainForResolver(
    const WitnessResolverDecision &decision) {
  using DomainClass = WitnessStrictDomainClass;
  using FallbackClass = WitnessFallbackClass;

  WitnessStrictDomainDecision result;
  result.fallbackClass = decision.fallbackClass;
  result.reason = decision.failureReason;

  if (decision.strictUseResolver && decision.resolverIndex) {
    result.domainClass = DomainClass::DeclaredInDomain;
    result.obligation = WitnessStrictDomainObligation::Unknown;
    if (result.reason.empty())
      result.reason = "resolver-authoritative-in-domain";
    return result;
  }

  if (decision.strictFailClosed || decision.composition.failureIsFatal ||
      decision.fallbackClass ==
          FallbackClass::MultipleNonEquivalentWitnessClasses) {
    result.domainClass = DomainClass::AmbiguousOutOfDomain;
    result.obligation = StrictDomainObligationForFallbackClass(
        decision.fallbackClass == FallbackClass::Unknown
            ? FallbackClass::MultipleNonEquivalentWitnessClasses
            : decision.fallbackClass);
    if (result.reason.empty())
      result.reason = "multiple-non-equivalent-source-repairs";
    return result;
  }

  switch (decision.fallbackClass) {
  case FallbackClass::Unknown:
    result.domainClass = DomainClass::Unknown;
    result.obligation = WitnessStrictDomainObligation::Unknown;
    break;
  case FallbackClass::NoOwnerClosedWitness:
  case FallbackClass::InvalidMacroInvocation:
  case FallbackClass::InvalidPasteResult:
  case FallbackClass::UnsupportedDirectiveInteraction:
  case FallbackClass::ValidationFailure:
    result.domainClass = DomainClass::ExplicitOutOfDomain;
    result.obligation =
        StrictDomainObligationForFallbackClass(decision.fallbackClass);
    break;
  case FallbackClass::CompositionFailure:
    if (decision.failureReason ==
            "multiple-non-equivalent-composition-classes" ||
        decision.failureReason ==
            "composition-incompatible-terminal-tuple") {
      result.domainClass = DomainClass::AmbiguousOutOfDomain;
    } else {
      result.domainClass = DomainClass::PotentiallyInDomainMissingProof;
    }
    result.obligation =
        StrictDomainObligationForFallbackClass(decision.fallbackClass);
    break;
  case FallbackClass::MultipleNonEquivalentWitnessClasses:
    result.domainClass = DomainClass::AmbiguousOutOfDomain;
    result.obligation =
        StrictDomainObligationForFallbackClass(decision.fallbackClass);
    break;
  case FallbackClass::UnknownTargetPreprocessedTokens:
  case FallbackClass::UnknownSuffixState:
  case FallbackClass::UnprovenProducerKind:
  case FallbackClass::CounterStateMismatch:
  case FallbackClass::LineControlObserverMismatch:
  case FallbackClass::UnconvertedProofFamily:
  case FallbackClass::IncompleteWitnessKey:
  case FallbackClass::NoSelectableWitness:
    result.domainClass = DomainClass::PotentiallyInDomainMissingProof;
    result.obligation =
        StrictDomainObligationForFallbackClass(decision.fallbackClass);
    break;
  }

  if (result.reason.empty())
    result.reason = toString(result.fallbackClass).str();
  return result;
}

RefoldEngine::WitnessStrictDomainDecision
RefoldEngine::ClassifyStrictDomainForTerminalFallback(
    const TerminalFallbackProofFailure &failure) {
  using DomainClass = WitnessStrictDomainClass;
  using FailureReason = TerminalFallbackFailureReason;
  using FallbackClass = WitnessFallbackClass;

  WitnessStrictDomainDecision result;
  result.fallbackClass = ClassifyTerminalFallbackFailure(failure);
  result.obligation =
      StrictDomainObligationForFallbackClass(result.fallbackClass);
  result.reason = toString(failure.reason).str();

  switch (failure.reason) {
  case FailureReason::NoOwnerClosedCover:
  case FailureReason::NoTUAnchorForUnresolvedOwner:
  case FailureReason::UnknownPragmaCrossesBoundary:
  case FailureReason::ReverseSolvedDirectiveRequired:
  case FailureReason::MalformedInvocationPreservation:
  case FailureReason::ValidationFailure:
  case FailureReason::TheoremAuditInvariantViolation:
    result.domainClass = DomainClass::ExplicitOutOfDomain;
    break;

  case FailureReason::AmbiguousMixedOwnerTiling:
    result.domainClass = DomainClass::AmbiguousOutOfDomain;
    break;

  case FailureReason::NoDeterministicMixedOwnerTiling:
  case FailureReason::UncomposableEmissionEditSet:
    result.domainClass = DomainClass::AmbiguousOutOfDomain;
    break;

  case FailureReason::StateTransitionConsumedAndObserved:
  case FailureReason::LineControlStateNotProducerProven:
  case FailureReason::CounterStateNotStabilizable:
  case FailureReason::MacroStateNotStabilizable:
  case FailureReason::IncludeGuardStateNotStabilizable:
  case FailureReason::ConditionalStateNotStabilizable:
  case FailureReason::MissingProducerFacts:
  case FailureReason::NoCanonicalSuffixOrder:
  case FailureReason::UnmappableIncludeBEnvelope:
  case FailureReason::UndischargedEmissionArtifact:
    result.domainClass = DomainClass::PotentiallyInDomainMissingProof;
    break;

  case FailureReason::Unknown:
  case FailureReason::UnclassifiedTerminalFallback:
    result.domainClass = DomainClass::Unknown;
    break;
  }

  if (result.fallbackClass ==
      FallbackClass::MultipleNonEquivalentWitnessClasses)
    result.domainClass = DomainClass::AmbiguousOutOfDomain;

  return result;
}

void RefoldEngine::TraceWitnessStrictDomain(
    llvm::StringRef role, const WitnessStrictDomainDecision &decision) const {
  if (!ShouldEmitProofLog() &&
      GetWitnessResolverMode() == WitnessResolverMode::Off)
    return;

  LogProofLine(
      llvm::formatv("REFOLD-WITNESS-DOMAIN role={0} domain={1} obligation={2} "
                    "fallback_class={3} reason={4}\n",
                    role, toString(decision.domainClass),
                    toString(decision.obligation), decision.fallbackClass,
                    decision.reason.empty() ? StringRef("<none>")
                                            : StringRef(decision.reason)));
}

void RefoldEngine::TraceWitnessClosureLedger(
    const WitnessResolverDecision &decision) const {
  if (!ShouldEmitProofLog() && decision.mode == WitnessResolverMode::Off)
    return;

  for (const WitnessClosureLedgerEntry &entry : decision.closureLedger) {
    LogProofLine(llvm::formatv(
        "REFOLD-WITNESS-CLOSURE selector={0} family={1} "
        "test_region={2} missing={3} source_family={4} "
        "candidate_kind={5} theorem={6} witness_id={7} "
        "candidate_index={8} candidate_count={9} selectable={10} "
        "complete={11} incomplete={12} converted={13} unconverted={14} "
        "classes={15} owner_kind={16} owner={17} fallback_class={18} "
        "obligation={19} reason={20} missing_reason={21} "
        "composition={22}:classes={23}:complete={24}:incomplete={25}:"
        "incompatible={26}:reason={27}\n",
        formatWitnessClosureAtom(entry.selector), entry.family,
        formatWitnessClosureAtom(entry.testRegion),
        formatWitnessClosureAtom(entry.missingDimension),
        formatWitnessClosureAtom(entry.sourceFamily),
        formatWitnessClosureAtom(entry.candidateKind),
        formatWitnessClosureAtom(entry.theoremClass), entry.witnessId,
        entry.candidateIndex, decision.candidateCount, decision.selectableCount,
        decision.completeWitnessCount, decision.incompleteWitnessCount,
        decision.resolverAuthoritativeWitnessCount,
        decision.resolverUnconvertedWitnessCount,
        decision.equivalenceClassCount,
        formatWitnessClosureAtom(entry.sourceOwnerKind),
        formatWitnessClosureAtom(entry.owner), entry.fallbackClass,
        entry.obligation, formatWitnessClosureAtom(entry.resolverReason),
        formatWitnessClosureAtom(entry.missingReason),
        decision.composition.compatible ? StringRef("compatible")
                                        : StringRef("not-compatible"),
        decision.composition.globalClassCount,
        decision.composition.completeTupleCount,
        decision.composition.incompleteTupleCount,
        decision.composition.incompatibleTupleCount,
        formatWitnessClosureAtom(
            decision.composition.reason.empty()
                ? StringRef("<none>")
                : StringRef(decision.composition.reason))));
  }
}

void RefoldEngine::TraceWitnessResolverDecision(
    const WitnessResolverDecision &decision) const {
  // Probe and strict modes use the normal proof log as their audit channel.
  if (!ShouldEmitProofLog() && decision.mode == WitnessResolverMode::Off)
    return;

  StringRef authority = "legacy";
  if (decision.mode == WitnessResolverMode::Probe)
    authority = "probe-only";
  else if (decision.mode == WitnessResolverMode::Strict &&
           decision.strictUseResolver)
    authority = "strict-resolver";
  else if (decision.mode == WitnessResolverMode::Strict &&
           decision.strictFailClosed)
    authority = "strict-fail-closed";
  else if (decision.mode == WitnessResolverMode::Strict)
    authority = "strict-fallback-legacy";

  LogProofLine(llvm::formatv(
      "REFOLD-WITNESS-RESOLVER role={0} mode={1} candidates={2} "
      "selectable={3} proof_invalid={4} classes={5} complete={6} "
      "incomplete={7} converted={8} unconverted={9} computed={10} "
      "implemented={11} authority={12} legacy_index={13} "
      "resolver_index={14} agreement={15} reason={16} "
      "fallback_class={17} domain={18}:obligation={19}:reason={20} "
      "composition={21}:classes={22}:complete={23}:"
      "incomplete={24}:incompatible={25}:reason={26}\n",
      decision.role, decision.mode, decision.candidateCount,
      decision.selectableCount, decision.proofInvalidCount,
      decision.equivalenceClassCount, decision.completeWitnessCount,
      decision.incompleteWitnessCount,
      decision.resolverAuthoritativeWitnessCount,
      decision.resolverUnconvertedWitnessCount,
      decision.resolverComputed ? 1 : 0, decision.resolverImplemented ? 1 : 0,
      authority, formatOptionalIndex(decision.legacyIndex),
      formatOptionalIndex(decision.resolverIndex),
      decision.agreement.empty() ? StringRef("<none>")
                                 : StringRef(decision.agreement),
      decision.failureReason.empty() ? StringRef("<none>")
                                     : StringRef(decision.failureReason),
      decision.fallbackClass, decision.strictDomain.domainClass,
      decision.strictDomain.obligation,
      decision.strictDomain.reason.empty()
          ? StringRef("<none>")
          : StringRef(decision.strictDomain.reason),
      decision.composition.compatible ? StringRef("compatible")
                                      : StringRef("not-compatible"),
      decision.composition.globalClassCount,
      decision.composition.completeTupleCount,
      decision.composition.incompleteTupleCount,
      decision.composition.incompatibleTupleCount,
      decision.composition.reason.empty()
          ? StringRef("<none>")
          : StringRef(decision.composition.reason)));

  TraceWitnessStrictDomain(decision.role, decision.strictDomain);
  TraceWitnessClosureLedger(decision);
}

void RefoldEngine::TraceWitnessCompositionDecision(
    llvm::StringRef role, const WitnessCompositionDecision &decision) const {
  if (!ShouldEmitProofLog() &&
      GetWitnessResolverMode() == WitnessResolverMode::Off)
    return;

  LogProofLine(llvm::formatv(
      "REFOLD-WITNESS-COMPOSITION role={0} computed={1} tuples={2} "
      "complete={3} incomplete={4} incompatible={5} classes={6} "
      "compatible={7} fatal={8} reason={9}\n",
      role, decision.computed ? 1 : 0, decision.candidateTupleCount,
      decision.completeTupleCount, decision.incompleteTupleCount,
      decision.incompatibleTupleCount, decision.globalClassCount,
      decision.compatible ? 1 : 0, decision.failureIsFatal ? 1 : 0,
      decision.reason.empty() ? StringRef("<none>")
                              : StringRef(decision.reason)));
}

RefoldEngine::WitnessCompositionDecision
RefoldEngine::ResolveWitnessComposition(
    llvm::StringRef role,
    ArrayRef<std::pair<size_t, RefoldWitness>> selectableWitnesses,
    bool hasSingleConcreteRepairIdentity) const {
  WitnessCompositionDecision decision;
  decision.computed = true;
  decision.candidateTupleCount = selectableWitnesses.size();

  if (selectableWitnesses.empty()) {
    decision.reason = "no-selectable-composition-tuples";
    TraceWitnessCompositionDecision(role, decision);
    return decision;
  }

  std::map<std::string, uint64_t> globalClasses;

  for (const std::pair<size_t, RefoldWitness> &entry : selectableWitnesses) {
    const RefoldWitness &witness = entry.second;
    const WitnessEquivalenceKey &key = witness.key;

    if (key.HasUnknownDimensions()) {
      ++decision.incompleteTupleCount;
      continue;
    }

    if (key.compositionClass == WitnessCompositionClass::Unknown) {
      ++decision.incompleteTupleCount;
      continue;
    }

    if (key.compositionClass == WitnessCompositionClass::Terminal) {
      ++decision.incompatibleTupleCount;
      continue;
    }

    ++decision.completeTupleCount;

    // composition is intentionally tuple-level rather than
    // source-spelling based.  A candidate may be a one-tile local repair or a
    // durable mixed-owner tiling; in both cases the global composition class is
    // the ordered target stream plus the suffix/observer/counter state that the
    // tuple leaves for its neighbors.
    std::string classKey =
        llvm::formatv(
            "target={0}|suffix={1}|observers={2}|counter={3}|boundary={4}|"
            "diagnostics={5}|composition={6}|producers={7}",
            key.targetPPTokens.value, key.suffixState.value,
            key.preservedObservers.value, key.counterState.value,
            key.boundaryClass, key.diagnosticClass, key.compositionClass,
            key.producerKinds)
            .str();
    ++globalClasses[classKey];
  }

  decision.globalClassCount = globalClasses.size();

  if (decision.incompleteTupleCount != 0) {
    decision.reason = "composition-incomplete-witness-key";
  } else if (decision.incompatibleTupleCount != 0) {
    decision.reason = "composition-incompatible-terminal-tuple";
    decision.failureIsFatal = true;
  } else if (decision.globalClassCount == 1) {
    decision.compatible = true;
    decision.reason = "single-global-composition-class";
  } else if (hasSingleConcreteRepairIdentity) {
    decision.compatible = true;
    decision.reason = "single-source-repair-multiple-composition-classes";
  } else {
    decision.reason = "multiple-non-equivalent-composition-classes";
    decision.failureIsFatal = true;
  }

  TraceWitnessCompositionDecision(role, decision);
  return decision;
}

RefoldEngine::WitnessResolverDecision
RefoldEngine::ResolveWitnessesForSelection(
    llvm::StringRef role, size_t candidateCount,
    llvm::function_ref<bool(size_t)> isSelectable,
    llvm::function_ref<RefoldWitness(size_t)> buildWitness,
    llvm::function_ref<bool(size_t, size_t)> canonicalPrefers,
    std::optional<size_t> legacyIndex) const {
  WitnessResolverDecision decision;
  decision.role = role.str();
  decision.mode = GetWitnessResolverMode();
  decision.candidateCount = candidateCount;
  decision.legacyIndex = legacyIndex;

  const bool shouldCompute =
      ShouldEmitProofLog() || decision.mode != WitnessResolverMode::Off;
  if (!shouldCompute)
    return decision;

  decision.resolverComputed = true;

  std::map<std::string, SmallVector<size_t, 4>> classes;
  SmallVector<size_t, 8> selectableIndices;
  SmallVector<std::pair<size_t, RefoldWitness>, 8> selectableWitnesses;

  // Resolves witness *classes*, but a selector can legitimately carry several
  // complete proof certificates for the same concrete source repair. For
  // example, a macro invocation containing both # and ## may be certified by a
  // stringification witness and by a paste witness; those proof keys must stay
  // distinct, but strict mode must not interpret the duplicate certificate as a
  // request to abandon the macro-preserving repair.  Track a conservative
  // source repair identity separately from the semantic proof key so
  // multi-class proof-certificate ambiguity does not become source-repair
  // ambiguity.
  std::optional<std::string> commonRepairIdentity;
  bool hasConcreteRepairIdentity = false;
  bool singleConcreteRepairIdentity = true;

  auto repairIdentityForWitness = [](const RefoldWitness &witness) {
    if (witness.payloadPreview.empty())
      return std::string();

    return llvm::formatv(
               "owner={0}|source_order={1}|range_bytes={2}|"
               "arg_boundary={3}|owner_boundary={4}|payload={5}",
               witness.owner, witness.cost.sourceOrder,
               witness.cost.sourceRangeBytes,
               witness.cost.argumentBoundaryChangePenalty,
               witness.cost.ownerBoundaryChangePenalty,
               witness.payloadPreview)
        .str();
  };

  for (size_t i = 0; i < candidateCount; ++i) {
    RefoldWitness witness = buildWitness(i);
    const bool selectable = isSelectable(i);
    if (!selectable) {
      ++decision.proofInvalidCount;
      TraceWitnessRejected(witness, WitnessRejectReason::NotSelectable,
                           "central witness resolver rejected non-selectable "
                           "candidate");
      continue;
    }

    ++decision.selectableCount;
    selectableIndices.push_back(i);
    selectableWitnesses.push_back(std::make_pair(i, witness));
    if (witness.key.HasUnknownDimensions())
      ++decision.incompleteWitnessCount;
    else
      ++decision.completeWitnessCount;

    if (IsResolverAuthoritativeWitness(witness))
      ++decision.resolverAuthoritativeWitnessCount;
    else
      ++decision.resolverUnconvertedWitnessCount;

    classes[witness.key.PartitionString(witness.witnessId)].push_back(i);

    const std::string repairIdentity = repairIdentityForWitness(witness);
    if (repairIdentity.empty()) {
      singleConcreteRepairIdentity = false;
    } else if (!hasConcreteRepairIdentity) {
      commonRepairIdentity = repairIdentity;
      hasConcreteRepairIdentity = true;
    } else if (*commonRepairIdentity != repairIdentity) {
      singleConcreteRepairIdentity = false;
    }
  }

  // A whole-cover owner-realization candidate is a materialization fallback for
  // the same macro root, not a stronger source repair, when a converted
  // invocation-preserving witness already proves the identical target token
  // envelope for that root.  Dropping only that dominated realization prevents
  // an unconverted raw-expansion certificate from manufacturing a second
  // macro-selection class while keeping owner-realization-only selectors
  // probe-only until that proof family is explicitly converted.
  auto convertedInvocationRepairDominatesOwnerRealization =
      [&](const RefoldWitness &candidate) {
        if (role != "SelectPreferredMacroSelectionCandidate")
          return false;
        if (candidate.family != WitnessProofFamily::OwnerRealization)
          return false;
        if (candidate.sourceFamily != "MacroWholeCoverRealization")
          return false;
        if (candidate.key.HasUnknownDimensions())
          return false;
        if (!candidate.key.targetPPTokens.known)
          return false;
        if (candidate.key.boundaryClass != WitnessBoundaryClass::RootInvocation)
          return false;
        if (candidate.key.diagnosticClass !=
            WitnessDiagnosticClass::RealizesEditedSurface)
          return false;

        for (const std::pair<size_t, RefoldWitness> &entry :
             selectableWitnesses) {
          const RefoldWitness &other = entry.second;
          if (&other == &candidate)
            continue;
          if (!IsResolverAuthoritativeWitness(other))
            continue;
          if (other.key.HasUnknownDimensions())
            continue;
          if (!other.key.targetPPTokens.known)
            continue;
          if (other.owner != candidate.owner)
            continue;
          if (other.key.targetPPTokens.value !=
              candidate.key.targetPPTokens.value)
            continue;
          if (other.key.boundaryClass != WitnessBoundaryClass::RootInvocation)
            continue;
          if (other.key.diagnosticClass !=
              WitnessDiagnosticClass::PreservesDiagnostics)
            continue;
          return true;
        }

        return false;
      };

  SmallVector<std::pair<size_t, RefoldWitness>, 8> effectiveWitnesses;
  SmallVector<size_t, 8> effectiveIndices;
  bool removedDominatedOwnerRealization = false;
  for (const std::pair<size_t, RefoldWitness> &entry : selectableWitnesses) {
    if (convertedInvocationRepairDominatesOwnerRealization(entry.second)) {
      removedDominatedOwnerRealization = true;
      ++decision.proofInvalidCount;
      TraceWitnessRejected(
          entry.second, WitnessRejectReason::NonEquivalentAmbiguity,
          "whole-cover owner realization is dominated by a converted "
          "invocation-preserving witness for the same root and target tokens");
      continue;
    }

    effectiveWitnesses.push_back(entry);
    effectiveIndices.push_back(entry.first);
  }

  if (removedDominatedOwnerRealization) {
    selectableWitnesses = effectiveWitnesses;
    selectableIndices = effectiveIndices;
    decision.selectableCount = selectableWitnesses.size();
    decision.equivalenceClassCount = 0;
    decision.completeWitnessCount = 0;
    decision.incompleteWitnessCount = 0;
    decision.resolverAuthoritativeWitnessCount = 0;
    decision.resolverUnconvertedWitnessCount = 0;
    classes.clear();

    hasConcreteRepairIdentity = false;
    singleConcreteRepairIdentity = true;
    commonRepairIdentity.reset();

    for (const std::pair<size_t, RefoldWitness> &entry : selectableWitnesses) {
      const RefoldWitness &witness = entry.second;
      if (witness.key.HasUnknownDimensions())
        ++decision.incompleteWitnessCount;
      else
        ++decision.completeWitnessCount;

      if (IsResolverAuthoritativeWitness(witness))
        ++decision.resolverAuthoritativeWitnessCount;
      else
        ++decision.resolverUnconvertedWitnessCount;

      classes[witness.key.PartitionString(witness.witnessId)].push_back(
          entry.first);

      const std::string repairIdentity = repairIdentityForWitness(witness);
      if (repairIdentity.empty()) {
        singleConcreteRepairIdentity = false;
      } else if (!hasConcreteRepairIdentity) {
        commonRepairIdentity = repairIdentity;
        hasConcreteRepairIdentity = true;
      } else if (*commonRepairIdentity != repairIdentity) {
        singleConcreteRepairIdentity = false;
      }
    }
  }

  decision.equivalenceClassCount = classes.size();

  decision.composition = ResolveWitnessComposition(
      role, selectableWitnesses,
      hasConcreteRepairIdentity && singleConcreteRepairIdentity);

  TraceWitnessSelectionProbe(role, candidateCount, decision.selectableCount,
                             decision.proofInvalidCount,
                             decision.equivalenceClassCount,
                             decision.completeWitnessCount,
                             decision.incompleteWitnessCount);
  TraceWitnessAmbiguity(role, candidateCount, decision.selectableCount,
                        decision.equivalenceClassCount);

  if (decision.selectableCount == 0) {
    decision.failureReason = "no-selectable-witness";
    decision.fallbackClass = WitnessFallbackClass::NoSelectableWitness;
  } else if (decision.incompleteWitnessCount != 0) {
    // Unknown dimensions do not establish equivalence.  Strict mode therefore
    // falls back to the legacy path for this selector because the proof family
    // has not yet been fully converted.
    decision.failureReason = "incomplete-witness-key";
    decision.fallbackClass = ClassifyIncompleteWitnessKeys(selectableWitnesses);
  } else if (decision.resolverUnconvertedWitnessCount != 0) {
    // A complete key is necessary but not sufficient: the witness must also be
    // accepted by the resolver-authority gate so strict mode cannot become
    // authoritative for an unaudited proof family merely because its trace key
    // happened to be complete.
    decision.failureReason = "unconverted-proof-family";
    decision.fallbackClass = WitnessFallbackClass::UnconvertedProofFamily;
  } else if (!decision.composition.compatible) {
    // Tuple-level composition is an additional authority boundary. Unknown
    // composition facts fall back to the legacy selector; a complete converted
    // selector that proves multiple non-equivalent global tuples fails closed.
    decision.failureReason = decision.composition.reason.empty()
                                 ? "composition-not-proven"
                                 : decision.composition.reason;
    decision.strictFailClosed = decision.composition.failureIsFatal;
    decision.fallbackClass = WitnessFallbackClass::CompositionFailure;
  } else if (decision.equivalenceClassCount != 1) {
    decision.resolverImplemented = true;

    if (hasConcreteRepairIdentity && singleConcreteRepairIdentity) {
      // Multiple complete proof keys can certify the same emitted source edit.
      // That is proof-certificate ambiguity, not source-repair ambiguity. Keep
      // the semantic classes distinct for tracing, but allow strict mode to use
      // the concrete repair selected by the legacy selector until proof
      // normalization can compose proof certificates directly.
      decision.strictUseResolver = true;
      decision.failureReason =
          "single-source-repair-multiple-proof-classes";
      decision.fallbackClass = WitnessFallbackClass::Unknown;
      if (legacyIndex) {
        decision.resolverIndex = legacyIndex;
      } else {
        for (size_t idx : selectableIndices) {
          if (!decision.resolverIndex ||
              canonicalPrefers(idx, *decision.resolverIndex))
            decision.resolverIndex = idx;
        }
      }
    } else {
      // All selectable candidates have complete keys, and they describe
      // different concrete source repairs.  This is the real fail-closed case
      // for converted selector families.
      decision.strictFailClosed = true;
      decision.failureReason = "multiple-non-equivalent-classes";
      decision.fallbackClass =
          WitnessFallbackClass::MultipleNonEquivalentWitnessClasses;
    }
  } else {
    decision.resolverImplemented = true;
    decision.strictUseResolver = true;
    decision.failureReason = "single-equivalence-class";
    decision.fallbackClass = WitnessFallbackClass::Unknown;

    for (size_t idx : selectableIndices) {
      if (!decision.resolverIndex ||
          canonicalPrefers(idx, *decision.resolverIndex))
        decision.resolverIndex = idx;
    }
  }

  if (decision.fallbackClass == WitnessFallbackClass::Unknown &&
      !decision.failureReason.empty())
    decision.fallbackClass =
        ClassifyResolverFallbackReason(decision.failureReason,
                                       decision.composition);

  if (decision.legacyIndex && decision.resolverIndex)
    decision.agreement = (*decision.legacyIndex == *decision.resolverIndex)
                             ? "agree"
                             : "differ";
  else if (decision.legacyIndex && !decision.resolverIndex)
    decision.agreement = "legacy-only";
  else if (!decision.legacyIndex && decision.resolverIndex)
    decision.agreement = "resolver-only";
  else
    decision.agreement = "neither";

  decision.strictDomain = ClassifyStrictDomainForResolver(decision);
  PopulateWitnessClosureLedger(decision, selectableWitnesses);
  RecordWitnessResolverTheoremAudit(decision);

  TraceWitnessResolverDecision(decision);
  return decision;
}

void RefoldEngine::TraceWitnessEmitted(const RefoldWitness &witness) const {
  if (!ShouldEmitProofLog())
    return;

  LogProofLine(llvm::formatv("REFOLD-WITNESS {0}\n", witness));
  LogProofLine(llvm::formatv("REFOLD-WITNESS-KEY id={0} {1}\n",
                             witness.witnessId, witness.key));
  LogProofLine(llvm::formatv("REFOLD-WITNESS-COST id={0} {1}\n",
                             witness.witnessId, witness.cost));
  if (!witness.payloadPreview.empty())
    LogProofLine(llvm::formatv("REFOLD-WITNESS-PAYLOAD id={0} text={1}\n",
                            witness.witnessId, witness.payloadPreview));
}

void RefoldEngine::TraceWitnessRejected(const RefoldWitness &witness,
                                        WitnessRejectReason reason,
                                        llvm::StringRef detail) const {
  if (!ShouldEmitProofLog())
    return;

  LogProofLine(llvm::formatv("REFOLD-WITNESS-REJECT id={0} family={1} "
                          "owner={2} reason={3} detail={4}\n",
                          witness.witnessId, witness.family, witness.owner,
                          reason, detail.empty() ? StringRef("<none>")
                                                 : detail));
}

void RefoldEngine::TraceWitnessAmbiguity(llvm::StringRef role,
                                         uint64_t candidateCount,
                                         uint64_t selectableCount,
                                         uint64_t ambiguityClassCount) const {
  if (!ShouldEmitProofLog())
    return;

  LogProofLine(llvm::formatv("REFOLD-WITNESS-AMBIGUITY role={0} "
                          "candidates={1} selectable={2} classes={3}\n",
                          role, candidateCount, selectableCount,
                          ambiguityClassCount));
}

void RefoldEngine::TraceWitnessSelectionProbe(
    llvm::StringRef role, uint64_t candidateCount,
    uint64_t proofValidCount, uint64_t proofInvalidCount,
    uint64_t equivalenceClassCount, uint64_t completeWitnessCount,
    uint64_t incompleteWitnessCount) const {
  if (!ShouldEmitProofLog())
    return;

  StringRef preferenceScope = "no-valid-witness";
  if (proofValidCount == 1)
    preferenceScope = "single-valid-witness";
  else if (proofValidCount > 1 && equivalenceClassCount == 1)
    preferenceScope = "one-equivalence-class";
  else if (proofValidCount > 1 && equivalenceClassCount > 1)
    preferenceScope = "multiple-equivalence-classes";

  LogProofLine(llvm::formatv(
      "REFOLD-WITNESS-SELECTION role={0} candidates={1} "
      "proof_valid={2} proof_invalid={3} classes={4} "
      "complete={5} incomplete={6} preference_scope={7}\n",
      role, candidateCount, proofValidCount, proofInvalidCount,
      equivalenceClassCount, completeWitnessCount, incompleteWitnessCount,
      preferenceScope));
}

void RefoldEngine::TraceWitnessChosen(const RefoldWitness &witness,
                                      uint64_t selectedIndex) const {
  if (!ShouldEmitProofLog())
    return;

  LogProofLine(llvm::formatv("REFOLD-WITNESS-CHOOSE index={0} id={1} "
                          "family={2} owner={3} cost={4}\n",
                          selectedIndex, witness.witnessId, witness.family,
                          witness.owner, witness.cost));
}

void RefoldEngine::TraceWitnessFallback(
    const TerminalFallbackRequest &request) const {
  if (!ShouldEmitProofLog())
    return;

  const WitnessFallbackClass fallbackClass =
      ClassifyTerminalFallbackFailure(request.failure);
  const WitnessStrictDomainDecision domain =
      ClassifyStrictDomainForTerminalFallback(request.failure);

  LogProofLine(llvm::formatv(
      "REFOLD-WITNESS-FALLBACK reason={0} "
      "fallback_class={1} domain={2} obligation={3} "
      "domain_reason={4} stage={5} detail={6}\n",
      request.failure, fallbackClass, domain.domainClass, domain.obligation,
      domain.reason.empty() ? StringRef("<none>") : StringRef(domain.reason),
      request.stage, stringutils::showWsWithClip(request.detail, 200)));
  TraceWitnessStrictDomain("TerminalFallback", domain);

  if (domain.domainClass ==
      WitnessStrictDomainClass::PotentiallyInDomainMissingProof) {
    StringRef missing = "unknown";
    switch (fallbackClass) {
    case WitnessFallbackClass::UnknownTargetPreprocessedTokens:
      missing = "target_pp";
      break;
    case WitnessFallbackClass::UnknownSuffixState:
      missing = "suffix_state";
      break;
    case WitnessFallbackClass::UnprovenProducerKind:
    case WitnessFallbackClass::NoSelectableWitness:
      missing = "producer";
      break;
    case WitnessFallbackClass::CounterStateMismatch:
      missing = "counter";
      break;
    case WitnessFallbackClass::LineControlObserverMismatch:
      missing = "observers";
      break;
    case WitnessFallbackClass::CompositionFailure:
      missing = "composition";
      break;
    case WitnessFallbackClass::UnconvertedProofFamily:
      missing = "converted_family";
      break;
    case WitnessFallbackClass::IncompleteWitnessKey:
      missing = "complete_key";
      break;
    case WitnessFallbackClass::NoOwnerClosedWitness:
      missing = "owner_closure";
      break;
    case WitnessFallbackClass::ValidationFailure:
      missing = "diagnostics";
      break;
    case WitnessFallbackClass::Unknown:
    case WitnessFallbackClass::MultipleNonEquivalentWitnessClasses:
    case WitnessFallbackClass::InvalidMacroInvocation:
    case WitnessFallbackClass::InvalidPasteResult:
    case WitnessFallbackClass::UnsupportedDirectiveInteraction:
      break;
    }

    LogProofLine(llvm::formatv(
        "REFOLD-WITNESS-CLOSURE selector=TerminalFallback "
        "family={0} test_region=terminal missing={1} "
        "source_family=TerminalFallback candidate_kind=TerminalOutOfDomain "
        "theorem=TerminalFallback witness_id=0 candidate_index=0 "
        "candidate_count=1 selectable=0 complete=0 incomplete=1 "
        "converted=0 unconverted=1 classes=0 owner_kind=terminal "
        "owner=terminal fallback_class={2} obligation={3} reason={4} "
        "missing_reason={5} "
        "composition=not-compatible:classes=0:complete=0:incomplete=1:"
        "incompatible=0:reason={6}\n",
        WitnessProofFamily::TerminalFallback, formatWitnessClosureAtom(missing),
        fallbackClass, domain.obligation,
        formatWitnessClosureAtom(domain.reason.empty()
                                     ? StringRef("<none>")
                                     : StringRef(domain.reason)),
        formatWitnessClosureAtom(request.failure.ToString()),
        formatWitnessClosureAtom(request.stage.empty()
                                     ? StringRef("<none>")
                                     : StringRef(request.stage))));
  }
}

void RefoldEngine::AttachMixedOwnerTilingWitnessForTokenEnvelope(
    ProofSummary &summary, uint64_t aStart, uint64_t aEnd, uint64_t bStart,
    uint64_t bEnd) const {
  // Keeps mixed-owner splitting as a normalization step, but it no longer lets
  // path-local binding order decide which proof wins.  Every matching durable
  // segment is converted into a candidate ProofSummary and the shared lattice
  // chooses both whether the mixed-owner overlay beats the owner-specific
  // summary and which matching tiling witness is strongest.
  std::optional<ProofSummary> selectedSummary;

  for (const MixedOwnerTilingSegmentBinding &binding :
       mixedOwnerTilingSegmentBindings_) {
    if (binding.aStart != aStart || binding.aEnd != aEnd ||
        binding.bStart != bStart || binding.bEnd != bEnd)
      continue;
    if (binding.witnessIndex >= mixedOwnerTilingWitnesses_.size())
      continue;

    const MixedOwnerTilingWitness &witness =
        mixedOwnerTilingWitnesses_[binding.witnessIndex];
    if (witness.witnessId != binding.parentTilingWitnessId ||
        binding.segmentIndex >= witness.segments.size())
      continue;

    const MixedOwnerTilingSegmentWitness &segment =
        witness.segments[binding.segmentIndex];
    if (segment.parentTilingWitnessId != witness.witnessId ||
        segment.segmentIndex != binding.segmentIndex ||
        segment.kind != MixedOwnerTilingEdgeKind::TokenSegment ||
        segment.aStart != aStart || segment.aEnd != aEnd ||
        segment.bStart != bStart || segment.bEnd != bEnd)
      continue;

    ProofSummary candidate = summary;
    candidate.hasMixedOwnerTilingWitness = true;
    candidate.mixedOwnerTilingWitness = witness;
    candidate.theoremClass = TheoremProofClass::MixedOwnerTilingProof;
    candidate.primaryProofClassExplicit = true;
    FinalizeProofSummary(candidate);

    if (LatticePrefers(summary, candidate))
      continue;

    if (!selectedSummary || LatticePrefers(candidate, *selectedSummary))
      selectedSummary = std::move(candidate);
  }

  if (selectedSummary)
    summary = std::move(*selectedSummary);
}

RefoldEngine::OwnerRealizationResult
RefoldEngine::TryBuildOwnerRealization(OwnerRealizationEvidenceKind evidence,
                                       OwnerClosure closure,
                                       StringRef detail) const {
  OwnerRealizationResult result;
  result.witness.evidence = evidence;
  result.witness.closure = AttachCanonicalStateSummary(std::move(closure));
  result.witness.detail = detail.str();
  result.detail = detail.str();

  // The shared realization gate is deliberately owner-polymorphic: callers may
  // still know how to spell a macro callsite, inline an include body, or write
  // a direct TU byte edit, but they no longer get to invent separate proof
  // rules for the common closure facts below.
  const bool hasOwner = result.witness.closure.owner.IsKnown();
  const bool hasSourceInterval = result.witness.closure.source.IsComplete();
  const bool hasATokenCover = result.witness.closure.aTokens.IsValid();
  const bool hasBTokenEnvelope = result.witness.closure.bTokens.IsValid();

  auto addUniqueComponent = [](std::vector<OwnerStateComponent> &components,
                               OwnerStateComponent component) {
    if (component == OwnerStateComponent::Unknown)
      return;
    if (llvm::none_of(components, [&](OwnerStateComponent existing) {
          return existing == component;
        }))
      components.push_back(component);
  };

  std::vector<OwnerStateComponent> mutatedComponents;
  for (OwnerStateComponent component :
       StateComponentsMutatedByDelta(result.witness.closure.stateIn))
    addUniqueComponent(mutatedComponents, component);
  for (OwnerStateComponent component :
       StateComponentsMutatedByDelta(result.witness.closure.stateOut))
    addUniqueComponent(mutatedComponents, component);

  // Owner realization proves stateful closures by widening the realized owner
  // through each component it mutates.  Keep those proofs as typed witnesses
  // instead of projecting the result through the old coarse discharge enum.
  const OwnerStateBoundary stateBoundary =
      OwnerStateBoundary::FromSourceAndATokens(result.witness.closure.source,
                                               result.witness.closure.aTokens);
  for (OwnerStateComponent component : mutatedComponents) {
    ClosureWideningWitness stateWitness;
    stateWitness.component = component;
    stateWitness.boundary = stateBoundary;
    stateWitness.detail = detail.str();
    result.witness.stateWitnesses.push_back(
        SuffixStabilityWitness::From(std::move(stateWitness)));
  }
  auto reject = [&](TerminalFallbackObligationKind obligation,
                    TerminalFallbackFailureReason reason) {
    result.accepted = false;
    TerminalFallbackFailureContext context;
    context.owner = toString(result.witness.closure.owner.kind).str();
    if (result.witness.closure.source.HasPath())
      context.sourcePath = result.witness.closure.source.path;
    if (result.witness.closure.source.IsValid()) {
      context.sourceBegin = result.witness.closure.source.begin;
      context.sourceEnd = result.witness.closure.source.end;
    }
    if (result.witness.closure.aTokens.IsValid()) {
      context.aTokenBegin = result.witness.closure.aTokens.begin;
      context.aTokenEnd = result.witness.closure.aTokens.end;
    }
    if (result.witness.closure.bTokens.IsValid()) {
      context.bTokenBegin = result.witness.closure.bTokens.begin;
      context.bTokenEnd = result.witness.closure.bTokens.end;
    }
    result.failure = MakeTerminalFallbackProofFailure(obligation, reason,
                                                      std::move(context));
    REFOLD_LOG_TRACE("proof/owner-realization",
          "reject owner realization evidence={0} owner={1}: {2} detail='{3}'",
          evidence, result.witness.closure.owner.kind, result.failure,
          result.detail);
    return result;
  };

  if (evidence == OwnerRealizationEvidenceKind::Unknown)
    return reject(TerminalFallbackObligationKind::ProducerFactsAvailable,
                  TerminalFallbackFailureReason::MissingProducerFacts);

  if (!hasOwner || !hasSourceInterval || !hasATokenCover)
    return reject(TerminalFallbackObligationKind::OwnerClosedCover,
                  TerminalFallbackFailureReason::NoOwnerClosedCover);

  if (!hasBTokenEnvelope)
    return reject(TerminalFallbackObligationKind::ProducerFactsAvailable,
                  TerminalFallbackFailureReason::MissingProducerFacts);

  // A state-neutral owner needs no suffix-stability witness.  Otherwise the
  // realization gate must have produced at least one typed state witness for
  // the component-specific mutations carried by the owner closure.
  const bool mutatesOrCarriesUnmodeledState =
      OwnerStateDeltaMutatesAnyState(result.witness.closure.stateIn) ||
      OwnerStateDeltaMutatesAnyState(result.witness.closure.stateOut);
  if (mutatesOrCarriesUnmodeledState && result.witness.stateWitnesses.empty())
    return reject(
        TerminalFallbackObligationKind::StateTransitionClosure,
        TerminalFallbackFailureReason::StateTransitionConsumedAndObserved);

  result.accepted = true;
  REFOLD_LOG_TRACE("proof/owner-realization",
        "accept owner realization evidence={0} owner={1} source='{2}'[{3},{4}) "
        "A=[{5},{6}) B=[{7},{8}) stateWitnesses={9} detail='{10}'",
        evidence, result.witness.closure.owner.kind,
        result.witness.closure.source.path, result.witness.closure.source.begin,
        result.witness.closure.source.end, result.witness.closure.aTokens.begin,
        result.witness.closure.aTokens.end,
        result.witness.closure.bTokens.begin,
        result.witness.closure.bTokens.end,
        static_cast<uint64_t>(result.witness.stateWitnesses.size()),
        result.detail);
  return result;
}

void RefoldEngine::ApplyOwnerRealizationResultToProofSummary(
    ProofSummary &summary, const OwnerRealizationResult &result) const {
  // Makes TryBuildOwnerRealization() the authoritative common proof gate for
  // realized owners.  Macro/include/TU callers may still construct their
  // replacement bytes with owner-specific code, but once they delegate to the
  // shared helper they must not keep a realized candidate selectable after that
  // helper rejects the owner/source/A-cover/B-envelope/state obligations.
  summary.ownerRealizationWitness = result.witness;
  summary.hasOwnerRealizationWitness = result.accepted;

  ++summary.discharge.obligationsEvaluated;
  if (result.accepted) {
    ++summary.discharge.obligationsSatisfied;
    FinalizeProofSummary(summary);
    return;
  }

  summary.discharge.status = ProofDischargeStatus::Rejected;
  if (summary.discharge.failedObligation == ProofObligationKind::Unknown)
    summary.discharge.failedObligation =
        ProofObligationKind::OwnerRealizationWitnessTracked;
  if (summary.discharge.failureReason == ProofFailureReason::None)
    summary.discharge.failureReason =
        ProofFailureReason::MissingOwnerRealizationWitness;

  REFOLD_LOG_TRACE("proof/owner-realization",
        "reject accepted candidate after owner-realization gate failed: {0} "
        "detail='{1}'",
        result.failure, result.detail);
  FinalizeProofSummary(summary);
}

RefoldEngine::OwnerRealizationResult
RefoldEngine::BuildMacroWholeCoverOwnerRealization(
    const RefoldModel::MacroInvocation &macro,
    const WholeCoverPlan &plan) const {
  const std::string sourcePath =
      macro.invFile ? macro.invFile->str() : model_.GetSourcePath().str();
  const bool hasSourceInterval = macro.invB && macro.invE;
  const uint64_t sourceBegin = hasSourceInterval ? *macro.invB : 1;
  const uint64_t sourceEnd = hasSourceInterval ? *macro.invE : 0;

  OwnerClosure closure = OwnerClosure::From(
      Owner::MacroInvocation(macro.id),
      OwnerSourceRange::From(sourcePath, sourceBegin, sourceEnd,
                             macro.ownerIncludeId),
      OwnerTokenRange::From(plan.covLoA, plan.covHiA),
      OwnerTokenRange::From(plan.bTokStart, plan.bTokEnd));

  // Whole-cover macro realization replaces the invocation's entire expansion
  // envelope.  Any state observation/mutation that the macro path needed to
  // preserve must therefore have been discharged by the caller before the path
  // reached this shared proof gate.
  return TryBuildOwnerRealization(
      OwnerRealizationEvidenceKind::MacroWholeCover, std::move(closure),
      formatv("macroId={0} whole-cover", macro.id).str());
}

RefoldEngine::OwnerRealizationResult
RefoldEngine::BuildIncludeOwnerRealization(
    const RefoldModel::IncludeItem &include, AcceptedPathKind currentPath,
    IncludeRealizationEvidenceKind evidenceKind,
    std::optional<IncludeRealizationBTokenEnvelope> bTokenEnvelope) const {
  std::optional<OwnerTokenRange> aTokens;
  std::optional<OwnerTokenRange> bTokens;

  if (bTokenEnvelope) {
    // Inline include realization from B supplies an explicit producer-proven
    // B envelope.  Do not repackage that fact as an include-specific theorem
    // witness: the owner-polymorphic realization gate below validates the
    // common A-cover/B-envelope obligations exactly once.
    aTokens = OwnerTokenRange::From(include.cover.begin, include.cover.end);
    bTokens = OwnerTokenRange::From(bTokenEnvelope->first,
                                    bTokenEnvelope->second);
  } else {
    // Materialized include expansion is still an owner realization even when
    // no ordinary B-token envelope exists.  Use the include expansion spans as
    // the conservative A cover, and use the classified zero-token B envelope so
    // the shared proof audit sees an explicit realized owner rather than a
    // hidden fallback path.
    uint64_t begin = std::numeric_limits<uint64_t>::max();
    uint64_t end = 0;
    for (const RefoldModel::PPSpan &span : include.spans) {
      if (!span.IsValid())
        continue;
      begin = std::min<uint64_t>(begin, span.begin);
      end = std::max<uint64_t>(end, span.end);
    }
    if (begin != std::numeric_limits<uint64_t>::max())
      aTokens = OwnerTokenRange::From(begin, end);

    if (!aTokens &&
        currentPath == AcceptedPathKind::IncludeMaterializedExpansion) {
      // Sideband-only headers can have no ordinary PP-token spans after the
      // pragma sideband normalizer removes preserved directives from the token
      // stream.  The concrete include occurrence and source interval still
      // identify the owner, while the normal-token cover is the empty include
      // cover.
      aTokens = OwnerTokenRange::From(include.cover.begin, include.cover.end);
    }
    if (currentPath == AcceptedPathKind::IncludeMaterializedExpansion)
      bTokens = OwnerTokenRange::From(0, 0);
  }

  OwnerClosure closure = OwnerClosure::From(
      Owner::Include(include.id),
      OwnerSourceRange::From(include.sitePath, include.siteB, include.siteE,
                             include.parent),
      aTokens.value_or(OwnerTokenRange::From(1, 0)),
      bTokens.value_or(OwnerTokenRange::From(1, 0)));

  const OwnerRealizationEvidenceKind evidence =
      currentPath == AcceptedPathKind::IncludeMaterializedExpansion
          ? OwnerRealizationEvidenceKind::IncludeMaterializedExpansion
          : OwnerRealizationEvidenceKind::IncludeBEnvelope;

  const std::string detail =
      formatv("includeId={0} path={1} includeEnvelopeEvidence={2}", include.id,
              currentPath, evidenceKind)
          .str();

  // Include realization is owner-specific spelling, but include identity and
  // include-guard effects are semantic state transitions.  Route those
  // components through the common state-transition gateway with typed witnesses
  // while keeping the owner-realization gate as the only closure validator.
  const OwnerStateBoundary includeBoundary =
      IncludeStateBoundaryForIncludeSite(include);
  (void)CheckStateTransitionAcrossEditBoundary(
      includeBoundary, OwnerStateComponent::IncludeState,
      StateMutationKind::Materialized,
      BuildStateTransitionWitness(
          SuffixStabilityWitnessKind::OwnerMaterialization,
          OwnerStateComponent::IncludeState, includeBoundary, detail),
      "include-owner-realization", detail, /*requireKnownObserver=*/false);
  (void)CheckStateTransitionAcrossEditBoundary(
      includeBoundary, OwnerStateComponent::IncludeGuardState,
      StateMutationKind::WidenedIntoClosure,
      BuildStateTransitionWitness(SuffixStabilityWitnessKind::ClosureWidening,
                                  OwnerStateComponent::IncludeGuardState,
                                  includeBoundary, detail),
      "include-owner-realization", detail, /*requireKnownObserver=*/false);

  return TryBuildOwnerRealization(evidence, std::move(closure), detail);
}

RefoldEngine::OwnerRealizationResult
RefoldEngine::BuildTUOwnerRealization(AcceptedPathKind currentPath,
                                      uint64_t begin, uint64_t end) const {
  OwnerClosure closure = OwnerClosure::From(
      Owner::TU(), OwnerSourceRange::From(model_.GetSourcePath(), begin, end),
      OwnerTokenRange::From(0, 0), OwnerTokenRange::From(0, 0));

  // Direct TU byte-span realization has already had its concrete byte spelling
  // selected before this helper is called.  The shared proof gate records only
  // that the TU owner and byte interval have a closed realization carrier.
  return TryBuildOwnerRealization(
      OwnerRealizationEvidenceKind::TUByteSpan, std::move(closure),
      formatv("tu-byte-edit path={0}", currentPath).str());
}

void RefoldEngine::StampMacroWholeCoverRealizationPatch(
    MacroPatch &patch, const WholeCoverPlan &plan,
    const RefoldModel::MacroInvocation &macro) const {
  // Promote accepted whole-cover output into an explicit invocation
  // realization proof. The plan already carries the exact A/B token envelope
  // and containment facts, so stamping it here keeps the accepted patch
  // deterministic and fully described without changing selection behavior.
  patch.wholeCoverUsedBodyRange = plan.usedBodyRange;
  patch.wholeCoverSelfContained = plan.selfContained;
  patch.wholeCoverAdjustedLeft = plan.adjustedLeft;
  patch.wholeCoverAdjustedRight = plan.adjustedRight;
  patch.wholeCoverClaimsClipped = plan.claimsClipped;
  patch.wholeCoverALo = plan.covLoA;
  patch.wholeCoverAHi = plan.covHiA;
  patch.wholeCoverBRawLo = plan.rawBTokStart;
  patch.wholeCoverBRawHi = plan.rawBTokEnd;
  patch.wholeCoverBAdjLo = plan.bTokStart;
  patch.wholeCoverBAdjHi = plan.bTokEnd;
  patch.hasMaterializedBTokenRange = true;
  patch.materializedBTokStart = plan.bTokStart;
  patch.materializedBTokEnd = plan.bTokEnd;
  const OwnerRealizationResult ownerRealization =
      BuildMacroWholeCoverOwnerRealization(macro, plan);
  MacroPatchProof proof =
      MakeMacroPatchProof(MacroPatchProofKind::WholeCoverRealization,
                          /*preservesInvocationStructure=*/false, macro.id);
  if (ownerRealization.accepted)
    proof.ownerRealization = ownerRealization.witness;
  SetMacroPatchProof(patch, std::move(proof));
}

RefoldEngine::ProofSummary RefoldEngine::BuildAcceptedPathProofSummary(
    AcceptedPathKind currentPath, const IncludePatch *patch,
    const TUAnchorWitness *tuAnchorWitness,
    const IncludeAnchorWitness *includeAnchorWitness,
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
    ConfigureProofSummary(summary,
                          BuildTheoremProofClassForAcceptedPath(currentPath),
                          AcceptedProofClass::IncludePreserving,
                          RealizationMode::PreserveOriginalStructure,
                          SelectionPreference::PreferStructurePreservation,
                          SurfaceDisposition::None,
                          /*preservesInvocationStructure=*/true);
    if (includeAnchorWitness) {
      summary.hasIncludeAnchorWitness = true;
      summary.includeAnchorWitness = *includeAnchorWitness;
    }
    summary.discharge = ValidateIncludePreservingProof(currentPath, patch,
                                                       includeAnchorWitness);
    break;

  case AcceptedPathKind::IncludeRealizationInlineFromB:
    ConfigureProofSummary(summary,
                          BuildTheoremProofClassForAcceptedPath(currentPath),
                          AcceptedProofClass::IncludeRealization,
                          RealizationMode::RealizeEditedSurface,
                          SelectionPreference::PreferSurfaceRealization,
                          SurfaceDisposition::RealizeInlineTouchedIncludesFromB,
                          /*preservesInvocationStructure=*/false);
    // removes the last include-specific realization validator.  The
    // include path itself only needs the generic accepted-path baseline here;
    // BuildIncludeOwnerRealization() and
    // ApplyOwnerRealizationResultToProofSummary() attach the authoritative
    // OwnerRealizationProof immediately after the owner-specific include
    // spelling has supplied its evidence.
    summary.discharge = BuildAcceptedPathBaselineDischarge(summary.inventory);
    break;

  case AcceptedPathKind::IncludeMaterializedExpansion: {
    ConfigureProofSummary(
        summary, BuildTheoremProofClassForAcceptedPath(currentPath),
        AcceptedProofClass::IncludeRealization,
        RealizationMode::RealizeEditedSurface,
        SelectionPreference::PreferSurfaceRealization,
        SurfaceDisposition::RealizeMaterializedIncludeExpansion,
        /*preservesInvocationStructure=*/false);
    summary.discharge = BuildAcceptedPathBaselineDischarge(summary.inventory);
    break;
  }

  case AcceptedPathKind::TUExactSlotBoundary:
  case AcceptedPathKind::TUProvableInsertionAnchor:
    ConfigureProofSummary(
        summary, BuildTheoremProofClassForAcceptedPath(currentPath),
        AcceptedProofClass::TUAnchor,
        RealizationMode::PreserveOriginalStructure,
        SelectionPreference::PreferExactAnchoring, SurfaceDisposition::None,
        /*preservesInvocationStructure=*/true);
    if (tuAnchorWitness) {
      summary.hasTUAnchorWitness = true;
      summary.tuAnchorWitness = *tuAnchorWitness;
    }
    summary.discharge = ValidateTUAnchorProof(currentPath, tuAnchorWitness);
    break;

  case AcceptedPathKind::TUByteSpanMappedEdit:
  case AcceptedPathKind::TUByteSpanConservativeEdit:
  case AcceptedPathKind::TUIncludeClosureEdit: {
    ConfigureProofSummary(summary,
                          BuildTheoremProofClassForAcceptedPath(currentPath),
                          AcceptedProofClass::TUTextualEdit,
                          RealizationMode::RealizeEditedSurface,
                          SelectionPreference::PreferSurfaceRealization,
                          SurfaceDisposition::RealizeTranslationUnitByteEdit,
                          /*preservesInvocationStructure=*/false);
    summary.discharge = BuildAcceptedPathBaselineDischarge(summary.inventory);
    break;
  }

  case AcceptedPathKind::TerminalEmitEditedPreprocessedStream: {
    if (terminalFallbackWitness) {
      summary.terminalFallbackWitness = *terminalFallbackWitness;
    }
    ConfigureProofSummary(summary,
                          BuildTheoremProofClassForAcceptedPath(currentPath),
                          AcceptedProofClass::TerminalOutOfDomain,
                          RealizationMode::RealizeEditedSurface,
                          SelectionPreference::PreferSurfaceRealization,
                          SurfaceDisposition::EmitEditedPreprocessedStream,
                          /*preservesInvocationStructure=*/false);
    summary.discharge = BuildAcceptedPathBaselineDischarge(
        summary.inventory, /*explicitOutOfDomain=*/true);
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
    break;
  }

  FinalizeProofSummary(summary);
  return summary;
}

RefoldEngine::ProofSummary
RefoldEngine::BuildIncludePatchProofSummary(bool realizedSurface,
                                            AcceptedPathKind currentPath,
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

  return BuildAcceptedPathProofSummary(currentPath, patch);
}

RefoldEngine::EmittedProof
RefoldEngine::BuildEmittedProofFromSummary(TheoremProofClass theoremClass,
                                           const ProofSummary &summary) {
  EmittedProof proof;
  proof.theoremClass = theoremClass;
  proof.discharge = summary.discharge;

  if (summary.hasOwnerRealizationWitness)
    proof.ownerRealization = summary.ownerRealizationWitness;
  if (summary.hasMixedOwnerTilingWitness)
    proof.mixedOwnerTiling = summary.mixedOwnerTilingWitness;
  if (summary.hasTUAnchorWitness)
    proof.tuAnchor = summary.tuAnchorWitness;
  if (summary.hasIncludeAnchorWitness)
    proof.includeAnchor = summary.includeAnchorWitness;
  if (summary.hasSuffixStabilityWitness)
    proof.suffixStability = summary.suffixStabilityWitness;
  if (summary.terminalFallbackWitness.has_value())
    proof.terminalFallback = summary.terminalFallbackWitness;

  return proof;
}

std::optional<RefoldEngine::EmittedProof>
RefoldEngine::BuildCanonicalEmittedProofFromSummary(
    const ProofSummary &summary) const {
  // summary-owned theorem gate. AcceptedProofClass is now only
  // construction provenance; it may describe where a candidate came from, but
  // it must not answer why that candidate is theorem-admissible.  The final
  // theorem answer is ProofSummary::theoremClass plus the typed witnesses and
  // discharge/domain records checked below.
  const bool terminalSummary =
      summary.inventory.currentPath ==
          AcceptedPathKind::TerminalEmitEditedPreprocessedStream ||
      summary.theoremClass == TheoremProofClass::TerminalOutOfDomainProof;

  if (terminalSummary) {
    // Terminal fallback is a theorem carrier only when every summary-owned
    // proof fact agrees that the result is the named out-of-domain boundary.
    // No caller may use the historical fallback path itself as proof.
    if (summary.theoremClass != TheoremProofClass::TerminalOutOfDomainProof)
      return std::nullopt;
    if (summary.inventory.currentPath !=
        AcceptedPathKind::TerminalEmitEditedPreprocessedStream)
      return std::nullopt;
    if (!summary.primaryProofClassExplicit)
      return std::nullopt;
    if (!summary.terminalFallbackWitness)
      return std::nullopt;
    const TerminalFallbackWitness &terminalWitness =
        *summary.terminalFallbackWitness;
    const TerminalFallbackProofFailure *primaryFailure =
        terminalWitness.PrimaryFailure();
    if (!primaryFailure)
      return std::nullopt;
    if (!IsClassifiedTerminalFallbackProofFailure(*primaryFailure))
      return std::nullopt;
    for (const TerminalFallbackProofFailure &failure :
         terminalWitness.proofFailures)
      if (!IsClassifiedTerminalFallbackProofFailure(failure))
        return std::nullopt;
    if (summary.theoremDomain.kind !=
            TheoremDomainKind::ExplicitOutOfDomainClass ||
        summary.completeness.coverage !=
            CompletenessCoverageKind::ExplicitOutOfDomainClass) {
      return std::nullopt;
    }
    if (summary.discharge.status != ProofDischargeStatus::Rejected ||
        summary.discharge.failedObligation !=
            ProofObligationKind::ExplicitOutOfDomainResultTracked ||
        summary.discharge.failureReason !=
            ProofFailureReason::ExplicitOutOfDomainResult) {
      return std::nullopt;
    }
    return BuildEmittedProofFromSummary(summary.theoremClass, summary);
  }

  // Non-terminal byte edits must be in-domain theorem carriers. Reject every
  // transitional, implicit, or undischarged summary before accepting the final
  // theorem class declared on the summary.  AcceptedProofClass deliberately
  // does not appear in this gate.
  if (summary.inventory.currentPath == AcceptedPathKind::Unknown)
    return std::nullopt;
  if (summary.inventory.support != AcceptanceSupportKind::ExplicitProofBacked)
    return std::nullopt;
  if (summary.theoremClass == TheoremProofClass::Unknown ||
      summary.theoremClass == TheoremProofClass::TerminalOutOfDomainProof ||
      !summary.primaryProofClassExplicit)
    return std::nullopt;
  if (summary.discharge.status != ProofDischargeStatus::Discharged)
    return std::nullopt;
  if (summary.completeness.coverage !=
          CompletenessCoverageKind::DeclaredProofClass ||
      summary.theoremDomain.kind != TheoremDomainKind::DeclaredInDomainClass ||
      !summary.theoremDomain.inDeclaredDomain) {
    return std::nullopt;
  }
  if (ProofSummaryRequiresOwnerRealizationWitness(summary) &&
      !summary.hasOwnerRealizationWitness &&
      summary.theoremClass != TheoremProofClass::MixedOwnerTilingProof) {
    return std::nullopt;
  }

  switch (summary.theoremClass) {
  case TheoremProofClass::MixedOwnerTilingProof: {
    if (!summary.hasMixedOwnerTilingWitness)
      return std::nullopt;
    const MixedOwnerTilingWitness &witness = summary.mixedOwnerTilingWitness;
    if (!witness.stateSummariesComposed || !witness.ownerBoundariesComposed ||
        !witness.targetTokenStreamComposed || !witness.compositionEdgesProven ||
        witness.tokenSegmentCount == 0 || witness.segments.empty() ||
        witness.globalTargetPPTokenSignature.empty() ||
        witness.globalCompositionSignature.empty() ||
        witness.originalAEnd < witness.originalAStart ||
        witness.originalBEnd < witness.originalBStart ||
        witness.originalBEnd > bToks_.size()) {
      return std::nullopt;
    }

    uint64_t expectedA = witness.originalAStart;
    uint64_t expectedB = witness.originalBStart;
    uint32_t tokenSegmentCount = 0;
    uint32_t stateGapCount = 0;
    for (size_t i = 0; i < witness.segments.size(); ++i) {
      const MixedOwnerTilingSegmentWitness &segment = witness.segments[i];
      if (segment.parentTilingWitnessId != witness.witnessId ||
          segment.segmentIndex != i || !segment.ownerClosureComplete ||
          segment.ownerSignature.empty() || segment.sourceSignature.empty() ||
          segment.producerPathSignature.empty() ||
          segment.targetPPTokenSignature.empty() ||
          segment.aEnd < segment.aStart || segment.bEnd < segment.bStart ||
          segment.bEnd > bToks_.size()) {
        return std::nullopt;
      }

      switch (segment.kind) {
      case MixedOwnerTilingEdgeKind::TokenSegment:
        if (segment.zeroTokenStateGap || segment.aStart != expectedA ||
            segment.bStart != expectedB || segment.aEnd <= segment.aStart ||
            (!segment.allowEmptyBEnvelope && segment.bEnd <= segment.bStart)) {
          return std::nullopt;
        }
        expectedA = segment.aEnd;
        expectedB = segment.bEnd;
        ++tokenSegmentCount;
        break;

      case MixedOwnerTilingEdgeKind::StateGap:
        if (!segment.zeroTokenStateGap || segment.aStart != expectedA ||
            segment.aEnd != expectedA || segment.bStart != expectedB ||
            segment.bEnd != expectedB) {
          return std::nullopt;
        }
        ++stateGapCount;
        break;

      case MixedOwnerTilingEdgeKind::Unknown:
        return std::nullopt;
      }
    }

    if (expectedA != witness.originalAEnd ||
        expectedB != witness.originalBEnd ||
        tokenSegmentCount != witness.tokenSegmentCount ||
        stateGapCount != witness.stateGapCount) {
      return std::nullopt;
    }

    return BuildEmittedProofFromSummary(summary.theoremClass, summary);
  }

  case TheoremProofClass::OwnerRealizationProof:
    if (!summary.hasOwnerRealizationWitness)
      return std::nullopt;
    return BuildEmittedProofFromSummary(summary.theoremClass, summary);

  case TheoremProofClass::IdentityPreservingProof:
    // Identity preservation needs no additional owner witness once the summary
    // has already passed the explicit in-domain discharge checks above.
  case TheoremProofClass::InvocationPreservingProof:
  case TheoremProofClass::DirectivePreservingProof:
  case TheoremProofClass::SuffixStabilizationProof:
  case TheoremProofClass::StateRepairProof:
    return BuildEmittedProofFromSummary(summary.theoremClass, summary);

  case TheoremProofClass::TerminalOutOfDomainProof:
  case TheoremProofClass::Unknown:
    return std::nullopt;
  }

  return std::nullopt;
}

std::optional<RefoldEngine::EmittedProof> RefoldEngine::BuildEmittedProof(
    const AcceptedResultCandidate &candidate) const {
  // AcceptedResultCandidate remains construction provenance during this proof
  // pass. The theorem proof itself now lives in ProofSummary::emittedProof and
  // is keyed by ProofSummary::theoremClass.  Rebuild only when the cached proof
  // is absent or stale so selector/emission code never falls back to
  // AcceptedProofClass as proof authority.
  if (candidate.kind == AcceptedResultCandidateKind::Unknown)
    return std::nullopt;

  std::optional<EmittedProof> proof = candidate.proofSummary.emittedProof;
  if (!proof || proof->theoremClass != candidate.proofSummary.theoremClass)
    proof = BuildCanonicalEmittedProofFromSummary(candidate.proofSummary);
  if (!proof)
    return std::nullopt;

  const bool terminalKind =
      candidate.kind == AcceptedResultCandidateKind::TerminalOutOfDomain;
  const bool terminalProof =
      proof->theoremClass == TheoremProofClass::TerminalOutOfDomainProof;
  if (terminalKind != terminalProof)
    return std::nullopt;

  return proof;
}

std::optional<RefoldEngine::TheoremProofClass>
RefoldEngine::NormalizeAcceptedProof(
    const AcceptedResultCandidate &candidate) const {
  const std::optional<EmittedProof> proof = BuildEmittedProof(candidate);
  if (!proof)
    return std::nullopt;
  return proof->theoremClass;
}

RefoldEngine::GlobalSelectionLattice
RefoldEngine::BuildGlobalSelectionLattice(const ProofSummary &summary) const {
  GlobalSelectionLattice lattice;

  // removes AcceptedProofClass from lattice authority. The conflict
  // domain is still path provenance because macro, include, TU, and terminal
  // artifacts occupy different owner spaces; the final proof family remains
  // summary.theoremClass and is checked separately by the emitted-proof gate.
  switch (summary.inventory.currentPath) {
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
    lattice.domain = LatticeConflictDomain::MacroInvocationRootSpan;
    lattice.mergeLaw = LatticeMergeLaw::NestedOuterShadowsInner;
    lattice.conflictLaw =
        summary.realizationMode == RealizationMode::PreserveOriginalStructure
            ? LatticeConflictLaw::PreferStructurePreservation
            : LatticeConflictLaw::RejectPartialOverlap;
    break;

  case AcceptedPathKind::IncludeDeleteReplaceMappedHeaderTokens:
  case AcceptedPathKind::IncludeInsertSelectedConditionalBoundary:
  case AcceptedPathKind::IncludeInsertChildBoundary:
  case AcceptedPathKind::IncludeInsertRightNeighborPP:
  case AcceptedPathKind::IncludeInsertLeftNeighborPP:
  case AcceptedPathKind::IncludeInsertDeclBoundary:
    lattice.domain = LatticeConflictDomain::IncludeOwnerRegion;
    lattice.mergeLaw = LatticeMergeLaw::DisjointCompose;
    lattice.conflictLaw =
        LatticeConflictLaw::PreferOwnerPreservingBeforeRealization;
    break;

  case AcceptedPathKind::IncludeRealizationInlineFromB:
  case AcceptedPathKind::IncludeMaterializedExpansion:
    lattice.domain = LatticeConflictDomain::IncludeOwnerRegion;
    lattice.mergeLaw = LatticeMergeLaw::SelectSingleWitness;
    lattice.conflictLaw =
        LatticeConflictLaw::PreferOwnerPreservingBeforeRealization;
    break;

  case AcceptedPathKind::TUExactSlotBoundary:
  case AcceptedPathKind::TUProvableInsertionAnchor:
    lattice.domain = LatticeConflictDomain::TUAnchorPoint;
    lattice.mergeLaw = LatticeMergeLaw::SelectSingleWitness;
    lattice.conflictLaw = LatticeConflictLaw::PreferExactAnchorWitness;
    break;

  case AcceptedPathKind::TUByteSpanMappedEdit:
  case AcceptedPathKind::TUByteSpanConservativeEdit:
  case AcceptedPathKind::TUIncludeClosureEdit:
    lattice.domain = LatticeConflictDomain::WholeTranslationUnit;
    lattice.mergeLaw = LatticeMergeLaw::DisjointCompose;
    lattice.conflictLaw = LatticeConflictLaw::RejectPartialOverlap;
    break;

  case AcceptedPathKind::TerminalEmitEditedPreprocessedStream:
    lattice.domain = LatticeConflictDomain::WholeTranslationUnit;
    lattice.mergeLaw = LatticeMergeLaw::TerminalReplacesAll;
    lattice.conflictLaw = LatticeConflictLaw::ExplicitOutOfDomainTerminalResult;
    break;

  case AcceptedPathKind::Unknown:
  case AcceptedPathKind::IncludePatchPendingMaterialization:
    break;
  }

  return lattice;
}

RefoldEngine::CompletenessContract
RefoldEngine::BuildCompletenessContract(const ProofSummary &summary) const {
  CompletenessContract contract;

  // The declared domain is explicit. For theorem-facing summaries,
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
    if (summary.terminalFallbackWitness) {
      if (const TerminalFallbackProofFailure *primary =
              summary.terminalFallbackWitness->PrimaryFailure()) {
        contract.hasExplicitExclusion = true;
        contract.explicitExclusion = primary->theoremFailure;
      }
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

  // Theorem-domain reporting, completeness reporting, and the theorem audit
  // must say the same thing. This helper remains derived-only: it does not
  // introduce new acceptance behavior, it only restates whether the summary is
  // in-domain, transitional-internal, or an explicit named out-of-domain class
  // under the same declared-domain contract.
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
  auto theoremTieBreakerPrefers =
      [](const ProofSummary &candidate,
         const ProofSummary &other) -> std::optional<bool> {
    // Explicit named theorem tie-breakers are the only permitted escape hatch
    // for preferences that used to live in path-local selection branches.  They
    // do not discharge proof obligations by themselves; they only order two
    // already-normalized accepted summaries after the builder has proved the
    // witness-specific preconditions for the named rule.
    switch (candidate.selectionTieBreaker) {
    case TheoremSelectionTieBreakerKind::
        ExactTUArgumentEditOverEquivalentMacroArgsOnly:
      return other.inventory.currentPath ==
             AcceptedPathKind::MacroArgsOnlyStandard;
    case TheoremSelectionTieBreakerKind::Unknown:
      return std::nullopt;
    }
    return std::nullopt;
  };

  if (std::optional<bool> lhsTie = theoremTieBreakerPrefers(lhs, rhs)) {
    if (*lhsTie)
      return true;
  }
  if (std::optional<bool> rhsTie = theoremTieBreakerPrefers(rhs, lhs)) {
    if (*rhsTie)
      return false;
  }

  auto preferenceRank = [](SelectionPreference preference) -> uint8_t {
    // Lower rank means stronger selection preference. These are lattice-level
    // policy categories, not local heuristics.
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
    // When two proofs have the same structural preference, prefer the result
    // that stays closer to the original source structure before falling back to
    // broader surface/TU emission.
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

  auto hasMixedOwnerTilingProof = [](const ProofSummary &summary) {
    return summary.hasMixedOwnerTilingWitness &&
           summary.theoremClass == TheoremProofClass::MixedOwnerTilingProof;
  };

  auto mixedOwnerCoverWidth = [](const MixedOwnerTilingWitness &witness) {
    return (witness.originalAEnd - witness.originalAStart) +
           (witness.originalBEnd - witness.originalBStart);
  };

  const bool lhsMixedOwner = hasMixedOwnerTilingProof(lhs);
  const bool rhsMixedOwner = hasMixedOwnerTilingProof(rhs);
  if (lhsMixedOwner != rhsMixedOwner &&
      lhs.inventory.currentPath == rhs.inventory.currentPath) {
    // centralizes the former mixed-owner overlay preference.  A
    // segment proven by a durable mixed-owner tiling is strictly stronger than
    // the owner-specific realization/preservation summary for the same emitted
    // artifact, but that ordering belongs here in the lattice rather than in
    // the path-local witness attachment code.
    return lhsMixedOwner;
  }

  if (lhsMixedOwner && rhsMixedOwner) {
    const MixedOwnerTilingWitness &lhsWitness = lhs.mixedOwnerTilingWitness;
    const MixedOwnerTilingWitness &rhsWitness = rhs.mixedOwnerTilingWitness;
    const uint64_t lhsWidth = mixedOwnerCoverWidth(lhsWitness);
    const uint64_t rhsWidth = mixedOwnerCoverWidth(rhsWitness);
    if (lhsWidth != rhsWidth)
      return lhsWidth < rhsWidth;
    if (lhsWitness.tokenSegmentCount != rhsWitness.tokenSegmentCount)
      return lhsWitness.tokenSegmentCount > rhsWitness.tokenSegmentCount;
    if (lhsWitness.stateGapCount != rhsWitness.stateGapCount)
      return lhsWitness.stateGapCount > rhsWitness.stateGapCount;
    if (lhsWitness.witnessId != rhsWitness.witnessId)
      return lhsWitness.witnessId > rhsWitness.witnessId;
  }

  // The remaining tie-breakers are deterministic enum orderings. They should
  // only be reached after the explicit lattice preferences above agree.  Use
  // the final theorem class here; AcceptedProofClass remains available only as
  // construction provenance and no longer orders selectable proofs.
  if (lhs.theoremClass != rhs.theoremClass)
    return static_cast<uint8_t>(lhs.theoremClass) <
           static_cast<uint8_t>(rhs.theoremClass);

  return static_cast<uint8_t>(lhs.inventory.currentPath) <
         static_cast<uint8_t>(rhs.inventory.currentPath);
}

bool RefoldEngine::IsSelectableAcceptedResultCandidate(
    const AcceptedResultCandidate &candidate) const {
  // Selection uses the same theorem-normalization gate as final emission.  A
  // path-specific builder may still stamp AcceptedPathKind and
  // AcceptedProofClass, but those are construction provenance only; a selector
  // may consider the result only after NormalizeAcceptedProof() proves that the
  // candidate maps to one final theorem class.
  return NormalizeAcceptedProof(candidate).has_value();
}

bool RefoldEngine::
    AcceptedResultCandidateHasOnlyNonTopLevelMacroSelectorFailure(
        const AcceptedResultCandidate &candidate) const {
  if (candidate.kind != AcceptedResultCandidateKind::MacroPatch)
    return false;

  // Detect the one selector-time macro rejection that can still be useful as a
  // diagnostic candidate: the proof root exists, but it is not top-level.
  const ProofDischargeRecord &discharge = candidate.proofSummary.discharge;
  return discharge.status == ProofDischargeStatus::Rejected &&
         discharge.failedObligation ==
             ProofObligationKind::MacroProofRootIsTopLevel &&
         discharge.failureReason ==
             ProofFailureReason::NonTopLevelMacroProofRoot;
}

bool RefoldEngine::AcceptedResultCandidateProofPrefers(
    const AcceptedResultCandidate &lhs,
    const AcceptedResultCandidate &rhs) const {
  if (LatticePrefers(lhs.proofSummary, rhs.proofSummary))
    return true;
  if (LatticePrefers(rhs.proofSummary, lhs.proofSummary))
    return false;
  return false;
}

bool RefoldEngine::AcceptedResultCandidateCanonicalPrefers(
    const AcceptedResultCandidate &lhs,
    const AcceptedResultCandidate &rhs) const {
  // The lattice intentionally stays coarse. When two summaries tie, prefer the
  // candidate that is more specific about the concrete artifact it will emit.
  // This is a named canonical preference step rather than a proof validity
  // test.  It is reached only after both candidates are selectable and neither
  // proof summary strictly outranks the other.
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

bool RefoldEngine::AcceptedResultCandidatePrefers(
    const AcceptedResultCandidate &lhs,
    const AcceptedResultCandidate &rhs) const {
  if (AcceptedResultCandidateProofPrefers(lhs, rhs))
    return true;
  if (AcceptedResultCandidateProofPrefers(rhs, lhs))
    return false;
  return AcceptedResultCandidateCanonicalPrefers(lhs, rhs);
}

std::optional<size_t> RefoldEngine::SelectPreferredCandidateIndex(
    size_t candidateCount, function_ref<bool(size_t)> isSelectable,
    function_ref<bool(size_t, size_t)> prefers) const {
  SmallVector<size_t, 8> selectableIndices;

  // This keeps selector behavior-preserving, but explicitly splits the two
  // questions that the old loop answered at the same time:
  //
  //   1. proof validity: which candidates are selectable at all?
  //   2. canonical preference: among those already-valid candidates, which
  //      deterministic representative should the legacy selector choose?
  //
  // The preference relation is deliberately never invoked on an invalid
  // candidate. Equivalence-class canonicalization can replace the second step
  // without changing the validity gate.
  for (size_t i = 0; i < candidateCount; ++i)
    if (isSelectable(i))
      selectableIndices.push_back(i);

  std::optional<size_t> bestIdx;
  for (size_t idx : selectableIndices) {
    if (!bestIdx) {
      bestIdx = idx;
      continue;
    }
    if (prefers(idx, *bestIdx))
      bestIdx = idx;
  }

  const uint64_t selectableCount = selectableIndices.size();
  if (selectableCount > 1) {
    ++lastTheoremAudit_.selectorCompetitions;
    if (bestIdx)
      ++lastTheoremAudit_.selectorResolutions;
  } else if (!bestIdx && candidateCount != 0) {
    ++lastTheoremAudit_.selectorNoSelectable;
    if (candidateCount > 1)
      ++lastTheoremAudit_.selectorUnresolvedCompetitions;
  }

  return bestIdx;
}

RefoldEngine::MacroSelectionCandidate
RefoldEngine::BuildMacroSelectionCandidate(
    const MacroPatch &patch, bool allowNonTopLevelMacroSelectorFailure) const {
  MacroSelectionCandidate candidate;
  candidate.selectorCandidate = BuildAcceptedMacroCandidate(patch);

  // Build the emitted carrier through the emission-specific macro gate, but do
  // not use it for ranking.  If it is not theorem-normalized, leave the optional
  // empty so a selected macro patch cannot accidentally stamp a selector-only
  // proof onto MacroPatch::selectedAcceptedCandidate.
  AcceptedResultCandidate emittedCandidate =
      RestampAcceptedMacroCandidateForEmission(
          patch, candidate.selectorCandidate);
  if (IsSelectableAcceptedResultCandidate(emittedCandidate))
    candidate.emittedCandidate = std::move(emittedCandidate);

  candidate.selectorOnly =
      allowNonTopLevelMacroSelectorFailure &&
      !IsSelectableAcceptedResultCandidate(candidate.selectorCandidate) &&
      AcceptedResultCandidateHasOnlyNonTopLevelMacroSelectorFailure(
          candidate.selectorCandidate);
  return candidate;
}

bool RefoldEngine::IsSelectableMacroSelectionCandidate(
    const MacroSelectionCandidate &candidate) const {
  if (IsSelectableAcceptedResultCandidate(candidate.selectorCandidate))
    return true;

  // The only macro-local non-final selector proof admitted by this carrier is
  // the explicit nested proof-root exception.  The emitted candidate remains a
  // separate optional and is audited only if it is later stamped for emission.
  return candidate.selectorOnly &&
         AcceptedResultCandidateHasOnlyNonTopLevelMacroSelectorFailure(
             candidate.selectorCandidate);
}

bool RefoldEngine::MacroSelectionCandidatePrefers(
    const MacroSelectionCandidate &lhs,
    const MacroSelectionCandidate &rhs) const {
  return AcceptedResultCandidatePrefers(lhs.selectorCandidate,
                                        rhs.selectorCandidate);
}

std::optional<RefoldEngine::SelectedMacroSelectionCandidate>
RefoldEngine::SelectPreferredMacroSelectionCandidate(
    ArrayRef<MacroSelectionCandidate> candidates) const {
  auto isSelectable = [&](size_t idx) {
    return IsSelectableMacroSelectionCandidate(candidates[idx]);
  };
  auto prefers = [&](size_t lhsIdx, size_t rhsIdx) {
    return MacroSelectionCandidatePrefers(candidates[lhsIdx],
                                          candidates[rhsIdx]);
  };

  std::optional<size_t> legacyBestIdx = SelectPreferredCandidateIndex(
      candidates.size(), isSelectable, prefers);

  WitnessResolverDecision resolverDecision = ResolveWitnessesForSelection(
      "SelectPreferredMacroSelectionCandidate", candidates.size(), isSelectable,
      [&](size_t idx) {
        return BuildRefoldWitness(candidates[idx].selectorCandidate,
                                  "SelectPreferredMacroSelectionCandidate",
                                  idx);
      },
      prefers, legacyBestIdx);

  std::optional<size_t> bestIdx = legacyBestIdx;
  // Preserve the selector-only diagnostic trace that is not part of the generic
  // resolver validity predicate: a nested selector candidate may be useful for
  // ranking but still lack an emission-normalized carrier.
  if (ShouldEmitProofLog()) {
    for (size_t i = 0; i < candidates.size(); ++i) {
      if (!isSelectable(i) || !candidates[i].selectorOnly ||
          candidates[i].emittedCandidate) {
        continue;
      }
      TraceWitnessRejected(
          BuildRefoldWitness(candidates[i].selectorCandidate,
                             "SelectPreferredMacroSelectionCandidate", i),
          WitnessRejectReason::SelectorOnlyNoEmittedCandidate,
          "selector-only macro witness has no emission-normalized carrier");
    }
  }

  if (resolverDecision.ShouldFailClosed())
    return std::nullopt;
  if (resolverDecision.ShouldUseResolverIndex())
    bestIdx = resolverDecision.resolverIndex;

  if (!bestIdx)
    return std::nullopt;

  SelectedMacroSelectionCandidate selected;
  selected.candidate = candidates[*bestIdx];
  selected.index = *bestIdx;
  TraceWitnessChosen(
      BuildRefoldWitness(selected.candidate.selectorCandidate,
                         "SelectPreferredMacroSelectionCandidate", *bestIdx),
      *bestIdx);
  return selected;
}

std::optional<RefoldEngine::SelectedAcceptedResultCandidate>
RefoldEngine::SelectPreferredAcceptedResultCandidate(
    ArrayRef<AcceptedResultCandidate> candidates) const {
  auto isSelectable = [&](size_t idx) {
    return IsSelectableAcceptedResultCandidate(candidates[idx]);
  };
  auto prefers = [&](size_t lhsIdx, size_t rhsIdx) {
    return AcceptedResultCandidatePrefers(candidates[lhsIdx],
                                          candidates[rhsIdx]);
  };

  std::optional<size_t> legacyBestIdx = SelectPreferredCandidateIndex(
      candidates.size(), isSelectable, prefers);

  WitnessResolverDecision resolverDecision = ResolveWitnessesForSelection(
      "SelectPreferredAcceptedResultCandidate", candidates.size(), isSelectable,
      [&](size_t idx) {
        return BuildRefoldWitness(
            candidates[idx], "SelectPreferredAcceptedResultCandidate", idx);
      },
      prefers, legacyBestIdx);

  std::optional<size_t> bestIdx = legacyBestIdx;
  if (resolverDecision.ShouldFailClosed())
    return std::nullopt;
  if (resolverDecision.ShouldUseResolverIndex())
    bestIdx = resolverDecision.resolverIndex;

  if (!bestIdx)
    return std::nullopt;

  AuditAcceptedResultCandidateForLegacyAuthority(
      candidates[*bestIdx], "SelectPreferredAcceptedResultCandidate");

  SelectedAcceptedResultCandidate selected;
  selected.candidate = candidates[*bestIdx];
  selected.index = *bestIdx;
  TraceWitnessChosen(
      BuildRefoldWitness(selected.candidate,
                         "SelectPreferredAcceptedResultCandidate", *bestIdx),
      *bestIdx);
  return selected;
}

std::optional<size_t> RefoldEngine::SelectPreferredAcceptedResultCandidateIndex(
    ArrayRef<AcceptedResultCandidate> candidates) const {
  std::optional<SelectedAcceptedResultCandidate> selected =
      SelectPreferredAcceptedResultCandidate(candidates);
  if (!selected)
    return std::nullopt;
  return selected->index;
}

RefoldEngine::EmissionPathKind
RefoldEngine::PrimaryEmissionPathForCandidateKind(
    AcceptedResultCandidateKind kind) {
  switch (kind) {
  case AcceptedResultCandidateKind::MacroPatch:
    return EmissionPathKind::MacroPatch;
  case AcceptedResultCandidateKind::IncludePatch:
    return EmissionPathKind::IncludePatch;
  case AcceptedResultCandidateKind::TUAnchor:
    return EmissionPathKind::TUAnchor;
  case AcceptedResultCandidateKind::TUTextEdit:
    return EmissionPathKind::TUTextEdit;
  case AcceptedResultCandidateKind::TerminalOutOfDomain:
    return EmissionPathKind::TerminalOutOfDomain;
  case AcceptedResultCandidateKind::Unknown:
    return EmissionPathKind::Unknown;
  }
  return EmissionPathKind::Unknown;
}

void RefoldEngine::RefreshAcceptedCandidateEmissionPathInventory(
    AcceptedResultCandidate &candidate) const {
  EmissionPathInventory inventory;
  inventory.Add(PrimaryEmissionPathForCandidateKind(candidate.kind));

  // Mixed-owner tiling and owner realization are theorem/proof overlays that
  // can be carried by a macro, include, or TU primary emitted surface.  Record
  // them explicitly so the proof model can force those surviving paths through
  // the accepted-result gate without treating them as separate primary
  // surfaces.
  if (candidate.proofSummary.hasMixedOwnerTilingWitness ||
      candidate.proofSummary.theoremClass ==
          TheoremProofClass::MixedOwnerTilingProof) {
    inventory.Add(EmissionPathKind::MixedOwnerTilingSegment);
  }

  if (candidate.proofSummary.hasOwnerRealizationWitness ||
      candidate.proofSummary.theoremClass ==
          TheoremProofClass::OwnerRealizationProof) {
    inventory.Add(EmissionPathKind::OwnerRealizationMaterialization);
  }

  candidate.emissionPaths = std::move(inventory);
}

RefoldEngine::AcceptedResultCandidate
RefoldEngine::BuildAcceptedMacroCandidate(const MacroPatch &patch) const {
  AcceptedResultCandidate candidate;
  candidate.kind = AcceptedResultCandidateKind::MacroPatch;
  candidate.proofSummary = ClassifyMacroPatchProof(patch);
  if (patch.proof.kind == MacroPatchProofKind::WholeCoverRealization &&
      patch.wholeCoverALo <= patch.wholeCoverAHi &&
      patch.wholeCoverBAdjLo <= patch.wholeCoverBAdjHi) {
    AttachMixedOwnerTilingWitnessForTokenEnvelope(
        candidate.proofSummary, patch.wholeCoverALo, patch.wholeCoverAHi,
        patch.wholeCoverBAdjLo, patch.wholeCoverBAdjHi);
  }
  candidate.begin = patch.invStart;
  candidate.end = patch.invEnd;
  if (patch.hasMaterializedBTokenRange) {
    candidate.hasTargetBTokenRange = true;
    candidate.targetBTokStart = patch.materializedBTokStart;
    candidate.targetBTokEnd = patch.materializedBTokEnd;
  }

  const bool isOrdinaryMacroActualRepair =
      patch.proof.kind == MacroPatchProofKind::ArgsOnlyStandard ||
      patch.proof.kind == MacroPatchProofKind::ArgsOnlyPairedPureInsertion;
  const bool macroActualArityStable =
      patch.proof.proofRootMacroId != 0 &&
      patch.proof.proofRootMacroId == patch.macroId;
  if (isOrdinaryMacroActualRepair && patch.proof.wholeEnvelopeReplay &&
      patch.proof.wholeEnvelopeReplay->replayValidated &&
      patch.hasMaterializedBTokenRange && macroActualArityStable) {
    candidate.hasMacroActualRepairWitness = true;
    candidate.macroActualWholeEnvelopeReplayValidated = true;
    candidate.macroActualDefinitionTapeReplayValidated =
        patch.proof.wholeEnvelopeReplay->definitionTapeReplayValidated;
    candidate.macroActualArityStable = true;
  }

  if (patch.proof.generatedCalleeReplay &&
      patch.proof.generatedCalleeReplay->calleeChainDeterministic &&
      patch.proof.generatedCalleeReplay->replacementReplayValidated &&
      patch.proof.generatedCalleeReplay->solvedActualsMappedToRoot &&
      patch.hasMaterializedBTokenRange) {
    const GeneratedCalleeReplayWitness &w =
        *patch.proof.generatedCalleeReplay;
    candidate.hasGeneratedCalleeReplayWitness = true;
    candidate.generatedCalleeRootMacroId = w.rootMacroId;
    candidate.generatedCalleeFinalDirectiveId = w.finalDirectiveId;
    candidate.generatedCalleeDepth = w.generatedCallDepth;
    candidate.generatedCalleeObjectAliasHops = w.objectAliasHops;
    candidate.generatedCalleeChainDeterministic =
        w.calleeChainDeterministic;
    candidate.generatedCalleeReplacementReplayValidated =
        w.replacementReplayValidated;
    candidate.generatedCalleeSolvedActualsMappedToRoot =
        w.solvedActualsMappedToRoot;
    candidate.generatedCalleeUsesForwarding = w.usesForwarding;
    candidate.generatedCalleeUsesStringification =
        w.usesStringification;
    candidate.generatedCalleeUsesPaste = w.usesPaste;
    candidate.generatedCalleeUsesVariadicForwarding =
        w.usesVariadicForwarding;
    candidate.generatedCalleeUsesObjectAlias = w.usesObjectAlias;
    candidate.generatedCalleeDecodedStringLiteralEvidenceOnly =
        w.decodedStringLiteralEvidenceOnly;
  }

  if (patch.proof.variadicCommaReplay && patch.hasMaterializedBTokenRange &&
      patch.proof.preservesInvocationStructure &&
      !candidate.hasGeneratedCalleeReplayWitness) {
    const VariadicCommaWitness &w = *patch.proof.variadicCommaReplay;
    candidate.hasVariadicCommaWitness = true;
    candidate.variadicRootMacroId = w.rootMacroId;
    candidate.variadicFormalIndex = w.variadicFormalIndex;
    candidate.variadicArityStable = w.arityStable;
    candidate.variadicOriginalMissing = w.originalMissing;
    candidate.variadicOriginalExplicitEmpty = w.originalExplicitEmpty;
    candidate.variadicOriginalNonEmpty = w.originalNonEmpty;
    candidate.variadicResultMissing = w.resultMissing;
    candidate.variadicResultExplicitEmpty = w.resultExplicitEmpty;
    candidate.variadicResultNonEmpty = w.resultNonEmpty;
    candidate.variadicLiteralCommaInActual = w.literalCommaInActual;
    candidate.variadicCommaInserted = w.commaInserted;
    candidate.variadicCommaDeleted = w.commaDeleted;
    candidate.variadicGnuCommaElision = w.gnuCommaElision;
    candidate.variadicVaOptPresent = w.vaOptPresent;
    candidate.variadicVaOptOriginallyActive = w.vaOptOriginallyActive;
    candidate.variadicVaOptResultActive = w.vaOptResultActive;
    candidate.variadicVaOptCommaIntroduced = w.vaOptCommaIntroduced;
    candidate.variadicVaOptCommaDeleted = w.vaOptCommaDeleted;
    candidate.variadicVaOptNodeCount = w.vaOptNodeCount;
    candidate.variadicVaOptIncludedCount = w.vaOptIncludedCount;
    candidate.variadicProducerSignature = w.producerSignature;
    candidate.variadicPackStateSignature = w.packStateSignature;
  }

  std::optional<ZeroTokenBoundaryWitness> synthesizedZeroTokenBoundary;
  if (!patch.proof.zeroTokenBoundaryReplay &&
      patch.proof.kind == MacroPatchProofKind::ArgsOnlyPairedPureInsertion &&
      patch.hasMaterializedBTokenRange &&
      patch.proof.preservesInvocationStructure) {
    ZeroTokenBoundaryWitness witness;
    witness.ownerId = patch.proof.proofRootMacroId
                          ? patch.proof.proofRootMacroId
                          : patch.macroId;
    witness.ownerKind = "macro";
    witness.hasSourceAnchor = true;
    witness.sourceAnchor = patch.invStart;
    witness.hasBTokenRange = true;
    witness.bTokStart = patch.materializedBTokStart;
    witness.bTokEnd = patch.materializedBTokEnd;
    witness.producerProven = true;
    witness.ownerClosed = true;
    witness.layoutStable = true;
    witness.observersStable = true;
    witness.counterStable = true;
    witness.fromPairedInsertion = true;
    witness.boundarySignature =
        llvm::formatv("macro-paired-pure-insertion:owner={0}:source=[{1},{2}):"
                      "b=[{3},{4})",
                      witness.ownerId, patch.invStart, patch.invEnd,
                      witness.bTokStart, witness.bTokEnd)
            .str();
    synthesizedZeroTokenBoundary = std::move(witness);
  }

  const ZeroTokenBoundaryWitness *zeroTokenWitnessForCandidate =
      patch.proof.zeroTokenBoundaryReplay
          ? &*patch.proof.zeroTokenBoundaryReplay
          : (synthesizedZeroTokenBoundary ? &*synthesizedZeroTokenBoundary
                                          : nullptr);
  if (zeroTokenWitnessForCandidate && patch.hasMaterializedBTokenRange &&
      patch.proof.preservesInvocationStructure &&
      !candidate.hasGeneratedCalleeReplayWitness) {
    const ZeroTokenBoundaryWitness &w = *zeroTokenWitnessForCandidate;
    candidate.hasZeroTokenBoundaryWitness = true;
    candidate.zeroTokenOwnerId = w.ownerId;
    candidate.zeroTokenOwnerKind = w.ownerKind;
    candidate.zeroTokenHasPPGap = w.hasPPGap;
    candidate.zeroTokenPPGap = w.ppGap;
    candidate.zeroTokenHasSourceAnchor = w.hasSourceAnchor;
    candidate.zeroTokenSourceAnchor = w.sourceAnchor;
    candidate.zeroTokenHasBTokenRange = w.hasBTokenRange;
    candidate.zeroTokenBTokStart = w.bTokStart;
    candidate.zeroTokenBTokEnd = w.bTokEnd;
    candidate.zeroTokenProducerProven = w.producerProven;
    candidate.zeroTokenOwnerClosed = w.ownerClosed;
    candidate.zeroTokenLayoutStable = w.layoutStable;
    candidate.zeroTokenObserversStable = w.observersStable;
    candidate.zeroTokenCounterStable = w.counterStable;
    candidate.zeroTokenFromEmptyActual = w.fromEmptyActual;
    candidate.zeroTokenFromReplacementGap = w.fromReplacementGap;
    candidate.zeroTokenFromPairedInsertion = w.fromPairedInsertion;
    candidate.zeroTokenFromTUAnchor = w.fromTUAnchor;
    candidate.zeroTokenFromIncludeBoundary = w.fromIncludeBoundary;
    candidate.zeroTokenFromDirectiveLayoutGap = w.fromDirectiveLayoutGap;
    candidate.zeroTokenBoundarySignature = w.boundarySignature;
  }

  // Expose direct stringification and token-paste producer semantics in the
  // common witness key.  This does not revalidate or reprioritize the patch; it
  // only summarizes producer facts for macro patches that have already survived
  // their family-specific proof path.
  if (const RefoldModel::MacroInvocation *inv =
          FindMacroInvocationById(patch.macroId)) {
    if (!inv->stringifySpans.empty() && patch.hasMaterializedBTokenRange &&
        patch.proof.preservesInvocationStructure &&
        !candidate.hasGeneratedCalleeReplayWitness) {
      candidate.hasStringificationWitness = true;
      candidate.stringificationRootMacroId = patch.proof.proofRootMacroId
                                                 ? patch.proof.proofRootMacroId
                                                 : patch.macroId;
      candidate.stringificationSpanCount =
          static_cast<uint32_t>(inv->stringifySpans.size());

      SmallVector<uint32_t, 8> argIdxs;
      std::vector<RefoldModel::PPArgSpan> spans = inv->stringifySpans;
      llvm::sort(spans, [](const RefoldModel::PPArgSpan &lhs,
                           const RefoldModel::PPArgSpan &rhs) {
        if (lhs.argIdx != rhs.argIdx)
          return lhs.argIdx < rhs.argIdx;
        if (lhs.begin != rhs.begin)
          return lhs.begin < rhs.begin;
        return lhs.end < rhs.end;
      });

      std::string producerSig =
          llvm::formatv("root={0}:spans={1}",
                        candidate.stringificationRootMacroId, spans.size())
              .str();
      std::string payloadSig;
      bool payloadsCanonical = true;
      for (const RefoldModel::PPArgSpan &span : spans) {
        argIdxs.push_back(span.argIdx);
        producerSig += llvm::formatv(":arg={0}:[{1},{2})", span.argIdx,
                                     span.begin, span.end)
                           .str();
        StringRef literal = SliceASource(span.begin, span.end).trim();
        std::optional<std::string> unstringified =
            UnstringifyLiteralToArgText(literal, /*allowTopLevelComma=*/true);
        std::optional<std::string> canonical =
            unstringified
                ? stringutils::canonicalizeStringifyInversePayload(
                      StringRef(*unstringified))
                : std::nullopt;
        if (!unstringified || !canonical ||
            StringRef(*canonical).trim() != StringRef(*unstringified).trim()) {
          payloadsCanonical = false;
          payloadSig += llvm::formatv(":arg={0}:payload=unknown", span.argIdx)
                            .str();
          continue;
        }
        payloadSig += llvm::formatv(":arg={0}:canon={1}", span.argIdx,
                                    FormatWitnessTraceHash(*canonical))
                          .str();
      }
      llvm::sort(argIdxs);
      argIdxs.erase(std::unique(argIdxs.begin(), argIdxs.end()),
                    argIdxs.end());
      candidate.stringificationArgCount =
          static_cast<uint32_t>(argIdxs.size());
      candidate.stringificationWhitespaceNormalized = payloadsCanonical;
      candidate.stringificationEscapedSpellingStable = payloadsCanonical;
      candidate.stringificationProducerSignature = std::move(producerSig);
      candidate.stringificationCanonicalPayloadSignature =
          payloadSig.empty() ? std::string("empty") : std::move(payloadSig);
      if (!payloadsCanonical) {
        // If the producer-recorded stringify literal cannot be normalized into
        // the supported inverse domain, do not let claim a known
        // stringification equivalence dimension. The already accepted macro
        // proof remains intact; the common resolver simply keeps this
        // dimension unknown until a later proof can explain it.
        candidate.hasStringificationWitness = false;
      }
    }

    const bool pasteProofPresent = patch.proof.paste.has_value() ||
        patch.pasteReplayValidated ||
        patch.proof.kind == MacroPatchProofKind::ArgsOnlyPasteSingle ||
        patch.proof.kind == MacroPatchProofKind::ArgsOnlyPasteMulti ||
        patch.proof.kind == MacroPatchProofKind::ArgsOnlyPurePasteOnly ||
        patch.proof.kind == MacroPatchProofKind::PasteDerivedCalleeSelector;
    if ((!inv->pasteSpans.empty() || !inv->pasteTokens.empty()) &&
        patch.hasMaterializedBTokenRange && pasteProofPresent &&
        !candidate.hasGeneratedCalleeReplayWitness) {
      candidate.hasTokenPasteWitness = true;
      candidate.tokenPasteRootMacroId = patch.proof.proofRootMacroId
                                            ? patch.proof.proofRootMacroId
                                            : patch.macroId;
      candidate.tokenPasteSpanCount =
          static_cast<uint32_t>(inv->pasteSpans.size());
      candidate.tokenPasteTokenCount =
          static_cast<uint32_t>(inv->pasteTokens.size());

      std::string producerSig =
          llvm::formatv("root={0}:spans={1}:tokens={2}",
                        candidate.tokenPasteRootMacroId,
                        inv->pasteSpans.size(), inv->pasteTokens.size())
              .str();
      std::string resultSig;

      std::vector<RefoldModel::PPArgSpan> pasteSpans = inv->pasteSpans;
      llvm::sort(pasteSpans, [](const RefoldModel::PPArgSpan &lhs,
                                const RefoldModel::PPArgSpan &rhs) {
        if (lhs.argIdx != rhs.argIdx)
          return lhs.argIdx < rhs.argIdx;
        if (lhs.begin != rhs.begin)
          return lhs.begin < rhs.begin;
        return lhs.end < rhs.end;
      });
      for (const RefoldModel::PPArgSpan &span : pasteSpans) {
        producerSig += llvm::formatv(":span_arg={0}:[{1},{2})",
                                     span.argIdx, span.begin, span.end)
                           .str();
      }

      for (const RefoldModel::PasteToken &token : inv->pasteTokens) {
        resultSig += llvm::formatv(":result={0}:parts={1}",
                                   FormatWitnessTraceHash(token.spelling),
                                   token.parts.size())
                         .str();
        candidate.tokenPastePartCount +=
            static_cast<uint32_t>(token.parts.size());
        for (const RefoldModel::PastePart &part : token.parts) {
          if (part.kind == RefoldModel::PastePartKind::Arg) {
            ++candidate.tokenPasteArgPartCount;
            if (part.byteBegin == 0)
              candidate.tokenPasteHasLeftProducer = true;
            if (part.byteEnd == token.spelling.size())
              candidate.tokenPasteHasRightProducer = true;
            producerSig +=
                llvm::formatv(":arg_part={0}:[{1},{2})",
                              part.argIndex
                                  ? *part.argIndex
                                  : std::numeric_limits<uint32_t>::max(),
                              part.byteBegin, part.byteEnd)
                    .str();
          } else {
            ++candidate.tokenPasteLiteralPartCount;
            producerSig += llvm::formatv(":lit_part=[{0},{1})", part.byteBegin,
                                         part.byteEnd)
                               .str();
          }
        }
      }

      if (!candidate.tokenPasteHasLeftProducer &&
          candidate.tokenPasteArgPartCount != 0) {
        candidate.tokenPasteHasLeftProducer = true;
      }
      if (!candidate.tokenPasteHasRightProducer &&
          candidate.tokenPasteArgPartCount > 1) {
        candidate.tokenPasteHasRightProducer = true;
      }

      if (candidate.hasTargetBTokenRange) {
        for (uint64_t tok = candidate.targetBTokStart;
             tok < candidate.targetBTokEnd && tok < bToks_.size(); ++tok) {
          resultSig +=
              llvm::formatv(":bkind={0}:bspell={1}", bToks_[tok].kind,
                            FormatWitnessTraceHash(bToks_[tok].spelling))
                  .str();
        }
      }

      candidate.tokenPasteResultValidated =
          patch.pasteReplayValidated ||
          (patch.proof.paste &&
           (patch.proof.paste->requiresProducerPasteSpans ||
            patch.proof.paste->replayValidated));
      candidate.tokenPasteDiagnosticSafe = candidate.tokenPasteResultValidated;
      candidate.tokenPasteProducerSignature = std::move(producerSig);
      candidate.tokenPasteResultSignature =
          resultSig.empty() ? std::string("empty") : std::move(resultSig);
    }
  }

  if (patch.proof.counterState) {
    candidate.hasCounterStateWitness = true;
    candidate.counterStateWitness = *patch.proof.counterState;
  }

  // Ordinary macro-repair/replay families may be counter-stable even when
  // they do not carry a value-specific CounterStateWitness.  The proof is not
  // textual: it is the producer macro-invocation tree.  If the accepted repair
  // preserves one macro invocation, the original expansion subtree contains no
  // producer-recorded __COUNTER__ invocation, and the emitted source repair
  // does not introduce a raw __COUNTER__ spelling, then this tile has zero
  // counter consumption delta.  In that case all suffix counter observers are
  // vacuously preserved by this tile.  Do not use this path for explicit
  // counter materialization/literalization, which already installed a typed
  // counter witness above.
  auto macroProofKindCanBeCounterNeutral = [](MacroPatchProofKind kind) {
    switch (kind) {
    case MacroPatchProofKind::ArgsOnlyStandard:
    case MacroPatchProofKind::DagSubtreeRoot:
    case MacroPatchProofKind::CallChainSuffix:
    case MacroPatchProofKind::PasteDerivedCalleeSelector:
      return true;
    case MacroPatchProofKind::Unknown:
    case MacroPatchProofKind::ArgsOnlyPasteSingle:
    case MacroPatchProofKind::ArgsOnlyPasteMulti:
    case MacroPatchProofKind::ArgsOnlyPurePasteOnly:
    case MacroPatchProofKind::ArgsOnlyPairedPureInsertion:
    case MacroPatchProofKind::CounterLiteral:
    case MacroPatchProofKind::WholeCoverRealization:
      return false;
    }
    return false;
  };

  auto macroInvocationIsInSubtreeOf = [&](
      const RefoldModel::MacroInvocation &macro, uint64_t rootId) {
    uint64_t currentId = macro.id;
    SmallVector<uint64_t, 8> seen;
    while (true) {
      if (currentId == rootId)
        return true;
      if (std::find(seen.begin(), seen.end(), currentId) != seen.end())
        return false;
      seen.push_back(currentId);

      const RefoldModel::MacroInvocation *current =
          FindMacroInvocationById(currentId);
      if (!current || !current->callerMacroId)
        return false;
      currentId = *current->callerMacroId;
    }
  };

  auto macroSubtreeContainsCounterInvocation = [&](uint64_t rootId) {
    if (!FindMacroInvocationById(rootId))
      return true;
    for (const RefoldModel::MacroInvocation &macro :
         model_.GetMacroInvocations()) {
      if (macro.name != "__COUNTER__")
        continue;
      if (macroInvocationIsInSubtreeOf(macro, rootId))
        return true;
    }
    return false;
  };

  const uint64_t counterNeutralRootId = patch.proof.proofRootMacroId
                                            ? patch.proof.proofRootMacroId
                                            : patch.macroId;
  if (!candidate.hasCounterStateWitness && counterNeutralRootId != 0 &&
      macroProofKindCanBeCounterNeutral(patch.proof.kind) &&
      patch.proof.preservesInvocationStructure &&
      !RawIdentifierAppearsInText("__COUNTER__", patch.replacement) &&
      !macroSubtreeContainsCounterInvocation(counterNeutralRootId)) {
    candidate.hasCounterStateWitness = true;
    candidate.counterStateWitness = CounterStateWitness{};
    candidate.counterStateWitness.counterOrderKnown = true;
    candidate.counterStateWitness.suffixStateStable = true;
    candidate.counterStateWitness.suffixUnobserved = true;
    candidate.counterStateWitness.suffixObserverSignature =
        llvm::formatv(
            "counter-neutral:path={0}:root={1}:target=[{2},{3}):"
            "producer_subtree=no-counter:replacement=no-raw-counter",
            patch.proof.kind, counterNeutralRootId,
            candidate.hasTargetBTokenRange ? candidate.targetBTokStart : 0,
            candidate.hasTargetBTokenRange ? candidate.targetBTokEnd : 0)
            .str();
  }

  // Preserve the proof root separately from the byte span so selector/audit
  // code can reason about macro ancestry without reclassifying the patch.
  if (patch.proof.proofRootMacroId) {
    candidate.hasRootMacroId = true;
    candidate.rootMacroId = patch.proof.proofRootMacroId;
  }

  candidate.hasPayloadPreview = true;
  candidate.payloadPreview =
      stringutils::showWsWithClip(patch.replacement, 120);
  AttachLineControlObserverWitness(candidate);
  AttachCounterStateWitness(candidate);
  RefreshAcceptedCandidateEmissionPathInventory(candidate);
  AuditMacroPatchProofForLegacyAuthority(patch, "BuildAcceptedMacroCandidate");
  TraceWitnessEmitted(
      BuildRefoldWitness(candidate, "BuildAcceptedMacroCandidate"));
  return candidate;
}

RefoldEngine::AcceptedResultCandidate
RefoldEngine::RestampAcceptedMacroCandidateForEmission(
    const MacroPatch &patch, AcceptedResultCandidate candidate) const {
  // Restamp emitted preserving macro artifacts onto the emission-specific
  // discharge rule, removing the byte-edit boundary's selector-only
  // nested-macro exception. Selector competition still uses the stronger
  // top-level proof-root contract through BuildAcceptedMacroCandidate().
  if (candidate.kind == AcceptedResultCandidateKind::MacroPatch &&
      candidate.proofSummary.theoremClass ==
          TheoremProofClass::InvocationPreservingProof &&
      AcceptedResultCandidateHasOnlyNonTopLevelMacroSelectorFailure(
          candidate)) {
    candidate.proofSummary.discharge =
        ValidateEmittedInvocationPreservingProof(patch);
    FinalizeProofSummary(candidate.proofSummary);
    AttachLineControlObserverWitness(candidate);
  AttachCounterStateWitness(candidate);
    RefreshAcceptedCandidateEmissionPathInventory(candidate);
  }

  return candidate;
}

RefoldEngine::AcceptedResultCandidate
RefoldEngine::BuildAcceptedMacroEmissionCandidate(
    const MacroPatch &patch) const {
  return RestampAcceptedMacroCandidateForEmission(
      patch, BuildAcceptedMacroCandidate(patch));
}

bool RefoldEngine::FinalizeSelectedMacroPatchForEmission(
    MacroPatch &patch, StringRef role) const {
  // A patch may be merged or materialized by a path that never participated in
  // the final macro selector.  Before the owner/macro-id map is flattened into
  // emission buckets, refresh the canonical proof summary and force every
  // emission-bound patch through the same theorem-normalized carrier gate.
  SyncMacroPatchProofSummary(patch);

  if (patch.selectedAcceptedCandidate)
    return true;

  SmallVector<AcceptedResultCandidate, 1> candidates;
  candidates.push_back(BuildAcceptedMacroEmissionCandidate(patch));

  const std::optional<SelectedAcceptedResultCandidate> selected =
      SelectPreferredAcceptedResultCandidate(candidates);
  if (selected) {
    StampSelectedMacroPatchCandidate(patch, selected->candidate, role);
    return true;
  }

  const bool rejected = RejectMissingSelectedMacroPatchCarrier(
      patch, role,
      llvm::formatv(
          "macro patch bytes=[{0},{1}) was queued for emission but no "
          "theorem-normalized AcceptedResultCandidate could be selected",
          patch.invStart, patch.invEnd)
          .str());

  // Diagnostic-only audit may keep collecting evidence in non-strict runs, but
  // strict engine runs and strict no-legacy audit runs must not forward an
  // unstamped MacroPatch into the emitted edit buckets.
  return !rejected;
}

RefoldEngine::AcceptedResultCandidate
RefoldEngine::BuildAcceptedEmittedMacroCandidate(
    const MacroPatch &patch) const {
  // Makes the byte-edit boundary consume only the carrier selected before
  // emission bucketing.  Rebuilding from MacroPatch here would recreate the
  // legacy proof-authority escape that the selectedAcceptedCandidate invariant
  // is meant to eliminate.
  if (!patch.selectedAcceptedCandidate) {
    RejectMissingSelectedMacroPatchCarrier(
        patch, "BuildAcceptedEmittedMacroCandidate",
        llvm::formatv(
            "emitted macro patch bytes=[{0},{1}) reached emission without "
            "a selected AcceptedResultCandidate",
            patch.invStart, patch.invEnd)
            .str());
    return AcceptedResultCandidate{};
  }

  AuditAcceptedResultCandidateForLegacyAuthority(
      *patch.selectedAcceptedCandidate,
      "BuildAcceptedEmittedMacroCandidate/selected-carrier");
  return *patch.selectedAcceptedCandidate;
}

RefoldEngine::AcceptedResultCandidate
RefoldEngine::BuildAcceptedIncludeCandidate(
    AcceptedPathKind currentPath, const IncludePatch &patch,
    const IncludeAnchorWitness *includeAnchorWitness) const {
  AcceptedResultCandidate candidate;
  candidate.kind = AcceptedResultCandidateKind::IncludePatch;

  // Include-preserving candidates carry include-anchor proof metadata only.
  // Include realization is no longer a parallel include-specific proof family;
  // materialized/inline include output uses
  // BuildAcceptedIncludeRealizationCandidate() so the generic
  // OwnerRealizationWitness is the theorem-facing closure proof.
  candidate.proofSummary = BuildAcceptedPathProofSummary(
      currentPath, &patch, /*tuAnchorWitness=*/nullptr, includeAnchorWitness);
  AttachMixedOwnerTilingWitnessForTokenEnvelope(
      candidate.proofSummary, patch.aStart, patch.aEnd, patch.bStart,
      patch.bEnd);

  candidate.begin = patch.aStart;
  candidate.end = patch.aEnd;

  if (patch.include) {
    candidate.hasOwnerIncludeId = true;
    candidate.ownerIncludeId = patch.include->id;
  }

  // Preserve anchor byte information for selector diagnostics and deterministic
  // tie-breaking without requiring later code to reopen the witness object.
  if (includeAnchorWitness && includeAnchorWitness->hasAnchorByte) {
    candidate.hasAnchorByte = true;
    candidate.anchorByte = includeAnchorWitness->anchorByte;
  }

  // Include-preserving insertions at an empty A range are zero-token
  // boundary-gap witnesses.  The existing include-anchor proof already
  // established the source byte; this mirrors that proof into the common
  // witness/equivalence vocabulary.
  if (includeAnchorWitness && patch.aStart == patch.aEnd &&
      includeAnchorWitness->hasAnchorByte) {
    candidate.hasZeroTokenBoundaryWitness = true;
    candidate.zeroTokenOwnerKind = "include";
    candidate.zeroTokenOwnerId = patch.include ? patch.include->id : 0;
    candidate.zeroTokenHasPPGap = true;
    candidate.zeroTokenPPGap = patch.aStart;
    candidate.zeroTokenHasSourceAnchor = true;
    candidate.zeroTokenSourceAnchor = includeAnchorWitness->anchorByte;
    candidate.zeroTokenHasBTokenRange = true;
    candidate.zeroTokenBTokStart = patch.bStart;
    candidate.zeroTokenBTokEnd = patch.bEnd;
    candidate.zeroTokenProducerProven = true;
    candidate.zeroTokenOwnerClosed = true;
    candidate.zeroTokenLayoutStable = true;
    candidate.zeroTokenObserversStable = true;
    candidate.zeroTokenCounterStable = true;
    candidate.zeroTokenFromIncludeBoundary = true;
    candidate.zeroTokenFromDirectiveLayoutGap =
        includeAnchorWitness->evidence ==
            IncludeAnchorEvidenceKind::SelectedConditionalBoundary ||
        includeAnchorWitness->evidence ==
            IncludeAnchorEvidenceKind::DeclBoundary;
    candidate.zeroTokenBoundarySignature =
        llvm::formatv("include-anchor:evidence={0}:include={1}:pp_gap={2}:"
                      "byte={3}:b=[{4},{5}):cond={6}:{7}:child={8}:{9}:"
                      "neighbor={10}:{11}",
                      includeAnchorWitness->evidence,
                      candidate.zeroTokenOwnerId, patch.aStart,
                      includeAnchorWitness->anchorByte, patch.bStart,
                      patch.bEnd, includeAnchorWitness->hasCondArmId ? 1 : 0,
                      includeAnchorWitness->condArmId,
                      includeAnchorWitness->hasChildIncludeId ? 1 : 0,
                      includeAnchorWitness->childIncludeId,
                      includeAnchorWitness->hasNeighborPP ? 1 : 0,
                      includeAnchorWitness->neighborPP)
            .str();
  }

  candidate.hasPayloadPreview = true;
  candidate.payloadPreview =
      stringutils::showWsWithClip(patch.insertBytes, 120);
  AttachLineControlObserverWitness(candidate);
  AttachCounterStateWitness(candidate);
  RefreshAcceptedCandidateEmissionPathInventory(candidate);
  AuditAcceptedResultCandidateForLegacyAuthority(
      candidate, "BuildAcceptedIncludeCandidate");
  TraceWitnessEmitted(
      BuildRefoldWitness(candidate, "BuildAcceptedIncludeCandidate"));
  return candidate;
}

RefoldEngine::ProofSummary RefoldEngine::BuildOwnerRealizationProofSummary(
    AcceptedPathKind currentPath,
    const OwnerRealizationResult &ownerRealization) const {
  // keeps spelling and materialization local to the include/TU
  // emitters.  This helper is therefore deliberately proof-only: it normalizes
  // an already-built owner closure into the shared OwnerRealizationWitness and
  // leaves candidate kind, byte/token interval, payload preview, and owner-id
  // decoration to the caller that actually knows the emitted surface.
  ProofSummary summary = BuildAcceptedPathProofSummary(
      currentPath, /*patch=*/nullptr, /*tuAnchorWitness=*/nullptr,
      /*includeAnchorWitness=*/nullptr);
  ApplyOwnerRealizationResultToProofSummary(summary, ownerRealization);
  return summary;
}

RefoldEngine::AcceptedResultCandidate
RefoldEngine::BuildAcceptedIncludeRealizationCandidate(
    AcceptedPathKind currentPath, const RefoldModel::IncludeItem &include,
    IncludeRealizationEvidenceKind evidenceKind,
    std::optional<IncludeRealizationBTokenEnvelope> bTokenEnvelope) const {
  const OwnerRealizationResult ownerRealization =
      BuildIncludeOwnerRealization(include, currentPath, evidenceKind,
                                   bTokenEnvelope);
  AcceptedResultCandidate candidate;
  candidate.kind = AcceptedResultCandidateKind::IncludePatch;
  candidate.proofSummary =
      BuildOwnerRealizationProofSummary(currentPath, ownerRealization);
  candidate.begin = include.siteB;
  candidate.end = include.siteE;
  candidate.hasPayloadPreview = true;
  candidate.payloadPreview = formatv("{0}", currentPath).str();

  if (bTokenEnvelope) {
    AttachMixedOwnerTilingWitnessForTokenEnvelope(
        candidate.proofSummary, include.cover.begin, include.cover.end,
        bTokenEnvelope->first, bTokenEnvelope->second);
  }

  candidate.hasOwnerIncludeId = true;
  candidate.ownerIncludeId = include.id;
  AttachLineControlObserverWitness(candidate);
  AttachCounterStateWitness(candidate);
  RefreshAcceptedCandidateEmissionPathInventory(candidate);
  AuditAcceptedResultCandidateForLegacyAuthority(
      candidate, "BuildAcceptedIncludeRealizationCandidate");
  TraceWitnessEmitted(BuildRefoldWitness(
      candidate, "BuildAcceptedIncludeRealizationCandidate"));
  return candidate;
}

RefoldEngine::AcceptedResultCandidate
RefoldEngine::BuildAcceptedTUTextEditCandidate(AcceptedPathKind currentPath,
                                               uint64_t begin, uint64_t end,
                                               StringRef payloadPreview) const {
  const OwnerRealizationResult ownerRealization =
      BuildTUOwnerRealization(currentPath, begin, end);
  AcceptedResultCandidate candidate;
  candidate.kind = AcceptedResultCandidateKind::TUTextEdit;
  candidate.proofSummary =
      BuildOwnerRealizationProofSummary(currentPath, ownerRealization);
  candidate.begin = begin;
  candidate.end = end;
  candidate.hasPayloadPreview = true;
  candidate.payloadPreview = payloadPreview.str();
  AttachLineControlObserverWitness(candidate);
  AttachCounterStateWitness(candidate);
  RefreshAcceptedCandidateEmissionPathInventory(candidate);
  AuditAcceptedResultCandidateForLegacyAuthority(
      candidate, "BuildAcceptedTUTextEditCandidate");
  TraceWitnessEmitted(
      BuildRefoldWitness(candidate, "BuildAcceptedTUTextEditCandidate"));
  return candidate;
}

RefoldEngine::AcceptedResultCandidate
RefoldEngine::BuildAcceptedTUAnchorCandidate(
    AcceptedPathKind currentPath, const TUAnchorWitness &witness) const {
  AcceptedResultCandidate candidate;
  candidate.kind = AcceptedResultCandidateKind::TUAnchor;

  // TU anchors are selector candidates for insertion/frontier proofs. Preserve
  // the witness-derived gap/byte position so later diagnostics can report the
  // exact anchor that discharged the path.
  candidate.proofSummary = BuildAcceptedPathProofSummary(
      currentPath, /*patch=*/nullptr, &witness);

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
        TUAnchorWitnessHasProvableEvidence(witness) || witness.exactPPMatch;
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
  AttachLineControlObserverWitness(candidate);
  AttachCounterStateWitness(candidate);
  RefreshAcceptedCandidateEmissionPathInventory(candidate);
  AuditAcceptedResultCandidateForLegacyAuthority(
      candidate, "BuildAcceptedTUAnchorCandidate");
  TraceWitnessEmitted(
      BuildRefoldWitness(candidate, "BuildAcceptedTUAnchorCandidate"));
  return candidate;
}

RefoldEngine::AcceptedResultCandidate
RefoldEngine::BuildAcceptedTerminalCandidate(
    const TerminalFallbackWitness &witness) const {
  AcceptedResultCandidate candidate;
  candidate.kind = AcceptedResultCandidateKind::TerminalOutOfDomain;

  // Terminal fallback is intentionally represented as an accepted candidate
  // only after the proof inventory has declared the case out-of-domain.
  candidate.proofSummary = BuildAcceptedPathProofSummary(
      AcceptedPathKind::TerminalEmitEditedPreprocessedStream,
      /*patch=*/nullptr, /*tuAnchorWitness=*/nullptr,
      /*includeAnchorWitness=*/nullptr, &witness);
  AttachLineControlObserverWitness(candidate);
  AttachCounterStateWitness(candidate);
  RefreshAcceptedCandidateEmissionPathInventory(candidate);
  AuditAcceptedResultCandidateForLegacyAuthority(
      candidate, "BuildAcceptedTerminalCandidate");
  TraceWitnessEmitted(
      BuildRefoldWitness(candidate, "BuildAcceptedTerminalCandidate"));
  return candidate;
}

// Implementation extracted verbatim to keep `RefoldEngine.cpp`
// physically smaller without changing ownership or semantics.
#include "RefoldEngine.AcceptedProofs.inc"



} // namespace refold
} // namespace clang
