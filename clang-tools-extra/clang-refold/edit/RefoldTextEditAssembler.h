//===--- RefoldTextEditAssembler.h ------------------------------*- C++ -*-===//
//
// Final byte-edit assembly for clang-refold.
//
// This class owns pending #line resync flushing, accepted-result carrier
// attachment/auditing, sideband replay certifying, materialized edit-map range
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
#include "proof/RefoldTheoremAudit.h"

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

/// Assembles final byte edits and proof carriers after structural planning.
///
/// The assembler owns emission-order checks, pending line-control resync
/// flushing, sideband replay partitioning, materialized range certification,
/// and final accepted-result carrier attachment; it does not choose macro, TU,
/// or include candidates.
class RefoldTextEditAssembler {
public:
  using TextEdit = ::clang::refold::TextEdit;
  using PendingResync = ::clang::refold::PendingResync;
  using ResyncOutcome = ::clang::refold::ResyncOutcome;
  using AcceptedResultCandidate = ::clang::refold::AcceptedResultCandidate;
  using AcceptedResultCandidateKind =
      ::clang::refold::AcceptedResultCandidateKind;
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
        bTokOff_(bTokOff), abTokHunks_(abTokHunks), abTokMapA2B_(abTokMapA2B),
        abTokMapB2A_(abTokMapB2A), sourceMapper_(sourceMapper),
        proofLattice_(proofLattice), ownerStateProof_(ownerStateProof),
        macroTopology_(macroTopology), lineControlProof_(lineControlProof),
        lineDirs_(lineDirs), terminalSink_(terminalSink), tuEdits_(tuEdits),
        theoremAuditService_(theoremAuditService),
        sidebandPragmaEdits_(sidebandPragmaEdits), theoremAudit_(theoremAudit),
        hooks_(std::move(hooks)) {}

  /// \brief Compute how to preserve __LINE__ after applying replacement to
  /// [start,end) in originalFileText.
  ///
  /// This method detects "line drift" by comparing the newline count in the
  /// original span versus the replacement text. If there is no drift, it
  /// returns (replacement, nullopt).
  ///
  /// If drift is detected, the method computes the logical resume line for the
  /// first character at `end` in the original file, attempts a local resync via
  /// LineDirectiveInserter::MaybeAppendResyncAfterReplacement, and otherwise
  /// returns a PendingResync so the emission layer can flush a `#line`
  /// directive at the next safe BOL.
  ///
  /// Safety note: local injection may fail when inserting a directive would
  /// change token adjacency. In that case, pending resync state is carried only
  /// because a model-recorded suffix __LINE__ observer exists; otherwise no
  /// synthetic directive is produced.
  ResyncOutcome ApplyResyncOrPend(
      llvm::StringRef originalFileText, uint64_t start, uint64_t end,
      llvm::StringRef replacement, llvm::StringRef fileSpellingForDirective,
      std::optional<uint64_t> ownerIncludeId = std::nullopt) const;

  /// \brief Attach accepted-result metadata to an emitted text edit.
  ///
  /// Copies the normalized accepted-result carrier selected by the proof
  /// lattice onto the concrete `TextEdit` that will be emitted. This preserves
  /// the accepted path, proof-discharge inventory, witnesses, and audit
  /// metadata at the byte-edit boundary so later validation/reporting can
  /// reason about the emitted edit without re-running candidate selection.
  void
  AttachAcceptedResultCarrier(TextEdit &edit,
                              const AcceptedResultCandidate &candidate) const;

  /// \brief Audit the complete accepted-proof surface before bytes are emitted.
  ///
  /// This is the last accepted-proof gate before the applicator splices
  /// replacement text into a source file. The audit checks every normalized
  /// edit, every carrier attached to each edit, and the composition law for
  /// multi-carrier edits.
  bool AuditAcceptedEditProofs(
      llvm::ArrayRef<TextEdit> edits, llvm::StringRef emissionStage,
      llvm::StringRef emissionOwner = llvm::StringRef()) const;

  /// \brief Return whether an emitted non-terminal byte edit is backed only by
  /// emission-discharged normalized accepted-result carriers.
  ///
  /// Proof discharge is the universal gate at the actual emission boundary.
  /// Every non-terminal artifact that survives to a TextEdit must carry at
  /// least one normalized accepted-result candidate, and every such carrier
  /// must already be fully discharged under the proof contract that is
  /// appropriate for emitted source text. Terminal out-of-domain results are
  /// never valid carriers for non-terminal emitted edits.
  bool EmittedTextEditHasDischargedAcceptedResults(
      const TextEdit &edit, llvm::StringRef emissionStage,
      llvm::StringRef emissionOwner = llvm::StringRef()) const;

  /// \brief Verify that multiple carriers on one edit compose in source order.
  ///
  /// Individual carrier normalization is not enough for a composite `TextEdit`:
  /// the carriers must either be equivalent witnesses for the same source
  /// surface or form a deterministic, gap-free ordered segment sequence in one
  /// comparable coordinate space. Until future state-gap proof extensions
  /// provide typed state-closed gap witnesses, non-empty inter-segment gaps are
  /// rejected here rather than guessed.
  bool EmittedTextEditHasOrderedAcceptedProofComposition(
      const TextEdit &edit, llvm::StringRef emissionStage,
      llvm::StringRef emissionOwner = llvm::StringRef()) const;

  /// Remove visible sideband replay bytes from an ordinary replay payload when
  /// those bytes are owned by a separate, non-insertion sideband source edit.
  ///
  /// This enforces the replay-partition invariant: B-only sideband insertions
  /// may be carried by an ordinary insertion island, but sideband replacements
  /// and deletions have their own source edit and must not be duplicated by the
  /// ordinary token-envelope replay.
  std::string StripSeparatelyOwnedSidebandReplay(
      llvm::StringRef replayText, std::optional<uint64_t> replayBByteBegin,
      std::optional<uint64_t> replayBByteEnd) const;

  /// Certify a TextEdit with the B-byte range of the materialized surface.
  void CertifyTextEditMaterializedBByteRange(TextEdit &edit, uint64_t begin,
                                             uint64_t end) const;

  /// Certify a TextEdit with the B-byte range described by a B-token envelope.
  void CertifyTextEditMaterializedBTokenRange(TextEdit &edit,
                                              uint64_t bTokBegin,
                                              uint64_t bTokEnd) const;

  /// Certify a TextEdit with the materialized ranges witnessed by a sideband
  /// edit proof. The complete sideband proof binds the emitted replacement
  /// payload to its raw-B byte provenance, so TU sideband emission should
  /// certify those two edit-map facts through this single gate rather than as
  /// independent fields.
  void CertifyTextEditMaterializedBReplayProof(
      TextEdit &edit, const SidebandPragmaEdit &sideband) const;

  /// Return the B-byte envelope contributed by sideband pragma edits owned by
  /// one materialized include, if that sideband stream supplies such a witness.
  std::optional<std::pair<uint64_t, uint64_t>>
  SidebandPragmaMaterializedBByteRangeForInclude(uint64_t includeId) const;

  /// Certify a TextEdit with the replacement-text subrange to report on the
  /// refolded-output side of the optional materialized edit map.
  void CertifyTextEditMaterializedOutputTextRange(TextEdit &edit,
                                                  uint64_t begin,
                                                  uint64_t end) const;

  /// Return the replacement-text subrange to report for a final TextEdit.
  std::optional<std::pair<uint64_t, uint64_t>>
  TextEditMaterializedOutputTextRange(const TextEdit &edit) const;

  /// Return the replacement-text subrange to report for a macro patch.
  std::optional<std::pair<uint64_t, uint64_t>>
  MacroPatchMaterializedOutputTextRange(const MacroPatch &patch) const;

  /// Return the B-byte range carried by a final TextEdit, recovering direct TU
  /// hunk ranges from token provenance when the byte range was not
  /// pre-certified.
  std::optional<std::pair<uint64_t, uint64_t>>
  TextEditMaterializedBByteRange(const TextEdit &edit) const;

  /// Return the B-byte range for a macro patch, using its certified envelope
  /// when present and falling back to the macro's mapped expansion cover
  /// otherwise.
  std::optional<std::pair<uint64_t, uint64_t>>
  MacroPatchMaterializedBByteRange(const MacroPatch &patch) const;

  /// \brief Apply a set of TextEdits to originalFileText, producing the final
  /// refolded text for a single file while preserving __LINE__ transparency via
  /// pending resync.
  ///
  /// Core responsibilities:
  /// * normalize edits by de-duplicating exact-span edits, with the last one
  ///   winning;
  /// * order edits by increasing start/end and enforce non-overlap;
  /// * stream output in order as original slices plus replacement text; and
  /// * when an edit carries a PendingResync, defer `#line` emission until the
  ///   next safe BOL in subsequent unchanged original text, using
  ///   AppendOriginalSliceWithPending().
  ///
  /// Pending semantics: if multiple edits produce pending drift and an earlier
  /// pending resync could not be flushed yet, the most recent PendingResync
  /// wins. EOF behavior: if a pending resync remains at end-of-file, it is
  /// dropped as harmless because there is no subsequent original code whose
  /// __LINE__ needs correction.
  std::string ApplyTextEditsWithPendingResync(
      llvm::StringRef originalFileText, llvm::ArrayRef<TextEdit> edits,
      llvm::DenseSet<uint64_t> *appliedExpandedMacroRootIds = nullptr,
      llvm::StringRef emissionOwner = llvm::StringRef(),
      std::optional<uint64_t> ownerIncludeId = std::nullopt,
      std::vector<MaterializedEditMapping> *materializedEditMappings = nullptr,
      std::vector<FinalLineControlPruneCandidate> *lineControlPruneCandidates =
          nullptr,
      std::vector<FinalLineControlSourceMapping> *lineControlSourceMappings =
          nullptr) const;

  /// \brief Append an unchanged slice of the original file original[from:to)
  /// into out, while attempting to flush a previously deferred PendingResync at
  /// the earliest safe point.
  ///
  /// A pending resync represents a required logical `#line` correction that
  /// could not be emitted inside a prior replacement without risking token
  /// adjacency changes. This method flushes that directive when it becomes safe
  /// to do so while streaming unchanged original content.
  ///
  /// If no safe flush point exists within [from,to), the pending resync is
  /// returned unchanged so it can be attempted again on the next original
  /// slice.
  std::optional<PendingResync> AppendOriginalSliceWithPending(
      llvm::SmallVectorImpl<char> &out, llvm::StringRef original, uint64_t from,
      uint64_t to, std::optional<PendingResync> pending,
      llvm::StringRef emissionOwner = llvm::StringRef(),
      std::optional<uint64_t> ownerIncludeId = std::nullopt,
      std::vector<FinalLineControlPruneCandidate> *lineControlPruneCandidates =
          nullptr,
      std::vector<FinalLineControlSourceMapping> *lineControlSourceMappings =
          nullptr) const;

  /// Build a TextEdit for a direct TU hunk by routing through the TU edit
  /// planner's `BuildDirectTUHunkEditPlan` and certifying the resulting edit
  /// with materialized-byte and accepted-result carriers.
  ///
  /// This service is the single home for assembling proof-certified direct TU
  /// hunk edits; orchestration sites just supply the hunk + span + payload
  /// inputs and consume the resulting `TextEdit`.
  TextEdit BuildDirectTUHunkTextEdit(
      const diffutils::Hunk &h, uint64_t hunkIndex,
      const std::pair<uint64_t, uint64_t> &span, ResyncOutcome resync,
      llvm::StringRef acceptedPayload, uint64_t rawTUStart, uint64_t rawTUEnd,
      std::optional<uint64_t> materializedBByteBegin,
      std::optional<uint64_t> materializedBByteEnd,
      AcceptedPathKind acceptedPath) const;

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
