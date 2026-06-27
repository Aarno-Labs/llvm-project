//===--- RefoldProofLattice.h ----------------------------------*- C++ -*-===//
//
// Accepted-result proof lattice boundary for clang-refold.
//
// This service owns proof-summary normalization, accepted-result ranking,
// witness resolver classification, and theorem/audit carrier construction.
// Callers use RefoldEngine::ProofLattice() rather than RefoldEngine
// forwarding wrappers. Proof/witness, accepted-result, and edit carriers live
// in focused namespace-level headers.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDPROOFLATTICE_H
#define LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDPROOFLATTICE_H

#include "core/RefoldLog.h"
#include "edit/RefoldEditTypes.h"
#include "edit/RefoldPatchTypes.h"
#include "line-control/RefoldLineControlProof.h"
#include "macro/RefoldMacroTopology.h"
#include "proof/RefoldAcceptedResultTypes.h"
#include "proof/RefoldOwnerStateTypes.h"
#include "proof/RefoldProofTypes.h"
#include "proof/RefoldTerminalProofSink.h"
#include "proof/RefoldWitnessTypes.h"
#include "source/DiffAlgorithms.h"
#include "source/RefoldSourceMapper.h"
#include "source/RefoldToken.h"
#include "source/RefoldTokenTextAnalysis.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/STLFunctionalExtras.h"
#include "llvm/ADT/StringRef.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace clang {
namespace refold {

class RefoldArgTextRecovery;
class RefoldOwnerStateProof;
class RefoldTUEditPlanner;
class RefoldTheoremAudit;

class RefoldProofLattice {
  // Proof/witness/accepted-result carriers now live at namespace scope.
  using LegacyPathKind = ::clang::refold::LegacyPathKind;
  using LegacyPathDefinition = ::clang::refold::LegacyPathDefinition;
  using LegacyAuditEvidence = ::clang::refold::LegacyAuditEvidence;
  using WitnessProofFamily = ::clang::refold::WitnessProofFamily;
  using WitnessProducerKind = ::clang::refold::WitnessProducerKind;
  using WitnessBoundaryClass = ::clang::refold::WitnessBoundaryClass;
  using WitnessDiagnosticClass = ::clang::refold::WitnessDiagnosticClass;
  using WitnessCompositionClass = ::clang::refold::WitnessCompositionClass;
  using WitnessRejectReason = ::clang::refold::WitnessRejectReason;
  using WitnessFallbackClass = ::clang::refold::WitnessFallbackClass;
  using WitnessStrictDomainClass = ::clang::refold::WitnessStrictDomainClass;
  using WitnessStrictDomainObligation = ::clang::refold::WitnessStrictDomainObligation;
  using WitnessStrictDomainDecision = ::clang::refold::WitnessStrictDomainDecision;
  using WitnessEquivalenceDimension = ::clang::refold::WitnessEquivalenceDimension;
  using WitnessProducerKindSet = ::clang::refold::WitnessProducerKindSet;
  using WitnessEquivalenceKey = ::clang::refold::WitnessEquivalenceKey;
  using WitnessCanonicalCost = ::clang::refold::WitnessCanonicalCost;
  using RefoldWitness = ::clang::refold::RefoldWitness;
  using WitnessAmbiguityClass = ::clang::refold::WitnessAmbiguityClass;
  using WitnessCompositionDecision = ::clang::refold::WitnessCompositionDecision;
  using WitnessClosureLedgerEntry = ::clang::refold::WitnessClosureLedgerEntry;
  using WitnessResolverMode = ::clang::refold::WitnessResolverMode;
  using WitnessResolverDecision = ::clang::refold::WitnessResolverDecision;
  using RefoldStats = ::clang::refold::RefoldStats;
  using TheoremAuditStats = ::clang::refold::TheoremAuditStats;
  using MacroPatch = ::clang::refold::MacroPatch;
  using TextEdit = ::clang::refold::TextEdit;
  using MixedOwnerTilingWitness = ::clang::refold::MixedOwnerTilingWitness;
  using MixedOwnerTilingSegmentBinding = ::clang::refold::MixedOwnerTilingSegmentBinding;
  using BInsertionClaim = ::clang::refold::BInsertionClaim;
  using BInsertionProv = ::clang::refold::BInsertionProv;
  using MacroStateObservationKind = ::clang::refold::MacroStateObservationKind;
  using MacroStateDirectiveLineInterval = ::clang::refold::MacroStateDirectiveLineInterval;
  using MacroDefinitionReplacementListInterval = ::clang::refold::MacroDefinitionReplacementListInterval;
  using PendingResync = ::clang::refold::PendingResync;
  using ResyncOutcome = ::clang::refold::ResyncOutcome;
  using EmissionPathKind = ::clang::refold::EmissionPathKind;
  using EmissionPathInventory = ::clang::refold::EmissionPathInventory;
  using AcceptedResultCandidate = ::clang::refold::AcceptedResultCandidate;
  using LineControlWrappedText = ::clang::refold::LineControlWrappedText;
  using IncludeTextEditPlan = ::clang::refold::IncludeTextEditPlan;
  using PasteArgEdit = ::clang::refold::PasteArgEdit;
  using OwnerKind = ::clang::refold::OwnerKind;
  using Owner = ::clang::refold::Owner;
  using OwnerTokenRange = ::clang::refold::OwnerTokenRange;
  using OwnerSourceRange = ::clang::refold::OwnerSourceRange;
  using OwnerObserverSummary = ::clang::refold::OwnerObserverSummary;
  using MacroStateIdentity = ::clang::refold::MacroStateIdentity;
  using MacroObservationKind = ::clang::refold::MacroObservationKind;
  using MacroStateObservation = ::clang::refold::MacroStateObservation;
  using LineDirectiveOperandProvenance = ::clang::refold::LineDirectiveOperandProvenance;
  using LineControlStateIdentity = ::clang::refold::LineControlStateIdentity;
  using BuiltinLocationObservationKind = ::clang::refold::BuiltinLocationObservationKind;
  using BuiltinLocationObservation = ::clang::refold::BuiltinLocationObservation;
  using CounterEventIdentity = ::clang::refold::CounterEventIdentity;
  using IncludeStateIdentity = ::clang::refold::IncludeStateIdentity;
  using IncludeGuardObservationKind = ::clang::refold::IncludeGuardObservationKind;
  using IncludeGuardStateIdentity = ::clang::refold::IncludeGuardStateIdentity;
  using PragmaStateClassification = ::clang::refold::PragmaStateClassification;
  using PragmaStateIdentity = ::clang::refold::PragmaStateIdentity;
  using ConditionalStateRole = ::clang::refold::ConditionalStateRole;
  using ConditionalStateIdentity = ::clang::refold::ConditionalStateIdentity;
  using MissingStateFactKind = ::clang::refold::MissingStateFactKind;
  using MissingStateFact = ::clang::refold::MissingStateFact;
  using OwnerStateFacts = ::clang::refold::OwnerStateFacts;
  using StateRequirements = ::clang::refold::StateRequirements;
  using StateObservations = ::clang::refold::StateObservations;
  using StateMutations = ::clang::refold::StateMutations;
  using StateGuarantees = ::clang::refold::StateGuarantees;
  using OwnerStateDelta = ::clang::refold::OwnerStateDelta;
  using OwnerClosure = ::clang::refold::OwnerClosure;
  using OwnerStateComponent = ::clang::refold::OwnerStateComponent;
  using OwnerStateBoundary = ::clang::refold::OwnerStateBoundary;
  using OwnerStateGraphNodeKind = ::clang::refold::OwnerStateGraphNodeKind;
  using OwnerStateGraphNode = ::clang::refold::OwnerStateGraphNode;
  using SuffixObservationKind = ::clang::refold::SuffixObservationKind;
  using SuffixOrderingProofKind = ::clang::refold::SuffixOrderingProofKind;
  using SuffixStateObserverSite = ::clang::refold::SuffixStateObserverSite;
  using SuffixObserverResult = ::clang::refold::SuffixObserverResult;
  using OwnerStateGraphObserverIndex = ::clang::refold::OwnerStateGraphObserverIndex;
  using OwnerStateGraphAuditStats = ::clang::refold::OwnerStateGraphAuditStats;
  using OwnerStateGraph = ::clang::refold::OwnerStateGraph;
  using OwnerStateFactIndex = ::clang::refold::OwnerStateFactIndex;
  using SuffixObserverQueryResult = ::clang::refold::SuffixObserverQueryResult;
  using StateMutationKind = ::clang::refold::StateMutationKind;
  using DirectStateCheckKind = ::clang::refold::DirectStateCheckKind;
  using DirectStateCheckClosureKind = ::clang::refold::DirectStateCheckClosureKind;
  using DirectiveClosureStatus = ::clang::refold::DirectiveClosureStatus;
  using SuffixUnobservedWitness = ::clang::refold::SuffixUnobservedWitness;
  using StateRepairWitness = ::clang::refold::StateRepairWitness;
  using OwnerMaterializationWitness = ::clang::refold::OwnerMaterializationWitness;
  using ClosureWideningWitness = ::clang::refold::ClosureWideningWitness;
  using LiteralizationWitness = ::clang::refold::LiteralizationWitness;
  using TerminalStateFailureWitness = ::clang::refold::TerminalStateFailureWitness;
  using SuffixStabilityWitnessKind = ::clang::refold::SuffixStabilityWitnessKind;
  using SuffixStabilityWitness = ::clang::refold::SuffixStabilityWitness;
  using StateTransitionProof = ::clang::refold::StateTransitionProof;
  using StateTransitionGatewayRequest = ::clang::refold::StateTransitionGatewayRequest;
  using TheoremProofClass = ::clang::refold::TheoremProofClass;
  using AcceptedProofClass = ::clang::refold::AcceptedProofClass;
  using RealizationMode = ::clang::refold::RealizationMode;
  using SelectionPreference = ::clang::refold::SelectionPreference;
  using SurfaceDisposition = ::clang::refold::SurfaceDisposition;
  using TheoremSelectionTieBreakerKind = ::clang::refold::TheoremSelectionTieBreakerKind;
  using AcceptedPathKind = ::clang::refold::AcceptedPathKind;
  using ExpansionFallbackBranchProofClass = ::clang::refold::ExpansionFallbackBranchProofClass;
  using ExpansionFallbackBranchKind = ::clang::refold::ExpansionFallbackBranchKind;
  using ExpansionFallbackBranchClassification = ::clang::refold::ExpansionFallbackBranchClassification;
  using AcceptanceSupportKind = ::clang::refold::AcceptanceSupportKind;
  using FutureProofTarget = ::clang::refold::FutureProofTarget;
  using AcceptancePathInventory = ::clang::refold::AcceptancePathInventory;
  using LatticeConflictDomain = ::clang::refold::LatticeConflictDomain;
  using LatticeMergeLaw = ::clang::refold::LatticeMergeLaw;
  using LatticeConflictLaw = ::clang::refold::LatticeConflictLaw;
  using GlobalSelectionLattice = ::clang::refold::GlobalSelectionLattice;
  using CompletenessCoverageKind = ::clang::refold::CompletenessCoverageKind;
  using CompletenessExpectationKind = ::clang::refold::CompletenessExpectationKind;
  using CompletenessContract = ::clang::refold::CompletenessContract;
  using TheoremDomainKind = ::clang::refold::TheoremDomainKind;
  using TheoremDomainContract = ::clang::refold::TheoremDomainContract;
  using TUAnchorEvidenceKind = ::clang::refold::TUAnchorEvidenceKind;
  using TUAnchorWitness = ::clang::refold::TUAnchorWitness;
  using IncludeAnchorEvidenceKind = ::clang::refold::IncludeAnchorEvidenceKind;
  using IncludeAnchorWitness = ::clang::refold::IncludeAnchorWitness;
  using IncludeRealizationEvidenceKind = ::clang::refold::IncludeRealizationEvidenceKind;
  using IncludeRealizationBTokenEnvelope = ::clang::refold::IncludeRealizationBTokenEnvelope;
  using OwnerRealizationEvidenceKind = ::clang::refold::OwnerRealizationEvidenceKind;
  using OwnerRealizationWitness = ::clang::refold::OwnerRealizationWitness;
  using OwnerRealizationResult = ::clang::refold::OwnerRealizationResult;
  using MixedOwnerTilingEdgeKind = ::clang::refold::MixedOwnerTilingEdgeKind;
  using MixedOwnerTilingSegmentWitness = ::clang::refold::MixedOwnerTilingSegmentWitness;
  using ProofDischargeStatus = ::clang::refold::ProofDischargeStatus;
  using ProofObligationKind = ::clang::refold::ProofObligationKind;
  using ProofFailureReason = ::clang::refold::ProofFailureReason;
  using ProofDischargeRecord = ::clang::refold::ProofDischargeRecord;
  using ProofDischargeAccumulator = ::clang::refold::ProofDischargeAccumulator;
  using ProofSummary = ::clang::refold::ProofSummary;
  using IncludePatch = ::clang::refold::IncludePatch;
  using EmittedProof = ::clang::refold::EmittedProof;
  using AcceptedResultCandidateKind = ::clang::refold::AcceptedResultCandidateKind;
  using LineControlObserverWitness = ::clang::refold::LineControlObserverWitness;
  using CounterStateWitness = ::clang::refold::CounterStateWitness;
  using SelectedAcceptedResultCandidate = ::clang::refold::SelectedAcceptedResultCandidate;
  using MacroSelectionCandidate = ::clang::refold::MacroSelectionCandidate;
  using SelectedMacroSelectionCandidate = ::clang::refold::SelectedMacroSelectionCandidate;
  using MacroPatchProofKind = ::clang::refold::MacroPatchProofKind;
  using PasteWitness = ::clang::refold::PasteWitness;
  using SubtreeCertificate = ::clang::refold::SubtreeCertificate;
  using CallChainWitness = ::clang::refold::CallChainWitness;
  using GeneratedCalleeReplayWitness = ::clang::refold::GeneratedCalleeReplayWitness;
  using WholeEnvelopeReplayWitness = ::clang::refold::WholeEnvelopeReplayWitness;
  using VariadicCommaWitness = ::clang::refold::VariadicCommaWitness;
  using ZeroTokenBoundaryWitness = ::clang::refold::ZeroTokenBoundaryWitness;
  using MacroPatchProof = ::clang::refold::MacroPatchProof;
  using StabilizedMaterializedHeaderMacroPatch = ::clang::refold::StabilizedMaterializedHeaderMacroPatch;
  using WholeCoverPlan = ::clang::refold::WholeCoverPlan;
  using IncludeEdits = ::clang::refold::IncludeEdits;
  using OccurrenceSupportMode = ::clang::refold::OccurrenceSupportMode;
  using ForcedMacroPatchRequest = ::clang::refold::ForcedMacroPatchRequest;
  using CounterOccurrence = ::clang::refold::CounterOccurrence;
  using LineStateObserverSite = ::clang::refold::LineStateObserverSite;
public:
  /// Exceptional callback set for proof-lattice operations that still break a
  /// construction cycle.  Whole-cover planning is owned by the macro patch
  /// planner, which itself depends on the lattice for proof classification, so
  /// this remains a late-bound query until that arbitration surface is split.
  /// Theorem/audit policy is owned by RefoldTheoremAudit.
  struct Hooks {
    std::function<std::optional<WholeCoverPlan>(
        const RefoldModel::MacroInvocation &)>
        computeWholeCoverPlan;
  };

