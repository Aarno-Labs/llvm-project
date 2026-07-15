//===--- RefoldAcceptancePathClassifier.cpp ---------------------*- C++ -*-===//
//
// Acceptance-path proof-summary classifier — implementation.
//
// Lattice-side validators reached through `Dependencies` callbacks:
//   * `validateIncludePreservingProof` — include-side preserving proof
//   * `validateTUAnchorProof` — TU-anchor witness proof
// Both fire only on include/TU paths and stay off the macro hot path.
//
//===----------------------------------------------------------------------===//

#include "proof/RefoldAcceptancePathClassifier.h"

#include "core/RefoldLog.h"
#include "core/RefoldModel.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/FormatVariadic.h"

#include <algorithm>
#include <cstdint>
#include <optional>
#include <utility>

using namespace llvm;

namespace clang {
namespace refold {

RefoldAcceptancePathClassifier::RefoldAcceptancePathClassifier(
    Dependencies deps)
    : deps_(std::move(deps)) {}

::clang::refold::AcceptancePathInventory
RefoldAcceptancePathClassifier::InventoryMacroPatchProofAcceptancePath(
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
  case MacroPatchProofKind::RecursiveTupleGeneratedCalleeReplay:
    return BuildAcceptancePathInventory(
        AcceptedPathKind::MacroRecursiveTupleGeneratedCalleeReplay);
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

::clang::refold::AcceptancePathInventory
RefoldAcceptancePathClassifier::InventoryMacroPatchAcceptancePath(
    const MacroPatch &patch) const {
  // Keep the full-patch overload as a convenience shim only. The classification
  // decision itself is made from MacroPatchProof so the mapping has one source
  // of truth and cannot drift from the theorem-facing carrier.
  return InventoryMacroPatchProofAcceptancePath(patch.proof);
}

::clang::refold::AcceptancePathInventory
RefoldAcceptancePathClassifier::BuildAcceptancePathInventory(
    AcceptedPathKind currentPath) const {
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
  case AcceptedPathKind::MacroRecursiveTupleGeneratedCalleeReplay:
    inventory.support = AcceptanceSupportKind::ExplicitProofBacked;
    inventory.futureTarget =
        FutureProofTarget::MacroRecursiveTupleGeneratedCalleeReplay;
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
    // must fail closed and recertify onto a concrete include class first.
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

::clang::refold::TheoremProofClass
RefoldAcceptancePathClassifier::BuildTheoremProofClassForAcceptedPath(
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
  case AcceptedPathKind::MacroRecursiveTupleGeneratedCalleeReplay:
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

void RefoldAcceptancePathClassifier::RequireAcceptedPathBaseline(
    ProofDischargeAccumulator &discharge,
    const AcceptancePathInventory &inventory) const {
  discharge.Require(inventory.currentPath != AcceptedPathKind::Unknown,
                    ProofObligationKind::AcceptedPathClassified,
                    ProofFailureReason::MissingAcceptedPathClassification);
  discharge.Require(inventory.futureTarget != FutureProofTarget::Unknown,
                    ProofObligationKind::FutureTargetMapped,
                    ProofFailureReason::MissingFutureTargetMapping);
}

::clang::refold::ProofDischargeRecord
RefoldAcceptancePathClassifier::BuildAcceptedPathBaselineDischarge(
    const AcceptancePathInventory &inventory, bool explicitOutOfDomain) const {
  ProofDischargeAccumulator discharge;
  RequireAcceptedPathBaseline(discharge, inventory);
  if (explicitOutOfDomain) {
    discharge.Fail(ProofObligationKind::ExplicitOutOfDomainResultTracked,
                   ProofFailureReason::ExplicitOutOfDomainResult);
  }
  return discharge.Finish();
}

void RefoldAcceptancePathClassifier::RequireIncludeZeroWidthAnchor(
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

::clang::refold::ProofSummary
RefoldAcceptancePathClassifier::BuildAcceptedPathProofSummary(
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
    deps_.proofSummaryBuilder.ConfigureProofSummary(
        summary, BuildTheoremProofClassForAcceptedPath(currentPath),
        AcceptedProofClass::IncludePreserving,
        RealizationMode::PreserveOriginalStructure,
        SelectionPreference::PreferStructurePreservation,
        SurfaceDisposition::None,
        /*preservesInvocationStructure=*/true);
    if (includeAnchorWitness) {
      summary.hasIncludeAnchorWitness = true;
      summary.includeAnchorWitness = *includeAnchorWitness;
    }
    summary.discharge = deps_.validateIncludePreservingProof(
        currentPath, patch, includeAnchorWitness);
    break;

  case AcceptedPathKind::IncludeRealizationInlineFromB:
    deps_.proofSummaryBuilder.ConfigureProofSummary(
        summary, BuildTheoremProofClassForAcceptedPath(currentPath),
        AcceptedProofClass::IncludeRealization,
        RealizationMode::RealizeEditedSurface,
        SelectionPreference::PreferSurfaceRealization,
        SurfaceDisposition::RealizeInlineTouchedIncludesFromB,
        /*preservesInvocationStructure=*/false);
    // removes the last include-specific realization validator.  The
    // include path itself only needs the generic accepted-path baseline here;
    // deps_.ownerRealizationProofBuilder.BuildIncludeOwnerRealization() and
    // deps_.ownerRealizationProofBuilder.ApplyOwnerRealizationResultToProofSummary()
    // attach the authoritative OwnerRealizationProof immediately after the
    // owner-specific include spelling has supplied its evidence.
    summary.discharge = BuildAcceptedPathBaselineDischarge(summary.inventory);
    break;

  case AcceptedPathKind::IncludeMaterializedExpansion: {
    deps_.proofSummaryBuilder.ConfigureProofSummary(
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
    deps_.proofSummaryBuilder.ConfigureProofSummary(
        summary, BuildTheoremProofClassForAcceptedPath(currentPath),
        AcceptedProofClass::TUAnchor,
        RealizationMode::PreserveOriginalStructure,
        SelectionPreference::PreferExactAnchoring, SurfaceDisposition::None,
        /*preservesInvocationStructure=*/true);
    if (tuAnchorWitness) {
      summary.hasTUAnchorWitness = true;
      summary.tuAnchorWitness = *tuAnchorWitness;
    }
    summary.discharge =
        deps_.validateTUAnchorProof(currentPath, tuAnchorWitness);
    break;

  case AcceptedPathKind::TUByteSpanMappedEdit:
  case AcceptedPathKind::TUByteSpanConservativeEdit:
  case AcceptedPathKind::TUIncludeClosureEdit: {
    deps_.proofSummaryBuilder.ConfigureProofSummary(
        summary, BuildTheoremProofClassForAcceptedPath(currentPath),
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
    deps_.proofSummaryBuilder.ConfigureProofSummary(
        summary, BuildTheoremProofClassForAcceptedPath(currentPath),
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
  case AcceptedPathKind::MacroRecursiveTupleGeneratedCalleeReplay:
  case AcceptedPathKind::MacroDagSubtreeRoot:
  case AcceptedPathKind::MacroCallChainSuffix:
  case AcceptedPathKind::MacroCounterLiteral:
  case AcceptedPathKind::MacroWholeCoverRealization:
    break;
  }

  deps_.proofSummaryBuilder.FinalizeProofSummary(summary);
  return summary;
}

::clang::refold::ProofSummary
RefoldAcceptancePathClassifier::BuildIncludePatchProofSummary(
    bool realizedSurface, AcceptedPathKind currentPath,
    const IncludePatch *patch) const {
  (void)realizedSurface;
  (void)patch;

  // IncludePatch is a pre-materialization working object. By default it does
  // not claim any normalized accepted path at all; only the later
  // witness-backed materialization step may mint theorem-facing include
  // preserving or realization summaries. If a caller explicitly provides a
  // concrete include path, reuse the accepted-path classifier for that final
  // recertified state.
  if (currentPath == AcceptedPathKind::Unknown)
    return ProofSummary{};

  return BuildAcceptedPathProofSummary(currentPath, patch);
}

} // namespace refold
} // namespace clang
