//===--- RefoldIncludeSubtreeWorkClassifier.cpp -----------------*- C++ -*-===//
//
// Include-subtree work classification for clang-refold.
//
// This file implements the read-only include-subtree classifier used by include
// materialization to decide which child subtrees require recursive realization
// and which materialized children need sideband-only replay provenance.
//
//===----------------------------------------------------------------------===//

#include "include/RefoldIncludeSubtreeWorkClassifier.h"

#include "llvm/ADT/STLExtras.h"

using namespace llvm;

namespace clang {
namespace refold {

RefoldIncludeSubtreeWorkClassifier::RefoldIncludeSubtreeWorkClassifier(
    const RefoldModel &model, const IncludeEditMap &perInclude,
    const MacroPatchByOwnerMap &macroPatchesByOwner,
    const IncludeChildrenMap &children,
    ArrayRef<SidebandPragmaEdit> sidebandPragmaEdits,
    const llvm::DenseSet<uint64_t> *ownersMustExpand)
    : model_(model), perInclude_(perInclude),
      macroPatchesByOwner_(macroPatchesByOwner), children_(children),
      sidebandPragmaEdits_(sidebandPragmaEdits),
      ownersMustExpand_(ownersMustExpand) {}

bool RefoldIncludeSubtreeWorkClassifier::HasDescendantWork(
    uint64_t includeId) const {
  bool selfWork = HasSidebandPragmaWork(includeId) ||
                  (ownersMustExpand_ && ownersMustExpand_->count(includeId));
  if (auto it = perInclude_.find(includeId); it != perInclude_.end())
    selfWork = selfWork || !it->second.patches.empty();
  if (!selfWork) {
    if (auto it = macroPatchesByOwner_.find(std::optional<uint64_t>(includeId));
        it != macroPatchesByOwner_.end())
      selfWork = !it->second.empty();
  }

  if (selfWork)
    return true;

  if (auto it = children_.find(includeId); it != children_.end()) {
    for (const auto *child : it->second) {
      if (HasDescendantWork(child->id))
        return true;
    }
  }

  return false;
}

bool RefoldIncludeSubtreeWorkClassifier::SubtreeContainsIncludeNext(
    uint64_t includeId) const {
  const RefoldModel::IncludeItem *include = model_.GetIncludeById(includeId);
  if (include && include->subkind == "#include_next")
    return true;

  if (auto it = children_.find(includeId); it != children_.end()) {
    for (const auto *child : it->second)
      if (child && SubtreeContainsIncludeNext(child->id))
        return true;
  }

  return false;
}

bool RefoldIncludeSubtreeWorkClassifier::UsesOnlySidebandReplayEnvelope(
    uint64_t includeId) const {
  bool sawSideband = HasSidebandPragmaWork(includeId);
  if (HasOrdinaryReplayTokens(includeId))
    return false;
  if (auto it = perInclude_.find(includeId);
      it != perInclude_.end() && !it->second.patches.empty())
    return false;
  if (auto it = macroPatchesByOwner_.find(std::optional<uint64_t>(includeId));
      it != macroPatchesByOwner_.end() && !it->second.empty())
    return false;
  if (auto it = children_.find(includeId); it != children_.end()) {
    for (const auto *child : it->second) {
      if (!UsesOnlySidebandReplayEnvelope(child->id))
        return false;
      sawSideband = true;
    }
  }
  return sawSideband;
}

RefoldIncludeSubtreeWorkClassifier::MaterializationWorkClass
RefoldIncludeSubtreeWorkClassifier::ClassifyMaterializationWork(
    uint64_t includeId) const {
  bool sawSideband = HasSidebandPragmaWork(includeId);
  if (ownersMustExpand_ && ownersMustExpand_->count(includeId))
    return MaterializationWorkClass::Ordinary;
  if (HasLineDirectiveForcingSidebandWork(includeId))
    return MaterializationWorkClass::Ordinary;
  if (sawSideband && HasOrdinaryReplayTokens(includeId))
    return MaterializationWorkClass::Ordinary;
  if (auto it = perInclude_.find(includeId);
      it != perInclude_.end() && !it->second.patches.empty())
    return MaterializationWorkClass::Ordinary;
  if (auto it = macroPatchesByOwner_.find(std::optional<uint64_t>(includeId));
      it != macroPatchesByOwner_.end() && !it->second.empty())
    return MaterializationWorkClass::Ordinary;
  if (auto it = children_.find(includeId); it != children_.end()) {
    for (const auto *child : it->second) {
      switch (ClassifyMaterializationWork(child->id)) {
      case MaterializationWorkClass::Ordinary:
        return MaterializationWorkClass::Ordinary;
      case MaterializationWorkClass::SidebandPragmaOnly:
        sawSideband = true;
        break;
      case MaterializationWorkClass::None:
        break;
      }
    }
  }
  return sawSideband ? MaterializationWorkClass::SidebandPragmaOnly
                     : MaterializationWorkClass::None;
}

bool RefoldIncludeSubtreeWorkClassifier::HasSidebandPragmaWork(
    uint64_t includeId) const {
  return llvm::any_of(sidebandPragmaEdits_,
                      [includeId](const SidebandPragmaEdit &edit) {
                        return edit.TargetsInclude(includeId);
                      });
}

bool RefoldIncludeSubtreeWorkClassifier::HasLineDirectiveForcingSidebandWork(
    uint64_t includeId) const {
  return llvm::any_of(sidebandPragmaEdits_,
                      [includeId](const SidebandPragmaEdit &edit) {
                        return edit.TargetsInclude(includeId) &&
                               edit.ForcesIncludeLineDirectiveWrappers();
                      });
}

bool RefoldIncludeSubtreeWorkClassifier::HasOrdinaryReplayTokens(
    uint64_t includeId) const {
  if (const auto *item = model_.GetIncludeById(includeId))
    return item->cover.end > item->cover.begin;
  return false;
}

} // namespace refold
} // namespace clang
