//===--- RefoldTUEditPlanner.h ---------------------------------*- C++ -*-===//
//
// Translation-unit edit planning service for clang-refold.
//
// This service owns pure-insertion include-boundary ownership, direct TU hunk
// edit plans, and the closed trailing call-suffix extension policy.  TU anchor
// and byte-span proofs are RefoldTUAnchorProof's; final TextEdit assembly
// remains outside this service.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDTUEDITPLANNER_H
#define LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDTUEDITPLANNER_H

#include "edit/RefoldEditTypes.h"
#include "edit/RefoldTUAnchorProof.h"
#include "model/RefoldToken.h"
#include "proof/RefoldOwnerStateTypes.h"
#include "proof/RefoldTheoremTypes.h"
#include "source/RefoldDiffTypes.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace clang {
namespace refold {

class RefoldLineControlProof;
class RefoldMacroTopology;
class RefoldModel;
struct PrintedPragmaCarrier;
struct SidebandPragmaLinePairing;

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
/// This class owns TU-side edit planning only: resolving pure-insertion
/// include-boundary ownership, constructing direct-TU hunk edit plans, and
/// deciding whether a direct TU replacement may absorb a closed trailing call
/// suffix.  Anchors and TU byte spans come from RefoldTUAnchorProof.  Final
/// TextEdit ordering/application and generic accepted-result carrier
/// certifying remain outside this service.
class RefoldTUEditPlanner {
public:
  /// Borrowed services and shared token caches for TU edit planning.
  struct Deps {
    /// Producer model containing TU/source ownership facts.
    const RefoldModel &model;
    /// Macro topology service used for macro-call suffix checks.
    const RefoldMacroTopology &macroTopology;
    /// TU anchor and byte-span proofs the plans are built from.
    const RefoldTUAnchorProof &tuAnchorProof;
    /// A-token stream for suffix checks.
    llvm::ArrayRef<PPTok> aTokens;
    /// A-to-B token map from the token diff planner.
    const std::vector<int64_t> &abTokenMapA2B;
  };

  explicit RefoldTUEditPlanner(Deps deps);

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

  /// Check whether the TU byte suffix [oldEnd, extEnd) is closed over the
  /// corresponding B-token interval.  This is a token-closure proof for a
  /// trailing call-suffix extension, not a general byte-span widening rule.
  bool TUReplacementExtensionIsBTokenClosed(uint64_t aTokStart, uint64_t oldEnd,
                                            uint64_t extEnd, uint64_t bStart,
                                            uint64_t bEnd,
                                            llvm::StringRef tuPath) const;

  /// Return the closed trailing call-suffix extension, if the existing TU span
  /// may be widened.  The returned carrier records the old and extended TU byte
  /// endpoints; callers decide whether to mutate their local span.
  std::optional<TUTrailingCallSuffixExtension>
  MaybeExtendTUSpanOverClosedTrailingCallSuffix(
      const diffutils::Hunk &h, llvm::StringRef tuPath, llvm::StringRef tuBytes,
      llvm::StringRef replacement, const TUByteSpanPlan &initialSpan) const;

  /// TU source-byte-span mutating overload for engine orchestration.  It
  /// delegates to the carrier-returning planner method and only updates
  /// span.second when the same closed-suffix proof succeeds.
  void MaybeExtendTUSpanOverClosedTrailingCallSuffix(
      const diffutils::Hunk &h, llvm::StringRef tuPath, llvm::StringRef tuBytes,
      llvm::StringRef replacement, std::pair<uint64_t, uint64_t> &span) const;

private:
  Deps deps_;
};

/// Advance a zero-width TU insertion past a producer-backed source line-control
/// prefix when \p tuAnchorProof has already proved an exact slot-boundary
/// anchor.
///
/// The function mutates \p span in place and returns true when the anchor was
/// advanced.  Insertions at a selected conditional-arm exit are excluded; their
/// anchor is owned by the cond-group join, not by this source-prefix slide.
bool maybeAdvanceTUInsertionPastSourceLineControlPrefix(
    const RefoldTUAnchorProof &tuAnchorProof,
    const RefoldLineControlProof &lineControlProof, const diffutils::Hunk &h,
    llvm::StringRef tuPath, llvm::StringRef tuBytes,
    std::pair<uint64_t, uint64_t> &span);

/// Where a pure TU insertion lands among the `#pragma` lines that clang
/// printed at its A gap and that survived unedited into B.
struct PrintedPragmaInsertionPlacement {
  enum class Kind : uint8_t {
    /// No surviving printed line at the gap is printed after the payload in
    /// B, so the ordinary anchor and token envelope already realize B.
    NotApplicable,
    /// The insertion must land at `tuByteOffset`, replaying only the B bytes
    /// ending at `bByteEnd` when that is set.
    Placed,
    /// A surviving printed line constrains the placement, but no exact source
    /// site could be proved.  Any other placement emits that line on the
    /// wrong side of the payload, so the direct TU edit must not be built.
    Refused,
  };
  Kind kind = Kind::NotApplicable;
  uint64_t tuByteOffset = 0;
  /// End of the replayed B bytes, set when a line B prints after the payload
  /// would otherwise be replayed with it; unset keeps the token envelope.
  std::optional<uint64_t> bByteEnd;
};

/// Place a pure TU insertion relative to the printed `#pragma` lines preserved
/// at its A gap.
///
/// A paired line keeps its source directive, so it is printed wherever that
/// source line stands; its B gap says which side of the payload B printed it
/// on.  Lines B printed at `h.bStart` precede the payload, lines at `h.bEnd`
/// follow it.  The insertion therefore lands between the end of the last
/// preceding line and the start of the first following one -- at the base
/// anchor when that already lies there -- and replays B only up to the first
/// following line's B copy, which the preserved directive already prints.
///
/// The placement is proved only when every line at the gap binds to exactly
/// one relocatable carrier (see `PrintedPragmaCarrier::relocatable`), all
/// preceding lines come before all following ones in source and in B, no line
/// sits strictly inside the payload, and the source between the base anchor
/// and the placement holds nothing but such carriers and lexer trivia.  None
/// of those carriers changes preprocessor state, so moving the insertion
/// across them changes only the order in which their lines and the payload
/// are printed -- the order B fixes.
PrintedPragmaInsertionPlacement placeTUInsertionAmongPrintedPragmas(
    const RefoldTUAnchorProof &tuAnchorProof,
    llvm::ArrayRef<PrintedPragmaCarrier> carriers,
    llvm::ArrayRef<SidebandPragmaLinePairing> pairings,
    const diffutils::Hunk &h, llvm::ArrayRef<size_t> bTokOff,
    llvm::StringRef tuBytes, uint64_t baseAnchor);

} // namespace refold
} // namespace clang

#endif // LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDTUEDITPLANNER_H
