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
#include "proof/RefoldTheoremAudit.h"

#include "llvm/ADT/ArrayRef.h"

#include <cstdint>
#include <optional>
#include <utility>

using namespace llvm;

namespace clang {
namespace refold {

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
        witness.originalBEnd > deps_.bToks.size()) {
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
          segment.bEnd > deps_.bToks.size()) {
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
