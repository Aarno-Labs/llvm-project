//===--- RefoldTUEditPlanner.h ---------------------------------*- C++ -*-===//
//
// Translation-unit edit planning service for clang-refold.
//
// This service owns TU insertion-anchor proof queries, TU byte-span planning,
// pure-insertion include-boundary ownership, direct TU hunk edit plans, and the
// closed trailing call-suffix extension policy.  Final TextEdit assembly remains
// outside this service.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDTUEDITPLANNER_H
#define LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDTUEDITPLANNER_H

#include "edit/RefoldEditTypes.h"
#include "proof/RefoldAcceptedResultTypes.h"
#include "source/DiffAlgorithms.h"
#include "source/RefoldToken.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace clang {
namespace refold {

class LineDirectiveInserter;
class RefoldMacroTopology;
class RefoldModel;
class RefoldPathIdentity;
class RefoldTUAnchorProof;

/// Shared immutable inputs for TU edit-planning routines.
///
/// Run-wide dependencies such as the model, path identity, TU-anchor proof
/// builder remain constructor dependencies of RefoldTUEditPlanner.  This
/// context is for per-hunk/per-file inputs that vary between calls and keeps
/// planner entry points from growing broad ad-hoc parameter lists.
struct TUEditPlanningContext {
  llvm::StringRef TUPath;
  llvm::StringRef TUBytes;
  const diffutils::Hunk *Hunk = nullptr;
  uint64_t HunkIndex = 0;
  llvm::ArrayRef<PPTok> ATokens;
  llvm::ArrayRef<PPTok> BTokens;

  TUEditPlanningContext() = default;

  TUEditPlanningContext(llvm::StringRef tuPath, llvm::StringRef tuBytes,
                        const diffutils::Hunk &hunk, uint64_t hunkIndex,
                        llvm::ArrayRef<PPTok> aTokens,
                        llvm::ArrayRef<PPTok> bTokens)
      : TUPath(tuPath), TUBytes(tuBytes), Hunk(&hunk), HunkIndex(hunkIndex),
        ATokens(aTokens), BTokens(bTokens) {}
};

/// Result of proving that an A-side PP gap has a concrete TU byte anchor.
///
/// The optional witness mirrors the existing proof/audit side channel.  Callers
/// that only need the concrete byte offset can use TUByteOffset; callers that
/// forward accepted-result metadata can also preserve the witness unchanged.
struct TUInsertionAnchor {
  uint64_t PPGap = 0;
  uint64_t TUByteOffset = 0;
  std::optional<TUAnchorWitness> Witness;

  TUInsertionAnchor() = default;

  TUInsertionAnchor(uint64_t ppGap, uint64_t tuByteOffset,
                    std::optional<TUAnchorWitness> witness = std::nullopt)
      : PPGap(ppGap), TUByteOffset(tuByteOffset),
        Witness(std::move(witness)) {}
};

/// Planned TU byte span for a token hunk.
///
/// The span is half-open in TU byte coordinates.  A pure insertion is represented
/// by Begin == End and may carry the exact insertion anchor that justified the
/// zero-width span.
struct TUByteSpanPlan {
  uint64_t ATokenBegin = 0;
  uint64_t ATokenEnd = 0;
  uint64_t TUByteBegin = 0;
  uint64_t TUByteEnd = 0;
  std::optional<TUInsertionAnchor> InsertionAnchor;

  TUByteSpanPlan() = default;

  TUByteSpanPlan(uint64_t aTokenBegin, uint64_t aTokenEnd,
                 uint64_t tuByteBegin, uint64_t tuByteEnd,
                 std::optional<TUInsertionAnchor> insertionAnchor =
                     std::nullopt)
      : ATokenBegin(aTokenBegin), ATokenEnd(aTokenEnd),
        TUByteBegin(tuByteBegin), TUByteEnd(tuByteEnd),
        InsertionAnchor(std::move(insertionAnchor)) {}

  bool isPureInsertion() const { return ATokenBegin == ATokenEnd; }
  std::pair<uint64_t, uint64_t> byteRange() const {
    return {TUByteBegin, TUByteEnd};
  }
};

/// Include-boundary owner selected for a pure insertion before TU edit planning.
///
/// The current engine helper returns an IncludeItem pointer.  The service-level
/// carrier records stable ids instead so future call sites do not have to expose
/// raw model storage when they only need the resolved boundary ownership.
struct BoundaryParentIncludePlan {
  uint64_t PPGap = 0;
  uint64_t IncludeId = 0;
  std::optional<uint64_t> ParentIncludeId;

  BoundaryParentIncludePlan() = default;

  BoundaryParentIncludePlan(uint64_t ppGap, uint64_t includeId,
                            std::optional<uint64_t> parentIncludeId =
                                std::nullopt)
      : PPGap(ppGap), IncludeId(includeId),
        ParentIncludeId(parentIncludeId) {}
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
  uint64_t OriginalATokenEnd = 0;
  uint64_t ExtendedATokenEnd = 0;
  uint64_t OriginalTUByteEnd = 0;
  uint64_t ExtendedTUByteEnd = 0;
  uint64_t BTokenBegin = 0;
  uint64_t BTokenEnd = 0;
  bool BTokenSuffixClosed = false;

  TUTrailingCallSuffixExtension() = default;

  TUTrailingCallSuffixExtension(uint64_t originalATokenEnd,
                                uint64_t extendedATokenEnd,
                                uint64_t originalTUByteEnd,
                                uint64_t extendedTUByteEnd,
                                uint64_t bTokenBegin, uint64_t bTokenEnd,
                                bool bTokenSuffixClosed)
      : OriginalATokenEnd(originalATokenEnd),
        ExtendedATokenEnd(extendedATokenEnd),
        OriginalTUByteEnd(originalTUByteEnd),
        ExtendedTUByteEnd(extendedTUByteEnd), BTokenBegin(bTokenBegin),
        BTokenEnd(bTokenEnd), BTokenSuffixClosed(bTokenSuffixClosed) {}
};

/// Concrete direct-TU hunk edit plan before final TextEdit assembly.
///
/// This mirrors the existing BuildDirectTUHunkTextEdit() state bundle without
/// taking over final edit application or ordering.  RefoldTUEditPlanner populates
/// this record; the compatibility wrapper/assembler boundary converts it into
/// the final TextEdit carrier and attaches accepted-result metadata.
struct DirectTUHunkEditPlan {
  diffutils::Hunk Hunk;
  uint64_t HunkIndex = 0;
  TUByteSpanPlan Span;
  std::optional<ResyncOutcome> Resync;
  std::string AcceptedPayload;
  uint64_t RawTUStart = 0;
  uint64_t RawTUEnd = 0;
  std::optional<uint64_t> MaterializedBByteBegin;
  std::optional<uint64_t> MaterializedBByteEnd;
  AcceptedPathKind AcceptedPath = AcceptedPathKind::Unknown;

  DirectTUHunkEditPlan() = default;

  DirectTUHunkEditPlan(diffutils::Hunk hunk, uint64_t hunkIndex,
                       TUByteSpanPlan span,
                       std::optional<ResyncOutcome> resync,
                       std::string acceptedPayload, uint64_t rawTUStart,
                       uint64_t rawTUEnd,
                       std::optional<uint64_t> materializedBByteBegin,
                       std::optional<uint64_t> materializedBByteEnd,
                       AcceptedPathKind acceptedPath)
      : Hunk(std::move(hunk)), HunkIndex(hunkIndex), Span(std::move(span)),
        Resync(std::move(resync)), AcceptedPayload(std::move(acceptedPayload)),
        RawTUStart(rawTUStart), RawTUEnd(rawTUEnd),
        MaterializedBByteBegin(materializedBByteBegin),
        MaterializedBByteEnd(materializedBByteEnd),
        AcceptedPath(acceptedPath) {}
};

/// Translation-unit edit planning service.
///
/// This class owns TU-side edit planning only: proving anchors, computing TU byte
/// spans, resolving pure-insertion include-boundary ownership, constructing
/// direct-TU hunk edit plans, and deciding whether a direct TU replacement may
/// absorb a closed trailing call suffix.  Final TextEdit ordering/application
/// and generic accepted-result carrier stamping remain outside this service.
class RefoldTUEditPlanner {
public:
  struct Deps {
    const RefoldModel &Model;
    const RefoldPathIdentity &PathIdentity;
    const RefoldMacroTopology &MacroTopology;
    const RefoldTUAnchorProof &TUAnchorProof;
    const LineDirectiveInserter &LineDirs;
    llvm::ArrayRef<PPTok> ATokens;
    const std::vector<int64_t> &ABTokenMapA2B;
    const std::vector<uint32_t> &OwnerDepthGap;
    bool Strict = true;

  };

  explicit RefoldTUEditPlanner(Deps deps);

  const RefoldModel &model() const { return D_.Model; }

  /// Anchors a pure insertion PP gap to an exact producer-recorded TU slot.
  ///
  /// The lookup performs no nearest-boundary snapping. It succeeds only when
  /// the producer supplied a boundary-like TU slot whose PP coordinate exactly
  /// equals \p ppGap. Directive-line newline adjustment remains local to this
  /// planner because TU insertion anchoring is line-boundary sensitive.
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
  std::optional<uint64_t>
  FindExactSlotBoundaryFromPPGap(llvm::StringRef tuPath,
                                uint64_t ppGap) const;

  /// Return true iff a PP gap sits at the exit of a selected conditional arm.
  bool IsPPGapAtSelectedConditionalArmExit(uint64_t ppGap) const;

