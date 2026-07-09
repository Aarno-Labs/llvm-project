//===--- RefoldMacroPatchProofCertifier.cpp ---------------------*- C++ -*-===//
//
// Implementation of the macro-patch proof certifier.  See the header for the
// architectural contract.  Each method is a small, focused certification
// operation; nothing here builds proof summaries or accepted-result keys.
//
//===----------------------------------------------------------------------===//

#include "macro/RefoldMacroPatchProofCertifier.h"

#include "core/RefoldLog.h"
#include "macro/RefoldMacroPlannerHelpers.h"
#include "proof/RefoldProofLattice.h"

#include <utility>

namespace clang {
namespace refold {

RefoldMacroPatchProofCertifier::RefoldMacroPatchProofCertifier(
    Dependencies deps)
    : deps_(std::move(deps)) {}

void RefoldMacroPatchProofCertifier::
    CertifyInvocationRewriteMaterializedOutputRange(
        MacroPatch &patch, uint64_t outputByteStart,
        uint64_t outputByteEnd) const {
  patch.materialized.hasOutputByteRange = true;
  patch.materialized.outputByteStart = outputByteStart;
  patch.materialized.outputByteEnd = outputByteEnd;
}

void RefoldMacroPatchProofCertifier::CertifySelectedFinalMacroCandidate(
    const RefoldModel::MacroInvocation &invocation,
    const SelectedMacroSelectionCandidate &selectedCandidate,
    MacroPatch &selectedPatch) const {
  if (selectedCandidate.candidate.emittedCandidate) {
    deps_.lattice.AcceptedCandidateBuilder().CertifySelectedMacroPatchCandidate(
        selectedPatch, *selectedCandidate.candidate.emittedCandidate,
        "macro/final-selector");
    return;
  }

  // A selector-only macro proof may choose the concrete spelling, but it is
  // not an emitted accepted artifact.  Leave the patch uncertified so the
  // final emission-bucket gate must either construct a theorem-normalized
  // emitted carrier or fail closed under the strict/theorem no-legacy audit.
  REFOLD_LOG_TRACE(
      "macro/proof",
      "selected macro candidate has no emitted accepted carrier: inv "
      "id={0} name={1} proofKind={2} selectorOnly={3}",
      invocation.id, invocation.name, selectedPatch.proof.kind,
      selectedCandidate.candidate.selectorOnly ? 1 : 0);
}

void RefoldMacroPatchProofCertifier::AttachWholeCoverProofCarrier(
    MacroPatch &patch, const WholeCoverPlan &plan,
    const RefoldModel::MacroInvocation &invocation) const {
  deps_.lattice.CertifyMacroWholeCoverRealizationPatch(patch, plan, invocation);
}

void RefoldMacroPatchProofCertifier::CertifyWholeCoverAcceptedCandidate(
    MacroPatch &patch, const WholeCoverPlan &plan,
    const RefoldModel::MacroInvocation &invocation) const {
  // Whole-cover realization has a single certifying operation: it records the
  // materialized B-token envelope, whole-cover diagnostics, proof kind, proof
  // root, and owner-realization witness together.  Keep that operation atomic
  // by routing through the named proof-carrier helper.
  AttachWholeCoverProofCarrier(patch, plan, invocation);
}

void RefoldMacroPatchProofCertifier::CertifyReusedMacroPatchAcceptedCandidate(
    MacroPatch &patch) const {
  (void)patch;
  // Reuse must not manufacture a fresh theorem carrier.  The selected patch
  // is an already accepted same-span patch, so its proof kind, proof-root id,
  // materialized range, and owner certificate are intentionally preserved.
}

void RefoldMacroPatchProofCertifier::SetArgsOnlyStandardProof(
    MacroPatch &patch, const RefoldModel::MacroInvocation &m,
    bool wholeEnvelopeReplayValidated,
    bool definitionTapeReplayValidated) const {
  MacroPatchProof proof = deps_.lattice.MakeMacroPatchProof(
      MacroPatchProofKind::ArgsOnlyStandard,
      /*preservesInvocationStructure=*/true, m.id);
  if (wholeEnvelopeReplayValidated) {
    WholeEnvelopeReplayWitness witness;
    witness.rootMacroId = m.id;
    witness.replayValidated = true;
    witness.definitionTapeReplayValidated = definitionTapeReplayValidated;
    proof.wholeEnvelopeReplay = witness;
  }
  deps_.lattice.SetMacroPatchProof(patch, std::move(proof));
}

void RefoldMacroPatchProofCertifier::CertifyGeneratedCalleeReplayProof(
    MacroPatch &patch, const RefoldModel::MacroInvocation &m,
    uint64_t finalDirectiveId, uint32_t generatedCallDepth,
    uint32_t objectAliasHops, bool usesStringification, bool usesPaste,
    bool usesVariadicForwarding, bool decodedStringLiteralEvidenceOnly) const {
  // This only enriches the already-validated whole-envelope replay.  It does
  // not reclassify the macro proof or change selector behavior.
  MacroPatchProof proof = patch.proof;
  GeneratedCalleeReplayWitness witness;
  witness.rootMacroId = m.id;
  witness.finalDirectiveId = finalDirectiveId;
  witness.generatedCallDepth = generatedCallDepth;
  witness.objectAliasHops = objectAliasHops;
  witness.calleeChainDeterministic = true;
  witness.replacementReplayValidated = true;
  witness.solvedActualsMappedToRoot = true;
  witness.usesForwarding = true;
  witness.usesStringification = usesStringification;
  witness.usesPaste = usesPaste;
  witness.usesVariadicForwarding = usesVariadicForwarding;
  witness.usesObjectAlias = objectAliasHops != 0;
  witness.decodedStringLiteralEvidenceOnly =
      decodedStringLiteralEvidenceOnly || usesStringification;
  proof.generatedCalleeReplay = std::move(witness);
  deps_.lattice.SetMacroPatchProof(patch, std::move(proof));
}

void RefoldMacroPatchProofCertifier::AttachArgsOnlyProofCarrier(
    MacroPatch &patch, const RefoldModel::MacroInvocation &invocation,
    bool wholeEnvelopeReplayValidated,
    bool definitionTapeReplayValidated) const {
  SetArgsOnlyStandardProof(patch, invocation, wholeEnvelopeReplayValidated,
                           definitionTapeReplayValidated);
}

void RefoldMacroPatchProofCertifier::CertifyArgsOnlyAcceptedCandidate(
    MacroPatch &patch, uint64_t outputByteStart, uint64_t outputByteEnd,
    uint64_t bTokenStart, uint64_t bTokenEnd) const {
  patch.materialized.hasOutputByteRange = true;
  patch.materialized.outputByteStart = outputByteStart;
  patch.materialized.outputByteEnd = outputByteEnd;
  certifyMacroPatchMaterializedBTokenRange(patch, bTokenStart, bTokenEnd);
}

} // namespace refold
} // namespace clang
