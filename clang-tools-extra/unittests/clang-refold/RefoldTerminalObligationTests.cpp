//===-- RefoldTerminalObligationTests.cpp ----------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Coverage for the terminal-fallback obligations that no input can reach.
//
// Most of the terminal vocabulary is pinned by lit: an input shape drives the
// engine to raw B and the request record is asserted from the log.  Several
// obligations cannot be covered that way, and that is a property of the
// obligations rather than of the shapes tried.  They are produced only by
// internal-invariant assertions -- a state component no planner passes to the
// gateway, a suffix-observed transition with no typed witness, an emitted edit
// with no discharged carrier -- each of which also records a theorem-audit
// violation.  Reaching one from a source file would mean the engine already
// had a defect.
//
// What can still be pinned is the projection those assertions use: which
// obligation each state component names, and that every local failure reason
// normalizes to the frozen theorem vocabulary.  That is what these tests own.
// They are the reason the engine is built as a library.
//
//===----------------------------------------------------------------------===//

#include "proof/RefoldOwnerStateProof.h"
#include "proof/RefoldOwnerStateTypes.h"
#include "proof/RefoldProofVocabulary.h"

#include "gtest/gtest.h"

using namespace clang::refold;

namespace {

/// Every `OwnerStateComponent`, so a new component cannot be added without
/// deciding which obligation it projects to.
constexpr OwnerStateComponent AllOwnerStateComponents[] = {
    OwnerStateComponent::MacroState,      OwnerStateComponent::DefinedOperator,
    OwnerStateComponent::ConditionalState, OwnerStateComponent::LineNumber,
    OwnerStateComponent::FileState,       OwnerStateComponent::FileName,
    OwnerStateComponent::Counter,         OwnerStateComponent::PragmaState,
    OwnerStateComponent::IncludeGuardState, OwnerStateComponent::IncludeState,
    OwnerStateComponent::UnmodeledState,  OwnerStateComponent::Unknown,
};

/// Every `TerminalFallbackFailureReason`, so a new reason cannot be added
/// without deciding how it normalizes.
constexpr TerminalFallbackFailureReason AllTerminalFailureReasons[] = {
    TerminalFallbackFailureReason::Unknown,
    TerminalFallbackFailureReason::NoOwnerClosedCover,
    TerminalFallbackFailureReason::NoDeterministicMixedOwnerTiling,
    TerminalFallbackFailureReason::AmbiguousMixedOwnerTiling,
    TerminalFallbackFailureReason::StateTransitionConsumedAndObserved,
    TerminalFallbackFailureReason::UnknownPragmaCrossesBoundary,
    TerminalFallbackFailureReason::LineControlStateNotProducerProven,
    TerminalFallbackFailureReason::CounterStateNotStabilizable,
    TerminalFallbackFailureReason::MacroStateNotStabilizable,
    TerminalFallbackFailureReason::IncludeGuardStateNotStabilizable,
    TerminalFallbackFailureReason::ConditionalStateNotStabilizable,
    TerminalFallbackFailureReason::MalformedInvocationPreservation,
    TerminalFallbackFailureReason::MissingProducerFacts,
    TerminalFallbackFailureReason::NoCanonicalSuffixOrder,
    TerminalFallbackFailureReason::ValidationFailure,
    TerminalFallbackFailureReason::NoTUAnchorForUnresolvedOwner,
    TerminalFallbackFailureReason::UnmappableIncludeBEnvelope,
    TerminalFallbackFailureReason::UndischargedEmissionArtifact,
    TerminalFallbackFailureReason::UncomposableEmissionEditSet,
    TerminalFallbackFailureReason::TheoremAuditInvariantViolation,
    TerminalFallbackFailureReason::UnclassifiedTerminalFallback,
};

struct ComponentProjection {
  OwnerStateComponent component;
  TerminalFallbackObligationKind obligation;
  TerminalFallbackFailureReason reason;
};

/// The complete component-to-obligation projection.
///
/// Four of these rows are the only coverage those obligations have anywhere:
/// `ConditionalState` and `PragmaState` are never passed to the state gateway
/// by any planner, and `Counter`/`IncludeState` reach it only with a typed
/// witness that the gateway accepts.  Their failure rows are therefore
/// reachable only through the gateway's defect exits.
constexpr ComponentProjection ExpectedProjections[] = {
    {OwnerStateComponent::MacroState,
     TerminalFallbackObligationKind::MacroStateStabilizable,
     TerminalFallbackFailureReason::MacroStateNotStabilizable},
    {OwnerStateComponent::DefinedOperator,
     TerminalFallbackObligationKind::MacroStateStabilizable,
     TerminalFallbackFailureReason::MacroStateNotStabilizable},
    {OwnerStateComponent::ConditionalState,
     TerminalFallbackObligationKind::ConditionalStateStabilizable,
     TerminalFallbackFailureReason::ConditionalStateNotStabilizable},
    {OwnerStateComponent::LineNumber,
     TerminalFallbackObligationKind::LineControlStateProducerProven,
     TerminalFallbackFailureReason::LineControlStateNotProducerProven},
    {OwnerStateComponent::FileState,
     TerminalFallbackObligationKind::LineControlStateProducerProven,
     TerminalFallbackFailureReason::LineControlStateNotProducerProven},
    {OwnerStateComponent::FileName,
     TerminalFallbackObligationKind::LineControlStateProducerProven,
     TerminalFallbackFailureReason::LineControlStateNotProducerProven},
    {OwnerStateComponent::Counter,
     TerminalFallbackObligationKind::CounterStateStabilizable,
     TerminalFallbackFailureReason::CounterStateNotStabilizable},
    {OwnerStateComponent::PragmaState,
     TerminalFallbackObligationKind::PragmaBoundaryKnown,
     TerminalFallbackFailureReason::UnknownPragmaCrossesBoundary},
    {OwnerStateComponent::IncludeGuardState,
     TerminalFallbackObligationKind::IncludeGuardStateStabilizable,
     TerminalFallbackFailureReason::IncludeGuardStateNotStabilizable},
    {OwnerStateComponent::IncludeState,
     TerminalFallbackObligationKind::StateTransitionClosure,
     TerminalFallbackFailureReason::StateTransitionConsumedAndObserved},
    {OwnerStateComponent::UnmodeledState,
     TerminalFallbackObligationKind::ProducerFactsAvailable,
     TerminalFallbackFailureReason::MissingProducerFacts},
    {OwnerStateComponent::Unknown,
     TerminalFallbackObligationKind::StateTransitionClosure,
     TerminalFallbackFailureReason::StateTransitionConsumedAndObserved},
};

TEST(RefoldTerminalObligation, ComponentProjectionIsExact) {
  ASSERT_EQ(std::size(ExpectedProjections), std::size(AllOwnerStateComponents));
  for (const ComponentProjection &expected : ExpectedProjections) {
    const TerminalFallbackProofFailure failure =
        RefoldOwnerStateProof::SuffixStabilityTerminalFailureForComponent(
            expected.component);
    EXPECT_EQ(failure.obligation, expected.obligation)
        << "component " << toString(expected.component).str();
    EXPECT_EQ(failure.reason, expected.reason)
        << "component " << toString(expected.component).str();
  }
}

TEST(RefoldTerminalObligation, ComponentProjectionNamesTheComponent) {
  // The projection is the only place a rejection records which state made the
  // suffix unstable, so every row must carry its component into the context.
  for (OwnerStateComponent component : AllOwnerStateComponents) {
    const TerminalFallbackProofFailure failure =
        RefoldOwnerStateProof::SuffixStabilityTerminalFailureForComponent(
            component);
    EXPECT_EQ(failure.context.stateComponent, toString(component).str())
        << "component " << toString(component).str();
  }
}

TEST(RefoldTerminalObligation, EveryFailureReasonNormalizes) {
  // The theorem vocabulary is the frozen answer to "which strict-domain
  // obligation failed".  A reason with no normalization would reach the seam
  // as an unclassified terminal fallback.
  for (TerminalFallbackFailureReason reason : AllTerminalFailureReasons) {
    const std::optional<TheoremFallbackFailureKind> kind =
        NormalizeTerminalFallbackFailureReason(reason);
    if (reason == TerminalFallbackFailureReason::Unknown ||
        reason == TerminalFallbackFailureReason::UnclassifiedTerminalFallback) {
      // These two deliberately have no theorem kind: they name the absence of
      // a classification rather than a classified domain wall.
      EXPECT_FALSE(kind.has_value()) << toString(reason).str();
      continue;
    }
    EXPECT_TRUE(kind.has_value()) << toString(reason).str();
  }
}

TEST(RefoldTerminalObligation, EveryFailureReasonHasADistinctSpelling) {
  // Diagnostics are the only oracle for which obligation fired, so two reasons
  // sharing a spelling would make a request record ambiguous.
  for (TerminalFallbackFailureReason lhs : AllTerminalFailureReasons) {
    EXPECT_FALSE(toString(lhs).empty());
    for (TerminalFallbackFailureReason rhs : AllTerminalFailureReasons) {
      if (lhs == rhs)
        continue;
      EXPECT_NE(toString(lhs), toString(rhs));
    }
  }
}

} // namespace
