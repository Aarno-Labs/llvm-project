//===--- RefoldMacroSelectorSubstitutionPhase.h ----------------*- C++ -*-===//
//
// Paste-derived callee-selector substitution phase.
//
// When the DAG-chained args-only path declines to produce a root
// candidate, the orchestrator gives this phase a chance to build a
// structure-preserving root patch by rewriting the selector argument
// that picks the descendant callee.
//
// Proof shape: a descendant macro callee was produced by token pasting,
// the edited surface changes body-owned tokens of the selected callee,
// and another *already-active* macro reachable through the same paste
// expression exactly explains B under the same non-selector arguments.
// The only emitted source edit is a root invocation argument
// replacement, such as `DISPATCH(ONE, 10)` -> `DISPATCH(TWO, 10)`.
//
// The proof is fail-closed: there must be exactly one active alternate
// macro target, the original target must reproduce the A cover using
// the same local expansion model, the alternate target must reproduce
// the B cover, and the pasted-name witness must identify one unique
// root selector argument to rewrite.
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

/// Attempts selector-substitution macro candidates for one whole-cover
/// planning context.  The phase is used after direct replay paths fail to
/// produce a preserving candidate and before final whole-cover realization is
/// forced.
class RefoldMacroSelectorSubstitutionPhase {
public:
  /// Borrowed inputs needed by the selector substitution phase.  All
  /// references must outlive the phase; the orchestrator owns all of
  /// them.
  struct Dependencies {
    const RefoldModel &model;
    const RefoldSourceMapper &sourceMapper;
    llvm::ArrayRef<PPTok> aToks;
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
    /// Used to confirm that a selector-rewritten replacement still
    /// preserves the root invocation envelope.
    std::function<bool(const RefoldModel::MacroInvocation &, llvm::StringRef,
                       llvm::StringRef)>
        validateMergedDirectAndDagRootReplacement;
  };

  explicit RefoldMacroSelectorSubstitutionPhase(Dependencies deps);

  /// Try to build a structure-preserving root patch via paste-derived
  /// selector substitution.  Returns nullopt when the proof preconditions
  /// don't hold, when no candidate matches, or when multiple candidates
  /// disagree on the rewritten root replacement (treated as ambiguous).
  std::optional<MacroPatch>
  Run(const RefoldMacroWholeCoverPlanningContext &planningCtx) const;

private:
  Dependencies deps_;
};

} // namespace refold
} // namespace clang

#endif // LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDMACROSELECTORSUBSTITUTIONPHASE_H
