//===--- RefoldMacroPatchProofClassifier.cpp --------------------*- C++ -*-===//
//
// Macro-patch proof classification and validation — implementation.
//
//===----------------------------------------------------------------------===//

#include "proof/RefoldMacroPatchProofClassifier.h"

#include "core/RefoldLog.h"
#include "core/RefoldModel.h"
#include "macro/RefoldMacroTopology.h"
#include "proof/RefoldOwnerStateProof.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/FormatVariadic.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

using namespace llvm;

namespace clang {
namespace refold {

namespace {

/// Return whether the recursive tuple witness names exact, non-overlapping
/// slices of one root tuple formal.
bool recursiveTupleGeneratedCalleeReplaySlicesAreWellFormed(
    const RecursiveTupleGeneratedCalleeReplayWitness &witness) {
  if (witness.actualSlices.empty())
    return false;

  for (size_t i = 0; i < witness.actualSlices.size(); ++i) {
    const GeneratedActualRootTupleSlice &slice = witness.actualSlices[i];
    if (slice.rootTupleFormalIndex != witness.rootTupleFormalIndex)
      return false;
    if (slice.rootTuplePayloadByteBegin >= slice.rootTuplePayloadByteEnd)
      return false;

    for (size_t j = i + 1; j < witness.actualSlices.size(); ++j) {
      const GeneratedActualRootTupleSlice &other = witness.actualSlices[j];
      if (slice.generatedActualIndex == other.generatedActualIndex)
        return false;
      if (slice.generatedFormalIndex == other.generatedFormalIndex)
        return false;
    }
  }

  std::vector<GeneratedActualRootTupleSlice> ordered = witness.actualSlices;
  llvm::sort(ordered, [](const GeneratedActualRootTupleSlice &lhs,
                         const GeneratedActualRootTupleSlice &rhs) {
    if (lhs.rootTuplePayloadByteBegin != rhs.rootTuplePayloadByteBegin)
      return lhs.rootTuplePayloadByteBegin < rhs.rootTuplePayloadByteBegin;
    if (lhs.rootTuplePayloadByteEnd != rhs.rootTuplePayloadByteEnd)
      return lhs.rootTuplePayloadByteEnd < rhs.rootTuplePayloadByteEnd;
    return lhs.generatedActualIndex < rhs.generatedActualIndex;
  });

  uint64_t previousEnd = 0;
  bool havePrevious = false;
  for (const GeneratedActualRootTupleSlice &slice : ordered) {
    if (havePrevious && slice.rootTuplePayloadByteBegin < previousEnd)
      return false;
    previousEnd = slice.rootTuplePayloadByteEnd;
    havePrevious = true;
  }

  return true;
}

/// Return whether the recursive tuple theorem witness names the same root and
/// carries the mandatory unique path / tuple / replay obligations.
bool recursiveTupleGeneratedCalleeReplayWitnessIsWellFormed(
    const MacroPatchProof &proof) {
  if (!proof.recursiveTupleGeneratedCalleeReplay)
    return false;

  const RecursiveTupleGeneratedCalleeReplayWitness &witness =
      *proof.recursiveTupleGeneratedCalleeReplay;
  return witness.rootInvocationId != 0 &&
         witness.rootInvocationId == proof.proofRootMacroId &&
         witness.terminalGeneratedInvocationId != 0 &&
         witness.terminalCalleeDefinitionDirectiveId != 0 &&
         witness.terminalGeneratedInvocationId != witness.rootInvocationId &&
         witness.rootCalleeFormalIndex != witness.rootTupleFormalIndex &&
         witness.uniquePath && witness.uniqueTupleFormal &&
         witness.uniqueReplaySolution &&
         recursiveTupleGeneratedCalleeReplaySlicesAreWellFormed(witness);
}

} // namespace

RefoldMacroPatchProofClassifier::RefoldMacroPatchProofClassifier(
    Dependencies deps)
    : deps_(std::move(deps)) {}

::clang::refold::ProofSummary
RefoldMacroPatchProofClassifier::ClassifyMacroPatchProof(
    const MacroPatch &patch) const {
  ProofSummary summary;
  const MacroPatchProof &proof = patch.proof;

  // MacroPatchProof is the sole proof-facing input to classification.  This
  // function deliberately does not read path-local construction metadata for
  // primary proof identity; otherwise construction side bits could become
  // theorem authority by another name.
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
  summary.inventory =
      deps_.acceptancePathClassifier.InventoryMacroPatchProofAcceptancePath(
          proof);

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
  case MacroPatchProofKind::RecursiveTupleGeneratedCalleeReplay:
    // Recursive tuple-generated-callee replay is structure-preserving only when
    // its witness proves one producer-recorded forwarding path and exact tuple
    // element slices for the terminal generated invocation.
  case MacroPatchProofKind::DagSubtreeRoot:
    // DAG-preserving rewrites must carry the explicit subtree certificate
    // recorded on accepted root patches.
  case MacroPatchProofKind::CallChainSuffix:
    // Call-chain suffix rewrites are emitted directly on the root callsite
    // slice, so the patch's owning macro id must already be that root.
    deps_.proofSummaryBuilder.ConfigureProofSummary(
        summary,
        deps_.acceptancePathClassifier.BuildTheoremProofClassForAcceptedPath(
            summary.inventory.currentPath),
        AcceptedProofClass::InvocationPreserving,
        RealizationMode::PreserveOriginalStructure,
        SelectionPreference::PreferStructurePreservation,
        SurfaceDisposition::None,
        /*preservesInvocationStructure=*/true);
    break;

  case MacroPatchProofKind::CounterLiteral:
    deps_.proofSummaryBuilder.ConfigureProofSummary(
        summary,
        deps_.acceptancePathClassifier.BuildTheoremProofClassForAcceptedPath(
            summary.inventory.currentPath),
        AcceptedProofClass::InvocationRealization,
        RealizationMode::RealizeEditedSurface,
        SelectionPreference::PreferSurfaceRealization, SurfaceDisposition::None,
        /*preservesInvocationStructure=*/false);
    break;

  case MacroPatchProofKind::WholeCoverRealization:
    deps_.proofSummaryBuilder.ConfigureProofSummary(
        summary,
        deps_.acceptancePathClassifier.BuildTheoremProofClassForAcceptedPath(
            summary.inventory.currentPath),
        AcceptedProofClass::InvocationRealization,
        RealizationMode::RealizeEditedSurface,
        SelectionPreference::PreferSurfaceRealization,
        SurfaceDisposition::RealizeWholeCoverMacros,
        /*preservesInvocationStructure=*/false);
    break;

  case MacroPatchProofKind::Unknown:
    // Unknown macro proof kind means the patch lacks the required primary
    // proof-class witness and will be rejected by
    // deps_.proofSummaryBuilder.FinalizeProofSummary().
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
    // Identity proofs have no macro-local validator here; they are accepted
    // only through the generic summary/discharge gate below.
  case TheoremProofClass::DirectivePreservingProof:
  case TheoremProofClass::StateRepairProof:
  case TheoremProofClass::MixedOwnerTilingProof:
  case TheoremProofClass::TerminalOutOfDomainProof:
    break;
  }

  deps_.proofSummaryBuilder.FinalizeProofSummary(summary);
  return summary;
}

void RefoldMacroPatchProofClassifier::RefreshMacroPatchDerivedProofWitnesses(
    MacroPatch &patch) const {
  // This helper does not reconstruct primary proof identity from side fields.
  // It only enriches the canonical carrier with witnesses whose raw evidence is
  // stored as patch-local construction metadata: paste replay status, DAG
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
      patch.subtree.backed) {
    proof.subtree = patch.subtree;
  }

  if (proof.kind == MacroPatchProofKind::CallChainSuffix) {
    CallChainWitness witness;
    witness.rootMacroId = proof.proofRootMacroId;
    witness.callsiteMacroId = patch.macroId;
    proof.callChain = witness;
  }

  patch.proof = std::move(proof);
}

void RefoldMacroPatchProofClassifier::SyncMacroPatchProofSummary(
    MacroPatch &patch) const {
  RefreshMacroPatchDerivedProofWitnesses(patch);
  patch.proofSummary = ClassifyMacroPatchProof(patch);
}

bool RefoldMacroPatchProofClassifier::
    MacroInvocationHasWellFormedPasteWitnesses(
        const RefoldModel::MacroInvocation &m) const {
  // Consume the exact `paste_tokens` witnesses serialized by the producer.
  // The producer records the argument-derived fragments of each pasted token,
  // but it does not need to emit the literal glue bytes that came from the
  // macro body itself. For example, `X##_##Y` may serialize only the `X` and
  // `Y` slices while leaving the `_` as an uncovered gap in the final spelling.
  //
  // The witness stream is therefore usable when each recorded fragment is
  // ordered, non-overlapping, non-empty, and stays within the pasted token's
  // spelling, and when every argument-derived fragment names a valid formal.
  // Requiring a contiguous partition of the final spelling would incorrectly
  // reject perfectly valid producer witnesses for common paste patterns.
  //
  // There is one additional producer-side case that must also be treated as
  // witness-backed: a parent invocation can carry `pasteSpans` that are only
  // propagated child-paste contributors inside a standard occurrence of the
  // same parent formal. Those spans are validated at the child/current-surface
  // level by the existing args-only safety gates and do not require a direct
  // parent-level `paste_tokens` decomposition.
  auto hasOnlyPropagatedChildPasteSpans = [&]() -> bool {
    if (m.pasteSpans.empty())
      return false;

    DenseMap<uint32_t, SmallVector<std::pair<uint64_t, uint64_t>, 4>>
        standardOccByteRangesByArg;
    for (const auto &occ : m.argSpans) {
      if (occ.kind != PPArgSpanKind::Standard || !occ.ppByteBegin ||
          !occ.ppByteEnd)
        continue;
      standardOccByteRangesByArg[occ.argIdx].push_back(
          {static_cast<uint64_t>(*occ.ppByteBegin),
           static_cast<uint64_t>(*occ.ppByteEnd)});
    }

    bool sawPasteSpan = false;
    for (const auto &ps : m.pasteSpans) {
      sawPasteSpan = true;
      if (!ps.ppByteBegin || !ps.ppByteEnd)
        return false;

      auto stdIt = standardOccByteRangesByArg.find(ps.argIdx);
      if (stdIt == standardOccByteRangesByArg.end())
        return false;

      const std::pair<uint64_t, uint64_t> spanBytes = {
          static_cast<uint64_t>(*ps.ppByteBegin),
          static_cast<uint64_t>(*ps.ppByteEnd)};
      if (!llvm::is_contained(stdIt->second, spanBytes))
        return false;
    }

    return sawPasteSpan;
  };

  if (m.pasteTokens.empty())
    return hasOnlyPropagatedChildPasteSpans();

  llvm::SmallDenseSet<uint32_t, 8> pasteSpanArgIndices;
  for (const auto &ps : m.pasteSpans)
    pasteSpanArgIndices.insert(ps.argIdx);

  for (const auto &tok : m.pasteTokens) {
    if (tok.spelling.empty() || tok.parts.empty())
      return false;

    uint32_t prevEnd = 0;
    bool sawArgDerivedPart = false;
    for (const auto &part : tok.parts) {
      if (part.byteEnd <= part.byteBegin || part.byteEnd > tok.spelling.size())
        return false;

      // Parts must remain in producer order and may be adjacent or separated by
      // literal macro-body glue, but they must never overlap or move backward.
      if (part.byteBegin < prevEnd)
        return false;

      if (part.argIndex) {
        if (*part.argIndex >= m.defParams.size())
          return false;
        if (!pasteSpanArgIndices.empty() &&
            !pasteSpanArgIndices.count(*part.argIndex))
          return false;
        sawArgDerivedPart = true;
      }

      prevEnd = part.byteEnd;
    }

    // A pasted-token witness that never attributes any bytes back to a formal
    // is not useful for args-only paste preservation.
    if (!sawArgDerivedPart)
      return false;
  }

  return true;
}

::clang::refold::ProofDischargeRecord
RefoldMacroPatchProofClassifier::ValidateInvocationPreservingProofImpl(
    const MacroPatch &patch, bool requireTopLevelRoot) const {
  ProofDischargeAccumulator discharge;
  const MacroPatchProof &proof = patch.proof;
  const AcceptancePathInventory inventory =
      deps_.acceptancePathClassifier.InventoryMacroPatchProofAcceptancePath(
          proof);

  deps_.acceptancePathClassifier.RequireAcceptedPathBaseline(discharge,
                                                             inventory);
  discharge.Require(proof.proofRootMacroId != 0,
                    ProofObligationKind::ProofRootTracked,
                    ProofFailureReason::MissingProofRoot);

  const RefoldModel::MacroInvocation *root =
      proof.proofRootMacroId
          ? deps_.macroTopology.FindMacroInvocationById(proof.proofRootMacroId)
          : nullptr;
  discharge.Require(root != nullptr,
                    ProofObligationKind::MacroProofRootResolved,
                    ProofFailureReason::MissingMacroProofRootResolution);
  if (requireTopLevelRoot && root) {
    discharge.Require(deps_.macroTopology.GetRootMacroId(root->id) == root->id,
                      ProofObligationKind::MacroProofRootIsTopLevel,
                      ProofFailureReason::NonTopLevelMacroProofRoot);
  }

  switch (proof.kind) {
  case MacroPatchProofKind::ArgsOnlyStandard:
    break;

  case MacroPatchProofKind::ArgsOnlyPasteSingle:
  case MacroPatchProofKind::ArgsOnlyPasteMulti:
  case MacroPatchProofKind::ArgsOnlyPurePasteOnly:
    discharge.Require(root && !root->pasteSpans.empty(),
                      ProofObligationKind::MacroPasteWitnessPresent,
                      ProofFailureReason::MissingPasteWitness);
    if (root && !root->pasteSpans.empty()) {
      const bool hasProducerWitness =
          MacroInvocationHasWellFormedPasteWitnesses(*root);
      const bool replayValidated = proof.paste && proof.paste->replayValidated;
      discharge.Require(hasProducerWitness || replayValidated,
                        ProofObligationKind::MacroPasteWitnessWellFormed,
                        ProofFailureReason::MalformedPasteWitness);
    }
    break;

  case MacroPatchProofKind::ArgsOnlyPairedPureInsertion:
    discharge.Require(root && root->pasteSpans.empty(),
                      ProofObligationKind::MacroPasteFreeSurfaceTracked,
                      ProofFailureReason::UnexpectedPasteSurface);
    break;

  case MacroPatchProofKind::PasteDerivedCalleeSelector:
    // The selector-substitution builder validates the hard semantic witness
    // before certifying the patch: a single existing pasted-callee target must
    // reproduce the edited B expansion, the original target must reproduce the
    // A expansion under the same non-selector arguments, and the root edit must
    // rewrite only the proved selector argument. Keep validation here to the
    // common invocation-preserving obligations above so compatible same-root
    // merges do not become invalid merely because their materialized edit-map
    // subrange is no longer representable as one selector slice.
    break;

  case MacroPatchProofKind::RecursiveTupleGeneratedCalleeReplay: {
    const RecursiveTupleGeneratedCalleeReplayWitness *witness =
        proof.recursiveTupleGeneratedCalleeReplay
            ? &*proof.recursiveTupleGeneratedCalleeReplay
            : nullptr;
    const RefoldModel::MacroInvocation *terminal =
        witness ? deps_.macroTopology.FindMacroInvocationById(
                      witness->terminalGeneratedInvocationId)
                : nullptr;
    const bool terminalDefinitionMatchesWitness =
        terminal && terminal->definitionDirectiveId &&
        *terminal->definitionDirectiveId ==
            witness->terminalCalleeDefinitionDirectiveId;
    discharge.Require(
        recursiveTupleGeneratedCalleeReplayWitnessIsWellFormed(proof) &&
            terminalDefinitionMatchesWitness,
        ProofObligationKind::
            MacroRecursiveTupleGeneratedCalleeReplayWitnessTracked,
        ProofFailureReason::MissingRecursiveTupleGeneratedCalleeReplayWitness);
    discharge.Require(
        witness && witness->uniquePath,
        ProofObligationKind::
            MacroRecursiveTupleGeneratedCalleeReplayPathUnique,
        ProofFailureReason::NonUniqueRecursiveTupleGeneratedCalleeReplayPath);
    discharge.Require(
        witness && witness->uniqueTupleFormal,
        ProofObligationKind::
            MacroRecursiveTupleGeneratedCalleeReplayTupleUnique,
        ProofFailureReason::NonUniqueRecursiveTupleGeneratedCalleeReplayTuple);
    discharge.Require(
        witness && witness->uniqueReplaySolution,
        ProofObligationKind::
            MacroRecursiveTupleGeneratedCalleeReplayReplayUnique,
        ProofFailureReason::
            NonUniqueRecursiveTupleGeneratedCalleeReplaySolution);
    discharge.Require(
        witness && !witness->actualSlices.empty(),
        ProofObligationKind::
            MacroRecursiveTupleGeneratedCalleeReplaySlicesTracked,
        ProofFailureReason::MissingRecursiveTupleGeneratedCalleeReplaySlices);
    discharge.Require(
        witness &&
            recursiveTupleGeneratedCalleeReplaySlicesAreWellFormed(*witness),
        ProofObligationKind::
            MacroRecursiveTupleGeneratedCalleeReplaySlicesNonOverlapping,
        ProofFailureReason::
            OverlappingRecursiveTupleGeneratedCalleeReplaySlices);
    break;
  }

  case MacroPatchProofKind::DagSubtreeRoot:
    discharge.Require(proof.subtree && proof.subtree->backed,
                      ProofObligationKind::MacroSubtreeCertificateTracked,
                      ProofFailureReason::MissingSubtreeCertificate);
    if (proof.subtree && proof.subtree->backed) {
      discharge.Require(proof.subtree->admissible,
                        ProofObligationKind::SubtreeAdmissibilityTracked,
                        ProofFailureReason::MissingSubtreeAdmissibility);
    }
    break;

  case MacroPatchProofKind::CallChainSuffix:
    discharge.Require(proof.callChain &&
                          proof.callChain->rootMacroId ==
                              proof.proofRootMacroId &&
                          proof.callChain->callsiteMacroId == patch.macroId &&
                          patch.macroId == proof.proofRootMacroId,
                      ProofObligationKind::MacroCallChainWitnessTracked,
                      ProofFailureReason::MissingCallChainWitness);
    break;

  case MacroPatchProofKind::CounterLiteral:
  case MacroPatchProofKind::WholeCoverRealization:
  case MacroPatchProofKind::Unknown:
    break;
  }

  return discharge.Finish();
}

