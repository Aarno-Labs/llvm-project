//===--- RefoldAcceptedResultRanker.h ---------------------------*- C++ -*-===//
//
// Accepted-result ranking and selection service for clang-refold.
//
// Owns the per-attempt comparators and selection orchestration used to pick
// a single representative accepted-result candidate (or macro-selection
// candidate) from a set of selectable proofs:
//
//   * `LatticePrefers` — coarse proof-summary ordering: explicit theorem
//     tie-breakers, selection-preference rank, surface-disposition rank,
//     mixed-owner cover comparison, then deterministic enum fallbacks.
//   * `AcceptedResultCandidate*Prefers` and `MacroSelectionCandidatePrefers`
//     — candidate-level comparators that compose `LatticePrefers` with a
//     canonical artifact-shape tie-breaker.
//   * `IsSelectable*` — admission gates that delegate to the lattice's proof
//     normalizer (via a borrowed callback) plus the static selector-only
//     nested-macro failure detector.
//   * `SelectPreferred*` — orchestration entry points that run the legacy
//     canonical selector AND the central witness resolver, then choose the
//     final index and emit the appropriate trace records.
//
// The ranker explicitly does NOT own proof-summary construction or witness
// equivalence-key building; it consumes those through `RefoldProofLattice`
// (via the `normalizeAcceptedProof` callback) and `RefoldWitnessResolver`.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDACCEPTEDRESULTRANKER_H
#define LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDACCEPTEDRESULTRANKER_H

#include "proof/RefoldAcceptedResultTypes.h"
#include "proof/RefoldProofVocabulary.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/STLFunctionalExtras.h"

#include <cstddef>
#include <functional>
#include <optional>

namespace clang {
namespace refold {

class RefoldTheoremAudit;
class RefoldWitnessResolver;
class RefoldWitnessTrace;

/// Ranks accepted-result and macro-selection candidates under the proof
/// lattice.  The ranker owns deterministic selection loops and tie behavior;
/// proof-summary construction, witness classification, and theorem audit are
/// delegated to their respective proof services.
class RefoldAcceptedResultRanker {
public:
  /// Borrowed inputs needed to rank and select accepted-result candidates.
  /// Every reference must outlive the ranker; the lattice owns all of them.
  struct Dependencies {
    /// Witness trace + audit-log gating.
    const RefoldWitnessTrace &witnessTrace;
    /// Central witness resolver used inside `SelectPreferred*` orchestration.
    const RefoldWitnessResolver &witnessResolver;
    /// Theorem audit ledger used to record legacy-authority admissions for
    /// the winning accepted-result candidate.
    const RefoldTheoremAudit &theoremAudit;
    /// Per-attempt selector statistics counters owned by the engine; updated
    /// by `SelectPreferredCandidateIndex` to attribute selector competitions
    /// and unresolved outcomes to the appropriate run.
    TheoremAuditStats &lastTheoremAudit;
    /// Proof-summary normalization callback.
    /// `IsSelectableAcceptedResultCandidate` admits a candidate exactly when
    /// `normalizeAcceptedProof(candidate)` returns a non-empty theorem class.
    /// Implementation lives on the lattice today
    /// (`RefoldProofLattice::NormalizeAcceptedProof`) and will move to the
    /// proof-summary builder service when that lands.
    std::function<std::optional<TheoremProofClass>(
        const AcceptedResultCandidate &)>
        normalizeAcceptedProof;
  };

  explicit RefoldAcceptedResultRanker(Dependencies deps);

  /// Return whether the normalized lattice prefers \p lhs over \p rhs.
  ///
  /// Converted selection sites use this comparator directly. The ordering is
  /// deterministic and stable-on-ties so equal summaries can preserve the
  /// existing caller-supplied precedence order.
  bool LatticePrefers(const ProofSummary &lhs, const ProofSummary &rhs) const;

  /// Return whether a normalized accepted result is admissible for
  /// lattice-based selection at the converted selector sites.
  ///
  /// Proof discharge is the participation gate for converted selector sites. A
  /// non-terminal candidate may participate only when its declared proof class
  /// discharged successfully; the explicit terminal out-of-domain result
  /// remains selectable only as the named terminal rejection class.
  bool IsSelectableAcceptedResultCandidate(
      const AcceptedResultCandidate &candidate) const;

