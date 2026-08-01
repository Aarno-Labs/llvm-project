//===--- RefoldIncludeMaterializationScheduler.h ----------------*- C++ -*-===//
//
// Include materialization scheduling for clang-refold.
//
// This service owns the run-local orchestration that decides which include
// instances must be materialized, orders those materializations
// deterministically, and lowers realized TU-root include expansions into final
// TU text edits.  It deliberately does not own include replay proof or include
// body realization: RefoldIncludeReplayProof and RefoldIncludeMaterializer
// remain the proof and mechanics boundaries.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_INCLUDE_REFOLDINCLUDEMATERIALIZATIONSCHEDULER_H
#define LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_INCLUDE_REFOLDINCLUDEMATERIALIZATIONSCHEDULER_H

#include "core/RefoldModel.h"
#include "edit/RefoldEditTypes.h"
#include "edit/RefoldPatchTypes.h"
#include "include/RefoldSourceGraphProof.h"
#include "line-control/FinalLineControlModel.h"
#include "macro/RefoldMacroStateRepairPlanner.h"
#include "proof/RefoldAcceptedResultTypes.h"
#include "source/DiffAlgorithms.h"
#include "source/RefoldToken.h"
#include "util/RefoldDenseMapInfo.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace clang {
namespace refold {

class RefoldIncludeInsertionPlanner;
class RefoldIncludeMaterializer;
class RefoldLineObserverLayout;
class RefoldPathIdentity;
class RefoldPragmaOnceGuardRewriter;
class RefoldProofLattice;
class RefoldStructuralHunkDispatcher;
class RefoldTerminalProofSink;
class RefoldTextEditAssembler;
struct SidebandPragmaEdit;

/// Schedules include materialization and TU-root include-emission edits.
///
/// A scheduler instance is intentionally run-scoped: include child indexes,
/// layout-only seed decisions, realized include text, line-control metadata,
/// and accepted-result carriers are shared across include materialization and
/// TU-root emission. Keeping this state in one object avoids rebuilding helper
/// indexes and preserves deterministic ordering around TU macro edit staging.
class RefoldIncludeMaterializationScheduler {
public:
  /// Services used by the scheduler while proof and realization remain in their
  /// dedicated include/proof subsystems.
  ///
  /// All pointers are borrowed from the engine service graph.  Null means the
  /// scheduler is not fully wired and cannot safely perform include
  /// materialization for the current pass.
  struct Dependencies {
    /// Producer model containing include and source facts.
    const RefoldModel *model = nullptr;
    /// Path identity service for include/source comparisons.
    const RefoldPathIdentity *pathIdentity = nullptr;
    /// Include materializer that realizes selected include bodies.
    const RefoldIncludeMaterializer *includeMaterializer = nullptr;
    /// Include insertion planner for include-boundary insertion repairs.
    const RefoldIncludeInsertionPlanner *includeInsertionPlanner = nullptr;
    /// Line observer layout service for include-entry/exit wrappers.
    const RefoldLineObserverLayout *lineObserverLayout = nullptr;
    /// Macro-state repair planner consulted around include realization.
    const RefoldMacroStateRepairPlanner *macroStateRepairPlanner = nullptr;
    /// Final text-edit assembler used for TU-root include edits.
    const RefoldTextEditAssembler *textEditAssembler = nullptr;
    /// Proof lattice used to construct accepted-result carriers.
    const RefoldProofLattice *proofLattice = nullptr;
    /// Synthetic `#pragma once` guard catalog.  Mutable because the scheduler is
    /// what learns which physical headers are actually inlined.
    RefoldPragmaOnceGuardRewriter *pragmaOnceGuards = nullptr;
    /// Terminal fallback sink for fail-closed include scheduling failures.
    RefoldTerminalProofSink *terminalSink = nullptr;
    /// Sideband pragma edits that may be owned by materialized includes.
    const std::vector<SidebandPragmaEdit> *sidebandPragmaEdits = nullptr;
  };

  /// Immutable run inputs and mutable staging surfaces for one include
  /// materialization scheduling pass.
  struct IncludeMaterializationRequest {
    /// Translation-unit path for diagnostics and owner classification.
    llvm::StringRef tuPath;
    /// Translation-unit source bytes before final edit assembly.
    llvm::StringRef tuBytes;
    /// Original preprocessed A source bytes.
    llvm::StringRef aSource;
    /// Edited preprocessed B source bytes.
    llvm::StringRef bSource;
    /// Original preprocessed token stream A.
    llvm::ArrayRef<PPTok> aTokens;
    /// Edited preprocessed token stream B.
    llvm::ArrayRef<PPTok> bTokens;
    /// Byte offsets for A tokens in `aSource`.
    llvm::ArrayRef<size_t> aTokenOffsets;
    /// Byte offsets for B tokens in `bSource`.
    llvm::ArrayRef<size_t> bTokenOffsets;
    /// Token hunks after owner-aware normalization.
    llvm::ArrayRef<diffutils::Hunk> tokenHunks;
    /// Optional raw byte hunks used for sidecar/source-graph proof.
    const std::vector<diffutils::Hunk> *rawByteHunks = nullptr;
    /// Dispatcher that receives staged include-owned/TU-root edits.
    RefoldStructuralHunkDispatcher *structuralHunkDispatcher = nullptr;
    /// Optional source-graph sidecar output ledger.
    std::vector<SourceGraphOutput> *sourceGraphOutputs = nullptr;
  };

  /// Constructs a run-scoped scheduler and builds the include child index once.
  RefoldIncludeMaterializationScheduler(Dependencies deps,
                                        IncludeMaterializationRequest request);

  /// Decides which include instances must be realized, pulls in their
  /// ancestors, orders them deterministically, and asks
  /// RefoldIncludeMaterializer to build the materialized expansion text for
  /// each selected include.
  bool MaterializeIncludeExpansions();

  /// Emits TU text edits for already-realized TU-root include expansions after
  /// TU-owned macro patches and the second macro-state carry pass have been
  /// staged.
  bool StageTURootIncludeExpansionEdits(
      RefoldMacroStateRepairPlanner::MacroStateRepairPlan &macroStatePlan,
      const RefoldMacroStateRepairPlanner::MacroStateRepairRequest
          &macroStateRequest);

  /// Returns the include ids whose expansion text was materialized in this
  /// pass.
  const llvm::DenseSet<uint64_t> &ExpandedIncludeIds() const;

  /// Returns the number of include expansions realized by this scheduler.
  size_t MaterializedIncludeCount() const;

private:
  /// Kind of realized work contributed by a TU-root include subtree.
  ///
  /// Only genuinely sideband-only owner replay may suppress the ordinary
  /// line-control wrappers that --with-lines include materialization would
  /// emit. If the include contributes ordinary PP tokens, macro edits,
  /// layout-only obligations, or nested ordinary work, replacing its directive
  /// with header bytes is a real logical file transition and keeps the
  /// enter/exit wrapper.
  enum class TUIncludeMaterializationWorkClass {
    /// The subtree contributes no realized work.
    None,
    /// The subtree contributes only sideband pragma replay.
    SidebandPragmaOnly,
    /// The subtree contributes ordinary header bytes or layout/proof work.
    Ordinary
  };

  /// Build parent-to-child include edges once for recursive materialization.
  void BuildChildrenIndex();

  /// Finds a macro directive by producer id for line-control source-graph
  /// proof.
  const RefoldModel::MacroDirective *FindMacroDirectiveById(uint64_t id) const;

  /// Returns whether a definition used inside an include's line-control region
  /// was supplied by that include's immediate includer.
  bool DefinitionIsSuppliedByImmediateIncluder(
      const RefoldModel::IncludeItem &include,
      const RefoldModel::MacroDirective &definition) const;

  /// Returns whether a materialized include has source-graph-preservable
  /// line-control macro state supplied by its immediate includer.
  bool IncludeHasIncluderSuppliedLineControlMacroState(
      const RefoldModel::IncludeItem &include) const;

  /// Returns the end byte of a token in a source/token-offset pair.
  static uint64_t TokenEndOffset(llvm::ArrayRef<PPTok> tokens,
                                 llvm::ArrayRef<size_t> tokenOffsets,
                                 size_t index);

  /// Locates the PP-token gap that wholly contains a raw source byte range.
  std::optional<uint64_t> FindTokenGapContainingByteRange(
      llvm::StringRef source, llvm::ArrayRef<PPTok> tokens,
      llvm::ArrayRef<size_t> tokenOffsets, uint64_t begin, uint64_t end) const;

  /// Returns the include owner whose edge-local layout is witnessed by a raw
  /// byte-only hunk, or std::nullopt when the hunk is not an include-layout
  /// materialization seed.
  std::optional<uint64_t>
  LayoutOnlyIncludeSeedForRawByteHunk(const diffutils::Hunk &hunk) const;

  /// Records include-layout materialization seeds from raw byte hunks.
  void BuildLayoutOnlyIncludeMaterializationSeeds();

  /// Adds direct include, layout-only, sideband, and header-macro seeds.
  ///
  /// The seed set is the scheduler's first proof boundary: it records every
  /// include subtree that must be realized before ancestor closure and
  /// deterministic ordering are applied.
  llvm::DenseSet<uint64_t> BuildInitialMaterializationSeeds() const;

  /// Collect the canonical physical paths reachable from a seed set's subtrees.
  void CollectSubtreePhysicalPaths(const llvm::DenseSet<uint64_t> &seeds,
                                   std::vector<std::string> &paths) const;

  /// Force materialization of any include whose transitive closure re-enters an
  /// inlined `#pragma once` header.
  ///
  /// A synthetic guard is emitted into TU byte-space and therefore cannot reach
  /// a re-entry that occurs inside an unmodified header.  Since clang-refold
  /// never edits headers, such an include must be materialized so its nested
  /// directive becomes guardable, or the refolding is unsound.  Iterates to a
  /// fixed point because materializing an include enlarges the inlined set.
  void AddGuardReentryMaterializationSeeds(llvm::DenseSet<uint64_t> &seeds) const;

  /// Pulls selected include ancestors into the seed set so parent surfaces are
  /// materialized before child surfaces.
  void AddAncestorMaterializationSeeds(llvm::DenseSet<uint64_t> &seeds) const;

  /// Returns include nesting depth for deterministic ancestor-first ordering.
  unsigned IncludeMaterializationDepth(uint64_t includeId) const;

  /// Sorts selected include ids ancestor-first, then by producer id.
  llvm::SmallVector<uint64_t, 32>
  OrderedMaterializationSeeds(const llvm::DenseSet<uint64_t> &seeds) const;

  /// Materializes selected include ids in deterministic order.
  void MaterializeOrderedSeeds(llvm::ArrayRef<uint64_t> orderedSeeds);

  /// Record which physical headers may be inlined, before any body is realized.
  ///
  /// The guard catalog must know the active set before the first materialized
  /// body or TU include edit is finalized, because a clean include of a header
  /// can need wrapping purely because a *different* instance of that same
  /// physical header was inlined elsewhere.
  ///
  /// The set is deliberately an over-approximation taken from the ordered seeds
  /// and their descendants: materialization decisions are refined during include
  /// recursion, and under-approximating would leave an occurrence unguarded.
  /// Over-approximating is inert for an unconditional pragma.
  void RecordActiveGuardedHeaders(llvm::ArrayRef<uint64_t> orderedSeeds);

  /// Stage the once-guard wrapper for a TU-owned include that survives as a
  /// directive because its header was inlined at another occurrence.
  bool StageTURootSurvivingIncludeGuards();

  /// Prove that no surviving include can re-enter a header whose inlined body
  /// lost its own include-guard protection.
  ///
  /// A body realized from the edited preprocessed stream is tokens, not source:
  /// every directive it contained is gone, including the `#ifndef`/`#define`
  /// pair a conventional include guard relies on.  The header's content is then
  /// present in the TU while its controlling macro is undefined, so any later
  /// path back to that physical file re-enters it and emits the content twice.
  ///
  /// A `#pragma once` header is handled instead by the synthetic guard, and a
  /// *source*-materialized body carries its own guard directives along and
  /// self-protects, so neither needs this check.
  ///
  /// Restoring the lost state properly would require defining each affected
  /// header's real controlling macro, which the producer does not record today
  /// (Clang tracks it as `HeaderFileInfo::ControllingMacro`).  Until it does,
  /// this fails closed rather than emitting a duplicated body.
  bool ProveNoReentryIntoUnprotectedInlinedHeaders();

  /// Return whether the producer recorded a controlling macro for one physical
  /// header, meaning its lost include-guard state can be restored by name.
  bool HeaderControllingMacroIsRecorded(llvm::StringRef physicalPath) const;

  /// Return whether any macro defined by one physical header is invoked from
  /// outside that header.
  ///
  /// Restoring a header's include guard suppresses every later inclusion of it.
  /// A body realized from B dropped the header's `#define`s together with its
  /// guard, so that suppression also removes the only remaining source of its
  /// macro state.  Restoration is admissible only when nothing outside the
  /// header observes that state.
  bool HeaderMacroStateIsObservedOutside(llvm::StringRef physicalPath) const;

  /// Return whether one physical header contributed any A tokens.
  ///
  /// A header that is entirely directives has no content to duplicate, so
  /// re-entering it is harmless for the content-duplication hazard and is often
  /// required: it is the only remaining source of macro state that a
  /// realized-from-B body discarded.
  bool HeaderContributedTokens(llvm::StringRef physicalPath) const;

  /// Return whether any include edge to one physical header was skipped.
  ///
  /// A skip is the only evidence that the header was protected at all.  Headers
  /// with no protection by design -- Clang's `__stddef_*.h`, `assert.h` -- are
  /// entered every time, so re-entering them in the refolded TU reproduces the
  /// original instead of duplicating content.
  bool HeaderWasEverSkipped(llvm::StringRef physicalPath) const;

  /// Collect the physical paths whose content is emitted by one inlined body.
  ///
  /// This walks the *entered* include subtree, which is exact: `children_` is
  /// built from producer parent links, and only an entered edge contributes
  /// content.  A skipped edge under the same header contributed nothing, so
  /// counting it would over-trigger.
  void CollectEnteredSubtreePhysicalPaths(uint64_t includeId,
                                          std::vector<std::string> &paths) const;

  /// Refreshes the public expanded-include result set from realized expansions.
  void RebuildExpandedIncludeIds();

  /// Returns whether sideband work in this include forces ordinary #line
  /// wrappers rather than sideband-only suppression.
  bool IncludeHasLineDirectiveForcingSidebandWork(uint64_t includeId) const;

  /// Returns whether the include contributes ordinary replay tokens.
  bool IncludeHasOrdinaryReplayTokens(uint64_t includeId) const;

  /// Returns whether an include subtree's materialized B envelope comes only
  /// from sideband pragma replay.
  bool IncludeUsesOnlySidebandReplayEnvelope(uint64_t includeId) const;

  /// Classifies the kind of realized work in an include subtree for final
  /// line-control wrapper policy.
  TUIncludeMaterializationWorkClass
  ClassifyTUIncludeMaterializationWork(uint64_t includeId) const;

  /// Returns whether the include subtree contains a layout-only materialization
  /// obligation that source-graph sidecar preservation cannot discharge.
  bool IncludeSubtreeHasLayoutOnlyMaterializationSeed(uint64_t includeId) const;

  /// Returns the producer site range extended to the full physical include
  /// directive when line-spliced source made the recorded range too short.
  std::pair<uint64_t, uint64_t>
  ExtendedTUSiteRange(const RefoldModel::IncludeItem &include) const;

  /// Tries to preserve an include as a source-graph sidecar instead of emitting
  /// it into the TU, appending output carriers when the proof layer accepts or
  /// rejects a sidecar path.
  ///
  /// A rejected source-graph path can still produce a cleanup-only carrier for
  /// a stale generated sidecar.  The scheduler records those carriers but does
  /// not perform filesystem writes; RefoldSourceGraphWriter owns the I/O side.
  bool TryPreserveSourceGraphOutput(const RefoldModel::IncludeItem &include,
                                    llvm::StringRef expansionText) const;

  /// Lowers one realized TU-root include expansion into a final TU text edit.
  bool StageTURootIncludeExpansionEdit(
      uint64_t includeId,
      RefoldMacroStateRepairPlanner::MacroStateRepairPlan &macroStatePlan,
      const RefoldMacroStateRepairPlanner::MacroStateRepairRequest
          &macroStateRequest);

  Dependencies deps_;
  IncludeMaterializationRequest request_;

  const RefoldModel &model_;
  const RefoldPathIdentity &pathIdentity_;
  const RefoldIncludeMaterializer &includeMaterializer_;
  const RefoldIncludeInsertionPlanner &includeInsertionPlanner_;
  const RefoldLineObserverLayout &lineObserverLayout_;
  const RefoldMacroStateRepairPlanner &macroStateRepairPlanner_;
  const RefoldTextEditAssembler &textEditAssembler_;
  RefoldPragmaOnceGuardRewriter &pragmaOnceGuards_;
  const RefoldProofLattice &proofLattice_;
  RefoldTerminalProofSink &terminalSink_;
  const std::vector<SidebandPragmaEdit> &sidebandPragmaEdits_;
  RefoldStructuralHunkDispatcher &structuralHunkDispatcher_;
  std::vector<TextEdit> &tuEdits_;

  llvm::DenseMap<uint64_t, std::vector<const RefoldModel::IncludeItem *>>
      children_;
  llvm::DenseSet<uint64_t> layoutOnlyIncludeMaterializationSeeds_;
  llvm::DenseMap<uint64_t, std::string> includeExpansion_;
  llvm::DenseMap<uint64_t, std::vector<FinalLineControlPruneCandidate>>
      includeExpansionLineControlPruneCandidates_;
  llvm::DenseMap<uint64_t, std::vector<FinalLineControlSourceMapping>>
      includeExpansionLineControlSourceMappings_;
  llvm::DenseMap<uint64_t, size_t> includeExpansionStartLineNos_;
  llvm::DenseMap<uint64_t, AcceptedResultCandidate>
      includeExpansionAcceptedResults_;
  llvm::DenseSet<uint64_t> expandedIncludeIds_;
};

} // namespace refold
} // namespace clang

#endif // LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_INCLUDE_REFOLDINCLUDEMATERIALIZATIONSCHEDULER_H
