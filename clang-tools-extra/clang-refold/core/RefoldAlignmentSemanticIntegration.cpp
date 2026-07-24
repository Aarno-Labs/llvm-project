//===--- RefoldAlignmentSemanticIntegration.cpp ---------------------------===//
//
// RefoldEngine integration for exact semantic alignment resolution.
//
//===----------------------------------------------------------------------===//

#include "core/RefoldEngine.h"

#include "core/RefoldLog.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/FormatVariadic.h"

#include <algorithm>
#include <optional>
#include <string>
#include <utility>
#include <vector>

using namespace llvm;

namespace clang {
namespace refold {
namespace {

class ExactKeyBuilder {
public:
  void AddU64(uint64_t value) {
    value_.append(std::to_string(value));
    value_.push_back(';');
  }

  void AddBool(bool value) { AddU64(value ? 1 : 0); }

  void AddString(StringRef value) {
    AddU64(value.size());
    value_.append(value.data(), value.size());
    value_.push_back(';');
  }

  void AddRaw(StringRef value) {
    value_.append(value.data(), value.size());
  }

  void AddOptionalU64(const std::optional<uint64_t> &value) {
    AddBool(value.has_value());
    if (value)
      AddU64(*value);
  }

  void AddOptionalU32(const std::optional<uint32_t> &value) {
    AddBool(value.has_value());
    if (value)
      AddU64(*value);
  }

  void AddOptionalString(const std::optional<std::string> &value) {
    AddBool(value.has_value());
    if (value)
      AddString(*value);
  }

  std::string Take() { return std::move(value_); }

private:
  std::string value_;
};

void appendStructuralTilingWitness(ExactKeyBuilder &key,
                                   const MixedOwnerTilingWitness &witness);

void appendStructuralTilingWitnesses(
    ExactKeyBuilder &key, ArrayRef<MixedOwnerTilingWitness> witnesses) {
  key.AddU64(witnesses.size());
  for (const MixedOwnerTilingWitness &witness : witnesses)
    appendStructuralTilingWitness(key, witness);
}

void appendMaterializedMappings(
    ExactKeyBuilder &key,
    ArrayRef<MaterializedEditMapping> materializedMappings) {
  std::vector<MaterializedEditMapping> sortedMappings(
      materializedMappings.begin(), materializedMappings.end());
  llvm::sort(sortedMappings,
             [](const MaterializedEditMapping &lhs,
                const MaterializedEditMapping &rhs) {
               if (lhs.modifiedPreprocessedBegin !=
                   rhs.modifiedPreprocessedBegin)
                 return lhs.modifiedPreprocessedBegin <
                        rhs.modifiedPreprocessedBegin;
               if (lhs.modifiedPreprocessedEnd != rhs.modifiedPreprocessedEnd)
                 return lhs.modifiedPreprocessedEnd < rhs.modifiedPreprocessedEnd;
               if (lhs.refoldedSourceBegin != rhs.refoldedSourceBegin)
                 return lhs.refoldedSourceBegin < rhs.refoldedSourceBegin;
               return lhs.refoldedSourceEnd < rhs.refoldedSourceEnd;
             });
  key.AddU64(sortedMappings.size());
  for (const MaterializedEditMapping &mapping : sortedMappings) {
    key.AddU64(mapping.modifiedPreprocessedBegin);
    key.AddU64(mapping.modifiedPreprocessedEnd);
    key.AddU64(mapping.refoldedSourceBegin);
    key.AddU64(mapping.refoldedSourceEnd);
  }
}

void appendSourceGraphOutputs(ExactKeyBuilder &key,
                              ArrayRef<SourceGraphOutput> sourceGraphOutputs) {
  std::vector<SourceGraphOutput> sortedOutputs(sourceGraphOutputs.begin(),
                                                sourceGraphOutputs.end());
  llvm::sort(sortedOutputs,
             [](const SourceGraphOutput &lhs, const SourceGraphOutput &rhs) {
               if (lhs.includeId != rhs.includeId)
                 return lhs.includeId < rhs.includeId;
               if (lhs.relativePath != rhs.relativePath)
                 return lhs.relativePath < rhs.relativePath;
               if (lhs.originalTarget != rhs.originalTarget)
                 return lhs.originalTarget < rhs.originalTarget;
               if (lhs.resolvedPath != rhs.resolvedPath)
                 return lhs.resolvedPath < rhs.resolvedPath;
               if (lhs.bytes != rhs.bytes)
                 return lhs.bytes < rhs.bytes;
               return lhs.cleanupOnly < rhs.cleanupOnly;
             });
  key.AddU64(sortedOutputs.size());
  for (const SourceGraphOutput &output : sortedOutputs) {
    key.AddU64(output.includeId);
    key.AddString(output.relativePath);
    key.AddString(output.originalTarget);
    key.AddString(output.resolvedPath);
    key.AddString(output.bytes);
    key.AddBool(output.cleanupOnly);
  }
}

void appendFinalLineControlOwner(
    ExactKeyBuilder &key,
    const std::optional<FinalLineControlOwnerKey> &owner) {
  key.AddBool(owner.has_value());
  if (!owner)
    return;
  key.AddString(owner->physicalFile);
  key.AddOptionalU64(owner->ownerIncludeId);
}

void appendFinalLineControlPlan(
    ExactKeyBuilder &key,
    ArrayRef<FinalLineControlPruneCandidate> pruneCandidates,
    ArrayRef<FinalLineControlSourceMapping> sourceMappings) {
  // Preserve the exact planner-produced candidate order. The production
  // pruner receives this same sequence and canonicalizes it deterministically;
  // comparing the stronger pre-canonicalized sequence cannot merge two plans
  // that would exercise different pruning inputs.
  key.AddU64(pruneCandidates.size());
  for (const FinalLineControlPruneCandidate &candidate : pruneCandidates) {
    key.AddU64(candidate.finalBegin);
    key.AddU64(candidate.finalEnd);
    key.AddU64(static_cast<uint64_t>(candidate.origin));
    appendFinalLineControlOwner(key, candidate.physicalOwner);
    key.AddBool(candidate.producerProven);

    key.AddBool(candidate.obligationProof.has_value());
    if (candidate.obligationProof) {
      key.AddU64(static_cast<uint64_t>(
          candidate.obligationProof->obligation));
      key.AddU64(static_cast<uint64_t>(candidate.obligationProof->origin));
      appendFinalLineControlOwner(
          key, candidate.obligationProof->physicalOwner);
      key.AddBool(candidate.obligationProof->producerProven);
    }

    key.AddBool(candidate.removalProof.has_value());
    if (candidate.removalProof) {
      key.AddU64(static_cast<uint64_t>(candidate.removalProof->verdict));
      key.AddU64(static_cast<uint64_t>(candidate.removalProof->origin));
      key.AddU64(static_cast<uint64_t>(candidate.removalProof->discharge));
      appendFinalLineControlOwner(key, candidate.removalProof->physicalOwner);
      key.AddBool(candidate.removalProof->producerProven);
    }
  }

  std::vector<FinalLineControlSourceMapping> canonicalMappings(
      sourceMappings.begin(), sourceMappings.end());
  CanonicalizeFinalLineControlSourceMappings(canonicalMappings);
  key.AddU64(canonicalMappings.size());
  for (const FinalLineControlSourceMapping &mapping : canonicalMappings) {
    key.AddU64(mapping.finalBegin);
    key.AddU64(mapping.finalEnd);
    key.AddString(mapping.physicalFile);
    key.AddU64(mapping.sourceBegin);
    key.AddU64(mapping.sourceEnd);
    key.AddOptionalU64(mapping.ownerIncludeId);
  }
}

void appendAlignmentSemanticMutation(
    ExactKeyBuilder &key, const AlignmentSemanticMutationRecord &mutation) {
  key.AddU64(static_cast<uint64_t>(mutation.domain));
  key.AddOptionalU64(mutation.ownerIncludeId);
  key.AddU64(mutation.ownerId);
  key.AddU64(mutation.begin);
  key.AddU64(mutation.end);
  key.AddU64(mutation.replacementBytes);
  key.AddBool(mutation.protectedStructure);
}

void appendAlignmentSemanticMutations(
    ExactKeyBuilder &key,
    ArrayRef<AlignmentSemanticMutationRecord> mutations,
    bool insertions) {
  uint64_t count = 0;
  for (const AlignmentSemanticMutationRecord &mutation : mutations) {
    if (mutation.IsInsertion() == insertions)
      ++count;
  }
  key.AddU64(count);
  for (const AlignmentSemanticMutationRecord &mutation : mutations) {
    if (mutation.IsInsertion() != insertions)
      continue;
    appendAlignmentSemanticMutation(key, mutation);
  }
}

void appendAlignmentSemanticExpansionIdentities(
    ExactKeyBuilder &key,
    const AlignmentSemanticPreservationFootprint &footprint) {
  key.AddU64(footprint.expandedIncludeIds.size());
  for (uint64_t includeId : footprint.expandedIncludeIds)
    key.AddU64(includeId);
  key.AddU64(footprint.expandedMacroRootIds.size());
  for (uint64_t macroRootId : footprint.expandedMacroRootIds)
    key.AddU64(macroRootId);
}

void appendOwner(ExactKeyBuilder &key, const Owner &owner) {
  key.AddU64(static_cast<uint64_t>(owner.kind));
  key.AddOptionalU64(owner.includeId);
  key.AddOptionalU64(owner.macroInvocationId);
  key.AddOptionalU64(owner.macroDirectiveId);
  key.AddOptionalU64(owner.lineControlId);
  key.AddOptionalU64(owner.pragmaId);
  key.AddOptionalU64(owner.condGroupId);
  key.AddOptionalU64(owner.condArmId);
}

void appendOwnerSourceRange(ExactKeyBuilder &key,
                            const OwnerSourceRange &source) {
  key.AddString(source.path);
  key.AddU64(source.begin);
  key.AddU64(source.end);
  key.AddOptionalU64(source.includeId);
}

void appendOwnerTokenRange(ExactKeyBuilder &key,
                           const OwnerTokenRange &tokens) {
  key.AddU64(tokens.begin);
  key.AddU64(tokens.end);
}

void appendMacroStateIdentity(ExactKeyBuilder &key,
                              const MacroStateIdentity &identity) {
  key.AddString(identity.macroName);
  key.AddOptionalU64(identity.definitionDirectiveId);
  key.AddOptionalU64(identity.undefDirectiveId);
  key.AddBool(identity.functionLike);
  key.AddOptionalU32(identity.arity);
  key.AddBool(identity.variadic);
  key.AddString(identity.replacementTokenHash);
}

void appendMacroStateObservation(ExactKeyBuilder &key,
                                 const MacroStateObservation &observation) {
  key.AddU64(static_cast<uint64_t>(observation.kind));
  appendMacroStateIdentity(key, observation.identity);
}

void appendLineControlStateIdentity(
    ExactKeyBuilder &key, const LineControlStateIdentity &identity) {
  key.AddU64(identity.eventId);
  key.AddString(identity.physicalFile);
  key.AddOptionalU64(identity.siteBegin);
  key.AddOptionalU64(identity.siteEnd);
  key.AddBool(identity.active);
  key.AddBool(identity.producerProven);
  key.AddU64(identity.logicalLineAfter);
  key.AddString(identity.logicalFileAfter);
  key.AddOptionalU64(identity.ownerIncludeId);
  key.AddU64(static_cast<uint64_t>(identity.operandProvenance));
}

void appendBuiltinLocationObservation(
    ExactKeyBuilder &key, const BuiltinLocationObservation &observation) {
  key.AddU64(static_cast<uint64_t>(observation.kind));
  key.AddOptionalU64(observation.ownerIncludeId);
  key.AddOptionalU64(observation.sourceBegin);
  key.AddOptionalU64(observation.sourceEnd);
  key.AddOptionalU64(observation.aTokenBegin);
  key.AddOptionalU64(observation.aTokenEnd);
}

void appendCounterEventIdentity(ExactKeyBuilder &key,
                                const CounterEventIdentity &identity) {
  key.AddU64(identity.macroInvocationId);
  key.AddU64(identity.occurrenceOrdinal);
  key.AddOptionalU64(identity.ownerIncludeId);
  key.AddOptionalU64(identity.callerMacroId);
  key.AddU64(identity.aTokenBegin);
  key.AddU64(identity.aTokenEnd);
  key.AddString(identity.expansionSiteFile);
  key.AddOptionalU64(identity.expansionSiteBegin);
  key.AddOptionalU64(identity.expansionSiteEnd);
  key.AddString(identity.aValue);
  key.AddOptionalString(identity.expectedBValue);
  key.AddBool(identity.canStabilizeByLiteralization);
  key.AddBool(identity.canStabilizeByMaterialization);
}

void appendPragmaStateIdentity(ExactKeyBuilder &key,
                               const PragmaStateIdentity &identity) {
  key.AddU64(identity.pragmaId);
  key.AddString(identity.sitePath);
  key.AddU64(identity.siteBegin);
  key.AddU64(identity.siteEnd);
  key.AddOptionalU64(identity.ownerIncludeId);
  key.AddU64(static_cast<uint64_t>(identity.classification));
  key.AddString(identity.directiveFingerprint);
}

void appendIncludeStateIdentity(ExactKeyBuilder &key,
                                const IncludeStateIdentity &identity) {
  key.AddU64(identity.includeId);
  key.AddString(identity.directiveKind);
  key.AddString(identity.sitePath);
  key.AddU64(identity.siteBegin);
  key.AddU64(identity.siteEnd);
  key.AddString(identity.target);
  key.AddOptionalString(identity.resolvedPath);
  key.AddBool(identity.angled);
  key.AddOptionalU64(identity.parentIncludeId);
  key.AddBool(identity.hasTokenMaterialization);
}

void appendIncludeGuardStateIdentity(
    ExactKeyBuilder &key, const IncludeGuardStateIdentity &identity) {
  key.AddU64(static_cast<uint64_t>(identity.kind));
  key.AddU64(identity.includeId);
  key.AddString(identity.headerPath);
  key.AddOptionalString(identity.guardMacroName);
  key.AddOptionalU64(identity.parentIncludeId);
  key.AddBool(identity.producerProvenGuard);
}

void appendConditionalStateIdentity(
    ExactKeyBuilder &key, const ConditionalStateIdentity &identity) {
  key.AddU64(static_cast<uint64_t>(identity.role));
  key.AddU64(identity.groupId);
  key.AddOptionalU64(identity.armId);
  key.AddString(identity.file);
  key.AddU64(identity.groupBegin);
  key.AddU64(identity.groupEnd);
  key.AddOptionalU64(identity.parentArmId);
  key.AddOptionalU64(identity.parentIncludeId);
  key.AddString(identity.armKind);
  key.AddOptionalString(identity.conditionText);
  key.AddBool(identity.selected);
  key.AddOptionalU64(identity.aTokenBegin);
  key.AddOptionalU64(identity.aTokenEnd);
  key.AddBool(identity.conditionTruthProducerProven);
  key.AddBool(identity.reverseSolvedDirectiveRequired);
}

void appendOwnerStateFacts(ExactKeyBuilder &key,
                           const OwnerStateFacts &facts) {
  key.AddU64(facts.macroDefinitions.size());
  for (const MacroStateIdentity &identity : facts.macroDefinitions)
    appendMacroStateIdentity(key, identity);
  key.AddU64(facts.macroUndefinitions.size());
  for (const MacroStateIdentity &identity : facts.macroUndefinitions)
    appendMacroStateIdentity(key, identity);
  key.AddU64(facts.macroRequirements.size());
  for (const MacroStateIdentity &identity : facts.macroRequirements)
    appendMacroStateIdentity(key, identity);
  key.AddU64(facts.macroExpansionObservations.size());
  for (const MacroStateObservation &observation :
       facts.macroExpansionObservations)
    appendMacroStateObservation(key, observation);
  key.AddU64(facts.definedOperatorObservations.size());
  for (const MacroStateObservation &observation :
       facts.definedOperatorObservations)
    appendMacroStateObservation(key, observation);
  key.AddU64(facts.conditionalMacroObservations.size());
  for (const MacroStateObservation &observation :
       facts.conditionalMacroObservations)
    appendMacroStateObservation(key, observation);
  key.AddU64(facts.lineControlEvents.size());
  for (const LineControlStateIdentity &identity : facts.lineControlEvents)
    appendLineControlStateIdentity(key, identity);
  key.AddU64(facts.builtinLocationObservations.size());
  for (const BuiltinLocationObservation &observation :
       facts.builtinLocationObservations)
    appendBuiltinLocationObservation(key, observation);
  key.AddU64(facts.counterEvents.size());
  for (const CounterEventIdentity &identity : facts.counterEvents)
    appendCounterEventIdentity(key, identity);
  key.AddU64(facts.pragmaStateEvents.size());
  for (const PragmaStateIdentity &identity : facts.pragmaStateEvents)
    appendPragmaStateIdentity(key, identity);
  key.AddU64(facts.includeStateEvents.size());
  for (const IncludeStateIdentity &identity : facts.includeStateEvents)
    appendIncludeStateIdentity(key, identity);
  key.AddU64(facts.includeGuardStateEvents.size());
  for (const IncludeGuardStateIdentity &identity :
       facts.includeGuardStateEvents)
    appendIncludeGuardStateIdentity(key, identity);
  key.AddU64(facts.conditionalStateEvents.size());
  for (const ConditionalStateIdentity &identity :
       facts.conditionalStateEvents)
    appendConditionalStateIdentity(key, identity);
  key.AddU64(facts.missingStateFacts.size());
  for (const MissingStateFact &fact : facts.missingStateFacts) {
    key.AddU64(static_cast<uint64_t>(fact.kind));
    key.AddString(fact.detail);
  }
}

void appendOwnerStateDelta(ExactKeyBuilder &key,
                           const OwnerStateDelta &delta) {
  appendOwnerStateFacts(key, delta.entry);
  appendOwnerStateFacts(key, delta.observes);
  appendOwnerStateFacts(key, delta.mutates);
  appendOwnerStateFacts(key, delta.exit);
}

void appendOwnerObserverSummary(ExactKeyBuilder &key,
                                const OwnerObserverSummary &summary) {
  key.AddBool(summary.observesMacroExpansion);
  key.AddBool(summary.observesDefinedOperator);
  key.AddBool(summary.observesConditionalEvaluation);
  key.AddBool(summary.observesLineNumber);
  key.AddBool(summary.observesFileState);
  key.AddBool(summary.observesFileName);
  key.AddBool(summary.observesCounter);
  key.AddBool(summary.observesPragmaState);
  key.AddBool(summary.observesIncludeGuardState);
  key.AddBool(summary.observesIncludeState);
}

void appendOwnerStateBoundary(ExactKeyBuilder &key,
                              const OwnerStateBoundary &boundary) {
  key.AddBool(boundary.hasSourceBoundary);
  key.AddBool(boundary.hasTokenBoundary);
  appendOwnerSourceRange(key, boundary.source);
  appendOwnerTokenRange(key, boundary.aTokens);
}

void appendSuffixObserverResult(ExactKeyBuilder &key,
                                const SuffixObserverResult &result) {
  key.AddU64(result.observerSiteIndex);
  key.AddU64(result.nodeId);
  appendOwner(key, result.firstObserver);
  key.AddU64(static_cast<uint64_t>(result.component));
  key.AddU64(static_cast<uint64_t>(result.observationKind));
  key.AddU64(static_cast<uint64_t>(result.orderingProof));
  key.AddBool(result.materializable);
  key.AddBool(result.repairCanPrecede);

  key.AddU64(result.site.nodeId);
  key.AddU64(static_cast<uint64_t>(result.site.nodeKind));
  appendOwner(key, result.site.owner);
  appendOwnerSourceRange(key, result.site.source);
  appendOwnerTokenRange(key, result.site.aTokens);
  key.AddU64(static_cast<uint64_t>(result.site.component));
  key.AddU64(static_cast<uint64_t>(result.site.observationKind));
  appendOwnerObserverSummary(key, result.site.observations);
  key.AddU64(result.site.order);
  key.AddString(result.site.componentKey);
  key.AddString(result.site.detail);
}

void appendOptionalSuffixObserverResult(
    ExactKeyBuilder &key,
    const std::optional<SuffixObserverResult> &observer) {
  key.AddBool(observer.has_value());
  if (observer)
    appendSuffixObserverResult(key, *observer);
}

void appendClosureWideningWitness(ExactKeyBuilder &key,
                                  const ClosureWideningWitness &witness) {
  key.AddU64(static_cast<uint64_t>(witness.component));
  appendOwnerStateBoundary(key, witness.boundary);
  appendOptionalSuffixObserverResult(key, witness.widenedThroughObserver);
  key.AddString(witness.detail);
}

void appendSuffixStabilityWitness(ExactKeyBuilder &key,
                                  const SuffixStabilityWitness &witness) {
  key.AddU64(static_cast<uint64_t>(witness.kind));
  switch (witness.kind) {
  case SuffixStabilityWitnessKind::None:
    return;
  case SuffixStabilityWitnessKind::SuffixUnobserved:
    key.AddBool(witness.suffixUnobserved.has_value());
    if (witness.suffixUnobserved) {
      key.AddU64(static_cast<uint64_t>(witness.suffixUnobserved->component));
      appendOwnerStateBoundary(key, witness.suffixUnobserved->boundary);
      key.AddString(witness.suffixUnobserved->detail);
    }
    return;
  case SuffixStabilityWitnessKind::StateRepair:
    key.AddBool(witness.stateRepair.has_value());
    if (witness.stateRepair) {
      key.AddU64(static_cast<uint64_t>(witness.stateRepair->component));
      appendOwnerStateBoundary(key, witness.stateRepair->boundary);
      appendOptionalSuffixObserverResult(key,
                                         witness.stateRepair->firstObserver);
      key.AddString(witness.stateRepair->detail);
    }
    return;
  case SuffixStabilityWitnessKind::OwnerMaterialization:
    key.AddBool(witness.ownerMaterialization.has_value());
    if (witness.ownerMaterialization) {
      key.AddU64(
          static_cast<uint64_t>(witness.ownerMaterialization->component));
      appendOwnerStateBoundary(key, witness.ownerMaterialization->boundary);
      appendOptionalSuffixObserverResult(
          key, witness.ownerMaterialization->materializedObserver);
      key.AddString(witness.ownerMaterialization->detail);
    }
    return;
  case SuffixStabilityWitnessKind::ClosureWidening:
    key.AddBool(witness.closureWidening.has_value());
    if (witness.closureWidening)
      appendClosureWideningWitness(key, *witness.closureWidening);
    return;
  case SuffixStabilityWitnessKind::Literalization:
    key.AddBool(witness.literalization.has_value());
    if (witness.literalization) {
      key.AddU64(static_cast<uint64_t>(witness.literalization->component));
      appendOwnerStateBoundary(key, witness.literalization->boundary);
      appendOptionalSuffixObserverResult(
          key, witness.literalization->literalizedObserver);
      key.AddString(witness.literalization->detail);
    }
    return;
  case SuffixStabilityWitnessKind::TerminalStateFailure:
    key.AddBool(witness.terminalFailure.has_value());
    if (witness.terminalFailure) {
      key.AddU64(static_cast<uint64_t>(witness.terminalFailure->component));
      appendOwnerStateBoundary(key, witness.terminalFailure->boundary);
      key.AddBool(witness.terminalFailure->hasFailure);
      key.AddString(witness.terminalFailure->failure.ToString());
      key.AddString(witness.terminalFailure->detail);
    }
    return;
  }
  llvm_unreachable("invalid suffix stability witness kind");
}

void appendStateTransitionProof(ExactKeyBuilder &key,
                                const StateTransitionProof &proof) {
  appendOwnerStateDelta(key, proof.before);
  appendOwnerStateDelta(key, proof.after);
  key.AddU64(proof.suffixWitnesses.size());
  for (const SuffixStabilityWitness &witness : proof.suffixWitnesses)
    appendSuffixStabilityWitness(key, witness);
  key.AddU64(proof.wideningWitnesses.size());
  for (const ClosureWideningWitness &witness : proof.wideningWitnesses)
    appendClosureWideningWitness(key, witness);
  key.AddBool(proof.failure.has_value());
  if (proof.failure)
    key.AddString(proof.failure->ToString());
}

void appendStructuralTilingWitness(ExactKeyBuilder &key,
                                   const MixedOwnerTilingWitness &witness) {
  // Deliberately omit run-local witness ids.  The surrounding vector order,
  // original hunk envelope, and complete witness payload identify the theorem
  // structurally; allocator order is not a semantic equivalence dimension.
  key.AddU64(witness.originalAStart);
  key.AddU64(witness.originalAEnd);
  key.AddU64(witness.originalBStart);
  key.AddU64(witness.originalBEnd);
  key.AddU64(static_cast<uint64_t>(witness.reason));
  key.AddBool(witness.uniquePartition);
  key.AddBool(witness.sourceByteCoverComplete);
  key.AddBool(witness.targetTokenStreamComposed);
  key.AddBool(witness.stateTransitionsComposed);
  key.AddBool(witness.preservedGapsDisjointFromEdits);
  key.AddU64(witness.tokenSegmentCount);
  key.AddU64(witness.stateGapCount);
  key.AddU64(witness.distinctRealizerCount);
  key.AddU64(witness.protectedStructureGapCount);
  key.AddU64(witness.preservedInPlaceGapCount);
  key.AddU64(witness.physicalSourceRunCount);
  key.AddBool(witness.physicalSourceRunsProven);
  key.AddBool(witness.uniqueMinimumFragmentPartition);
  key.AddBool(witness.uniqueBoundaryProjectionProven);
  key.AddU64(witness.boundaryProjectionCount);
  key.AddBool(witness.sharedEmptyBEnvelopeProven);
  key.AddU64(witness.sharedEmptyBBoundary);
  key.AddBool(witness.preservedStateChainComposed);
  key.AddBool(witness.stateSummariesComposed);
  key.AddBool(witness.ownerBoundariesComposed);
  key.AddBool(witness.compositionEdgesProven);
  key.AddBool(witness.preservedGapSourceOrderProven);
  key.AddBool(witness.preservedGapsDisjointFromTokenSegments);
  key.AddString(witness.globalTargetPPTokenSignature);
  key.AddString(witness.globalCompositionSignature);

  key.AddU64(witness.boundaryProjections.size());
  for (const StructuralBoundaryProjectionWitness &projection :
       witness.boundaryProjections) {
    key.AddU64(projection.aTokenBoundary);
    key.AddU64(projection.lowerBTokenBoundary);
    key.AddU64(projection.upperBTokenBoundary);
    key.AddU64(projection.bTokenBoundary);
    key.AddBool(projection.uniqueProjection);
  }

  key.AddU64(witness.edges.size());
  for (const MixedOwnerTilingSegmentWitness &edge : witness.edges) {
    key.AddU64(edge.segmentIndex);
    key.AddU64(edge.sourceOrderPosition);
    key.AddU64(static_cast<uint64_t>(edge.kind));
    key.AddU64(edge.aStart);
    key.AddU64(edge.aEnd);
    key.AddU64(edge.bStart);
    key.AddU64(edge.bEnd);
    key.AddBool(edge.zeroTokenStateGap);
    key.AddU64(static_cast<uint64_t>(edge.gapDisposition));
    key.AddBool(edge.protectedPreprocessingStructure);
    key.AddBool(edge.allowEmptyBEnvelope);
    key.AddBool(edge.ownerClosureComplete);
    key.AddBool(edge.ownerIdentityKnown);
    appendOwner(key, edge.ownerIdentity);
    key.AddBool(edge.sourceByteRangeKnown);
    key.AddString(edge.sourcePath);
    key.AddOptionalU64(edge.sourceIncludeId);
    key.AddU64(edge.sourceBegin);
    key.AddU64(edge.sourceEnd);
    key.AddBool(edge.protectedStructureIdentityRecorded);
    key.AddU64(static_cast<uint64_t>(edge.protectedStructureKind));
    key.AddU64(static_cast<uint64_t>(edge.producerIdentityKind));
    key.AddOptionalU64(edge.producerItemId);
    key.AddOptionalU64(edge.producerConditionalGroupId);
    key.AddOptionalU64(edge.producerConditionalArmId);
    key.AddBool(edge.sourceBytesPreservedUnchanged);
    key.AddBool(edge.protectedStructurePreservedOutsideSegment);
    key.AddString(edge.ownerSignature);
    key.AddString(edge.sourceSignature);
    key.AddString(edge.producerPathSignature);
    key.AddString(edge.targetPPTokenSignature);
    appendStateTransitionProof(key, edge.canonicalStateTransition);
  }
}

std::string buildAlignmentRealizationEquivalenceKey(
    const AlignmentSemanticEquivalenceComponents &components) {
  ExactKeyBuilder key;
  key.AddString("clang-refold-alignment-realization-key-v1");
  // Raw A/B hunk partitioning is deliberately omitted. Every retained field
  // is produced after owner/structure planning and therefore describes the
  // exact realized source, state witnesses, and externally observable output.
  // A deterministic representative may be chosen only after every surviving
  // core-optimal map has this identical byte-exact key.
  key.AddRaw(components.structuralTilingWitnesses);
  key.AddString(components.stagedTopology);
  key.AddRaw(components.materializedMappings);
  key.AddRaw(components.sourceGraphOutputs);
  key.AddString(components.finalTU);
  key.AddRaw(components.destructiveMutationFootprint);
  key.AddRaw(components.insertionFrontiers);
  key.AddRaw(components.expansionIdentities);
  key.AddString(components.semanticPostconditions);
  key.AddString(components.realizationPostconditions);
  return key.Take();
}

std::string buildAlignmentConcreteOutputEquivalenceKey(
    const AlignmentSemanticEquivalenceComponents &components) {
  ExactKeyBuilder key;
  key.AddString("clang-refold-alignment-concrete-output-key-v1");
  // Every member of this class has independently completed the full theorem
  // audit. At that point the authoritative question is whether alignment
  // ambiguity changes the concrete emitted artifact or the deterministic
  // post-emission pruning inputs—not which internal proof carrier happened to
  // establish the same bytes. Keeping proof-carrier provenance here would
  // allow hunk partitioning to manufacture semantic classes after the planner
  // has already proved one identical output.
  key.AddString(components.finalTU);
  key.AddRaw(components.materializedMappings);
  key.AddRaw(components.sourceGraphOutputs);
  key.AddRaw(components.finalLineControlPlan);
  return key.Take();
}

} // namespace

void RefoldEngine::ResolveSemanticAlignment(
    ArrayRef<StringRef> aLexemes, ArrayRef<StringRef> bLexemes,
    ArrayRef<diffutils::LcsAGapProvenance> aGapProvenance,
    ArrayRef<diffutils::LcsBGapProvenance> bGapProvenance,
    diffutils::CertifiedLcsResult &alignment) {
  alignmentSemanticResolutionWitnesses_.clear();
  if (!alignmentSemanticResolverEnabled_ || alignmentSelectionOverride_ ||
      !alignment.completeCertification)
    return;

  RefoldAlignmentSemanticResolver resolver(
      RefoldAlignmentSemanticResolver::Dependencies{
          aLexemes, bLexemes, alignment, aGapProvenance, bGapProvenance,
          [this](const AlignmentSelectionOverride &selection) {
            return SimulateSemanticAlignmentCandidate(selection);
          }});
  RefoldAlignmentSemanticResolver::ResolutionResult resolution =
      resolver.Resolve();
  alignment.selectedMap = std::move(resolution.selectedMap);
  alignment.selectedAnchorProofs =
      std::move(resolution.selectedAnchorProofs);
  alignmentSemanticResolutionWitnesses_ = std::move(resolution.witnesses);

  if (resolution.committedEquivalentClass) {
    alignmentSemanticTheoremActive_ = true;
    REFOLD_LOG_TRACE(
        "lcs/semantic-resolver",
        "committed one theorem-equivalent alignment class: witnesses={0}",
        alignmentSemanticResolutionWitnesses_.size());
  }
}

AlignmentSemanticSimulationResult
RefoldEngine::SimulateSemanticAlignmentCandidate(
    const AlignmentSelectionOverride &selection) const {
  AlignmentSemanticSimulationResult result;
  std::vector<MaterializedEditMapping> materializedMappings;
  std::vector<SourceGraphOutput> sourceGraphOutputs;

  // Candidate simulations must expose exactly the same optional output
  // surfaces as the parent run.  Supplying sidecar sinks unconditionally makes
  // the nested engine prove and serialize artifacts that production was never
  // asked to emit.  Two alignments may then be separated solely by private
  // edit-envelope provenance even though every requested output is identical.
  // Preserve fail-closed behavior when a sidecar was requested by passing the
  // corresponding sink through to every candidate; otherwise leave it absent,
  // exactly as in the parent engine.
  std::vector<MaterializedEditMapping> *candidateMaterializedMappings =
      materializedEditMappings_ ? &materializedMappings : nullptr;
  std::vector<SourceGraphOutput> *candidateSourceGraphOutputs =
      sourceGraphOutputs_ ? &sourceGraphOutputs : nullptr;

  RefoldEngine candidate(
      model_.CloneForReadOnlyConsumer(), aSource_, aToks_, aTokOff_, bSource_,
      bToks_, bTokOff_, noLines_, strict_, proofAuditMode_, finalOutputPath_,
      sidebandPragmaEdits_, candidateMaterializedMappings,
      FinalLineControlValidationCallback(), candidateSourceGraphOutputs,
      selection, /*alignmentSemanticResolverEnabled=*/false,
      StringRef(tuSourceBytes_));

  // Run the same complete structural pipeline with the outer planner policy.
  // The alignment override activates the dedicated semantic theorem boundary,
  // so witness resolution and no-legacy auditing are strict without changing
  // unrelated planner behavior. The production parent activates the same
  // boundary when this class commits. Executable final-line pruning is
  // deliberately disabled in an
  // isolated simulation: its callback writes temporary files and invokes the
  // preprocessor, so it is not a read-only planning service.  Comparing the
  // exact unpruned final source is conservative—equal simulation outputs would
  // present identical inputs to the later production pruning oracle, while two
  // outputs that only converge after pruning remain separate and fail closed.
  // The nested engine also has semantic resolution disabled, so the supplied
  // alignment theorem is the only non-forced anchor authority and recursion is
  // impossible.
  std::string finalTU = candidate.Refold();

  // Capture every production theorem component once before any rejection
  // return. The resolver compares the post-owner realization and requested
  // concrete outputs directly; evidence-only hashes are intentionally omitted
  // from the hot candidate-simulation path.
  AlignmentSemanticEquivalenceComponents &components = result.components;

  ExactKeyBuilder structuralTilingKey;
  appendStructuralTilingWitnesses(structuralTilingKey,
                                  candidate.mixedOwnerTilingWitnesses_);
  components.structuralTilingWitnesses = structuralTilingKey.Take();
  components.stagedTopology =
      candidate.alignmentSimulationStagedTopologyKey_;
  components.preservationFootprint =
      candidate.alignmentSimulationPreservationFootprint_;
  ExactKeyBuilder destructiveMutationKey;
  appendAlignmentSemanticMutations(
      destructiveMutationKey,
      components.preservationFootprint.mutations,
      /*insertions=*/false);
  components.destructiveMutationFootprint = destructiveMutationKey.Take();
  ExactKeyBuilder insertionFrontierKey;
  appendAlignmentSemanticMutations(
      insertionFrontierKey, components.preservationFootprint.mutations,
      /*insertions=*/true);
  components.insertionFrontiers = insertionFrontierKey.Take();
  ExactKeyBuilder expansionIdentityKey;
  appendAlignmentSemanticExpansionIdentities(
      expansionIdentityKey, components.preservationFootprint);
  components.expansionIdentities = expansionIdentityKey.Take();
  components.semanticPostconditions =
      components.preservationFootprint.semanticPostconditionKey;
  components.realizationPostconditions =
      components.preservationFootprint.realizationPostconditionKey;
  components.finalTU = finalTU;
  ExactKeyBuilder finalLineControlPlanKey;
  appendFinalLineControlPlan(finalLineControlPlanKey,
                             candidate.finalLineControlPruneCandidates_,
                             candidate.finalLineControlSourceMappings_);
  components.finalLineControlPlan = finalLineControlPlanKey.Take();

  ExactKeyBuilder materializedMappingKey;
  appendMaterializedMappings(materializedMappingKey, materializedMappings);
  components.materializedMappings = materializedMappingKey.Take();
  ExactKeyBuilder sourceGraphOutputKey;
  appendSourceGraphOutputs(sourceGraphOutputKey, sourceGraphOutputs);
  components.sourceGraphOutputs = sourceGraphOutputKey.Take();
  if (candidate.terminalSink_.HasRequest()) {
    result.disposition =
        AlignmentSemanticSimulationDisposition::TerminalFallback;
    result.rejectionReason =
        "candidate requested terminal fallback during structural planning";
    return result;
  }

  std::string failure;
  if (!candidate.AlignmentSimulationProofComplete(failure)) {
    result.disposition =
        AlignmentSemanticSimulationDisposition::ProofIncomplete;
    result.rejectionReason = std::move(failure);
    return result;
  }

  result.accepted = true;
  result.disposition = AlignmentSemanticSimulationDisposition::Accepted;
  result.realizationEquivalenceKey =
      buildAlignmentRealizationEquivalenceKey(components);
  result.concreteOutputEquivalenceKey =
      buildAlignmentConcreteOutputEquivalenceKey(components);
  return result;
}

bool RefoldEngine::AlignmentSimulationProofComplete(std::string &failure) const {
  // Alignment equivalence is itself a theorem boundary. Candidate
  // simulations must therefore satisfy the complete final-emission theorem
  // audit even when the outer user-facing run is non-strict. Non-strict mode
  // may report an audit violation without requesting terminal fallback; such a
  // candidate is still insufficient authority for restoring a non-forced
  // alignment anchor.
  if (!lastTheoremAudit_.theoremSatisfied) {
    failure = lastTheoremAudit_.firstViolation.empty()
                  ? std::string("candidate failed the final theorem audit")
                  : llvm::formatv("candidate failed the final theorem audit: {0}",
                                  lastTheoremAudit_.firstViolation)
                        .str();
    return false;
  }

  if (structuralHunkPlanningPhase_ !=
      StructuralHunkPlanningPhase::InsertionLedgerReady) {
    failure = "candidate did not complete structural hunk planning";
    return false;
  }

  if (!alignmentSimulationStagedTopologyComplete_) {
    failure = alignmentSimulationStagedTopologyFailure_.empty()
                  ? std::string("candidate staged topology has incomplete "
                                "witness-equivalence proof")
                  : alignmentSimulationStagedTopologyFailure_;
    return false;
  }

  for (const MixedOwnerTilingWitness &witness :
       mixedOwnerTilingWitnesses_) {
    if (!witness.uniquePartition || !witness.sourceByteCoverComplete ||
        !witness.targetTokenStreamComposed ||
        !witness.stateTransitionsComposed ||
        !witness.preservedGapsDisjointFromEdits ||
        !witness.ownerBoundariesComposed ||
        !witness.compositionEdgesProven ||
        !witness.preservedGapSourceOrderProven ||
        !witness.preservedGapsDisjointFromTokenSegments) {
      failure = llvm::formatv(
                    "structural tiling witness {0} is incomplete",
                    witness.witnessId)
                    .str();
      return false;
    }
    for (const MixedOwnerTilingSegmentWitness &edge : witness.edges) {
      if (!edge.ownerClosureComplete || !edge.ownerIdentityKnown ||
          !edge.ownerIdentity.IsKnown()) {
        failure = llvm::formatv(
                      "structural tiling witness {0} has incomplete owner "
                      "closure at edge {1}",
                      witness.witnessId, edge.segmentIndex)
                      .str();
        return false;
      }
      if (edge.kind == StructuralHunkTilingEdgeKind::TokenSegment &&
          !edge.sourceByteRangeKnown) {
        failure = llvm::formatv(
                      "structural tiling witness {0} lacks a physical source "
                      "range at token edge {1}",
                      witness.witnessId, edge.segmentIndex)
                      .str();
        return false;
      }
      if (edge.protectedPreprocessingStructure &&
          (!edge.protectedStructureIdentityRecorded ||
           !edge.sourceBytesPreservedUnchanged)) {
        failure = llvm::formatv(
                      "structural tiling witness {0} consumes unproved "
                      "protected structure at edge {1}",
                      witness.witnessId, edge.segmentIndex)
                      .str();
        return false;
      }
    }
  }

  if (alignmentSimulationStagedTopologyKey_.empty()) {
    // An empty edit topology is valid only when the normalized diff itself is
    // empty. A nonempty hunk set must have reached an explicit source carrier.
    if (!abTokHunks_.empty()) {
      failure = "nonempty candidate diff produced no staged edit topology";
      return false;
    }
  }
  return true;
}

} // namespace refold
} // namespace clang
