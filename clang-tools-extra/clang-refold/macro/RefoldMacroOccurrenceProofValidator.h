//===--- RefoldMacroOccurrenceProofValidator.h ----------------*- C++ -*-===//
//
// Macro subtree-membership / occurrence-proof predicates for clang-refold.
//
// This service owns the small, read-only predicates used by candidate
// admission and DAG validation: ancestry walks, descendant-depth queries,
// and counter-invocation containment. These predicates have no mutable
// state; they take borrowed model and topology dependencies and answer
// boolean / depth queries deterministically.
//
// Replay-stability predicates such as paste/stringify/sibling-surface
// validation and whole-envelope replay safety live in the replay-stability
// and subtree validators.  This service is intentionally limited to the
// lightweight membership-only queries to keep its API surface tight.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDMACROOCCURRENCEPROOFVALIDATOR_H
#define LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDMACROOCCURRENCEPROOFVALIDATOR_H

#include "core/RefoldModel.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/StringRef.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>

namespace clang {
namespace refold {

class RefoldMacroTopology;

/// Root-scoped state shared by DAG/subtree replay validation helpers.
///
/// The DAG path proves a descendant rewrite by lifting it through producer
/// caller edges back to one root invocation.  This carrier names that root
/// surface and the caller-owned invocation index; it borrows storage only
/// and does not promote per-call solver state into planner fields.
struct MacroSubtreeReplayValidationContext {
  const RefoldModel::MacroInvocation &rootInvocation;
  llvm::StringRef rootInvocationText;
  llvm::ArrayRef<std::pair<size_t, size_t>> rootInvocationArgRanges;
  const llvm::DenseMap<uint64_t, const RefoldModel::MacroInvocation *>
      &invocationById;
};

/// Answers read-only occurrence and subtree-membership proof predicates.
///
/// The validator checks producer ancestry, descendant depth, and counter
/// containment for macro DAG/subtree admission.  It does not mutate planning
/// state or perform replay-surface validation.
class RefoldMacroOccurrenceProofValidator {
public:
  struct Dependencies {
    const RefoldModel *model = nullptr;
    const RefoldMacroTopology *macroTopology = nullptr;
  };

  explicit RefoldMacroOccurrenceProofValidator(Dependencies deps)
      : deps_(std::move(deps)) {}

  /// Return whether `macro` lies in the producer caller ancestry chain that
  /// terminates at `rootId`.  Fail-closed: broken or cyclic ancestry returns
  /// false.
  bool
  CurrentLevelInvocationIsInSubtreeOf(const RefoldModel::MacroInvocation &macro,
                                      uint64_t rootId) const;

  /// Return whether the subtree rooted at `rootId` contains any
  /// `__COUNTER__` invocation. Used to gate counter-stabilization replay.
  bool CurrentLevelSubtreeContainsCounterInvocation(uint64_t rootId) const;

  /// Return the strict descendant distance from `candidate` to the validated
  /// root, or nullopt if the chain breaks before reaching it.  The root
  /// invocation itself is not a descendant.
  std::optional<unsigned> CandidateDepthInValidatedSubtree(
      const MacroSubtreeReplayValidationContext &ctx,
      const RefoldModel::MacroInvocation &candidate) const;

  /// Predicate form of CandidateDepthInValidatedSubtree.
  bool CandidateBelongsToValidatedSubtree(
      const MacroSubtreeReplayValidationContext &ctx,
      const RefoldModel::MacroInvocation &candidate) const;

private:
  Dependencies deps_;
};

} // namespace refold
} // namespace clang

#endif // LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDMACROOCCURRENCEPROOFVALIDATOR_H
