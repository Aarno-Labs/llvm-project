//===--- RefoldOwnerRealizationProofBuilder.h -----------------*- C++ -*-===//
//
// Owner-realization proof construction + witness attachment.
//
// Turns caller-supplied owner-closure evidence (macro whole-cover /
// include realization / TU replacement) into an authoritative
// `OwnerRealizationResult`, and attaches the resulting witnesses onto
// an `AcceptedResultCandidate` or `ProofSummary`.
//
// Methods:
//   * `TryBuildOwnerRealization` — the shared proof gate that every
//     caller-specific `Build*OwnerRealization` funnels into.
//   * `BuildMacroWholeCoverOwnerRealization` /
//     `BuildIncludeOwnerRealization` /
//     `BuildTUOwnerRealization` — caller-specific closures onto the
//     shared gate.
//   * `ApplyOwnerRealizationResultToProofSummary` — apply the gate's
//     verdict to a `ProofSummary` (discharge status, witness, finalize).
//   * `BuildOwnerRealizationProofSummary` — one-call convenience that
//     initializes a summary and applies the realization result.
//   * `AttachLineControlObserverWitness` — recover the line-control
//     observer/state witness on an accepted candidate from the
//     `ProofSummary` and owner-state facts.
//   * `AttachCounterStateWitness` — same, for counter-state facts.
//   * `AttachMixedOwnerTilingWitnessForTokenEnvelope` — attach the
//     strongest matching mixed-owner tiling witness for a token
//     envelope, folding through the lattice-level preference gate.
//
// The service has no back-reference to `RefoldProofLattice`.  Every
// primitive it needs is reached through the explicit `Dependencies`
// bundle: owner-state proof, model, and the summary-builder / ranker
// pair that arbitrate mixed-owner tiling candidates.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDOWNERREALIZATIONPROOFBUILDER_H
#define LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDOWNERREALIZATIONPROOFBUILDER_H

#include "core/RefoldModel.h"
#include "edit/RefoldPatchTypes.h"
#include "proof/RefoldAcceptedResultRanker.h"
#include "proof/RefoldAcceptedResultTypes.h"
#include "proof/RefoldOwnerStateTypes.h"
#include "proof/RefoldProofSummaryBuilder.h"
#include "proof/RefoldProofVocabulary.h"

#include "llvm/ADT/StringRef.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <vector>

namespace clang {
namespace refold {

class RefoldOwnerStateProof;

/// Owner-realization proof builder.
///
/// Owns caller-specific realization construction and witness attachment for
/// proof summaries that represent macro, include, or TU realized output.
class RefoldOwnerRealizationProofBuilder {
public:
  /// Borrowed inputs.  Every reference must outlive the builder; the
  /// lattice owns the underlying storage.
  struct Dependencies {
    const RefoldModel &model;
    const RefoldOwnerStateProof &ownerStateProof;
    const RefoldProofSummaryBuilder &proofSummaryBuilder;
    const RefoldAcceptedResultRanker &acceptedResultRanker;
    const std::vector<MixedOwnerTilingSegmentBinding>
        &mixedOwnerTilingSegmentBindings;
    const std::vector<MixedOwnerTilingWitness> &mixedOwnerTilingWitnesses;

    /// Delegates to `RefoldProofLattice::BuildAcceptedPathProofSummary`.
    /// `BuildOwnerRealizationProofSummary` uses this to initialize its starter
    /// summary; full acceptance-path proof-summary construction stays on the
    /// lattice.
    std::function<ProofSummary(
        AcceptedPathKind currentPath, const IncludePatch *patch,
        const TUAnchorWitness *tuAnchorWitness,
        const IncludeAnchorWitness *includeAnchorWitness,
        const TerminalFallbackWitness *terminalFallbackWitness)>
        buildAcceptedPathProofSummary;
  };

  explicit RefoldOwnerRealizationProofBuilder(Dependencies deps);

  /// Attach line-control / builtin-location observer witness facts to an
  /// accepted candidate using only existing proof-summary state.
  void
  AttachLineControlObserverWitness(AcceptedResultCandidate &candidate) const;

  /// Attach `__COUNTER__` consumption/observer witness facts to an accepted
  /// candidate using only existing proof-summary state.
  void AttachCounterStateWitness(AcceptedResultCandidate &candidate) const;

  /// Attach the strongest matching mixed-owner tiling witness for the requested
  /// token envelope, folding through the lattice-level preference gate.
  ///
  /// Include-realization input witnesses are intentionally not formatted as a
  /// separate theorem-facing proof family anymore.  Realized include, macro,
  /// and TU output routes through `OwnerRealizationWitness` so audit logs
  /// expose one owner-polymorphic realization proof instead of parallel
  /// include-specific and owner-specific proof records.
  void AttachMixedOwnerTilingWitnessForTokenEnvelope(ProofSummary &summary,
                                                     uint64_t aStart,
                                                     uint64_t aEnd,
                                                     uint64_t bStart,
                                                     uint64_t bEnd) const;

  /// Discharge the common owner-realization obligations for a constructed
  /// closure.
  ///
  /// This is the collapse point: macro/include/TU realization paths may still
  /// have distinct spelling mechanics, but they all prove the same
  /// owner/source/A-cover/B-envelope/state obligations here.
  OwnerRealizationResult
  TryBuildOwnerRealization(OwnerRealizationEvidenceKind evidence,
                           OwnerClosure closure, llvm::StringRef detail) const;

  /// Attach the shared owner-realization result to an accepted proof summary.
  ///
  /// The shared realization gate is authoritative: once a macro, include, or TU
  /// realization path delegates to `TryBuildOwnerRealization()`, the caller may
  /// not independently declare the realized edit admissible after that gate
  /// rejects it.  This helper therefore records accepted witnesses and converts
  /// rejected results into a local proof-discharge failure on the candidate
  /// that tried to realize the owner.
  void ApplyOwnerRealizationResultToProofSummary(
      ProofSummary &summary, const OwnerRealizationResult &result) const;

  /// Build an owner-realization result for a macro whole-cover path.
  ///
  /// The helper does not manufacture replacement text; it only constructs the
  /// macro-specific closure and delegates the shared proof obligations to
  /// `TryBuildOwnerRealization()`.
  OwnerRealizationResult BuildMacroWholeCoverOwnerRealization(
      const RefoldModel::MacroInvocation &macro,
      const WholeCoverPlan &plan) const;

  /// Build an owner-realization result for an emitted include realization.
  ///
  /// The helper does not manufacture include spelling; it only constructs the
  /// include-specific closure and delegates the shared proof obligations to
  /// `TryBuildOwnerRealization()`.
  OwnerRealizationResult BuildIncludeOwnerRealization(
      const RefoldModel::IncludeItem &include, AcceptedPathKind currentPath,
      IncludeRealizationEvidenceKind evidenceKind,
      std::optional<IncludeRealizationBTokenEnvelope> bTokenEnvelope) const;

  /// Build an owner-realization result for a direct TU realization path.
  ///
  /// The helper does not construct the byte edit; it only builds the TU closure
  /// and delegates the common owner-realization obligations to
  /// `TryBuildOwnerRealization()`.
  OwnerRealizationResult BuildTUOwnerRealization(AcceptedPathKind currentPath,
                                                 uint64_t begin,
                                                 uint64_t end) const;

  /// Build the generic proof summary for realized include/TU/macro output.
  ///
  /// Proof is separate from spelling: include, TU, and macro emitters still own
  /// their materialized surface metadata, while this helper owns only the
  /// theorem-facing OwnerRealizationWitness installation.  Keeping the helper
  /// proof-only prevents spelling fields from becoming a second
  /// owner-realization proof family.
  ProofSummary BuildOwnerRealizationProofSummary(
      AcceptedPathKind currentPath,
      const OwnerRealizationResult &ownerRealization) const;

private:
  Dependencies deps_;
};

} // namespace refold
} // namespace clang

#endif // LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDOWNERREALIZATIONPROOFBUILDER_H
