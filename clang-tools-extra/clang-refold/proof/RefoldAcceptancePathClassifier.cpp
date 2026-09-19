//===--- RefoldAcceptancePathClassifier.cpp ---------------------*- C++ -*-===//
//
// Acceptance-path proof-summary classifier — implementation.
//
// Path-specific validators:
//   * `ValidateIncludePreservingProof` — include-side preserving proof
//   * `validateTUAnchorProof` — the shared TU-anchor witness theorem
// Both fire only on include/TU paths and stay off the macro hot path.
//
//===----------------------------------------------------------------------===//

#include "proof/RefoldAcceptancePathClassifier.h"

#include "edit/RefoldTUAnchorProof.h"
#include "model/RefoldModel.h"
#include "support/RefoldLog.h"

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
  case MacroPatchProofKind::DirectCalleeSubstitution:
    return BuildAcceptancePathInventory(
        AcceptedPathKind::MacroDirectCalleeSubstitution);
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
  case AcceptedPathKind::MacroDirectCalleeSubstitution:
    inventory.support = AcceptanceSupportKind::ExplicitProofBacked;
    inventory.futureTarget = FutureProofTarget::MacroDirectCalleeSubstitution;
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
  case AcceptedPathKind::IncludeInsertPrintedPragmaPlacement:
    inventory.support = AcceptanceSupportKind::ExplicitProofBacked;
    inventory.futureTarget =
        FutureProofTarget::IncludeInsertionByPrintedPragmaPlacement;
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
  case AcceptedPathKind::MacroDirectCalleeSubstitution:
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
  case AcceptedPathKind::IncludeInsertPrintedPragmaPlacement:
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
  case AcceptedPathKind::IncludeInsertPrintedPragmaPlacement:
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
    summary.discharge = ValidateIncludePreservingProof(currentPath, patch,
                                                       includeAnchorWitness);
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
    // RefoldOwnerRealizationProofBuilder::BuildIncludeOwnerRealization() and
    // ApplyOwnerRealizationResultToProofSummary() attach the authoritative
    // OwnerRealizationProof immediately after the
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
        validateTUAnchorProof(currentPath, tuAnchorWitness,
                              BuildAcceptancePathInventory(currentPath));
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
  case AcceptedPathKind::MacroDirectCalleeSubstitution:
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
    bool /*realizedSurface*/, AcceptedPathKind currentPath,
    const IncludePatch *patch) const {
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

::clang::refold::ProofDischargeRecord
RefoldAcceptancePathClassifier::ValidateIncludePreservingProof(
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
      BuildAcceptancePathInventory(currentPath);

  // Common include-preserving baseline: the accepted path must be classified,
  // mapped to a future proof target, and backed by an actual include patch
  // shape before path-specific witness obligations are checked below.
  RequireAcceptedPathBaseline(discharge, inventory);
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
    RequireIncludeZeroWidthAnchor(
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
    RequireIncludeZeroWidthAnchor(
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
      for (const auto &child : deps_.model.GetIncludes()) {
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
    RequireIncludeZeroWidthAnchor(
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
    RequireIncludeZeroWidthAnchor(
        discharge, *patch, witness, IncludeAnchorEvidenceKind::LeftNeighborPP,
        ProofObligationKind::IncludeLeftNeighborWitnessTracked,
        ProofFailureReason::MissingIncludeLeftNeighborWitness);
    discharge.Require(witness && witness->hasNeighborPP,
                      ProofObligationKind::IncludeLeftNeighborWitnessTracked,
                      ProofFailureReason::MissingIncludeLeftNeighborWitness);
    break;

  case AcceptedPathKind::IncludeInsertPrintedPragmaPlacement:
    // A printed-pragma placement anchors at the edge of a header directive
    // line that B prints on the other side of the payload; the anchor byte is
    // that edge, derived from the directive's own census interval.
    RequireIncludeZeroWidthAnchor(
        discharge, *patch, witness,
        IncludeAnchorEvidenceKind::PrintedPragmaPlacement,
        ProofObligationKind::IncludePrintedPragmaPlacementWitnessTracked,
        ProofFailureReason::MissingIncludePrintedPragmaPlacementWitness);
    break;

  case AcceptedPathKind::IncludeInsertDeclBoundary:
    // Declaration-boundary insertion is anchored at the end of the declaration
    // header range. Requiring anchorByte == declHeaderE prevents a witness from
    // naming the right range but anchoring at a different byte.
    RequireIncludeZeroWidthAnchor(
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
  case AcceptedPathKind::MacroRecursiveTupleGeneratedCalleeReplay:
  case AcceptedPathKind::MacroDagSubtreeRoot:
  case AcceptedPathKind::MacroCallChainSuffix:
  case AcceptedPathKind::MacroCounterLiteral:
  case AcceptedPathKind::MacroWholeCoverRealization:
  case AcceptedPathKind::MacroDirectCalleeSubstitution:
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

} // namespace refold
} // namespace clang
