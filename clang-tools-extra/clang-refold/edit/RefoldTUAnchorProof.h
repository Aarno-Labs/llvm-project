//===--- RefoldTUAnchorProof.h -------------------------------*- C++ -*-===//
//
// TU-anchor and TU byte-span proofs for clang-refold.
//
// This service proves where a hunk lands in translation-unit source: exact
// slot and provable insertion anchors for pure insertions, the TU byte span of
// a mapped A-token interval, the ordinary/macro-repair envelope checks on
// those spans, and revalidation of a direct TU owner-realization carrier.
// Every successful anchor proof mints its normalized AcceptedResultCandidate
// here.  It does not classify owners, build edit plans, or participate in
// accepted-result selection: RefoldOwnerClassifier, RefoldTUEditPlanner and the
// owner-realization proof builder consume these answers.  The summary and
// contract construction itself is the shared RefoldProofSummaryBuilder, a leaf
// service.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDTUANCHORPROOF_H
#define LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDTUANCHORPROOF_H

#include "model/RefoldToken.h"
#include "proof/RefoldAcceptancePathTypes.h"
#include "proof/RefoldAnchorWitnessTypes.h"
#include "proof/RefoldCandidateTypes.h"
#include "proof/RefoldProofDischargeTypes.h"
#include "proof/RefoldProofSummaryBuilder.h"
#include "proof/RefoldTheoremTypes.h"
#include "source/RefoldDiffTypes.h"
#include "source/RefoldPreprocessingStructureIndex.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/StringRef.h"

#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

namespace clang {
namespace refold {

class LineDirectiveInserter;
class RefoldMacroTopology;
class RefoldPathIdentity;
class RefoldTheoremAudit;

/// Result of proving that an A-side PP gap has a concrete TU byte anchor.
///
/// The optional witness mirrors the existing proof/audit side channel.  Callers
/// that only need the concrete byte offset can use TUByteOffset; callers that
/// forward accepted-result metadata can also preserve the witness unchanged.
struct TUInsertionAnchor {
  /// A-side PP gap that was anchored.
  uint64_t ppGap = 0;
  /// TU byte offset that represents the same insertion coordinate.
  uint64_t tuByteOffset = 0;
  /// Optional accepted-result witness for the anchor proof.
  std::optional<TUAnchorWitness> witness;

  TUInsertionAnchor() = default;

  TUInsertionAnchor(uint64_t ppGap, uint64_t tuByteOffset,
                    std::optional<TUAnchorWitness> witness = std::nullopt)
      : ppGap(ppGap), tuByteOffset(tuByteOffset), witness(std::move(witness)) {}
};

/// Proven adjustment applied to a pure-insertion source anchor.
enum class TUInsertionAnchorAdjustmentKind : uint8_t {
  Unknown,
  SourceLineControlPrefix,
};

/// Typed proof input for moving a pure insertion across preserved source
/// structure without consuming that structure.
struct TUInsertionAnchorAdjustment {
  TUInsertionAnchorAdjustmentKind kind =
      TUInsertionAnchorAdjustmentKind::Unknown;
  uint64_t originalTUByteOffset = 0;
  uint64_t adjustedTUByteOffset = 0;

  bool IsValid() const {
    return kind != TUInsertionAnchorAdjustmentKind::Unknown &&
           originalTUByteOffset <= adjustedTUByteOffset;
  }
};

/// Planned TU byte span for a token hunk.
///
/// The span is half-open in TU byte coordinates.  A pure insertion is
/// represented by Begin == End and may carry the exact insertion anchor that
/// justified the zero-width span.
struct TUByteSpanPlan {
  /// Beginning of the A-token interval represented by this TU byte span.
  uint64_t aTokenBegin = 0;
  /// End of the A-token interval represented by this TU byte span.
  uint64_t aTokenEnd = 0;
  /// Beginning of the half-open TU byte range.
  uint64_t tuByteBegin = 0;
  /// End of the half-open TU byte range.
  uint64_t tuByteEnd = 0;
  /// Exact insertion anchor for pure insertions, when one was proven.
  std::optional<TUInsertionAnchor> insertionAnchor;
  /// Optional typed proof for moving a pure insertion past preserved source
  /// line-control structure while leaving that structure outside the edit.
  std::optional<TUInsertionAnchorAdjustment> insertionAnchorAdjustment;

  TUByteSpanPlan() = default;

  TUByteSpanPlan(
      uint64_t aTokenBegin, uint64_t aTokenEnd, uint64_t tuByteBegin,
      uint64_t tuByteEnd,
      std::optional<TUInsertionAnchor> insertionAnchor = std::nullopt,
      std::optional<TUInsertionAnchorAdjustment> insertionAnchorAdjustment =
          std::nullopt)
      : aTokenBegin(aTokenBegin), aTokenEnd(aTokenEnd),
        tuByteBegin(tuByteBegin), tuByteEnd(tuByteEnd),
        insertionAnchor(std::move(insertionAnchor)),
        insertionAnchorAdjustment(std::move(insertionAnchorAdjustment)) {}

  std::pair<uint64_t, uint64_t> byteRange() const {
    return {tuByteBegin, tuByteEnd};
  }
};

/// Durable binding from one structurally split token hunk back to the proof
/// that protected preprocessing structure remains outside the emitted edit.
///
/// This value type exists before same-owner structural splitting is enabled,
/// so ordinary direct TU spans normally pass no binding. Structural tiling may
/// populate the record only after it has proved a complete original-hunk
/// partition, exact segment identity, exact source-byte
/// ownership, and preservation of every protected interval outside this
/// segment's emitted byte range. A bare witness id or a best-effort split is
/// deliberately insufficient.
struct StructuralHunkSegmentBinding {
  /// Original unsplit A-token envelope.
  uint64_t originalAStart = 0;
  uint64_t originalAEnd = 0;
  /// Original unsplit B-token envelope.
  uint64_t originalBStart = 0;
  uint64_t originalBEnd = 0;
  /// Exact emitted segment A-token envelope.
  uint64_t segmentAStart = 0;
  uint64_t segmentAEnd = 0;
  /// Exact emitted segment B-token envelope.
  uint64_t segmentBStart = 0;
  uint64_t segmentBEnd = 0;
  /// Exact emitted segment TU byte interval.
  uint64_t sourceBegin = 0;
  uint64_t sourceEnd = 0;
  /// Stable identity of the durable structural-tiling witness.
  uint64_t witnessId = 0;
  /// Stable segment ordinal within that witness.
  uint32_t segmentIndex = 0;
  /// The witness proved complete source-byte coverage for the original hunk.
  bool sourceByteCoverComplete = false;
  /// Every protected preprocessing interval lies outside this segment edit.
  bool protectedStructurePreservedOutsideSegment = false;

  /// Return whether the record is internally well formed.
  bool IsWellFormed() const {
    const bool isProperSegment =
        originalAStart != segmentAStart || originalAEnd != segmentAEnd ||
        originalBStart != segmentBStart || originalBEnd != segmentBEnd;
    return originalAStart <= segmentAStart && segmentAEnd <= originalAEnd &&
           originalBStart <= segmentBStart && segmentBEnd <= originalBEnd &&
           segmentAStart <= segmentAEnd && segmentBStart <= segmentBEnd &&
           sourceBegin <= sourceEnd && isProperSegment && witnessId != 0 &&
           sourceByteCoverComplete &&
           protectedStructurePreservedOutsideSegment;
  }
};

/// Return whether a TU-anchor witness carries the path-local evidence required
/// by TUProvableInsertionAnchor.
///
/// Exact-slot witnesses use a stronger, separate ExactSlotBoundary contract and
/// therefore intentionally return false here.
bool tuAnchorWitnessHasProvableEvidence(const TUAnchorWitness &witness);

/// Validate the local obligations for a TU-anchor proof using the caller's
/// already-computed accepted-path inventory.
///
/// The inventory parameter lets RefoldAcceptancePathClassifier preserve its
/// generic path classification semantics while RefoldTUAnchorProof can reuse
/// the same validator with its narrower TU-anchor-only inventory.
ProofDischargeRecord
validateTUAnchorProof(AcceptedPathKind currentPath,
                      const TUAnchorWitness *witness,
                      const AcceptancePathInventory &inventory);

/// Proves TU insertion anchors and TU byte spans, and builds the
/// accepted-result carriers for the anchors it proves.
///
/// The service answers where a hunk lands in TU source; ranking and
/// theorem-selection behavior remain with the accepted-result selector.
class RefoldTUAnchorProof {
public:
  /// Borrowed producer facts and token streams for TU-anchor and TU byte-span
  /// proofs.
  struct Deps {
    /// Producer model containing TU/source ownership facts.
    const RefoldModel &model;
    /// Path identity service for TU/include boundary comparisons.
    const RefoldPathIdentity &pathIdentity;
    /// Macro topology service used for arg-like anchors and macro-boundary
    /// checks.
    const RefoldMacroTopology &macroTopology;
    /// Logical line directive model for exact slot boundary queries.
    const LineDirectiveInserter &lineDirs;
    /// Exact preprocessing-structure census for the physical translation unit.
    const RefoldPreprocessingStructureIndex &preprocessingStructureIndex;
    /// Exact physical bytes indexed by `preprocessingStructureIndex`.
    llvm::StringRef tuSourceBytes;
    /// A-token stream for owner-depth and suffix checks.
    llvm::ArrayRef<PPTok> aTokens;
    /// Number of edited-preprocessed B tokens available to direct hunks.
    uint64_t bTokenCount = 0;
    /// B-token stream, used to ask what an insertion would actually emit.  It
    /// is also the shared proof-summary builder's only hard input.
    llvm::ArrayRef<PPTok> bTokens;
  };

  RefoldTUAnchorProof(const RefoldTheoremAudit &theoremAudit, Deps deps);

  /// Build the normalized carrier for a proven TU insertion anchor.
  ///
  /// Only TUExactSlotBoundary and TUProvableInsertionAnchor are valid anchor
  /// paths.  Other paths fail closed by producing the same undischarged summary
  /// shape that the lattice-side builder would have produced for a misrouted
  /// TU-anchor request.
  AcceptedResultCandidate
  BuildAcceptedTUAnchorCandidate(AcceptedPathKind currentPath,
                                 const TUAnchorWitness &witness) const;

  /// Anchors a *pure insertion* (a PP-gap insertion) to a deterministic,
  /// canonical TU byte boundary representing the *same* preprocessed
  /// coordinate, when possible.
  ///
  /// A PP-gap `ppGap` is a boundary between two adjacent PP tokens (i.e. a
  /// "gap" index). For an insertion that conceptually occurs at that PP
  /// boundary, this method returns the TU byte offset of an **exact**
  /// structural boundary corresponding to that same PP coordinate.
  ///
  /// **Key property:** this method performs *no* "nearest" snapping. If `ppGap`
  /// does not exactly match a known boundary PP coordinate, it returns
  /// `std::nullopt` so callers can fall back to neighbor-based span anchoring.
  /// This avoids regressions where an insertion belonging inside a nested owner
  /// (include/arm) is incorrectly pulled out to a shallower boundary.
  ///
  /// **Boundary sources considered** include explicit TU slots with an emitted
  /// `pp` coordinate and a conservative boundary-like slot kind.
  ///
  /// **Directive-line newline adjustment:** some recorded boundary slots may
  /// point at the newline that terminates a preprocessor directive line. For
  /// insertions at those boundaries, anchoring at the newline byte can cause
  /// directive concatenation. Candidates for selected slot kinds are adjusted
  /// to anchor after the newline, handling both `\n` and `\r\n`.
  ///
  /// **Exact-match requirement:** candidates are filtered to those whose PP
  /// coordinate equals `ppGap` exactly. If none match, returns `std::nullopt`.
  ///
  /// **Deterministic tie-breaking:** if multiple candidates share the same PP
  /// coordinate, the chosen candidate is the one with the highest priority by
  /// slot kind, then the smallest TU byte offset, then the smallest slot id.
  std::optional<uint64_t> AnchorToExactSlotBoundaryFromPPGap(
      llvm::StringRef tuPath, uint64_t ppGap,
      TUAnchorWitness *witness = nullptr,
      AcceptedResultCandidate *acceptedCandidate = nullptr) const;

  /// Query the producer-backed exact-slot boundary helper used by TU-anchor
  /// diagnostics.
  ///
  /// This deliberately exposes only the low-level slot lookup that the anchor
  /// proofs already consume. It does not classify ownership, widen spans, or
  /// build text edits; callers that need the full insertion-anchor proof should
  /// use FindProvableTUInsertionAnchor() instead.
  std::optional<uint64_t> FindExactSlotBoundaryFromPPGap(llvm::StringRef tuPath,
                                                         uint64_t ppGap) const;

  /// Return true iff a PP gap sits at the exit of a selected conditional arm.
  bool IsPPGapAtSelectedConditionalArmExit(uint64_t ppGap) const;

  /// \brief Return a conservative TU byte anchor for a pure insertion at PP gap
  /// \p pp.
  ///
  /// Determines whether the empty A-side hunk at preprocessing-output gap \p pp
  /// has a \em provable insertion point in the translation unit identified by
  /// \p tuPath. This proof is used for two purposes: deciding whether the pure
  /// insertion is truthfully TU-owned, and materializing the corresponding
  /// zero-width TU span in byte space.
  ///
  /// The check is intentionally fail-closed. It accepts only exact structural
  /// slot anchors recorded by the producer, exact TU-side macro "arg-like
  /// begin" anchors for wrapper/deferred expansion shapes, immediate mapped TU
  /// neighbors when the gap is outside include coverage, a zero-token top-level
  /// include boundary bracketed by the same PP gap, or, in non-strict mode, a
  /// bounded whitespace probe whose nearest mapped neighbors on both sides
  /// agree on TU ownership without crossing an owner-depth boundary.
  ///
  /// The returned carrier records both the concrete TU byte and the witness
  /// that justified it. The optional out-parameters let existing orchestration
  /// forward the exact witness/candidate without rebuilding proof metadata.
  std::optional<TUInsertionAnchor> FindProvableTUInsertionAnchor(
      uint64_t pp, llvm::StringRef tuPath, TUAnchorWitness *witness = nullptr,
      AcceptedResultCandidate *acceptedCandidate = nullptr) const;

  /// Project the TU-owned subset of an A-token interval to its legacy physical
  /// min/max envelope for owner topology only.
  ///
  /// This method does not authorize an edit.  It preserves the historical
  /// owner-classification projection used to locate include/conditional slot
  /// surfaces without coupling ownership to the stronger direct-realization
  /// theorem implemented by `PlanTUByteSpan()`.
  std::optional<TUByteSpanPlan>
  ProjectTUByteEnvelopeForOwnership(uint64_t a0, uint64_t a1,
                                    llvm::StringRef tuPath) const;

  /// \brief Compute the TU byte span \c [b,e) corresponding to an A-side
  /// PP-token interval \c [a0,a1).
  ///
  /// This routine converts a diff hunk expressed in A-token indices into a
  /// concrete byte range in the TU source file. The contract is intentionally
  /// conservative: if the interval cannot be proven to touch the TU, or cannot
  /// be safely anchored into the TU for a pure insertion, the method returns
  /// \c std::nullopt rather than "snapping" across ownership boundaries.
  ///
  /// A nonempty interval is admitted only when every consumed A token has one
  /// exact in-bounds TU mapping and those mappings are source-monotone and
  /// nonoverlapping.  Each internal physical gap is then discharged in one of
  /// two separate proof domains: ordinary lexer trivia with no overlapping
  /// protected structure, or complete producer-bound `#define`/`#undef`
  /// evidence converted into explicit obligations for mandatory macro-state
  /// repair.  Every other preprocessing construct or unknown byte rejects the
  /// span until structural tiling can leave it outside the edit.
  ///
  /// For pure insertions, this first tries an exact producer-recorded TU
  /// slot boundary at the same PP gap. If no exact slot exists, it defers to
  /// the same provable TU insertion-anchor logic used by owner classification.
  /// A returned span with Begin == End denotes a concrete insertion anchor
  /// point in the TU.
  std::optional<TUByteSpanPlan> PlanTUByteSpan(uint64_t a0, uint64_t a1,
                                               llvm::StringRef tuPath) const;

  /// Revalidate the concrete carrier used by a direct TU owner realization.
  ///
  /// This is intentionally stronger than owner classification. The supplied
  /// hunk must match the span's A envelope exactly, the hunk's B envelope must
  /// be a real edited-token interval, the final physical byte interval must
  /// contain the independently re-derived base span, and protected structure
  /// may appear in the provisional carrier only as complete producer-bound
  /// `#define`/`#undef` evidence deferred to mandatory macro-state repair. This
  /// validation grants no authority to consume those intervals. An optional
  /// structural binding is accepted only when it names this exact segment and
  /// proves that all protected structure remains outside the edit.
  bool ValidateTUOwnerRealizationCarrier(
      const diffutils::Hunk &hunk, const TUByteSpanPlan &span,
      const StructuralHunkSegmentBinding *structuralBinding = nullptr) const;

  /// Return true when a pure insertion would supply a macro replacement-list
  /// literal from outside its invocation.
  ///
  /// The token LCS may slide an inserted token left across identical fixed
  /// replacement-list literals. With `#define M(x) ((x) >= 0)`, rewriting the
  /// actual to `(y).f` admits an alignment in which the TU token preceding the
  /// invocation anchors to the macro body's leading `(` and the invocation's
  /// own leading `(` becomes a pure insertion in the TU. Both parentheses are
  /// spelled identically, so nothing textual separates them; only definition
  /// tape replay against the producer-recorded expansion envelope can decide
  /// which side owns the token, and `RefoldMacroDefinitionTapeSolver` exists to
  /// make exactly that decision.
  ///
  /// Reaching a direct-TU carrier means that replay did not absorb the token.
  /// Emitting it as TU text would leave the invocation realizing an incomplete
  /// instance of its replacement list, with a fixed body literal supplied by a
  /// neighboring carrier that never proved the composition. The realized source
  /// then reproduces a different expansion than the edited stream requires, so
  /// this configuration fails closed rather than composing two locally valid
  /// carriers into an unproved whole.
  bool InsertionSuppliesMacroBoundaryLiteral(
      const diffutils::Hunk &hunk) const;

  /// Validate a provisional direct-TU source envelope.
  ///
  /// Complete producer-bound `#define`/`#undef` intervals may be recognized
  /// only as evidence that the specialized macro-state repair planner has an
  /// exact transition to inspect.  This theorem creates no protected-source
  /// authority, and the final edit remains inadmissible until that specialized
  /// planner has discharged every consumed transition.
  bool ValidateDirectTUEnvelope(llvm::StringRef tuPath, uint64_t begin,
                                uint64_t end) const;

  /// Validate a direct-TU envelope that may contain only ordinary source.
  ///
  /// This stricter form is used by independent widening theorems that have no
  /// pre-existing macro transition to defer to specialized repair.
  bool ValidateOrdinaryDirectTUEnvelope(llvm::StringRef tuPath,
                                        uint64_t begin, uint64_t end) const;

private:
  /// Collect the exact macro transitions a direct-TU envelope defers to the
  /// specialized repair planner.
  ///
  /// The returned intervals are evidence only. The query rejects partial
  /// macro lines and every unrelated protected construct; a `_Pragma` token
  /// nested wholly inside one collected macro replacement is the only nested
  /// interval admitted by the same physical transition.
  bool CollectDirectTUMacroRepairEvidence(
      uint64_t begin, uint64_t end,
      std::vector<const PreprocessingStructureInterval *> &intervals) const;

  /// Collect the exact macro-repair evidence in an envelope, accepting an
  /// empty evidence set when the envelope contains no protected structure.
  bool CollectDirectTUMacroRepairEvidenceOrEmpty(
      uint64_t begin, uint64_t end,
      std::vector<const PreprocessingStructureInterval *> &intervals) const;

  /// Return whether widening preserves exactly the macro-transition evidence
  /// already present in the independently re-derived base carrier.
  bool DirectTUEnvelopeRetainsMacroRepairEvidence(
      llvm::StringRef tuPath, uint64_t baseBegin, uint64_t baseEnd,
      uint64_t widenedBegin, uint64_t widenedEnd) const;

  /// Prove a direct-TU gap that is eligible for specialized macro repair.
  ///
  /// Ordinary bytes are discharged exclusively by
  /// `ProveOrdinaryDirectTUInternalGap()`. Complete producer-bound
  /// `#define`/`#undef` intervals may complete the gap only as evidence for the
  /// later macro-state repair planner. No authorization record is returned or
  /// stored by the direct-span theorem.
  bool ProveDirectTUGapWithMacroRepairEvidence(uint64_t begin,
                                               uint64_t end) const;

  /// Return the narrowest include id covering a PP index, if any.
  ///
  /// This is a TU-anchor guard, not general owner classification: it prevents a
  /// zero-width TU insertion from being proved through a PP gap that lies
  /// inside an include expansion.
  std::optional<uint64_t> IncludeIdCoveringPPIndex(uint64_t pp) const;

  /// A-token coordinates that occur more than once in the producer tokmap.
  /// Duplicate entries make the physical spelling ambiguous even when the
  /// model's DenseMap retained one deterministic last writer.
  llvm::DenseSet<uint64_t> duplicateTokmapPP_;

  const RefoldTheoremAudit &theoremAudit_;
  RefoldProofSummaryBuilder proofSummaryBuilder_;
  Deps deps_;
};

} // namespace refold
} // namespace clang

#endif // LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDTUANCHORPROOF_H