  explicit RefoldProofLattice(
      const RefoldModel &model, llvm::StringRef bSource,
      llvm::ArrayRef<PPTok> bToks, RefoldSourceMapper &sourceMapper,
      const RefoldTokenTextAnalysis &tokenText,
      const RefoldArgTextRecovery &argTextRecovery,
      const RefoldMacroTopology &macroTopology,
      const RefoldOwnerStateProof &ownerStateProof,
      const RefoldTerminalProofSink &terminalSink,
      const RefoldTUEditPlanner &tuEdits,
      const RefoldTheoremAudit &theoremAudit,
      TheoremAuditStats &lastTheoremAudit, bool strict,
      ProofAuditMode &proofAuditMode,
      std::vector<MixedOwnerTilingSegmentBinding> &mixedOwnerTilingSegmentBindings,
      std::vector<MixedOwnerTilingWitness> &mixedOwnerTilingWitnesses,
      Hooks hooks);

  ::clang::refold::AcceptancePathInventory
  InventoryMacroPatchProofAcceptancePath(const MacroPatchProof &proof) const;

  ::clang::refold::AcceptancePathInventory
  InventoryMacroPatchAcceptancePath(const MacroPatch &patch) const;

  ::clang::refold::AcceptancePathInventory
  BuildAcceptancePathInventory(AcceptedPathKind currentPath) const;

  ::clang::refold::TheoremProofClass
  BuildTheoremProofClassForAcceptedPath(AcceptedPathKind currentPath) const;

  void
  RequireAcceptedPathBaseline(ProofDischargeAccumulator &discharge,
    const AcceptancePathInventory &inventory) const;

  ::clang::refold::ProofDischargeRecord
  BuildAcceptedPathBaselineDischarge(const AcceptancePathInventory &inventory,
                                      bool explicitOutOfDomain = false) const;

  void
  ConfigureProofSummary(ProofSummary &summary, TheoremProofClass theoremClass,
    AcceptedProofClass acceptedClass, RealizationMode realizationMode,
    SelectionPreference preference,
    SurfaceDisposition surfaceDisposition, bool structurePreserving) const;

  bool
  ProofSummaryRequiresOwnerRealizationWitness(const ProofSummary &summary) const;

  void
  FinalizeProofSummary(ProofSummary &summary) const;

  void
  RequireIncludeZeroWidthAnchor(ProofDischargeAccumulator &discharge, const IncludePatch &patch,
    const IncludeAnchorWitness *witness, IncludeAnchorEvidenceKind evidence,
    ProofObligationKind witnessObligation,
    ProofFailureReason witnessFailure) const;

  ::clang::refold::ProofSummary
  ClassifyMacroPatchProof(const MacroPatch &patch) const;

