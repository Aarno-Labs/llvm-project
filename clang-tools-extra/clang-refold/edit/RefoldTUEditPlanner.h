//===--- RefoldTUEditPlanner.h ---------------------------------*- C++ -*-===//
//
// Translation-unit edit planning service for clang-refold.
//
// This service owns TU insertion-anchor proof queries, TU byte-span planning,
// pure-insertion include-boundary ownership, direct TU hunk edit plans, and the
// closed trailing call-suffix extension policy.  Final TextEdit assembly
// remains outside this service.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDTUEDITPLANNER_H
#define LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDTUEDITPLANNER_H

#include "edit/RefoldEditTypes.h"
#include "proof/RefoldAcceptedResultTypes.h"
#include "source/DiffAlgorithms.h"
#include "source/RefoldToken.h"
#include "source/RefoldPreprocessingStructureIndex.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/StringRef.h"

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace clang {
namespace refold {

class LineDirectiveInserter;
class RefoldLineControlProof;
class RefoldMacroTopology;
class RefoldModel;
class RefoldPathIdentity;
class RefoldTUAnchorProof;
class RefoldTUEditPlanner;

/// Shared immutable inputs for TU edit-planning routines.
///
/// Run-wide dependencies such as the model, path identity, TU-anchor proof
/// builder remain constructor dependencies of RefoldTUEditPlanner.  This
/// context is for per-hunk/per-file inputs that vary between calls and keeps
/// planner entry points from growing broad ad-hoc parameter lists.
struct TUEditPlanningContext {
  /// Translation-unit path used for owner/proof diagnostics.
  llvm::StringRef tuPath;
  /// Translation-unit source bytes used for TU byte-span projection.
  llvm::StringRef tuBytes;
  /// A/B token-level hunk being planned; null only for default-constructed
  /// values.
  const diffutils::Hunk *hunk = nullptr;
  /// Stable index of `hunk` in the A/B token-hunk vector.
  uint64_t hunkIndex = 0;
  /// Original A token stream for the current refold pass.
  llvm::ArrayRef<PPTok> aTokens;
  /// Edited B token stream for the current refold pass.
  llvm::ArrayRef<PPTok> bTokens;

  TUEditPlanningContext() = default;

  TUEditPlanningContext(llvm::StringRef tuPath, llvm::StringRef tuBytes,
                        const diffutils::Hunk &hunk, uint64_t hunkIndex,
                        llvm::ArrayRef<PPTok> aTokens,
                        llvm::ArrayRef<PPTok> bTokens)
      : tuPath(tuPath), tuBytes(tuBytes), hunk(&hunk), hunkIndex(hunkIndex),
        aTokens(aTokens), bTokens(bTokens) {}
};

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

  bool isPureInsertion() const { return aTokenBegin == aTokenEnd; }
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

/// Include-boundary owner selected for a pure insertion before TU edit
/// planning.
///
/// The carrier records stable ids rather than an IncludeItem pointer so callers
/// do not have to expose raw model storage when they only need the resolved
/// boundary ownership.
struct BoundaryParentIncludePlan {
  /// A-side PP gap at the include boundary.
  uint64_t ppGap = 0;
  /// Include instance that owns the boundary.
  uint64_t includeId = 0;
  /// Parent include instance when the boundary is header-owned.
  std::optional<uint64_t> parentIncludeId;

  BoundaryParentIncludePlan() = default;

  BoundaryParentIncludePlan(
      uint64_t ppGap, uint64_t includeId,
      std::optional<uint64_t> parentIncludeId = std::nullopt)
      : ppGap(ppGap), includeId(includeId), parentIncludeId(parentIncludeId) {}
};

/// Result of deciding whether a TU replacement may absorb a closed trailing
/// macro-call suffix.
///
/// This policy is intentionally modeled as a separate carrier, not as an
/// ordinary byte-span widening flag.  Closed suffix extension is allowed only
/// after the added TU bytes are proven to be closed over the corresponding
/// mapped B-token interval; keeping the carrier separate prevents generic TU
/// byte-span planning from widening spans accidentally.
struct TUTrailingCallSuffixExtension {
  /// Original exclusive A-token end before suffix extension.
  uint64_t originalATokenEnd = 0;
  /// Extended exclusive A-token end after absorbing the suffix.
  uint64_t extendedATokenEnd = 0;
  /// Original exclusive TU source-byte end before suffix extension.
  uint64_t originalTUByteEnd = 0;
  /// Extended exclusive TU source-byte end after absorbing the suffix.
  uint64_t extendedTUByteEnd = 0;
  /// Inclusive B-token index for the suffix surface to prove closed.
  uint64_t bTokenBegin = 0;
  /// Exclusive B-token index for the suffix surface to prove closed.
  uint64_t bTokenEnd = 0;
  /// True when the B-token suffix surface is token-closed for TU widening.
  bool bTokenSuffixClosed = false;

  TUTrailingCallSuffixExtension() = default;

  TUTrailingCallSuffixExtension(uint64_t originalATokenEnd,
                                uint64_t extendedATokenEnd,
                                uint64_t originalTUByteEnd,
                                uint64_t extendedTUByteEnd,
                                uint64_t bTokenBegin, uint64_t bTokenEnd,
                                bool bTokenSuffixClosed)
      : originalATokenEnd(originalATokenEnd),
        extendedATokenEnd(extendedATokenEnd),
        originalTUByteEnd(originalTUByteEnd),
        extendedTUByteEnd(extendedTUByteEnd), bTokenBegin(bTokenBegin),
        bTokenEnd(bTokenEnd), bTokenSuffixClosed(bTokenSuffixClosed) {}
};

/// Concrete direct-TU hunk edit plan before final TextEdit assembly.
///
/// This records direct TU edit planning facts without taking over final edit
/// application or ordering.  RefoldTUEditPlanner populates the record; the
/// assembler boundary converts it into the final TextEdit carrier and attaches
/// accepted-result metadata.
struct DirectTUHunkEditPlan {
  /// A/B token-level hunk that produced this direct TU edit.
  diffutils::Hunk hunk;
  /// Stable index of `hunk` in the A/B token-hunk vector.
  uint64_t hunkIndex = 0;
  /// Proven half-open TU source-byte span for the edit.
  TUByteSpanPlan span;
  /// Optional line-control resync result for the replacement payload.
  std::optional<ResyncOutcome> resync;
  /// Replacement bytes accepted for this direct TU edit.
  std::string acceptedPayload;
  /// Inclusive raw TU source-byte start before final widening/spacing repair.
  uint64_t rawTUStart = 0;
  /// Exclusive raw TU source-byte end before final widening/spacing repair.
  uint64_t rawTUEnd = 0;
  /// Inclusive byte offset in edited preprocessed stream B for sidecar mapping.
  std::optional<uint64_t> materializedBByteBegin;
  /// Exclusive byte offset in edited preprocessed stream B for sidecar mapping.
  std::optional<uint64_t> materializedBByteEnd;
  /// Accepted path kind used to build the emitted proof carrier.
  AcceptedPathKind acceptedPath = AcceptedPathKind::Unknown;

  DirectTUHunkEditPlan() = default;

  DirectTUHunkEditPlan(diffutils::Hunk hunk, uint64_t hunkIndex,
                       TUByteSpanPlan span, std::optional<ResyncOutcome> resync,
                       std::string acceptedPayload, uint64_t rawTUStart,
                       uint64_t rawTUEnd,
                       std::optional<uint64_t> materializedBByteBegin,
                       std::optional<uint64_t> materializedBByteEnd,
                       AcceptedPathKind acceptedPath)
      : hunk(std::move(hunk)), hunkIndex(hunkIndex), span(std::move(span)),
        resync(std::move(resync)), acceptedPayload(std::move(acceptedPayload)),
        rawTUStart(rawTUStart), rawTUEnd(rawTUEnd),
        materializedBByteBegin(materializedBByteBegin),
        materializedBByteEnd(materializedBByteEnd), acceptedPath(acceptedPath) {
  }
};

/// Translation-unit edit planning service.
///
/// This class owns TU-side edit planning only: proving anchors, computing TU
/// byte spans, resolving pure-insertion include-boundary ownership,
/// constructing direct-TU hunk edit plans, and deciding whether a direct TU
/// replacement may absorb a closed trailing call suffix.  Final TextEdit
/// ordering/application and generic accepted-result carrier certifying remain
/// outside this service.
class RefoldTUEditPlanner {
public:
  /// Borrowed services and shared token caches for TU edit planning.
  struct Deps {
    /// Producer model containing TU/source ownership facts.
    const RefoldModel &model;
    /// Path identity service for TU/include boundary comparisons.
    const RefoldPathIdentity &pathIdentity;
    /// Macro topology service used for macro-call suffix checks.
    const RefoldMacroTopology &macroTopology;
    /// TU-anchor proof builder used when pure insertions are proven.
    const RefoldTUAnchorProof &tuAnchorProof;
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
    /// A-to-B token map from the token diff planner.
    const std::vector<int64_t> &abTokenMapA2B;
    /// A-side PP-gap owner-depth profile from the token diff planner.
    const std::vector<uint32_t> &ownerDepthGap;
    /// Strict-mode flag controlling fail-closed diagnostics.
    bool strict = true;
  };

  explicit RefoldTUEditPlanner(Deps deps);

  const RefoldModel &model() const { return deps_.model; }

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
  /// This deliberately exposes only the low-level slot lookup that the planner
  /// already consumes. It does not classify ownership, widen spans, or build
  /// text edits; callers that need the full insertion-anchor proof should use
  /// FindProvableTUInsertionAnchor() instead.
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

  /// Context-shaped overload for future call sites that already carry a hunk
  /// planning context.  The context hunk must be a pure insertion.
  std::optional<TUInsertionAnchor>
  FindProvableTUInsertionAnchor(const TUEditPlanningContext &ctx) const;

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
  /// For pure insertions, the planner first tries an exact producer-recorded TU
  /// slot boundary at the same PP gap. If no exact slot exists, it defers to
  /// the same provable TU insertion-anchor logic used by owner classification.
  /// A returned span with Begin == End denotes a concrete insertion anchor
  /// point in the TU.
  std::optional<TUByteSpanPlan> PlanTUByteSpan(uint64_t a0, uint64_t a1,
                                               llvm::StringRef tuPath) const;

  /// Context-shaped overload using the hunk stored in the planning context.
  std::optional<TUByteSpanPlan>
  PlanTUByteSpan(const TUEditPlanningContext &ctx) const;

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

  /// Validate a provisional direct-TU source envelope.
  ///
  /// Complete producer-bound `#define`/`#undef` intervals may be recognized
  /// only as evidence that the specialized macro-state repair planner has an
  /// exact transition to inspect.  This theorem creates no protected-source
  /// authority, and the final edit remains inadmissible until that specialized
  /// planner has discharged every consumed transition.
  bool ValidateDirectTUEnvelope(llvm::StringRef tuPath, uint64_t begin,
                                uint64_t end) const;

  /// \brief Determine whether an insertion hunk lands exactly on an include PP
  /// boundary and, if so, return the include level that should own the boundary
  /// insertion.
  ///
  /// This helper is used to conservatively assign ownership for edits that
  /// occur at an insertion point in the A-side PP token stream. The intent is
  /// to detect insertions that are *exactly* between sibling include regions
  /// and to attribute the insertion to their parent include, rather than
  /// incorrectly placing it inside one of the adjacent children.
  ///
  /// This policy is only applicable when the hunk has an empty A-span. To avoid
  /// heuristic ownership mistakes, it does not probe for “nearest” tokmap
  /// entries and does not snap to nearby PP tokens. Instead, it only considers
  /// includes whose A-domain PP cover interval touches the insertion position
  /// exactly.
  std::optional<BoundaryParentIncludePlan>
  FindBoundaryParentIncludeForPureInsertion(const diffutils::Hunk &h) const;

  /// Context-shaped overload using the hunk stored in the planning context.
  std::optional<BoundaryParentIncludePlan>
  FindBoundaryParentIncludeForPureInsertion(
      const TUEditPlanningContext &ctx) const;

  /// Build the direct-TU hunk edit plan for an already-proved TU byte span.
  ///
  /// This method deliberately returns a plan, not an applied or globally
  /// ordered edit.  It re-proves every nonempty raw A-token carrier and the
  /// actual final source envelope; failure returns `std::nullopt`.  The caller
  /// remains responsible for converting an accepted plan into the final
  /// TextEdit carrier so proof-certifying and final edit assembly continue to
  /// flow through the existing assembler/audit boundary.
  std::optional<DirectTUHunkEditPlan> BuildDirectTUHunkEditPlan(
      const diffutils::Hunk &h, uint64_t hunkIndex,
      const std::pair<uint64_t, uint64_t> &span, ResyncOutcome resync,
      llvm::StringRef acceptedPayload, uint64_t rawTUStart, uint64_t rawTUEnd,
      std::optional<uint64_t> materializedBByteBegin,
      std::optional<uint64_t> materializedBByteEnd,
      AcceptedPathKind acceptedPath,
      std::optional<TUInsertionAnchorAdjustment> insertionAnchorAdjustment =
          std::nullopt) const;

  /// Context-shaped overload for call sites that already carry the A/B token
  /// hunk and TU source-byte span planning records together.
  std::optional<DirectTUHunkEditPlan>
  BuildDirectTUHunkEditPlan(const TUEditPlanningContext &ctx,
                            const TUByteSpanPlan &span, ResyncOutcome resync,
                            llvm::StringRef acceptedPayload,
                            uint64_t rawTUStart, uint64_t rawTUEnd,
                            std::optional<uint64_t> materializedBByteBegin,
                            std::optional<uint64_t> materializedBByteEnd,
                            AcceptedPathKind acceptedPath,
                            std::optional<TUInsertionAnchorAdjustment>
                                insertionAnchorAdjustment = std::nullopt) const;

  /// Check whether the TU byte suffix [oldEnd, extEnd) is closed over the
  /// corresponding B-token interval.  This is a token-closure proof for a
  /// trailing call-suffix extension, not a general byte-span widening rule.
  bool TUReplacementExtensionIsBTokenClosed(uint64_t aTokStart, uint64_t oldEnd,
                                            uint64_t extEnd, uint64_t bStart,
                                            uint64_t bEnd,
                                            llvm::StringRef tuPath) const;

  /// Context-shaped closure check for callers that already have a suffix
  /// extension carrier.
  bool TUReplacementExtensionIsBTokenClosed(
      const TUEditPlanningContext &ctx,
      const TUTrailingCallSuffixExtension &extension) const;

  /// Return the closed trailing call-suffix extension, if the existing TU span
  /// may be widened.  The returned carrier records the old and extended TU byte
  /// endpoints; callers decide whether to mutate their local span.
  std::optional<TUTrailingCallSuffixExtension>
  MaybeExtendTUSpanOverClosedTrailingCallSuffix(
      const diffutils::Hunk &h, llvm::StringRef tuPath, llvm::StringRef tuBytes,
      llvm::StringRef replacement, const TUByteSpanPlan &initialSpan) const;

  /// Context-shaped overload for call sites that carry the A/B token hunk and
  /// TU source-byte span together.  The replacement text remains explicit
  /// because suffix discovery depends on the exact replacement surface.
  std::optional<TUTrailingCallSuffixExtension>
  MaybeExtendTUSpanOverClosedTrailingCallSuffix(
      const TUEditPlanningContext &ctx, const TUByteSpanPlan &initialSpan,
      llvm::StringRef replacement) const;

  /// TU source-byte-span mutating overload for engine orchestration.  It
  /// delegates to the carrier-returning planner method and only updates
  /// span.second when the same closed-suffix proof succeeds.
  void MaybeExtendTUSpanOverClosedTrailingCallSuffix(
      const diffutils::Hunk &h, llvm::StringRef tuPath, llvm::StringRef tuBytes,
      llvm::StringRef replacement, std::pair<uint64_t, uint64_t> &span) const;

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

  /// Validate a direct-TU envelope that may contain only ordinary source.
  ///
  /// This stricter form is used by independent widening theorems that have no
  /// pre-existing macro transition to defer to specialized repair.
  bool ValidateOrdinaryDirectTUEnvelope(llvm::StringRef tuPath,
                                        uint64_t begin, uint64_t end) const;

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

  Deps deps_;
};

/// Advance a zero-width TU insertion past a producer-backed source line-control
/// prefix when the planner has already proved an exact slot-boundary anchor.
///
/// The function mutates \p span in place and returns true when the anchor was
/// advanced.  Insertions at a selected conditional-arm exit are excluded; their
/// anchor is owned by the cond-group join, not by this source-prefix slide.
bool maybeAdvanceTUInsertionPastSourceLineControlPrefix(
    const RefoldTUEditPlanner &planner,
    const RefoldLineControlProof &lineControlProof, const diffutils::Hunk &h,
    llvm::StringRef tuPath, llvm::StringRef tuBytes,
    std::pair<uint64_t, uint64_t> &span);

} // namespace refold
} // namespace clang

#endif // LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDTUEDITPLANNER_H