::clang::refold::ProofDischargeRecord
RefoldMacroPatchProofClassifier::ValidateInvocationPreservingProof(
    const MacroPatch &patch) const {
  return ValidateInvocationPreservingProofImpl(patch,
                                               /*requireTopLevelRoot=*/true);
}

::clang::refold::ProofDischargeRecord
RefoldMacroPatchProofClassifier::ValidateEmittedInvocationPreservingProof(
    const MacroPatch &patch) const {
  return ValidateInvocationPreservingProofImpl(patch,
                                               /*requireTopLevelRoot=*/false);
}

::clang::refold::ProofDischargeRecord
RefoldMacroPatchProofClassifier::ValidateInvocationRealizationProof(
    const MacroPatch &patch) const {
  ProofDischargeAccumulator discharge;
  const MacroPatchProof &proof = patch.proof;
  const AcceptancePathInventory inventory =
      deps_.acceptancePathClassifier.InventoryMacroPatchProofAcceptancePath(
          proof);

  // Invocation-realization patches still need the common accepted-path
  // metadata, but unlike invocation-preserving proofs they are expected to
  // realize expansion text rather than preserve source invocation structure.
  deps_.acceptancePathClassifier.RequireAcceptedPathBaseline(discharge,
                                                             inventory);
  discharge.Require(proof.proofRootMacroId != 0,
                    ProofObligationKind::ProofRootTracked,
                    ProofFailureReason::MissingProofRoot);

  if (proof.kind == MacroPatchProofKind::WholeCoverRealization) {
    // Whole-cover realization has additional audit obligations: the A/B token
    // envelopes, adjusted B envelope, containment status, and boundary
    // accounting must all be explicitly recorded on the emitted patch.
    const bool boundsTracked =
        patch.wholeCover.aLo <= patch.wholeCover.aHi &&
        patch.wholeCover.bRawLo <= patch.wholeCover.bRawHi &&
        patch.wholeCover.bAdjLo <= patch.wholeCover.bAdjHi;
    const bool boundaryAccountingTracked =
        boundsTracked && patch.wholeCover.bRawLo <= patch.wholeCover.bAdjLo &&
        patch.wholeCover.bAdjHi <= patch.wholeCover.bRawHi;
    discharge.Require(boundsTracked,
                      ProofObligationKind::WholeCoverBoundsTracked,
                      ProofFailureReason::MissingWholeCoverBounds);
    // Nested/coarse-span whole-cover fallback is gone.  A macro realization
    // must now carry the direct self-contained owner-cover proof that was
    // checked before
    // deps_.ownerRealizationProofBuilder.BuildMacroWholeCoverOwnerRealization()
    // certified the shared OwnerRealizationProof.
    discharge.Require(patch.wholeCover.selfContained,
                      ProofObligationKind::WholeCoverContainmentTracked,
                      ProofFailureReason::MissingWholeCoverContainment);
    discharge.Require(boundaryAccountingTracked,
                      ProofObligationKind::WholeCoverBoundaryAccountingTracked,
                      ProofFailureReason::MissingWholeCoverBoundaryAccounting);
    discharge.Require(proof.ownerRealization &&
                          proof.ownerRealization->evidence ==
                              OwnerRealizationEvidenceKind::MacroWholeCover,
                      ProofObligationKind::OwnerRealizationWitnessTracked,
                      ProofFailureReason::MissingOwnerRealizationWitness);
  } else {
    // Non-whole-cover realization paths are admitted by their explicit macro
    // proof kind plus a typed state-stability witness when the proof depends on
    // stabilizing preprocessor state. There is no separate legacy validation
    // bit in the theorem vocabulary.
    if (proof.kind == MacroPatchProofKind::CounterLiteral) {
      const SuffixStabilityWitnessKind kind =
          proof.suffixStability ? proof.suffixStability->kind
                                : SuffixStabilityWitnessKind::None;
      const bool counterWitnessTracked =
          proof.suffixStability &&
          deps_.ownerStateProof.SuffixStabilityWitnessNamesComponent(
              *proof.suffixStability, OwnerStateComponent::Counter) &&
          (kind == SuffixStabilityWitnessKind::Literalization ||
           kind == SuffixStabilityWitnessKind::OwnerMaterialization ||
           kind == SuffixStabilityWitnessKind::ClosureWidening);
      discharge.Require(counterWitnessTracked,
                        ProofObligationKind::CounterStateWitnessTracked,
                        ProofFailureReason::MissingCounterStateWitness);
    }
  }

  return discharge.Finish();
}

} // namespace refold
} // namespace clang