  void
  RefreshMacroPatchDerivedProofWitnesses(MacroPatch &patch) const;

  void
  SyncMacroPatchProofSummary(MacroPatch &patch) const;

  ::clang::refold::MacroPatchProof
  MakeMacroPatchProof(MacroPatchProofKind kind, bool preservesInvocationStructure,
    uint64_t proofRootMacroId) const;

  void
  SetMacroPatchProof(MacroPatch &patch,
                                      MacroPatchProof proof) const;

  void
  StampSelectedMacroPatchCandidate(MacroPatch &patch, const AcceptedResultCandidate &candidate,
    llvm::StringRef role) const;

  static ::clang::refold::WitnessProofFamily
  WitnessFamilyForAcceptedPath(AcceptedPathKind path);

  static bool
  IsResolverAuthoritativeWitnessFamily(WitnessProofFamily family);

  static bool
  IsResolverAuthoritativeWitness(const RefoldWitness &witness);

  static ::clang::refold::WitnessProducerKind
  WitnessProducerKindForAcceptedPath(AcceptedPathKind path);

  static ::clang::refold::WitnessBoundaryClass
  WitnessBoundaryClassForAcceptedCandidate(const AcceptedResultCandidate &candidate);

  static std::string
  FormatWitnessTraceHash(llvm::StringRef text);

  void
  AttachLineControlObserverWitness(AcceptedResultCandidate &candidate) const;

  void
  AttachCounterStateWitness(AcceptedResultCandidate &candidate) const;

  ::clang::refold::WitnessEquivalenceKey
  BuildWitnessEquivalenceKey(const AcceptedResultCandidate &candidate) const;

  ::clang::refold::WitnessCanonicalCost
  BuildWitnessCanonicalCost(const AcceptedResultCandidate &candidate) const;

  ::clang::refold::RefoldWitness
  BuildRefoldWitness(const AcceptedResultCandidate &candidate,
                                 llvm::StringRef role,
                                 uint64_t witnessId = 0) const;

  ::clang::refold::WitnessResolverMode
  GetWitnessResolverMode() const;

  bool
  ShouldEmitProofLog() const;

  static ::clang::refold::WitnessFallbackClass
  ClassifyTerminalFallbackFailure(const TerminalFallbackProofFailure &failure);

  static ::clang::refold::WitnessFallbackClass
  ClassifyIncompleteWitnessKeys(llvm::ArrayRef<std::pair<size_t, RefoldWitness>> witnesses);

  static void
  PopulateWitnessClosureLedger(WitnessResolverDecision &decision,
    llvm::ArrayRef<std::pair<size_t, RefoldWitness>> selectableWitnesses);

  static ::clang::refold::WitnessFallbackClass
  ClassifyResolverFallbackReason(llvm::StringRef reason, const WitnessCompositionDecision &composition);

  static ::clang::refold::WitnessStrictDomainObligation
  StrictDomainObligationForFallbackClass(WitnessFallbackClass fallbackClass);

  static ::clang::refold::WitnessStrictDomainDecision
  ClassifyStrictDomainForResolver(const WitnessResolverDecision &decision);

  static ::clang::refold::WitnessStrictDomainDecision
  ClassifyStrictDomainForTerminalFallback(const TerminalFallbackProofFailure &failure);

  void
  TraceWitnessStrictDomain(llvm::StringRef role, const WitnessStrictDomainDecision &decision) const;

  void
  TraceWitnessClosureLedger(const WitnessResolverDecision &decision) const;

  void
  TraceWitnessResolverDecision(const WitnessResolverDecision &decision) const;

  void
  TraceWitnessCompositionDecision(llvm::StringRef role, const WitnessCompositionDecision &decision) const;

  ::clang::refold::WitnessCompositionDecision
  ResolveWitnessComposition(llvm::StringRef role,
    llvm::ArrayRef<std::pair<size_t, RefoldWitness>> selectableWitnesses,
    bool hasSingleConcreteRepairIdentity) const;

  ::clang::refold::WitnessResolverDecision
  ResolveWitnessesForSelection(llvm::StringRef role, size_t candidateCount,
    llvm::function_ref<bool(size_t)> isSelectable,
    llvm::function_ref<RefoldWitness(size_t)> buildWitness,
    llvm::function_ref<bool(size_t, size_t)> canonicalPrefers,
    std::optional<size_t> legacyIndex) const;

  void
  TraceWitnessEmitted(const RefoldWitness &witness) const;

  void
  TraceWitnessRejected(const RefoldWitness &witness,
                                        WitnessRejectReason reason,
                                        llvm::StringRef detail) const;

  void
  TraceWitnessAmbiguity(llvm::StringRef role,
                                         uint64_t candidateCount,
                                         uint64_t selectableCount,
                                         uint64_t ambiguityClassCount) const;

  void
  TraceWitnessSelectionProbe(llvm::StringRef role, uint64_t candidateCount,
    uint64_t proofValidCount, uint64_t proofInvalidCount,
    uint64_t equivalenceClassCount, uint64_t completeWitnessCount,
    uint64_t incompleteWitnessCount) const;

  void
  TraceWitnessChosen(const RefoldWitness &witness,
                                      uint64_t selectedIndex) const;

  void
  TraceWitnessFallback(const TerminalFallbackRequest &request) const;

  void
  AttachMixedOwnerTilingWitnessForTokenEnvelope(ProofSummary &summary, uint64_t aStart, uint64_t aEnd, uint64_t bStart,
    uint64_t bEnd) const;

  ::clang::refold::OwnerRealizationResult
  TryBuildOwnerRealization(OwnerRealizationEvidenceKind evidence,
                                       OwnerClosure closure,
                                       llvm::StringRef detail) const;

  void
  ApplyOwnerRealizationResultToProofSummary(ProofSummary &summary, const OwnerRealizationResult &result) const;

  ::clang::refold::OwnerRealizationResult
  BuildMacroWholeCoverOwnerRealization(const RefoldModel::MacroInvocation &macro,
    const WholeCoverPlan &plan) const;

  ::clang::refold::OwnerRealizationResult
  BuildIncludeOwnerRealization(const RefoldModel::IncludeItem &include, AcceptedPathKind currentPath,
    IncludeRealizationEvidenceKind evidenceKind,
    std::optional<IncludeRealizationBTokenEnvelope> bTokenEnvelope) const;

  ::clang::refold::OwnerRealizationResult
  BuildTUOwnerRealization(AcceptedPathKind currentPath,
                                      uint64_t begin, uint64_t end) const;

  void
  StampMacroWholeCoverRealizationPatch(MacroPatch &patch, const WholeCoverPlan &plan,
    const RefoldModel::MacroInvocation &macro) const;

  ::clang::refold::ProofSummary
  BuildAcceptedPathProofSummary(AcceptedPathKind currentPath,
    const IncludePatch *patch = nullptr,
    const TUAnchorWitness *tuAnchorWitness = nullptr,
    const IncludeAnchorWitness *includeAnchorWitness = nullptr,
    const TerminalFallbackWitness *terminalFallbackWitness = nullptr) const;

