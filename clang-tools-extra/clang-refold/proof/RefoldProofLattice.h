//===--- RefoldProofLattice.h ----------------------------------*- C++ -*-===//
//
// Composition boundary for clang-refold's accepted-result proof stack.
//
// `RefoldProofLattice` is primarily a composition boundary: it owns the
// shared construction context (`Hooks`, borrowed model/mapper/topology
// inputs, mixed-owner tiling storage) and holds the sub-services callers
// reach through named accessors:
//
//   * `WitnessTrace()` / `EquivalenceKeyBuilder()` /
//     `WitnessResolver()` / `AcceptedResultRanker()` /
//     `ProofSummaryBuilder()` — proof primitives.
//   * `OwnerRealizationProofBuilder()` — owner-realization construction
//     + witness attachment.
//   * `AcceptedCandidateBuilder()` — the `BuildAccepted*` /
//     `Recertify*` / `Finalize*` / `CertifySelected*` factory family.
//   * `MacroPatchProofClassifier()` — `ClassifyMacroPatchProof`,
//     `SyncMacroPatchProofSummary`, and the three
//     invocation-preserving / realization validators.
//   * `AcceptancePathClassifier()` — acceptance-path inventory /
//     theorem-class / baseline discharge / path proof-summary builders.
//
// The methods that remain as concrete members on the lattice fall into
// three groups:
//
//   * **Proof carrier primitives** used by both the lattice and its
//     sub-services: `MakeMacroPatchProof`, `SetMacroPatchProof`,
//     `CertifyMacroWholeCoverRealizationPatch`.  These stay because
//     they mutate the patch's proof carrier in place and are the
//     natural composition seam between construction and classification.
//   * **Path-preservation validators owned by the lattice**:
//     `ValidateIncludePreservingProof`, `ValidateTUAnchorProof`.  These are
//     include/TU-specific and are not on the macro hot path; they stay on the
//     lattice so `AcceptancePathClassifier` can reach them through two
//     remaining `std::function` callbacks without a mutual-dependency cycle.
//   * **Terminal-fallback / whole-cover helpers**:
//     `BuildTerminalFallbackWitness`,
//     `IsOwnerUnresolvedNoTUAnchorOutOfDomain`,
//     `BuildOwnerUnresolvedNoTUAnchorDetail`,
//     `BuildWholeCoverReplacementText`.  Small, cohesive with the
//     lattice's composition role.
//
// Callers use `RefoldEngine::ProofLattice()` rather than RefoldEngine
// forwarding wrappers.  Proof/witness, accepted-result, and edit
// carriers live in focused namespace-level headers.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDPROOFLATTICE_H
#define LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDPROOFLATTICE_H

#include "core/RefoldLog.h"
#include "edit/RefoldEditTypes.h"
#include "edit/RefoldPatchTypes.h"
#include "line-control/RefoldLineControlProof.h"
#include "macro/RefoldMacroTopology.h"
#include "proof/RefoldAcceptancePathClassifier.h"
#include "proof/RefoldAcceptedCandidateBuilder.h"
#include "proof/RefoldAcceptedResultRanker.h"
#include "proof/RefoldAcceptedResultTypes.h"
#include "proof/RefoldMacroPatchProofClassifier.h"
#include "proof/RefoldOwnerRealizationProofBuilder.h"
#include "proof/RefoldOwnerStateTypes.h"
#include "proof/RefoldProofSummaryBuilder.h"
#include "proof/RefoldProofVocabulary.h"
#include "proof/RefoldTheoremAudit.h"
#include "proof/RefoldWitnessEquivalenceKeyBuilder.h"
#include "proof/RefoldWitnessResolver.h"
#include "proof/RefoldWitnessTrace.h"
#include "source/DiffAlgorithms.h"
#include "source/RefoldSourceMapper.h"
#include "source/RefoldToken.h"
#include "source/RefoldTokenTextAnalysis.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/STLFunctionalExtras.h"
#include "llvm/ADT/StringRef.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace clang {
namespace refold {

class RefoldArgTextRecovery;
class RefoldOwnerStateProof;
class RefoldTUEditPlanner;
class RefoldTheoremAudit;

/// Facade for proof-summary construction, validation, ranking, and discharge.
///
/// The lattice composes the narrower proof services at construction/emission
/// boundaries.  It does not own low-level source mapping or macro replay; it
/// normalizes their evidence into theorem-facing proof records.
class RefoldProofLattice {
public:
  /// Exceptional callback set for proof-lattice operations that still break a
  /// construction cycle.  Whole-cover planning is owned by the macro patch
  /// planner, which itself depends on the lattice for proof classification, so
  /// this remains a late-bound query until that arbitration surface is split.
  /// Theorem/audit policy is owned by RefoldTheoremAudit.
  struct Hooks {
    std::function<std::optional<WholeCoverPlan>(
        const RefoldModel::MacroInvocation &)>
        computeWholeCoverPlan;
  };

  /// Read-only access to the witness-trace subsystem for proof diagnostics
  /// such as `TraceWitnessFallback(...)`.
  const RefoldWitnessTrace &WitnessTrace() const { return witnessTrace_; }

  /// Read-only access to the witness equivalence-key builder used to
  /// normalize candidate witness identity.
  const RefoldWitnessEquivalenceKeyBuilder &EquivalenceKeyBuilder() const {
    return equivalenceKeyBuilder_;
  }

  /// Read-only access to the witness resolver for witness construction and
  /// selection-time witness-discharge decisions.
  const RefoldWitnessResolver &WitnessResolver() const {
    return witnessResolver_;
  }

  /// Read-only access to the accepted-result ranker for proof-preference,
  /// selectability, and deterministic candidate-ordering decisions.
  const RefoldAcceptedResultRanker &AcceptedResultRanker() const {
    return acceptedResultRanker_;
  }

  /// Read-only access to the proof-summary builder for proof normalization,
  /// emitted-proof construction, and summary finalization.
  const RefoldProofSummaryBuilder &ProofSummaryBuilder() const {
    return proofSummaryBuilder_;
  }

  /// Read-only access to the owner-realization proof builder.  Owns
  /// `AttachLineControlObserverWitness`, `AttachCounterStateWitness`,
  /// `AttachMixedOwnerTilingWitnessForTokenEnvelope`,
  /// `TryBuildOwnerRealization`,
  /// `ApplyOwnerRealizationResultToProofSummary`, and the three
  /// `Build*OwnerRealization` / `BuildOwnerRealizationProofSummary`
  /// helpers.
  const RefoldOwnerRealizationProofBuilder &
  OwnerRealizationProofBuilder() const {
    return ownerRealizationProofBuilder_;
  }

  /// Read-only access to the accepted-result candidate factory.  Owns
  /// the `BuildAccepted*` / `Recertify*` / `Finalize*` /
  /// `CertifySelected*` factory family.
  const RefoldAcceptedCandidateBuilder &AcceptedCandidateBuilder() const {
    return acceptedCandidateBuilder_;
  }

  /// Read-only access to the macro-patch proof classifier.  Owns
  /// `ClassifyMacroPatchProof`,
  /// `SyncMacroPatchProofSummary`,
  /// `MacroInvocationHasWellFormedPasteWitnesses`, and the three
  /// invocation-preserving / realization `Validate*Proof` methods.
  const RefoldMacroPatchProofClassifier &MacroPatchProofClassifier() const {
    return macroPatchProofClassifier_;
  }

  /// Read-only access to the acceptance-path proof-summary classifier.
  /// Owns the acceptance-path inventory / theorem-class / baseline
  /// discharge / path-summary builders.
  const RefoldAcceptancePathClassifier &AcceptancePathClassifier() const {
    return acceptancePathClassifier_;
  }

  explicit RefoldProofLattice(
      const RefoldModel &model, llvm::StringRef bSource,
      llvm::ArrayRef<PPTok> bToks, RefoldSourceMapper &sourceMapper,
      const RefoldTokenTextAnalysis &tokenText,
      const RefoldArgTextRecovery &argTextRecovery,
      const RefoldMacroTopology &macroTopology,
      const RefoldOwnerStateProof &ownerStateProof,
      const RefoldTerminalProofSink &terminalSink,
      const RefoldTUEditPlanner &tuEdits,
      const RefoldTheoremAudit &theoremAudit,
      TheoremAuditStats &lastTheoremAudit, bool strict,
      ProofAuditMode &proofAuditMode,
      const bool &alignmentSemanticTheoremActive,
      std::vector<MixedOwnerTilingSegmentBinding>
          &mixedOwnerTilingSegmentBindings,
      std::vector<MixedOwnerTilingWitness> &mixedOwnerTilingWitnesses,
      Hooks hooks);

  /// Build the canonical proof carrier for a macro patch.
  ///
  /// This small factory keeps call sites from open-coding the three primary
  /// proof facts while the proof model is carrier-first.  More specialized
  /// witnesses are attached to the returned MacroPatchProof before
  /// SetMacroPatchProof() when the construction site already owns them.
  ::clang::refold::MacroPatchProof
  MakeMacroPatchProof(MacroPatchProofKind kind,
                      bool preservesInvocationStructure,
                      uint64_t proofRootMacroId) const;

  /// Install the canonical macro proof carrier and refresh summaries.
  ///
  /// This is the only primary certification API for macro-patch proof identity.  The
  /// normalized ProofSummary and its canonical EmittedProof are rebuilt from
  /// MacroPatchProof immediately through `MacroPatchProofClassifier()`.
  void SetMacroPatchProof(MacroPatch &patch, MacroPatchProof proof) const;

  /// Materialize the explicit whole-cover realization proof.
  ///
  /// Whole-cover output is not tracked as an anonymous fallback result.  This
  /// helper certifies the accepted patch as a first-class invocation-realization
  /// proof and copies the exact realization envelope derived by the whole-cover
  /// plan into the patch-local certificate fields.
  void CertifyMacroWholeCoverRealizationPatch(
      MacroPatch &patch, const WholeCoverPlan &plan,
      const RefoldModel::MacroInvocation &macro) const;

  /// Validate an include-preserving accepted path.
  ///
  /// This lattice-facing entry point composes include-anchor evidence with the
  /// accepted-path classifier while keeping include-specific proof details out
  /// of macro and TU validators.
  ::clang::refold::ProofDischargeRecord
  ValidateIncludePreservingProof(AcceptedPathKind currentPath,
                                 const IncludePatch *patch,
                                 const IncludeAnchorWitness *witness) const;

  /// Discharge the proof obligations for a TU anchor path.
  ///
  /// Validates that the accepted path is a TU exact-slot or provable-insertion
  /// anchor and that it carries the local witness fields required by that
  /// anchor class. Exact-slot anchors must identify the matched slot, while
  /// provable insertion anchors must record the supporting neighbor,
  /// include-boundary, macro, or corroborated-owner evidence.
  ::clang::refold::ProofDischargeRecord
  ValidateTUAnchorProof(AcceptedPathKind currentPath,
                        const TUAnchorWitness *witness) const;

  /// Build the terminal-fallback witness for the current refold pass.
  ///
  /// Captures the ordered structured failures recorded by the terminal sink.
  /// The resulting witness is used to make terminal fallback explicit in
  /// accepted-result selection, theorem audit, and proof-discharge reporting.
  TerminalFallbackWitness BuildTerminalFallbackWitness() const;

  /// Return whether the hunk lies on the explicit unresolved-owner /
  /// no-TU-anchor theorem boundary.
  ///
  /// Use the conservative domain-wall interpretation for the last ownership
  /// gap. This predicate does not search for any new witness. It only re-states
  /// the evidence that has already been exhausted:
  ///
  /// * no resolved macro/TU/include owner remained,
  /// * no exact include-boundary owner exists for a pure insertion,
  /// * no truthful TU-owned mapped span exists for the A interval, and
  /// * for pure insertions, no exact/provable TU insertion anchor exists.
  ///
  /// When all of those facts hold, the edit is explicitly outside the declared
  /// structural refolding domain and must terminate via
  /// `OwnerUnresolvedNoTUAnchor`.
  bool IsOwnerUnresolvedNoTUAnchorOutOfDomain(const diffutils::Hunk &h,
                                              llvm::StringRef tuPath,
                                              const Owner &owner,
                                              bool mapsToTU) const;

  /// Build a detailed terminal-fallback reason for the unresolved-owner /
  /// no-TU-anchor domain wall.
  ///
  /// This helper records the deterministic owner and TU-anchor searches that
  /// were already exhausted before the engine concluded that no declared
  /// macro/include/TU proof class could own the edit. It does not guess a new
  /// owner or widen admissibility.
  std::string BuildOwnerUnresolvedNoTUAnchorDetail(size_t hunkIndex,
                                                   const diffutils::Hunk &h,
                                                   llvm::StringRef tuPath,
                                                   const Owner &owner,
                                                   bool mapsToTU) const;

  /// Build the replacement text for a macro whole-cover realization.
  ///
  /// Computes the same whole-cover plan used by macro patch construction and
  /// returns its clipped replacement text. This helper is for callers that need
  /// the materialized whole-cover text without constructing a full
  /// `MacroPatch`.
  std::optional<std::string>
  BuildWholeCoverReplacementText(const RefoldModel::MacroInvocation &m) const;

private:
  Hooks hooks_;
  const RefoldModel &model_;
  /// Retained so an owner-unresolved diagnostic can ask whether a producer
  /// invocation covers the hunk instead of asserting the search was exhausted.
  const RefoldMacroTopology &macroTopology_;
  llvm::StringRef bSource_;
  llvm::ArrayRef<PPTok> bToks_;
  RefoldWitnessTrace witnessTrace_;
  RefoldWitnessEquivalenceKeyBuilder equivalenceKeyBuilder_;
  RefoldProofSummaryBuilder proofSummaryBuilder_;
  RefoldWitnessResolver witnessResolver_;
  RefoldAcceptedResultRanker acceptedResultRanker_;
  RefoldOwnerRealizationProofBuilder ownerRealizationProofBuilder_;
  // The declaration order of the three services below is load-bearing.
  // C++ member init runs in declaration order, and each downstream
  // service holds a *direct reference* to the upstream one (replacing
  // hot-path `std::function` callbacks — ~4 type-erased calls saved
  // per macro-patch classification).  Preserve
  // acceptancePathClassifier_ < macroPatchProofClassifier_ <
  // acceptedCandidateBuilder_.
  RefoldAcceptancePathClassifier acceptancePathClassifier_;
  RefoldMacroPatchProofClassifier macroPatchProofClassifier_;
  RefoldAcceptedCandidateBuilder acceptedCandidateBuilder_;
  const RefoldTerminalProofSink &terminalSink_;

  /// TU planning service used only for theorem diagnostics that need to explain
  /// whether an owner-unresolved hunk still had a deterministic TU witness.
  /// The accepted TU-anchor carrier construction itself lives in
  /// RefoldTUAnchorProof, so this reference does not create a planner/lattice
  /// service cycle.
  const RefoldTUEditPlanner &tuEdits_;
};

} // namespace refold
} // namespace clang

#endif // LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDPROOFLATTICE_H
