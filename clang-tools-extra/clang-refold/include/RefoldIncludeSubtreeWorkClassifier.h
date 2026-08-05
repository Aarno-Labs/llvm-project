//===--- RefoldIncludeSubtreeWorkClassifier.h ------------------*- C++ -*-===//
//
// Include-subtree work classification for clang-refold.
//
// RefoldIncludeSubtreeWorkClassifier answers whether an include subtree has
// materialization work and whether that work is ordinary replay work or only
// sideband pragma replay.  It borrows the run-scoped staging maps produced by
// structural hunk dispatch and owns no mutable planning state.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDINCLUDESUBTREEWORKCLASSIFIER_H
#define LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDINCLUDESUBTREEWORKCLASSIFIER_H

#include "core/RefoldModel.h"
#include "edit/RefoldPatchTypes.h"
#include "proof/RefoldSidebandReplayProof.h"
#include "util/RefoldDenseMapInfo.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/DenseMap.h"

#include <cstdint>
#include <optional>
#include <vector>

namespace clang {
namespace refold {

/// Classifies materialization work contained in an include subtree.
///
/// The classifier keeps the materializer's recursive child decisions separate
/// from edit assembly: it only inspects include patches, macro patches,
/// sideband pragma edits, child edges, and ordinary PP-token cover.
class RefoldIncludeSubtreeWorkClassifier {
public:
  using IncludeEditMap = llvm::DenseMap<uint64_t, IncludeEdits>;
  using MacroPatchByOwnerMap =
      llvm::DenseMap<std::optional<uint64_t>, std::vector<MacroPatch>>;
  using IncludeChildrenMap =
      llvm::DenseMap<uint64_t, std::vector<const RefoldModel::IncludeItem *>>;

  /// Describes the strongest work class present in an include subtree.
  enum class MaterializationWorkClass {
    /// No include, macro, sideband, or descendant materialization work exists.
    None,

    /// The subtree needs only sideband pragma replay provenance.
    SidebandPragmaOnly,

    /// The subtree contains ordinary include or macro materialization work.
    Ordinary
  };

  /// Borrow immutable run-scoped staging maps for subtree classification.
  RefoldIncludeSubtreeWorkClassifier(
      const RefoldModel &model, const IncludeEditMap &perInclude,
      const MacroPatchByOwnerMap &macroPatchesByOwner,
      const IncludeChildrenMap &children,
      llvm::ArrayRef<SidebandPragmaEdit> sidebandPragmaEdits,
      const llvm::DenseSet<uint64_t> *ownersMustExpand = nullptr);

  /// Returns whether the include subtree has any materialization work.
  /// Clean-child replay-context checks are intentionally outside this query.
  bool HasDescendantWork(uint64_t includeId) const;

  /// Returns whether the include subtree can reach an `#include_next` edge.
  /// This is a reachability predicate for forced include-next materialization.
  bool SubtreeContainsIncludeNext(uint64_t includeId) const;

  /// Returns whether subtree replay provenance comes only from sideband edits.
  /// Ordinary PP-token covers, include patches, and macro patches fail closed.
  bool UsesOnlySidebandReplayEnvelope(uint64_t includeId) const;

  /// Returns the strongest materialization work class for the include subtree.
  /// Sideband-only classification is strict: visible sideband wrappers that
  /// require ordinary line-state preservation are classified as ordinary work.
  MaterializationWorkClass
  ClassifyMaterializationWork(uint64_t includeId) const;

private:
  /// Returns whether a sideband pragma edit targets this include.
  bool HasSidebandPragmaWork(uint64_t includeId) const;

  /// Returns whether a targeted sideband edit forces include #line wrappers.
  bool HasLineDirectiveForcingSidebandWork(uint64_t includeId) const;

  /// Returns whether the include owns ordinary replay tokens.
  bool HasOrdinaryReplayTokens(uint64_t includeId) const;

  const RefoldModel &model_;
  const IncludeEditMap &perInclude_;
  const MacroPatchByOwnerMap &macroPatchesByOwner_;
  const IncludeChildrenMap &children_;
  llvm::ArrayRef<SidebandPragmaEdit> sidebandPragmaEdits_;

  /// Includes the closing output check ruled out from keeping their directive.
  ///
  /// Such an include carries no edit of its own -- the divergence it owns was
  /// realized somewhere inside it -- so it is invisible to every other work
  /// source here.  It is nonetheless work: the include must be materialized, and
  /// an ancestor must be materialized to have somewhere to put it.
  const llvm::DenseSet<uint64_t> *ownersMustExpand_ = nullptr;
};

} // namespace refold
} // namespace clang

#endif // LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDINCLUDESUBTREEWORKCLASSIFIER_H
