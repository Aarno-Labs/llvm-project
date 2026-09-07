//===--- RefoldWitnessResolver.h --------------------------------*- C++ -*-===//
//
// Witness resolution service for clang-refold.
//
// Owns the central witness-resolver logic: given a selector role and a set of
// accepted-result candidates, build a `WitnessResolverDecision` that records
// which candidates were selectable, how their equivalence keys partition into
// classes, which composition tuple they form, and whether strict mode can
// authoritatively pick a single candidate, reuse the caller-supplied
// compatibility index, or fail closed.
//
// The resolver coordinates four collaborators:
//
//   * `RefoldWitnessTrace` for emission gating + closure-ledger / resolver
//     decision tracing,
//   * `RefoldWitnessEquivalenceKeyBuilder` for per-candidate key construction
//     inside `BuildRefoldWitness`,
//   * `RefoldTheoremAudit` for recording the resolver decision against the
//     theorem-audit ledger,
//   * the witness-classifier free functions for fallback-class classification
//     (consumed via `ClassifyIncompleteWitnessKeys` and
//     `classifyResolverFallbackReason`).
//
// The resolver intentionally does NOT own candidate-ranking preferences;
// those remain on `RefoldProofLattice`.  This service is the resolver
// authority, not the canonical-preference authority.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDWITNESSRESOLVER_H
#define LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDWITNESSRESOLVER_H

#include "proof/RefoldAcceptedResultTypes.h"
#include "proof/RefoldProofVocabulary.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/STLFunctionalExtras.h"
#include "llvm/ADT/StringRef.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>

namespace clang {
namespace refold {

class RefoldTheoremAudit;
class RefoldWitnessEquivalenceKeyBuilder;
class RefoldWitnessTrace;

/// Selection-role label for the macro-selection stage.  It is both the trace
/// label the ranker emits and the key a resolver-authority gate keys on, so it
/// is defined once here: the producer (RefoldAcceptedResultRanker) and the
/// consumer (the dominance gate in RefoldWitnessResolver) reference the same
/// constant, which makes a rename a single-point change instead of a silent
/// drift between two hand-written string literals.
inline constexpr llvm::StringLiteral kSelectPreferredMacroSelectionRole =
    "SelectPreferredMacroSelectionCandidate";

/// Resolves selectable accepted-result witnesses into one strict-domain
/// decision.  The resolver partitions candidates by equivalence key, checks
/// composition compatibility, records theorem-audit state, and traces the
/// resulting closure ledger.
class RefoldWitnessResolver {
public:
  /// Borrowed inputs needed to resolve witnesses across a candidate set.
  /// Every reference must outlive the resolver; the lattice owns all three.
  struct Dependencies {
    /// Witness trace + audit-log gating.
    const RefoldWitnessTrace &witnessTrace;
    /// Equivalence-key construction used by `BuildRefoldWitness` to derive
    /// the resolver-comparable key for each accepted-result candidate.
    const RefoldWitnessEquivalenceKeyBuilder &equivalenceKeyBuilder;
    /// Theorem audit ledger; resolver decisions are recorded against it.
    const RefoldTheoremAudit &theoremAudit;
  };

  explicit RefoldWitnessResolver(Dependencies deps);

  /// Return whether \p family is a witness proof family whose proofs the
  /// resolver may treat as authoritative in strict mode.  Selector-only and
  /// owner-realization-only families return false until their proofs are
  /// explicitly converted.
  static bool IsResolverAuthoritativeWitnessFamily(WitnessProofFamily family);

  /// Return whether \p witness comes from a resolver-authoritative source.
  /// Macro whole-cover realization is admitted only when its proof key
  /// matches the converted invocation-preserving shape; every other family
  /// uses the broad family-level decision.
  static bool IsResolverAuthoritativeWitness(const RefoldWitness &witness);

  /// Classify a set of incomplete witness keys into the strongest single
  /// fallback class.  Used by strict mode when no witness can be ranked.
  static ::clang::refold::WitnessFallbackClass ClassifyIncompleteWitnessKeys(
      llvm::ArrayRef<std::pair<size_t, RefoldWitness>> witnesses);

  /// Build the deterministic canonical cost for an accepted-result candidate.
  /// Costs are total-ordered for canonical ranking; no instance state needed.
  static ::clang::refold::WitnessCanonicalCost
  BuildWitnessCanonicalCost(const AcceptedResultCandidate &candidate);

  /// Populate `decision.closureLedger` with one entry per missing-proof
  /// dimension found across `selectableWitnesses`.  Only used when the
  /// strict-domain class is `PotentiallyInDomainMissingProof`.
  static void PopulateWitnessClosureLedger(
      WitnessResolverDecision &decision,
      llvm::ArrayRef<std::pair<size_t, RefoldWitness>> selectableWitnesses);

  /// Build the full `RefoldWitness` carrier for an accepted-result candidate.
  /// Combines proof-family classification, owner identity, equivalence-key
  /// computation, and canonical cost into the trace-stable record consumed by
  /// the resolver and the trace subsystem.
  ::clang::refold::RefoldWitness
  BuildRefoldWitness(const AcceptedResultCandidate &candidate,
                     llvm::StringRef role, uint64_t witnessId = 0) const;

  /// Decide tuple-level composition compatibility for `selectableWitnesses`.
  ///
  /// Tuples are partitioned by the (target, suffix, observer, counter,
  /// boundary, diagnostic, composition, producer) signature, and a multi-class
  /// composition is rejected unless every tuple emits one identical repair.
  ///
  /// That exception is a theorem rather than a tolerance: the state a tuple
  /// leaves for its neighbors follows from the bytes it emits, so tuples
  /// sharing an `EmittedRepairIdentity` compose identically whichever is
  /// selected.  It is needed because the signature also carries provenance --
  /// each proof family authors its own spellings for the same facts, and
  /// `boundaryClass` / `producerKinds` describe the certificate rather than
  /// the resulting state -- so a class count above one means the certificates
  /// are not textually identical, which is weaker than a disagreement.
  ///
  /// \param hasSingleConcreteRepairIdentity whether every selectable tuple
  ///        carries the same known `EmittedRepairIdentity`.  False when any
  ///        tuple's emitted bytes are unknown, which fails closed.
  ///
  /// Thin wrapper: `ClassifyWitnessComposition` decides, this traces.
  ::clang::refold::WitnessCompositionDecision ResolveWitnessComposition(
      llvm::StringRef role,
      llvm::ArrayRef<std::pair<size_t, RefoldWitness>> selectableWitnesses,
      bool hasSingleConcreteRepairIdentity) const;

  /// Decide tuple-level composition compatibility, without tracing it.
  ///
  /// This is the whole decision, and it is deliberately a pure function of
  /// the tuples and the repair-identity flag: no instance state, no trace
  /// handle, no role.  A classifier that cannot reach the tracer cannot be
  /// influenced by whether tracing is on, which is the property the refolder
  /// requires of every diagnostic and which is otherwise only reviewable by
  /// reading.  `ResolveWitnessComposition` adds the trace record.
  ///
  /// The refusals are not interchangeable, and their order is part of the
  /// contract rather than an artifact:
  ///
  ///   * an incomplete key withholds compatibility without being fatal --
  ///     the composition is unproven, not disproven;
  ///   * a terminal tuple is fatal, and outranks the repair-identity rule
  ///     below: that rule answers "which certificate describes this
  ///     repair?", never "may this compose at all?";
  ///   * one class composes on the key alone, needing no repair identity;
  ///   * several classes compose only under the identical-repair theorem;
  ///   * anything else fails closed.
  static ::clang::refold::WitnessCompositionDecision
  ClassifyWitnessComposition(
      llvm::ArrayRef<std::pair<size_t, RefoldWitness>> selectableWitnesses,
      bool hasSingleConcreteRepairIdentity);

  /// Resolve witnesses for one selector role.  Builds equivalence-key
  /// partitions, runs composition resolution, records the strict-domain
  /// decision, populates the closure ledger when the result is potentially in
  /// domain, and traces the final resolver decision.
  ::clang::refold::WitnessResolverDecision ResolveWitnessesForSelection(
      llvm::StringRef role, size_t candidateCount,
      llvm::function_ref<bool(size_t)> isSelectable,
      llvm::function_ref<RefoldWitness(size_t)> buildWitness,
      llvm::function_ref<bool(size_t, size_t)> canonicalPrefers,
      std::optional<size_t> legacyIndex) const;

private:
  Dependencies deps_;
};

} // namespace refold
} // namespace clang

#endif // LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDWITNESSRESOLVER_H
