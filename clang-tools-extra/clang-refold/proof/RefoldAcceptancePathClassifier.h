//===--- RefoldAcceptancePathClassifier.h ---------------------*- C++ -*-===//
//
// Acceptance-path proof-summary classifier.
//
// Owns the classification of an `AcceptedPathKind` into a theorem proof
// class + acceptance-path inventory + baseline-discharge record, and
// the composite `Build*ProofSummary` factories that use those
// primitives.
//
// Methods:
//   * `InventoryMacroPatchProofAcceptancePath` /
//     `InventoryMacroPatchAcceptancePath` — map a macro patch's proof
//     (or its selected accepted-result carrier) into an acceptance-path
//     inventory.
//   * `BuildAcceptancePathInventory` — turn an `AcceptedPathKind` into
//     an `AcceptancePathInventory`.
//   * `BuildTheoremProofClassForAcceptedPath` — turn an
//     `AcceptedPathKind` into its authoritative `TheoremProofClass`.
//   * `RequireAcceptedPathBaseline` /
//     `BuildAcceptedPathBaselineDischarge` — accepted-path baseline
//     obligation checks used by every path-side validator.
//   * `BuildAcceptedPathProofSummary` — the shared path-side proof-
//     summary builder consumed by include/TU/terminal accepted-result
//     construction and by `RefoldOwnerRealizationProofBuilder`.
//   * `BuildIncludePatchProofSummary` — the include-specific composite
//     that pairs an accepted-path summary with the include-preserving
//     validator output.
//
// **Perf note:** the owner-realization builder, accepted-candidate
// builder, and macro-patch proof classifier all hold this classifier
// by direct reference (rather than through `std::function` callbacks),
// so per-macro-invocation classification stays a plain method call
// instead of ~4 type-erased calls per fire.
//
// The service has no back-reference to `RefoldProofLattice`.
// `BuildAcceptedPathProofSummary` still calls two lattice-side
// path-preservation validators (`ValidateIncludePreservingProof`,
// `ValidateTUAnchorProof`); those are reached through two
// `std::function` callbacks supplied at construction, and fire only
// for include/TU paths (not the macro hot path).
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDACCEPTANCEPATHCLASSIFIER_H
#define LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDACCEPTANCEPATHCLASSIFIER_H

#include "core/RefoldModel.h"
#include "edit/RefoldPatchTypes.h"
#include "proof/RefoldAcceptedResultTypes.h"
#include "proof/RefoldOwnerRealizationProofBuilder.h"
#include "proof/RefoldProofSummaryBuilder.h"
#include "proof/RefoldProofVocabulary.h"

#include <functional>

namespace clang {
namespace refold {

/// Acceptance-path proof-summary classifier.
class RefoldAcceptancePathClassifier {
public:
  /// Borrowed inputs.  All references must outlive the classifier; the
  /// lattice owns the underlying storage.
  struct Dependencies {
    const RefoldProofSummaryBuilder &proofSummaryBuilder;
    const RefoldOwnerRealizationProofBuilder &ownerRealizationProofBuilder;

    /// Delegates to `RefoldProofLattice::ValidateIncludePreservingProof`.
    /// Not on the macro hot path — fires only for include/TU paths.
    std::function<ProofDischargeRecord(AcceptedPathKind currentPath,
                                       const IncludePatch *patch,
                                       const IncludeAnchorWitness *witness)>
        validateIncludePreservingProof;

    /// Delegates to `RefoldProofLattice::ValidateTUAnchorProof`.
    /// Not on the macro hot path.
    std::function<ProofDischargeRecord(AcceptedPathKind currentPath,
                                       const TUAnchorWitness *witness)>
        validateTUAnchorProof;
  };

  explicit RefoldAcceptancePathClassifier(Dependencies deps);

  /// Map a macro patch's proof carrier into its acceptance-path
  /// inventory.
  AcceptancePathInventory
  InventoryMacroPatchProofAcceptancePath(const MacroPatchProof &proof) const;

  /// Map a macro patch's selected accepted-result carrier into an
  /// acceptance-path inventory.
  AcceptancePathInventory
  InventoryMacroPatchAcceptancePath(const MacroPatch &patch) const;

  /// Map an accepted path to its proof-discharge inventory.
  ///
  /// Centralizes the mapping from the current accepted-result path to the
  /// future theorem-facing proof target that is expected to discharge it.
  /// Validators use this inventory to check that each accepted result is both
  /// classified and assigned to a declared proof class.
  AcceptancePathInventory
  BuildAcceptancePathInventory(AcceptedPathKind currentPath) const;

  /// Map construction provenance onto the final theorem proof class.
  ///
  /// AcceptedPathKind / AcceptedProofClass remain construction provenance only.
  /// This helper is the single path-to-theorem bridge used by builders before
  /// the summary is finalized; selectors and emitters then read
  /// ProofSummary::theoremClass / ProofSummary::emittedProof instead of
  /// re-deriving validity from AcceptedProofClass.
  TheoremProofClass
  BuildTheoremProofClassForAcceptedPath(AcceptedPathKind currentPath) const;

  /// Accepted-path baseline obligation: every accepted-path summary must record
  /// its acceptance-path inventory before further gating.
  void
  RequireAcceptedPathBaseline(ProofDischargeAccumulator &discharge,
                              const AcceptancePathInventory &inventory) const;

  /// Build the accepted-path baseline discharge record used by
  /// `BuildAcceptedPathProofSummary`.
  ProofDischargeRecord
  BuildAcceptedPathBaselineDischarge(const AcceptancePathInventory &inventory,
                                     bool explicitOutOfDomain = false) const;

  /// Build a normalized proof summary for a concrete accepted path.
  ///
  /// This helper attaches class-local obligation/discharge metadata to
  /// non-macro accepted paths such as include anchors, TU anchors, and terminal
  /// fallback. Callers may supply an explicit TU/include/terminal witness so
  /// the accepted-path audit can report the exact deterministic anchor, mapped
  /// byte range, or failed terminal obligation that was used.
  ProofSummary BuildAcceptedPathProofSummary(
      AcceptedPathKind currentPath, const IncludePatch *patch = nullptr,
      const TUAnchorWitness *tuAnchorWitness = nullptr,
      const IncludeAnchorWitness *includeAnchorWitness = nullptr,
      const TerminalFallbackWitness *terminalFallbackWitness = nullptr) const;

  /// Build the internal working proof summary for an include patch.
  ///
  /// Include patches are created before materialization chooses a concrete
  /// preserving anchor or realization envelope. The resulting summary therefore
  /// is only a staging object, so the default summary intentionally avoids
  /// claiming any normalized accepted path. Callers must restamp the emitted
  /// accepted result onto a concrete witness-backed include class once
  /// materialization chooses the final path.
  ProofSummary BuildIncludePatchProofSummary(bool realizedSurface,
                                             AcceptedPathKind currentPath,
                                             const IncludePatch *patch) const;

  /// Include zero-width anchor obligation helper used by
  /// `BuildAcceptedPathProofSummary` internally and by the lattice's
  /// `ValidateIncludePreservingProof`, which remains the include-specific
  /// path-preservation validator.
  void RequireIncludeZeroWidthAnchor(ProofDischargeAccumulator &discharge,
                                     const IncludePatch &patch,
                                     const IncludeAnchorWitness *witness,
                                     IncludeAnchorEvidenceKind evidence,
                                     ProofObligationKind witnessObligation,
                                     ProofFailureReason witnessFailure) const;

private:
  Dependencies deps_;
};

} // namespace refold
} // namespace clang

#endif // LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDACCEPTANCEPATHCLASSIFIER_H
