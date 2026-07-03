//===--- RefoldMacroPatchReusePhase.h --------------------------*- C++ -*-===//
//
// Same-root macro-patch reuse / merge / conflict utilities for the
// whole-cover orchestrator.
//
// Owns the four predicates + one helper struct that coordinate same-root
// patch reuse:
//
//   * `ValidateMergedDirectAndDagRootReplacement` — replay-validate that
//     a textually-merged invocation replacement still parses as the same
//     root invocation shape (formal-argument ranges line up, all fixed
//     non-argument spans match byte-for-byte).
//   * `IsConcreteSubtreeWitnessCohort` — predicate identifying concrete
//     subtree-backed witness cohorts (no lexical bridge, no passthrough
//     flatten, no deferred root args).
//   * `ConflictingConcreteSubtreeWitnesses` — true iff two same-root
//     concrete subtree witnesses disagree on an overlapping concrete
//     root-formal rewrite.  Deferred wrapper/stringify cohorts and
//     bridge-backed witnesses do NOT count as conflicts.
//   * `MergeCurrentRootWithExistingCallsitePatch` — try to compose a
//     candidate root patch with an already-accepted structure-preserving
//     callsite patch for the same root.  Updates the candidate in place
//     when the merge passes both text compatibility and root-envelope
//     replay; sets the planning context's whole-cover-forced flag when
//     the witnesses conflict.
//
// The fourth method internally consults the private
// `parseExpectedRootFormalSummary` helper that recovers per-formal
// old/new text pairs from a patch audit summary string.
//
// The phase has no back-reference to the planner.  Planner-side helpers
// (`GetMacroInvocationFormalArgContentRanges`) are reached through one
// std::function callback supplied at construction; the Clang
// `LangOptions` used by the lexer-driven audit-summary parser is held by
// reference.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDMACROPATCHREUSEPHASE_H
#define LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDMACROPATCHREUSEPHASE_H

#include "core/RefoldModel.h"
#include "edit/RefoldPatchTypes.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/StringRef.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace clang {

class LangOptions;

namespace refold {

struct RefoldMacroWholeCoverPlanningContext;

/// Admits reusable macro patches from earlier planning paths.
///
/// The phase checks owner/span compatibility, invocation surface preservation,
/// same-root string-rewrite conflicts, and same-pass reuse constraints before
/// an existing patch can participate in final selection.
class RefoldMacroPatchReusePhase {
public:
  /// Borrowed inputs needed by the patch-reuse phase.  All references
  /// must outlive the phase; the orchestrator owns all of them.
  struct Dependencies {
    /// Used by `parseExpectedRootFormalSummary` to lex the compact
    /// audit-summary syntax (`{0:'old'->'new', ...}`).
    const clang::LangOptions &lexLang;

    /// Delegates to
    /// `RefoldMacroPatchPlanner::GetMacroInvocationFormalArgContentRanges`.
    /// Used by the merge validator to re-parse both invocation spellings.
    std::function<std::optional<std::vector<std::pair<size_t, size_t>>>(
        const RefoldModel::MacroInvocation &, llvm::StringRef)>
        getMacroInvocationFormalArgContentRanges;
  };

  explicit RefoldMacroPatchReusePhase(Dependencies deps);

  /// Validate the textual merge of a direct root rewrite with a DAG-
  /// backed root rewrite.  The merged text must still parse as the same
  /// root invocation shape: every formal-argument content range must be
  /// recovered, and every fixed (non-argument) span must remain byte-
  /// for-byte identical.  Returns true on success.  Unlike the full DAG
  /// proof validator, this does NOT rebuild subtree semantics; it only
  /// confirms that combining the direct and DAG root replacements did
  /// not alter the root invocation envelope.
  bool ValidateMergedDirectAndDagRootReplacement(
      const RefoldModel::MacroInvocation &m, llvm::StringRef baseText,
      llvm::StringRef newText) const;

  /// True iff `patch` is a concrete subtree-backed witness cohort — i.e.
  /// neither a lexical bridge nor a passthrough flatten and with no
  /// deferred root-formal arguments.  Deferred wrapper/stringify cohorts
  /// represent the same root at a different abstraction level and are
  /// intentionally excluded so they cannot be mis-classified as
  /// conflicting concrete witnesses.
  bool IsConcreteSubtreeWitnessCohort(const MacroPatch &patch) const;

  /// True iff two same-root subtree-backed witnesses at the same
  /// concrete discharge level disagree on an overlapping concrete root-
  /// formal rewrite.  When this is true the orchestrator falls back to
  /// whole-cover realization instead of merging text and losing witness
  /// continuity.
  bool
  ConflictingConcreteSubtreeWitnesses(const RefoldModel::MacroInvocation &m,
                                      const MacroPatch &existing,
                                      const MacroPatch &candidate) const;

  /// Try to compose `candidate` with the existing structure-preserving
  /// callsite patch for the same root (if any) recorded in
  /// `planningCtx.reuseAdmissionCtx`.  When the witnesses are compatible
  /// and the merged text passes
  /// `ValidateMergedDirectAndDagRootReplacement`, the candidate's
  /// replacement (and related fields) are updated in place.  When the
  /// witnesses conflict, sets
  /// `planningCtx.conflictingConcreteSubtreeWitnessForcesWholeCover` to
  /// true and leaves the candidate unchanged.
  void MergeCurrentRootWithExistingCallsitePatch(
      RefoldMacroWholeCoverPlanningContext &planningCtx,
      MacroPatch &candidate) const;

private:
  /// Per-formal old/new text pair recovered from a patch audit summary.
  ///
  /// Same-root reuse uses this compact carrier to compare concrete root-formal
  /// rewrites from existing and candidate patches without rebuilding the full
  /// DAG certificate.  The pair is meaningful only in the context of the parsed
  /// formal index that owns it.
  struct FormalTextPair {
    std::string oldText;
    std::string newText;
  };

  /// Parse the compact expected-root-formal summary stored in patch
  /// audit metadata, for example `{0:'bill'->'bill', 1:'y'->'z'}`.
  /// Returns an empty map for malformed summaries (fail-closed).  Uses
  /// Clang's raw lexer so punctuation, numeric constants, and quoted
  /// payload tokens are recognized consistently with the rest of the
  /// refold pipeline.
  llvm::DenseMap<uint32_t, FormalTextPair>
  parseExpectedRootFormalSummary(llvm::StringRef summary) const;

  Dependencies deps_;
};

} // namespace refold
} // namespace clang

#endif // LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDMACROPATCHREUSEPHASE_H
