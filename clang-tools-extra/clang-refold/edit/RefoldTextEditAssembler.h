//===--- RefoldTextEditAssembler.h ------------------------------*- C++ -*-===//
//
// Final byte-edit assembly for clang-refold.
//
// This class owns pending #line resync flushing, the accepted-proof audit of
// the final edit set, materialized edit-map range recovery, and final
// splice/application behavior.  Protected-source capabilities and materialized
// range certification are RefoldTextEditCertifier's, and newline-drift resync
// is RefoldLineObserverLayout's.  All state and services needed for final byte
// assembly are explicit constructor dependencies.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDTEXTEDITASSEMBLER_H
#define LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDTEXTEDITASSEMBLER_H

#include "edit/RefoldEditTypes.h"
#include "edit/RefoldPatchTypes.h"
#include "edit/RefoldTUEditPlanner.h"
#include "line-control/RefoldLineControlProof.h"
#include "macro/RefoldMacroTopology.h"
#include "proof/RefoldAcceptancePathTypes.h"
#include "proof/RefoldCandidateTypes.h"
#include "proof/RefoldCompletenessTypes.h"
#include "proof/RefoldMacroPatchTypes.h"
#include "proof/RefoldOwnerStateTypes.h"
#include "proof/RefoldSidebandReplayProof.h"
#include "proof/RefoldTheoremAudit.h"
#include "proof/RefoldTheoremTypes.h"
#include "proof/RefoldTilingWitnessTypes.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace clang {

class LangOptions;

namespace refold {

class LineDirectiveInserter;
class RefoldAcceptedCandidateBuilder;
class RefoldMacroWholeCoverPlanBuilder;
class RefoldOwnerStateProof;
class RefoldOwnerRealizationProofBuilder;
class RefoldPathIdentity;
class RefoldPreprocessingStructureIndex;
class RefoldProofSummaryBuilder;
class RefoldSourceMapper;
class RefoldLineObserverLayout;
class RefoldTUAnchorProof;
class RefoldTUEditPlanner;
class RefoldTextEditCertifier;
class RefoldTheoremAudit;

/// Assembles final byte edits and proof carriers after structural planning.
///
/// The assembler owns emission-order checks, pending line-control resync
/// flushing, and materialized range recovery; it does not choose macro, TU, or
/// include candidates.
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

  RefoldTextEditAssembler(
      const RefoldModel &model, llvm::StringRef bSource,
      llvm::ArrayRef<PPTok> aToks, llvm::ArrayRef<PPTok> bToks,
      llvm::ArrayRef<size_t> bTokOff,
      const std::vector<diffutils::Hunk> &abTokHunks,
      const std::vector<int64_t> &abTokMapA2B,
      const std::vector<int64_t> &abTokMapB2A,
      const RefoldSourceMapper &sourceMapper, const clang::LangOptions &lexLang,
      const RefoldProofSummaryBuilder &proofSummaryBuilder,
      const RefoldAcceptedCandidateBuilder &acceptedCandidateBuilder,
      const RefoldOwnerRealizationProofBuilder &ownerRealizationProofBuilder,
      const RefoldMacroWholeCoverPlanBuilder &wholeCoverPlanBuilder,
      const RefoldOwnerStateProof &ownerStateProof,
      const RefoldMacroTopology &macroTopology,
      const RefoldLineControlProof &lineControlProof,
      const LineDirectiveInserter &lineDirs,
      const RefoldTerminalProofSink &terminalSink,
      const RefoldTUEditPlanner &tuEdits,
      const RefoldTUAnchorProof &tuAnchorProof,
      const RefoldTextEditCertifier &textEditCertifier,
      const RefoldLineObserverLayout &lineObserverLayout,
      const RefoldTheoremAudit &theoremAuditService,
      const std::vector<MixedOwnerTilingWitness> &mixedOwnerTilingWitnesses,
      TheoremAuditStats &theoremAudit)
      : model_(model), bSource_(bSource), aToks_(aToks), bToks_(bToks),
        bTokOff_(bTokOff), abTokHunks_(abTokHunks), abTokMapA2B_(abTokMapA2B),
        abTokMapB2A_(abTokMapB2A), sourceMapper_(sourceMapper),
        lexLang_(lexLang), proofSummaryBuilder_(proofSummaryBuilder),
        acceptedCandidateBuilder_(acceptedCandidateBuilder),
        ownerRealizationProofBuilder_(ownerRealizationProofBuilder),
        wholeCoverPlanBuilder_(wholeCoverPlanBuilder),
        ownerStateProof_(ownerStateProof), macroTopology_(macroTopology),
        lineControlProof_(lineControlProof), lineDirs_(lineDirs),
        terminalSink_(terminalSink), tuEdits_(tuEdits),
        tuAnchorProof_(tuAnchorProof), textEditCertifier_(textEditCertifier),
        lineObserverLayout_(lineObserverLayout),
        theoremAuditService_(theoremAuditService),
        mixedOwnerTilingWitnesses_(mixedOwnerTilingWitnesses),
        theoremAudit_(theoremAudit) {}

  /// \brief Audit the complete accepted-proof surface before bytes are emitted.
  ///
  /// This is the last accepted-proof gate before the applicator splices
  /// replacement text into a source file. The audit checks every normalized
  /// edit, every carrier attached to each edit, and the composition law for
  /// multi-carrier edits. `originalFileText` supplies the final physical bytes
  /// needed to distinguish a newline-terminated directive from an end-of-file
  /// directive when auditing a zero-width insertion at the interval end.
  bool AuditAcceptedEditProofs(
      llvm::ArrayRef<TextEdit> edits, llvm::StringRef emissionStage,
      llvm::StringRef emissionOwner = llvm::StringRef(),
      std::optional<uint64_t> ownerIncludeId = std::nullopt,
      llvm::ArrayRef<TextEdit> plannedEdits = {},
      llvm::StringRef originalFileText = llvm::StringRef()) const;

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

  /// Verify the preserved-gap theorem against the normalized physical edit set
  /// rather than only against the planner's token carriers.
  ///
  /// The pre-normalization edit set must contain every token segment belonging
  /// to the current physical source owner, the final edit set must retain those
  /// exact segment carriers, and no normalized `TextEdit` may interfere with a
  /// gap recorded as `PreservedInPlace`. A zero-width insertion at the beginning
  /// or inside such an interval can extend or disable the logical directive
  /// without deleting one of its original bytes. An insertion at the physical
  /// end is also rejected when the preserved directive reaches end-of-file
  /// without a terminating newline, because that insertion extends the same
  /// logical directive line. Comparing both edit sets closes
  /// widening, merging, and conservative-closure paths that occur after token
  /// tiling and could otherwise erase the witness they invalidate.  Replacement
  /// witnesses additionally retain the unique A-to-B boundary
  /// projection proof through this final emission audit.
  bool PreservedStructuralGapsRemainOutsideEmittedEdits(
      llvm::ArrayRef<TextEdit> plannedEdits,
      llvm::ArrayRef<TextEdit> emittedEdits, llvm::StringRef emissionStage,
      llvm::StringRef emissionOwner,
      std::optional<uint64_t> ownerIncludeId,
      llvm::StringRef originalFileText) const;

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
  /// inputs and consume the resulting `TextEdit`.  A failed final TU-span
  /// revalidation returns `std::nullopt` so orchestration can continue to its
  /// declared fallback path.
  std::optional<TextEdit> BuildDirectTUHunkTextEdit(
      const diffutils::Hunk &h, uint64_t hunkIndex,
      const std::pair<uint64_t, uint64_t> &span, ResyncOutcome resync,
      llvm::StringRef acceptedPayload, uint64_t rawTUStart, uint64_t rawTUEnd,
      std::optional<uint64_t> materializedBByteBegin,
      std::optional<uint64_t> materializedBByteEnd,
      AcceptedPathKind acceptedPath,
      std::optional<TUInsertionAnchorAdjustment> insertionAnchorAdjustment =
          std::nullopt) const;

private:
  /// Reject a preserved-gap audit at the common theorem/fallback boundary.
  bool RejectPreservedStructuralGapAudit(llvm::StringRef emissionStage,
                                         llvm::StringRef detail) const;

  const RefoldModel &model_;
  llvm::StringRef bSource_;
  llvm::ArrayRef<PPTok> aToks_;
  llvm::ArrayRef<PPTok> bToks_;
  llvm::ArrayRef<size_t> bTokOff_;
  const std::vector<diffutils::Hunk> &abTokHunks_;
  const std::vector<int64_t> &abTokMapA2B_;
  const std::vector<int64_t> &abTokMapB2A_;
  const RefoldSourceMapper &sourceMapper_;
  const clang::LangOptions &lexLang_;
  const RefoldProofSummaryBuilder &proofSummaryBuilder_;
  const RefoldAcceptedCandidateBuilder &acceptedCandidateBuilder_;
  const RefoldOwnerRealizationProofBuilder &ownerRealizationProofBuilder_;
  /// Whole-cover plan computation.  Borrowed directly: the builder depends on
  /// neither the proof services nor the macro patch planner, so it is
  /// constructed before both and needs no late binding here.
  const RefoldMacroWholeCoverPlanBuilder &wholeCoverPlanBuilder_;
  const RefoldOwnerStateProof &ownerStateProof_;
  const RefoldMacroTopology &macroTopology_;
  const RefoldLineControlProof &lineControlProof_;
  const LineDirectiveInserter &lineDirs_;
  const RefoldTerminalProofSink &terminalSink_;
  const RefoldTUEditPlanner &tuEdits_;
  const RefoldTUAnchorProof &tuAnchorProof_;
  const RefoldTextEditCertifier &textEditCertifier_;
  const RefoldLineObserverLayout &lineObserverLayout_;
  const RefoldTheoremAudit &theoremAuditService_;
  /// Durable structural partitions used to resolve validated TU carrier keys
  /// after later edit normalization has discarded path-local bindings.
  const std::vector<MixedOwnerTilingWitness> &mixedOwnerTilingWitnesses_;
  TheoremAuditStats &theoremAudit_;
};

} // namespace refold
} // namespace clang

#endif // LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDTEXTEDITASSEMBLER_H
