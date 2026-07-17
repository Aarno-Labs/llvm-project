//===--- RefoldMacroSelectorSubstitutionPhase.h ----------------*- C++ -*-===//
//
// Callee-substitution macro fallback phase.
//
// When the direct replay paths decline to produce a root candidate, the
// orchestrator gives this phase a chance to build a structure-preserving root
// patch by rewriting only the callee-selection surface of an invocation.  The
// phase supports two conservative proof shapes:
//
// * direct literal callee substitution, such as `ADD(5, 20)` -> `SUB(5, 20)`,
//   or `ADD(5, 20)` -> `SUB(15, 24)` when the same cover also contains
//   argument edits, after proving that the original active definition exactly
//   replays A under A-side actuals and a unique active alternate definition
//   exactly replays B under B-side actuals;
// * paste-derived selector substitution, such as `DISPATCH(ONE, 10)` ->
//   `DISPATCH(TWO, 10)`, after proving that a descendant pasted callee can be
//   selected by replacing one producer-identified root argument slice.
//
// Both paths are fail-closed.  They require exact A/B replay, active source
// definitions, deterministic uniqueness, and unchanged unmodelled fixed bytes.
// The paste-derived path also uses the same root invocation merge validator as
// the other macro preservation candidates; the direct path uses a callee/argument
// slot shape validator because changing the root callee token and any
// producer-mapped argument slots is the point of that proof.
//
// The phase has no back-reference to the planner.  Planner-side helpers
// (`GetMacroInvocationFormalArgContentRanges`, `TokenSpellingsEqualToA`,
// `TokenSpellingsEqualToB`) and the same-root merge validator (a
// `RefoldMacroPatchReusePhase` method) are reached through std::function
// callbacks supplied at construction.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDMACROSELECTORSUBSTITUTIONPHASE_H
#define LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDMACROSELECTORSUBSTITUTIONPHASE_H

#include "core/RefoldModel.h"
#include "edit/RefoldPatchTypes.h"
#include "source/RefoldToken.h"

#include "llvm/ADT/ArrayRef.h"
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
class RefoldProofLattice;
class RefoldSourceMapper;

/// Attempts callee-substitution macro candidates for one whole-cover
/// planning context.  The phase is used after direct replay paths fail to
/// produce a preserving candidate and before final whole-cover realization is
/// forced.  It covers both direct callee-name substitution (`ADD(...)` ->
/// `SUB(...)`) and the older paste-derived selector substitution path.
class RefoldMacroSelectorSubstitutionPhase {
public:
  /// Borrowed inputs needed by the callee-substitution phase.  All
  /// references must outlive the phase; the orchestrator owns all of
  /// them.
  struct Dependencies {
    const RefoldModel &model;
    const RefoldSourceMapper &sourceMapper;
    llvm::ArrayRef<PPTok> aToks;
    llvm::ArrayRef<PPTok> bToks;
    RefoldProofLattice &proofLattice;

    /// Delegates to
    /// `RefoldMacroPatchPlanner::GetMacroInvocationFormalArgContentRanges`.
    std::function<std::optional<std::vector<std::pair<size_t, size_t>>>(
        const RefoldModel::MacroInvocation &, llvm::StringRef)>
        getMacroInvocationFormalArgContentRanges;

    /// Delegates to `RefoldMacroPatchPlanner::TokenSpellingsEqualToA`.
    std::function<bool(llvm::ArrayRef<std::string>, uint64_t, uint64_t)>
        tokenSpellingsEqualToA;

    /// Delegates to `RefoldMacroPatchPlanner::TokenSpellingsEqualToB`.
    std::function<bool(llvm::ArrayRef<std::string>, uint64_t, uint64_t)>
        tokenSpellingsEqualToB;

    /// Delegates to
    /// `RefoldMacroPatchReusePhase::ValidateMergedDirectAndDagRootReplacement`.
    /// Used by paste-derived selector substitution to confirm that a
    /// selector-rewritten replacement still preserves the root invocation
    /// envelope.  Direct callee substitution uses its own callee/argument
    /// shape validator because it deliberately changes the fixed callee span
    /// and may compose producer-mapped argument edits.
    std::function<bool(const RefoldModel::MacroInvocation &, llvm::StringRef,
                       llvm::StringRef)>
        validateMergedDirectAndDagRootReplacement;
  };

  explicit RefoldMacroSelectorSubstitutionPhase(Dependencies deps);

  /// Try to build a structure-preserving root patch via direct or
  /// paste-derived callee substitution.  Returns nullopt when the proof
  /// preconditions don't hold, when no candidate matches, or when multiple
  /// candidates disagree on the rewritten root replacement (treated as
  /// ambiguous).
  std::optional<MacroPatch>
  Run(const RefoldMacroWholeCoverPlanningContext &planningCtx) const;

private:
  /// Attempt the direct literal-callee proof for the current invocation.
  /// This rewrites the callsite callee token, and when the same macro cover
  /// also contains argument edits, rewrites only the producer-recovered
  /// argument slots.  The proof first replays the original active definition
  /// over A actuals, then replays a unique active alternate definition over B
  /// actuals.
  std::optional<MacroPatch> TryDirectCalleeSubstitution(
      const RefoldMacroWholeCoverPlanningContext &planningCtx,
      llvm::StringRef invSpanText) const;

  Dependencies deps_;
};

} // namespace refold
} // namespace clang

#endif // LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDMACROSELECTORSUBSTITUTIONPHASE_H
