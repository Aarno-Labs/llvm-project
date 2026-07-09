//===--- RefoldMacroOccurrenceProofValidator.cpp ----------------*- C++ -*-===//
//
// Macro subtree-membership / occurrence-proof predicates for clang-refold.
//
//===----------------------------------------------------------------------===//

#include "macro/RefoldMacroOccurrenceProofValidator.h"

#include "macro/RefoldMacroTopology.h"

#include "llvm/ADT/SmallVector.h"

#include <algorithm>

using namespace llvm;

namespace clang {
namespace refold {

bool RefoldMacroOccurrenceProofValidator::CurrentLevelInvocationIsInSubtreeOf(
    const RefoldModel::MacroInvocation &macro, uint64_t rootId) const {
  uint64_t currentId = macro.id;
  SmallVector<uint64_t, 8> seen;
  while (true) {
    if (currentId == rootId)
      return true;
    if (std::find(seen.begin(), seen.end(), currentId) != seen.end())
      return false;
    seen.push_back(currentId);

    const RefoldModel::MacroInvocation *current =
        (*deps_.macroTopology).FindMacroInvocationById(currentId);
    if (!current || !current->callerMacroId)
      return false;
    currentId = *current->callerMacroId;
  }
}

bool RefoldMacroOccurrenceProofValidator::
    CurrentLevelSubtreeContainsCounterInvocation(uint64_t rootId) const {
  if (!(*deps_.macroTopology).FindMacroInvocationById(rootId))
    return false;
  for (const RefoldModel::MacroInvocation &macro :
       (*deps_.model).GetMacroInvocations()) {
    if (macro.name != "__COUNTER__")
      continue;
    if (CurrentLevelInvocationIsInSubtreeOf(macro, rootId))
      return true;
  }
  return false;
}

std::optional<unsigned>
RefoldMacroOccurrenceProofValidator::CandidateDepthInValidatedSubtree(
    const MacroSubtreeReplayValidationContext &ctx,
    const RefoldModel::MacroInvocation &candidate) const {
  // Candidates must be strict descendants of the validated root.  The root
  // invocation itself is not a descendant candidate for this path.  The
  // returned depth is also the original DAG leaf tie-breaker, so it must be
  // computed from the same fail-closed ancestry walk as the membership test.
  unsigned depth = 0;
  std::optional<uint64_t> parent = candidate.callerMacroId;
  while (parent) {
    ++depth;
    if (*parent == ctx.rootInvocation.id)
      return depth;
    auto it = ctx.invocationById.find(*parent);
    if (it == ctx.invocationById.end())
      return std::nullopt;
    parent = it->second->callerMacroId;
  }
  return std::nullopt;
}

bool RefoldMacroOccurrenceProofValidator::CandidateBelongsToValidatedSubtree(
    const MacroSubtreeReplayValidationContext &ctx,
    const RefoldModel::MacroInvocation &candidate) const {
  return CandidateDepthInValidatedSubtree(ctx, candidate).has_value();
}

} // namespace refold
} // namespace clang
