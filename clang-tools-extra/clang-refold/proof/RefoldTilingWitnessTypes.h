//===--- RefoldTilingWitnessTypes.h -----------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// Structural hunk tiling witnesses for clang-refold.
//
// The durable partition records: when one hunk must be split because its
// owners are independently realizable, or because a protected preprocessing
// interval lies between adjacent token segments, this is the complete ordered
// witness that says so.  Proof-only gap edges stay durable even though they
// emit no tokens, so every emitted carrier resolves back to exactly one edge.
//
// The `MixedOwnerTiling*` aliases at the end preserve the historical names for
// the mixed-realizer case, which is now one reason among several.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDTILINGWITNESSTYPES_H
#define LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDTILINGWITNESSTYPES_H

#include "proof/RefoldOwnerStateTypes.h"

#include "llvm/ADT/StringRef.h"
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace clang {
namespace refold {

using llvm::StringRef;

/// Semantic reason that made a structural hunk partition necessary.
///
/// `MixedRealizers` preserves the historical theorem: independently realizable
/// owner domains require separate emitted hunks.  `PreservedPreprocessingStructure`
/// records the new same-realizer case where a protected preprocessing interval
/// lies strictly between adjacent token segments and would be consumed by one
/// merged byte edit.  The combined value is used when both obligations are
/// present in the same unique partition.  `Unknown` is never admissible in a
/// persisted witness.
#define REFOLD_STRUCTURAL_TILING_REASON_LIST(REFOLD_X)                           \
  REFOLD_X(Unknown)                                                             \
  REFOLD_X(MixedRealizers)                                                      \
  REFOLD_X(PreservedPreprocessingStructure)                                     \
  REFOLD_X(MixedRealizersAndPreservedStructure)

enum class StructuralTilingReason : uint8_t {
#define REFOLD_X(name) name,
  REFOLD_STRUCTURAL_TILING_REASON_LIST(REFOLD_X)
#undef REFOLD_X
};

inline StringRef toString(StructuralTilingReason value) {
  switch (value) {
#define REFOLD_X(name)                                                          \
  case StructuralTilingReason::name:                                            \
    return #name;
    REFOLD_STRUCTURAL_TILING_REASON_LIST(REFOLD_X)
#undef REFOLD_X
  }
  return "Unknown";
}
#undef REFOLD_STRUCTURAL_TILING_REASON_LIST

/// Edge kind recorded in a persisted structural hunk-tiling witness.
///
/// Token segments are the non-empty A/B envelopes emitted as ordinary
/// normalized hunks. State gaps are zero-token proof edges that retain source
/// structure between emitted segments without producing replacement bytes.
#define REFOLD_STRUCTURAL_HUNK_TILING_EDGE_KIND_LIST(REFOLD_X)                 \
  REFOLD_X(Unknown)                                                            \
  REFOLD_X(TokenSegment)                                                       \
  REFOLD_X(StateGap)

enum class StructuralHunkTilingEdgeKind : uint8_t {
#define REFOLD_X(name) name,
  REFOLD_STRUCTURAL_HUNK_TILING_EDGE_KIND_LIST(REFOLD_X)
#undef REFOLD_X
};

inline StringRef toString(StructuralHunkTilingEdgeKind value) {
  switch (value) {
#define REFOLD_X(name)                                                         \
  case StructuralHunkTilingEdgeKind::name:                                     \
    return #name;
    REFOLD_STRUCTURAL_HUNK_TILING_EDGE_KIND_LIST(REFOLD_X)
#undef REFOLD_X
  }
  return "Unknown";
}
#undef REFOLD_STRUCTURAL_HUNK_TILING_EDGE_KIND_LIST

/// Physical treatment of one proof-only structural source gap.
///
/// A state-gap owner and a source-gap disposition answer different questions:
/// the owner identifies the producer state transition, while the disposition
/// proves what the emitted edit set does with the corresponding source bytes.
/// `PreservedInPlace` is intentionally stronger than merely omitting a token
/// hunk: the complete source interval remains outside every emitted edit and
/// therefore executes in its original physical order. The remaining values
/// reserve explicit authority for later owner-materialization and state-repair
/// theorems; `Unknown` never authorizes a structure-preserving split.
#define REFOLD_STRUCTURAL_GAP_DISPOSITION_LIST(REFOLD_X)                       \
  REFOLD_X(Unknown)                                                            \
  REFOLD_X(PreservedInPlace)                                                   \
  REFOLD_X(MaterializedByOwner)                                                \
  REFOLD_X(RepairedByStatePlanner)

enum class StructuralGapDisposition : uint8_t {
#define REFOLD_X(name) name,
  REFOLD_STRUCTURAL_GAP_DISPOSITION_LIST(REFOLD_X)
#undef REFOLD_X
};

inline StringRef toString(StructuralGapDisposition value) {
  switch (value) {
#define REFOLD_X(name)                                                         \
  case StructuralGapDisposition::name:                                        \
    return #name;
    REFOLD_STRUCTURAL_GAP_DISPOSITION_LIST(REFOLD_X)
#undef REFOLD_X
  }
  return "Unknown";
}
#undef REFOLD_STRUCTURAL_GAP_DISPOSITION_LIST

/// Producer record class persisted for one protected structural gap.
///
/// Producer ids live in different model arrays and are not globally unique.
/// The kind therefore accompanies every optional id. `None` is a valid exact
/// result for a lexically protected but producer-unbound directive: preserving
/// its bytes requires no authority to reconstruct it.
#define REFOLD_STRUCTURAL_PRODUCER_IDENTITY_KIND_LIST(REFOLD_X)                \
  REFOLD_X(None)                                                               \
  REFOLD_X(MacroDirective)                                                     \
  REFOLD_X(IncludeDirective)                                                   \
  REFOLD_X(PragmaDirective)                                                    \
  REFOLD_X(LineControlEvent)                                                   \
  REFOLD_X(ConditionalDirective)

enum class StructuralProducerIdentityKind : uint8_t {
#define REFOLD_X(name) name,
  REFOLD_STRUCTURAL_PRODUCER_IDENTITY_KIND_LIST(REFOLD_X)
#undef REFOLD_X
};

inline StringRef toString(StructuralProducerIdentityKind value) {
  switch (value) {
#define REFOLD_X(name)                                                         \
  case StructuralProducerIdentityKind::name:                                  \
    return #name;
    REFOLD_STRUCTURAL_PRODUCER_IDENTITY_KIND_LIST(REFOLD_X)
#undef REFOLD_X
  }
  return "None";
}
#undef REFOLD_STRUCTURAL_PRODUCER_IDENTITY_KIND_LIST

/// Lexical preprocessing construct persisted for one protected source gap.
///
/// This enum deliberately mirrors the structure index without depending on a
/// source-layer header from the theorem-facing proof vocabulary. `Unknown` is
/// never valid for an edge that claims protected preprocessing structure.
#define REFOLD_STRUCTURAL_PROTECTED_STRUCTURE_KIND_LIST(REFOLD_X)             \
  REFOLD_X(Unknown)                                                            \
  REFOLD_X(ConditionalIf)                                                      \
  REFOLD_X(ConditionalIfdef)                                                   \
  REFOLD_X(ConditionalIfndef)                                                  \
  REFOLD_X(ConditionalElif)                                                    \
  REFOLD_X(ConditionalElifdef)                                                 \
  REFOLD_X(ConditionalElifndef)                                                \
  REFOLD_X(ConditionalElse)                                                    \
  REFOLD_X(ConditionalEndif)                                                   \
  REFOLD_X(MacroDefine)                                                        \
  REFOLD_X(MacroUndef)                                                         \
  REFOLD_X(Include)                                                            \
  REFOLD_X(IncludeNext)                                                        \
  REFOLD_X(Import)                                                             \
  REFOLD_X(Pragma)                                                             \
  REFOLD_X(PragmaOperator)                                                     \
  REFOLD_X(LineControl)                                                        \
  REFOLD_X(ErrorDirective)                                                     \
  REFOLD_X(WarningDirective)                                                   \
  REFOLD_X(OtherDirective)

enum class StructuralProtectedStructureKind : uint8_t {
#define REFOLD_X(name) name,
  REFOLD_STRUCTURAL_PROTECTED_STRUCTURE_KIND_LIST(REFOLD_X)
#undef REFOLD_X
};

inline StringRef toString(StructuralProtectedStructureKind value) {
  switch (value) {
#define REFOLD_X(name)                                                         \
  case StructuralProtectedStructureKind::name:                                \
    return #name;
    REFOLD_STRUCTURAL_PROTECTED_STRUCTURE_KIND_LIST(REFOLD_X)
#undef REFOLD_X
  }
  return "Unknown";
}
#undef REFOLD_STRUCTURAL_PROTECTED_STRUCTURE_KIND_LIST

/// Durable proof record for one edge in a structural hunk partition.
///
/// The partition is theorem-facing rather than a transient normalizer detail.
/// Every emitted token segment and every zero-token state gap names the parent
/// witness, its stable source-order position, the exact A/B token envelope, the
/// exact physical owner, and the canonical state-transition proof carried by
/// that edge. State-gap entries have zero-width A/B envelopes at the adjoining
/// boundary but still retain the exact source interval that made the complete
/// structural cover composable.
struct StructuralHunkTilingEdgeWitness {
  /// Durable identity of the complete partition containing this edge.
  uint64_t parentTilingWitnessId = 0;

  /// Legacy edge index retained for compatibility with existing consumers.
  uint32_t segmentIndex = 0;

  /// Explicit position in the complete ordered edge chain.
  ///
  /// This intentionally duplicates `segmentIndex` so generalized witness
  /// consumers need not infer source-order semantics from a legacy field name.
  uint32_t sourceOrderPosition = 0;

  /// Whether this edge emits a token carrier or proves an untouched state gap.
  StructuralHunkTilingEdgeKind kind = StructuralHunkTilingEdgeKind::Unknown;

  /// Exact source and target token envelopes assigned to this edge.
  uint64_t aStart = 0;
  uint64_t aEnd = 0;
  uint64_t bStart = 0;
  uint64_t bEnd = 0;

  /// True only for a proof-only state edge with zero-width A and B envelopes.
  bool zeroTokenStateGap = false;

  /// Typed proof of how the source bytes owned by a proof-only gap are treated
  /// by the emitted edit set. Token segments must retain `Unknown`.
  StructuralGapDisposition gapDisposition =
      StructuralGapDisposition::Unknown;

  /// True when this proof-only gap contains at least one exact protected
  /// preprocessing interval from `RefoldPreprocessingStructureIndex`. Merely
  /// having a zero-token owner or a nonempty source gap does not set this bit.
  bool protectedPreprocessingStructure = false;

  /// True when an emitted token segment is permitted to carry `[q,q)` as its
  /// target envelope. Delete-only structural partitions require every token
  /// segment and state gap to share the same exact empty B boundary.  A
  /// replacement segment may be empty only when adjacent uniquely projected
  /// structural boundaries collapse onto that same B boundary.
  bool allowEmptyBEnvelope = false;

  /// True when the edge's owner closure was completely proved before the edge
  /// was admitted to the durable structural partition.
  bool ownerClosureComplete = false;

  /// Structured physical owner identity. Diagnostic signatures below are not
  /// consulted as proof authority.
  bool ownerIdentityKnown = false;
  Owner ownerIdentity;

  /// True when the witness records the exact physical source bytes occupied by
  /// this edge. Structural TU-segment authority requires this typed range; the
  /// textual `sourceSignature` remains diagnostic only.
  bool sourceByteRangeKnown = false;

  /// Exact physical source identity, including the include occurrence when the
  /// same file was materialized more than once.
  ///
  /// The final byte-edit audit uses these structured fields directly and never
  /// parses diagnostic text when checking `PreservedInPlace` gaps.
  std::string sourcePath;
  std::optional<uint64_t> sourceIncludeId;
  uint64_t sourceBegin = 0;
  uint64_t sourceEnd = 0;

  /// True when the lexical preprocessing construct occupying this edge was
  /// recorded as a typed proof fact. Token segments leave this false.
  bool protectedStructureIdentityRecorded = false;

  /// Exact lexical kind of the protected preprocessing construct.
  ///
  /// An edge claiming protected preprocessing structure may not use `Unknown`.
  StructuralProtectedStructureKind protectedStructureKind =
      StructuralProtectedStructureKind::Unknown;

  /// Producer record namespace for the optional identities below.
  ///
  /// `None` is valid for an exact lexically discovered directive that has no
  /// producer record and whose bytes are merely preserved in place.
  StructuralProducerIdentityKind producerIdentityKind =
      StructuralProducerIdentityKind::None;

  /// Exact producer record identities. Producer ids are array-local, so their
  /// meaning is always interpreted together with `producerIdentityKind`.
  std::optional<uint64_t> producerItemId;
  std::optional<uint64_t> producerConditionalGroupId;
  std::optional<uint64_t> producerConditionalArmId;

  /// True when everything committed after this preserved `#define`/`#undef`
  /// was proved to name no identifier.
  ///
  /// A macro-state directive between two token segments is otherwise reserved
  /// for the macro-state liveness planner, which is the only stage that can
  /// order the directive against payload that mentions the name it binds.  This
  /// flag records the one case where that planner has nothing to decide, so the
  /// directive may stay exactly where it is instead of being relocated past the
  /// replacement.
  ///
  /// It is meaningful only on a `StateGap` edge whose protected structure is a
  /// producer-bound `MacroDefine` or `MacroUndef`; set anywhere else it is a
  /// malformed witness and the tiling proof rejects it.
  bool macroStatePlacementInsensitiveProven = false;

  /// Affirmative physical evidence for `PreservedInPlace`.
  ///
  /// The edge emits no replacement bytes, and the final assembler independently
  /// rechecks that no normalized `TextEdit` overlaps or inserts into the interval.
  /// For an end-of-file directive with no terminating newline, insertion exactly
  /// at `sourceEnd` is also interference because it extends the same logical line.
  bool sourceBytesPreservedUnchanged = false;

  /// True only for a token segment whose parent witness established that every
  /// protected preprocessing interval belonging to the original hunk remains
  /// outside this segment's emitted source edit.
  bool protectedStructurePreservedOutsideSegment = false;

  /// Human-readable diagnostics retained for tracing and witness dumps.
  /// Structured owner, source, and producer fields above are authoritative.
  std::string ownerSignature;
  std::string sourceSignature;
  std::string producerPathSignature;
  std::string targetPPTokenSignature;

  /// Canonical transition carried by this ordered edge.
  ///
  /// A `PreservedInPlace` gap normally carries no reconstructed value delta;
  /// its exact source identity and order are the transition authority.
  StateTransitionProof canonicalStateTransition;
};

/// Exact A-to-B projection proof for one interior structural split.
///
/// The lower and upper projections are respectively the minimum and maximum B
/// frontiers reached by every maximum-length local token alignment at this A
/// boundary. Recording both lets downstream theorem consumers verify that the
/// normalizer
/// did not choose one alignment or one side of an ambiguous replacement.  An
/// admitted record always satisfies
/// `lowerBTokenBoundary == upperBTokenBoundary == bTokenBoundary`.
struct StructuralBoundaryProjectionWitness {
  uint64_t aTokenBoundary = 0;
  uint64_t lowerBTokenBoundary = 0;
  uint64_t upperBTokenBoundary = 0;
  uint64_t bTokenBoundary = 0;
  bool uniqueProjection = false;
};

/// Persisted proof for one deterministic structural hunk partition.
///
/// This generalized authority was formerly named `MixedOwnerTilingWitness`.
/// It covers historical mixed-realizer partitions, same-owner partitions that
/// preserve preprocessing structure, and partitions requiring both obligations.
/// Every emitted carrier resolves back to exactly one edge in this complete,
/// ordered witness; proof-only gap edges remain durable even though they emit no
/// token hunk themselves.
struct StructuralHunkTilingWitness {
  /// Stable identity used by emitted segment bindings and the durable ledger.
  uint64_t witnessId = 0;

  /// Exact original hunk envelope before structural partitioning.
  uint64_t originalAStart = 0;
  uint64_t originalAEnd = 0;
  uint64_t originalBStart = 0;
  uint64_t originalBEnd = 0;

  /// Why this partition is theorem-relevant.
  StructuralTilingReason reason = StructuralTilingReason::Unknown;

  /// The graph search or canonical physical-run derivation produced one and
  /// only one admissible partition. Equal-cost alternatives are rejected.
  bool uniquePartition = false;

  /// Every physical source byte belonging to the original hunk is covered by
  /// an emitted segment, a preserved structural gap, or another explicitly
  /// proved owner edge. Token-stream composition alone cannot set this bit.
  bool sourceByteCoverComplete = false;

  /// The ordered token segments compose exactly to the original target B-token
  /// stream without overlap or omission.
  bool targetTokenStreamComposed = false;

  /// The complete ordered edge chain composes its state obligations.
  ///
  /// This includes value-level owner summaries and physical
  /// `PreservedInPlace` transitions whose source order is itself the authority.
  bool stateTransitionsComposed = false;

  /// Every preserved structural gap remains outside the final emitted edit set,
  /// including insertion-aware logical-line boundaries, not merely outside the
  /// initial token-segment carriers.
  bool preservedGapsDisjointFromEdits = false;

  /// Number of emitted token-segment edges in `edges`.
  uint32_t tokenSegmentCount = 0;

  /// Number of proof-only state-gap edges in `edges`.
  uint32_t stateGapCount = 0;

  /// Number of distinct token-segment realizers in the proved path.
  uint32_t distinctRealizerCount = 0;

  /// Number of state-gap edges covering exact protected preprocessing
  /// structure. This is distinct from `stateGapCount`: an arbitrary zero-token
  /// owner cannot justify same-realizer splitting.
  uint32_t protectedStructureGapCount = 0;

  /// Number of state-gap edges whose complete source interval remains physically
  /// untouched between the emitted token segments.
  uint32_t preservedInPlaceGapCount = 0;

  /// Number of canonical maximal physical source runs derived from exact A-token
  /// mappings for the preserved-structure theorem. Historical mixed-realizer
  /// tilings with no preserved preprocessing seam may leave this zero.
  uint32_t physicalSourceRunCount = 0;

  /// Every A token in the structure-preserving hunk had one exact, unique,
  /// ordered physical source mapping, and every token segment remains within the
  /// maximal runs induced by exact protected gaps.
  bool physicalSourceRunsProven = false;

  /// The emitted token segments are exactly the maximal physical source runs,
  /// so no partition with fewer fragments can leave every protected gap outside
  /// the edit set.  A combined mixed-realizer partition may validly leave this
  /// false because owner boundaries require additional segments inside a run;
  /// its minimum-cost uniqueness is carried by `uniquePartition`.
  bool uniqueMinimumFragmentPartition = false;

  /// Every interior source-run boundary of a structure-preserving replacement
  /// has one exact target boundary across all maximum-length local token
  /// alignments:
  /// `projectLower(aSplit) == projectUpper(aSplit) == bSplit`.  This bit is false
  /// for delete-only partitions, whose target theorem is the shared empty B
  /// boundary below.
  bool uniqueBoundaryProjectionProven = false;

  /// Number of exact interior structural boundaries proved by
  /// `boundaryProjections`.  A replacement over N canonical source runs requires
  /// exactly N-1 records.
  uint32_t boundaryProjectionCount = 0;

  /// Ordered lower/upper target-boundary proofs for the canonical source-run
  /// splits.  The order is the same as the source-run order.  Combined
  /// mixed-realizer tilings may contain additional token-segment boundaries
  /// inside a run; those do not consume projection records.
  std::vector<StructuralBoundaryProjectionWitness> boundaryProjections;

  /// Every token and state edge in a delete-only partition carries the same
  /// empty target envelope `[q,q)`.
  bool sharedEmptyBEnvelopeProven = false;

  /// The common `q` used by the proved empty target envelopes.
  uint64_t sharedEmptyBBoundary = 0;

  /// The complete delete-only state-gap chain consists only of exact
  /// `PreservedInPlace` intervals that remain between the same source runs.
  /// No transition in this chain is replayed, reconstructed, or repaired.
  bool preservedStateChainComposed = false;

  /// Retained value-level owner-state composition theorem used by historical
  /// mixed-realizer consumers.
  bool stateSummariesComposed = false;

  /// Adjacent edge owner boundaries compose in the accepted source order.
  bool ownerBoundariesComposed = false;

  /// Every transition between adjacent edges has an explicit composition proof.
  bool compositionEdgesProven = false;

  /// Preserved gap intervals occur in the same physical source order as the
  /// emitted token segments surrounding them.
  bool preservedGapSourceOrderProven = false;

  /// No initial emitted token-segment carrier intersects a
  /// `PreservedInPlace` gap. The stronger final-edit property is recorded in
  /// `preservedGapsDisjointFromEdits`.
  bool preservedGapsDisjointFromTokenSegments = false;

  /// Deterministic signatures retained for diagnostics and equivalence checks.
  std::string globalTargetPPTokenSignature;
  std::string globalCompositionSignature;

  /// Complete ordered token-segment and proof-only state-gap chain.
  std::vector<StructuralHunkTilingEdgeWitness> edges;
};

/// Reverse index from one emitted token segment to its structural hunk witness.
///
/// The normalizer still emits ordinary token hunks for downstream classifiers.
/// This binding lets an accepted candidate recover the complete structural
/// witness without changing the hunk type or duplicating proof-only state-gap
/// edges in the emitted edit stream.
struct StructuralHunkTilingSegmentBinding {
  /// Exact emitted token-segment envelope used as the reverse-index key.
  uint64_t aStart = 0;
  uint64_t aEnd = 0;
  uint64_t bStart = 0;
  uint64_t bEnd = 0;

  /// Position of the complete witness in the durable witness ledger.
  size_t witnessIndex = 0;

  /// Redundant durable identity used to reject stale or mismatched bindings.
  uint64_t parentTilingWitnessId = 0;

  /// Exact token-segment edge selected within the complete witness.
  uint32_t segmentIndex = 0;
};

/// Transitional aliases for APIs whose externally visible names still mention
/// mixed-owner tiling.
///
/// The persisted types themselves are fully generalized. These aliases can be
/// removed independently after the remaining consumers are renamed.
using MixedOwnerTilingEdgeKind = StructuralHunkTilingEdgeKind;
using MixedOwnerTilingSegmentWitness = StructuralHunkTilingEdgeWitness;
using MixedOwnerTilingWitness = StructuralHunkTilingWitness;
using MixedOwnerTilingSegmentBinding = StructuralHunkTilingSegmentBinding;

} // namespace refold
} // namespace clang

#endif // LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDTILINGWITNESSTYPES_H
