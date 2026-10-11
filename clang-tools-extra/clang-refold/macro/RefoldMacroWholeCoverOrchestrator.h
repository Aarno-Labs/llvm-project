//===--- RefoldMacroWholeCoverOrchestrator.h -----------------*- C++ -*-===//
//
// Whole-cover macro patch orchestrator for clang-refold.
//
// Owns whole-cover macro construction orchestration:
//   - TryCounterLiteralWholeCoverPatch: counter-state replay specialization.
//   - BuildMacroInvocationPatchWholeCover: the entry point coordinating
//     args-only replay, DAG-subtree lifting, existing-patch reuse, selector
//     substitution, whole-cover realization, and final selection.
//
// Plan computation itself is not here: it is
// `RefoldMacroWholeCoverPlanBuilder`, which depends on neither the planner
// nor the proof services and is therefore constructible before both.  The
// orchestrator forwards the engine-owned builder to the phase services that
// need a plan.
//
// The orchestrator borrows a back-reference to RefoldMacroPatchPlanner so
// it can access planner-owned sub-services and the public planner helpers
// its phase services need (`Deps()`, `GetOwnerStateProof()`,
// `RecoverWholeCoverReuseContext()`, `IsParenthesizedTuple()`,
// `TokenSpellingsEqualToA/B()`, etc.).  No friend-class relationship is used or
// needed.  The planner builds a temporary orchestrator for each whole-cover
// call, after its own construction is complete, so the planner's sub-services
// already exist: the args-only entry point is reached through the borrowed
// `RefoldMacroStandardArgsOnlyPatchBuilder`, by the recursive-tuple ancestor
// probe and by the phases alike.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDMACROWHOLECOVERORCHESTRATOR_H
#define LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDMACROWHOLECOVERORCHESTRATOR_H

#include "edit/RefoldPatchTypes.h"
#include "macro/RefoldMacroArgsOnlyWholeCoverPhase.h"
#include "macro/RefoldMacroDAGLeafDiscoveryPhase.h"
#include "macro/RefoldMacroDAGLiftingPhase.h"
#include "macro/RefoldMacroFinalCandidateSelector.h"
#include "macro/RefoldMacroPatchReusePhase.h"
#include "macro/RefoldMacroSelectorSubstitutionPhase.h"
#include "model/RefoldModel.h"
#include "proof/RefoldMacroPatchTypes.h"
#include "source/RefoldDiffTypes.h"

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

  /// Rewrite, in source, the actuals that a call formed by \p root's
  /// expansion read from after \p root's callsite, when they alone explain
  /// \p hunk and every other hunk touching \p root's cover.
  ///
  /// `#define CALL(f, a) int k = f(a` leaves `ADD(1` open, and in
  /// `CALL(ADD, 1), 2);` the call reads `, 2)` from source, so `ADD`'s `b` is
  /// the `2` there; a chain such as `PICK()(10)` is the same shape.  The
  /// producer locates such an actual in `use_arg_ranges` and credits each
  /// copy of it to its formal.  Admission requires that one such call explain
  /// every hunk touching the root's cover through the occurrences of its
  /// actuals located after the root's callsite; that each derived actual
  /// replay every occurrence of its formal, stay one actual, and name no
  /// macro; that no other cover token depend on a rewritten formal; and that
  /// no other invocation and no preprocessing structure lie in the rewritten
  /// bytes.  The patch covers only those bytes, keeping every callsite, under
  /// a `CallChainSuffix` proof rooted at \p root.  Only a translation-unit
  /// root atop its caller chain is tried; an actual inside its callsite is
  /// left to the root's own args-only proofs.
  std::optional<MacroPatch>
  TryCallChainSuffixReplay(const RefoldModel::MacroInvocation &root,
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