  /// Return whether \p lhs has a strictly stronger proof than \p rhs.
  ///
  /// This names the proof-validity/preference boundary explicitly: this
  /// predicate may use only theorem-facing proof summaries.  It is not allowed
  /// to inspect artifact-local byte ranges or spelling previews.
  bool
  AcceptedResultCandidateProofPrefers(const AcceptedResultCandidate &lhs,
                                      const AcceptedResultCandidate &rhs) const;

  /// Deterministic canonical tie-breaker for already-valid candidates.
  ///
  /// This predicate is intentionally not a proof obligation.  It may order
  /// concrete emitted artifacts only after both candidates have passed the
  /// selector's proof-validity gate and neither proof summary strictly outranks
  /// the other.
  bool AcceptedResultCandidateCanonicalPrefers(
      const AcceptedResultCandidate &lhs,
      const AcceptedResultCandidate &rhs) const;

  /// Return whether \p lhs outranks \p rhs under the normalized
  /// accepted-result candidate ordering.
  ///
  /// Keep the public ordering behavior-preserving, but implement it as a
  /// proof-ordering step followed by a canonical tie-breaker step.
  bool AcceptedResultCandidatePrefers(const AcceptedResultCandidate &lhs,
                                      const AcceptedResultCandidate &rhs) const;

  /// Shared deterministic selector loop.
  ///
  /// Accepted-result selection and macro-local selection use different
  /// admissibility predicates, but they must share one stable ranking loop so
  /// selector accounting and tie behavior cannot drift apart.
  ///
  /// Returns nullopt when no candidate is selectable; updates the selector
  /// stats on the engine's per-attempt audit counters as a side effect.
  std::optional<size_t> SelectPreferredCandidateIndex(
      size_t candidateCount, llvm::function_ref<bool(size_t)> isSelectable,
      llvm::function_ref<bool(size_t, size_t)> prefers) const;

  /// Return whether a macro selector carrier may participate in ranking.
  ///
  /// A macro candidate may rank either because its selector proof discharged as
  /// a normal accepted result or because it is the narrow, caller-enabled
  /// selector-only nested macro case. The latter is still not an emitted
  /// accepted artifact.
  bool IsSelectableMacroSelectionCandidate(
      const MacroSelectionCandidate &candidate) const;

  /// Return whether one macro selector carrier outranks another.
  ///
  /// Macro-local ranking is intentionally based on selectorCandidate only. The
  /// emittedCandidate exists solely for the later emission certificate and must not
  /// change selector ordering.
  bool MacroSelectionCandidatePrefers(const MacroSelectionCandidate &lhs,
                                      const MacroSelectionCandidate &rhs) const;

  /// Return the strongest selectable macro-local candidate.
  ///
  /// This selector is the only place where selector-only macro proofs may
  /// participate. It never returns an AcceptedResultCandidate directly, which
  /// prevents non-final selector proofs from reaching the emitted artifact
  /// boundary.
  std::optional<::clang::refold::SelectedMacroSelectionCandidate>
  SelectPreferredMacroSelectionCandidate(
      llvm::ArrayRef<MacroSelectionCandidate> candidates) const;

  /// Return the strongest selectable accepted candidate.
  ///
  /// Accepted-result selection is reserved for emission-admissible artifacts.
  /// Every candidate considered here must normalize to one final theorem proof;
  /// selector-only macro staging objects must use
  /// SelectPreferredMacroSelectionCandidate().  Records the winning candidate
  /// against the no-legacy theorem-audit ledger before returning.
  std::optional<::clang::refold::SelectedAcceptedResultCandidate>
  SelectPreferredAcceptedResultCandidate(
      llvm::ArrayRef<AcceptedResultCandidate> candidates) const;

  /// Compatibility wrapper returning only the selected index.
  ///
  /// Existing callers that do not own a concrete artifact can still ask for the
  /// index, but the implementation delegates to the carrier-returning selector
  /// above so there is only one selection authority.
  std::optional<size_t> SelectPreferredAcceptedResultCandidateIndex(
      llvm::ArrayRef<AcceptedResultCandidate> candidates) const;

private:
  Dependencies deps_;
};

} // namespace refold
} // namespace clang

#endif // LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDACCEPTEDRESULTRANKER_H
