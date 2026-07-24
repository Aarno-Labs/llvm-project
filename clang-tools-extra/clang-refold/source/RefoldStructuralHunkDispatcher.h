//===--- RefoldStructuralHunkDispatcher.h ------------------------*- C++
//-*-===//
//
// Structural hunk dispatch staging for clang-refold.
//
// This service owns the mutable buckets populated while token hunks are
// classified into TU edits, include edits, and macro patches.  It deliberately
// does not choose proof classes; RunRefoldPass and the dedicated proof
// services still make those decisions.  The dispatcher owns the staging rules:
// coalescing macro patches by physical invocation span, sorting include-local
// insertion patches, building closure-fallback claimed intervals, and exposing
// narrow materialization/emission views only where downstream code still
// consumes map/vector carrier surfaces.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDSTRUCTURALHUNKDISPATCHER_H
#define LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDSTRUCTURALHUNKDISPATCHER_H

#include "edit/RefoldEditTypes.h"
#include "edit/RefoldPatchTypes.h"
#include "source/RefoldAlignmentSemanticEvidence.h"
#include "util/RefoldDenseMapInfo.h"
#include "clang/Basic/LangOptions.h"
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

namespace diffutils {
struct Hunk;
}

class RefoldLineObserverLayout;
class RefoldMacroTopology;
class RefoldModel;
class RefoldPathIdentity;
class RefoldProofLattice;
class RefoldWitnessEquivalenceKeyBuilder;

/// Exact staged-topology serialization used by the semantic alignment
/// resolver. `complete` is false when any emitted accepted carrier lacks a
/// complete witness-equivalence key, in which case `failure` names the first
/// missing proof dimension and the key cannot become production authority.
struct AlignmentSemanticTopologyKeyResult {
  bool complete = true;
  std::string key;
  std::string failure;
  AlignmentSemanticPreservationFootprint preservationFootprint;
};

/// Mutable staging area for one structural hunk-dispatch pass.
class RefoldStructuralHunkDispatcher {
public:
  using IncludeEditMap = llvm::DenseMap<uint64_t, IncludeEdits>;
  using MacroPatchByMacroIdMap = llvm::DenseMap<uint64_t, MacroPatch>;
  using MacroPatchByOwnerMap =
      llvm::DenseMap<std::optional<uint64_t>, std::vector<MacroPatch>>;
  using MacroPatchByOwnerByMacroIdMap =
      llvm::DenseMap<std::optional<uint64_t>, MacroPatchByMacroIdMap>;

  /// Staging facts for one physical macro invocation span.
  struct MacroPatchStagingSlot {
    std::optional<uint64_t> ownerIncludeId;
    uint64_t patchKey = 0;
    MacroPatch *existingPatch = nullptr;
    bool existingIsCallsite = false;
    std::string currentInvocationText;
  };

  /// Append a TU edit to the structural staging bucket.
  void AddTUEdit(TextEdit edit);

  /// Return TU-local edits for later TU repair/audit/emission phases.
  std::vector<TextEdit> &MutableTUEditsForRepairAndEmission();

  /// Append an include-local patch to the bucket for its owning include.
  void AddIncludePatch(const RefoldModel::IncludeItem *include,
                       IncludePatch patch);

  /// Sort each include-local patch list into deterministic emission order.
  ///
  /// Multiple insertion patches may be attributed to the same include instance.
  /// Discovery order can depend on map traversal or incidental diff/LCS
  /// reconstruction details, so this helper canonicalizes each include bucket
  /// by `IncludePatch::aStart` first and `IncludePatch::bStart` second.  It
  /// does not widen, merge, coalesce, rechunk, rewrite payloads, or make
  /// ownership decisions; it only reorders already-attributed patches in place.
  void OrderIncludeInsertions();

  /// Append line-observer realization edits to the staged TU and include
  /// buckets.
  bool AppendLineObserverRealizationEdits(RefoldLineObserverLayout &layout,
                                          llvm::StringRef tuPath,
                                          llvm::StringRef tuBytes);

  /// Return whether a particular include bucket contains direct patches.
  bool HasIncludePatchesFor(uint64_t includeId) const;

  /// Return include-local edit buckets for include materialization.
  IncludeEditMap &MutableIncludeEditBucketsForMaterialization();

  /// Prepare the coalesced staging slot for a macro invocation callsite.
  MacroPatchStagingSlot
  PrepareMacroPatchStagingSlot(const RefoldModel::MacroInvocation &macro);

  /// Return true if this physical macro invocation span already has a staged
  /// patch in the current dispatch pass.
  bool
  HasMacroPatchForInvocation(const RefoldModel::MacroInvocation &macro) const;

  /// Return the mutable owner->macro-id merge buckets consumed by
  /// macro-planning APIs.
  MacroPatchByOwnerByMacroIdMap &MacroPatchMergeBucketsForPlanner();

  /// Merge the previous and current B-token materialization envelopes into the
  /// candidate patch before it replaces the staged callsite slot.
  void MergeMaterializedBTokenRangeFromSlot(MacroPatch &patch,
                                            const MacroPatchStagingSlot &slot,
                                            const diffutils::Hunk &hunk) const;

  /// Install or replace a staged macro patch under a prepared staging slot.
  void StageMacroPatch(const MacroPatchStagingSlot &slot, MacroPatch patch);

  /// Flatten per-owner macro-patch merge buckets into final patch vectors.
  ///
  /// The merge buckets use DenseMap storage while patches are discovered.
  /// Finalization sorts owners and macro ids explicitly before asking the proof
  /// lattice whether each patch is selectable for emission, preserving a
  /// deterministic output order.
  void FinalizeMacroPatchBuckets(RefoldProofLattice &proofLattice);

  /// Return final macro patches for the given owner if the owner has any.
  std::vector<MacroPatch> *
  FindFinalMacroPatchesForOwner(std::optional<uint64_t> ownerIncludeId);
  const std::vector<MacroPatch> *
  FindFinalMacroPatchesForOwner(std::optional<uint64_t> ownerIncludeId) const;

  /// Return true when the finalized owner bucket contains macro patches.
  bool
  HasFinalMacroPatchesForOwner(std::optional<uint64_t> ownerIncludeId) const;

  /// Return true when any include bucket contains patches.
  bool IncludeBucketsHavePatches() const;

  /// Return true when any finalized macro bucket contains patches.
  bool MacroBucketsHavePatches() const;

  /// Return the number of finalized TU-owned macro patches.
  size_t CountTUMacroPatches() const;

  /// Return the total number of include-local patches across all buckets.
  size_t CountIncludePatches() const;

  /// Return the number of direct include-edit buckets.
  size_t IncludeBucketCount() const;

  /// Return the number of finalized macro owner buckets.
  size_t MacroOwnerBucketCount() const;

  /// Insert include ids that have direct include-edit buckets.
  void AddDirectIncludeEditSeeds(llvm::DenseSet<uint64_t> &seeds) const;

  /// Insert include ids that own finalized header-local macro patches.
  void AddHeaderMacroPatchSeeds(llvm::DenseSet<uint64_t> &seeds) const;

  /// Build the source intervals already claimed by TU edits or TU-owned macro
  /// patches, for unresolved-hunk closure fallback checks.
  llvm::SmallVector<std::pair<uint64_t, uint64_t>, 8>
  BuildTUClosureSourceIntervals() const;

  /// Return final macro patches keyed by owner include id for materialization.
  MacroPatchByOwnerMap &FinalMacroPatchesByOwnerForMaterialization();

  /// Return applied macro-root ids for include materialization side effects.
  llvm::DenseSet<uint64_t> &MutableAppliedExpandedMacroRootIds();

  /// Charge root macros whose owning includes were materialized in the emitted
  /// result, so final stats reflect expanded structure that survived emission.
  void RecordExpandedMacroRootsInMaterializedIncludes(
      const RefoldModel &model, const RefoldMacroTopology &macroTopology,
      const llvm::DenseSet<uint64_t> &expandedIncludeIds);

  /// Return the number of root macro ids recorded for emitted expansions.
  size_t ExpandedMacroRootCount() const;

  /// Return the exact root macro ids recorded for emitted expansions.
  ///
  /// This read-only view is consumed by isolated semantic-alignment theorem
  /// simulations. It cannot change statistics attribution or emission order.
  const llvm::DenseSet<uint64_t> &ExpandedMacroRootIds() const;

  /// Serialize the complete staged source-edit topology for alignment witness
  /// equivalence. `tuSourceBytes` supplies immutable original bytes for exact
  /// TU-carrier transformation evidence. The serialization is deterministic
  /// and length-delimited; constructing it never mutates staging order or proof
  /// state.
  AlignmentSemanticTopologyKeyResult BuildAlignmentSemanticTopologyKey(
      const RefoldWitnessEquivalenceKeyBuilder &equivalenceKeyBuilder,
      llvm::StringRef tuSourceBytes) const;

private:
  IncludeEdits &EnsureIncludeEdits(const RefoldModel::IncludeItem *include);
  MacroPatchByMacroIdMap &
  MacroPatchBucketForOwner(std::optional<uint64_t> ownerIncludeId);
  MacroPatchByMacroIdMap *
  FindMacroPatchBucketForOwner(std::optional<uint64_t> ownerIncludeId);
  const MacroPatchByMacroIdMap *
  FindMacroPatchBucketForOwner(std::optional<uint64_t> ownerIncludeId) const;
  void StageMacroPatchUnderKey(std::optional<uint64_t> ownerIncludeId,
                               uint64_t macroPatchKey, MacroPatch patch);
  MacroPatch *FindMacroPatchByKey(std::optional<uint64_t> ownerIncludeId,
                                  uint64_t macroPatchKey);
  std::optional<uint64_t>
  FindMacroPatchKeyByInvocationSpan(std::optional<uint64_t> ownerIncludeId,
                                    uint64_t invStart, uint64_t invEnd) const;

  llvm::DenseSet<uint64_t> appliedExpandedMacroRootIds_;
  IncludeEditMap perInclude_;
  MacroPatchByOwnerMap macroPatchesByOwner_;
  MacroPatchByOwnerByMacroIdMap macroPatchByOwnerByMacroId_;
  std::vector<TextEdit> tuEdits_;
};

/// Widen a zero-width TU insertion left across one ordinary whitespace gap when
/// the replacement begins with the narrow lexical-separation punctuation class.
///
/// This is limited to the punctuation set that may replace a horizontal source
/// gap after lexical-separation proof.  The widening is allowed only when the
/// absorbed gap does not overlap any recorded include or macro-invocation
/// interval and the resulting left/right lexical neighbours do not require a
/// synthesized separator on either side.
bool maybeConsumeOrdinarySeparatorGapForPunctuation(
    const RefoldModel &model, const RefoldPathIdentity &pathIdentity,
    llvm::StringRef tuPath, llvm::StringRef tuBytes,
    std::pair<uint64_t, uint64_t> &span, llvm::StringRef replacement,
    const clang::LangOptions &lexLang);

} // namespace refold
} // namespace clang

#endif // LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDSTRUCTURALHUNKDISPATCHER_H
