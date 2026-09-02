//===--- RefoldAcceptedCandidateBuilder.h ---------------------*- C++ -*-===//
//
// Accepted-result candidate factory.
//
// Owns the family of `BuildAccepted*` factories that turn a concrete
// patch (macro / include / TU / terminal) into a fully-witnessed
// `AcceptedResultCandidate`, plus the small selector-carrier
// construction and emission-recertification helpers that operate on
// those candidates.
//
// Methods:
//   * `BuildAcceptedMacroCandidate` — the largest factory; classifies
//     the macro patch's proof, attaches every macro-side witness, and
//     records the emission-path inventory.
//   * `RecertifyAcceptedMacroCandidateForEmission` /
//     `BuildAcceptedMacroEmissionCandidate` /
//     `BuildAcceptedEmittedMacroCandidate` — re-run the macro proof
//     gate for emission and materialize the emission carrier.
//   * `BuildAcceptedIncludeCandidate` /
//     `BuildAcceptedIncludeRealizationCandidate` — include-side
//     equivalents (preservation vs. realization).
//   * `BuildAcceptedTUTextEditCandidate` — TU byte-edit factory.
//   * `BuildAcceptedTerminalCandidate` — terminal-fallback carrier.
//   * `BuildMacroSelectionCandidate` /
//     `FinalizeSelectedMacroPatchForEmission` /
//     `CertifySelectedMacroPatchCandidate` — selector-carrier
//     construction and final selection certifying.
//
// The service has no back-reference to `RefoldProofLattice`.  The
// adjacent classifier services (`RefoldMacroPatchProofClassifier`,
// `RefoldAcceptancePathClassifier`) are held as direct references in
// `Dependencies` rather than `std::function` callbacks, keeping the
// per-candidate factory path free of type-erased calls.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDACCEPTEDCANDIDATEBUILDER_H
#define LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDACCEPTEDCANDIDATEBUILDER_H

#include "core/RefoldModel.h"
#include "edit/RefoldPatchTypes.h"
#include "proof/RefoldAcceptancePathClassifier.h"
#include "proof/RefoldAcceptedResultRanker.h"
#include "proof/RefoldAcceptedResultTypes.h"
#include "proof/RefoldMacroPatchProofClassifier.h"
#include "proof/RefoldOwnerRealizationProofBuilder.h"
#include "proof/RefoldOwnerStateTypes.h"
#include "proof/RefoldProofSummaryBuilder.h"
#include "proof/RefoldProofVocabulary.h"
#include "proof/RefoldTheoremAudit.h"
#include "proof/RefoldWitnessResolver.h"
#include "proof/RefoldWitnessTrace.h"
#include "source/RefoldToken.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"

#include <cstdint>
#include <functional>
#include <optional>

namespace clang {
namespace refold {

struct StructuralHunkSegmentBinding;
struct TUByteSpanPlan;

namespace diffutils {
struct Hunk;
} // namespace diffutils

class RefoldArgTextRecovery;
class RefoldMacroTopology;
class RefoldSourceMapper;
class RefoldTokenTextAnalysis;

/// Accepted-result candidate factory.
///
/// Owns construction of normalized `AcceptedResultCandidate` and
/// `MacroSelectionCandidate` carriers from already-built macro, include, TU,
/// and terminal-fallback artifacts.
class RefoldAcceptedCandidateBuilder {
public:
  /// Borrowed inputs.  Every reference must outlive the builder; the
  /// lattice owns the underlying storage.
  struct Dependencies {
    const RefoldModel &model;
    const RefoldTokenTextAnalysis &tokenText;
    const RefoldArgTextRecovery &argTextRecovery;
    const RefoldMacroTopology &macroTopology;
    const RefoldSourceMapper &sourceMapper;
    llvm::ArrayRef<PPTok> bToks;
    const RefoldTheoremAudit &theoremAudit;
    const RefoldWitnessTrace &witnessTrace;
    const RefoldWitnessResolver &witnessResolver;
    const RefoldAcceptedResultRanker &acceptedResultRanker;
    const RefoldProofSummaryBuilder &proofSummaryBuilder;
    const RefoldOwnerRealizationProofBuilder &ownerRealizationProofBuilder;

    /// Direct references to the two adjacent classifier services.  Held
    /// by reference (rather than routed through `std::function`
    /// callbacks) so each per-candidate factory call stays a plain
    /// virtual-free method invocation.
    const RefoldMacroPatchProofClassifier &macroPatchProofClassifier;
    const RefoldAcceptancePathClassifier &acceptancePathClassifier;
  };

  explicit RefoldAcceptedCandidateBuilder(Dependencies deps);

  /// Certify a candidate that has already been selected by the macro
  /// final-selection path.  Records the selection into the theorem
  /// audit.
  void
  CertifySelectedMacroPatchCandidate(MacroPatch &patch,
                                     const AcceptedResultCandidate &candidate,
                                     llvm::StringRef role) const;

  /// Build the macro-local selector carrier for one macro patch.
  ///
  /// The selector candidate retains macro-ranking obligations. The emitted
  /// candidate is built through the emission-specific macro proof gate and is
  /// present only when it normalizes to one final theorem class. This keeps
  /// selector-only macro proofs useful for deterministic ranking without making
  /// them acceptable TextEdit carriers.
  MacroSelectionCandidate
  BuildMacroSelectionCandidate(const MacroPatch &patch,
                               bool allowNonTopLevelMacroSelectorFailure) const;

  /// Build an accepted-result candidate for a macro patch.
  ///
  /// Packages an already constructed macro patch with its normalized accepted
  /// path, proof-discharge inventory, proof summary, and macro-patch audit
  /// metadata. This is the macro-specific candidate wrapper used by final
  /// selection, theorem audit, and proof discharge.
  AcceptedResultCandidate
  BuildAcceptedMacroCandidate(const MacroPatch &patch) const;

  /// Recertify a macro selector carrier for byte-edit emission.
  ///
  /// The input candidate must come from BuildAcceptedMacroCandidate().  This
  /// helper owns only the selector-to-emission proof transition, so macro
  /// selection can derive both carriers without rebuilding or reauditing the
  /// same MacroPatch proof twice.
  AcceptedResultCandidate RecertifyAcceptedMacroCandidateForEmission(
      const MacroPatch &patch, AcceptedResultCandidate candidate) const;

  /// Build the accepted-result carrier used by macro emission.
  ///
  /// Final macro selection uses BuildAcceptedMacroCandidate(), because selector
  /// competition must still enforce selector-only obligations such as the
  /// top-level proof-root rule.  At the byte-edit emission boundary, however,
  /// an already accepted nested preserving macro patch is validated with the
  /// emitted-artifact contract instead of being reclassified as a selector-only
  /// candidate.  This helper is the single normalization point for that emitted
  /// macro carrier so selection finalization and TextEdit attachment do not
  /// duplicate the recertification logic.
  AcceptedResultCandidate
  BuildAcceptedMacroEmissionCandidate(const MacroPatch &patch) const;

  /// Return the selected carrier used at the actual macro byte-edit emission
  /// boundary.
  ///
  /// This helper is a pure boundary accessor: proof authority must already
  /// have been selected and certified before the MacroPatch entered the final
  /// emission bucket.  A missing selectedAcceptedCandidate is therefore an
  /// invariant violation and is never repaired here by rebuilding from the raw
  /// MacroPatch.
  AcceptedResultCandidate
  BuildAcceptedEmittedMacroCandidate(const MacroPatch &patch) const;

  /// Ensure a macro patch queued for emission has a selected carrier.
  ///
  /// All MacroPatch objects that reach macroPatchesByOwner must cross this
  /// helper before they can later materialize as TextEdit objects.  It
  /// refreshes the canonical MacroPatchProof summary, builds the
  /// emitted-boundary AcceptedResultCandidate through the shared theorem gate,
  /// and certifies the selected carrier on the patch.  If the patch cannot
  /// normalize to one final theorem proof, the shared missing-carrier invariant
  /// requests terminal fallback instead of allowing an uncertified raw MacroPatch
  /// to survive to emission.
  bool FinalizeSelectedMacroPatchForEmission(MacroPatch &patch,
                                             llvm::StringRef role) const;

  /// Build an accepted-result candidate for an include-preserving path.
  ///
  /// Packages an already constructed include patch with its normalized accepted
  /// path, proof-discharge inventory, and optional include-anchor witness.
  /// Include realization no longer carries a parallel include-specific closure
  /// witness; realized include output is routed through OwnerRealizationWitness
  /// by BuildAcceptedIncludeRealizationCandidate().
  AcceptedResultCandidate BuildAcceptedIncludeCandidate(
      AcceptedPathKind currentPath, const IncludePatch &patch,
      const IncludeAnchorWitness *anchorWitness) const;

  /// Build an accepted-result candidate for an emitted include realization.
  ///
  /// Used when the engine materializes an include expansion directly rather
  /// than carrying an `IncludePatch` from the ordinary include-patch path.  The
  /// optional B-token envelope is owner-realization input, not a separate
  /// include proof family: TryBuildOwnerRealization() validates the common
  /// owner/source/A-cover/B-envelope/state obligations.
  AcceptedResultCandidate BuildAcceptedIncludeRealizationCandidate(
      AcceptedPathKind currentPath, const RefoldModel::IncludeItem &include,
      IncludeRealizationEvidenceKind evidenceKind =
          IncludeRealizationEvidenceKind::Unknown,
      std::optional<IncludeRealizationBTokenEnvelope> bTokenEnvelope =
          std::nullopt) const;

  /// Build an accepted-result candidate for an ordinary direct TU span.
  ///
  /// The exact token hunk and final span plan are proof inputs. They are routed
  /// through `BuildTUOwnerRealization()` so the resulting OwnerClosure carries
  /// real A/B ranges and the owner gate cannot manufacture byte-span authority.
  AcceptedResultCandidate BuildAcceptedTUTextEditCandidate(
      AcceptedPathKind currentPath, const diffutils::Hunk &hunk,
      const TUByteSpanPlan &spanPlan,
      const StructuralHunkSegmentBinding *structuralBinding,
      llvm::StringRef payloadPreview) const;

  /// Build an accepted-result carrier for an explicitly specialized TU edit.
  ///
  /// These callers already own directive/state-specific proof machinery and do
  /// not claim ordinary `TUByteSpan` evidence. Keeping the factory separate
  /// prevents future direct callers from dropping their hunk/span proof inputs.
  AcceptedResultCandidate BuildAcceptedSpecializedTUTextEditCandidate(
      AcceptedPathKind currentPath, uint64_t begin, uint64_t end,
      llvm::StringRef payloadPreview) const;

  /// Build an accepted-result candidate for terminal fallback.
  ///
  /// Wraps the explicit out-of-domain terminal result in the same normalized
  /// candidate structure as ordinary accepted results. This keeps terminal
  /// fallback visible to selection, theorem audit, and proof-discharge
  /// reporting instead of treating it as an implicit escape path.
  AcceptedResultCandidate
  BuildAcceptedTerminalCandidate(const TerminalFallbackWitness &witness) const;

private:
  /// Attach the standard witnesses, then record the finished candidate under
  /// its builder name in the legacy-authority audit and the witness trace.
  ///
  /// Every carrier builder whose candidate is complete at return ends here, so
  /// the audit name and the trace name cannot drift apart.  Both trailing calls
  /// are observation-only and must stay that way: per CLAUDE.md a diagnostic
  /// may expose proof state but never influence it.
  void FinalizeAcceptedCandidate(AcceptedResultCandidate &candidate,
                                 llvm::StringRef builderName) const;

  Dependencies deps_;
};

} // namespace refold
} // namespace clang

#endif // LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDACCEPTEDCANDIDATEBUILDER_H