  /// Prove a zero-width TU insertion anchor at an exact A-side PP gap.
  ///
  /// The returned carrier records both the concrete TU byte and the witness that
  /// justified it.  The optional out-parameters let existing orchestration
  /// forward the exact witness/candidate without rebuilding proof metadata.
  std::optional<TUInsertionAnchor> FindProvableTUInsertionAnchor(
      uint64_t pp, llvm::StringRef tuPath,
      TUAnchorWitness *witness = nullptr,
      AcceptedResultCandidate *acceptedCandidate = nullptr) const;

  /// Context-shaped overload for future call sites that already carry a hunk
  /// planning context.  The context hunk must be a pure insertion.
  std::optional<TUInsertionAnchor>
  FindProvableTUInsertionAnchor(const TUEditPlanningContext &ctx) const;

  /// Compute the conservative TU byte span for an A-side PP-token interval.
  std::optional<TUByteSpanPlan> PlanTUByteSpan(
      uint64_t a0, uint64_t a1, llvm::StringRef tuPath) const;

  /// Context-shaped overload using the hunk stored in the planning context.
  std::optional<TUByteSpanPlan>
  PlanTUByteSpan(const TUEditPlanningContext &ctx) const;

  /// Resolve an include-boundary owner for a pure insertion.
  std::optional<BoundaryParentIncludePlan>
  FindBoundaryParentIncludeForPureInsertion(const diffutils::Hunk &h) const;

  /// Context-shaped overload using the hunk stored in the planning context.
  std::optional<BoundaryParentIncludePlan>
  FindBoundaryParentIncludeForPureInsertion(
      const TUEditPlanningContext &ctx) const;

  /// Build the direct-TU hunk edit plan for an already-proved TU byte span.
  ///
  /// This method deliberately returns a plan, not an applied or globally ordered
  /// edit.  The caller remains responsible for converting the plan into the
  /// final TextEdit carrier so proof-stamping and final edit assembly continue to
  /// flow through the existing assembler/audit boundary.
  DirectTUHunkEditPlan BuildDirectTUHunkEditPlan(
      const diffutils::Hunk &h, uint64_t hunkIndex,
      const std::pair<uint64_t, uint64_t> &span, ResyncOutcome resync,
      llvm::StringRef acceptedPayload, uint64_t rawTUStart, uint64_t rawTUEnd,
      std::optional<uint64_t> materializedBByteBegin,
      std::optional<uint64_t> materializedBByteEnd,
      AcceptedPathKind acceptedPath) const;

  /// Context-shaped overload for future call sites that already carry the hunk
  /// and byte-span planning records together.
  std::optional<DirectTUHunkEditPlan> BuildDirectTUHunkEditPlan(
      const TUEditPlanningContext &ctx, const TUByteSpanPlan &span,
      ResyncOutcome resync, llvm::StringRef acceptedPayload,
      uint64_t rawTUStart, uint64_t rawTUEnd,
      std::optional<uint64_t> materializedBByteBegin,
      std::optional<uint64_t> materializedBByteEnd,
      AcceptedPathKind acceptedPath) const;

  /// Check whether the TU byte suffix [oldEnd, extEnd) is closed over the
  /// corresponding B-token interval.  This is a token-closure proof for a
  /// trailing call-suffix extension, not a general byte-span widening rule.
  bool TUReplacementExtensionIsBTokenClosed(
      uint64_t aTokStart, uint64_t oldEnd, uint64_t extEnd,
      uint64_t bStart, uint64_t bEnd, llvm::StringRef tuPath) const;

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
      const diffutils::Hunk &h, llvm::StringRef tuPath,
      llvm::StringRef tuBytes, llvm::StringRef replacement,
      const TUByteSpanPlan &initialSpan) const;

  /// Context-shaped overload for future call sites that carry the hunk and
  /// span together.  The replacement text remains explicit because suffix
  /// discovery depends on the exact replacement surface.
  std::optional<TUTrailingCallSuffixExtension>
  MaybeExtendTUSpanOverClosedTrailingCallSuffix(
      const TUEditPlanningContext &ctx, const TUByteSpanPlan &initialSpan,
      llvm::StringRef replacement) const;

  /// Span-mutating overload for existing engine orchestration.  It delegates to
  /// the carrier-returning planner method and only updates span.second when the
  /// same closed-suffix proof succeeds.
  void MaybeExtendTUSpanOverClosedTrailingCallSuffix(
      const diffutils::Hunk &h, llvm::StringRef tuPath,
      llvm::StringRef tuBytes, llvm::StringRef replacement,
      std::pair<uint64_t, uint64_t> &span) const;

private:
  /// Return the narrowest include id covering a PP index, if any.
  ///
  /// This is a TU-anchor guard, not general owner classification: it prevents a
  /// zero-width TU insertion from being proved through a PP gap that lies inside
  /// an include expansion.
  std::optional<uint64_t> IncludeIdCoveringPPIndex(uint64_t pp) const;

  Deps D_;
};

} // namespace refold
} // namespace clang

#endif // LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDTUEDITPLANNER_H
