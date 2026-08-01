//===--- RefoldMacroOccurrenceProofValidator.cpp ----------------*- C++ -*-===//
//
// Macro subtree-membership / occurrence-proof predicates for clang-refold.
//
//===----------------------------------------------------------------------===//

#include "macro/RefoldMacroOccurrenceProofValidator.h"

#include "macro/RefoldMacroTopology.h"

namespace clang {
namespace refold {

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