  ::clang::refold::ProofSummary
  BuildIncludePatchProofSummary(bool realizedSurface,
                                            AcceptedPathKind currentPath,
                                            const IncludePatch *patch) const;

  static ::clang::refold::EmittedProof
  BuildEmittedProofFromSummary(TheoremProofClass theoremClass,
                                           const ProofSummary &summary);

  std::optional<::clang::refold::EmittedProof>
  BuildCanonicalEmittedProofFromSummary(const ProofSummary &summary) const;

  std::optional<::clang::refold::EmittedProof>
  BuildEmittedProof(const AcceptedResultCandidate &candidate) const;

  std::optional<::clang::refold::TheoremProofClass>
  NormalizeAcceptedProof(const AcceptedResultCandidate &candidate) const;

  ::clang::refold::GlobalSelectionLattice
  BuildGlobalSelectionLattice(const ProofSummary &summary) const;

  ::clang::refold::CompletenessContract
  BuildCompletenessContract(const ProofSummary &summary) const;

  ::clang::refold::TheoremDomainContract
  BuildTheoremDomainContract(const ProofSummary &summary) const;

  bool
  LatticePrefers(const ProofSummary &lhs,
                                  const ProofSummary &rhs) const;

  bool
  IsSelectableAcceptedResultCandidate(const AcceptedResultCandidate &candidate) const;

  /// Return whether \p candidate failed only the nested-macro top-level
  /// selector rule.  This is proof-lattice selection logic: callers use it to
  /// distinguish a selector-only nested proof from a fully rejected macro
  /// candidate.
  bool AcceptedResultCandidateHasOnlyNonTopLevelMacroSelectorFailure(
      const AcceptedResultCandidate &candidate) const;

  bool
  AcceptedResultCandidateProofPrefers(const AcceptedResultCandidate &lhs,
    const AcceptedResultCandidate &rhs) const;

  bool
  AcceptedResultCandidateCanonicalPrefers(const AcceptedResultCandidate &lhs,
    const AcceptedResultCandidate &rhs) const;

  bool
  AcceptedResultCandidatePrefers(const AcceptedResultCandidate &lhs,
    const AcceptedResultCandidate &rhs) const;

  std::optional<size_t>
  SelectPreferredCandidateIndex(size_t candidateCount, llvm::function_ref<bool(size_t)> isSelectable,
    llvm::function_ref<bool(size_t, size_t)> prefers) const;

  ::clang::refold::MacroSelectionCandidate
  BuildMacroSelectionCandidate(const MacroPatch &patch, bool allowNonTopLevelMacroSelectorFailure) const;

  bool
  IsSelectableMacroSelectionCandidate(const MacroSelectionCandidate &candidate) const;

  bool
  MacroSelectionCandidatePrefers(const MacroSelectionCandidate &lhs,
    const MacroSelectionCandidate &rhs) const;

  std::optional<::clang::refold::SelectedMacroSelectionCandidate>
  SelectPreferredMacroSelectionCandidate(llvm::ArrayRef<MacroSelectionCandidate> candidates) const;

  std::optional<::clang::refold::SelectedAcceptedResultCandidate>
  SelectPreferredAcceptedResultCandidate(llvm::ArrayRef<AcceptedResultCandidate> candidates) const;

  std::optional<size_t>
  SelectPreferredAcceptedResultCandidateIndex(llvm::ArrayRef<AcceptedResultCandidate> candidates) const;

  static ::clang::refold::EmissionPathKind
  PrimaryEmissionPathForCandidateKind(AcceptedResultCandidateKind kind);

  void
  RefreshAcceptedCandidateEmissionPathInventory(AcceptedResultCandidate &candidate) const;

  ::clang::refold::AcceptedResultCandidate
  BuildAcceptedMacroCandidate(const MacroPatch &patch) const;

  ::clang::refold::AcceptedResultCandidate
  RestampAcceptedMacroCandidateForEmission(const MacroPatch &patch, AcceptedResultCandidate candidate) const;

  ::clang::refold::AcceptedResultCandidate
  BuildAcceptedMacroEmissionCandidate(const MacroPatch &patch) const;

  bool
  FinalizeSelectedMacroPatchForEmission(MacroPatch &patch, llvm::StringRef role) const;

  ::clang::refold::AcceptedResultCandidate
  BuildAcceptedEmittedMacroCandidate(const MacroPatch &patch) const;

  ::clang::refold::AcceptedResultCandidate
  BuildAcceptedIncludeCandidate(AcceptedPathKind currentPath, const IncludePatch &patch,
    const IncludeAnchorWitness *includeAnchorWitness) const;

  ::clang::refold::ProofSummary
  BuildOwnerRealizationProofSummary(AcceptedPathKind currentPath,
    const OwnerRealizationResult &ownerRealization) const;

  ::clang::refold::AcceptedResultCandidate
  BuildAcceptedIncludeRealizationCandidate(
      AcceptedPathKind currentPath, const RefoldModel::IncludeItem &include,
      IncludeRealizationEvidenceKind evidenceKind =
          IncludeRealizationEvidenceKind::Unknown,
      std::optional<IncludeRealizationBTokenEnvelope> bTokenEnvelope =
          std::nullopt) const;

  ::clang::refold::AcceptedResultCandidate
  BuildAcceptedTUTextEditCandidate(AcceptedPathKind currentPath,
                                               uint64_t begin, uint64_t end,
                                               llvm::StringRef payloadPreview) const;

  ::clang::refold::AcceptedResultCandidate
  BuildAcceptedTerminalCandidate(const TerminalFallbackWitness &witness) const;

  bool
  MacroInvocationHasWellFormedPasteWitnesses(const RefoldModel::MacroInvocation &m) const;

  ::clang::refold::ProofDischargeRecord
  ValidateInvocationPreservingProofImpl(const MacroPatch &patch, bool requireTopLevelRoot) const;

  ::clang::refold::ProofDischargeRecord
  ValidateInvocationPreservingProof(const MacroPatch &patch) const;

  ::clang::refold::ProofDischargeRecord
  ValidateEmittedInvocationPreservingProof(const MacroPatch &patch) const;

  ::clang::refold::ProofDischargeRecord
  ValidateInvocationRealizationProof(const MacroPatch &patch) const;

  ::clang::refold::ProofDischargeRecord
  ValidateIncludePreservingProof(AcceptedPathKind currentPath, const IncludePatch *patch,
    const IncludeAnchorWitness *witness) const;

  ::clang::refold::ProofDischargeRecord
  ValidateTUAnchorProof(AcceptedPathKind currentPath,
                                    const TUAnchorWitness *witness) const;

  TerminalFallbackWitness
  BuildTerminalFallbackWitness() const;

  bool
  IsOwnerUnresolvedNoTUAnchorOutOfDomain(const diffutils::Hunk &h, llvm::StringRef tuPath, const Owner &owner,
    bool mapsToTU) const;

  std::string
  BuildOwnerUnresolvedNoTUAnchorDetail(size_t hunkIndex, const diffutils::Hunk &h, llvm::StringRef tuPath,
    const Owner &owner, bool mapsToTU) const;

  std::optional<std::string>
  BuildWholeCoverReplacementText(const RefoldModel::MacroInvocation &m) const;

private:
  Hooks hooks_;
  const RefoldTheoremAudit &theoremAudit_;
  const RefoldTokenTextAnalysis &tokenText_;
  const RefoldArgTextRecovery &argTextRecovery_;
  const RefoldMacroTopology &macroTopology_;
  const RefoldOwnerStateProof &ownerStateProof_;
  const RefoldModel &model_;
  llvm::StringRef bSource_;
  llvm::ArrayRef<PPTok> bToks_;
  RefoldSourceMapper &sourceMapper_;
  TheoremAuditStats &lastTheoremAudit_;
  bool strict_;
  ProofAuditMode &proofAuditMode_;
  const RefoldTerminalProofSink &terminalSink_;

  /// TU planning service used only for theorem diagnostics that need to explain
  /// whether an owner-unresolved hunk still had a deterministic TU witness.
  /// The accepted TU-anchor carrier construction itself lives in
  /// RefoldTUAnchorProof, so this reference does not create a planner/lattice
  /// service cycle.
  const RefoldTUEditPlanner &tuEdits_;

  std::vector<MixedOwnerTilingSegmentBinding> &mixedOwnerTilingSegmentBindings_;
  std::vector<MixedOwnerTilingWitness> &mixedOwnerTilingWitnesses_;
};

} // namespace refold
} // namespace clang

#endif // LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDPROOFLATTICE_H
