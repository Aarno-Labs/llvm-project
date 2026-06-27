//===--- RefoldTerminalProof.cpp -------------------------------*- C++ -*-===//
//
// Terminal fallback proof vocabulary and normalization for clang-refold.
//
// This file builds and formats terminal fallback proof objects only.  It does
// not request fallback, mutate RefoldEngine state, select raw-B emission, or
// inspect edit/materialization state.
//
//===----------------------------------------------------------------------===//

#include "RefoldTerminalProof.h"

#include "llvm/Support/FormatVariadic.h"
#include "llvm/Support/raw_ostream.h"

#include <utility>

using namespace llvm;

namespace clang {
namespace refold {

StringRef toString(TerminalFallbackObligationKind obligation) {
  switch (obligation) {
  case TerminalFallbackObligationKind::Unknown:
    return "Unknown";
  case TerminalFallbackObligationKind::OwnerClosedCover:
    return "OwnerClosedCover";
  case TerminalFallbackObligationKind::DeterministicMixedOwnerTiling:
    return "DeterministicMixedOwnerTiling";
  case TerminalFallbackObligationKind::StateTransitionClosure:
    return "StateTransitionClosure";
  case TerminalFallbackObligationKind::PragmaBoundaryKnown:
    return "PragmaBoundaryKnown";
  case TerminalFallbackObligationKind::LineControlStateProducerProven:
    return "LineControlStateProducerProven";
  case TerminalFallbackObligationKind::CounterStateStabilizable:
    return "CounterStateStabilizable";
  case TerminalFallbackObligationKind::MacroStateStabilizable:
    return "MacroStateStabilizable";
  case TerminalFallbackObligationKind::IncludeGuardStateStabilizable:
    return "IncludeGuardStateStabilizable";
  case TerminalFallbackObligationKind::ConditionalStateStabilizable:
    return "ConditionalStateStabilizable";
  case TerminalFallbackObligationKind::ReverseSolvedDirectiveForbidden:
    return "ReverseSolvedDirectiveForbidden";
  case TerminalFallbackObligationKind::InvocationPreservationWellFormed:
    return "InvocationPreservationWellFormed";
  case TerminalFallbackObligationKind::ProducerFactsAvailable:
    return "ProducerFactsAvailable";
  case TerminalFallbackObligationKind::FinalValidationSucceeded:
    return "FinalValidationSucceeded";
  case TerminalFallbackObligationKind::IncludeRealizationBEnvelopeMapped:
    return "IncludeRealizationBEnvelopeMapped";
  case TerminalFallbackObligationKind::EmissionArtifactDischarged:
    return "EmissionArtifactDischarged";
  case TerminalFallbackObligationKind::EmissionEditSetComposable:
    return "EmissionEditSetComposable";
  case TerminalFallbackObligationKind::TheoremAuditInvariantSatisfied:
    return "TheoremAuditInvariantSatisfied";
  }
  return "Unknown";
}

StringRef toString(TerminalFallbackFailureReason reason) {
  switch (reason) {
  case TerminalFallbackFailureReason::Unknown:
    return "Unknown";
  case TerminalFallbackFailureReason::NoOwnerClosedCover:
    return "NoOwnerClosedCover";
  case TerminalFallbackFailureReason::NoDeterministicMixedOwnerTiling:
    return "NoDeterministicMixedOwnerTiling";
  case TerminalFallbackFailureReason::AmbiguousMixedOwnerTiling:
    return "AmbiguousMixedOwnerTiling";
  case TerminalFallbackFailureReason::StateTransitionConsumedAndObserved:
    return "StateTransitionConsumedAndObserved";
  case TerminalFallbackFailureReason::UnknownPragmaCrossesBoundary:
    return "UnknownPragmaCrossesBoundary";
  case TerminalFallbackFailureReason::LineControlStateNotProducerProven:
    return "LineControlStateNotProducerProven";
  case TerminalFallbackFailureReason::CounterStateNotStabilizable:
    return "CounterStateNotStabilizable";
  case TerminalFallbackFailureReason::MacroStateNotStabilizable:
    return "MacroStateNotStabilizable";
  case TerminalFallbackFailureReason::IncludeGuardStateNotStabilizable:
    return "IncludeGuardStateNotStabilizable";
  case TerminalFallbackFailureReason::ConditionalStateNotStabilizable:
    return "ConditionalStateNotStabilizable";
  case TerminalFallbackFailureReason::ReverseSolvedDirectiveRequired:
    return "ReverseSolvedDirectiveRequired";
  case TerminalFallbackFailureReason::MalformedInvocationPreservation:
    return "MalformedInvocationPreservation";
  case TerminalFallbackFailureReason::MissingProducerFacts:
    return "MissingProducerFacts";
  case TerminalFallbackFailureReason::NoCanonicalSuffixOrder:
    return "NoCanonicalSuffixOrder";
  case TerminalFallbackFailureReason::ValidationFailure:
    return "ValidationFailure";
  case TerminalFallbackFailureReason::NoTUAnchorForUnresolvedOwner:
    return "NoTUAnchorForUnresolvedOwner";
  case TerminalFallbackFailureReason::UnmappableIncludeBEnvelope:
    return "UnmappableIncludeBEnvelope";
  case TerminalFallbackFailureReason::UndischargedEmissionArtifact:
    return "UndischargedEmissionArtifact";
  case TerminalFallbackFailureReason::UncomposableEmissionEditSet:
    return "UncomposableEmissionEditSet";
  case TerminalFallbackFailureReason::TheoremAuditInvariantViolation:
    return "TheoremAuditInvariantViolation";
  case TerminalFallbackFailureReason::UnclassifiedTerminalFallback:
    return "UnclassifiedTerminalFallback";
  }
  return "Unknown";
}

StringRef toString(TheoremFallbackFailureKind kind) {
  switch (kind) {
  case TheoremFallbackFailureKind::Unknown:
    return "Unknown";
  case TheoremFallbackFailureKind::NoOwnerClosedCover:
    return "NoOwnerClosedCover";
  case TheoremFallbackFailureKind::NoDeterministicMixedOwnerTiling:
    return "NoDeterministicMixedOwnerTiling";
  case TheoremFallbackFailureKind::AmbiguousMixedOwnerTiling:
    return "AmbiguousMixedOwnerTiling";
  case TheoremFallbackFailureKind::StateTransitionConsumedAndObserved:
    return "StateTransitionConsumedAndObserved";
  case TheoremFallbackFailureKind::UnknownPragmaCrossesBoundary:
    return "UnknownPragmaCrossesBoundary";
  case TheoremFallbackFailureKind::LineControlStateNotProducerProven:
    return "LineControlStateNotProducerProven";
  case TheoremFallbackFailureKind::CounterStateNotStabilizable:
    return "CounterStateNotStabilizable";
  case TheoremFallbackFailureKind::MacroStateNotStabilizable:
    return "MacroStateNotStabilizable";
  case TheoremFallbackFailureKind::IncludeGuardStateNotStabilizable:
    return "IncludeGuardStateNotStabilizable";
  case TheoremFallbackFailureKind::ConditionalStateNotStabilizable:
    return "ConditionalStateNotStabilizable";
  case TheoremFallbackFailureKind::ReverseSolvedDirectiveRequired:
    return "ReverseSolvedDirectiveRequired";
  case TheoremFallbackFailureKind::MalformedInvocationPreservation:
    return "MalformedInvocationPreservation";
  case TheoremFallbackFailureKind::MissingProducerFacts:
    return "MissingProducerFacts";
  case TheoremFallbackFailureKind::ValidationFailure:
    return "ValidationFailure";
  }
  return "Unknown";
}

std::optional<TheoremFallbackFailureKind>
NormalizeTerminalFallbackFailureReason(TerminalFallbackFailureReason reason) {
  switch (reason) {
  case TerminalFallbackFailureReason::NoOwnerClosedCover:
  case TerminalFallbackFailureReason::NoTUAnchorForUnresolvedOwner:
    return TheoremFallbackFailureKind::NoOwnerClosedCover;
  case TerminalFallbackFailureReason::NoDeterministicMixedOwnerTiling:
    return TheoremFallbackFailureKind::NoDeterministicMixedOwnerTiling;
  case TerminalFallbackFailureReason::AmbiguousMixedOwnerTiling:
    return TheoremFallbackFailureKind::AmbiguousMixedOwnerTiling;
  case TerminalFallbackFailureReason::StateTransitionConsumedAndObserved:
    return TheoremFallbackFailureKind::StateTransitionConsumedAndObserved;
  case TerminalFallbackFailureReason::UnknownPragmaCrossesBoundary:
    return TheoremFallbackFailureKind::UnknownPragmaCrossesBoundary;
  case TerminalFallbackFailureReason::LineControlStateNotProducerProven:
    return TheoremFallbackFailureKind::LineControlStateNotProducerProven;
  case TerminalFallbackFailureReason::CounterStateNotStabilizable:
    return TheoremFallbackFailureKind::CounterStateNotStabilizable;
  case TerminalFallbackFailureReason::MacroStateNotStabilizable:
    return TheoremFallbackFailureKind::MacroStateNotStabilizable;
  case TerminalFallbackFailureReason::IncludeGuardStateNotStabilizable:
    return TheoremFallbackFailureKind::IncludeGuardStateNotStabilizable;
  case TerminalFallbackFailureReason::ConditionalStateNotStabilizable:
    return TheoremFallbackFailureKind::ConditionalStateNotStabilizable;
  case TerminalFallbackFailureReason::ReverseSolvedDirectiveRequired:
    return TheoremFallbackFailureKind::ReverseSolvedDirectiveRequired;
  case TerminalFallbackFailureReason::MalformedInvocationPreservation:
    return TheoremFallbackFailureKind::MalformedInvocationPreservation;
  case TerminalFallbackFailureReason::MissingProducerFacts:
  case TerminalFallbackFailureReason::NoCanonicalSuffixOrder:
  case TerminalFallbackFailureReason::UnmappableIncludeBEnvelope:
  case TerminalFallbackFailureReason::UndischargedEmissionArtifact:
  case TerminalFallbackFailureReason::TheoremAuditInvariantViolation:
    return TheoremFallbackFailureKind::MissingProducerFacts;
  case TerminalFallbackFailureReason::UncomposableEmissionEditSet:
    return TheoremFallbackFailureKind::NoDeterministicMixedOwnerTiling;
  case TerminalFallbackFailureReason::ValidationFailure:
    return TheoremFallbackFailureKind::ValidationFailure;
  case TerminalFallbackFailureReason::Unknown:
  case TerminalFallbackFailureReason::UnclassifiedTerminalFallback:
    return std::nullopt;
  }
  return std::nullopt;
}

TerminalFallbackFailureContext
TerminalFallbackFailureContext::ForStateComponent(StringRef name) {
  TerminalFallbackFailureContext context;
  context.stateComponent = name.str();
  return context;
}

TerminalFallbackFailureContext
TerminalFallbackFailureContext::ForHunkTokenEnvelope(
    uint64_t hunkIndex, uint64_t aBegin, uint64_t aEnd, uint64_t bBegin,
    uint64_t bEnd) {
  TerminalFallbackFailureContext context;
  context.hunk = hunkIndex;
  context.aTokenBegin = aBegin;
  context.aTokenEnd = aEnd;
  context.bTokenBegin = bBegin;
  context.bTokenEnd = bEnd;
  return context;
}

bool TerminalFallbackFailureContext::Empty() const {
  return !owner && !hunk && !stateComponent && !sourcePath && !sourceBegin &&
         !sourceEnd && !aTokenBegin && !aTokenEnd && !bTokenBegin &&
         !bTokenEnd;
}

std::string TerminalFallbackFailureContext::ToString() const {
  if (Empty())
    return "<empty>";

  std::string text;
  raw_string_ostream os(text);
  bool first = true;
  auto add = [&](StringRef key, const auto &value) {
    if (!first)
      os << ",";
    first = false;
    os << key << "=" << value;
  };

  if (owner)
    add("owner", *owner);
  if (hunk)
    add("hunk", *hunk);
  if (stateComponent)
    add("state", *stateComponent);
  if (sourcePath)
    add("source", *sourcePath);
  if (sourceBegin || sourceEnd)
    add("sourceBytes", formatv("[{0},{1})", sourceBegin.value_or(0),
                                sourceEnd.value_or(0)));
  if (aTokenBegin || aTokenEnd)
    add("aTokens", formatv("[{0},{1})", aTokenBegin.value_or(0),
                            aTokenEnd.value_or(0)));
  if (bTokenBegin || bTokenEnd)
    add("bTokens", formatv("[{0},{1})", bTokenBegin.value_or(0),
                            bTokenEnd.value_or(0)));
  os.flush();
  return text;
}

std::string TerminalFallbackProofFailure::ToString() const {
  return formatv("obligation={0} theoremFailure={1} reason={2} context={3}",
                 obligation, theoremFailure, reason, context)
      .str();
}

bool IsClassifiedTerminalFallbackProofFailure(
    const TerminalFallbackProofFailure &failure) {
  return failure.obligation != TerminalFallbackObligationKind::Unknown &&
         failure.theoremFailure != TheoremFallbackFailureKind::Unknown &&
         failure.reason != TerminalFallbackFailureReason::Unknown &&
         failure.reason !=
             TerminalFallbackFailureReason::UnclassifiedTerminalFallback;
}

TerminalFallbackProofFailure MakeTerminalFallbackProofFailure(
    TerminalFallbackObligationKind obligation,
    TerminalFallbackFailureReason reason) {
  TerminalFallbackProofFailure failure;
  failure.obligation = obligation;
  failure.reason = reason;
  failure.theoremFailure =
      NormalizeTerminalFallbackFailureReason(reason).value_or(
          TheoremFallbackFailureKind::Unknown);
  return failure;
}

TerminalFallbackProofFailure MakeTerminalFallbackProofFailure(
    TerminalFallbackObligationKind obligation,
    TerminalFallbackFailureReason reason,
    TerminalFallbackFailureContext context) {
  TerminalFallbackProofFailure failure =
      MakeTerminalFallbackProofFailure(obligation, reason);
  failure.context = std::move(context);
  return failure;
}

const TerminalFallbackProofFailure *TerminalFallbackWitness::PrimaryFailure()
    const {
  return proofFailures.empty() ? nullptr : &proofFailures.front();
}

std::string TerminalFallbackWitness::ToString() const {
  const TerminalFallbackProofFailure *primary = PrimaryFailure();
  return formatv("{0} failureCount={1} secondaryFailureCount={2}",
                 primary ? primary->ToString()
                         : std::string("<missing-terminal-failure>"),
                 proofFailures.size(),
                 proofFailures.size() > 1 ? proofFailures.size() - 1 : 0)
      .str();
}

std::string TerminalFallbackRequest::ToString() const {
  return formatv(
             "terminal fallback requested: action=raw-b-emission "
             "obligation={0} reason={1} context={2} theoremFailure={3} "
             "stage={4} detail={5}",
             failure.obligation, failure.reason, failure.context,
             failure.theoremFailure,
             stage.empty() ? StringRef("<unspecified>") : StringRef(stage),
             detail.empty() ? StringRef("<none>") : StringRef(detail))
      .str();
}

TerminalFallbackRequest MakeTerminalFallbackRequest(
    TerminalFallbackProofFailure failure, StringRef stage, StringRef detail) {
  TerminalFallbackRequest request;
  request.failure = std::move(failure);
  request.stage = stage.str();
  request.detail = detail.str();
  return request;
}

} // namespace refold
} // namespace clang
