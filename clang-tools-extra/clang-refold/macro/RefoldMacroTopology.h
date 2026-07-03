//===--- RefoldMacroTopology.h ---------------------------------*- C++ -*-===//
//
// Macro-topology service for clang-refold.
//
// This service owns derived indices over the producer-recorded macro invocation
// graph and small macro/conditional containment predicates that are shared by
// proof and planning modules.  The indices are built once from the immutable
// RefoldModel and then borrowed read-only by callers, so services no longer
// need privileged RefoldEngine access for macro lookup, root traversal, child
// traversal, conditional containment, or __COUNTER__ event normalization.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDMACROTOPOLOGY_H
#define LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDMACROTOPOLOGY_H

#include "core/RefoldModel.h"
#include "edit/RefoldPatchTypes.h"
#include "proof/RefoldOwnerStateTypes.h"
#include "source/RefoldSourceMapper.h"
#include "source/RefoldToken.h"
#include "util/RefoldPathIdentity.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/StringRef.h"

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace clang {
namespace refold {

/// Read-only macro graph/topology oracle for one refold run.
///
/// The topology service owns macro identity and traversal queries used by
/// higher-level patch orchestration.  It preserves the producer-recorded lookup
/// policy exactly: if a malformed model contains duplicate macro IDs, the first
/// producer record wins.
class RefoldMacroTopology {
public:
  using CounterOutputRange = std::pair<uint64_t, uint64_t>;

  RefoldMacroTopology(const RefoldModel &model, llvm::ArrayRef<PPTok> aToks,
                      llvm::ArrayRef<PPTok> bToks,
                      const RefoldSourceMapper &sourceMapper,
                      const RefoldPathIdentity &paths);

  /// Resolve a model-assigned macro invocation ID through the per-run index.
  const RefoldModel::MacroInvocation *
  FindMacroInvocationById(uint64_t macroId) const;

  /// Return all direct child invocations whose caller_macro_id is \p parentId.
  ///
  /// Children are returned in deterministic producer/model order so call sites
  /// that replay nested macro surfaces do not reintroduce traversal-order
  /// dependence.
  llvm::ArrayRef<const RefoldModel::MacroInvocation *>
  MacroChildrenOf(uint64_t parentId) const;

  /// Follow caller_macro_id links to the outermost resolved invocation.
  uint64_t GetRootMacroId(uint64_t macroId) const;

  /// True iff \p m is lexically contained in a producer-recorded #define
  /// extent.
  bool IsInvocationInsideDefineDirective(
      const RefoldModel::MacroInvocation &m) const;

  /// True iff the byte range is wholly inside the selected producer arm.
  bool SourceRangeInsideConditionalArm(uint64_t condArmId, llvm::StringRef file,
                                       std::optional<uint64_t> ownerIncludeId,
                                       uint64_t begin, uint64_t end) const;

  /// Return the producer-backed A-token ranges for one `__COUNTER__`
  /// invocation.
  ///
  /// Body spans are preferred over explicit spans, which are preferred over the
  /// conservative cover. Zero-width anchors are preserved here and widened by
  /// BuildCounterEventIdentity() so every caller uses one deterministic event
  /// normalizer.
  std::vector<CounterOutputRange> CounterOutputRangesForInvocation(
      const RefoldModel::MacroInvocation &macro) const;

  /// Format the A-side counter value represented by a normalized token range.
  std::string CounterValueForTokenRange(uint64_t begin, uint64_t end) const;

  /// Return the innermost patchable macro invocation that covers an A range.
  ///
  /// A candidate must have a real invocation-site span and must not be spelled
  /// inside a macro definition. Candidate ranking is deterministic: body-span
  /// coverage is preferred over argument-like coverage, which is preferred
  /// over broad cover coverage; ties
  /// use smaller span width and then lower producer macro id.
  const RefoldModel::MacroInvocation *SmallestCoveringPatchableMacro(
      uint64_t aStart, uint64_t aEnd,
      std::optional<uint64_t> ownerIncludeId = std::nullopt) const;

  /// Build the stable identity for one concrete `__COUNTER__` event.
  CounterEventIdentity BuildCounterEventIdentity(
      const RefoldModel::MacroInvocation &macro, uint64_t occurrenceOrdinal,
      uint64_t begin, uint64_t end,
      std::optional<uint64_t> ownerIncludeOverride = std::nullopt) const;

  /// Return true when a selected macro patch leaves the root callsite expanded
  /// in the final source rather than reconstructing the recorded invocation
  /// spelling.  This is a topology-level query because it depends only on the
  /// patch replacement and the producer macro invocation index.
  bool MacroPatchRemainsExpanded(const MacroPatch &patch) const;

private:
  struct DefineDirectiveExtent {
    uint64_t begin = 0;
    uint64_t end = 0;
  };

  void BuildMacroInvocationGraph();
  void BuildDefineDirectiveIndex() const;
  std::string ToAbsolutePath(llvm::StringRef spelledPath) const;

  const RefoldModel &model_;
  llvm::ArrayRef<PPTok> aToks_;
  const RefoldPathIdentity &paths_;
  llvm::DenseMap<uint64_t, const RefoldModel::MacroInvocation *>
      macroInvocationById_;
  llvm::DenseMap<uint64_t,
                 llvm::SmallVector<const RefoldModel::MacroInvocation *, 4>>
      macroChildrenById_;

  /// Per-run source-text cache used while building #define containment extents.
  /// It is intentionally not process-global because the producer working
  /// directory and directive spellings are model-specific.
  mutable llvm::StringMap<std::string> defineFileTextCache_;

  /// Cache widened #define end offsets by producer directive ID.
  mutable llvm::DenseMap<uint64_t, uint64_t> defineEndCache_;

  /// Index of widened #define extents keyed by replay-time absolute source
  /// path.
  mutable llvm::StringMap<std::vector<DefineDirectiveExtent>> definesByAbsPath_;

  /// True after the lazy #define containment index has been built for this run.
  mutable bool definesIndexBuilt_ = false;
};

} // namespace refold
} // namespace clang

#endif // LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDMACROTOPOLOGY_H
