//===--- RefoldProofSummaryBuilder.h ----------------------------*- C++ -*-===//
//
// Proof-summary construction service for clang-refold.
//
// Owns the path-agnostic primitives that configure, finalize, and normalize a
// `ProofSummary` / `EmittedProof` carrier:
//
//   * `ConfigureProofSummary` / `FinalizeProofSummary` — set the theorem class,
//     realization mode, completeness / theorem-domain contracts, and the
//     canonical emitted-proof carrier on a summary.
//   * `BuildEmittedProof{,FromSummary}` and
//   `BuildCanonicalEmittedProofFromSummary`
//     — build the theorem-facing `EmittedProof` from a summary or candidate,
//     including the per-class admission gates (mixed-owner tiling shape,
//     owner-realization witness presence, terminal-fallback domain, etc.).
//   * `NormalizeAcceptedProof` — convenience accessor returning just the
//     theorem class for an accepted-result candidate.
//   * `BuildCompletenessContract` / `BuildTheoremDomainContract` — derive the
//     secondary contract carriers a finalized summary needs.
//   * `ProofSummaryRequiresOwnerRealizationWitness` — the per-path predicate
//     that gates the owner-realization witness requirement.
//
// The path-specific summary builders (`BuildAcceptedPathProofSummary`,
// `BuildIncludePatchProofSummary`, `BuildOwnerRealizationProofSummary`) stay
// on `RefoldProofLattice` because their bodies are thin switch-arms over
// path-specific validators and inventory helpers that also live on the
// lattice; lifting them out would force the builder to back-reference the
// lattice for those helpers.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDPROOFSUMMARYBUILDER_H
#define LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDPROOFSUMMARYBUILDER_H

#include "proof/RefoldAcceptedResultTypes.h"
#include "proof/RefoldProofVocabulary.h"
#include "source/RefoldToken.h"

#include "llvm/ADT/ArrayRef.h"

#include <optional>

namespace clang {
namespace refold {

/// Build, finalize, and normalize proof summaries / emitted-proof carriers.
class RefoldProofSummaryBuilder {
public:
  /// Borrowed inputs needed during proof-summary construction.  The only
  /// hard dependency is the B-token stream; it is consumed by the mixed-
  /// owner tiling shape check inside
  /// `BuildCanonicalEmittedProofFromSummary` to verify segment B ranges
  /// remain inside the recorded B-token bound.
  struct Dependencies {
    llvm::ArrayRef<PPTok> bToks;
  };

  explicit RefoldProofSummaryBuilder(Dependencies deps);

  /// Certify a summary with the theorem class, realization mode, preference,
  /// surface disposition, and structure-preserving flag.  Marks
  /// `primaryProofClassExplicit` when the declared theorem class is
  /// non-Unknown.
  void ConfigureProofSummary(ProofSummary &summary,
                             TheoremProofClass theoremClass,
                             AcceptedProofClass acceptedClass,
                             RealizationMode realizationMode,
                             SelectionPreference preference,
                             SurfaceDisposition surfaceDisposition,
                             bool structurePreserving) const;

  /// True iff the accepted path requires an owner-realization witness as
  /// part of its theorem-facing proof.  Path-specific; selector-only or
  /// counter-literal paths return false.
  bool ProofSummaryRequiresOwnerRealizationWitness(
      const ProofSummary &summary) const;

  /// Run the theorem-class declaration gate, then derive the global
  /// selection lattice, completeness contract, theorem-domain contract, and
  /// canonical emitted-proof carrier onto `summary`.  Strict-mode rejection
  /// is recorded against the summary's discharge accumulator only — no
  /// candidate is chosen here.
  void FinalizeProofSummary(ProofSummary &summary) const;

  /// Build an emitted-proof carrier from `summary` against the explicit
  /// theorem class `theoremClass`.  Does not gate; the caller must already
  /// have admitted the summary.  Pure function of the inputs.
  static ::clang::refold::EmittedProof
  BuildEmittedProofFromSummary(TheoremProofClass theoremClass,
                               const ProofSummary &summary);

  /// Build the canonical emitted-proof carrier for `summary`, applying all
  /// per-class admission gates.  Returns nullopt when the summary is not yet
  /// a theorem-facing carrier (transitional, undischarged, terminal-domain
  /// mismatched, missing witness).
  std::optional<::clang::refold::EmittedProof>
  BuildCanonicalEmittedProofFromSummary(const ProofSummary &summary) const;

  /// Build the emitted-proof carrier for an accepted-result candidate.
  /// Reuses the summary's cached emitted-proof when fresh; otherwise rebuilds
  /// it via `BuildCanonicalEmittedProofFromSummary`.  Returns nullopt when
  /// the candidate's kind and proof family disagree on the terminal-domain
  /// boundary, or when the summary is not yet a theorem-facing carrier.
  std::optional<::clang::refold::EmittedProof>
  BuildEmittedProof(const AcceptedResultCandidate &candidate) const;

  /// Return the normalized final theorem class for `candidate`, or nullopt
  /// when the candidate is not yet a theorem-facing carrier.  Thin accessor
  /// over `BuildEmittedProof`.
  std::optional<::clang::refold::TheoremProofClass>
  NormalizeAcceptedProof(const AcceptedResultCandidate &candidate) const;

  /// Derive the completeness contract for `summary`: coverage class,
  /// declared target, and explicit-out-of-domain exclusion record when
  /// applicable.
  ::clang::refold::CompletenessContract
  BuildCompletenessContract(const ProofSummary &summary) const;

  /// Derive the theorem-domain contract for `summary` from its completeness
  /// contract.  Each completeness coverage class maps deterministically onto
  /// a theorem-domain kind.
  ::clang::refold::TheoremDomainContract
  BuildTheoremDomainContract(const ProofSummary &summary) const;

private:
  Dependencies deps_;
};

} // namespace refold
} // namespace clang

#endif // LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDPROOFSUMMARYBUILDER_H
