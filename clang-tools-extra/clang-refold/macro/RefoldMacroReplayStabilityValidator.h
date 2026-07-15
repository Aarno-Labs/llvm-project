//===--- RefoldMacroReplayStabilityValidator.h -----------------*- C++ -*-===//
//
// Final-admission replay-stability gates for structure-preserving macro
// patches.  The validator owns pure boolean verdicts over already-built
// candidates so patch construction and replay-admission checks remain separate.
//
// Owns the following gates (all consumed by
// `MacroCandidateReplayIsStableForFinalSelection`):
//
//   * `StructurePreservingCallsiteHasStableFormalSyntax` — the replacement
//     text is itself a syntactically well-formed callsite for this macro
//     (formal/actual arity is variadic-compatible).
//   * `ArgsOnlyWholeEnvelopeCandidateHasLiteralBodyReplay` — an args-only
//     whole-envelope claim has every fixed replacement-list body token
//     replayed literally in B (no second-guessing of complete replay
//     witnesses).
//   * `RootPreservingCandidateHasLiteralFixedRootBodyReplay` — a root-
//     preserving candidate leaves the root's own fixed body literal in B,
//     after excluding descendant covers and argument-dependent surfaces.
//   * `CallsiteReplayObservesActiveHeaderMacroState` — true (rejecting)
//     when a preserving callsite replay would observe an active header-
//     owned macro-state directive that whole-cover/materialized emission
//     must carry instead.
//   * `MacroCandidateReplayIsStableForFinalSelection` — orchestrator that
//     conjoins the four gates above with the subtree validator's
//     `ClaimedWholeEnvelopeIsReplaySafe`.
//
// The validator does NOT mutate state.  It reads model/topology/source
// facts and returns deterministic boolean verdicts.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDMACROREPLAYSTABILITYVALIDATOR_H
#define LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDMACROREPLAYSTABILITYVALIDATOR_H

#include "core/RefoldModel.h"
#include "edit/RefoldPatchTypes.h"
#include "macro/RefoldMacroOccurrenceProofValidator.h"
#include "source/RefoldToken.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"

namespace clang {

class LangOptions;

namespace refold {

class LineDirectiveInserter;
class RefoldMacroStateProof;
class RefoldMacroSubtreeReplayValidator;
class RefoldMacroTopology;
class RefoldPathIdentity;
class RefoldSourceMapper;

/// Validates that macro replay candidates preserve required sibling and
/// envelope surfaces.  The validator owns replay-stability checks that are
/// broader than occurrence ancestry but narrower than final candidate ranking.
class RefoldMacroReplayStabilityValidator {
public:
  /// Borrowed inputs needed during replay-stability validation.  All
  /// references must outlive the validator; the planner owns all of them.
  ///
  /// `aToks`/`bToks` are safe as `ArrayRef` because the engine sets the
  /// underlying storage once at engine construction and never resizes it.
  /// Late-populated vectors like `abTokHunks_` must not be captured as
  /// `ArrayRef` here because they may be filled after service construction.
  struct Dependencies {
    /// Model used to walk macro invocations and directives.
    const RefoldModel &model;
    /// Topology used when constructing `RefoldMacroOccurrenceProofValidator`
    /// on demand for descendant-cover discovery.
    const RefoldMacroTopology &macroTopology;
    /// Source mapper for A-to-B token-range projection and B-source slicing.
    const RefoldSourceMapper &sourceMapper;
    /// A-side preprocessor tokens.  Stable for the lifetime of the engine.
    llvm::ArrayRef<PPTok> aToks;
    /// B-side preprocessor tokens.  Stable for the lifetime of the engine.
    llvm::ArrayRef<PPTok> bToks;
    /// Language options used by the actual-recovery lexer.
    const clang::LangOptions &lexLang;
    /// Path identity for owner-include path comparison.
    const RefoldPathIdentity &pathIdentity;
    /// Line-directive inserter for absolute-path resolution.
    const LineDirectiveInserter &lineDirs;
    /// Macro-state proof service for directive-line recovery and
    /// replacement-observes-directive checks.
    RefoldMacroStateProof &macroStateProof;
    /// Subtree replay validator (owned by the planner) — consulted by
    /// `MacroCandidateReplayIsStableForFinalSelection`.
    const RefoldMacroSubtreeReplayValidator &subtreeReplayValidator;
  };

  explicit RefoldMacroReplayStabilityValidator(Dependencies deps);

  /// True when the replacement text is itself a syntactically well-formed
  /// callsite for `m` (variadic-compatible arity).
  bool StructurePreservingCallsiteHasStableFormalSyntax(
      const MacroPatch &patch, const RefoldModel::MacroInvocation &m) const;

  /// True when an args-only whole-envelope claim proves that every fixed
  /// replacement-list body surface still replays literally in B.
  bool ArgsOnlyWholeEnvelopeCandidateHasLiteralBodyReplay(
      const MacroSubtreeReplayValidationContext &ctx,
      const MacroPatch &patch) const;

  /// True when a root-preserving candidate leaves the root-owned fixed body
  /// literal in B, after excluding argument-dependent and descendant macro
  /// surfaces.
  bool RootPreservingCandidateHasLiteralFixedRootBodyReplay(
      const RefoldModel::MacroInvocation &m, llvm::StringRef baseInvText,
      const MacroPatch &patch) const;

  /// True (i.e. inadmissible) when a preserving callsite replay would
  /// observe an active header-owned macro-state directive that whole-
  /// cover/materialized emission must carry instead.
  bool CallsiteReplayObservesActiveHeaderMacroState(
      const RefoldModel::MacroInvocation &m, const MacroPatch &patch) const;

  /// Apply all final replay-stability gates used before whole-cover-family
  /// candidate admission and proof-lattice selection.
  bool MacroCandidateReplayIsStableForFinalSelection(
      const MacroSubtreeReplayValidationContext &ctx,
      const MacroPatch &patch) const;

private:
  Dependencies deps_;
};

} // namespace refold
} // namespace clang

#endif // LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDMACROREPLAYSTABILITYVALIDATOR_H
