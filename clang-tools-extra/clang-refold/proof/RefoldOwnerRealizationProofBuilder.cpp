//===--- RefoldOwnerRealizationProofBuilder.cpp -----------------*- C++ -*-===//
//
// Owner-realization proof construction + witness attachment — implementation.
//
//===----------------------------------------------------------------------===//

#include "proof/RefoldOwnerRealizationProofBuilder.h"

#include "core/RefoldLog.h"
#include "core/RefoldModel.h"
#include "edit/RefoldTUEditPlanner.h"
#include "proof/RefoldAcceptedResultPredicates.h"
#include "proof/RefoldOwnerStateProof.h"
#include "proof/RefoldStructuralHunkTilingProof.h"
#include "proof/RefoldWitnessTrace.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/FormatVariadic.h"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <utility>
#include <vector>

using namespace llvm;

namespace clang {
namespace refold {
namespace {

/// Return whether one typed state witness is the legacy closure-widening form.
bool isClosureWideningWitness(const SuffixStabilityWitness &witness) {
  return witness.kind == SuffixStabilityWitnessKind::ClosureWidening;
}

/// Build the canonical fail-closed result for an invalid direct TU carrier.
///
/// This helper takes the closure by value so callers may safely reject both
/// before and after the shared realization gate moves its local closure into an
/// `OwnerRealizationResult`.  It never manufactures state or token authority.
OwnerRealizationResult buildRejectedDirectTUCarrier(
    OwnerClosure closure, StringRef detail,
    TerminalFallbackObligationKind obligation,
    TerminalFallbackFailureReason reason) {
  OwnerRealizationResult result;
  result.witness.evidence = OwnerRealizationEvidenceKind::TUByteSpan;
  result.witness.closure = std::move(closure);
  result.witness.detail = detail.str();
  result.detail = detail.str();

  TerminalFallbackFailureContext context;
  context.owner = toString(result.witness.closure.owner.kind).str();
  context.sourcePath = result.witness.closure.source.path;
  context.sourceBegin = result.witness.closure.source.begin;
  context.sourceEnd = result.witness.closure.source.end;
  context.aTokenBegin = result.witness.closure.aTokens.begin;
  context.aTokenEnd = result.witness.closure.aTokens.end;
  context.bTokenBegin = result.witness.closure.bTokens.begin;
  context.bTokenEnd = result.witness.closure.bTokens.end;
  result.failure = MakeTerminalFallbackProofFailure(
      obligation, reason, std::move(context));
  REFOLD_LOG_TRACE(
      "proof/owner-realization",
      "reject direct TU owner carrier: {0} detail='{1}'", result.failure,
      result.detail);
  return result;
}

} // namespace

RefoldOwnerRealizationProofBuilder::RefoldOwnerRealizationProofBuilder(
    Dependencies deps)
    : deps_(std::move(deps)) {}

void RefoldOwnerRealizationProofBuilder::AttachLineControlObserverWitness(
    AcceptedResultCandidate &candidate) const {
  candidate.hasLineControlObserverWitness = false;
  candidate.lineControlObserverWitness = LineControlObserverWitness{};

  LineControlObserverWitness witness;
  const ProofSummary &summary = candidate.proofSummary;

  auto appendSig = [](std::string &dst, llvm::StringRef part) {
    if (!dst.empty())
      dst += ";";
    dst += part;
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
              llvm::formatv(
                  "{0}:{1}", bucket,
                  deps_.ownerStateProof.FormatLineControlEventForWitness(event))
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
    appendSig(
        witness.observerSignature,
        llvm::formatv(
            "{0}:{1}", bucket,
            deps_.ownerStateProof.FormatBuiltinLocationObservationForWitness(
                obs))
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
                  llvm::formatv("{0}:missing-line-control-fact:{1}", bucket,
                                fact.detail)
                      .str());
      }
    }
  };

  auto collectDelta = [&](const OwnerStateDelta &delta, llvm::StringRef role) {
    collectFacts(delta.entry, llvm::formatv("{0}.entry", role).str());
    collectFacts(delta.observes, llvm::formatv("{0}.observes", role).str());
    collectFacts(delta.mutates, llvm::formatv("{0}.mutates", role).str());
    collectFacts(delta.exit, llvm::formatv("{0}.exit", role).str());
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
    for (const MixedOwnerTilingSegmentWitness &segment : tiling.edges) {
      collectDelta(segment.canonicalStateTransition.before,
                   "mixed.segment.before");
      collectDelta(segment.canonicalStateTransition.after,
                   "mixed.segment.after");
      for (const SuffixStabilityWitness &suffix :
           segment.canonicalStateTransition.suffixWitnesses) {
        const OwnerStateComponent component =
            deps_.ownerStateProof.ComponentNamedBySuffixStabilityWitness(
                suffix);
        if (!deps_.ownerStateProof.IsLineControlStateComponent(component))
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
        deps_.ownerStateProof.ComponentNamedBySuffixStabilityWitness(suffix);
    if (deps_.ownerStateProof.IsLineControlStateComponent(component)) {
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
    const ZeroTokenBoundaryWitness &zeroToken =
        candidate.zeroTokenBoundaryWitness;
    witness.physicalLayoutKnown = true;
    witness.physicalLayoutStable = zeroToken.layoutStable;
    if (zeroToken.fromIncludeBoundary)
      witness.includeReturnResyncCount += 1;
    if (zeroToken.fromDirectiveLayoutGap)
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

void RefoldOwnerRealizationProofBuilder::AttachStandardWitnesses(
    AcceptedResultCandidate &candidate) const {
  AttachLineControlObserverWitness(candidate);
  AttachCounterStateWitness(candidate);
  RefreshAcceptedCandidateEmissionPathInventory(candidate);
}

void RefoldOwnerRealizationProofBuilder::AttachCounterStateWitness(
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
    dst += part;
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
                              RefoldWitnessTrace::FormatWitnessTraceHash(
                                  *event.expectedBValue))
                    .str());
    } else {
      witness.hasMissingExpectedBValues = true;
      ++witness.missingExpectedBValueCount;
    }
    appendSig(
        witness.consumptionSignature,
        llvm::formatv("{0}:{1}", bucket,
                      deps_.ownerStateProof.FormatCounterEventForWitness(event))
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
                  llvm::formatv(
                      "{0}:missing-counter-facts:{1}", bucket,
                      RefoldWitnessTrace::FormatWitnessTraceHash(fact.detail))
                      .str());
      }
    }
  };

  auto collectDelta = [&](const OwnerStateDelta &delta,
                          llvm::StringRef bucket) {
    collectFacts(delta.entry, llvm::formatv("{0}.entry", bucket).str(),
                 /*observation=*/true, /*mutation=*/false);
    collectFacts(delta.observes, llvm::formatv("{0}.observes", bucket).str(),
                 /*observation=*/true, /*mutation=*/false);
    collectFacts(delta.mutates, llvm::formatv("{0}.mutates", bucket).str(),
                 /*observation=*/false, /*mutation=*/true);
    collectFacts(delta.exit, llvm::formatv("{0}.exit", bucket).str(),
                 /*observation=*/false, /*mutation=*/true);
  };

  auto recordSuffixWitness = [&](const SuffixStabilityWitness &suffix,
                                 llvm::StringRef bucket) {
    if (!deps_.ownerStateProof.SuffixStabilityWitnessNamesComponent(
            suffix, OwnerStateComponent::Counter))
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
              llvm::formatv(
                  "{0}:suffix={1}:component={2}", bucket, suffix.kind,
                  deps_.ownerStateProof.ComponentNamedBySuffixStabilityWitness(
                      suffix))
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
    for (const MixedOwnerTilingSegmentWitness &segment : tiling.edges) {
      collectDelta(
          segment.canonicalStateTransition.before,
          llvm::formatv("mixed.segment{0}.before", segment.segmentIndex).str());
      collectDelta(
          segment.canonicalStateTransition.after,
          llvm::formatv("mixed.segment{0}.after", segment.segmentIndex).str());
      for (const SuffixStabilityWitness &suffix :
           segment.canonicalStateTransition.suffixWitnesses)
        recordSuffixWitness(suffix, llvm::formatv("mixed.segment{0}.suffix",
                                                  segment.segmentIndex)
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

  if (candidate.hasZeroTokenBoundaryWitness &&
      candidate.zeroTokenBoundaryWitness.counterStable)
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

void RefoldOwnerRealizationProofBuilder::
    AttachMixedOwnerTilingWitnessForTokenEnvelope(ProofSummary &summary,
                                                  uint64_t aStart,
                                                  uint64_t aEnd,
                                                  uint64_t bStart,
                                                  uint64_t bEnd) const {
  // Keeps mixed-owner splitting as a normalization step, but it no longer lets
  // path-local binding order decide which proof wins.  Every matching durable
  // segment is converted into a candidate ProofSummary and the shared lattice
  // chooses both whether the mixed-owner overlay beats the owner-specific
  // summary and which matching tiling witness is strongest.
  std::optional<ProofSummary> selectedSummary;

  for (const MixedOwnerTilingSegmentBinding &binding :
       deps_.mixedOwnerTilingSegmentBindings) {
    if (binding.aStart != aStart || binding.aEnd != aEnd ||
        binding.bStart != bStart || binding.bEnd != bEnd)
      continue;
    if (binding.witnessIndex >= deps_.mixedOwnerTilingWitnesses.size())
      continue;

    const MixedOwnerTilingWitness &witness =
        deps_.mixedOwnerTilingWitnesses[binding.witnessIndex];
    if (witness.witnessId != binding.parentTilingWitnessId ||
        binding.segmentIndex >= witness.edges.size())
      continue;

    const MixedOwnerTilingSegmentWitness &segment =
        witness.edges[binding.segmentIndex];
    if (segment.parentTilingWitnessId != witness.witnessId ||
        segment.segmentIndex != binding.segmentIndex ||
        segment.kind != MixedOwnerTilingEdgeKind::TokenSegment ||
        segment.aStart != aStart || segment.aEnd != aEnd ||
        segment.bStart != bStart || segment.bEnd != bEnd)
      continue;

    ProofSummary candidate = summary;
    candidate.hasMixedOwnerTilingWitness = true;
    candidate.mixedOwnerTilingWitness = witness;
    candidate.hasMixedOwnerTilingSegmentSelection = true;
    candidate.mixedOwnerTilingSegmentIndex = binding.segmentIndex;
    candidate.theoremClass = TheoremProofClass::MixedOwnerTilingProof;
    candidate.primaryProofClassExplicit = true;
    deps_.proofSummaryBuilder.FinalizeProofSummary(candidate);

    if (deps_.acceptedResultRanker.LatticePrefers(summary, candidate))
      continue;

    if (!selectedSummary ||
        deps_.acceptedResultRanker.LatticePrefers(candidate, *selectedSummary))
      selectedSummary = std::move(candidate);
  }

  if (selectedSummary)
    summary = std::move(*selectedSummary);
}

::clang::refold::OwnerRealizationResult
RefoldOwnerRealizationProofBuilder::TryBuildOwnerRealization(
    OwnerRealizationEvidenceKind evidence, OwnerClosure closure,
    StringRef detail) const {
  return TryBuildOwnerRealizationImpl(evidence, std::move(closure), detail,
                                      /*attachCanonicalStateSummary=*/true);
}

::clang::refold::OwnerRealizationResult
RefoldOwnerRealizationProofBuilder::TryBuildOwnerRealizationImpl(
    OwnerRealizationEvidenceKind evidence, OwnerClosure closure,
    StringRef detail, bool attachCanonicalStateSummary) const {
  OwnerRealizationResult result;
  result.witness.evidence = evidence;
  result.witness.closure =
      attachCanonicalStateSummary
          ? deps_.ownerStateProof.AttachCanonicalStateSummary(
                std::move(closure))
          : std::move(closure);
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
       deps_.ownerStateProof.StateComponentsMutatedByDelta(
           result.witness.closure.stateIn))
    addUniqueComponent(mutatedComponents, component);
  for (OwnerStateComponent component :
       deps_.ownerStateProof.StateComponentsMutatedByDelta(
           result.witness.closure.stateOut))
    addUniqueComponent(mutatedComponents, component);

  // Legacy macro/include owner realizations still project their canonical
  // owner-state mutations into widening obligations. Direct and specialized TU
  // source carriers deliberately do not: widening is not authority to consume
  // a preprocessing directive. Ordinary TUByteSpan evidence is authorized by
  // the exact hunk/span theorem; specialized TU evidence is authorized by the
  // directive/state-specific planner that called its separate factory.
  const bool isTURealization =
      evidence == OwnerRealizationEvidenceKind::TUByteSpan ||
      evidence == OwnerRealizationEvidenceKind::TUSpecializedRealization;
  if (!isTURealization) {
    const OwnerStateBoundary stateBoundary =
        OwnerStateBoundary::FromSourceAndATokens(
            result.witness.closure.source, result.witness.closure.aTokens);
    for (OwnerStateComponent component : mutatedComponents) {
      ClosureWideningWitness stateWitness;
      stateWitness.component = component;
      stateWitness.boundary = stateBoundary;
      stateWitness.detail = detail.str();
      result.witness.stateWitnesses.push_back(
          SuffixStabilityWitness::From(std::move(stateWitness)));
    }
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
    REFOLD_LOG_TRACE(
        "proof/owner-realization",
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
      deps_.ownerStateProof.OwnerStateDeltaMutatesAnyState(
          result.witness.closure.stateIn) ||
      deps_.ownerStateProof.OwnerStateDeltaMutatesAnyState(
          result.witness.closure.stateOut);
  if (mutatesOrCarriesUnmodeledState && result.witness.stateWitnesses.empty() &&
      evidence != OwnerRealizationEvidenceKind::TUSpecializedRealization) {
    return reject(
        TerminalFallbackObligationKind::StateTransitionClosure,
        TerminalFallbackFailureReason::StateTransitionConsumedAndObserved);
  }

  result.accepted = true;
  REFOLD_LOG_TRACE(
      "proof/owner-realization",
      "accept owner realization evidence={0} owner={1} source='{2}'[{3},{4}) "
      "A=[{5},{6}) B=[{7},{8}) stateWitnesses={9} detail='{10}'",
      evidence, result.witness.closure.owner.kind,
      result.witness.closure.source.path, result.witness.closure.source.begin,
      result.witness.closure.source.end, result.witness.closure.aTokens.begin,
      result.witness.closure.aTokens.end, result.witness.closure.bTokens.begin,
      result.witness.closure.bTokens.end,
      static_cast<uint64_t>(result.witness.stateWitnesses.size()),
      result.detail);
  return result;
}

void RefoldOwnerRealizationProofBuilder::
    ApplyOwnerRealizationResultToProofSummary(
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
    deps_.proofSummaryBuilder.FinalizeProofSummary(summary);
    return;
  }

  summary.discharge.status = ProofDischargeStatus::Rejected;
  if (summary.discharge.failedObligation == ProofObligationKind::Unknown)
    summary.discharge.failedObligation =
        ProofObligationKind::OwnerRealizationWitnessTracked;
  if (summary.discharge.failureReason == ProofFailureReason::None)
    summary.discharge.failureReason =
        ProofFailureReason::MissingOwnerRealizationWitness;

  REFOLD_LOG_TRACE(
      "proof/owner-realization",
      "reject accepted candidate after owner-realization gate failed: {0} "
      "detail='{1}'",
      result.failure, result.detail);
  deps_.proofSummaryBuilder.FinalizeProofSummary(summary);
}

::clang::refold::OwnerRealizationResult
RefoldOwnerRealizationProofBuilder::BuildMacroWholeCoverOwnerRealization(
    const RefoldModel::MacroInvocation &macro,
    const WholeCoverPlan &plan) const {
  const std::string sourcePath =
      macro.invFile ? macro.invFile->str() : deps_.model.GetSourcePath().str();
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

::clang::refold::OwnerRealizationResult
RefoldOwnerRealizationProofBuilder::BuildIncludeOwnerRealization(
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
    bTokens =
        OwnerTokenRange::From(bTokenEnvelope->first, bTokenEnvelope->second);
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

  OwnerClosure closure =
      OwnerClosure::From(Owner::Include(include.id),
                         OwnerSourceRange::From(include.sitePath, include.siteB,
                                                include.siteE, include.parent),
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
      deps_.ownerStateProof.IncludeStateBoundaryForIncludeSite(include);
  (void)deps_.ownerStateProof.CheckStateTransitionAcrossEditBoundary(
      includeBoundary, OwnerStateComponent::IncludeState,
      StateMutationKind::Materialized,
      deps_.ownerStateProof.BuildStateTransitionWitness(
          SuffixStabilityWitnessKind::OwnerMaterialization,
          OwnerStateComponent::IncludeState, includeBoundary, detail),
      "include-owner-realization", detail, /*requireKnownObserver=*/false);
  (void)deps_.ownerStateProof.CheckStateTransitionAcrossEditBoundary(
      includeBoundary, OwnerStateComponent::IncludeGuardState,
      StateMutationKind::WidenedIntoClosure,
      deps_.ownerStateProof.BuildStateTransitionWitness(
          SuffixStabilityWitnessKind::ClosureWidening,
          OwnerStateComponent::IncludeGuardState, includeBoundary, detail),
      "include-owner-realization", detail, /*requireKnownObserver=*/false);

  return TryBuildOwnerRealization(evidence, std::move(closure), detail);
}

bool RefoldOwnerRealizationProofBuilder::
    ValidateDurableStructuralSegmentBinding(
        const diffutils::Hunk &hunk, const TUByteSpanPlan &spanPlan,
        const StructuralHunkSegmentBinding &structuralBinding) const {
  // The local binding is only an index key. Resolve it through the persisted
  // tiling ledger before treating it as authority that preprocessing structure
  // was kept outside this segment's emitted source interval.
  if (!structuralBinding.IsWellFormed() ||
      structuralBinding.segmentAStart != hunk.aStart ||
      structuralBinding.segmentAEnd != hunk.aEnd ||
      structuralBinding.segmentBStart != hunk.bStart ||
      structuralBinding.segmentBEnd != hunk.bEnd ||
      structuralBinding.sourceBegin != spanPlan.tuByteBegin ||
      structuralBinding.sourceEnd != spanPlan.tuByteEnd) {
    return false;
  }

  bool foundUniqueBinding = false;
  for (const MixedOwnerTilingSegmentBinding &binding :
       deps_.mixedOwnerTilingSegmentBindings) {
    if (binding.aStart != hunk.aStart || binding.aEnd != hunk.aEnd ||
        binding.bStart != hunk.bStart || binding.bEnd != hunk.bEnd ||
        binding.parentTilingWitnessId != structuralBinding.witnessId ||
        binding.segmentIndex != structuralBinding.segmentIndex) {
      continue;
    }

    // Two durable ledger entries for the same structural segment are
    // ambiguous authority. Reject rather than relying on vector order.
    if (foundUniqueBinding ||
        binding.witnessIndex >= deps_.mixedOwnerTilingWitnesses.size()) {
      return false;
    }

    const MixedOwnerTilingWitness &witness =
        deps_.mixedOwnerTilingWitnesses[binding.witnessIndex];
    if (witness.witnessId != structuralBinding.witnessId ||
        witness.originalAStart != structuralBinding.originalAStart ||
        witness.originalAEnd != structuralBinding.originalAEnd ||
        witness.originalBStart != structuralBinding.originalBStart ||
        witness.originalBEnd != structuralBinding.originalBEnd ||
        (witness.reason !=
             StructuralTilingReason::PreservedPreprocessingStructure &&
         witness.reason != StructuralTilingReason::
                               MixedRealizersAndPreservedStructure) ||
        witness.stateGapCount == 0 ||
        witness.protectedStructureGapCount == 0 ||
        witness.preservedInPlaceGapCount != witness.stateGapCount ||
        witness.preservedInPlaceGapCount <
            witness.protectedStructureGapCount ||
        !witness.uniquePartition || !witness.stateSummariesComposed ||
        !witness.stateTransitionsComposed ||
        !witness.ownerBoundariesComposed ||
        !witness.targetTokenStreamComposed ||
        !witness.compositionEdgesProven ||
        !witness.sourceByteCoverComplete ||
        !witness.preservedGapSourceOrderProven ||
        !witness.preservedGapsDisjointFromTokenSegments ||
        !witness.preservedGapsDisjointFromEdits ||
        structuralBinding.segmentIndex >= witness.edges.size()) {
      return false;
    }

    if (!structuralPreservedSourceTopologyIsComplete(witness))
      return false;

    const bool deleteOnlyWitness =
        witness.originalBStart == witness.originalBEnd;
    if ((deleteOnlyWitness &&
         (!witness.sharedEmptyBEnvelopeProven ||
          witness.sharedEmptyBBoundary != witness.originalBStart ||
          !witness.preservedStateChainComposed)) ||
        (!deleteOnlyWitness &&
         (witness.sharedEmptyBEnvelopeProven ||
          witness.preservedStateChainComposed))) {
      return false;
    }

    const bool requiresCanonicalPhysicalRunProof =
        witness.reason ==
            StructuralTilingReason::PreservedPreprocessingStructure ||
        structuralReplacementRequiresBoundaryProjection(witness);
    if (requiresCanonicalPhysicalRunProof &&
        (witness.physicalSourceRunCount < 2 ||
         !witness.physicalSourceRunsProven)) {
      return false;
    }
    if (witness.reason ==
            StructuralTilingReason::PreservedPreprocessingStructure &&
        (witness.physicalSourceRunCount != witness.tokenSegmentCount ||
         !witness.uniqueMinimumFragmentPartition)) {
      return false;
    }

    if (deleteOnlyWitness) {
      if (witness.uniqueBoundaryProjectionProven ||
          witness.boundaryProjectionCount != 0 ||
          !witness.boundaryProjections.empty()) {
        return false;
      }
    } else if (requiresCanonicalPhysicalRunProof) {
      if (!structuralReplacementBoundaryProjectionIsComplete(witness))
        return false;
    } else if (witness.uniqueBoundaryProjectionProven ||
               witness.boundaryProjectionCount != 0 ||
               !witness.boundaryProjections.empty()) {
      return false;
    }

    for (const MixedOwnerTilingSegmentWitness &witnessSegment :
         witness.edges) {
      if (witnessSegment.kind == MixedOwnerTilingEdgeKind::TokenSegment) {
        if (witnessSegment.gapDisposition !=
            StructuralGapDisposition::Unknown) {
          return false;
        }
        continue;
      }

      if (witnessSegment.kind != MixedOwnerTilingEdgeKind::StateGap ||
          witnessSegment.gapDisposition !=
              StructuralGapDisposition::PreservedInPlace ||
          !witnessSegment.sourceBytesPreservedUnchanged ||
          !witnessSegment.ownerIdentityKnown ||
          witnessSegment.ownerIdentity.kind == OwnerKind::Unknown ||
          !witnessSegment.sourceByteRangeKnown ||
          witnessSegment.sourcePath.empty() ||
          witnessSegment.sourceEnd <= witnessSegment.sourceBegin ||
          (witnessSegment.protectedPreprocessingStructure &&
           (!witnessSegment.protectedStructureIdentityRecorded ||
            witnessSegment.protectedStructureKind ==
                StructuralProtectedStructureKind::Unknown))) {
        return false;
      }
    }

    const MixedOwnerTilingSegmentWitness &segment =
        witness.edges[structuralBinding.segmentIndex];
    if (segment.parentTilingWitnessId != witness.witnessId ||
        segment.segmentIndex != structuralBinding.segmentIndex ||
        segment.sourceOrderPosition != structuralBinding.segmentIndex ||
        segment.kind != MixedOwnerTilingEdgeKind::TokenSegment ||
        segment.gapDisposition != StructuralGapDisposition::Unknown ||
        segment.aStart != hunk.aStart || segment.aEnd != hunk.aEnd ||
        segment.bStart != hunk.bStart || segment.bEnd != hunk.bEnd ||
        (deleteOnlyWitness &&
         (!segment.allowEmptyBEnvelope ||
          segment.bStart != witness.sharedEmptyBBoundary ||
          segment.bEnd != witness.sharedEmptyBBoundary)) ||
        (!deleteOnlyWitness &&
         ((segment.bStart == segment.bEnd) != segment.allowEmptyBEnvelope ||
          (segment.allowEmptyBEnvelope &&
           !witness.uniqueBoundaryProjectionProven))) ||
        !segment.ownerClosureComplete || !segment.ownerIdentityKnown ||
        segment.ownerIdentity.kind == OwnerKind::Unknown ||
        segment.sourceBytesPreservedUnchanged ||
        segment.protectedStructureIdentityRecorded ||
        segment.protectedStructureKind !=
            StructuralProtectedStructureKind::Unknown ||
        !segment.sourceByteRangeKnown ||
        segment.sourcePath.empty() ||
        segment.sourceBegin != spanPlan.tuByteBegin ||
        segment.sourceEnd != spanPlan.tuByteEnd ||
        !segment.protectedStructurePreservedOutsideSegment) {
      return false;
    }

    foundUniqueBinding = true;
  }

  return foundUniqueBinding;
}

::clang::refold::OwnerRealizationResult
RefoldOwnerRealizationProofBuilder::BuildTUOwnerRealization(
    AcceptedPathKind currentPath, const diffutils::Hunk &hunk,
    const TUByteSpanPlan &spanPlan,
    const StructuralHunkSegmentBinding *structuralBinding) const {
  OwnerClosure closure = OwnerClosure::From(
      Owner::TU(),
      OwnerSourceRange::From(deps_.model.GetSourcePath(),
                             spanPlan.tuByteBegin, spanPlan.tuByteEnd),
      OwnerTokenRange::From(hunk.aStart, hunk.aEnd),
      OwnerTokenRange::From(hunk.bStart, hunk.bEnd));

  const std::string detail =
      formatv("tu-byte-edit path={0} A=[{1},{2}) B=[{3},{4}) "
              "source=[{5},{6}) structuralBinding={7}",
              currentPath, hunk.aStart, hunk.aEnd, hunk.bStart, hunk.bEnd,
              spanPlan.tuByteBegin, spanPlan.tuByteEnd,
              structuralBinding ? "present" : "absent")
          .str();

  // Only ordinary direct-span paths may claim TUByteSpan evidence. Specialized
  // state-repair and source-closure planners use the separate source-only
  // evidence path below and cannot impersonate a concrete token hunk.
  if ((currentPath != AcceptedPathKind::TUByteSpanMappedEdit &&
       currentPath != AcceptedPathKind::TUByteSpanConservativeEdit) ||
      hunk.bEnd > deps_.bTokenCount ||
      !deps_.tuEdits.ValidateTUOwnerRealizationCarrier(
          hunk, spanPlan, structuralBinding) ||
      (structuralBinding &&
       !ValidateDurableStructuralSegmentBinding(hunk, spanPlan,
                                                *structuralBinding))) {
    return buildRejectedDirectTUCarrier(
        closure, detail, TerminalFallbackObligationKind::OwnerClosedCover,
        TerminalFallbackFailureReason::NoOwnerClosedCover);
  }

  OwnerRealizationResult result = TryBuildOwnerRealizationImpl(
      OwnerRealizationEvidenceKind::TUByteSpan, std::move(closure), detail,
      /*attachCanonicalStateSummary=*/false);
  if (!result.accepted)
    return result;

  result.witness.hasTUCarrierWitness = true;
  result.witness.tuCarrierWitness.exactHunkAndSpanValidated = true;
  result.witness.tuCarrierWitness.protectedStructureExcludedOrDeferred =
      true;
  if (structuralBinding) {
    result.witness.tuCarrierWitness.hasStructuralSegmentBinding = true;
    result.witness.tuCarrierWitness.structuralSegmentBindingValidated = true;
    result.witness.tuCarrierWitness.structuralWitnessId =
        structuralBinding->witnessId;
    result.witness.tuCarrierWitness.structuralSegmentIndex =
        structuralBinding->segmentIndex;
  }

  // A direct source carrier is never certified by closure widening. The span
  // theorem and optional structural binding are the only source authorities;
  // widening remains an obligation for owner-specific state proofs.
  if (llvm::any_of(result.witness.stateWitnesses,
                   isClosureWideningWitness)) {
    // Reject from the already-materialized closure.  The original local
    // closure was moved into the shared gate and must not be read again.
    return buildRejectedDirectTUCarrier(
        result.witness.closure, detail,
        TerminalFallbackObligationKind::StateTransitionClosure,
        TerminalFallbackFailureReason::StateTransitionConsumedAndObserved);
  }

  return result;
}

::clang::refold::OwnerRealizationResult
RefoldOwnerRealizationProofBuilder::BuildSpecializedTUOwnerRealization(
    AcceptedPathKind currentPath, uint64_t begin, uint64_t end) const {
  OwnerClosure closure = OwnerClosure::From(
      Owner::TU(),
      OwnerSourceRange::From(deps_.model.GetSourcePath(), begin, end),
      // Specialized sideband/repair edits may not correspond to one ordinary
      // token hunk. Their empty envelope is explicitly classified by the
      // distinct evidence kind below; it is not presented as TUByteSpan proof.
      OwnerTokenRange::From(0, 0), OwnerTokenRange::From(0, 0));

  if (currentPath != AcceptedPathKind::TUByteSpanConservativeEdit &&
      currentPath != AcceptedPathKind::TUIncludeClosureEdit) {
    return TryBuildOwnerRealizationImpl(
        OwnerRealizationEvidenceKind::Unknown, std::move(closure),
        formatv("invalid specialized TU path={0}", currentPath).str(),
        /*attachCanonicalStateSummary=*/false);
  }

  return TryBuildOwnerRealizationImpl(
      OwnerRealizationEvidenceKind::TUSpecializedRealization,
      std::move(closure),
      formatv("specialized-tu-edit path={0}", currentPath).str(),
      /*attachCanonicalStateSummary=*/true);
}

::clang::refold::ProofSummary
RefoldOwnerRealizationProofBuilder::BuildOwnerRealizationProofSummary(
    AcceptedPathKind currentPath,
    const OwnerRealizationResult &ownerRealization) const {
  // keeps spelling and materialization local to the include/TU
  // emitters.  This helper is therefore deliberately proof-only: it normalizes
  // an already-built owner closure into the shared OwnerRealizationWitness and
  // leaves candidate kind, byte/token interval, payload preview, and owner-id
  // decoration to the caller that actually knows the emitted surface.
  ProofSummary summary = deps_.buildAcceptedPathProofSummary(
      currentPath, /*patch=*/nullptr, /*tuAnchorWitness=*/nullptr,
      /*includeAnchorWitness=*/nullptr,
      /*terminalFallbackWitness=*/nullptr);
  ApplyOwnerRealizationResultToProofSummary(summary, ownerRealization);
  return summary;
}

} // namespace refold
} // namespace clang
