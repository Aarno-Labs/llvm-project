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
  /// Composition is independent of source-spelling: it partitions tuples by
  /// the (target, suffix, observer, counter, boundary, diagnostic,
  /// composition, producer) signature and rejects multi-class compositions
  /// unless the same source repair is the underlying identity.
  ::clang::refold::WitnessCompositionDecision ResolveWitnessComposition(
      llvm::StringRef role,
      llvm::ArrayRef<std::pair<size_t, RefoldWitness>> selectableWitnesses,
      bool hasSingleConcreteRepairIdentity) const;

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
