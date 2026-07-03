//===--- RefoldMacroDAGLiftingPhase.h --------------------------*- C++ -*-===//
//
// Lifting half of the DAG-chained args-only attempt for the whole-cover
// orchestrator.
//
// Runs after `RefoldMacroDAGLeafDiscoveryPhase` publishes a
// `DAGLeafDiscoveryResult` (leaf candidates, split-insertion root
// candidates, the invocation lookup map, the root callsite parsing, and
// the per-formal root argument cache).  `Run(planningCtx, discoveryResult)`
// takes the discovery result plus the planning context and runs the
// generalized single-parent lifting algorithm — argument-local template
// construction, invertibility-certificate solving, unique-realization
// proofs, caller parameter lifting, root-formal merge, and subtree
// certificate construction — to produce the final lifted root patch.
//
// The concrete text/lexical mechanics live in five sub-services (text
// primitives, invertibility solver, structured lifter, subtree certifier,
// candidate validator) constructed in the ctor.
//
// The phase has no back-reference to the planner.  Planner-side helpers
// that remain on the planner
// (`GetMacroInvocationFormalArgContentRanges`,
// `MacroArgReplacementMatchesAllOccurrencesInBIgnorePasteSemanticProof`)
// are reached through std::function callbacks supplied at construction;
// the proof lattice is passed by reference and the
// `RefoldMacroPasteArgumentBuilder` is constructed on demand from the
// phase's own dependencies.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDMACRODAGLIFTINGPHASE_H
#define LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDMACRODAGLIFTINGPHASE_H

#include "core/RefoldModel.h"
#include "edit/RefoldPatchTypes.h"
#include "macro/RefoldMacroDAGCandidateValidator.h"
#include "macro/RefoldMacroDAGInvertibilitySolver.h"
#include "macro/RefoldMacroDAGLiftingContext.h"
#include "macro/RefoldMacroDAGStructuredLifter.h"
#include "macro/RefoldMacroDAGSubtreeCertifier.h"
#include "macro/RefoldMacroDAGTextPrimitives.h"
#include "macro/RefoldMacroPasteArgumentBuilder.h"
#include "source/DiffAlgorithms.h"
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

class LangOptions;

namespace refold {

struct DAGLeafDiscoveryResult;
struct RefoldMacroWholeCoverPlanningContext;
class LineDirectiveInserter;
class RefoldArgTextRecovery;
class RefoldMacroTopology;
class RefoldProofLattice;
class RefoldSourceMapper;

/// Coordinates DAG leaf lifting from discovered nested candidates to root
/// macro patches.  It builds the lifting context, invokes invertibility and
/// subtree certification services, and records root candidates for final
/// macro selection.
class RefoldMacroDAGLiftingPhase {
public:
  /// Borrowed inputs needed by the DAG lifting phase.  All references
  /// must outlive the phase; the orchestrator owns all of them.
  struct Dependencies {
    const RefoldModel &model;
    const RefoldSourceMapper &sourceMapper;
    const clang::LangOptions &lexLang;
    const LineDirectiveInserter &lineDirs;
    const RefoldArgTextRecovery &argTextRecovery;
    const RefoldMacroTopology &macroTopology;
    llvm::ArrayRef<PPTok> aToks;
    llvm::ArrayRef<PPTok> bToks;
    RefoldProofLattice &proofLattice;

    /// Delegates to
    /// `RefoldMacroPatchPlanner::GetMacroInvocationFormalArgContentRanges`.
    /// Used to re-parse a caller invocation's formal-argument byte ranges
    /// during multi-hop lifting.
    std::function<std::optional<std::vector<std::pair<size_t, size_t>>>(
        const RefoldModel::MacroInvocation &, llvm::StringRef)>
        getMacroInvocationFormalArgContentRanges;

    /// Delegates to
    /// `RefoldMacroPatchPlanner::MacroArgReplacementMatchesAllOccurrencesInBIgnorePasteSemanticProof`.
    /// Used by the unique-realization audit to confirm that a candidate
    /// per-formal replacement matches every occurrence in B even when
    /// paste-derived spans are present.
    std::function<bool(const RefoldModel::MacroInvocation &, uint32_t,
                       llvm::StringRef, llvm::StringRef,
                       llvm::ArrayRef<diffutils::Hunk>)>
        macroArgReplacementMatchesAllOccurrencesInBIgnorePasteSemanticProof;

    /// Delegates to `RefoldMacroWholeCoverOrchestrator::ComputeWholeCoverPlan`.
    /// Used to compute the whole-cover plan for a candidate lifted
    /// invocation during selector-replacement reasoning.
    std::function<std::optional<WholeCoverPlan>(
        const RefoldModel::MacroInvocation &)>
        computeWholeCoverPlan;
  };

  explicit RefoldMacroDAGLiftingPhase(Dependencies deps);

  /// Run the lifting half against the orchestrator-built planning context
  /// and the discovery result produced by `RefoldMacroDAGLeafDiscoveryPhase`.
  /// Returns the lifted root candidate when one is produced; nullopt
  /// otherwise (the orchestrator falls through to the next candidate
  /// family in that case).
  std::optional<MacroPatch>
  Run(const RefoldMacroWholeCoverPlanningContext &planningCtx,
      const DAGLeafDiscoveryResult &discoveryResult) const;

private:
  /// Construct the on-demand paste-argument builder from this phase's
  /// own dependencies, mirroring the planner's
  /// `PasteArgumentBuilder()` accessor without back-referencing the
  /// planner.
  RefoldMacroPasteArgumentBuilder pasteArgumentBuilder() const;

  Dependencies deps_;

  /// Foundation text/parsing primitives used by every DAG-lifting subservice.
  ///
  /// Owned by value so the lifting phase constructs them once and reuses them
  /// across candidate attempts.
  RefoldMacroDAGTextPrimitives textPrimitives_;

  /// Semantic-interaction proof engine.
  ///
  /// Holds the argument-invertibility, semantic-rewrite, raw/paste-validation,
  /// semantic-interaction consistency, and formal-rewrite certificate builders.
  RefoldMacroDAGInvertibilitySolver invertibilitySolver_;

  /// Structured hop-lifting engine.
  ///
  /// Owns invocation, structured-lift, lift-chain, root-formal-merge
  /// certificate builders and the per-hop parent-constraint derivation
  /// primitives.
  RefoldMacroDAGStructuredLifter structuredLifter_;

  /// Subtree-rewrite certifier.
  ///
  /// Combines hop-level lifts and semantic-interaction evidence into one
  /// subtree-wide certificate.
  RefoldMacroDAGSubtreeCertifier subtreeCertifier_;

  /// Final DAG-candidate validator.
  ///
  /// Turns a subtree-rewrite certificate into a concrete root `MacroPatch`,
  /// validates it against B, and merges it into the acceptance context.
  RefoldMacroDAGCandidateValidator candidateValidator_;
};

} // namespace refold
} // namespace clang

#endif // LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDMACRODAGLIFTINGPHASE_H
