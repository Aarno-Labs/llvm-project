//===--- RefoldMacroWholeCoverOrchestrator.h -----------------*- C++ -*-===//
//
// Whole-cover macro patch orchestrator for clang-refold.
//
// Owns whole-cover macro construction orchestration:
//   - ComputeWholeCoverPlan / WholeCoverPatchMatchesPlan: plan computation
//     and plan/patch consistency checks for reuse admission.
//   - TryCounterLiteralWholeCoverPatch: counter-state replay specialization.
//   - BuildMacroInvocationPatchWholeCover: the entry point coordinating
//     args-only replay, DAG-subtree lifting, existing-patch reuse, selector
//     substitution, whole-cover realization, and final selection.
//
// The orchestrator borrows a back-reference to RefoldMacroPatchPlanner so
// it can access planner-owned sub-services and the public planner helpers
// its phase services need (`Deps()`, `GetProofLattice()`,
// `GetOwnerStateProof()`, `RecoverWholeCoverReuseContext()`,
// `IsParenthesizedTuple()`, `TokenSpellingsEqualToA/B()`, etc.).  No
// friend-class relationship is used or needed.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDMACROWHOLECOVERORCHESTRATOR_H
#define LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDMACROWHOLECOVERORCHESTRATOR_H

#include "core/RefoldModel.h"
#include "edit/RefoldPatchTypes.h"
#include "macro/RefoldMacroArgsOnlyWholeCoverPhase.h"
#include "macro/RefoldMacroDAGLeafDiscoveryPhase.h"
#include "macro/RefoldMacroDAGLiftingPhase.h"
#include "macro/RefoldMacroFinalCandidateSelector.h"
#include "macro/RefoldMacroPatchReusePhase.h"
#include "macro/RefoldMacroSelectorSubstitutionPhase.h"
#include "proof/RefoldMacroPatchTypes.h"
#include "source/DiffAlgorithms.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/StringRef.h"

#include <cstdint>
#include <optional>
#include <utility>

namespace clang {
namespace refold {

class RefoldMacroPatchPlanner;

/// Existing patch already associated with the physical invocation span by the
/// caller before whole-cover planning begins.
///
/// The context borrows the patch selected by the caller together with the owner
/// and same-pass metadata needed to decide whether the patch can be reused.
struct ExistingMacroPatchContext {
  const MacroPatch *patch = nullptr;
  bool isCallsite = false;
};

/// Coordinates whole-cover macro planning for a root invocation.
///
/// The orchestrator runs direct args-only, DAG lifting, selector substitution,
/// reuse, and realization phases over a shared planning context, then delegates
/// final ranking to the macro candidate selector.
class RefoldMacroWholeCoverOrchestrator {
public:
  /// Construct the orchestrator from the planner dependency surface.
  ///
  /// Replay sub-services are owned by the orchestrator and initialized from
  /// the planner's public dependency bundle via `planner->Deps()`.  Every
  /// planner-side hook the orchestrator needs is reachable through an explicit
  /// public accessor.
  explicit RefoldMacroWholeCoverOrchestrator(
      const RefoldMacroPatchPlanner *planner);

  std::optional<WholeCoverPlan>
  ComputeWholeCoverPlan(const RefoldModel::MacroInvocation &m) const;

  bool WholeCoverPatchMatchesPlan(const MacroPatch &patch,
                                  const WholeCoverPlan &plan,
                                  uint64_t rootMacroId) const;

  std::optional<MacroPatch> TryCounterLiteralWholeCoverPatch(
      const RefoldModel::MacroInvocation &invocation,
      const diffutils::Hunk &hunk, uint64_t invocationStart,
      uint64_t invocationEnd) const;

  /// Coordinate whole-cover candidate construction and final selection.
  ///
  /// The signature mirrors the planner's public surface so delegation remains
  /// straightforward while the orchestrator owns the concrete replay/reuse
  /// sequencing.
  std::optional<MacroPatch> BuildMacroInvocationPatchWholeCover(
      const RefoldModel::MacroInvocation &m, const diffutils::Hunk &h,
      llvm::StringRef baseInvText,
      const llvm::DenseMap<std::optional<uint64_t>,
                           llvm::DenseMap<uint64_t, MacroPatch>> &patchMap)
      const;

  std::optional<MacroPatch> BuildMacroInvocationPatchWholeCover(
      const RefoldModel::MacroInvocation &m, const diffutils::Hunk &h,
      llvm::StringRef baseInvText,
      const llvm::DenseMap<std::optional<uint64_t>,
                           llvm::DenseMap<uint64_t, MacroPatch>> &patchMap,
      ExistingMacroPatchContext existingContext) const;

private:
  /// Try the recursive tuple-generated-callee theorem from a generated
  /// descendant back to a source-spelled caller ancestor.
  ///
  /// This is deliberately separate from ordinary args-only admission because a
  /// terminal generated callee can have a producer invocation range such as
  /// `ADD, (1, 2)`, which is not a source callsite and therefore never reaches
  /// the normal callsite-shaped args-only phase.
  std::optional<MacroPatch> TryRecursiveTupleGeneratedReplayFromCallerAncestor(
      const RefoldModel::MacroInvocation &invocation,
      const diffutils::Hunk &hunk) const;

  const RefoldMacroPatchPlanner *planner_;
  RefoldMacroArgsOnlyWholeCoverPhase argsOnlyPhase_;
  RefoldMacroDAGLeafDiscoveryPhase dagLeafDiscoveryPhase_;
  RefoldMacroDAGLiftingPhase dagLiftingPhase_;
  RefoldMacroPatchReusePhase patchReusePhase_;
  RefoldMacroSelectorSubstitutionPhase selectorSubstitutionPhase_;
  RefoldMacroFinalCandidateSelector finalCandidateSelector_;
};

} // namespace refold
} // namespace clang

#endif // LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDMACROWHOLECOVERORCHESTRATOR_H
