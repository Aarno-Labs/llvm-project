//===--- RefoldMacroArgsOnlyWholeCoverPhase.h ----------------*- C++ -*-===//
//
// Direct-root args-only phase of the whole-cover orchestrator.
//
// This service owns the direct-root args-only candidate computation that
// runs at the top of `BuildMacroInvocationPatchWholeCover`.  Its single
// `Run` entry point consumes the per-call
// `RefoldMacroWholeCoverPlanningContext` and updates its mutable fields
// in place:
//
//   * `argsOnlyCandidate`            — direct root args-only patch when
//                                       one was producible
//   * `reuseExistingCallsitePatch`   — deferred-reuse signal when
//                                       args-only declined but an
//                                       existing callsite patch already
//                                       covers the trimmed hunk
//   * `rootHasDirectArgLikeSurface`  — surface fact consumed by later
//                                       DAG and reuse arbitration
//
// Scope boundary: the phase does NOT run standard args-only construction
// (that lives in `RefoldMacroStandardArgsOnlyPatchBuilder` and is called
// through the callback below), and does NOT run DAG lifting or
// whole-cover realization (each is owned by its own phase service).
//
// The phase has no back-reference to the planner.  Planner-side helpers
// that remain on the planner (`BuildMacroInvocationPatchArgsOnly`, which is
// the args-only ENTRY POINT shared with another call site) are reached
// through one std::function callback supplied at construction.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDMACROARGSONLYWHOLECOVERPHASE_H
#define LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDMACROARGSONLYWHOLECOVERPHASE_H

#include "core/RefoldModel.h"
#include "edit/RefoldPatchTypes.h"
#include "source/DiffAlgorithms.h"
#include "source/RefoldToken.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"

#include <functional>
#include <optional>
#include <vector>

namespace clang {
namespace refold {

struct RefoldMacroWholeCoverPlanningContext;
class RefoldProofLattice;
class RefoldSourceMapper;

/// Runs the direct args-only whole-cover admission phase for one planning
/// context.  It owns the fast path that preserves a root invocation through
/// argument-like spans before DAG lifting or selector substitution are tried.
class RefoldMacroArgsOnlyWholeCoverPhase {
public:
  /// Borrowed inputs needed by the args-only phase.  All references must
  /// outlive the phase; the planner owns all of them.
  ///
  /// `abTokHunks` is held by vector reference (not `ArrayRef`) because the
  /// engine populates it after the planner — and therefore this phase — is
  /// constructed; see `feedback_arrayref_captures.md`.
  struct Dependencies {
    const RefoldSourceMapper &sourceMapper;
    llvm::ArrayRef<PPTok> aToks;
    llvm::ArrayRef<PPTok> bToks;
    const std::vector<diffutils::Hunk> &abTokHunks;
    RefoldProofLattice &proofLattice;

    /// Delegates to
    /// `RefoldMacroPatchPlanner::BuildMacroInvocationPatchArgsOnly`. That
    /// method orchestrates actual-layout recovery and dispatches to paste-aware
    /// / standard args-only builders; it stays on the planner because the
    /// whole-cover orchestrator also calls it from a separate site.
    std::function<std::optional<MacroPatch>(
        const RefoldModel::MacroInvocation &, const diffutils::Hunk &,
        llvm::StringRef)>
        buildMacroInvocationPatchArgsOnly;
  };

  explicit RefoldMacroArgsOnlyWholeCoverPhase(Dependencies deps);

  /// Run the direct root args-only phase against the supplied planning
  /// context.  Updates `argsOnlyCandidate`, `reuseExistingCallsitePatch`,
  /// and `rootHasDirectArgLikeSurface` on the context.  Does not return a
  /// value; the orchestrator reads the updated context fields when
  /// dispatching the next phase.
  void Run(RefoldMacroWholeCoverPlanningContext &planningCtx) const;

private:
  /// Paired-pure-insertion fallback for the direct root args-only path.
  /// This phase owns the full candidate-construction logic for the paired
  /// insertion case.  Returns nullopt for a non-terminal
  /// miss.
  std::optional<MacroPatch> TryPairedPureInsertionRootArgsOnly(
      const RefoldMacroWholeCoverPlanningContext &planningCtx) const;

  Dependencies deps_;
};

} // namespace refold
} // namespace clang

#endif // LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDMACROARGSONLYWHOLECOVERPHASE_H
