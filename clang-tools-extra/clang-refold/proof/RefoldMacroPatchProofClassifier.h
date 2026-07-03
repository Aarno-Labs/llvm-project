//===--- RefoldMacroPatchProofClassifier.h --------------------*- C++ -*-===//
//
// Macro-patch proof classification and validation.
//
// Classifies a `MacroPatch`'s proof carrier into a `ProofSummary` and
// runs the fail-closed validation checks used by the emission-side
// gate.
//
// Methods:
//   * `ClassifyMacroPatchProof` — the top-level entry that builds a
//     starter `ProofSummary` for a proof-carrying macro patch and
//     dispatches to the invocation-preserving or realization
//     validator.
//   * `RefreshMacroPatchDerivedProofWitnesses` /
//     `SyncMacroPatchProofSummary` — refresh derived witnesses and
//     re-run classification on an in-place patch.
//   * `MacroInvocationHasWellFormedPasteWitnesses` — precondition
//     check for paste-touching proof carriers.
//   * `ValidateInvocationPreservingProof` /
//     `ValidateEmittedInvocationPreservingProof` — the two public
//     invocation-preserving validators (top-level required vs. emitted
//     variant).  Share the private impl helper.
//   * `ValidateInvocationRealizationProof` — the realization validator.
//
// The service has no back-reference to `RefoldProofLattice`.  The
// three acceptance-path primitives it needs
// (`BuildTheoremProofClassForAcceptedPath`,
// `InventoryMacroPatchProofAcceptancePath`,
// `RequireAcceptedPathBaseline`) live on
// `RefoldAcceptancePathClassifier`, held as a direct reference in
// `Dependencies` so the per-macro-invocation classify path stays free
// of type-erased calls.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDMACROPATCHPROOFCLASSIFIER_H
#define LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDMACROPATCHPROOFCLASSIFIER_H

#include "core/RefoldModel.h"
#include "edit/RefoldPatchTypes.h"
#include "proof/RefoldAcceptancePathClassifier.h"
#include "proof/RefoldAcceptedResultTypes.h"
#include "proof/RefoldOwnerRealizationProofBuilder.h"
#include "proof/RefoldProofSummaryBuilder.h"
#include "proof/RefoldProofVocabulary.h"

namespace clang {
namespace refold {

class RefoldMacroTopology;
class RefoldOwnerStateProof;

/// Macro-patch proof classifier and validator.  Owns classification,
/// validation, and derived-witness refresh for macro-patch proof
/// carriers.
class RefoldMacroPatchProofClassifier {
public:
  /// Borrowed inputs.  Every reference must outlive the classifier;
  /// the lattice owns the underlying storage.
  struct Dependencies {
    const RefoldMacroTopology &macroTopology;
    const RefoldOwnerStateProof &ownerStateProof;
    const RefoldProofSummaryBuilder &proofSummaryBuilder;
    const RefoldOwnerRealizationProofBuilder &ownerRealizationProofBuilder;

    /// Direct reference to the acceptance-path classifier.  Held by
    /// reference (rather than through `std::function` callbacks) because
    /// `ClassifyMacroPatchProof` fires once per macro invocation and
    /// calls three primitives on the classifier
    /// (`BuildTheoremProofClassForAcceptedPath`,
    /// `InventoryMacroPatchProofAcceptancePath`,
    /// `RequireAcceptedPathBaseline`); the direct reference removes ~4
    /// type-erased calls per invocation on the macro-heavy hot path.
    const RefoldAcceptancePathClassifier &acceptancePathClassifier;
  };

  explicit RefoldMacroPatchProofClassifier(Dependencies deps);

  /// Classify a proof-carrying macro patch's proof carrier into a
  /// starter `ProofSummary` (theorem class + discharge record).
  ProofSummary ClassifyMacroPatchProof(const MacroPatch &patch) const;

  /// Refresh the derived paste/subtree/call-chain witnesses on the
  /// patch's proof carrier.
  void RefreshMacroPatchDerivedProofWitnesses(MacroPatch &patch) const;

  /// Refresh derived witnesses and re-run classification into
  /// `patch.selectedProofSummary`.
  void SyncMacroPatchProofSummary(MacroPatch &patch) const;

  /// Return whether \p m carries usable producer-side paste witnesses.
  ///
  /// The producer's exact `##` decomposition is serialized into the model.  A
  /// paste-preserving args-only class may be discharged from either of two
  /// deterministic proof sources: a usable producer-side root witness stream,
  /// checked here, or an explicit direct replay check recorded on the accepted
  /// patch. This helper validates only the producer-side witness source.
  ///
  /// The producer records only the argument-derived fragments of each pasted
  /// token; literal glue bytes from the macro body (for example the `_` in
  /// `X##_##Y`) may appear as gaps between recorded parts. The helper is
  /// therefore strict about ordered, non-overlapping half-open ranges that stay
  /// within the final spelling, but it does not require the recorded parts to
  /// form a contiguous partition of the pasted token. It also accepts the
  /// parent-level case where `pasteSpans` are only propagated child-paste
  /// contributors inside a standard occurrence of the same formal, because
  /// those spans are validated by the args-only replay proof rather than by a
  /// direct parent-level `paste_tokens` decomposition.
  bool MacroInvocationHasWellFormedPasteWitnesses(
      const RefoldModel::MacroInvocation &m) const;

  /// Validate the current local proof contract for an accepted class.
  ///
  /// These validators expose the deterministic checks as named local
  /// obligations. Selector and emission sites consume their discharge result
  /// directly, while compatibility paths mirror the same facts into the
  /// normalized proof contract.
  ProofDischargeRecord
  ValidateInvocationPreservingProof(const MacroPatch &patch) const;

  /// Validate the emitted semantic proof contract for a preserving macro patch.
  ///
  /// Remove the byte-edit boundary's selector-only exception by rebuilding
  /// emitted nested preserving macro carriers under this validator. It
  /// discharges the same semantic obligations as
  /// ValidateInvocationPreservingProof(), but it does not re-impose the
  /// top-level proof-root selector rule that is only relevant while competing
  /// for final selection.
  ProofDischargeRecord
  ValidateEmittedInvocationPreservingProof(const MacroPatch &patch) const;

  /// Discharge the proof obligations for a macro invocation-realization patch.
  ///
  /// Validates that the macro patch is classified as a realization path rather
  /// than an invocation-preserving path, carries a tracked proof root, and has
  /// the proof metadata required by its realization class. Whole-cover
  /// realization patches must additionally record their A/B envelopes,
  /// containment status, and boundary-accounting metadata.
  ProofDischargeRecord
  ValidateInvocationRealizationProof(const MacroPatch &patch) const;

private:
  /// Shared invocation-preserving validator body used by the two
  /// public variants above.
  ProofDischargeRecord
  ValidateInvocationPreservingProofImpl(const MacroPatch &patch,
                                        bool requireTopLevelRoot) const;

  Dependencies deps_;
};

} // namespace refold
} // namespace clang

#endif // LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDMACROPATCHPROOFCLASSIFIER_H
