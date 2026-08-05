//===--- RefoldMacroFinalCandidateSelector.h ------------------*- C++ -*-===//
//
// Final macro candidate selector service.
//
// Runs after the orchestrator has populated the per-call planning
// context's candidate slots (direct args-only, DAG-root replay, selector
// substitution, existing-callsite reuse, existing-expanded reuse,
// whole-cover realization).  `Run(planningCtx)` admits the surviving
// candidates, ranks them through the proof lattice, and certifies the
// winner, returning the final `MacroPatch` or `std::nullopt` when no
// candidate survives final admission.
//
// The selector reaches planner-side helpers exclusively through
// `RefoldMacroPatchPlanner`'s public accessors (`Deps`, `GetProofLattice`,
// `GetOwnerStateProof`, `RecoverWholeCoverReuseContext`,
// `IsParenthesizedTuple`, `TokenSpellingsEqualToA/B`) plus the
// orchestrator-owned `RefoldMacroPatchReusePhase`, proof certifier, and
// replay-stability validator supplied through Dependencies.  No friend
// access is used or needed.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDMACROFINALCANDIDATESELECTOR_H
#define LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDMACROFINALCANDIDATESELECTOR_H

#include "core/RefoldModel.h"
#include "edit/RefoldPatchTypes.h"
#include "macro/RefoldMacroOccurrenceProofValidator.h"
#include "proof/RefoldAcceptedResultTypes.h"

#include "llvm/ADT/StringRef.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <utility>
#include <vector>

namespace clang {
namespace refold {

struct RefoldMacroWholeCoverPlanningContext;
class RefoldMacroPatchProofCertifier;
class RefoldMacroPatchReusePhase;
class RefoldMacroReplayStabilityValidator;
class RefoldMacroTopology;
class RefoldProofLattice;

/// Final whole-cover-family candidate selector.
///
/// The selector is constructed once by the orchestrator with borrowed proof
/// services and a small set of planner-callback hooks; `Run` consumes the
/// already-populated planning context and returns the selected patch.
class RefoldMacroFinalCandidateSelector {
public:
  /// Borrowed inputs needed by the final candidate selector.  All
  /// references must outlive the selector; the orchestrator owns them.
  struct Dependencies {
    const RefoldModel &model;
    const RefoldMacroTopology &topology;
    RefoldProofLattice &proofLattice;
    const RefoldMacroReplayStabilityValidator &replayStabilityValidator;
    const RefoldMacroPatchProofCertifier &proofCertifier;
    const RefoldMacroPatchReusePhase &patchReusePhase;

    /// Root invocations ruled out from keeping their callsite, or null when
    /// none are.  Borrowed from the planner's dependency bundle.
    const llvm::DenseSet<uint64_t> *ownersMustExpand = nullptr;

    /// Compute the whole-cover replacement plan for an invocation.
    /// Wraps `RefoldMacroWholeCoverOrchestrator::ComputeWholeCoverPlan`.
    std::function<std::optional<WholeCoverPlan>(
        const RefoldModel::MacroInvocation &)>
        computeWholeCoverPlan;

    /// Confirm that an existing whole-cover patch still matches the
    /// currently computed plan for the same proof root.  Wraps
    /// `RefoldMacroWholeCoverOrchestrator::WholeCoverPatchMatchesPlan`.
    std::function<bool(const MacroPatch &, const WholeCoverPlan &, uint64_t)>
        wholeCoverPatchMatchesPlan;

    /// Delegates to `RefoldMacroPatchPlanner::MacroPatchOwnerMatches`.
    std::function<bool(const MacroPatch &, const Owner &)>
        macroPatchOwnerMatches;

    /// Delegates to `RefoldMacroPatchPlanner::IsParenthesizedTuple`.
    std::function<bool(llvm::StringRef)> isParenthesizedTuple;

    /// Delegates to
    /// `RefoldMacroPatchPlanner::GetMacroInvocationFormalArgContentRanges`.
    std::function<std::optional<std::vector<std::pair<size_t, size_t>>>(
        const RefoldModel::MacroInvocation &, llvm::StringRef)>
        getMacroInvocationFormalArgContentRanges;
  };

  explicit RefoldMacroFinalCandidateSelector(Dependencies deps);

  /// Run final candidate admission + ranking over the planning context
  /// that the earlier phases have already populated.  Reads the candidate
  /// slots (`argsOnlyCandidate`, `dagRootCandidate`), the conflict flags
  /// (`directRootPreservationInadmissible`,
  /// `conflictingConcreteSubtreeWitnessForcesWholeCover`,
  /// `reuseExistingCallsitePatch`,
  /// `existingCallsitePatchAbsorbedByDirectCandidate`), and the reuse
  /// admission context.  May mutate the candidate slots and reuse flags
  /// as part of the pre-selection direct/existing merge.
  std::optional<MacroPatch>
  Run(RefoldMacroWholeCoverPlanningContext &planningCtx) const;

private:
  Dependencies deps_;
};

} // namespace refold
} // namespace clang

#endif // LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDMACROFINALCANDIDATESELECTOR_H
