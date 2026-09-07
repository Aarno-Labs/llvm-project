//===--- RefoldWitnessClassifier.cpp ----------------------------*- C++ -*-===//
//
// Witness classification implementation.
//
//===----------------------------------------------------------------------===//

#include "proof/RefoldWitnessClassifier.h"

#include "proof/RefoldTheoremAudit.h"

using namespace llvm;

namespace clang {
namespace refold {

::clang::refold::WitnessProofFamily
witnessFamilyForAcceptedPath(AcceptedPathKind path) {
  switch (path) {
  case AcceptedPathKind::MacroArgsOnlyStandard:
  case AcceptedPathKind::MacroArgsOnlyPairedPureInsertion:
  case AcceptedPathKind::MacroDirectCalleeSubstitution:
    return WitnessProofFamily::MacroActualRepair;
  case AcceptedPathKind::MacroArgsOnlyPasteSingle:
  case AcceptedPathKind::MacroArgsOnlyPasteMulti:
  case AcceptedPathKind::MacroArgsOnlyPurePasteOnly:
  case AcceptedPathKind::MacroPasteDerivedCalleeSelector:
    return WitnessProofFamily::TokenPaste;
  case AcceptedPathKind::MacroRecursiveTupleGeneratedCalleeReplay:
    return WitnessProofFamily::GeneratedCalleeReplay;
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

::clang::refold::WitnessProducerKind
witnessProducerKindForAcceptedPath(AcceptedPathKind path) {
  switch (path) {
  case AcceptedPathKind::MacroArgsOnlyStandard:
  case AcceptedPathKind::MacroArgsOnlyPairedPureInsertion:
  case AcceptedPathKind::MacroDirectCalleeSubstitution:
    return WitnessProducerKind::Forward;
  case AcceptedPathKind::MacroArgsOnlyPasteSingle:
  case AcceptedPathKind::MacroArgsOnlyPasteMulti:
  case AcceptedPathKind::MacroArgsOnlyPurePasteOnly:
  case AcceptedPathKind::MacroPasteDerivedCalleeSelector:
    return WitnessProducerKind::PasteResult;
  case AcceptedPathKind::MacroRecursiveTupleGeneratedCalleeReplay:
    return WitnessProducerKind::GeneratedCallee;
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

::clang::refold::WitnessBoundaryClass witnessBoundaryClassForAcceptedCandidate(
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

::clang::refold::WitnessFallbackClass
classifyTerminalFallbackFailure(const TerminalFallbackProofFailure &failure) {
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

::clang::refold::WitnessFallbackClass
classifyResolverFallbackReason(llvm::StringRef reason,
                               const WitnessCompositionDecision &composition) {
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
      reason == "joined-certificates-authorize-one-repair")
    return FallbackClass::Unknown;

  return FallbackClass::Unknown;
}

::clang::refold::WitnessStrictDomainObligation
strictDomainObligationForFallbackClass(WitnessFallbackClass fallbackClass) {
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

::clang::refold::WitnessStrictDomainDecision
classifyStrictDomainForResolver(const WitnessResolverDecision &decision) {
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
    result.obligation = strictDomainObligationForFallbackClass(
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
        strictDomainObligationForFallbackClass(decision.fallbackClass);
    break;
  case FallbackClass::CompositionFailure:
    if (decision.failureReason ==
            "multiple-non-equivalent-composition-classes" ||
        decision.failureReason == "composition-incompatible-terminal-tuple") {
      result.domainClass = DomainClass::AmbiguousOutOfDomain;
    } else {
      result.domainClass = DomainClass::PotentiallyInDomainMissingProof;
    }
    result.obligation =
        strictDomainObligationForFallbackClass(decision.fallbackClass);
    break;
  case FallbackClass::MultipleNonEquivalentWitnessClasses:
    result.domainClass = DomainClass::AmbiguousOutOfDomain;
    result.obligation =
        strictDomainObligationForFallbackClass(decision.fallbackClass);
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
        strictDomainObligationForFallbackClass(decision.fallbackClass);
    break;
  }

  if (result.reason.empty())
    result.reason = toString(result.fallbackClass).str();
  return result;
}

::clang::refold::WitnessStrictDomainDecision
classifyStrictDomainForTerminalFallback(
    const TerminalFallbackProofFailure &failure) {
  using DomainClass = WitnessStrictDomainClass;
  using FailureReason = TerminalFallbackFailureReason;
  using FallbackClass = WitnessFallbackClass;

  WitnessStrictDomainDecision result;
  result.fallbackClass = classifyTerminalFallbackFailure(failure);
  result.obligation =
      strictDomainObligationForFallbackClass(result.fallbackClass);
  result.reason = toString(failure.reason).str();

  switch (failure.reason) {
  case FailureReason::NoOwnerClosedCover:
  case FailureReason::NoTUAnchorForUnresolvedOwner:
  case FailureReason::UnknownPragmaCrossesBoundary:
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

} // namespace refold
} // namespace clang
