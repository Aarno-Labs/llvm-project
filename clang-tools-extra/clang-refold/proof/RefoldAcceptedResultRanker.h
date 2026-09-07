//===--- RefoldAcceptedResultRanker.h ---------------------------*- C++ -*-===//
//
// Accepted-result ranking and selection service for clang-refold.
//
// Owns the per-attempt comparators and selection orchestration used to pick
// a single representative accepted-result candidate (or macro-selection
// candidate) from a set of selectable proofs:
//
//   * `ProofDominates` — the strict order over proof summaries:
//     selection-preference rank, surface-disposition rank, mixed-owner cover
//     comparison, then deterministic enum fallbacks.  It is irreflexive,
//     asymmetric and transitive, which is what makes the selector's max scan
//     independent of candidate push order.
//   * `NamedTheoremTieBreakerPrefers` — a named theorem preference between two
//     summaries the *caller* has already proved to realize the same edit.  Its
//     precondition is pairwise and caller-established, so it is deliberately
//     not part of `ProofDominates`; only `ProvenEquivalentArtifactPrefers`
//     composes the two.
//   * `AcceptedResultCandidate*Prefers` and `MacroSelectionCandidatePrefers`
//     — candidate-level comparators that compose `ProofDominates` with a
//     canonical artifact-shape tie-breaker reached only on incomparability.
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
#include "llvm/ADT/StringRef.h"

#include <cstddef>
#include <functional>
#include <optional>

namespace clang {
namespace refold {

class RefoldTheoremAudit;
class RefoldWitnessResolver;
class RefoldWitnessTrace;

/// Outcome of comparing two candidates under the proof order.
///
/// `Incomparable` is a distinct answer rather than a synonym for "the right
/// side wins": it is the only outcome that licenses the canonical tie-break,
/// and collapsing it into a bool is what previously made an order defect
/// indistinguishable from a genuine preference.
enum class ProofDominanceOrder : uint8_t {
  LeftDominates,
  RightDominates,
  Incomparable,
};

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

  /// Return whether \p lhs strictly dominates \p rhs under the proof order.
  ///
  /// This is a strict weak ordering: it compares a lexicographic key derived
  /// from the two summaries alone, so it is irreflexive, asymmetric and
  /// transitive, and incomparability (false in both directions) is exactly
  /// key equality.  Those three laws are what make a left-to-right max scan
  /// over selectable candidates independent of candidate push order; the
  /// selector audits them on every competition it resolves.
  ///
  /// Nothing pairwise or caller-conditioned may enter this relation.  A
  /// preference that depends on a fact the two summaries do not carry belongs
  /// in `NamedTheoremTieBreakerPrefers` instead.
  static bool ProofDominates(const ProofSummary &lhs, const ProofSummary &rhs);

  /// Return the named theorem preference between two summaries whose
  /// equivalence the caller has *already proved*, or nullopt when no named
  /// tie-breaker applies.
  ///
  /// A named tie-breaker orders two representations of the same proven edit;
  /// it is meaningless between summaries that realize different edits, and the
  /// summaries themselves do not record whether they realize the same one.
  /// The relation is therefore not a property of the pair and cannot be part
  /// of `ProofDominates` without making that order depend on a fact it cannot
  /// see.  Only a caller that has discharged the equivalence obligation may
  /// consult it, through `ProvenEquivalentArtifactPrefers`.
  ///
  /// Returns nullopt when neither side names a tie-breaker against the other
  /// and also when both do, because a mutual named preference orders nothing.
  static std::optional<bool>
  NamedTheoremTieBreakerPrefers(const ProofSummary &lhs,
                                const ProofSummary &rhs);

  /// Return whether \p lhs wins a cross-domain competition against \p rhs,
  /// given that the caller has proved both realize the same edit.
  ///
  /// Named tie-breakers decide when one applies; otherwise the proof order
  /// decides, and an incomparable pair yields false in both directions.  This
  /// composite is not an order and must only be consumed pairwise.
  static bool ProvenEquivalentArtifactPrefers(const ProofSummary &lhs,
                                              const ProofSummary &rhs);

  /// Return the first counterexample to the strict-order laws of \p prefers
  /// over \p indices, or nullopt when the relation is an order on that set.
  ///
  /// Irreflexivity and asymmetry are checked before transitivity, and each
  /// scan runs in index order, so the reported violation is deterministic.
  /// The relation is supplied by the caller rather than assumed to be
  /// `ProofDominates`: what a selection actually depends on is the comparator
  /// the selector was handed, including its canonical tie-break.
  static std::optional<SelectionOrderViolation>
  FindSelectionOrderViolation(llvm::ArrayRef<size_t> indices,
                              llvm::function_ref<bool(size_t, size_t)> prefers);

  /// Return whether a normalized accepted result is admissible for
  /// lattice-based selection at the converted selector sites.
  ///
  /// Proof discharge is the participation gate for converted selector sites. A
  /// non-terminal candidate may participate only when its declared proof class
  /// discharged successfully; the explicit terminal out-of-domain result
  /// remains selectable only as the named terminal rejection class.
  bool IsSelectableAcceptedResultCandidate(
      const AcceptedResultCandidate &candidate) const;

  /// Compare two candidates under the proof order, distinguishing "the right
  /// side dominates" from "neither dominates".
  ///
  /// This names the proof-validity/preference boundary explicitly: the
  /// comparison may use only theorem-facing proof summaries.  It is not
  /// allowed to inspect artifact-local byte ranges or spelling previews; those
  /// belong to the canonical tie-break that `Incomparable` licenses.
  static ProofDominanceOrder
  CompareAcceptedResultCandidateProofs(const AcceptedResultCandidate &lhs,
                                       const AcceptedResultCandidate &rhs);

  /// Return whether \p lhs has a strictly stronger proof than \p rhs.
  ///
  /// Thin spelling of `CompareAcceptedResultCandidateProofs(...) ==
  /// LeftDominates`, kept for call sites that only need the strict answer.  A
  /// caller that must also act on incomparability should ask for the order
  /// directly rather than probing this predicate in both directions.
  static bool
  AcceptedResultCandidateProofPrefers(const AcceptedResultCandidate &lhs,
                                      const AcceptedResultCandidate &rhs);

  /// Deterministic canonical tie-breaker for already-valid candidates.
  ///
  /// This predicate is intentionally not a proof obligation.  It may order
  /// concrete emitted artifacts only after both candidates have passed the
  /// selector's proof-validity gate and neither proof summary strictly outranks
  /// the other.
  static bool
  AcceptedResultCandidateCanonicalPrefers(const AcceptedResultCandidate &lhs,
                                          const AcceptedResultCandidate &rhs);

  /// Return whether \p lhs outranks \p rhs under the normalized
  /// accepted-result candidate ordering.
  ///
  /// Keep the public ordering behavior-preserving, but implement it as a
  /// proof-ordering step followed by a canonical tie-breaker step.
  static bool
  AcceptedResultCandidatePrefers(const AcceptedResultCandidate &lhs,
                                 const AcceptedResultCandidate &rhs);

  /// Shared deterministic selector loop.
  ///
  /// Accepted-result selection and macro-local selection use different
  /// admissibility predicates, but they must share one stable ranking loop so
  /// selector accounting and tie behavior cannot drift apart.
  ///
  /// \p role names the competition in the proof trace and in any order-audit
  /// finding it produces.
  ///
  /// Returns nullopt when no candidate is selectable, and in strict mode also
  /// when \p prefers is not an order over the selectable set: a winner picked
  /// by a non-order relation is a winner picked by candidate push order.
  /// Updates the selector stats on the engine's per-attempt audit counters as
  /// a side effect.
  std::optional<size_t> SelectPreferredCandidateIndex(
      llvm::StringRef role, size_t candidateCount,
      llvm::function_ref<bool(size_t)> isSelectable,
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
  static bool
  MacroSelectionCandidatePrefers(const MacroSelectionCandidate &lhs,
                                 const MacroSelectionCandidate &rhs);

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
  /// Run the strict-order audit over \p selectableIndices and report whether
  /// the caller must refuse to select.
  ///
  /// The audit runs only when proof tracing is on (which strict mode implies)
  /// and only for a real competition: with fewer than two selectable
  /// candidates the max scan never invokes the relation, so there is nothing
  /// an order defect could change.  A violation is always traced and counted;
  /// it fails closed only in strict mode.
  bool
  AuditSelectionOrder(llvm::StringRef role,
                      llvm::ArrayRef<size_t> selectableIndices,
                      llvm::function_ref<bool(size_t, size_t)> prefers) const;

  Dependencies deps_;
};

} // namespace refold
} // namespace clang

#endif // LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDACCEPTEDRESULTRANKER_H
