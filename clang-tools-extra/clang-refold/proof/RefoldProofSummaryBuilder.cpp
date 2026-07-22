//===--- RefoldProofSummaryBuilder.cpp --------------------------*- C++ -*-===//
//
// Implementation of the proof-summary builder service.  See the header for
// the architectural contract.  The per-method comments below restate the
// admission rules enforced at each step.
//
//===----------------------------------------------------------------------===//

#include "proof/RefoldProofSummaryBuilder.h"

#include "proof/RefoldAcceptedResultTypes.h"
#include "proof/RefoldProofVocabulary.h"
#include "proof/RefoldStructuralHunkTilingProof.h"
#include "proof/RefoldTheoremAudit.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/STLExtras.h"

#include <cstdint>
#include <optional>
#include <set>
#include <string>
#include <utility>

using namespace llvm;

namespace clang {
namespace refold {
namespace {

/// Return whether one owner-state witness is the forbidden widening form for
/// ordinary direct TU byte-span evidence.
bool isClosureWideningWitness(const SuffixStabilityWitness &witness) {
  return witness.kind == SuffixStabilityWitnessKind::ClosureWidening;
}

} // namespace

RefoldProofSummaryBuilder::RefoldProofSummaryBuilder(Dependencies deps)
    : deps_(std::move(deps)) {}

void RefoldProofSummaryBuilder::ConfigureProofSummary(
    ProofSummary &summary, TheoremProofClass theoremClass,
    AcceptedProofClass acceptedClass, RealizationMode realizationMode,
    SelectionPreference preference, SurfaceDisposition surfaceDisposition,
    bool structurePreserving) const {
  summary.theoremClass = theoremClass;
  summary.acceptedClass = acceptedClass;
  summary.realizationMode = realizationMode;
  summary.preference = preference;
  summary.surfaceDisposition = surfaceDisposition;
  summary.structurePreserving = structurePreserving;

  // A configured theorem-facing summary must name its final theorem class
  // directly. AcceptedProofClass is now construction provenance only; this flag
  // therefore tracks whether the builder declared a final TheoremProofClass,
  // not whether it certified an implementation-local accepted class.
  summary.primaryProofClassExplicit =
      theoremClass != TheoremProofClass::Unknown;
}

bool RefoldProofSummaryBuilder::ProofSummaryRequiresOwnerRealizationWitness(
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
  case AcceptedPathKind::MacroDirectCalleeSubstitution:
  case AcceptedPathKind::MacroPasteDerivedCalleeSelector:
  case AcceptedPathKind::MacroRecursiveTupleGeneratedCalleeReplay:
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

void RefoldProofSummaryBuilder::FinalizeProofSummary(
    ProofSummary &summary) const {
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

::clang::refold::EmittedProof
RefoldProofSummaryBuilder::BuildEmittedProofFromSummary(
    TheoremProofClass theoremClass, const ProofSummary &summary) {
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

std::optional<::clang::refold::EmittedProof>
RefoldProofSummaryBuilder::BuildCanonicalEmittedProofFromSummary(
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
    // No caller may use the fallback path itself as proof.
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
    if (!summary.hasMixedOwnerTilingWitness ||
        !summary.hasMixedOwnerTilingSegmentSelection)
      return std::nullopt;
    const MixedOwnerTilingWitness &witness = summary.mixedOwnerTilingWitness;
    if (!witness.uniquePartition || !witness.stateSummariesComposed ||
        !witness.stateTransitionsComposed ||
        !witness.ownerBoundariesComposed ||
        !witness.targetTokenStreamComposed || !witness.compositionEdgesProven ||
        witness.reason == StructuralTilingReason::Unknown ||
        witness.tokenSegmentCount == 0 || witness.edges.empty() ||
        witness.globalTargetPPTokenSignature.empty() ||
        witness.globalCompositionSignature.empty() ||
        witness.originalAEnd < witness.originalAStart ||
        witness.originalBEnd < witness.originalBStart ||
        witness.originalBEnd > deps_.bToks.size() ||
        summary.mixedOwnerTilingSegmentIndex >= witness.edges.size()) {
      return std::nullopt;
    }

    const bool deleteOnlyWitness =
        witness.originalBStart == witness.originalBEnd;
    uint64_t expectedA = witness.originalAStart;
    uint64_t expectedB = witness.originalBStart;
    uint32_t tokenSegmentCount = 0;
    uint32_t stateGapCount = 0;
    uint32_t protectedStructureGapCount = 0;
    uint32_t preservedInPlaceGapCount = 0;
    std::set<std::string> distinctRealizerSignatures;
    bool allTokenSegmentsPreserveProtectedStructure = true;
    uint32_t emptyBTokenSegmentCount = 0;
    for (size_t i = 0; i < witness.edges.size(); ++i) {
      const MixedOwnerTilingSegmentWitness &segment = witness.edges[i];
      if (segment.parentTilingWitnessId != witness.witnessId ||
          segment.segmentIndex != i || segment.sourceOrderPosition != i ||
          !segment.ownerClosureComplete || !segment.ownerIdentityKnown ||
          segment.ownerIdentity.kind == OwnerKind::Unknown ||
          segment.ownerSignature.empty() || segment.sourceSignature.empty() ||
          segment.producerPathSignature.empty() ||
          segment.targetPPTokenSignature.empty() ||
          !segment.sourceByteRangeKnown || segment.sourcePath.empty() ||
          segment.sourceEnd < segment.sourceBegin ||
          segment.aEnd < segment.aStart || segment.bEnd < segment.bStart ||
          segment.bEnd > deps_.bToks.size()) {
        return std::nullopt;
      }

      switch (segment.kind) {
      case MixedOwnerTilingEdgeKind::TokenSegment:
        if (segment.zeroTokenStateGap || segment.aStart != expectedA ||
            segment.bStart != expectedB || segment.aEnd <= segment.aStart ||
            segment.protectedPreprocessingStructure ||
            segment.protectedStructureIdentityRecorded ||
            segment.protectedStructureKind !=
                StructuralProtectedStructureKind::Unknown ||
            segment.producerIdentityKind !=
                StructuralProducerIdentityKind::None ||
            segment.producerItemId || segment.producerConditionalGroupId ||
            segment.producerConditionalArmId ||
            segment.sourceBytesPreservedUnchanged ||
            segment.gapDisposition != StructuralGapDisposition::Unknown ||
            (!segment.allowEmptyBEnvelope && segment.bEnd <= segment.bStart) ||
            (segment.allowEmptyBEnvelope &&
             (segment.bStart != segment.bEnd ||
              (deleteOnlyWitness &&
               segment.bStart != witness.originalBStart) ||
              (!deleteOnlyWitness &&
               !witness.uniqueBoundaryProjectionProven)))) {
          return std::nullopt;
        }
        expectedA = segment.aEnd;
        expectedB = segment.bEnd;
        distinctRealizerSignatures.insert(segment.producerPathSignature);
        allTokenSegmentsPreserveProtectedStructure &=
            segment.protectedStructurePreservedOutsideSegment;
        if (segment.allowEmptyBEnvelope)
          ++emptyBTokenSegmentCount;
        ++tokenSegmentCount;
        break;

      case MixedOwnerTilingEdgeKind::StateGap:
        if (!segment.zeroTokenStateGap || segment.aStart != expectedA ||
            segment.aEnd != expectedA || segment.bStart != expectedB ||
            segment.bEnd != expectedB) {
          return std::nullopt;
        }
        // Patch 2.2 produces only physically preserved state gaps.  The other
        // disposition values reserve future specialized materialization and
        // repair theorems; accepting them here without their own durable
        // witness would manufacture authority.  `Unknown` is likewise a
        // fail-closed proof omission.
        if (segment.gapDisposition !=
                StructuralGapDisposition::PreservedInPlace ||
            !segment.sourceBytesPreservedUnchanged ||
            !segment.sourceByteRangeKnown ||
            segment.sourceEnd <= segment.sourceBegin) {
          return std::nullopt;
        }
        if (segment.protectedPreprocessingStructure) {
          if (!segment.protectedStructureIdentityRecorded ||
              segment.protectedStructureKind ==
                  StructuralProtectedStructureKind::Unknown)
            return std::nullopt;
          switch (segment.producerIdentityKind) {
          case StructuralProducerIdentityKind::None:
            if (segment.producerItemId ||
                segment.producerConditionalGroupId ||
                segment.producerConditionalArmId)
              return std::nullopt;
            break;
          case StructuralProducerIdentityKind::ConditionalDirective:
            if (!segment.producerConditionalGroupId || segment.producerItemId)
              return std::nullopt;
            break;
          case StructuralProducerIdentityKind::MacroDirective:
          case StructuralProducerIdentityKind::IncludeDirective:
          case StructuralProducerIdentityKind::PragmaDirective:
          case StructuralProducerIdentityKind::LineControlEvent:
            if (!segment.producerItemId ||
                segment.producerConditionalGroupId)
              return std::nullopt;
            break;
          }
        } else if (segment.protectedStructureIdentityRecorded ||
                   segment.protectedStructureKind !=
                       StructuralProtectedStructureKind::Unknown ||
                   segment.producerIdentityKind !=
                       StructuralProducerIdentityKind::None ||
                   segment.producerItemId ||
                   segment.producerConditionalGroupId ||
                   segment.producerConditionalArmId) {
          return std::nullopt;
        }
        ++preservedInPlaceGapCount;
        if (segment.protectedPreprocessingStructure) {
          ++protectedStructureGapCount;
        }
        ++stateGapCount;
        break;

      case MixedOwnerTilingEdgeKind::Unknown:
        return std::nullopt;
      }
    }

    const MixedOwnerTilingSegmentWitness &selectedSegment =
        witness.edges[summary.mixedOwnerTilingSegmentIndex];
    if (selectedSegment.kind != MixedOwnerTilingEdgeKind::TokenSegment ||
        selectedSegment.zeroTokenStateGap)
      return std::nullopt;

    if (deleteOnlyWitness) {
      if (!witness.sharedEmptyBEnvelopeProven ||
          witness.sharedEmptyBBoundary != witness.originalBStart ||
          emptyBTokenSegmentCount != tokenSegmentCount ||
          !witness.preservedStateChainComposed)
        return std::nullopt;
    } else if (witness.sharedEmptyBEnvelopeProven ||
               witness.preservedStateChainComposed) {
      return std::nullopt;
    }

    if (expectedA != witness.originalAEnd ||
        expectedB != witness.originalBEnd ||
        tokenSegmentCount != witness.tokenSegmentCount ||
        stateGapCount != witness.stateGapCount ||
        protectedStructureGapCount != witness.protectedStructureGapCount ||
        preservedInPlaceGapCount != witness.preservedInPlaceGapCount ||
        preservedInPlaceGapCount != stateGapCount ||
        witness.preservedGapsDisjointFromEdits !=
            (preservedInPlaceGapCount == 0 ||
             witness.preservedGapsDisjointFromTokenSegments) ||
        static_cast<uint32_t>(distinctRealizerSignatures.size()) !=
            witness.distinctRealizerCount) {
      return std::nullopt;
    }

    const bool preservesPreprocessingStructure =
        witness.reason ==
            StructuralTilingReason::PreservedPreprocessingStructure ||
        witness.reason == StructuralTilingReason::
                              MixedRealizersAndPreservedStructure;
    if (preservesPreprocessingStructure &&
        !structuralPreservedSourceTopologyIsComplete(witness)) {
      return std::nullopt;
    }

    // Patch 3.1/3.2 is mandatory for every structural replacement that preserves
    // preprocessing structure.  A mixed-realizer path cannot bypass ambiguous
    // B ownership merely because its adjacent token segments have different
    // owners.  Historical mixed-realizer partitions with no protected source
    // seam retain their original theorem and carry no projection facts.
    const bool requiresBoundaryProjectionTheorem =
        structuralReplacementRequiresBoundaryProjection(witness);
    if (deleteOnlyWitness) {
      if (witness.uniqueBoundaryProjectionProven ||
          witness.boundaryProjectionCount != 0 ||
          !witness.boundaryProjections.empty()) {
        return std::nullopt;
      }
    } else if (requiresBoundaryProjectionTheorem) {
      if (!structuralReplacementBoundaryProjectionIsComplete(witness))
        return std::nullopt;
    } else if (witness.uniqueBoundaryProjectionProven ||
               witness.boundaryProjectionCount != 0 ||
               !witness.boundaryProjections.empty()) {
      return std::nullopt;
    }

    switch (witness.reason) {
    case StructuralTilingReason::MixedRealizers:
      if (witness.distinctRealizerCount < 2 ||
          witness.protectedStructureGapCount != 0 ||
          witness.physicalSourceRunCount != 0 ||
          witness.physicalSourceRunsProven ||
          witness.uniqueMinimumFragmentPartition ||
          witness.uniqueBoundaryProjectionProven ||
          witness.boundaryProjectionCount != 0 ||
          !witness.boundaryProjections.empty())
        return std::nullopt;
      if (witness.preservedInPlaceGapCount != 0 &&
          (!witness.preservedGapSourceOrderProven ||
           !witness.preservedGapsDisjointFromTokenSegments ||
           !witness.preservedGapsDisjointFromEdits)) {
        return std::nullopt;
      }
      break;

    case StructuralTilingReason::PreservedPreprocessingStructure:
      if (witness.distinctRealizerCount != 1 ||
          witness.protectedStructureGapCount == 0 ||
          witness.preservedInPlaceGapCount <
              witness.protectedStructureGapCount ||
          witness.physicalSourceRunCount < 2 ||
          witness.physicalSourceRunCount != witness.tokenSegmentCount ||
          !witness.physicalSourceRunsProven ||
          !witness.uniqueMinimumFragmentPartition ||
          !witness.sourceByteCoverComplete ||
          !witness.preservedGapSourceOrderProven ||
          !witness.preservedGapsDisjointFromTokenSegments ||
          !witness.preservedGapsDisjointFromEdits ||
          !allTokenSegmentsPreserveProtectedStructure)
        return std::nullopt;
      break;

    case StructuralTilingReason::MixedRealizersAndPreservedStructure:
      if (witness.distinctRealizerCount < 2 ||
          witness.protectedStructureGapCount == 0 ||
          witness.preservedInPlaceGapCount <
              witness.protectedStructureGapCount ||
          !witness.preservedGapSourceOrderProven ||
          !witness.preservedGapsDisjointFromTokenSegments ||
          !witness.preservedGapsDisjointFromEdits)
        return std::nullopt;
      if (witness.sourceByteCoverComplete &&
          !allTokenSegmentsPreserveProtectedStructure)
        return std::nullopt;
      break;

    case StructuralTilingReason::Unknown:
      return std::nullopt;
    }

    return BuildEmittedProofFromSummary(summary.theoremClass, summary);
  }

  case TheoremProofClass::OwnerRealizationProof: {
    if (!summary.hasOwnerRealizationWitness)
      return std::nullopt;

    const OwnerRealizationWitness &owner = summary.ownerRealizationWitness;
    if (owner.evidence == OwnerRealizationEvidenceKind::TUByteSpan) {
      // Ordinary direct TU evidence is valid only when the exact hunk/span
      // carrier was re-proved. ClosureWidening is intentionally excluded: it
      // describes an obligation to cover an observer, not authority to consume
      // a preprocessing directive or manufacture token envelopes.
      if (!owner.hasTUCarrierWitness ||
          !owner.tuCarrierWitness.IsComplete() ||
          !owner.closure.owner.IsTU() ||
          llvm::any_of(owner.stateWitnesses, isClosureWideningWitness)) {
        return std::nullopt;
      }
    } else if (owner.evidence ==
               OwnerRealizationEvidenceKind::TUSpecializedRealization) {
      // Specialized state/directive planners use a separate TU evidence class.
      // They may not inherit the ordinary direct-span carrier or use closure
      // widening as substitute authority for their planner-specific theorem.
      if (owner.hasTUCarrierWitness || !owner.closure.owner.IsTU() ||
          llvm::any_of(owner.stateWitnesses, isClosureWideningWitness)) {
        return std::nullopt;
      }
    } else if (owner.hasTUCarrierWitness) {
      // The stronger direct-span witness may not be attached to macro or
      // include realization evidence.
      return std::nullopt;
    }

    return BuildEmittedProofFromSummary(summary.theoremClass, summary);
  }

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

std::optional<::clang::refold::EmittedProof>
RefoldProofSummaryBuilder::BuildEmittedProof(
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

std::optional<::clang::refold::TheoremProofClass>
RefoldProofSummaryBuilder::NormalizeAcceptedProof(
    const AcceptedResultCandidate &candidate) const {
  const std::optional<EmittedProof> proof = BuildEmittedProof(candidate);
  if (!proof)
    return std::nullopt;
  return proof->theoremClass;
}

::clang::refold::GlobalSelectionLattice
RefoldProofSummaryBuilder::BuildGlobalSelectionLattice(
    const ProofSummary &summary) const {
  GlobalSelectionLattice lattice;

  // The normalized summary keeps AcceptedProofClass out of lattice authority.
  // The conflict domain is still path provenance because macro, include, TU,
  // and terminal artifacts occupy different owner spaces; the final proof
  // family remains summary.theoremClass and is checked separately by the
  // emitted-proof gate.
  switch (summary.inventory.currentPath) {
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

::clang::refold::CompletenessContract
RefoldProofSummaryBuilder::BuildCompletenessContract(
    const ProofSummary &summary) const {
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
    return contract;
  }

  return contract;
}

::clang::refold::TheoremDomainContract
RefoldProofSummaryBuilder::BuildTheoremDomainContract(
    const ProofSummary &summary) const {
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
    contract.hasExplicitExclusion = summary.completeness.hasExplicitExclusion;
    contract.explicitExclusion = summary.completeness.explicitExclusion;
    contract.declaredTarget = summary.completeness.declaredTarget;
    break;

  case CompletenessCoverageKind::Unknown:
    break;
  }

  return contract;
}

} // namespace refold
} // namespace clang
