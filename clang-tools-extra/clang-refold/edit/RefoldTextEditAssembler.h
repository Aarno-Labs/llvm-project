//===--- RefoldTextEditAssembler.h ------------------------------*- C++ -*-===//
//
// Final byte-edit assembly for clang-refold.
//
// This class owns pending #line resync flushing, accepted-result carrier
// attachment/auditing, sideband replay stamping, materialized edit-map range
// recovery, and final splice/application behavior.  All state, proof services,
// and orchestration callbacks needed for final byte assembly are explicit
// constructor dependencies.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDTEXTEDITASSEMBLER_H
#define LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDTEXTEDITASSEMBLER_H

#include "edit/RefoldEditTypes.h"
#include "edit/RefoldPatchTypes.h"
#include "line-control/RefoldLineControlProof.h"
#include "macro/RefoldMacroTopology.h"
#include "proof/RefoldAcceptedResultTypes.h"
#include "proof/RefoldOwnerStateTypes.h"
#include "proof/RefoldSidebandReplayProof.h"
#include "proof/RefoldTerminalProof.h"
#include "proof/RefoldTerminalProofSink.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace clang {
namespace refold {

class LineDirectiveInserter;
class RefoldOwnerStateProof;
class RefoldProofLattice;
class RefoldSourceMapper;
class RefoldTUEditPlanner;
class RefoldTheoremAudit;

class RefoldTextEditAssembler {
public:
  using TextEdit = ::clang::refold::TextEdit;
  using PendingResync = ::clang::refold::PendingResync;
  using ResyncOutcome = ::clang::refold::ResyncOutcome;
  using AcceptedResultCandidate = ::clang::refold::AcceptedResultCandidate;
  using AcceptedResultCandidateKind = ::clang::refold::AcceptedResultCandidateKind;
  using MacroPatch = ::clang::refold::MacroPatch;
  using SidebandPragmaEdit = ::clang::refold::SidebandPragmaEdit;
  using LineStateObserverDemand = ::clang::refold::LineStateObserverDemand;
  using LineStateObserverSite = ::clang::refold::LineStateObserverSite;
  using OwnerSourceRange = ::clang::refold::OwnerSourceRange;
  using OwnerStateBoundary = ::clang::refold::OwnerStateBoundary;
  using OwnerStateComponent = ::clang::refold::OwnerStateComponent;
  using StateMutationKind = ::clang::refold::StateMutationKind;
  using SuffixStabilityWitness = ::clang::refold::SuffixStabilityWitness;
  using SuffixStabilityWitnessKind =
      ::clang::refold::SuffixStabilityWitnessKind;
  using AcceptanceSupportKind = ::clang::refold::AcceptanceSupportKind;
  using AcceptedPathKind = ::clang::refold::AcceptedPathKind;
  using CompletenessCoverageKind = ::clang::refold::CompletenessCoverageKind;
  using LegacyPathKind = ::clang::refold::LegacyPathKind;
  using ProofSummary = ::clang::refold::ProofSummary;
  using TheoremDomainKind = ::clang::refold::TheoremDomainKind;
  using TheoremProofClass = ::clang::refold::TheoremProofClass;
  using TerminalFallbackFailureContext =
      ::clang::refold::TerminalFallbackFailureContext;
  using TerminalFallbackFailureReason =
      ::clang::refold::TerminalFallbackFailureReason;
  using TerminalFallbackObligationKind =
      ::clang::refold::TerminalFallbackObligationKind;
  using TerminalFallbackProofFailure =
      ::clang::refold::TerminalFallbackProofFailure;

  /// Exceptional callback bundle for cycle-breaking orchestration queries.
  /// Whole-cover planning currently lives behind the macro planner/lattice
  /// cycle, and line-resync deferral is owned by RefoldLineObserverLayout,
  /// which depends on this assembler for emitted layout edits.  Theorem/audit
  /// policy is owned by RefoldTheoremAudit.
  struct Hooks {
    std::function<std::optional<WholeCoverPlan>(
        const RefoldModel::MacroInvocation &)>
        computeWholeCoverPlan;
    std::function<bool(llvm::StringRef, std::optional<uint64_t>, uint64_t)>
        lineResyncShouldDeferToConditionalJoin;
  };

  RefoldTextEditAssembler(
      const RefoldModel &model, llvm::StringRef bSource,
      llvm::ArrayRef<PPTok> aToks, llvm::ArrayRef<PPTok> bToks,
      llvm::ArrayRef<size_t> bTokOff,
      const std::vector<diffutils::Hunk> &abTokHunks,
      const std::vector<int64_t> &abTokMapA2B,
      const std::vector<int64_t> &abTokMapB2A,
      const RefoldSourceMapper &sourceMapper,
      const RefoldProofLattice &proofLattice,
      const RefoldOwnerStateProof &ownerStateProof,
      const RefoldMacroTopology &macroTopology,
      const RefoldLineControlProof &lineControlProof,
      const LineDirectiveInserter &lineDirs,
      const RefoldTerminalProofSink &terminalSink,
      const RefoldTUEditPlanner &tuEdits,
      const RefoldTheoremAudit &theoremAuditService,
      const std::vector<SidebandPragmaEdit> &sidebandPragmaEdits,
      TheoremAuditStats &theoremAudit, Hooks hooks)
      : model_(model), bSource_(bSource), aToks_(aToks), bToks_(bToks),
        bTokOff_(bTokOff), abTokHunks_(abTokHunks),
        abTokMapA2B_(abTokMapA2B), abTokMapB2A_(abTokMapB2A),
        sourceMapper_(sourceMapper), proofLattice_(proofLattice),
        ownerStateProof_(ownerStateProof), macroTopology_(macroTopology),
        lineControlProof_(lineControlProof), lineDirs_(lineDirs),
        terminalSink_(terminalSink), tuEdits_(tuEdits),
        theoremAuditService_(theoremAuditService),
        sidebandPragmaEdits_(sidebandPragmaEdits), theoremAudit_(theoremAudit),
        hooks_(std::move(hooks)) {}

  /// Compute the replacement text and optional deferred #line resync state for
  /// one source edit.  The assembler owns this because the decision is part of
  /// final byte emission, not patch planning.
  ResyncOutcome ApplyResyncOrPend(
      llvm::StringRef originalFileText, uint64_t start, uint64_t end,
      llvm::StringRef replacement, llvm::StringRef fileSpellingForDirective,
      std::optional<uint64_t> ownerIncludeId = std::nullopt) const;

  /// Attach the normalized accepted-result carrier selected by the proof
  /// lattice to a concrete emitted edit.
  void AttachAcceptedResultCarrier(
      TextEdit &edit, const AcceptedResultCandidate &candidate) const;

  /// Run the last accepted-result theorem audit before text bytes are emitted.
  bool AuditAcceptedEditProofs(llvm::ArrayRef<TextEdit> edits,
                               llvm::StringRef emissionStage,
                               llvm::StringRef emissionOwner =
                                   llvm::StringRef()) const;

  bool EmittedTextEditHasDischargedAcceptedResults(
      const TextEdit &edit, llvm::StringRef emissionStage,
      llvm::StringRef emissionOwner = llvm::StringRef()) const;

  bool EmittedTextEditHasOrderedAcceptedProofComposition(
      const TextEdit &edit, llvm::StringRef emissionStage,
      llvm::StringRef emissionOwner = llvm::StringRef()) const;

  std::string StripSeparatelyOwnedSidebandReplay(
      llvm::StringRef replayText, std::optional<uint64_t> replayBByteBegin,
      std::optional<uint64_t> replayBByteEnd) const;

  void StampTextEditMaterializedBByteRange(TextEdit &edit, uint64_t begin,
                                           uint64_t end) const;

  void StampTextEditMaterializedBTokenRange(TextEdit &edit, uint64_t bTokBegin,
                                            uint64_t bTokEnd) const;

  void StampTextEditMaterializedBReplayProof(
      TextEdit &edit, const SidebandPragmaEdit &sideband) const;

  std::optional<std::pair<uint64_t, uint64_t>>
  SidebandPragmaMaterializedBByteRangeForInclude(uint64_t includeId) const;

  void StampTextEditMaterializedOutputTextRange(TextEdit &edit, uint64_t begin,
                                                uint64_t end) const;

  std::optional<std::pair<uint64_t, uint64_t>>
  TextEditMaterializedOutputTextRange(const TextEdit &edit) const;

  std::optional<std::pair<uint64_t, uint64_t>>
  MacroPatchMaterializedOutputTextRange(const MacroPatch &patch) const;

  std::optional<std::pair<uint64_t, uint64_t>>
  TextEditMaterializedBByteRange(const TextEdit &edit) const;

  std::optional<std::pair<uint64_t, uint64_t>>
  MacroPatchMaterializedBByteRange(const MacroPatch &patch) const;

  /// Apply normalized text edits to one owner file while carrying pending
  /// newline-resync state and optional materialized-edit/source-map records.
  std::string ApplyTextEditsWithPendingResync(
      llvm::StringRef originalFileText, llvm::ArrayRef<TextEdit> edits,
      llvm::DenseSet<uint64_t> *appliedExpandedMacroRootIds = nullptr,
      llvm::StringRef emissionOwner = llvm::StringRef(),
      std::optional<uint64_t> ownerIncludeId = std::nullopt,
      std::vector<MaterializedEditMapping> *materializedEditMappings = nullptr,
      std::vector<FinalLineControlPruneCandidate>
          *lineControlPruneCandidates = nullptr,
      std::vector<FinalLineControlSourceMapping>
          *lineControlSourceMappings = nullptr) const;

  std::optional<PendingResync> AppendOriginalSliceWithPending(
      llvm::SmallVectorImpl<char> &out, llvm::StringRef original,
      uint64_t from, uint64_t to, std::optional<PendingResync> pending,
      llvm::StringRef emissionOwner = llvm::StringRef(),
      std::optional<uint64_t> ownerIncludeId = std::nullopt,
      std::vector<FinalLineControlPruneCandidate> *lineControlPruneCandidates =
          nullptr,
      std::vector<FinalLineControlSourceMapping> *lineControlSourceMappings =
          nullptr) const;

private:
  const RefoldModel &model_;
  llvm::StringRef bSource_;
  llvm::ArrayRef<PPTok> aToks_;
  llvm::ArrayRef<PPTok> bToks_;
  llvm::ArrayRef<size_t> bTokOff_;
  const std::vector<diffutils::Hunk> &abTokHunks_;
  const std::vector<int64_t> &abTokMapA2B_;
  const std::vector<int64_t> &abTokMapB2A_;
  const RefoldSourceMapper &sourceMapper_;
  const RefoldProofLattice &proofLattice_;
  const RefoldOwnerStateProof &ownerStateProof_;
  const RefoldMacroTopology &macroTopology_;
  const RefoldLineControlProof &lineControlProof_;
  const LineDirectiveInserter &lineDirs_;
  const RefoldTerminalProofSink &terminalSink_;
  const RefoldTUEditPlanner &tuEdits_;
  const RefoldTheoremAudit &theoremAuditService_;
  const std::vector<SidebandPragmaEdit> &sidebandPragmaEdits_;
  TheoremAuditStats &theoremAudit_;
  Hooks hooks_;
};

} // namespace refold
} // namespace clang

#endif // LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDTEXTEDITASSEMBLER_H
