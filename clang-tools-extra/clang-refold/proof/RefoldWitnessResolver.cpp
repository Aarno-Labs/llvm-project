//===--- RefoldWitnessResolver.cpp -------------------------------*- C++
//-*-===//
//
// Implementation of the central witness resolver.  See the header for the
// architectural contract.  Internal helpers (closure-ledger entry builders,
// the converted-invocation-repair domination check) live in the anonymous
// namespace below.
//
//===----------------------------------------------------------------------===//

#include "proof/RefoldWitnessResolver.h"

#include "proof/RefoldAcceptedResultTypes.h"
#include "proof/RefoldOwnerStateTypes.h"
#include "proof/RefoldProofVocabulary.h"
#include "proof/RefoldTheoremAudit.h"
#include "proof/RefoldWitnessClassifier.h"
#include "proof/RefoldWitnessEquivalenceKeyBuilder.h"
#include "proof/RefoldWitnessTrace.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Support/FormatVariadic.h"

#include <map>
#include <optional>
#include <string>
#include <utility>

using namespace llvm;

namespace clang {
namespace refold {

RefoldWitnessResolver::RefoldWitnessResolver(Dependencies deps)
    : deps_(std::move(deps)) {}

bool RefoldWitnessResolver::IsResolverAuthoritativeWitnessFamily(
    WitnessProofFamily family) {
  switch (family) {
  case WitnessProofFamily::MacroActualRepair:
  case WitnessProofFamily::DefinitionTapeReplay:
  case WitnessProofFamily::GeneratedCalleeReplay:
  case WitnessProofFamily::Stringification:
  case WitnessProofFamily::TokenPaste:
  case WitnessProofFamily::VariadicComma:
  case WitnessProofFamily::ZeroTokenBoundary:
  case WitnessProofFamily::LineControlObserver:
  case WitnessProofFamily::CounterState:
  case WitnessProofFamily::MixedOwnerTiling:
    return true;
  case WitnessProofFamily::Unknown:
  case WitnessProofFamily::AcceptedResult:
  case WitnessProofFamily::IncludePreservation:
  case WitnessProofFamily::IncludeRealization:
  case WitnessProofFamily::TUAnchor:
  case WitnessProofFamily::TUTextEdit:
  case WitnessProofFamily::OwnerRealization:
  case WitnessProofFamily::TerminalFallback:
    return false;
  }
  return false;
}

bool RefoldWitnessResolver::IsResolverAuthoritativeWitness(
    const RefoldWitness &witness) {
  if (IsResolverAuthoritativeWitnessFamily(witness.family))
    return true;

  // Macro whole-cover realization now carries the same theorem-facing
  // owner-closure, target-token, suffix-state, observer, counter, diagnostic,
  // and composition keys as the already converted families.  Keep the authority
  // gate witness-specific so include/TU realization carriers remain probe-only
  // until their own families are audited.
  if (witness.family != WitnessProofFamily::OwnerRealization)
    return false;
  if (witness.sourceFamily != "MacroWholeCoverRealization")
    return false;
  if (witness.key.boundaryClass != WitnessBoundaryClass::RootInvocation)
    return false;
  if (witness.key.compositionClass != WitnessCompositionClass::OwnerClosed)
    return false;
  if (witness.key.diagnosticClass !=
      WitnessDiagnosticClass::RealizesEditedSurface)
    return false;

  return true;
}

::clang::refold::WitnessCanonicalCost
RefoldWitnessResolver::BuildWitnessCanonicalCost(
    const AcceptedResultCandidate &candidate) {
  WitnessCanonicalCost cost;
  cost.preserveOriginalPenalty =
      candidate.proofSummary.structurePreserving ? 0 : 1;
  cost.sourceRangeBytes =
      candidate.end >= candidate.begin ? candidate.end - candidate.begin : 0;
  cost.ownerBoundaryChangePenalty =
      candidate.kind == AcceptedResultCandidateKind::TerminalOutOfDomain ? 1
                                                                         : 0;
  cost.spellingChangePenalty =
      candidate.hasPayloadPreview ? candidate.payloadPreview.size() : 0;
  cost.sourceOrder = candidate.begin;
  return cost;
}

::clang::refold::RefoldWitness RefoldWitnessResolver::BuildRefoldWitness(
    const AcceptedResultCandidate &candidate, llvm::StringRef role,
    uint64_t witnessId) const {
  RefoldWitness witness;
  witness.witnessId = witnessId;
  witness.family = witnessFamilyForAcceptedPath(
      candidate.proofSummary.inventory.currentPath);
  if (candidate.hasGeneratedCalleeReplayWitness)
    witness.family = WitnessProofFamily::GeneratedCalleeReplay;
  else if (candidate.hasVariadicCommaWitness)
    witness.family = WitnessProofFamily::VariadicComma;
  else if (candidate.hasTokenPasteWitness)
    witness.family = WitnessProofFamily::TokenPaste;
  else if (candidate.hasStringificationWitness)
    witness.family = WitnessProofFamily::Stringification;
  else if (candidate.hasZeroTokenBoundaryWitness)
    witness.family = WitnessProofFamily::ZeroTokenBoundary;
  if (witness.family == WitnessProofFamily::Unknown &&
      candidate.kind != AcceptedResultCandidateKind::Unknown)
    witness.family = WitnessProofFamily::AcceptedResult;
  if (candidate.hasCounterStateWitness &&
      (witness.family == WitnessProofFamily::AcceptedResult ||
       candidate.proofSummary.inventory.currentPath ==
           AcceptedPathKind::MacroCounterLiteral))
    witness.family = WitnessProofFamily::CounterState;
  if (candidate.hasLineControlObserverWitness &&
      witness.family == WitnessProofFamily::AcceptedResult)
    witness.family = WitnessProofFamily::LineControlObserver;
  if ((candidate.proofSummary.hasMixedOwnerTilingWitness ||
       candidate.proofSummary.theoremClass ==
           TheoremProofClass::MixedOwnerTilingProof) &&
      (witness.family == WitnessProofFamily::Unknown ||
       witness.family == WitnessProofFamily::AcceptedResult ||
       witness.family == WitnessProofFamily::TUTextEdit ||
       witness.family == WitnessProofFamily::OwnerRealization))
    witness.family = WitnessProofFamily::MixedOwnerTiling;

  if (candidate.hasRootMacroId)
    witness.owner = llvm::formatv("macro#{0}", candidate.rootMacroId).str();
  else if (candidate.hasOwnerIncludeId)
    witness.owner =
        llvm::formatv("include#{0}", candidate.ownerIncludeId).str();
  else if (candidate.hasAnchorByte)
    witness.owner = llvm::formatv("anchor@{0}", candidate.anchorByte).str();
  else
    witness.owner =
        llvm::formatv("range=[{0},{1})", candidate.begin, candidate.end).str();

  witness.selector = role.str();
  witness.sourceFamily =
      toString(candidate.proofSummary.inventory.currentPath).str();
  witness.candidateKind = toString(candidate.kind).str();
  witness.theoremClass = toString(candidate.proofSummary.theoremClass).str();

  witness.detail = llvm::formatv("role={0} kind={1} path={2} theorem={3}", role,
                                 candidate.kind,
                                 candidate.proofSummary.inventory.currentPath,
                                 candidate.proofSummary.theoremClass)
                       .str();
  if (candidate.hasLineControlObserverWitness) {
    const LineControlObserverWitness &line =
        candidate.lineControlObserverWitness;
    witness.detail +=
        llvm::formatv(" line_control=line:{0}/file:{1}/filename:{2}/"
                      "events:{3}/builtin:{4}",
                      line.observesLineNumber ? 1 : 0,
                      line.observesFileState ? 1 : 0,
                      line.observesFileName ? 1 : 0, line.lineControlEventCount,
                      line.builtinLocationObservationCount)
            .str();
  }
  if (candidate.hasCounterStateWitness) {
    const CounterStateWitness &counter = candidate.counterStateWitness;
    witness.detail +=
        llvm::formatv(
            " counter=consumes:{0}/order:{1}/suffix:{2}/"
            "expected_values:{3}/missing_values:{4}",
            counter.counterConsumptionCount, counter.counterOrderKnown ? 1 : 0,
            counter.suffixStateStable ? 1 : 0, counter.expectedBValueCount,
            counter.missingExpectedBValueCount)
            .str();
  }
  witness.key = deps_.equivalenceKeyBuilder.Build(candidate);
  witness.cost = BuildWitnessCanonicalCost(candidate);
  if (candidate.hasPayloadPreview)
    witness.payloadPreview = candidate.payloadPreview;
  return witness;
}

::clang::refold::WitnessFallbackClass
RefoldWitnessResolver::ClassifyIncompleteWitnessKeys(
    ArrayRef<std::pair<size_t, RefoldWitness>> witnesses) {
  using FallbackClass = WitnessFallbackClass;

  bool unknownTargetPP = false;
  bool unknownSuffix = false;
  bool unknownObservers = false;
  bool unknownCounter = false;
  bool unknownProducer = false;
  bool unknownBoundary = false;
  bool unknownDiagnostics = false;
  bool unknownComposition = false;

  for (const std::pair<size_t, RefoldWitness> &entry : witnesses) {
    const WitnessEquivalenceKey &key = entry.second.key;
    unknownTargetPP |= !key.targetPPTokens.known;
    unknownSuffix |= !key.suffixState.known;
    unknownObservers |= !key.preservedObservers.known;
    unknownCounter |= !key.counterState.known;
    unknownProducer |= !key.producerKinds.known;
    unknownBoundary |= key.boundaryClass == WitnessBoundaryClass::Unknown;
    unknownDiagnostics |=
        key.diagnosticClass == WitnessDiagnosticClass::Unknown;
    unknownComposition |=
        key.compositionClass == WitnessCompositionClass::Unknown;
  }

  // Prefer the first failed theorem obligation that prevents comparing witness
  // classes.  This ordering is conservative: it does not assert a mismatch; it
  // states which proof dimension is still missing strongly enough to require
  // terminal classification instead of resolver authority.
  if (unknownBoundary)
    return FallbackClass::NoOwnerClosedWitness;
  if (unknownTargetPP)
    return FallbackClass::UnknownTargetPreprocessedTokens;
  if (unknownProducer)
    return FallbackClass::UnprovenProducerKind;
  if (unknownSuffix)
    return FallbackClass::UnknownSuffixState;
  if (unknownObservers)
    return FallbackClass::LineControlObserverMismatch;
  if (unknownCounter)
    return FallbackClass::CounterStateMismatch;
  if (unknownComposition)
    return FallbackClass::CompositionFailure;
  if (unknownDiagnostics)
    return FallbackClass::ValidationFailure;
  return FallbackClass::IncompleteWitnessKey;
}

namespace {

/// Classify a witness owner string into a coarse owner-kind label.  Used by
/// the closure-ledger builder to attribute a missing-proof entry to the kind
/// of owner that produced the candidate (macro, include, anchor, etc.).
std::string ownerKindForWitness(const RefoldWitness &witness) {
  StringRef owner(witness.owner);
  if (owner.starts_with("macro#"))
    return std::string("macro");
  if (owner.starts_with("include#"))
    return std::string("include");
  if (owner.starts_with("anchor@"))
    return std::string("anchor");
  if (owner.starts_with("range="))
    return std::string("token_range");
  if (owner.empty())
    return std::string("unknown");
  return std::string("other");
}

/// Append one closure-ledger entry attributing a missing-proof dimension to
/// a specific witness.  Closure-ledger entries are the resolver's audit trail
/// for `PotentiallyInDomainMissingProof` decisions: each row names which
/// witness, which dimension, and which obligation could not be discharged.
void addClosureLedgerEntry(WitnessResolverDecision &decision,
                           const RefoldWitness &witness, size_t candidateIndex,
                           StringRef missingDimension, StringRef missingReason,
                           WitnessStrictDomainObligation obligation) {
  WitnessClosureLedgerEntry entry;
  entry.selector = decision.role;
  entry.testRegion =
      witness.owner.empty() ? std::string("<none>") : witness.owner;
  entry.witnessId = witness.witnessId;
  entry.candidateIndex = candidateIndex;
  entry.family = witness.family;
  entry.sourceFamily = witness.sourceFamily.empty() ? std::string("<none>")
                                                    : witness.sourceFamily;
  entry.candidateKind = witness.candidateKind.empty() ? std::string("<none>")
                                                      : witness.candidateKind;
  entry.theoremClass = witness.theoremClass.empty() ? std::string("<none>")
                                                    : witness.theoremClass;
  entry.sourceOwnerKind = ownerKindForWitness(witness);
  entry.owner = witness.owner.empty() ? std::string("<none>") : witness.owner;
  entry.missingDimension = missingDimension.str();
  entry.missingReason =
      missingReason.empty() ? std::string("<none>") : missingReason.str();
  entry.obligation = obligation;
  entry.fallbackClass = decision.fallbackClass;
  entry.resolverReason = decision.failureReason.empty()
                             ? std::string("<none>")
                             : decision.failureReason;
  decision.closureLedger.push_back(std::move(entry));
}

/// Emit a closure-ledger entry for every key dimension of `witness` that is
/// not yet resolver-known.  One witness can contribute multiple rows if it is
/// missing several proof dimensions simultaneously.
void addClosureLedgerEntriesForUnknownKeyDimensions(
    WitnessResolverDecision &decision, const RefoldWitness &witness,
    size_t candidateIndex) {
  using Obligation = WitnessStrictDomainObligation;
  const WitnessEquivalenceKey &key = witness.key;
  if (!key.targetPPTokens.known)
    addClosureLedgerEntry(decision, witness, candidateIndex, "target_pp",
                          key.targetPPTokens.unknownReason,
                          Obligation::TargetPreprocessedTokens);
  if (!key.suffixState.known)
    addClosureLedgerEntry(decision, witness, candidateIndex, "suffix_state",
                          key.suffixState.unknownReason,
                          Obligation::StateEquivalence);
  if (!key.preservedObservers.known)
    addClosureLedgerEntry(decision, witness, candidateIndex, "observers",
                          key.preservedObservers.unknownReason,
                          Obligation::LineControlObserverEquivalence);
  if (!key.counterState.known)
    addClosureLedgerEntry(decision, witness, candidateIndex, "counter",
                          key.counterState.unknownReason,
                          Obligation::CounterEquivalence);
  if (!key.producerKinds.known)
    addClosureLedgerEntry(decision, witness, candidateIndex, "producer",
                          key.producerKinds.unknownReason,
                          Obligation::ProducerProvenSourceWitness);
  if (key.boundaryClass == WitnessBoundaryClass::Unknown)
    addClosureLedgerEntry(decision, witness, candidateIndex, "owner_closure",
                          "boundary-class-unknown", Obligation::OwnerClosure);
  if (key.diagnosticClass == WitnessDiagnosticClass::Unknown)
    addClosureLedgerEntry(decision, witness, candidateIndex, "diagnostics",
                          "diagnostic-class-unknown",
                          Obligation::FinalValidation);
  if (key.compositionClass == WitnessCompositionClass::Unknown)
    addClosureLedgerEntry(decision, witness, candidateIndex, "composition",
                          "composition-class-unknown", Obligation::Composition);
}

/// True when a whole-cover owner-realization candidate is a materialization
/// fallback for the same macro root, not a stronger source repair, because a
/// converted invocation-preserving witness already proves the identical target
/// token envelope for that root.  Dropping only the dominated realization
/// prevents an unconverted raw-expansion certificate from manufacturing a
/// second macro-selection class while keeping owner-realization-only selectors
/// probe-only until that proof family is explicitly converted.
bool convertedInvocationRepairDominatesOwnerRealization(
    const RefoldWitness &candidate, llvm::StringRef role,
    llvm::ArrayRef<std::pair<size_t, RefoldWitness>> selectableWitnesses) {
  if (role != "SelectPreferredMacroSelectionCandidate")
    return false;
  if (candidate.family != WitnessProofFamily::OwnerRealization)
    return false;
  if (candidate.sourceFamily != "MacroWholeCoverRealization")
    return false;
  if (candidate.key.HasUnknownDimensions())
    return false;
  if (!candidate.key.targetPPTokens.known)
    return false;
  if (candidate.key.boundaryClass != WitnessBoundaryClass::RootInvocation)
    return false;
  if (candidate.key.diagnosticClass !=
      WitnessDiagnosticClass::RealizesEditedSurface)
    return false;

  for (const std::pair<size_t, RefoldWitness> &entry : selectableWitnesses) {
    const RefoldWitness &other = entry.second;
    if (&other == &candidate)
      continue;
    if (!RefoldWitnessResolver::IsResolverAuthoritativeWitness(other))
      continue;
    if (other.key.HasUnknownDimensions())
      continue;
    if (!other.key.targetPPTokens.known)
      continue;
    if (other.owner != candidate.owner)
      continue;
    if (other.key.targetPPTokens.value != candidate.key.targetPPTokens.value)
      continue;
    if (other.key.boundaryClass != WitnessBoundaryClass::RootInvocation)
      continue;
    if (other.key.diagnosticClass !=
        WitnessDiagnosticClass::PreservesDiagnostics)
      continue;
    return true;
  }

  return false;
}

} // namespace

void RefoldWitnessResolver::PopulateWitnessClosureLedger(
    WitnessResolverDecision &decision,
    ArrayRef<std::pair<size_t, RefoldWitness>> selectableWitnesses) {
  using DomainClass = WitnessStrictDomainClass;
  using Obligation = WitnessStrictDomainObligation;

  decision.closureLedger.clear();
  if (decision.strictDomain.domainClass !=
      DomainClass::PotentiallyInDomainMissingProof)
    return;

  for (const std::pair<size_t, RefoldWitness> &entry : selectableWitnesses) {
    const RefoldWitness &witness = entry.second;
    if (witness.key.HasUnknownDimensions())
      addClosureLedgerEntriesForUnknownKeyDimensions(decision, witness,
                                                     entry.first);
  }

  if (decision.closureLedger.empty() &&
      decision.fallbackClass == WitnessFallbackClass::UnconvertedProofFamily) {
    for (const std::pair<size_t, RefoldWitness> &entry : selectableWitnesses) {
      if (!IsResolverAuthoritativeWitness(entry.second))
        addClosureLedgerEntry(decision, entry.second, entry.first,
                              "converted_family",
                              "proof-family-not-resolver-authoritative",
                              Obligation::ConvertedProofFamily);
    }
  }

  if (decision.closureLedger.empty() &&
      decision.fallbackClass == WitnessFallbackClass::CompositionFailure) {
    const std::string reason = decision.composition.reason.empty()
                                   ? std::string("composition-not-proven")
                                   : decision.composition.reason;
    for (const std::pair<size_t, RefoldWitness> &entry : selectableWitnesses)
      addClosureLedgerEntry(decision, entry.second, entry.first, "composition",
                            reason, Obligation::Composition);
  }

  if (decision.closureLedger.empty() && selectableWitnesses.empty()) {
    RefoldWitness synthetic;
    synthetic.witnessId = 0;
    synthetic.family = WitnessProofFamily::Unknown;
    synthetic.owner.clear();
    synthetic.selector = decision.role;
    synthetic.sourceFamily = "<none>";
    synthetic.candidateKind = "<none>";
    synthetic.theoremClass = "<none>";
    addClosureLedgerEntry(decision, synthetic, 0, "producer",
                          decision.failureReason.empty()
                              ? StringRef("no-selectable-witness")
                              : StringRef(decision.failureReason),
                          Obligation::ProducerProvenSourceWitness);
  }

  if (decision.closureLedger.empty()) {
    for (const std::pair<size_t, RefoldWitness> &entry : selectableWitnesses)
      addClosureLedgerEntry(decision, entry.second, entry.first, "unknown",
                            decision.failureReason.empty()
                                ? StringRef("missing-proof")
                                : StringRef(decision.failureReason),
                            decision.strictDomain.obligation);
  }
}

::clang::refold::WitnessCompositionDecision
RefoldWitnessResolver::ResolveWitnessComposition(
    llvm::StringRef role,
    ArrayRef<std::pair<size_t, RefoldWitness>> selectableWitnesses,
    bool hasSingleConcreteRepairIdentity) const {
  WitnessCompositionDecision decision;
  decision.computed = true;
  decision.candidateTupleCount = selectableWitnesses.size();

  if (selectableWitnesses.empty()) {
    decision.reason = "no-selectable-composition-tuples";
    deps_.witnessTrace.TraceWitnessCompositionDecision(role, decision);
    return decision;
  }

  std::map<std::string, uint64_t> globalClasses;

  for (const std::pair<size_t, RefoldWitness> &entry : selectableWitnesses) {
    const RefoldWitness &witness = entry.second;
    const WitnessEquivalenceKey &key = witness.key;

    if (key.HasUnknownDimensions()) {
      ++decision.incompleteTupleCount;
      continue;
    }

    if (key.compositionClass == WitnessCompositionClass::Unknown) {
      ++decision.incompleteTupleCount;
      continue;
    }

    if (key.compositionClass == WitnessCompositionClass::Terminal) {
      ++decision.incompatibleTupleCount;
      continue;
    }

    ++decision.completeTupleCount;

    // composition is intentionally tuple-level rather than
    // source-spelling based.  A candidate may be a one-tile local repair or a
    // durable mixed-owner tiling; in both cases the global composition class is
    // the ordered target stream plus the suffix/observer/counter state that the
    // tuple leaves for its neighbors.
    std::string classKey =
        llvm::formatv(
            "target={0}|suffix={1}|observers={2}|counter={3}|boundary={4}|"
            "diagnostics={5}|composition={6}|producers={7}",
            key.targetPPTokens.value, key.suffixState.value,
            key.preservedObservers.value, key.counterState.value,
            key.boundaryClass, key.diagnosticClass, key.compositionClass,
            key.producerKinds)
            .str();
    ++globalClasses[classKey];
  }

  decision.globalClassCount = globalClasses.size();

  if (decision.incompleteTupleCount != 0) {
    decision.reason = "composition-incomplete-witness-key";
  } else if (decision.incompatibleTupleCount != 0) {
    decision.reason = "composition-incompatible-terminal-tuple";
    decision.failureIsFatal = true;
  } else if (decision.globalClassCount == 1) {
    decision.compatible = true;
    decision.reason = "single-global-composition-class";
  } else if (hasSingleConcreteRepairIdentity) {
    decision.compatible = true;
    decision.reason = "single-source-repair-multiple-composition-classes";
  } else {
    decision.reason = "multiple-non-equivalent-composition-classes";
    decision.failureIsFatal = true;
  }

  deps_.witnessTrace.TraceWitnessCompositionDecision(role, decision);
  return decision;
}

::clang::refold::WitnessResolverDecision
RefoldWitnessResolver::ResolveWitnessesForSelection(
    llvm::StringRef role, size_t candidateCount,
    llvm::function_ref<bool(size_t)> isSelectable,
    llvm::function_ref<RefoldWitness(size_t)> buildWitness,
    llvm::function_ref<bool(size_t, size_t)> canonicalPrefers,
    std::optional<size_t> legacyIndex) const {
  WitnessResolverDecision decision;
  decision.role = role.str();
  decision.mode = deps_.witnessTrace.GetWitnessResolverMode();
  decision.candidateCount = candidateCount;
  decision.legacyIndex = legacyIndex;

  const bool shouldCompute = deps_.witnessTrace.ShouldEmitProofLog() ||
                             decision.mode != WitnessResolverMode::Off;
  if (!shouldCompute)
    return decision;

  decision.resolverComputed = true;

  std::map<std::string, SmallVector<size_t, 4>> classes;
  SmallVector<size_t, 8> selectableIndices;
  SmallVector<std::pair<size_t, RefoldWitness>, 8> selectableWitnesses;

  // Resolves witness *classes*, but a selector can legitimately carry several
  // complete proof certificates for the same concrete source repair. For
  // example, a macro invocation containing both # and ## may be certified by a
  // stringification witness and by a paste witness; those proof keys must stay
  // distinct, but strict mode must not interpret the duplicate certificate as a
  // request to abandon the macro-preserving repair.  Track a conservative
  // source repair identity separately from the semantic proof key so
  // multi-class proof-certificate ambiguity does not become source-repair
  // ambiguity.
  std::optional<std::string> commonRepairIdentity;
  bool hasConcreteRepairIdentity = false;
  bool singleConcreteRepairIdentity = true;

  auto repairIdentityForWitness = [](const RefoldWitness &witness) {
    if (witness.payloadPreview.empty())
      return std::string();

    return llvm::formatv("owner={0}|source_order={1}|range_bytes={2}|"
                         "arg_boundary={3}|owner_boundary={4}|payload={5}",
                         witness.owner, witness.cost.sourceOrder,
                         witness.cost.sourceRangeBytes,
                         witness.cost.argumentBoundaryChangePenalty,
                         witness.cost.ownerBoundaryChangePenalty,
                         witness.payloadPreview)
        .str();
  };

  for (size_t i = 0; i < candidateCount; ++i) {
    RefoldWitness witness = buildWitness(i);
    const bool selectable = isSelectable(i);
    if (!selectable) {
      ++decision.proofInvalidCount;
      deps_.witnessTrace.TraceWitnessRejected(
          witness, WitnessRejectReason::NotSelectable,
          "central witness resolver rejected non-selectable "
          "candidate");
      continue;
    }

    ++decision.selectableCount;
    selectableIndices.push_back(i);
    selectableWitnesses.push_back(std::make_pair(i, witness));
    if (witness.key.HasUnknownDimensions())
      ++decision.incompleteWitnessCount;
    else
      ++decision.completeWitnessCount;

    if (IsResolverAuthoritativeWitness(witness))
      ++decision.resolverAuthoritativeWitnessCount;
    else
      ++decision.resolverUnconvertedWitnessCount;

    classes[witness.key.PartitionString(witness.witnessId)].push_back(i);

    const std::string repairIdentity = repairIdentityForWitness(witness);
    if (repairIdentity.empty()) {
      singleConcreteRepairIdentity = false;
    } else if (!hasConcreteRepairIdentity) {
      commonRepairIdentity = repairIdentity;
      hasConcreteRepairIdentity = true;
    } else if (*commonRepairIdentity != repairIdentity) {
      singleConcreteRepairIdentity = false;
    }
  }

  SmallVector<std::pair<size_t, RefoldWitness>, 8> effectiveWitnesses;
  SmallVector<size_t, 8> effectiveIndices;
  bool removedDominatedOwnerRealization = false;
  for (const std::pair<size_t, RefoldWitness> &entry : selectableWitnesses) {
    if (convertedInvocationRepairDominatesOwnerRealization(
            entry.second, role, selectableWitnesses)) {
      removedDominatedOwnerRealization = true;
      ++decision.proofInvalidCount;
      deps_.witnessTrace.TraceWitnessRejected(
          entry.second, WitnessRejectReason::NonEquivalentAmbiguity,
          "whole-cover owner realization is dominated by a converted "
          "invocation-preserving witness for the same root and target tokens");
      continue;
    }

    effectiveWitnesses.push_back(entry);
    effectiveIndices.push_back(entry.first);
  }

  if (removedDominatedOwnerRealization) {
    selectableWitnesses = effectiveWitnesses;
    selectableIndices = effectiveIndices;
    decision.selectableCount = selectableWitnesses.size();
    decision.equivalenceClassCount = 0;
    decision.completeWitnessCount = 0;
    decision.incompleteWitnessCount = 0;
    decision.resolverAuthoritativeWitnessCount = 0;
    decision.resolverUnconvertedWitnessCount = 0;
    classes.clear();

    hasConcreteRepairIdentity = false;
    singleConcreteRepairIdentity = true;
    commonRepairIdentity.reset();

    for (const std::pair<size_t, RefoldWitness> &entry : selectableWitnesses) {
      const RefoldWitness &witness = entry.second;
      if (witness.key.HasUnknownDimensions())
        ++decision.incompleteWitnessCount;
      else
        ++decision.completeWitnessCount;

      if (IsResolverAuthoritativeWitness(witness))
        ++decision.resolverAuthoritativeWitnessCount;
      else
        ++decision.resolverUnconvertedWitnessCount;

      classes[witness.key.PartitionString(witness.witnessId)].push_back(
          entry.first);

      const std::string repairIdentity = repairIdentityForWitness(witness);
      if (repairIdentity.empty()) {
        singleConcreteRepairIdentity = false;
      } else if (!hasConcreteRepairIdentity) {
        commonRepairIdentity = repairIdentity;
        hasConcreteRepairIdentity = true;
      } else if (*commonRepairIdentity != repairIdentity) {
        singleConcreteRepairIdentity = false;
      }
    }
  }

  decision.equivalenceClassCount = classes.size();

  decision.composition = ResolveWitnessComposition(
      role, selectableWitnesses,
      hasConcreteRepairIdentity && singleConcreteRepairIdentity);

  deps_.witnessTrace.TraceWitnessSelectionProbe(
      role, candidateCount, decision.selectableCount,
      decision.proofInvalidCount, decision.equivalenceClassCount,
      decision.completeWitnessCount, decision.incompleteWitnessCount);
  deps_.witnessTrace.TraceWitnessAmbiguity(role, candidateCount,
                                           decision.selectableCount,
                                           decision.equivalenceClassCount);

  if (decision.selectableCount == 0) {
    decision.failureReason = "no-selectable-witness";
    decision.fallbackClass = WitnessFallbackClass::NoSelectableWitness;
  } else if (decision.incompleteWitnessCount != 0) {
    // Unknown dimensions do not establish equivalence.  Strict mode therefore
    // may reuse only the caller-supplied compatibility index for this selector;
    // the proof family itself has not yet produced resolver-authoritative keys.
    decision.failureReason = "incomplete-witness-key";
    decision.fallbackClass = ClassifyIncompleteWitnessKeys(selectableWitnesses);
  } else if (decision.resolverUnconvertedWitnessCount != 0) {
    // A complete key is necessary but not sufficient: the witness must also be
    // accepted by the resolver-authority gate so strict mode cannot become
    // authoritative for an unaudited proof family merely because its trace key
    // happened to be complete.
    decision.failureReason = "unconverted-proof-family";
    decision.fallbackClass = WitnessFallbackClass::UnconvertedProofFamily;
  } else if (!decision.composition.compatible) {
    // Tuple-level composition is an additional authority boundary. Unknown
    // composition facts may reuse only the caller-supplied compatibility index;
    // a complete converted selector that proves multiple non-equivalent global
    // tuples fails closed.
    decision.failureReason = decision.composition.reason.empty()
                                 ? "composition-not-proven"
                                 : decision.composition.reason;
    decision.strictFailClosed = decision.composition.failureIsFatal;
    decision.fallbackClass = WitnessFallbackClass::CompositionFailure;
  } else if (decision.equivalenceClassCount != 1) {
    decision.resolverImplemented = true;

    if (hasConcreteRepairIdentity && singleConcreteRepairIdentity) {
      // Multiple complete proof keys can certify the same emitted source edit.
      // That is proof-certificate ambiguity, not source-repair ambiguity. Keep
      // the semantic classes distinct for tracing, but allow strict mode to use
      // the caller-supplied compatibility index for the concrete repair until
      // proof normalization can compose proof certificates directly.
      decision.strictUseResolver = true;
      decision.failureReason = "single-source-repair-multiple-proof-classes";
      decision.fallbackClass = WitnessFallbackClass::Unknown;
      if (legacyIndex) {
        decision.resolverIndex = legacyIndex;
      } else {
        for (size_t idx : selectableIndices) {
          if (!decision.resolverIndex ||
              canonicalPrefers(idx, *decision.resolverIndex))
            decision.resolverIndex = idx;
        }
      }
    } else {
      // All selectable candidates have complete keys, and they describe
      // different concrete source repairs.  This is the real fail-closed case
      // for converted selector families.
      decision.strictFailClosed = true;
      decision.failureReason = "multiple-non-equivalent-classes";
      decision.fallbackClass =
          WitnessFallbackClass::MultipleNonEquivalentWitnessClasses;
    }
  } else {
    decision.resolverImplemented = true;
    decision.strictUseResolver = true;
    decision.failureReason = "single-equivalence-class";
    decision.fallbackClass = WitnessFallbackClass::Unknown;

    for (size_t idx : selectableIndices) {
      if (!decision.resolverIndex ||
          canonicalPrefers(idx, *decision.resolverIndex))
        decision.resolverIndex = idx;
    }
  }

  if (decision.fallbackClass == WitnessFallbackClass::Unknown &&
      !decision.failureReason.empty())
    decision.fallbackClass = classifyResolverFallbackReason(
        decision.failureReason, decision.composition);

  if (decision.legacyIndex && decision.resolverIndex)
    decision.agreement =
        (*decision.legacyIndex == *decision.resolverIndex) ? "agree" : "differ";
  else if (decision.legacyIndex && !decision.resolverIndex)
    decision.agreement = "legacy-only";
  else if (!decision.legacyIndex && decision.resolverIndex)
    decision.agreement = "resolver-only";
  else
    decision.agreement = "neither";

  decision.strictDomain = classifyStrictDomainForResolver(decision);
  PopulateWitnessClosureLedger(decision, selectableWitnesses);
  deps_.theoremAudit.RecordWitnessResolverTheoremAudit(decision);

  deps_.witnessTrace.TraceWitnessResolverDecision(decision);
  return decision;
}

} // namespace refold
} // namespace clang
