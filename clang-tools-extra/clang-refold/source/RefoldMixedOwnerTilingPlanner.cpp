//===--- RefoldMixedOwnerTilingPlanner.cpp ----------------------*- C++ -*-===//
//
// Structural token-hunk tiling service for clang-refold.
//
// This file contains the deterministic DP normalizer that proves when a single
// token-level hunk may be lowered into multiple independently emitted token
// hunks.  The historical mixed-realizer theorem remains one reason for a split;
// an exact protected preprocessing interval between source runs is the second.
// That second theorem derives canonical maximal physical source runs from exact
// per-token mappings before the DP may use a protected boundary; arbitrary
// subrange search cannot manufacture the split.  Delete fragments share one
// empty B boundary. Replacement fragments are admitted only when every
// maximum-length local token alignment crosses each interior source-run seam at
// the same exact B-token boundary, producing one monotone B partition. This
// service does not
// build source edits.
// Each emitted segment remains subject to the ordinary TU/include/macro proof
// path after normalization.
//
//===----------------------------------------------------------------------===//

#include "source/RefoldMixedOwnerTilingPlanner.h"

#include "core/RefoldLog.h"
#include "core/RefoldModel.h"
#include "core/RefoldOwnerClassifier.h"
#include "line-control/LineDirectiveInserter.h"
#include "macro/RefoldMacroStateProof.h"
#include "macro/RefoldMacroTopology.h"
#include "proof/RefoldOwnerStateProof.h"
#include "proof/RefoldProofLattice.h"
#include "proof/RefoldStructuralHunkTilingProof.h"
#include "proof/RefoldWitnessTrace.h"
#include "source/RefoldPreprocessingStructureIndex.h"
#include "source/RefoldSourceGapProof.h"
#include "source/RefoldSourceMapper.h"
#include "util/RefoldPathIdentity.h"

#include "clang/Basic/LangOptions.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/FormatVariadic.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <cassert>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

using namespace llvm;

namespace clang {
namespace refold {

namespace {

/// Translate the source-index producer namespace into the persisted structural
/// witness namespace. Producer ids are array-local, so the kind must always be
/// retained with the optional integer id.
StructuralProtectedStructureKind structuralProtectedStructureKind(
    PreprocessingStructureKind kind) {
  switch (kind) {
  case PreprocessingStructureKind::ConditionalIf:
    return StructuralProtectedStructureKind::ConditionalIf;
  case PreprocessingStructureKind::ConditionalIfdef:
    return StructuralProtectedStructureKind::ConditionalIfdef;
  case PreprocessingStructureKind::ConditionalIfndef:
    return StructuralProtectedStructureKind::ConditionalIfndef;
  case PreprocessingStructureKind::ConditionalElif:
    return StructuralProtectedStructureKind::ConditionalElif;
  case PreprocessingStructureKind::ConditionalElifdef:
    return StructuralProtectedStructureKind::ConditionalElifdef;
  case PreprocessingStructureKind::ConditionalElifndef:
    return StructuralProtectedStructureKind::ConditionalElifndef;
  case PreprocessingStructureKind::ConditionalElse:
    return StructuralProtectedStructureKind::ConditionalElse;
  case PreprocessingStructureKind::ConditionalEndif:
    return StructuralProtectedStructureKind::ConditionalEndif;
  case PreprocessingStructureKind::MacroDefine:
    return StructuralProtectedStructureKind::MacroDefine;
  case PreprocessingStructureKind::MacroUndef:
    return StructuralProtectedStructureKind::MacroUndef;
  case PreprocessingStructureKind::Include:
    return StructuralProtectedStructureKind::Include;
  case PreprocessingStructureKind::IncludeNext:
    return StructuralProtectedStructureKind::IncludeNext;
  case PreprocessingStructureKind::Import:
    return StructuralProtectedStructureKind::Import;
  case PreprocessingStructureKind::Pragma:
    return StructuralProtectedStructureKind::Pragma;
  case PreprocessingStructureKind::PragmaOperator:
    return StructuralProtectedStructureKind::PragmaOperator;
  case PreprocessingStructureKind::LineControl:
    return StructuralProtectedStructureKind::LineControl;
  case PreprocessingStructureKind::ErrorDirective:
    return StructuralProtectedStructureKind::ErrorDirective;
  case PreprocessingStructureKind::WarningDirective:
    return StructuralProtectedStructureKind::WarningDirective;
  case PreprocessingStructureKind::OtherDirective:
    return StructuralProtectedStructureKind::OtherDirective;
  }
  llvm_unreachable("invalid preprocessing-structure kind");
}

/// Return the stable numeric component of one owner identity for deterministic
/// source-gap piece ordering.  The owner kind is carried separately as the
/// primary ordering key, so ids from different producer arrays never alias.
uint64_t structuralGapOwnerStableId(const Owner &owner) {
  switch (owner.kind) {
  case OwnerKind::TU:
    return 0;
  case OwnerKind::Include:
    return owner.includeId.value_or(std::numeric_limits<uint64_t>::max());
  case OwnerKind::MacroInvocation:
    return owner.macroInvocationId.value_or(
        std::numeric_limits<uint64_t>::max());
  case OwnerKind::MacroDirective:
    return owner.macroDirectiveId.value_or(
        std::numeric_limits<uint64_t>::max());
  case OwnerKind::LineControlIsland:
    return owner.lineControlId.value_or(
        std::numeric_limits<uint64_t>::max());
  case OwnerKind::PragmaIsland:
    return owner.pragmaId.value_or(std::numeric_limits<uint64_t>::max());
  case OwnerKind::ConditionalGroup:
    return owner.condGroupId.value_or(std::numeric_limits<uint64_t>::max());
  case OwnerKind::ConditionalArm:
    return owner.condArmId.value_or(std::numeric_limits<uint64_t>::max());
  case OwnerKind::Unknown:
    return std::numeric_limits<uint64_t>::max();
  }
  llvm_unreachable("invalid owner kind");
}

StructuralProducerIdentityKind structuralProducerIdentityKind(
    PreprocessingStructureModelKind kind) {
  switch (kind) {
  case PreprocessingStructureModelKind::None:
    return StructuralProducerIdentityKind::None;
  case PreprocessingStructureModelKind::MacroDirective:
    return StructuralProducerIdentityKind::MacroDirective;
  case PreprocessingStructureModelKind::IncludeDirective:
    return StructuralProducerIdentityKind::IncludeDirective;
  case PreprocessingStructureModelKind::PragmaDirective:
    return StructuralProducerIdentityKind::PragmaDirective;
  case PreprocessingStructureModelKind::LineControlEvent:
    return StructuralProducerIdentityKind::LineControlEvent;
  case PreprocessingStructureModelKind::ConditionalDirective:
    return StructuralProducerIdentityKind::ConditionalDirective;
  }
  llvm_unreachable("invalid preprocessing-structure model kind");
}

/// Emit the complete accepted structural partition in source order.
///
/// This diagnostic consumes only the already-persisted witness. It therefore
/// cannot influence partition search, ambiguity rejection, or normalized hunk
/// emission. Keeping the dump at this theorem boundary makes a future
/// regression classifiable from one trace block: the original A/B envelope,
/// every emitted segment, every untouched preprocessing gap, every replacement
/// boundary projection, and the semantic reason for splitting are reported
/// together.
void logAcceptedStructuralTiling(
    const StructuralHunkTilingWitness &witness) {
  if (!inTraceMode())
    return;

  REFOLD_LOG_TRACE("tiling/structural", "structural tiling accepted:");
  REFOLD_LOG_TRACE("tiling/structural",
                   "original A=[{0},{1}) B=[{2},{3})",
                   witness.originalAStart, witness.originalAEnd,
                   witness.originalBStart, witness.originalBEnd);
  REFOLD_LOG_TRACE("tiling/structural", "reason={0}",
                   toString(witness.reason));

  uint32_t tokenSegmentIndex = 0;
  uint32_t preservedGapIndex = 0;
  for (const StructuralHunkTilingEdgeWitness &edge : witness.edges) {
    if (edge.kind == StructuralHunkTilingEdgeKind::TokenSegment) {
      if (edge.sourceByteRangeKnown) {
        REFOLD_LOG_TRACE(
            "tiling/structural",
            "segment {0} A=[{1},{2}) B=[{3},{4}) source='{5}'[{6},{7}) "
            "owner={8}",
            tokenSegmentIndex++, edge.aStart, edge.aEnd, edge.bStart,
            edge.bEnd, edge.sourcePath, edge.sourceBegin, edge.sourceEnd,
            edge.ownerSignature);
      } else {
        REFOLD_LOG_TRACE(
            "tiling/structural",
            "segment {0} A=[{1},{2}) B=[{3},{4}) source=<unknown> "
            "owner={5}",
            tokenSegmentIndex++, edge.aStart, edge.aEnd, edge.bStart,
            edge.bEnd, edge.ownerSignature);
      }
      continue;
    }

    if (edge.kind == StructuralHunkTilingEdgeKind::StateGap) {
      REFOLD_LOG_TRACE(
          "tiling/structural",
          "preserved gap {0} A=[{1},{2}) B=[{3},{4}) "
          "source='{5}'[{6},{7}) structure={8} disposition={9} "
          "producerKind={10} producerItem={11} conditionalGroup={12} "
          "conditionalArm={13}",
          preservedGapIndex++, edge.aStart, edge.aEnd, edge.bStart,
          edge.bEnd, edge.sourcePath, edge.sourceBegin, edge.sourceEnd,
          toString(edge.protectedStructureKind),
          toString(edge.gapDisposition), toString(edge.producerIdentityKind),
          edge.producerItemId, edge.producerConditionalGroupId,
          edge.producerConditionalArmId);
    }
  }

  for (const StructuralBoundaryProjectionWitness &projection :
       witness.boundaryProjections) {
    REFOLD_LOG_TRACE(
        "tiling/structural",
        "boundary projection A={0} lowerB={1} upperB={2} selectedB={3} "
        "unique={4}",
        projection.aTokenBoundary, projection.lowerBTokenBoundary,
        projection.upperBTokenBoundary, projection.bTokenBoundary,
        projection.uniqueProjection);
  }

  REFOLD_LOG_TRACE(
      "tiling/structural",
      "proof uniquePartition={0} sourceByteCoverComplete={1} "
      "targetTokenStreamComposed={2} stateTransitionsComposed={3} "
      "preservedGapsDisjointFromEdits={4}",
      witness.uniquePartition, witness.sourceByteCoverComplete,
      witness.targetTokenStreamComposed, witness.stateTransitionsComposed,
      witness.preservedGapsDisjointFromEdits);
}

} // namespace

RefoldMixedOwnerTilingPlanner::RefoldMixedOwnerTilingPlanner(Dependencies deps)
    : deps_(deps) {}

RefoldMixedOwnerTilingPlanner::MixedOwnerTilingPlan
RefoldMixedOwnerTilingPlanner::Plan(std::vector<diffutils::Hunk> hunks,
                                    StringRef tuBytes) {
  // Source-gap classification needs read-only access to arbitrary source
  // owners, not just the top-level TU. Cache buffers by model path so repeated
  // include/gap proofs do not repeatedly hit the file system. Missing files
  // make the gap proof non-applicable; the cache never synthesizes text.
  llvm::StringMap<std::unique_ptr<llvm::MemoryBuffer>> sourceGapBufferCache;
  auto getSourceBytesForGapPath =
      [&](StringRef path) -> std::optional<StringRef> {
    if (deps_.pathIdentity.PathsEqual(path, deps_.tuPath))
      return tuBytes;

    auto found = sourceGapBufferCache.find(path);
    if (found != sourceGapBufferCache.end())
      return found->second->getBuffer();

    const std::string absolutePath = deps_.lineDirs.ToAbsolutePath(path);
    auto bufOrErr = llvm::MemoryBuffer::getFile(absolutePath);
    if (!bufOrErr)
      return std::nullopt;

    auto &cached = sourceGapBufferCache[path];
    cached = std::move(*bufOrErr);
    return cached->getBuffer();
  };

  // Same-realizer splitting must be justified by the same exact lexical
  // protection census used by direct TU byte realization.  The TU index is
  // already engine-owned; indexes for header/include occurrences are built
  // lazily and cached by physical path plus concrete include owner.  Repeated
  // header visits therefore never share producer bindings accidentally.
  using StructureIndexCacheKey =
      std::pair<std::string, std::optional<uint64_t>>;
  std::map<StructureIndexCacheKey,
           std::unique_ptr<RefoldPreprocessingStructureIndex>>
      sourceStructureIndexCache;
  auto getStructureIndexForSource =
      [&](const OwnerSourceRange &source)
      -> const RefoldPreprocessingStructureIndex * {
    if (!source.IsComplete())
      return nullptr;

    if (!source.includeId &&
        deps_.pathIdentity.PathsEqual(source.path, deps_.tuPath)) {
      return &deps_.tuPreprocessingStructureIndex;
    }

    StructureIndexCacheKey key{source.path, source.includeId};
    auto found = sourceStructureIndexCache.find(key);
    if (found != sourceStructureIndexCache.end())
      return found->second.get();

    std::optional<StringRef> sourceBytes =
        getSourceBytesForGapPath(source.path);
    if (!sourceBytes)
      return nullptr;

    auto index = std::make_unique<RefoldPreprocessingStructureIndex>(
        RefoldPreprocessingStructureIndex::Build(
            RefoldPreprocessingStructureIndex::Dependencies{
                deps_.model, deps_.pathIdentity, deps_.macroStateProof,
                deps_.lexLang},
            source.path, *sourceBytes, source.includeId));
    const RefoldPreprocessingStructureIndex *result = index.get();
    sourceStructureIndexCache.emplace(std::move(key), std::move(index));
    return result;
  };

  // Structural witnesses are rebuilt from the current token diff and attached
  // to later accepted candidates by exact A/B token-envelope binding.  The
  // legacy ledger type names are retained temporarily to avoid unrelated API
  // churn while same-realizer structure preservation is introduced.
  deps_.mixedOwnerTilingWitnesses.clear();
  deps_.mixedOwnerTilingSegmentBindings.clear();

  // Split replace/delete hunks when one unique structural partition is proven.
  //
  // The proof admits ordered partitions with more than one interior boundary
  // when each segment has an independently provable realizer and B-token
  // envelope.  This preserves the same exact-boundary obligation while
  // covering hunks with more than two independently realizable regions:
  //
  //   * every segment consumes a non-empty A subrange;
  //   * every segment has a known TU/include/macro realizer;
  //   * every segment has an A->B token envelope inside the original hunk;
  //   * segment B envelopes are contiguous and exactly tile the original B
  //     hunk;
  //   * the final partition contains either two different realizers or one
  //     exact protected preprocessing interval between adjacent same-realizer
  //     segments;
  //   * every protected replacement boundary, including one between different
  //     realizers, has one unique A-to-B token projection and exactly matches
  //     both neighboring B envelopes;
  //   * the minimal tiling is unique. Equal-cost alternatives are rejected
  //     rather than hidden behind an implementation-order tie-breaker.
  //
  // This remains a normalization-only proof.  Each emitted sub-hunk is still
  // validated later by the ordinary macro/include/TU classifier before any
  // source edit is accepted.  Proof-only zero-token state-gap edges are part of
  // the searched graph: they are attached to token-to-token DP transitions,
  // counted in the proof cost, and included in ambiguity detection even though
  // they are not emitted as hunks.
  if (hunks.size() > 0) {
    enum class HunkRealizerKind {
      Unknown,
      TU,
      Include,
      Macro,
    };

    struct HunkRealizer {
      HunkRealizerKind kind = HunkRealizerKind::Unknown;
      uint64_t id = 0;

      bool operator==(const HunkRealizer &other) const {
        return kind == other.kind && id == other.id;
      }

      bool operator!=(const HunkRealizer &other) const {
        return !(*this == other);
      }

      bool operator<(const HunkRealizer &other) const {
        if (kind != other.kind)
          return static_cast<unsigned>(kind) <
                 static_cast<unsigned>(other.kind);
        return id < other.id;
      }
    };

    auto classifyHunkRealizer = [&](uint64_t aStart,
                                    uint64_t aEnd) -> HunkRealizer {
      if (aEnd <= aStart)
        return {};

      diffutils::Hunk probe{aStart, aEnd, 0, 0};
      Owner probeOwner =
          deps_.ownerClassifier.ClassifyOwnerWithSegments(deps_.tuPath, probe);
      if (auto *m = deps_.macroTopology.SmallestCoveringPatchableMacro(
              aStart, aEnd, probeOwner.includeId)) {
        if (m->invB && m->invE)
          return {HunkRealizerKind::Macro, m->id};
      }

      if (probeOwner.kind == OwnerKind::Include && probeOwner.includeId)
        return {HunkRealizerKind::Include, *probeOwner.includeId};

      if (deps_.ownerClassifier.HunkMapsToTU(aStart, aEnd, deps_.tuPath))
        return {HunkRealizerKind::TU, 0};

      return {};
    };

    enum class PartitionEdgeKind {
      /// A normal token-producing segment.  These are the only edges that
      /// become emitted token hunks after normalization.
      TokenSegment,
      /// A zero-token source-state owner that sits between two token segments.
      /// These edges never become token hunks; they are proof-carrying
      /// separators used to show that the source gap between neighboring
      /// owners is closed rather than silently skipped.
      StateGap,
    };

    struct PartitionEdge {
      PartitionEdgeKind kind = PartitionEdgeKind::TokenSegment;
      uint64_t aStart = 0;
      uint64_t aEnd = 0;
      uint64_t bStart = 0;
      uint64_t bEnd = 0;
      HunkRealizer realizer;
      /// True when this token segment is permitted to carry an empty B
      /// envelope.  Every delete fragment uses the parent's shared empty
      /// boundary.  A replacement fragment may be empty only when two adjacent
      /// uniquely projected structural boundaries collapse onto the same B
      /// boundary.  Arbitrary subranges and insert-only tilings cannot use this
      /// escape hatch.
      bool allowEmptyBEnvelope = false;
      /// Typed physical treatment of a proof-only source gap.  Token segments
      /// retain `Unknown`; a state gap becomes `PreservedInPlace` only after
      /// exact lexical ownership, complete byte coverage, and source-order
      /// separation from both neighboring token segments have all been proved.
      StructuralGapDisposition gapDisposition =
          StructuralGapDisposition::Unknown;
      /// True only for a proof-only gap edge whose exact source interval covers
      /// protected preprocessing structure.  Ordinary zero-token owners and
      /// lexer trivia do not satisfy the same-realizer split theorem.
      bool protectedPreprocessingStructure = false;
      /// Exact lexical identity carried into the durable generalized witness.
      /// Token edges leave these fields empty. A producer-unbound directive is
      /// still recorded with kind `None`; its exact source range remains the
      /// preservation authority.
      bool protectedStructureIdentityRecorded = false;
      StructuralProtectedStructureKind protectedStructureKind =
          StructuralProtectedStructureKind::Unknown;
      StructuralProducerIdentityKind producerIdentityKind =
          StructuralProducerIdentityKind::None;
      std::optional<uint64_t> producerItemId;
      std::optional<uint64_t> producerConditionalGroupId;
      std::optional<uint64_t> producerConditionalArmId;
      std::optional<OwnerClosure> closure;

      bool IsTokenSegment() const {
        return kind == PartitionEdgeKind::TokenSegment;
      }

      bool IsStateGap() const { return kind == PartitionEdgeKind::StateGap; }
    };

    const size_t noTokenEdgeIndex = std::numeric_limits<size_t>::max();

    struct PartitionStateKey {
      uint64_t bPos = 0;
      HunkRealizer firstRealizer;
      HunkRealizer lastRealizer;
      size_t lastTokenEdgeIndex = std::numeric_limits<size_t>::max();
      bool mixedRealizers = false;
      bool preservedPreprocessingStructure = false;

      bool operator<(const PartitionStateKey &other) const {
        if (bPos != other.bPos)
          return bPos < other.bPos;
        if (firstRealizer != other.firstRealizer)
          return firstRealizer < other.firstRealizer;
        if (lastRealizer != other.lastRealizer)
          return lastRealizer < other.lastRealizer;
        if (lastTokenEdgeIndex != other.lastTokenEdgeIndex)
          return lastTokenEdgeIndex < other.lastTokenEdgeIndex;
        if (mixedRealizers != other.mixedRealizers)
          return mixedRealizers < other.mixedRealizers;
        return preservedPreprocessingStructure <
               other.preservedPreprocessingStructure;
      }
    };

    struct PartitionParent {
      bool valid = false;
      size_t edgeIndex = 0;
      PartitionStateKey prev;
      unsigned cost = 0;
      // State-gap edges are part of the searched proof graph.  Because they do
      // not advance A or B token coordinates, they are stored on the transition
      // that reaches the following token segment rather than as standalone DP
      // states that would create zero-length cycles.
      SmallVector<PartitionEdge, 4> stateGapsBeforeEdge;
      // True once the same DP state can be reached by two distinct minimal
      // parent chains.  The mixed-owner proof requires a deterministic tiling,
      // not merely a deterministic tie-breaker, so equal-cost ambiguity is
      // rejected.
      bool ambiguous = false;
    };

    struct ClosedStateGapTransition {
      SmallVector<PartitionEdge, 4> gaps;
      /// True only when the exact preprocessing-structure index found at least
      /// one protected interval in the physical gap and that interval had one
      /// unique modeled state-gap owner.
      bool hasProtectedPreprocessingStructure = false;
    };

    /// Return whether a preserved transition contains a #define or #undef.
    ///
    /// Direct TU realization treats complete producer-bound macro-state lines
    /// specially: it may consume them only because RefoldMacroStateRepairPlanner
    /// subsequently proves their final-stream liveness and repositions or
    /// replays them when necessary.  A structural replacement would instead
    /// leave the directive physically in place and emit independent edits on
    /// both sides.  Boundary projection proves only how B tokens are
    /// partitioned; it does not prove equivalence with that later macro-state
    /// ordering theorem.  Therefore Patch 3.1 must defer such transitions to the
    /// existing whole-hunk path until an explicit composition theorem connects
    /// projected fragments with macro-state liveness repair.
    auto transitionContainsMacroStateDirective =
        [](const ClosedStateGapTransition &transition) {
          return llvm::any_of(
              transition.gaps, [](const PartitionEdge &gap) {
                if (!gap.IsStateGap() ||
                    !gap.protectedPreprocessingStructure ||
                    !gap.protectedStructureIdentityRecorded ||
                    gap.producerIdentityKind !=
                        StructuralProducerIdentityKind::MacroDirective ||
                    !gap.producerItemId) {
                  return false;
                }
                return gap.protectedStructureKind ==
                           StructuralProtectedStructureKind::MacroDefine ||
                       gap.protectedStructureKind ==
                           StructuralProtectedStructureKind::MacroUndef;
              });
        };

    struct StructuralPartition {
      SmallVector<PartitionEdge, 8> edges;
      StructuralTilingReason reason = StructuralTilingReason::Unknown;
      /// The partition search or canonical physical-run derivation found one
      /// and only one admissible minimum-cost path. Equal-cost alternatives
      /// are rejected before this record is constructed.
      bool uniquePartition = false;
      /// Number of canonical maximal physical source runs derived directly
      /// from the original hunk's A-token mappings. This is nonzero for every
      /// replacement or deletion whose partition preserves protected source
      /// structure; historical mixed-realizer tilings with no such seam retain
      /// zero because they need not be one monotone physical-source cover.
      uint32_t physicalSourceRunCount = 0;
      /// Every A token was mapped exactly once, in preprocessing-token order,
      /// to a complete raw source token, and those mappings formed the
      /// canonical maximal runs recorded by `physicalSourceRunCount`.
      bool physicalSourceRunsProven = false;
      /// The emitted token segments are exactly those maximal runs. Because a
      /// run boundary exists only at an exact byte-complete protected source
      /// gap, no partition with fewer emitted fragments can preserve all such
      /// gaps outside the edits. A combined mixed-realizer partition may carry
      /// additional token segments inside a run and therefore leave this false.
      bool uniqueMinimumFragmentPartition = false;
      /// Ordered Patch 3.1/3.2 proofs for every interior canonical source-run
      /// boundary of a structure-preserving replacement. Delete-only partitions
      /// use the shared empty-B theorem below instead.
      SmallVector<StructuralBoundaryProjectionWitness, 8> boundaryProjections;
      bool uniqueBoundaryProjectionProven = false;
      /// Every emitted token segment of a delete-only partition carries the
      /// same empty B-token envelope `[q,q)`.  This is a theorem result, not an
      /// incidental consequence of initializing edge coordinates from the
      /// parent hunk.
      bool sharedEmptyBEnvelopeProven = false;
      uint64_t sharedEmptyBBoundary = 0;
      /// Every proof-only state edge in a delete-only partition is an exact
      /// `PreservedInPlace` interval, and those intervals compose in the same
      /// physical source order between the emitted token runs.
      bool preservedStateChainComposed = false;
      /// Complete physical source-byte cover of the min/max token carrier.
      /// This is stronger than token-stream composition and is required before
      /// a later TU segment may use this witness as structural authority.
      bool sourceByteCoverComplete = false;
      /// Every proof-only source gap remains physically ordered between the
      /// same two emitted token carriers that surrounded it in the input.
      bool preservedGapSourceOrderProven = false;
      /// No emitted token carrier intersects a gap classified as
      /// `PreservedInPlace`.
      bool preservedGapsDisjointFromTokenSegments = false;
    };

    /// One maximal A-token run whose mapped spellings occupy one monotone
    /// physical source interval without crossing protected preprocessing
    /// structure.
    struct PhysicalSourceRun {
      uint64_t aStart = 0;
      uint64_t aEnd = 0;
      OwnerSourceRange source;
    };

    /// Canonical physical source partition for one original hunk.
    ///
    /// The partition is not a search result.  It is derived by one left-to-
    /// right pass over every A token.  Adjacent tokens remain in the same run
    /// across exact lexer trivia; an exact byte-complete preprocessing-
    /// structure gap terminates the run.  Missing, duplicate, overlapping, or
    /// nonmonotone mappings make this direct structural theorem unavailable.
    struct PhysicalSourceRunPlan {
      SmallVector<PhysicalSourceRun, 8> runs;
      /// Exact source gap between `runs[i]` and `runs[i + 1]`.
      SmallVector<OwnerSourceRange, 8> protectedGaps;
      /// Exact lower/upper B-token projection for each interior run boundary.
      /// These records are populated only for replacement hunks and remain in
      /// the same order as `protectedGaps`.
      SmallVector<StructuralBoundaryProjectionWitness, 8> boundaryProjections;
    };

    std::set<uint64_t> duplicateTokmapPP;
    std::set<uint64_t> seenTokmapPP;
    for (const RefoldModel::TokMapEntry &entry : deps_.model.GetTokmap()) {
      if (!seenTokmapPP.insert(entry.pp).second)
        duplicateTokmapPP.insert(entry.pp);
    }

    auto mappedTUSourceRangeForTokens =
        [&](uint64_t aStart, uint64_t aEnd) -> std::optional<OwnerSourceRange> {
      const auto &tokmapByPP = deps_.model.GetTokmapByPP();
      uint64_t sourceBegin = std::numeric_limits<uint64_t>::max();
      uint64_t sourceEnd = 0;
      bool sawTU = false;

      for (uint64_t pp = aStart; pp < aEnd; ++pp) {
        auto it = tokmapByPP.find(pp);
        if (it == tokmapByPP.end())
          continue;
        const RefoldModel::TokMapEntry &entry = it->second;
        if (!deps_.pathIdentity.PathsEqual(entry.file, deps_.tuPath))
          return std::nullopt;
        sourceBegin = std::min<uint64_t>(sourceBegin, entry.b);
        sourceEnd = std::max<uint64_t>(sourceEnd, entry.e);
        sawTU = true;
      }

      if (!sawTU || sourceBegin > sourceEnd)
        return std::nullopt;
      return OwnerSourceRange::From(deps_.tuPath, sourceBegin, sourceEnd);
    };

    auto buildTokenSegmentClosure =
        [&](const PartitionEdge &edge) -> std::optional<OwnerClosure> {
      if (!edge.IsTokenSegment() || edge.aEnd <= edge.aStart ||
          edge.bEnd < edge.bStart ||
          (!edge.allowEmptyBEnvelope && edge.bEnd <= edge.bStart))
        return std::nullopt;

      Owner owner = Owner::Unknown();
      std::optional<OwnerSourceRange> source;
      switch (edge.realizer.kind) {
      case HunkRealizerKind::Macro: {
        const RefoldModel::MacroInvocation *macro =
            deps_.macroTopology.FindMacroInvocationById(edge.realizer.id);
        if (!macro || !macro->invFile || !macro->invB || !macro->invE)
          return std::nullopt;
        owner = Owner::MacroInvocation(macro->id);
        source = OwnerSourceRange::From(*macro->invFile, *macro->invB,
                                        *macro->invE, macro->ownerIncludeId);
        break;
      }
      case HunkRealizerKind::Include: {
        const RefoldModel::IncludeItem *include =
            deps_.model.GetIncludeById(edge.realizer.id);
        if (!include)
          return std::nullopt;
        owner = Owner::Include(include->id);
        source = OwnerSourceRange::From(include->sitePath, include->siteB,
                                        include->siteE, include->parent);
        break;
      }
      case HunkRealizerKind::TU: {
        owner = Owner::TU();
        source = mappedTUSourceRangeForTokens(edge.aStart, edge.aEnd);
        if (!source)
          return std::nullopt;
        break;
      }
      case HunkRealizerKind::Unknown:
        return std::nullopt;
      }

      return deps_.ownerStateProof.AttachCanonicalStateSummary(
          OwnerClosure::From(std::move(owner), std::move(*source),
                             OwnerTokenRange::From(edge.aStart, edge.aEnd),
                             OwnerTokenRange::From(edge.bStart, edge.bEnd)));
    };

    auto sourceSitesComparable = [&](const OwnerSourceRange &lhs,
                                     const OwnerSourceRange &rhs) {
      return lhs.IsComplete() && rhs.IsComplete() &&
             deps_.pathIdentity.PathsEqual(lhs.path, rhs.path) &&
             lhs.includeId == rhs.includeId;
    };

    auto sourceSiteMatchesGap = [&](const OwnerSourceRange &gap, StringRef file,
                                    std::optional<uint64_t> ownerIncludeId,
                                    uint64_t begin, uint64_t end) {
      return gap.IsComplete() &&
             deps_.pathIdentity.PathsEqual(gap.path, file) &&
             gap.includeId == ownerIncludeId && gap.begin <= begin &&
             begin <= end && end <= gap.end;
    };

    auto sourceSitePathMatchesGap = [&](const OwnerSourceRange &gap,
                                        StringRef file, uint64_t begin,
                                        uint64_t end) {
      return gap.IsComplete() &&
             deps_.pathIdentity.PathsEqual(gap.path, file) &&
             gap.begin <= begin && begin <= end && end <= gap.end;
    };

    struct SourceOwnerIdentity {
      std::optional<uint64_t> includeId;
      std::optional<uint64_t> condArmId;
    };

    auto resolveSourceOwnerIdentity =
        [&](StringRef file, uint64_t begin,
            uint64_t end) -> std::optional<SourceOwnerIdentity> {
      ArrayRef<RefoldModel::Segment> segments =
          deps_.model.GetSegmentsForFile(file);
      if (segments.empty())
        return std::nullopt;

      uint64_t cursor = begin;
      bool sawCover = false;
      SourceOwnerIdentity identity;

      for (const RefoldModel::Segment &segment : segments) {
        if (segment.e <= begin)
          continue;
        if (end <= segment.b)
          break;
        if (!intervalsOverlap(begin, end, segment.b, segment.e))
          continue;

        const uint64_t partBegin = std::max<uint64_t>(begin, segment.b);
        const uint64_t partEnd = std::min<uint64_t>(end, segment.e);
        if (partBegin > cursor)
          return std::nullopt;

        SourceOwnerIdentity part{segment.ownerIncludeId,
                                 segment.ownerCondArmId};
        if (!sawCover) {
          identity = part;
          sawCover = true;
        } else if (identity.includeId != part.includeId ||
                   identity.condArmId != part.condArmId) {
          return std::nullopt;
        }
        cursor = partEnd;
      }

      if (!sawCover || cursor < end)
        return std::nullopt;
      return identity;
    };

    auto bindSourceOwnerToGap =
        [&](const OwnerSourceRange &gap, StringRef file, uint64_t begin,
            uint64_t end) -> std::optional<SourceOwnerIdentity> {
      if (!sourceSitePathMatchesGap(gap, file, begin, end))
        return std::nullopt;

      std::optional<SourceOwnerIdentity> identity =
          resolveSourceOwnerIdentity(file, begin, end);
      if (!identity) {
        // Older maps do not always provide segment facts for TU-only files.
        // Treat that as TU-owned only when the enclosing gap is also TU-owned.
        // For include-owned gaps, accepting a path-only match would bind a
        // repeated header pragma/conditional to an arbitrary include instance.
        if (gap.includeId)
          return std::nullopt;
        identity = SourceOwnerIdentity{};
      }

      if (identity->includeId != gap.includeId)
        return std::nullopt;
      return identity;
    };

    auto makeStateGapEdge = [&](Owner owner, OwnerSourceRange source,
                                uint64_t aBoundary,
                                uint64_t bBoundary) -> PartitionEdge {
      PartitionEdge gap;
      gap.kind = PartitionEdgeKind::StateGap;
      gap.aStart = aBoundary;
      gap.aEnd = aBoundary;
      gap.bStart = bBoundary;
      gap.bEnd = bBoundary;
      gap.closure = deps_.ownerStateProof.AttachCanonicalStateSummary(
          OwnerClosure::From(std::move(owner), std::move(source),
                             OwnerTokenRange::From(aBoundary, aBoundary),
                             OwnerTokenRange::From(bBoundary, bBoundary)));
      return gap;
    };

    auto collectZeroTokenStateGaps =
        [&](const OwnerSourceRange &gapSource, uint64_t aBoundary,
            uint64_t bBoundary) -> SmallVector<PartitionEdge, 8> {
      SmallVector<PartitionEdge, 8> gaps;
      if (!gapSource.IsComplete() || gapSource.end <= gapSource.begin)
        return gaps;

      // Sideband directives are zero-token owners.  They are collected in
      // source order and represented as proof-only partition edges so the mixed
      // owner tiler can distinguish "there is no source gap" from "there is a
      // state gap that was proved closed".
      for (const RefoldModel::MacroDirective &directive :
           deps_.model.GetMacroDirectives()) {
        if (!sourceSiteMatchesGap(gapSource, directive.sitePath,
                                  directive.ownerIncludeId, directive.siteB,
                                  directive.siteE))
          continue;
        gaps.push_back(makeStateGapEdge(
            Owner::MacroDirective(directive.id),
            OwnerSourceRange::From(directive.sitePath, directive.siteB,
                                   directive.siteE, directive.ownerIncludeId),
            aBoundary, bBoundary));
      }

      for (const RefoldModel::LineControlEvent &event :
           deps_.model.GetLineControls()) {
        if (!event.siteB || !event.siteE)
          continue;
        if (!sourceSiteMatchesGap(gapSource, event.physicalFile,
                                  event.ownerIncludeId, *event.siteB,
                                  *event.siteE))
          continue;
        gaps.push_back(makeStateGapEdge(
            Owner::LineControlIsland(event.id),
            OwnerSourceRange::From(event.physicalFile, *event.siteB,
                                   *event.siteE, event.ownerIncludeId),
            aBoundary, bBoundary));
      }

      for (const RefoldModel::PragmaDirective &pragma :
           deps_.model.GetPragmas()) {
        // bind zero-token pragmas to the same source owner as the gap.  Newer
        // maps can carry owner_include_id directly; older maps are resolved
        // through segment facts.  A repeated-header pragma must never be
        // accepted from path+byte containment alone because the same file bytes
        // may be visited by several include occurrences.
        if (!pragma.ownerIncludeId) {
          unsigned samePhysicalSiteWithoutOwner = 0;
          for (const RefoldModel::PragmaDirective &other :
               deps_.model.GetPragmas()) {
            if (!other.ownerIncludeId &&
                deps_.pathIdentity.PathsEqual(other.sitePath,
                                              pragma.sitePath) &&
                other.siteB == pragma.siteB && other.siteE == pragma.siteE)
              ++samePhysicalSiteWithoutOwner;
          }
          if (samePhysicalSiteWithoutOwner > 1)
            continue;
        }
        std::optional<SourceOwnerIdentity> identity = bindSourceOwnerToGap(
            gapSource, pragma.sitePath, pragma.siteB, pragma.siteE);
        if (!identity)
          continue;
        if (pragma.ownerIncludeId &&
            *pragma.ownerIncludeId !=
                identity->includeId.value_or(
                    std::numeric_limits<uint64_t>::max()) &&
            !identity->includeId) {
          continue;
        }

        // A header pragma item describes a physical directive, not necessarily
        // a unique replay occurrence.  When segment facts prove that this
        // source gap belongs to a concrete repeated include instance, bind the
        // state-gap closure to that occurrence even if the serialized
        // owner_include_id names another replay of the same physical pragma.
        // The segment-derived identity is the stronger proof here.
        gaps.push_back(makeStateGapEdge(
            Owner::PragmaIsland(pragma.id, identity->condArmId),
            OwnerSourceRange::From(pragma.sitePath, pragma.siteB, pragma.siteE,
                                   identity->includeId),
            aBoundary, bBoundary));
      }

      for (const RefoldModel::IncludeItem &include :
           deps_.model.GetIncludes()) {
        if (!sourceSiteMatchesGap(gapSource, include.sitePath, include.parent,
                                  include.siteB, include.siteE))
          continue;
        // Non-empty includes already have token edges in the normal tiling.
        // Only zero-token include transitions are sideband gap owners here.
        if (include.cover.IsValid())
          continue;
        gaps.push_back(makeStateGapEdge(
            Owner::Include(include.id),
            OwnerSourceRange::From(include.sitePath, include.siteB,
                                   include.siteE, include.parent),
            aBoundary, bBoundary));
      }

      for (const RefoldModel::MacroInvocation &macro :
           deps_.model.GetMacroInvocations()) {
        if (!macro.invFile || !macro.invB || !macro.invE)
          continue;
        if (macro.cover.IsValid())
          continue;
        if (!sourceSiteMatchesGap(gapSource, *macro.invFile,
                                  macro.ownerIncludeId, *macro.invB,
                                  *macro.invE))
          continue;
        gaps.push_back(makeStateGapEdge(
            Owner::MacroInvocation(macro.id),
            OwnerSourceRange::From(*macro.invFile, *macro.invB, *macro.invE,
                                   macro.ownerIncludeId),
            aBoundary, bBoundary));
      }

      for (const RefoldModel::CondGroup &group : deps_.model.GetConds()) {
        if (!sourceSiteMatchesGap(gapSource, group.file, group.parentIncludeId,
                                  group.groupB, group.groupE))
          continue;
        // A conditional group owns the directive island
        // (#if/#elif/#else/#endif) and the branch-selection state.  Individual
        // arms own only their body intervals.  Using arm ids for the whole
        // group created overlapping zero-token owners and lost the distinction
        // between branch structure and selected-arm content.
        gaps.push_back(makeStateGapEdge(
            Owner::ConditionalGroup(group.id),
            OwnerSourceRange::From(group.file, group.groupB, group.groupE,
                                   group.parentIncludeId),
            aBoundary, bBoundary));
      }

      llvm::sort(gaps, [](const PartitionEdge &lhs, const PartitionEdge &rhs) {
        const OwnerSourceRange &l = lhs.closure->source;
        const OwnerSourceRange &r = rhs.closure->source;
        if (l.path != r.path)
          return l.path < r.path;
        if (l.includeId != r.includeId)
          return l.includeId.value_or(std::numeric_limits<uint64_t>::max()) <
                 r.includeId.value_or(std::numeric_limits<uint64_t>::max());
        if (l.begin != r.begin)
          return l.begin < r.begin;
        if (l.end != r.end)
          return l.end < r.end;

        auto ownerKey = [](const Owner &owner) {
          const uint64_t none = std::numeric_limits<uint64_t>::max();
          return std::make_tuple(
              static_cast<unsigned>(owner.kind), owner.includeId.value_or(none),
              owner.macroInvocationId.value_or(none),
              owner.macroDirectiveId.value_or(none),
              owner.lineControlId.value_or(none), owner.pragmaId.value_or(none),
              owner.condGroupId.value_or(none), owner.condArmId.value_or(none));
        };
        return ownerKey(lhs.closure->owner) < ownerKey(rhs.closure->owner);
      });

      return gaps;
    };

    auto sourceGapIsFullyCovered = [&](const OwnerSourceRange &gapSource,
                                       ArrayRef<PartitionEdge> gaps,
                                       std::string *reason) -> bool {
      if (!gapSource.IsComplete()) {
        if (reason)
          *reason = "source gap has incomplete source coordinates";
        return false;
      }

      const RefoldPreprocessingStructureIndex *structureIndex =
          getStructureIndexForSource(gapSource);
      if (!structureIndex) {
        if (reason)
          *reason = "source gap has no exact preprocessing-structure index";
        return false;
      }

      SmallVector<SourceGapProofPiece, 8> pieces;
      pieces.reserve(gaps.size());
      for (size_t gapIndex = 0; gapIndex < gaps.size(); ++gapIndex) {
        const PartitionEdge &gap = gaps[gapIndex];
        if (!gap.closure || !gap.closure->source.IsComplete()) {
          if (reason)
            *reason = "state-gap edge lacks a complete source closure";
          return false;
        }

        const OwnerSourceRange &owned = gap.closure->source;
        if (!deps_.pathIdentity.PathsEqual(owned.path, gapSource.path) ||
            owned.begin < gapSource.begin || owned.end > gapSource.end) {
          if (reason) {
            *reason =
                llvm::formatv("state-gap owner [{0},{1}) is outside source "
                              "gap [{2},{3})",
                              owned.begin, owned.end, gapSource.begin,
                              gapSource.end)
                    .str();
          }
          return false;
        }

        // The same physical header can be entered repeatedly.  A path and byte
        // range therefore do not identify the source surface by themselves;
        // every proof-only owner must retain the concrete include occurrence of
        // the token carriers on both sides of the gap.
        if (owned.includeId != gapSource.includeId) {
          if (reason)
            *reason =
                "state-gap owner belongs to a different include instance";
          return false;
        }

        const Owner &owner = gap.closure->owner;
        pieces.push_back(SourceGapProofPiece{
            owned.begin, owned.end, structuralGapOwnerStableId(owner),
            static_cast<uint32_t>(owner.kind),
            /*nestingClass=*/0,
            /*absorbedNestingClasses=*/0, gapIndex});
      }

      // Structural tiling does not permit overlapping proof-only owners.  The
      // shared theorem now performs the same deterministic interval
      // normalization, exact preprocessing-inventory check, and lexer-trivia
      // coverage used by expansion fallback.  No directive-looking byte can be
      // skipped merely because it contributed no ordinary PP token.
      std::optional<SourceGapProofResult> proof =
          proveSourceGapWithIndexedTrivia(*structureIndex, gapSource.begin,
                                          gapSource.end, pieces, reason);
      return proof &&
             proof->outerPiecePayloadIndices.size() == gaps.size();
    };

    auto stateGapOwnerMatchesStructureIdentity =
        [&](const PartitionEdge &gap,
            const PreprocessingStructureInterval &interval) {
          if (!gap.closure)
            return false;

          const Owner &owner = gap.closure->owner;
          switch (interval.modelKind) {
          case PreprocessingStructureModelKind::MacroDirective:
            return interval.modelItemId && owner.IsMacroDirective() &&
                   owner.macroDirectiveId == interval.modelItemId;

          case PreprocessingStructureModelKind::IncludeDirective:
            return interval.modelItemId && owner.IsInclude() &&
                   owner.includeId == interval.modelItemId;

          case PreprocessingStructureModelKind::PragmaDirective:
            return interval.modelItemId && owner.IsPragmaIsland() &&
                   owner.pragmaId == interval.modelItemId;

          case PreprocessingStructureModelKind::LineControlEvent:
            return interval.modelItemId && owner.IsLineControlIsland() &&
                   owner.lineControlId == interval.modelItemId;

          case PreprocessingStructureModelKind::ConditionalDirective:
            return interval.conditionalGroupId && owner.IsConditionalGroup() &&
                   owner.condGroupId == interval.conditionalGroupId;

          case PreprocessingStructureModelKind::None:
            break;
          }

          // A producer macro-state record can fail the structure index's
          // full-line text binding when the producer serializes normalized
          // directive trivia while `site_b/site_e` still identify the exact
          // physical macro-name site.  The lexical scanner nevertheless proves
          // the complete `#define`/`#undef` line.  In that case, bind the two
          // facts here only when one stable macro-directive id has the expected
          // directive kind and its exact producer site is wholly contained in
          // the lexical line in the same physical owner occurrence.
          //
          // This is not source-nearest recovery: the owner id, directive kind,
          // path, include occurrence, and nested byte ranges must all agree.
          // Multiple records with the same id or any partial crossing fail
          // closed.  Once bound, the lexical interval replaces the narrower
          // producer site as the physical preservation authority, avoiding two
          // overlapping proof-only edges for the same directive.
          if (interval.modelItemId || !owner.IsMacroDirective() ||
              !owner.macroDirectiveId ||
              (interval.kind != PreprocessingStructureKind::MacroDefine &&
               interval.kind != PreprocessingStructureKind::MacroUndef)) {
            return false;
          }

          const RefoldModel::MacroDirective *matchedDirective = nullptr;
          for (const RefoldModel::MacroDirective &directive :
               deps_.model.GetMacroDirectives()) {
            if (directive.id != *owner.macroDirectiveId)
              continue;
            if (matchedDirective)
              return false;
            matchedDirective = &directive;
          }
          if (!matchedDirective)
            return false;

          const StringRef expectedSubkind =
              interval.kind == PreprocessingStructureKind::MacroDefine
                  ? StringRef("#define")
                  : StringRef("#undef");
          if (matchedDirective->subkind != expectedSubkind ||
              !deps_.pathIdentity.PathsEqual(matchedDirective->sitePath,
                                             interval.sourcePath) ||
              matchedDirective->ownerIncludeId != interval.ownerIncludeId ||
              matchedDirective->siteE <= matchedDirective->siteB ||
              interval.begin > matchedDirective->siteB ||
              matchedDirective->siteE > interval.end) {
            return false;
          }

          const OwnerSourceRange &owned = gap.closure->source;
          return owned.IsComplete() &&
                 deps_.pathIdentity.PathsEqual(owned.path,
                                               matchedDirective->sitePath) &&
                 owned.includeId == matchedDirective->ownerIncludeId &&
                 owned.begin == matchedDirective->siteB &&
                 owned.end == matchedDirective->siteE;
        };

    auto ownerForPreservedStructureInterval =
        [](const PreprocessingStructureInterval &interval) -> Owner {
      // Prefer an exact producer identity when one exists.  The structure
      // index's lexical interval remains the physical authority, so this owner
      // is only a stable theorem label and never authorizes reconstruction.
      // Conditional controls are especially important: one `CondGroup` owns
      // several disjoint directive lines, while a structural deletion may
      // preserve only one of those lines.
      switch (interval.modelKind) {
      case PreprocessingStructureModelKind::MacroDirective:
        if (interval.modelItemId)
          return Owner::MacroDirective(*interval.modelItemId);
        break;

      case PreprocessingStructureModelKind::IncludeDirective:
        if (interval.modelItemId)
          return Owner::Include(*interval.modelItemId);
        break;

      case PreprocessingStructureModelKind::PragmaDirective:
        if (interval.modelItemId) {
          return Owner::PragmaIsland(*interval.modelItemId,
                                     interval.ownerConditionalArmId);
        }
        break;

      case PreprocessingStructureModelKind::LineControlEvent:
        if (interval.modelItemId)
          return Owner::LineControlIsland(*interval.modelItemId);
        break;

      case PreprocessingStructureModelKind::ConditionalDirective:
        if (interval.conditionalGroupId)
          return Owner::ConditionalGroup(*interval.conditionalGroupId);
        break;

      case PreprocessingStructureModelKind::None:
        break;
      }

      // A lexically exact non-conditional directive need not have a producer
      // record in order to be preserved safely.  Bind it to its concrete
      // physical TU/include occurrence: the disposition theorem proves that
      // the bytes are untouched and retain their order, so no semantic claim
      // about the unknown directive is required.  Conditional binding failures
      // never reach this fallback because they make the protection census
      // incomplete globally.
      if (interval.ownerIncludeId) {
        return Owner::Include(*interval.ownerIncludeId,
                              interval.ownerConditionalArmId);
      }
      return Owner::TU(interval.ownerConditionalArmId);
    };

    auto bindProtectedStructureIntervalsToStateGaps =
        [&](const OwnerSourceRange &gapSource, uint64_t aBoundary,
            uint64_t bBoundary,
            SmallVectorImpl<PartitionEdge> &gaps,
            std::string *reason) -> std::optional<bool> {
      const RefoldPreprocessingStructureIndex *structureIndex =
          getStructureIndexForSource(gapSource);
      if (!structureIndex ||
          !structureIndex->IsDirectTUProtectionCensusComplete()) {
        if (reason) {
          *reason =
              "preprocessing-structure census is incomplete for source gap";
        }
        return std::nullopt;
      }

      std::vector<const PreprocessingStructureInterval *> protectedIntervals =
          structureIndex->FindOverlapping(gapSource.begin, gapSource.end);
      if (protectedIntervals.empty())
        return false;

      // Work transactionally.  A failed exact binding must not widen one state
      // owner and then let the historical mixed-realizer path observe a partly
      // modified gap graph.
      SmallVector<PartitionEdge, 8> normalizedGaps(gaps.begin(), gaps.end());
      for (const PreprocessingStructureInterval *interval :
           protectedIntervals) {
        if (!interval || interval->begin < gapSource.begin ||
            interval->end > gapSource.end) {
          if (reason) {
            *reason =
                "protected preprocessing interval is only partially inside "
                "source gap";
          }
          return std::nullopt;
        }
        size_t matchingGapIndex = std::numeric_limits<size_t>::max();
        for (size_t gapIndex = 0; gapIndex < normalizedGaps.size();
             ++gapIndex) {
          const PartitionEdge &gap = normalizedGaps[gapIndex];
          if (!gap.closure || !gap.closure->source.IsComplete() ||
              !stateGapOwnerMatchesStructureIdentity(gap, *interval)) {
            continue;
          }

          const OwnerSourceRange &owned = gap.closure->source;
          if (!deps_.pathIdentity.PathsEqual(owned.path,
                                             interval->sourcePath) ||
              owned.includeId != interval->ownerIncludeId)
            continue;

          // When present, the exact model id relates a producer state owner to
          // one lexical interval.  Source coordinates must additionally be
          // nested in one direction: the producer may name a narrow site inside
          // the complete logical directive line, or a conditional-group owner
          // may enclose several of its bound control lines.  Partial crossing
          // is neither relation and is rejected.
          const bool ownerContainsInterval =
              owned.begin <= interval->begin && interval->end <= owned.end;
          const bool intervalContainsOwner =
              interval->begin <= owned.begin && owned.end <= interval->end;
          if (!ownerContainsInterval && !intervalContainsOwner)
            continue;

          if (matchingGapIndex != std::numeric_limits<size_t>::max()) {
            if (reason) {
              *reason = "protected preprocessing interval has ambiguous "
                        "state-gap ownership";
            }
            return std::nullopt;
          }
          matchingGapIndex = gapIndex;
        }

        if (matchingGapIndex == std::numeric_limits<size_t>::max()) {
          // Some producer owners are intentionally broader than one lexical
          // directive.  In particular, a conditional group spans from its
          // opening control through the matching `#endif`, while a structural
          // deletion may preserve only one control line between two token
          // segments.  Unmodeled non-conditional directives can likewise lack
          // a producer owner entirely.  The exact lexical census is sufficient
          // to instantiate a proof-only physical owner because this edge will
          // be authorized only as `PreservedInPlace`, never reconstructed.
          Owner exactOwner =
              ownerForPreservedStructureInterval(*interval);

          OwnerSourceRange exactSource = OwnerSourceRange::From(
              interval->sourcePath, interval->begin, interval->end,
              interval->ownerIncludeId);
          PartitionEdge exactGap = makeStateGapEdge(
              std::move(exactOwner), std::move(exactSource), aBoundary,
              bBoundary);
          if (!exactGap.closure || !exactGap.closure->IsComplete()) {
            if (reason)
              *reason = "exact protected state-gap owner has no closure";
            return std::nullopt;
          }
          matchingGapIndex = normalizedGaps.size();
          normalizedGaps.push_back(std::move(exactGap));
        }

        PartitionEdge &matchingGap = normalizedGaps[matchingGapIndex];
        const OwnerSourceRange &owned = matchingGap.closure->source;
        if (owned.begin != interval->begin || owned.end != interval->end) {
          // The lexical interval is the physical preservation authority.  A
          // producer site may be narrower (for example, just the macro name) or
          // broader (for example, the complete conditional group).  Once the
          // model identity is uniquely bound, normalize the proof-only edge to
          // the exact logical directive line in either case.  This prevents a
          // broad conditional owner from swallowing token-bearing arm bytes and
          // prevents a narrow producer site from leaving directive bytes
          // unaccounted for as trivia.
          OwnerSourceRange exactSource = OwnerSourceRange::From(
              interval->sourcePath, interval->begin, interval->end,
              interval->ownerIncludeId);
          matchingGap.closure =
              deps_.ownerStateProof.AttachCanonicalStateSummary(
                  OwnerClosure::From(matchingGap.closure->owner,
                                     std::move(exactSource),
                                     matchingGap.closure->aTokens,
                                     matchingGap.closure->bTokens));
        }
        matchingGap.protectedPreprocessingStructure = true;
        matchingGap.gapDisposition =
            StructuralGapDisposition::PreservedInPlace;
        matchingGap.protectedStructureIdentityRecorded = true;
        matchingGap.protectedStructureKind =
            structuralProtectedStructureKind(interval->kind);
        matchingGap.producerIdentityKind =
            structuralProducerIdentityKind(interval->modelKind);
        matchingGap.producerItemId = interval->modelItemId;
        if (interval->modelKind ==
                PreprocessingStructureModelKind::None &&
            matchingGap.closure->owner.IsMacroDirective() &&
            matchingGap.closure->owner.macroDirectiveId) {
          // The exact source-contained fallback above recovered a unique
          // producer macro identity even though the shared index could not bind
          // normalized producer text to the physical line.  Persist that typed
          // identity in the generalized witness rather than leaving the
          // preserved directive as an anonymous lexical interval.
          matchingGap.producerIdentityKind =
              StructuralProducerIdentityKind::MacroDirective;
          matchingGap.producerItemId =
              matchingGap.closure->owner.macroDirectiveId;
        }
        matchingGap.producerConditionalGroupId = interval->conditionalGroupId;
        matchingGap.producerConditionalArmId = interval->conditionalArmId;
      }

      llvm::sort(normalizedGaps,
                 [](const PartitionEdge &lhs, const PartitionEdge &rhs) {
                   const OwnerSourceRange &l = lhs.closure->source;
                   const OwnerSourceRange &r = rhs.closure->source;
                   if (l.path != r.path)
                     return l.path < r.path;
                   if (l.includeId != r.includeId) {
                     return l.includeId.value_or(
                                std::numeric_limits<uint64_t>::max()) <
                            r.includeId.value_or(
                                std::numeric_limits<uint64_t>::max());
                   }
                   if (l.begin != r.begin)
                     return l.begin < r.begin;
                   if (l.end != r.end)
                     return l.end < r.end;
                   return lhs.closure->owner.kind < rhs.closure->owner.kind;
                 });

      gaps.assign(normalizedGaps.begin(), normalizedGaps.end());
      return true;
    };

    auto buildPhysicalSourceRunPlan =
        [&](const diffutils::Hunk &h) -> std::optional<PhysicalSourceRunPlan> {
      if (h.aEnd <= h.aStart)
        return std::nullopt;

      const auto &tokmapByPP = deps_.model.GetTokmapByPP();
      PhysicalSourceRunPlan plan;
      std::optional<SourceOwnerIdentity> physicalOwner;
      std::string physicalPath;
      const RefoldPreprocessingStructureIndex *structureIndex = nullptr;
      uint64_t previousBegin = 0;
      uint64_t previousEnd = 0;
      bool havePrevious = false;

      PhysicalSourceRun currentRun;

      for (uint64_t pp = h.aStart; pp < h.aEnd; ++pp) {
        if (duplicateTokmapPP.count(pp) != 0)
          return std::nullopt;

        auto entryIt = tokmapByPP.find(pp);
        if (entryIt == tokmapByPP.end())
          return std::nullopt;

        const RefoldModel::TokMapEntry &entry = entryIt->second;
        if (entry.pp != pp || entry.file.empty() || entry.b >= entry.e)
          return std::nullopt;

        std::optional<SourceOwnerIdentity> tokenOwner =
            resolveSourceOwnerIdentity(entry.file, entry.b, entry.e);
        if (!tokenOwner) {
          // Older maps can omit segment facts for the top-level TU.  That
          // omission is unambiguous only for the exact TU source; a header path
          // without an include occurrence cannot participate in this direct
          // physical-source theorem.
          if (!deps_.pathIdentity.PathsEqual(entry.file, deps_.tuPath))
            return std::nullopt;
          tokenOwner = SourceOwnerIdentity{};
        }

        OwnerSourceRange tokenSource = OwnerSourceRange::From(
            entry.file, entry.b, entry.e, tokenOwner->includeId);
        const RefoldPreprocessingStructureIndex *tokenStructureIndex =
            getStructureIndexForSource(tokenSource);
        if (!tokenStructureIndex ||
            !tokenStructureIndex->IsDirectTUProtectionCensusComplete() ||
            !tokenStructureIndex->IsExactTokenSpellingInterval(entry.b,
                                                                entry.e)) {
          return std::nullopt;
        }

        if (!physicalOwner) {
          physicalOwner = tokenOwner;
          physicalPath = entry.file;
          structureIndex = tokenStructureIndex;
          currentRun.aStart = pp;
          currentRun.aEnd = pp + 1;
          currentRun.source = OwnerSourceRange::From(
              entry.file, entry.b, entry.e, tokenOwner->includeId);
          previousBegin = entry.b;
          previousEnd = entry.e;
          havePrevious = true;
          continue;
        }

        // A direct physical run plan is one ordered source surface. Changes of
        // file or include occurrence belong to the historical mixed-owner
        // theorem and are deliberately not coerced into a byte-distance
        // relation here. Conditional-arm ownership may change only across a
        // protected control directive, which is determined below.
        if (!deps_.pathIdentity.PathsEqual(physicalPath, entry.file) ||
            physicalOwner->includeId != tokenOwner->includeId ||
            structureIndex != tokenStructureIndex) {
          return std::nullopt;
        }

        // Distinct A tokens must map to strictly ordered, nonoverlapping raw
        // source spellings.  Equality is a repeated physical mapping; a lower
        // offset is nonmonotone.  Either condition makes the direct structural
        // path unavailable rather than selecting one spelling heuristically.
        if (!havePrevious || entry.b < previousBegin || entry.b < previousEnd)
          return std::nullopt;

        bool crossesProtectedStructure = false;
        bool crossesConditionalControl = false;
        if (previousEnd < entry.b) {
          // Canonical run construction and later state-gap discharge must use
          // the same physical byte-cover theorem.  Treat every indexed
          // preprocessing interval as an opaque preserved source piece and
          // require exact lexer trivia between those pieces.  This removes the
          // former second interval sorter/cursor proof from the tiler.
          std::optional<SourceGapProofResult> gapProof =
              proveSourceGapWithIndexedStructureAndTrivia(
                  *structureIndex, previousEnd, entry.b);
          if (!gapProof)
            return std::nullopt;

          for (const PreprocessingStructureInterval *interval :
               gapProof->protectedIntervals) {
            if (!interval ||
                interval->ownerIncludeId != tokenOwner->includeId) {
              return std::nullopt;
            }
            switch (interval->kind) {
            case PreprocessingStructureKind::ConditionalIf:
            case PreprocessingStructureKind::ConditionalIfdef:
            case PreprocessingStructureKind::ConditionalIfndef:
            case PreprocessingStructureKind::ConditionalElif:
            case PreprocessingStructureKind::ConditionalElifdef:
            case PreprocessingStructureKind::ConditionalElifndef:
            case PreprocessingStructureKind::ConditionalElse:
            case PreprocessingStructureKind::ConditionalEndif:
              crossesConditionalControl = true;
              break;
            default:
              break;
            }
          }
          crossesProtectedStructure =
              !gapProof->protectedIntervals.empty();
        }

        if (crossesProtectedStructure) {
          // A segment-owner arm transition is meaningful only when the exact
          // physical gap contains a conditional-control directive.  Another
          // protected directive (for example, `#define`) still terminates a
          // physical run, but it cannot by itself prove movement into or out of
          // a conditional arm.  Reject inconsistent producer segmentation
          // rather than treating any protected bytes as generic arm authority.
          if (physicalOwner->condArmId != tokenOwner->condArmId &&
              !crossesConditionalControl) {
            return std::nullopt;
          }
          if (currentRun.aEnd <= currentRun.aStart ||
              !currentRun.source.IsComplete() ||
              currentRun.source.end <= currentRun.source.begin) {
            return std::nullopt;
          }
          plan.runs.push_back(currentRun);
          plan.protectedGaps.push_back(OwnerSourceRange::From(
              entry.file, previousEnd, entry.b, tokenOwner->includeId));
          currentRun.aStart = pp;
          currentRun.aEnd = pp + 1;
          currentRun.source = OwnerSourceRange::From(
              entry.file, entry.b, entry.e, tokenOwner->includeId);
        } else {
          if (physicalOwner->condArmId != tokenOwner->condArmId)
            return std::nullopt;
          currentRun.aEnd = pp + 1;
          currentRun.source.end = entry.e;
        }

        physicalOwner->condArmId = tokenOwner->condArmId;
        previousBegin = entry.b;
        previousEnd = entry.e;
      }

      if (!havePrevious || currentRun.aEnd <= currentRun.aStart ||
          !currentRun.source.IsComplete() ||
          currentRun.source.end <= currentRun.source.begin) {
        return std::nullopt;
      }
      plan.runs.push_back(currentRun);
      if (plan.runs.empty() ||
          plan.protectedGaps.size() + 1 != plan.runs.size()) {
        return std::nullopt;
      }

      return plan;
    };

    auto stateGapComposesSafely = [&](const PartitionEdge &gap,
                                      std::string *reason) -> bool {
      if (!gap.IsStateGap() || !gap.closure) {
        if (reason)
          *reason = "state-gap edge has no closure";
        return false;
      }

      // A preserved-in-place gap is discharged by physical source identity,
      // not by replaying or interpreting its state transition.  Exact lexical
      // binding, complete byte coverage, and the later source-order/disjoint
      // proof guarantee that even an implementation-defined pragma or a
      // conditional transition with incomplete value-level modeling executes
      // from the same bytes in the same position.  Requiring a fully modeled
      // state delta here would conflate preservation with reconstruction and
      // defeat the purpose of the disposition.
      if (gap.gapDisposition ==
          StructuralGapDisposition::PreservedInPlace) {
        return true;
      }

      const OwnerStateDelta &summary = gap.closure->stateOut;
      if (deps_.ownerStateProof.OwnerStateDeltaHasUnmodeledState(summary)) {
        if (reason)
          *reason = "state gap contains unmodeled producer state";
        return false;
      }
      if (deps_.ownerStateProof.OwnerStateDeltaHasUnknownPragmaState(summary)) {
        if (reason)
          *reason = "state gap contains unknown pragma state";
        return false;
      }

      // composes state effects across the reconstructed path as a whole. This
      // local check therefore rejects only state-gap effects that cannot
      // participate in any ordered composition proof. A modeled state mutation
      // followed by a later observer is not rejected here merely because it is
      // visible after the gap: the later token/state edge may be part of the
      // same widened mixed-owner tiling closure.
      return true;
    };

    auto appendUniqueStateComponent =
        [](SmallVectorImpl<OwnerStateComponent> &out,
           OwnerStateComponent component) {
          if (component == OwnerStateComponent::Unknown)
            return;
          if (!llvm::is_contained(out, component))
            out.push_back(component);
        };

    auto stateComponentsObservedBySummary = [&](const OwnerStateDelta
                                                    &summary) {
      // Mixed-owner proof consumes the canonical theorem-facing delta directly.
      // composition is tied to precise Entry/Observes/Mutates/Exit facts and
      // explicit missing-fact markers, not a flat owner-state projection.
      const OwnerStateDelta theoremDelta = summary;
      const StateObservations &observations = theoremDelta.observes;
      SmallVector<OwnerStateComponent, 8> components;
      if (observations.HasMacroRequirements() ||
          observations.HasMacroExpansionObservations())
        appendUniqueStateComponent(components, OwnerStateComponent::MacroState);
      if (observations.HasDefinedOperatorObservations())
        appendUniqueStateComponent(components,
                                   OwnerStateComponent::DefinedOperator);
      if (observations.HasConditionalMacroObservations() ||
          observations.HasConditionalStateEvents())
        appendUniqueStateComponent(components,
                                   OwnerStateComponent::ConditionalState);
      if (llvm::any_of(observations.builtinLocationObservations,
                       [](const BuiltinLocationObservation &obs) {
                         return obs.kind ==
                                BuiltinLocationObservationKind::LineState;
                       }))
        appendUniqueStateComponent(components, OwnerStateComponent::LineNumber);
      if (llvm::any_of(observations.builtinLocationObservations,
                       [](const BuiltinLocationObservation &obs) {
                         return obs.kind ==
                                BuiltinLocationObservationKind::FileState;
                       }))
        appendUniqueStateComponent(components, OwnerStateComponent::FileState);
      if (llvm::any_of(observations.builtinLocationObservations,
                       [](const BuiltinLocationObservation &obs) {
                         return obs.kind ==
                                BuiltinLocationObservationKind::FileNameState;
                       }))
        appendUniqueStateComponent(components, OwnerStateComponent::FileName);
      if (observations.HasCounterEvents())
        appendUniqueStateComponent(components, OwnerStateComponent::Counter);
      if (observations.HasPragmaStateEvents() ||
          observations.HasTheoremUnknownPragmaState())
        appendUniqueStateComponent(components,
                                   OwnerStateComponent::PragmaState);
      if (observations.HasIncludeGuardStateEvents())
        appendUniqueStateComponent(components,
                                   OwnerStateComponent::IncludeGuardState);
      if (observations.HasIncludeStateEvents())
        appendUniqueStateComponent(components,
                                   OwnerStateComponent::IncludeState);
      for (const MissingStateFact &fact : observations.missingStateFacts) {
        if (fact.kind == MissingStateFactKind::MissingLineControlFacts) {
          appendUniqueStateComponent(components,
                                     OwnerStateComponent::LineNumber);
          appendUniqueStateComponent(components,
                                     OwnerStateComponent::FileState);
          appendUniqueStateComponent(components, OwnerStateComponent::FileName);
          continue;
        }
        appendUniqueStateComponent(
            components,
            deps_.ownerStateProof.StateComponentForMissingStateFact(fact.kind));
      }
      if (observations.HasMissingFactKind(
              MissingStateFactKind::MissingOwnerOrderingFacts))
        appendUniqueStateComponent(components,
                                   OwnerStateComponent::UnmodeledState);
      return components;
    };

    auto mergedEdgeStateSummary = [&](const PartitionEdge &edge) {
      OwnerStateDelta summary;
      if (edge.closure) {
        summary.MergeFrom(edge.closure->stateIn);
        summary.MergeFrom(edge.closure->stateOut);
      } else {
        OwnerStateFacts missingFacts;
        missingFacts.AddMissingStateFact(
            MissingStateFactKind::MissingOwnerOrderingFacts,
            "partition edge has no owner closure");
        summary = deps_.ownerStateProof.BuildTheoremStateDelta(
            missingFacts, OwnerStateDelta());
      }
      return summary;
    };

    auto mixedOwnerTilingStateSummariesCompose =
        [&](const diffutils::Hunk &h, ArrayRef<PartitionEdge> path,
            std::string *reason) -> bool {
      // treats the reconstructed token/state-gap path as one ordered
      // state-composition proof.  Earlier checks prove that each individual
      // token segment has a closure and that each zero-token source gap is
      // byte-covered.  This pass adds the cross-edge theorem: any state
      // observed after an earlier edge mutation is observed inside the same
      // widened mixed-owner closure, and any unmodeled state-gap transition is
      // rejected instead of being hidden between token edges.
      SmallVector<OwnerStateComponent, 8> activeMutations;
      SmallVector<OwnerStateComponent, 8> internallyObservedMutations;

      for (const PartitionEdge &edge : path) {
        if (!edge.closure || !edge.closure->IsComplete()) {
          if (reason)
            *reason = "mixed-owner state composition saw incomplete closure";
          return false;
        }

        if (edge.IsStateGap() &&
            edge.gapDisposition ==
                StructuralGapDisposition::PreservedInPlace) {
          // The state transition remains in the physical source stream.  Its
          // exact ordering is proved separately from value-level state
          // composition, so do not reinterpret an unknown or
          // implementation-defined transition as if the tiler materialized it.
          continue;
        }

        const OwnerStateDelta summary = mergedEdgeStateSummary(edge);
        if (edge.IsStateGap() &&
            deps_.ownerStateProof.OwnerStateDeltaHasUnmodeledState(summary)) {
          if (reason)
            *reason = "zero-token state gap contains unmodeled state";
          return false;
        }
        if (edge.IsStateGap() &&
            deps_.ownerStateProof.OwnerStateDeltaHasUnknownPragmaState(
                summary)) {
          if (reason)
            *reason = "zero-token state gap contains unknown pragma state";
          return false;
        }

        for (OwnerStateComponent observed :
             stateComponentsObservedBySummary(summary)) {
          if (llvm::is_contained(activeMutations, observed))
            appendUniqueStateComponent(internallyObservedMutations, observed);
        }

        for (OwnerStateComponent mutated :
             deps_.ownerStateProof.StateComponentsMutatedByDelta(summary)) {
          appendUniqueStateComponent(activeMutations, mutated);
        }
      }

      // The composition pass is intentionally not a value-level preprocessor
      // interpreter.  It proves only the ordering obligation that can discharge
      // locally: later observations of state already mutated by an earlier edge
      // are inside the same mixed-owner closure.  Components not internally
      // observed remain ordinary preserved-context obligations for the owner-
      // specific realization proofs that consume the emitted token segments.
      // Keep the trace explicit so future deletion/audit work can see which
      // state dependencies were closed by the tiling itself.
      if (!internallyObservedMutations.empty()) {
        SmallVector<std::string, 8> names;
        for (OwnerStateComponent component : internallyObservedMutations)
          names.push_back(toString(component).str());
      }

      return true;
    };

    auto buildStructuralTilingWitness =
        [&](const diffutils::Hunk &h, const StructuralPartition &partition,
            uint64_t witnessId) -> MixedOwnerTilingWitness {
      ArrayRef<PartitionEdge> path = partition.edges;
      MixedOwnerTilingWitness witness;
      witness.witnessId = witnessId;
      witness.reason = partition.reason;
      witness.originalAStart = h.aStart;
      witness.originalAEnd = h.aEnd;
      witness.originalBStart = h.bStart;
      witness.originalBEnd = h.bEnd;
      // Persist the explicit partition theorem instead of asking consumers to
      // infer uniqueness from the construction algorithm.
      witness.uniquePartition = partition.uniquePartition;
      witness.physicalSourceRunCount = partition.physicalSourceRunCount;
      witness.physicalSourceRunsProven = partition.physicalSourceRunsProven;
      witness.uniqueMinimumFragmentPartition =
          partition.uniqueMinimumFragmentPartition;
      witness.uniqueBoundaryProjectionProven =
          partition.uniqueBoundaryProjectionProven;
      witness.boundaryProjectionCount =
          static_cast<uint32_t>(partition.boundaryProjections.size());
      witness.boundaryProjections.assign(partition.boundaryProjections.begin(),
                                         partition.boundaryProjections.end());
      witness.sharedEmptyBEnvelopeProven =
          partition.sharedEmptyBEnvelopeProven;
      witness.sharedEmptyBBoundary = partition.sharedEmptyBBoundary;
      witness.preservedStateChainComposed =
          partition.preservedStateChainComposed;
      witness.stateSummariesComposed = true;
      witness.ownerBoundariesComposed = true;
      witness.targetTokenStreamComposed = true;
      witness.compositionEdgesProven = true;
      witness.sourceByteCoverComplete = partition.sourceByteCoverComplete;
      witness.preservedGapSourceOrderProven =
          partition.preservedGapSourceOrderProven;
      witness.preservedGapsDisjointFromTokenSegments =
          partition.preservedGapsDisjointFromTokenSegments;
      bool hasPreservedInPlaceEdge = false;
      for (const PartitionEdge &edge : path) {
        if (edge.IsStateGap() &&
            edge.gapDisposition ==
                StructuralGapDisposition::PreservedInPlace) {
          hasPreservedInPlaceEdge = true;
          break;
        }
      }
      witness.stateTransitionsComposed =
          witness.stateSummariesComposed &&
          (!hasPreservedInPlaceEdge ||
           (partition.preservedGapSourceOrderProven &&
            partition.preservedGapsDisjointFromTokenSegments));
      // The durable edge set is the initial emitted edit topology. Patch 2.4's
      // final assembler audit independently rechecks the same obligation after
      // normalization, so a later widened closure cannot consume a preserved
      // gap merely because the original carriers disappeared.
      witness.preservedGapsDisjointFromEdits =
          !hasPreservedInPlaceEdge ||
          partition.preservedGapsDisjointFromTokenSegments;
      witness.globalTargetPPTokenSignature =
          llvm::formatv("B=[{0},{1}):hash={2}", h.bStart, h.bEnd,
                        RefoldWitnessTrace::FormatWitnessTraceHash(
                            deps_.sourceMapper.SliceBSource(h.bStart, h.bEnd)))
              .str();
      witness.edges.reserve(path.size());

      auto formatOptionalId = [](std::optional<uint64_t> value) {
        return value ? llvm::formatv("{0}", *value).str() : std::string("none");
      };

      auto formatOwnerSignature = [&](const Owner &owner) {
        return llvm::formatv(
                   "kind={0}:include={1}:macro={2}:macro_directive={3}:"
                   "line={4}:pragma={5}:cond_group={6}:cond_arm={7}",
                   owner.kind, formatOptionalId(owner.includeId),
                   formatOptionalId(owner.macroInvocationId),
                   formatOptionalId(owner.macroDirectiveId),
                   formatOptionalId(owner.lineControlId),
                   formatOptionalId(owner.pragmaId),
                   formatOptionalId(owner.condGroupId),
                   formatOptionalId(owner.condArmId))
            .str();
      };

      auto formatSourceSignature = [&](const OwnerSourceRange &source) {
        return llvm::formatv("path={0}:range=[{1},{2}):include={3}",
                             source.path, source.begin, source.end,
                             formatOptionalId(source.includeId))
            .str();
      };

      auto formatRealizerSignature = [&](const HunkRealizer &realizer) {
        StringRef kind = "Unknown";
        switch (realizer.kind) {
        case HunkRealizerKind::Unknown:
          kind = "Unknown";
          break;
        case HunkRealizerKind::TU:
          kind = "TU";
          break;
        case HunkRealizerKind::Include:
          kind = "Include";
          break;
        case HunkRealizerKind::Macro:
          kind = "Macro";
          break;
        }
        return llvm::formatv("realizer={0}:{1}", kind, realizer.id).str();
      };

      auto formatBTokenSignature = [&](uint64_t begin, uint64_t end) {
        return llvm::formatv("B=[{0},{1}):hash={2}", begin, end,
                             RefoldWitnessTrace::FormatWitnessTraceHash(
                                 deps_.sourceMapper.SliceBSource(begin, end)))
            .str();
      };

      auto buildOwnerTransitionProof =
          [](const PartitionEdge &edge) -> StateTransitionProof {
        StateTransitionProof proof;
        if (!edge.closure)
          return proof;

        // `PreservedInPlace` proves the physical source transition by retaining
        // its exact bytes and ordering; the tiler neither replays nor models
        // the transition's value. Do not copy a canonical owner summary into
        // this witness, because a producer owner can be wider than the exact
        // lexical
        // gap (notably one conditional group spanning several control lines).
        // Carrying that broader delta would manufacture state obligations that
        // the preserved source interval itself does not consume.
        if (edge.IsStateGap() &&
            edge.gapDisposition ==
                StructuralGapDisposition::PreservedInPlace) {
          return proof;
        }

        proof.before = edge.closure->stateIn;
        proof.after = edge.closure->stateOut;
        return proof;
      };

      uint32_t segmentIndex = 0;
      std::set<HunkRealizer> distinctRealizers;
      std::string compositionStorage;
      llvm::raw_string_ostream compositionOS(compositionStorage);
      compositionOS << "tiling=" << witnessId
                    << ":reason=" << toString(partition.reason) << ":A=["
                    << h.aStart << ',' << h.aEnd << "):B=[" << h.bStart << ','
                    << h.bEnd << ")"
                    << ":unique_partition="
                    << (partition.uniquePartition ? 1 : 0)
                    << ":state_transitions="
                    << (witness.stateTransitionsComposed ? 1 : 0)
                    << ":disjoint_from_edits="
                    << (witness.preservedGapsDisjointFromEdits ? 1 : 0)
                    << ":source_cover="
                    << (partition.sourceByteCoverComplete ? 1 : 0)
                    << ":physical_runs="
                    << partition.physicalSourceRunCount
                    << ":physical_runs_proven="
                    << (partition.physicalSourceRunsProven ? 1 : 0)
                    << ":minimum_fragments="
                    << (partition.uniqueMinimumFragmentPartition ? 1 : 0)
                    << ":unique_boundary_projection="
                    << (partition.uniqueBoundaryProjectionProven ? 1 : 0)
                    << ":boundary_projection_count="
                    << partition.boundaryProjections.size()
                    << ":shared_empty_b="
                    << (partition.sharedEmptyBEnvelopeProven ? 1 : 0)
                    << ":shared_empty_b_boundary="
                    << partition.sharedEmptyBBoundary
                    << ":preserved_state_chain="
                    << (partition.preservedStateChainComposed ? 1 : 0)
                    << ":preserved_gap_order="
                    << (partition.preservedGapSourceOrderProven ? 1 : 0)
                    << ":preserved_gap_disjoint="
                    << (partition.preservedGapsDisjointFromTokenSegments ? 1
                                                                         : 0);
      for (const StructuralBoundaryProjectionWitness &projection :
           partition.boundaryProjections) {
        compositionOS << ":boundary{A=" << projection.aTokenBoundary
                      << ":lowerB=" << projection.lowerBTokenBoundary
                      << ":upperB=" << projection.upperBTokenBoundary
                      << ":B=" << projection.bTokenBoundary
                      << ":unique="
                      << (projection.uniqueProjection ? 1 : 0) << '}';
      }
      for (const PartitionEdge &edge : path) {
        const MixedOwnerTilingEdgeKind edgeKind =
            edge.IsStateGap() ? MixedOwnerTilingEdgeKind::StateGap
                              : MixedOwnerTilingEdgeKind::TokenSegment;

        MixedOwnerTilingSegmentWitness segmentWitness;
        segmentWitness.parentTilingWitnessId = witnessId;
        segmentWitness.segmentIndex = segmentIndex;
        segmentWitness.sourceOrderPosition = segmentIndex;
        segmentWitness.kind = edgeKind;
        segmentWitness.aStart = edge.aStart;
        segmentWitness.aEnd = edge.aEnd;
        segmentWitness.bStart = edge.bStart;
        segmentWitness.bEnd = edge.bEnd;
        segmentWitness.zeroTokenStateGap = edge.IsStateGap();
        segmentWitness.gapDisposition = edge.gapDisposition;
        segmentWitness.protectedPreprocessingStructure =
            edge.protectedPreprocessingStructure;
        segmentWitness.protectedStructureIdentityRecorded =
            edge.protectedStructureIdentityRecorded;
        segmentWitness.protectedStructureKind = edge.protectedStructureKind;
        segmentWitness.producerIdentityKind = edge.producerIdentityKind;
        segmentWitness.producerItemId = edge.producerItemId;
        segmentWitness.producerConditionalGroupId =
            edge.producerConditionalGroupId;
        segmentWitness.producerConditionalArmId =
            edge.producerConditionalArmId;
        segmentWitness.allowEmptyBEnvelope = edge.allowEmptyBEnvelope;
        segmentWitness.sourceBytesPreservedUnchanged =
            edge.IsStateGap() &&
            edge.gapDisposition == StructuralGapDisposition::PreservedInPlace;
        segmentWitness.canonicalStateTransition =
            buildOwnerTransitionProof(edge);
        segmentWitness.targetPPTokenSignature =
            formatBTokenSignature(edge.bStart, edge.bEnd);
        if (edge.closure) {
          segmentWitness.ownerClosureComplete = edge.closure->IsComplete();
          segmentWitness.ownerIdentityKnown = true;
          segmentWitness.ownerIdentity = edge.closure->owner;
          segmentWitness.sourceByteRangeKnown = true;
          segmentWitness.sourcePath = edge.closure->source.path;
          segmentWitness.sourceIncludeId = edge.closure->source.includeId;
          segmentWitness.sourceBegin = edge.closure->source.begin;
          segmentWitness.sourceEnd = edge.closure->source.end;
          segmentWitness.ownerSignature =
              formatOwnerSignature(edge.closure->owner);
          segmentWitness.sourceSignature =
              formatSourceSignature(edge.closure->source);

          const bool structuralReason =
              partition.reason ==
                  StructuralTilingReason::PreservedPreprocessingStructure ||
              partition.reason == StructuralTilingReason::
                                      MixedRealizersAndPreservedStructure;
          if (edge.IsTokenSegment() && structuralReason &&
              partition.sourceByteCoverComplete) {
            const RefoldPreprocessingStructureIndex *structureIndex =
                getStructureIndexForSource(edge.closure->source);
            segmentWitness.protectedStructurePreservedOutsideSegment =
                structureIndex &&
                structureIndex->IsDirectTUProtectionCensusComplete() &&
                structureIndex
                    ->FindOverlapping(edge.closure->source.begin,
                                      edge.closure->source.end)
                    .empty();
          }
        }
        segmentWitness.producerPathSignature =
            edge.IsStateGap()
                ? llvm::formatv("state_gap:{0}", segmentWitness.ownerSignature)
                      .str()
                : llvm::formatv("{0}:owner={1}",
                                formatRealizerSignature(edge.realizer),
                                segmentWitness.ownerSignature)
                      .str();

        compositionOS << ";seg" << segmentIndex
                      << "{kind=" << toString(edgeKind) << ":A=[" << edge.aStart
                      << ',' << edge.aEnd << "):B=[" << edge.bStart << ','
                      << edge.bEnd
                      << "):empty_b=" << (edge.allowEmptyBEnvelope ? 1 : 0)
                      << ":gap_disposition="
                      << toString(edge.gapDisposition)
                      << ":protected_gap="
                      << (edge.protectedPreprocessingStructure ? 1 : 0)
                      << ":structure_identity="
                      << (segmentWitness.protectedStructureIdentityRecorded ? 1
                                                                            : 0)
                      << ":structure_kind="
                      << toString(segmentWitness.protectedStructureKind)
                      << ":producer_kind="
                      << toString(segmentWitness.producerIdentityKind)
                      << ":producer_item="
                      << formatOptionalId(segmentWitness.producerItemId)
                      << ":producer_cond_group="
                      << formatOptionalId(
                             segmentWitness.producerConditionalGroupId)
                      << ":producer_cond_arm="
                      << formatOptionalId(
                             segmentWitness.producerConditionalArmId)
                      << ":bytes_unchanged="
                      << (segmentWitness.sourceBytesPreservedUnchanged ? 1 : 0)
                      << ":protected_outside="
                      << (segmentWitness
                                  .protectedStructurePreservedOutsideSegment
                              ? 1
                              : 0)
                      << ":owner=" << segmentWitness.ownerSignature
                      << ":source=" << segmentWitness.sourceSignature
                      << ":producer=" << segmentWitness.producerPathSignature
                      << '}';

        if (edge.IsStateGap()) {
          ++witness.stateGapCount;
          if (edge.gapDisposition ==
              StructuralGapDisposition::PreservedInPlace) {
            ++witness.preservedInPlaceGapCount;
          }
          if (edge.protectedPreprocessingStructure)
            ++witness.protectedStructureGapCount;
        } else {
          ++witness.tokenSegmentCount;
          distinctRealizers.insert(edge.realizer);
        }
        witness.edges.push_back(std::move(segmentWitness));
        ++segmentIndex;
      }
      witness.distinctRealizerCount =
          static_cast<uint32_t>(distinctRealizers.size());
      compositionOS.flush();
      witness.globalCompositionSignature =
          RefoldWitnessTrace::FormatWitnessTraceHash(compositionStorage);
      return witness;
    };

    auto buildClosedStateGapTransition = [&](const PartitionEdge *prev,
                                             const PartitionEdge &cur)
        -> std::optional<ClosedStateGapTransition> {
      ClosedStateGapTransition transition;

      // The first token segment has no predecessor, and token owners from
      // incomparable source sites do not create a proof obligation here.  They
      // are still checked later by the ordinary owner-realization proofs.
      if (!prev)
        return transition;
      if (!prev->IsTokenSegment() || !cur.IsTokenSegment() || !prev->closure ||
          !cur.closure)
        return std::nullopt;
      if (!sourceSitesComparable(prev->closure->source, cur.closure->source) ||
          cur.closure->source.begin < prev->closure->source.end)
        return transition;

      OwnerSourceRange gapSource = OwnerSourceRange::From(
          prev->closure->source.path, prev->closure->source.end,
          cur.closure->source.begin, prev->closure->source.includeId);
      SmallVector<PartitionEdge, 8> collected =
          collectZeroTokenStateGaps(gapSource, prev->aEnd, prev->bEnd);

      // Exact lexical intervals are bound before byte coverage so a producer
      // site that names only the directive keyword/name cannot leave the rest
      // of the logical directive line masquerading as trivia.  Keep this as a
      // second candidate graph: if exact binding is unavailable, the historical
      // mixed-realizer graph must retain precisely its old applicability.
      SmallVector<PartitionEdge, 8> exactCollected = collected;
      std::string structureReason;
      std::optional<bool> protectedBinding =
          bindProtectedStructureIntervalsToStateGaps(
              gapSource, prev->aEnd, prev->bEnd, exactCollected,
              &structureReason);

      auto validateGapGraph =
          [&](ArrayRef<PartitionEdge> candidate,
              SmallVectorImpl<PartitionEdge> &validated) {
            std::string coverageReason;
            if (!sourceGapIsFullyCovered(gapSource, candidate,
                                         &coverageReason)) {
              return false;
            }

            uint64_t lastGapEnd = gapSource.begin;
            for (const PartitionEdge &gap : candidate) {
              if (!gap.closure || gap.closure->source.begin < lastGapEnd)
                return false;
              lastGapEnd = gap.closure->source.end;

              std::string stateReason;
              if (!stateGapComposesSafely(gap, &stateReason))
                return false;
              validated.push_back(gap);
            }
            return true;
          };

      // Prefer the exact lexical graph when it proves protected structure.  It
      // is the only graph that may authorize same-realizer adjacency.  If its
      // widened full-line ranges expose an overlap or another inconsistency,
      // retry the unmodified legacy graph solely for mixed-realizer behavior.
      if (protectedBinding.value_or(false) &&
          validateGapGraph(exactCollected, transition.gaps)) {
        // The validated graph byte-covers the complete physical interval
        // between the neighboring token carriers.  Every proof-only edge in
        // that interval therefore remains untouched when the token segments
        // are emitted separately.  Classify even non-directive zero-token
        // owners this way so the entire ordered state chain has one explicit
        // physical disposition rather than mixing proved and implicit gaps.
        for (PartitionEdge &gap : transition.gaps) {
          gap.gapDisposition =
              StructuralGapDisposition::PreservedInPlace;
        }
        transition.hasProtectedPreprocessingStructure = true;
        return transition;
      }

      transition.gaps.clear();
      if (!validateGapGraph(collected, transition.gaps))
        return std::nullopt;

      // The legacy graph has the same physical lowering as the exact lexical
      // graph: only the neighboring token edges become hunks, so every
      // byte-complete zero-token owner remains in the original source.  Record
      // that fact explicitly even when no protected directive is present.  It
      // does not authorize same-realizer splitting; that still requires the
      // exact protected-structure bit below.
      for (PartitionEdge &gap : transition.gaps) {
        gap.gapDisposition =
            StructuralGapDisposition::PreservedInPlace;
      }

      transition.hasProtectedPreprocessingStructure = false;
      return transition;
    };

    auto structuralPartitionSourceCoverComplete =
        [&](ArrayRef<PartitionEdge> path) {
          const PartitionEdge *previousToken = nullptr;
          SmallVector<PartitionEdge, 4> gapsBetweenTokens;
          bool sawToken = false;

          for (const PartitionEdge &edge : path) {
            if (edge.IsStateGap()) {
              if (!previousToken)
                return false;
              gapsBetweenTokens.push_back(edge);
              continue;
            }

            if (!edge.closure || !edge.closure->source.IsComplete())
              return false;
            sawToken = true;
            if (!previousToken) {
              previousToken = &edge;
              continue;
            }

            if (!previousToken->closure ||
                !sourceSitesComparable(previousToken->closure->source,
                                       edge.closure->source) ||
                edge.closure->source.begin <
                    previousToken->closure->source.end) {
              return false;
            }

            OwnerSourceRange gapSource = OwnerSourceRange::From(
                previousToken->closure->source.path,
                previousToken->closure->source.end,
                edge.closure->source.begin,
                previousToken->closure->source.includeId);
            std::string coverageReason;
            if (!sourceGapIsFullyCovered(gapSource, gapsBetweenTokens,
                                         &coverageReason)) {
              return false;
            }

            gapsBetweenTokens.clear();
            previousToken = &edge;
          }

          // A proof-only gap cannot trail the final emitted token segment: it
          // would have no following edit boundary proving that the structure
          // remains outside the emitted carrier.
          return sawToken && gapsBetweenTokens.empty();
        };

    auto preservedInPlaceGapSourceOrderIsProven =
        [&](ArrayRef<PartitionEdge> path) {
          const PartitionEdge *previousToken = nullptr;
          SmallVector<const PartitionEdge *, 4> gapsBetweenTokens;
          bool sawPreservedGap = false;

          for (const PartitionEdge &edge : path) {
            if (edge.IsStateGap()) {
              if (edge.gapDisposition !=
                      StructuralGapDisposition::PreservedInPlace ||
                  !edge.closure || !edge.closure->source.IsComplete() ||
                  edge.closure->source.end <= edge.closure->source.begin ||
                  !previousToken || !previousToken->closure ||
                  !sourceSitesComparable(previousToken->closure->source,
                                         edge.closure->source) ||
                  edge.closure->source.begin <
                      previousToken->closure->source.end) {
                return false;
              }

              if (!gapsBetweenTokens.empty()) {
                const PartitionEdge *previousGap = gapsBetweenTokens.back();
                if (!previousGap->closure ||
                    !sourceSitesComparable(previousGap->closure->source,
                                           edge.closure->source) ||
                    edge.closure->source.begin <
                        previousGap->closure->source.end) {
                  return false;
                }
              }

              gapsBetweenTokens.push_back(&edge);
              sawPreservedGap = true;
              continue;
            }

            if (!edge.closure || !edge.closure->source.IsComplete())
              return false;

            if (!previousToken) {
              if (!gapsBetweenTokens.empty())
                return false;
              previousToken = &edge;
              continue;
            }

            // Different-realizer tilings may jump between incomparable source
            // owners without a physical gap edge.  That transition is governed
            // by the historical owner-boundary theorem and is irrelevant to a
            // local preserved gap.  Whenever gaps do occur, both surrounding
            // token carriers and every intervening gap must form one monotone
            // source sequence.
            if (gapsBetweenTokens.empty()) {
              previousToken = &edge;
              continue;
            }

            if (!sourceSitesComparable(previousToken->closure->source,
                                       edge.closure->source) ||
                edge.closure->source.begin <
                    previousToken->closure->source.end) {
              return false;
            }

            uint64_t cursor = previousToken->closure->source.end;
            for (const PartitionEdge *gap : gapsBetweenTokens) {
              if (!gap || !gap->closure ||
                  !sourceSitesComparable(previousToken->closure->source,
                                         gap->closure->source) ||
                  gap->closure->source.begin < cursor ||
                  edge.closure->source.begin < gap->closure->source.end) {
                return false;
              }
              cursor = gap->closure->source.end;
            }

            gapsBetweenTokens.clear();
            previousToken = &edge;
          }

          // A trailing source gap has no following emitted segment against
          // which its physical source position can be certified.
          return sawPreservedGap && gapsBetweenTokens.empty();
        };

    auto preservedInPlaceGapsAreDisjointFromAllTokenSegments =
        [&](ArrayRef<PartitionEdge> path) {
          SmallVector<const PartitionEdge *, 8> tokenSegments;
          SmallVector<const PartitionEdge *, 8> preservedGaps;

          for (const PartitionEdge &edge : path) {
            if (!edge.closure || !edge.closure->source.IsComplete())
              return false;
            if (edge.IsStateGap()) {
              if (edge.gapDisposition !=
                  StructuralGapDisposition::PreservedInPlace) {
                return false;
              }
              if (edge.closure->source.end <= edge.closure->source.begin)
                return false;
              preservedGaps.push_back(&edge);
              continue;
            }
            tokenSegments.push_back(&edge);
          }

          if (preservedGaps.empty())
            return false;

          // The same physical structure interval may appear only once in the
          // durable path.  Duplicate or overlapping preserved gaps would make
          // source-order authority ambiguous even though neither one emits
          // bytes directly.
          for (size_t i = 0; i < preservedGaps.size(); ++i) {
            for (size_t j = i + 1; j < preservedGaps.size(); ++j) {
              const OwnerSourceRange &lhs =
                  preservedGaps[i]->closure->source;
              const OwnerSourceRange &rhs =
                  preservedGaps[j]->closure->source;
              if (!sourceSitesComparable(lhs, rhs))
                continue;
              if (lhs.begin < rhs.end && rhs.begin < lhs.end)
                return false;
            }
          }

          // Check every emitted token carrier, not only the two path-adjacent
          // segments.  A mixed-owner A-order path can revisit the same physical
          // source file later; local predecessor/successor checks alone would
          // not exclude such a non-adjacent carrier from intersecting an
          // earlier
          // preserved interval.
          for (const PartitionEdge *gap : preservedGaps) {
            for (const PartitionEdge *token : tokenSegments) {
              if (!gap || !token || !gap->closure || !token->closure)
                return false;
              const OwnerSourceRange &gapSource = gap->closure->source;
              const OwnerSourceRange &tokenSource = token->closure->source;
              if (!sourceSitesComparable(gapSource, tokenSource))
                continue;
              if (tokenSource.begin < gapSource.end &&
                  gapSource.begin < tokenSource.end) {
                return false;
              }
            }
          }
          return true;
        };

    auto classifyStructuralTilingReason =
        [](bool mixedRealizers, bool preservedPreprocessingStructure) {
          if (mixedRealizers && preservedPreprocessingStructure) {
            return StructuralTilingReason::
                MixedRealizersAndPreservedStructure;
          }
          if (mixedRealizers)
            return StructuralTilingReason::MixedRealizers;
          if (preservedPreprocessingStructure) {
            return StructuralTilingReason::PreservedPreprocessingStructure;
          }
          return StructuralTilingReason::Unknown;
        };

    // boundary: failures while *searching* for a structural
    // partition are non-applicability, not terminal proof failures.  Until a
    // partition has been accepted and durable segment witnesses have been
    // emitted, the ordinary macro/include/TU owner-realization paths still own
    // the hunk.  Returning std::nullopt here therefore preserves the existing
    // lattice ordering instead of prematurely forcing raw-B terminal output.

    auto tryBuildStructuralPartition = [&](const diffutils::Hunk &h)
        -> std::optional<StructuralPartition> {
      const bool replaceHunk = h.isReplace();
      const bool deleteOnlyHunk = h.isDeleteOnly();

      // extends deterministic mixed-owner tiling beyond non-empty
      // replacements only where the theorem obligations are still meaningful.
      // Delete-only hunks have an A-side owner cover and an empty B envelope,
      // so they can be partitioned by the same owner-closure proof. Insert-only
      // hunks have no A-side owner cover; they require insertion-anchor proofs
      // handled by the existing insertion/macro/include machinery, not by this
      // mixed-owner tiler.  Equal/state-only hunks are likewise classified as
      // outside this normalizer instead of being silently interpreted as token
      // partitions.
      if (!replaceHunk && !deleteOnlyHunk)
        return std::nullopt;
      if (h.aEnd <= h.aStart || (replaceHunk && h.bEnd <= h.bStart) ||
          (deleteOnlyHunk && h.bEnd != h.bStart))
        return std::nullopt;
      if (h.aEnd - h.aStart < 2)
        return std::nullopt;

      // If the whole hunk is already a patchable macro invocation, leave it as
      // one hunk. Splitting inside an already-proven whole macro candidate
      // would make this normalization pass compete with the macro lattice
      // rather than merely exposing otherwise independent owners.
      Owner wholeOwner =
          deps_.ownerClassifier.ClassifyOwnerWithSegments(deps_.tuPath, h);
      if (auto *wholeMacro = deps_.macroTopology.SmallestCoveringPatchableMacro(
              h.aStart, h.aEnd, wholeOwner.includeId)) {
        if (wholeMacro->invB && wholeMacro->invE)
          return std::nullopt;
      }

      // Derive the canonical physical-source topology before constructing the
      // candidate edge graph. The historical mixed-realizer theorem may still
      // proceed when this direct topology is unavailable, but no partition may
      // claim preserved preprocessing structure without matching these exact
      // maximal runs.
      std::optional<PhysicalSourceRunPlan> physicalSourceRuns =
          buildPhysicalSourceRunPlan(h);

      // Patch 3.1 binds every canonical physical source-run boundary to one
      // exact B-token boundary before the DP may construct a replacement
      // partition. Patch 3.2 makes disagreement terminal for this normalization
      // attempt: retaining the physical plan only for same-realizer paths while
      // allowing the historical mixed-realizer search to continue would let an
      // alternate partition assign the same ambiguous payload indirectly.
      //
      // `ProjectATokenBoundaryToBTokenBounds()` now reports the minimum and
      // maximum B frontiers reached by all maximum-length local token
      // alignments. A strict range therefore means the B expression is
      // inseparable at this A
      // seam.  Do not choose an alignment, discard the structure witness, or put
      // the payload on a preferred side; preserve the original hunk for the
      // ordinary macro/include/expansion/fallback lattice.
      if (replaceHunk && physicalSourceRuns &&
          physicalSourceRuns->runs.size() >= 2) {
        bool projectionsComplete = true;
        physicalSourceRuns->boundaryProjections.clear();
        for (size_t runIndex = 0;
             runIndex + 1 < physicalSourceRuns->runs.size(); ++runIndex) {
          const uint64_t aBoundary =
              physicalSourceRuns->runs[runIndex].aEnd;
          std::optional<RefoldSourceMapper::ATokenBoundaryProjection>
              projection =
                  deps_.sourceMapper.ProjectATokenBoundaryToBTokenBounds(
                      h.aStart, h.aEnd, h.bStart, h.bEnd, aBoundary);
          if (!projection || !projection->IsUnique()) {
            if (inTraceMode()) {
              REFOLD_LOG_TRACE("tiling/structural",
                               "structural replacement rejected:");
              REFOLD_LOG_TRACE("tiling/structural",
                               "original A=[{0},{1}) B=[{2},{3})",
                               h.aStart, h.aEnd, h.bStart, h.bEnd);
              REFOLD_LOG_TRACE("tiling/structural",
                               "reason=non-unique B partition");
              if (projection) {
                REFOLD_LOG_TRACE(
                    "tiling/structural",
                    "ambiguous boundary A={0} lowerB={1} upperB={2}",
                    aBoundary, projection->lowerBTokenBoundary,
                    projection->upperBTokenBoundary);
              } else {
                REFOLD_LOG_TRACE("tiling/structural",
                                 "ambiguous boundary A={0} projection="
                                 "<incomplete>",
                                 aBoundary);
              }
            }
            projectionsComplete = false;
            break;
          }

          const uint64_t bBoundary = projection->lowerBTokenBoundary;
          if (bBoundary < h.bStart || bBoundary > h.bEnd ||
              (!physicalSourceRuns->boundaryProjections.empty() &&
               bBoundary <
                   physicalSourceRuns->boundaryProjections.back().bTokenBoundary)) {
            projectionsComplete = false;
            break;
          }

          StructuralBoundaryProjectionWitness boundaryWitness;
          boundaryWitness.aTokenBoundary = aBoundary;
          boundaryWitness.lowerBTokenBoundary =
              projection->lowerBTokenBoundary;
          boundaryWitness.upperBTokenBoundary =
              projection->upperBTokenBoundary;
          boundaryWitness.bTokenBoundary = bBoundary;
          boundaryWitness.uniqueProjection = true;
          physicalSourceRuns->boundaryProjections.push_back(boundaryWitness);
        }

        if (!projectionsComplete ||
            physicalSourceRuns->boundaryProjections.size() + 1 !=
                physicalSourceRuns->runs.size())
          return std::nullopt;
      }

      const uint64_t aLen = h.aEnd - h.aStart;
      std::vector<PartitionEdge> edges;
      std::vector<std::vector<size_t>> edgesByAOffset(
          static_cast<size_t>(aLen) + 1);

      for (uint64_t aLo = h.aStart; aLo < h.aEnd; ++aLo) {
        // Prefer wider segments when several partitions have the same number of
        // pieces. This keeps source structure maximally coarse while remaining
        // deterministic.
        for (uint64_t aHi = h.aEnd; aHi > aLo; --aHi) {
          HunkRealizer realizer = classifyHunkRealizer(aLo, aHi);
          if (realizer.kind == HunkRealizerKind::Unknown)
            continue;

          std::optional<size_t> exactPhysicalRunIndex;
          if (physicalSourceRuns) {
            for (size_t runIndex = 0;
                 runIndex < physicalSourceRuns->runs.size(); ++runIndex) {
              const PhysicalSourceRun &run =
                  physicalSourceRuns->runs[runIndex];
              if (run.aStart != aLo || run.aEnd != aHi)
                continue;
              if (exactPhysicalRunIndex) {
                exactPhysicalRunIndex.reset();
                break;
              }
              exactPhysicalRunIndex = runIndex;
            }
          }

          uint64_t edgeBStart = h.bStart;
          uint64_t edgeBEnd = h.bStart;
          if (!deleteOnlyHunk) {
            if (exactPhysicalRunIndex && physicalSourceRuns &&
                physicalSourceRuns->boundaryProjections.size() + 1 ==
                    physicalSourceRuns->runs.size()) {
              const size_t runIndex = *exactPhysicalRunIndex;
              edgeBStart =
                  runIndex == 0
                      ? h.bStart
                      : physicalSourceRuns
                            ->boundaryProjections[runIndex - 1]
                            .bTokenBoundary;
              edgeBEnd =
                  runIndex + 1 == physicalSourceRuns->runs.size()
                      ? h.bEnd
                      : physicalSourceRuns->boundaryProjections[runIndex]
                            .bTokenBoundary;
              if (edgeBEnd < edgeBStart)
                continue;
            } else {
              auto env =
                  deps_.sourceMapper
                      .MapATokRangeAToBTokenEnvelopeTrimEdgeInsertions(aLo,
                                                                       aHi);
              if (!env)
                continue;
              if (env->first < static_cast<size_t>(h.bStart) ||
                  env->second > static_cast<size_t>(h.bEnd) ||
                  env->first >= env->second) {
                continue;
              }
              edgeBStart = static_cast<uint64_t>(env->first);
              edgeBEnd = static_cast<uint64_t>(env->second);
            }
          }

          PartitionEdge edge;
          edge.aStart = aLo;
          edge.aEnd = aHi;
          edge.bStart = edgeBStart;
          edge.bEnd = edgeBEnd;
          edge.realizer = realizer;
          // A nonempty parent replacement may contain a uniquely projected
          // delete fragment.  Such an empty B envelope is authorized only for
          // one exact canonical physical run; arbitrary subrange envelopes
          // retain the historical nonempty requirement.
          edge.allowEmptyBEnvelope =
              deleteOnlyHunk ||
              (replaceHunk && exactPhysicalRunIndex &&
               edgeBStart == edgeBEnd);
          edge.closure = buildTokenSegmentClosure(edge);
          if (!edge.closure)
            continue;

          const size_t edgeIndex = edges.size();
          edges.push_back(edge);
          edgesByAOffset[static_cast<size_t>(aLo - h.aStart)].push_back(
              edgeIndex);
        }
      }

      auto physicalRunIndexForEdge =
          [&](const PartitionEdge &candidate) -> std::optional<size_t> {
        if (!physicalSourceRuns || !candidate.IsTokenSegment() ||
            !candidate.closure ||
            !candidate.closure->source.IsComplete()) {
          return std::nullopt;
        }

        std::optional<size_t> match;
        for (size_t runIndex = 0; runIndex < physicalSourceRuns->runs.size();
             ++runIndex) {
          const PhysicalSourceRun &run = physicalSourceRuns->runs[runIndex];
          if (candidate.aStart != run.aStart || candidate.aEnd != run.aEnd ||
              !sourceSitesComparable(candidate.closure->source, run.source) ||
              candidate.closure->source.begin != run.source.begin ||
              candidate.closure->source.end != run.source.end) {
            continue;
          }

          if (match)
            return std::nullopt;
          match = runIndex;
        }
        return match;
      };

      std::vector<std::map<PartitionStateKey, PartitionParent>> dp(
          static_cast<size_t>(aLen) + 1);
      PartitionStateKey startKey;
      startKey.bPos = h.bStart;
      startKey.lastTokenEdgeIndex = noTokenEdgeIndex;

      PartitionParent startParent;
      startParent.valid = true;
      startParent.cost = 0;
      dp[0][startKey] = std::move(startParent);

      for (uint64_t aOff = 0; aOff < aLen; ++aOff) {
        auto &states = dp[static_cast<size_t>(aOff)];
        if (states.empty())
          continue;

        for (const auto &state : states) {
          const PartitionStateKey &key = state.first;
          const unsigned curCost = state.second.cost;

          for (size_t edgeIndex : edgesByAOffset[static_cast<size_t>(aOff)]) {
            const PartitionEdge &edge = edges[edgeIndex];
            if (edge.bStart != key.bPos)
              continue;

            const PartitionEdge *prevEdge = nullptr;
            if (key.lastTokenEdgeIndex != noTokenEdgeIndex) {
              if (key.lastTokenEdgeIndex >= edges.size())
                continue;
              prevEdge = &edges[key.lastTokenEdgeIndex];
            }

            // state gaps are part of the searched proof graph.  They are
            // computed on the transition from the previous token segment to
            // this token segment, so an unsafe or uncovered source gap prevents
            // this DP edge from existing at all.  This lets cost and ambiguity
            // account for the real token+state proof graph instead of adding
            // state gaps as an after-the-fact annotation.
            auto transitionGaps =
                buildClosedStateGapTransition(prevEdge, edge);
            if (!transitionGaps)
              continue;

            // Protected preprocessing structure may justify a partition only
            // at one canonical maximal-run boundary. This prevents the DP from
            // manufacturing a split from arbitrary token subranges merely
            // because they surround the same directive. The predecessor and
            // successor must be the complete runs immediately adjacent to the
            // exact byte-complete physical gap.
            bool protectedReplacementBoundaryProven = false;
            if (transitionGaps->hasProtectedPreprocessingStructure &&
                prevEdge) {
              if (replaceHunk) {
                // Every replacement transition across protected source bytes,
                // including a transition between different realizers, needs the
                // same canonical source-run and unique B-frontier authority.
                // Otherwise a mixed-owner path could bypass the Patch 3.2
                // ambiguity rejection that applies to a same-owner path.
                if (!physicalSourceRuns)
                  continue;

                std::optional<size_t> boundaryIndex;
                for (size_t runIndex = 0;
                     runIndex + 1 < physicalSourceRuns->runs.size();
                     ++runIndex) {
                  const PhysicalSourceRun &leftRun =
                      physicalSourceRuns->runs[runIndex];
                  const PhysicalSourceRun &rightRun =
                      physicalSourceRuns->runs[runIndex + 1];
                  if (leftRun.aEnd != prevEdge->aEnd ||
                      rightRun.aStart != edge.aStart)
                    continue;
                  if (boundaryIndex) {
                    boundaryIndex.reset();
                    break;
                  }
                  boundaryIndex = runIndex;
                }
                if (!boundaryIndex ||
                    *boundaryIndex >=
                        physicalSourceRuns->protectedGaps.size() ||
                    *boundaryIndex >=
                        physicalSourceRuns->boundaryProjections.size()) {
                  continue;
                }

                const OwnerSourceRange &provedGap =
                    physicalSourceRuns->protectedGaps[*boundaryIndex];
                if (!sourceSitesComparable(prevEdge->closure->source,
                                           provedGap) ||
                    provedGap.begin != prevEdge->closure->source.end ||
                    provedGap.end != edge.closure->source.begin) {
                  continue;
                }

                // A complete #define/#undef line is not an ordinary preserved
                // separator in the direct TU pipeline.  The whole-hunk path is
                // allowed to consume it precisely so the dedicated macro-state
                // liveness planner can decide whether the directive must remain
                // before, move after, or be replayed around the replacement.
                // Splitting here would make that planner observe two narrow
                // edits and no consumed macro-state transition, thereby
                // replacing its established ordering theorem with an unrelated
                // byte/token alignment.  Defer instead of stealing the hunk.
                if (transitionContainsMacroStateDirective(*transitionGaps))
                  continue;

                const StructuralBoundaryProjectionWitness &projection =
                    physicalSourceRuns
                        ->boundaryProjections[*boundaryIndex];
                if (!projection.uniqueProjection ||
                    projection.aTokenBoundary != prevEdge->aEnd ||
                    projection.aTokenBoundary != edge.aStart ||
                    projection.lowerBTokenBoundary !=
                        projection.upperBTokenBoundary ||
                    projection.bTokenBoundary !=
                        projection.lowerBTokenBoundary ||
                    prevEdge->bEnd != projection.bTokenBoundary ||
                    edge.bStart != projection.bTokenBoundary) {
                  continue;
                }
                protectedReplacementBoundaryProven = true;
              } else if (prevEdge->realizer == edge.realizer) {
                // Delete-only same-realizer tiling retains the Patch 2 theorem:
                // both token edges must be the exact maximal runs surrounding
                // the proved protected gap.  Mixed-realizer deletion behavior is
                // intentionally unchanged.
                if (!physicalSourceRuns)
                  continue;
                std::optional<size_t> previousRun =
                    physicalRunIndexForEdge(*prevEdge);
                std::optional<size_t> currentRun =
                    physicalRunIndexForEdge(edge);
                if (!previousRun || !currentRun ||
                    *currentRun != *previousRun + 1 ||
                    *previousRun >= physicalSourceRuns->protectedGaps.size()) {
                  continue;
                }

                const OwnerSourceRange &provedGap =
                    physicalSourceRuns->protectedGaps[*previousRun];
                if (!sourceSitesComparable(prevEdge->closure->source,
                                           provedGap) ||
                    provedGap.begin != prevEdge->closure->source.end ||
                    provedGap.end != edge.closure->source.begin) {
                  continue;
                }
              }
            }

            if (replaceHunk &&
                transitionGaps->hasProtectedPreprocessingStructure &&
                !protectedReplacementBoundaryProven) {
              continue;
            }

            PartitionStateKey nextKey;
            nextKey.bPos = edge.bEnd;
            nextKey.lastRealizer = edge.realizer;
            nextKey.lastTokenEdgeIndex = edgeIndex;

            if (key.firstRealizer.kind == HunkRealizerKind::Unknown) {
              nextKey.firstRealizer = edge.realizer;
              nextKey.mixedRealizers = false;
              nextKey.preservedPreprocessingStructure = false;
            } else {
              // Adjacent equal realizers are never split merely because the
              // subrange search found two candidates.  Delete-only hunks require
              // one exact protected source gap.  Replacement hunks additionally
              // require the Patch 3.1 theorem for that canonical run boundary:
              // the independently derived lower and upper A-to-B projections
              // must agree with both neighboring edge envelopes.  This preserves
              // directive order without assigning replacement payload by
              // traversal order, textual proximity, or a preferred side.
              if (key.lastRealizer == edge.realizer) {
                const bool deleteBoundaryProven =
                    deleteOnlyHunk &&
                    transitionGaps->hasProtectedPreprocessingStructure;
                const bool replacementBoundaryProven =
                    replaceHunk &&
                    transitionGaps->hasProtectedPreprocessingStructure &&
                    protectedReplacementBoundaryProven;
                if (!deleteBoundaryProven && !replacementBoundaryProven)
                  continue;
              }
              nextKey.firstRealizer = key.firstRealizer;
              nextKey.mixedRealizers =
                  key.mixedRealizers || edge.realizer != key.firstRealizer;
              nextKey.preservedPreprocessingStructure =
                  key.preservedPreprocessingStructure ||
                  transitionGaps->hasProtectedPreprocessingStructure;
            }

            auto &dst = dp[static_cast<size_t>(edge.aEnd - h.aStart)];
            const unsigned gapEdgeCost =
                static_cast<unsigned>(transitionGaps->gaps.size());
            const unsigned nextCost = curCost + 1 + gapEdgeCost;
            auto existing = dst.find(nextKey);
            if (existing == dst.end() || nextCost < existing->second.cost) {
              PartitionParent parent;
              parent.valid = true;
              parent.edgeIndex = edgeIndex;
              parent.prev = key;
              parent.cost = nextCost;
              parent.stateGapsBeforeEdge = transitionGaps->gaps;
              parent.ambiguous = state.second.ambiguous;
              dst[nextKey] = std::move(parent);
            } else if (nextCost == existing->second.cost) {
              // Two minimal chains prove the same next state. Do not choose
              // between them by map/edge iteration order; mark the state as
              // ambiguous so the final tiling proof fails closed.
              existing->second.ambiguous = true;
            }
          }
        }
      }

      const auto &finalStates = dp[static_cast<size_t>(aLen)];
      auto bestFinal = finalStates.end();
      unsigned bestCost = std::numeric_limits<unsigned>::max();
      bool ambiguousBest = false;
      for (auto it = finalStates.begin(); it != finalStates.end(); ++it) {
        const PartitionStateKey &key = it->first;
        if (key.bPos != h.bEnd ||
            (!key.mixedRealizers &&
             !key.preservedPreprocessingStructure)) {
          continue;
        }

        const unsigned candidateCost = it->second.cost;
        if (candidateCost < bestCost) {
          bestFinal = it;
          bestCost = candidateCost;
          ambiguousBest = it->second.ambiguous;
        } else if (candidateCost == bestCost) {
          ambiguousBest = true;
        }
      }
      if (bestFinal == finalStates.end())
        return std::nullopt;

      if (ambiguousBest) {
        return std::nullopt;
      }

      // Reconstruct the unique lowest-cost structural path.  The DP tracks both
      // mixed-realizer and protected-structure obligations as state, rather
      // than
      // choosing the cheapest token cover first and classifying it afterwards.
      // This prevents a coarse TU edge from swallowing a smaller macro/include
      // segment or a preprocessing-structure boundary.  Equal-cost alternatives
      // are rejected instead of being hidden behind map/edge iteration order.
      SmallVector<PartitionEdge, 8> reversePath;
      uint64_t aPos = h.aEnd;
      PartitionStateKey stateKey = bestFinal->first;
      while (aPos != h.aStart) {
        const uint64_t aOff = aPos - h.aStart;
        const auto stateIt = dp[static_cast<size_t>(aOff)].find(stateKey);
        if (stateIt == dp[static_cast<size_t>(aOff)].end() ||
            !stateIt->second.valid) {
          return std::nullopt;
        }

        const PartitionParent &parent = stateIt->second;
        const PartitionEdge &edge = edges[parent.edgeIndex];
        reversePath.push_back(edge);
        // Gaps are stored before the token edge in forward order.  During
        // backward reconstruction, append them in reverse so the final reverse
        // below yields: previous token, gap..., current token.
        for (auto gapIt = parent.stateGapsBeforeEdge.rbegin();
             gapIt != parent.stateGapsBeforeEdge.rend(); ++gapIt) {
          reversePath.push_back(*gapIt);
        }

        aPos = edge.aStart;
        stateKey = parent.prev;
      }

      SmallVector<PartitionEdge, 8> path;
      path.reserve(reversePath.size());
      for (auto it = reversePath.rbegin(); it != reversePath.rend(); ++it)
        path.push_back(*it);

      size_t tokenSegmentCount = 0;
      for (const PartitionEdge &edge : path) {
        if (edge.IsTokenSegment())
          ++tokenSegmentCount;
      }
      if (tokenSegmentCount < 2)
        return std::nullopt;

      // Re-validate the reconstructed partition as a true structural tiling.
      // The dynamic-programming search already found a path, but the proof
      // obligation is stronger: each edge must start exactly where the previous
      // edge ended on both A and B, must consume a non-empty A-token envelope,
      // and must have a known realizer.  Same-realizer adjacency additionally
      // requires an intervening gap edge that carries exact protected
      // preprocessing structure; otherwise the split is artificial.
      uint64_t expectedA = h.aStart;
      uint64_t expectedB = h.bStart;
      std::optional<HunkRealizer> firstRealizer;
      std::optional<HunkRealizer> previousRealizer;
      bool sawDifferentRealizer = false;
      bool sawProtectedStructure = false;
      bool protectedStructureSincePreviousToken = false;
      bool sawProtectedReplacementBoundary = false;
      for (const PartitionEdge &edge : path) {
        if (edge.IsStateGap()) {
          if (edge.aStart != expectedA || edge.aEnd != expectedA ||
              edge.bStart != expectedB || edge.bEnd != expectedB ||
              !edge.closure || !edge.closure->IsComplete()) {
            return std::nullopt;
          }
          sawProtectedStructure |= edge.protectedPreprocessingStructure;
          protectedStructureSincePreviousToken |=
              edge.protectedPreprocessingStructure;
          continue;
        }

        if (edge.aStart != expectedA || edge.bStart != expectedB ||
            edge.aEnd <= edge.aStart || edge.bEnd < edge.bStart ||
            (!edge.allowEmptyBEnvelope && edge.bEnd <= edge.bStart) ||
            edge.realizer.kind == HunkRealizerKind::Unknown || !edge.closure ||
            !edge.closure->IsComplete()) {
          return std::nullopt;
        }

        if (!firstRealizer)
          firstRealizer = edge.realizer;
        else
          sawDifferentRealizer |= edge.realizer != *firstRealizer;

        if (previousRealizer && *previousRealizer == edge.realizer) {
          if (!protectedStructureSincePreviousToken)
            return std::nullopt;
        }
        sawProtectedReplacementBoundary |=
            replaceHunk && protectedStructureSincePreviousToken;
        previousRealizer = edge.realizer;
        protectedStructureSincePreviousToken = false;
        expectedA = edge.aEnd;
        expectedB = edge.bEnd;
      }

      // A structural tiling is theorem-relevant only when the complete token
      // envelope is covered and at least one of the two explicit split reasons
      // is present.  A same-realizer path with no protected structure remains
      // the ordinary single-owner case and is rejected here.
      if (expectedA != h.aEnd || expectedB != h.bEnd ||
          (!sawDifferentRealizer && !sawProtectedStructure)) {
        return std::nullopt;
      }

      const StructuralTilingReason reason = classifyStructuralTilingReason(
          sawDifferentRealizer, sawProtectedStructure);
      if (reason == StructuralTilingReason::Unknown)
        return std::nullopt;

      // A pure same-realizer preservation partition always requires the
      // canonical physical-run theorem.  Patch 3.2 extends that requirement to
      // every replacement containing a protected seam, including mixed-realizer
      // paths: owner diversity cannot authorize a B split that the token
      // alignment itself leaves ambiguous.
      const bool requiresCanonicalPhysicalRunProof =
          reason == StructuralTilingReason::PreservedPreprocessingStructure ||
          sawProtectedReplacementBoundary;

      uint32_t physicalSourceRunCount = 0;
      bool physicalSourceRunsProven = false;
      bool uniqueMinimumFragmentPartition = false;
      if (requiresCanonicalPhysicalRunProof) {
        if (!physicalSourceRuns || physicalSourceRuns->runs.size() < 2 ||
            physicalSourceRuns->protectedGaps.size() + 1 !=
                physicalSourceRuns->runs.size()) {
          return std::nullopt;
        }

        size_t expectedRunIndex = 0;
        size_t tokenSegmentsInRuns = 0;
        for (const PartitionEdge &edge : path) {
          if (edge.IsStateGap())
            continue;
          while (expectedRunIndex < physicalSourceRuns->runs.size() &&
                 edge.aStart ==
                     physicalSourceRuns->runs[expectedRunIndex].aEnd) {
            ++expectedRunIndex;
          }
          if (expectedRunIndex >= physicalSourceRuns->runs.size())
            return std::nullopt;
          const PhysicalSourceRun &run =
              physicalSourceRuns->runs[expectedRunIndex];
          if (edge.aStart < run.aStart || edge.aEnd > run.aEnd)
            return std::nullopt;
          ++tokenSegmentsInRuns;
        }
        if (expectedRunIndex + 1 != physicalSourceRuns->runs.size() ||
            path.empty())
          return std::nullopt;

        // Each run is maximal across exact lexer trivia and terminates only at
        // one byte-complete protected gap. Emitting exactly one token segment
        // per run is therefore the unique minimum-fragment partition that
        // leaves every protected interval outside the edit set.
        physicalSourceRunCount =
            static_cast<uint32_t>(physicalSourceRuns->runs.size());
        physicalSourceRunsProven = true;
        uniqueMinimumFragmentPartition =
            tokenSegmentsInRuns == physicalSourceRuns->runs.size();
      }

      SmallVector<StructuralBoundaryProjectionWitness, 8>
          boundaryProjections;
      bool uniqueBoundaryProjectionProven = false;
      if (replaceHunk && requiresCanonicalPhysicalRunProof) {
        if (!physicalSourceRuns ||
            physicalSourceRuns->boundaryProjections.size() + 1 !=
                physicalSourceRuns->runs.size()) {
          return std::nullopt;
        }

        const PartitionEdge *previousToken = nullptr;
        bool protectedStructureSincePreviousToken = false;
        size_t projectionIndex = 0;
        for (const PartitionEdge &edge : path) {
          if (edge.IsStateGap()) {
            protectedStructureSincePreviousToken |=
                edge.protectedPreprocessingStructure;
            continue;
          }
          if (!previousToken) {
            previousToken = &edge;
            protectedStructureSincePreviousToken = false;
            continue;
          }
          if (!protectedStructureSincePreviousToken) {
            previousToken = &edge;
            continue;
          }
          if (projectionIndex >=
              physicalSourceRuns->boundaryProjections.size()) {
            return std::nullopt;
          }

          const StructuralBoundaryProjectionWitness &projection =
              physicalSourceRuns->boundaryProjections[projectionIndex++];
          if (!projection.uniqueProjection ||
              projection.lowerBTokenBoundary !=
                  projection.upperBTokenBoundary ||
              projection.bTokenBoundary !=
                  projection.lowerBTokenBoundary ||
              projection.aTokenBoundary != previousToken->aEnd ||
              projection.aTokenBoundary != edge.aStart ||
              projection.bTokenBoundary != previousToken->bEnd ||
              projection.bTokenBoundary != edge.bStart) {
            return std::nullopt;
          }
          boundaryProjections.push_back(projection);
          previousToken = &edge;
          protectedStructureSincePreviousToken = false;
        }

        if (projectionIndex !=
            physicalSourceRuns->boundaryProjections.size()) {
          return std::nullopt;
        }
        uniqueBoundaryProjectionProven = true;
      }

      const bool sourceByteCoverComplete =
          structuralPartitionSourceCoverComplete(path);
      if (requiresCanonicalPhysicalRunProof && !sourceByteCoverComplete) {
        return std::nullopt;
      }

      const bool hasPreservedInPlaceGap = llvm::any_of(
          path, [](const PartitionEdge &edge) {
            return edge.IsStateGap() &&
                   edge.gapDisposition ==
                       StructuralGapDisposition::PreservedInPlace;
          });
      const bool preservedGapSourceOrderProven =
          hasPreservedInPlaceGap &&
          preservedInPlaceGapSourceOrderIsProven(path);
      const bool preservedGapsDisjointFromTokenSegments =
          hasPreservedInPlaceGap &&
          preservedInPlaceGapsAreDisjointFromAllTokenSegments(path);
      if (hasPreservedInPlaceGap &&
          (!preservedGapSourceOrderProven ||
           !preservedGapsDisjointFromTokenSegments)) {
        return std::nullopt;
      }

      bool sharedEmptyBEnvelopeProven = false;
      uint64_t sharedEmptyBBoundary = 0;
      bool preservedStateChainComposed = false;
      if (deleteOnlyHunk) {
        // A delete-only hunk gives every emitted token segment the same empty B
        // boundary.  Prove that fact edge-by-edge instead of relying on the
        // candidate builder having initialized every edge from `h.bStart`.
        // This makes the durable witness independently reject a malformed
        // partition that distributes a deletion over different B gaps.
        const uint64_t sharedBoundary = h.bStart;
        bool sawStateGap = false;
        for (const PartitionEdge &edge : path) {
          if (!edge.closure || edge.bStart != sharedBoundary ||
              edge.bEnd != sharedBoundary) {
            return std::nullopt;
          }

          if (edge.IsStateGap()) {
            sawStateGap = true;
            // A proof-only state edge is admissible for deletion only when its
            // complete physical bytes remain in the source stream.  A gap that
            // is unknown, materialized by an owner, or repaired by a state
            // planner would be consumed, moved, reconstructed, or replayed by
            // some other theorem and therefore cannot justify this split.
            if (edge.gapDisposition !=
                    StructuralGapDisposition::PreservedInPlace ||
                !edge.closure->source.IsComplete() ||
                edge.closure->source.end <= edge.closure->source.begin) {
              return std::nullopt;
            }
            continue;
          }

          // A pure same-realizer preserved-structure partition is
          // admitted by the canonical physical-run theorem, not by the coarse
          // state census attached to its realizer.  A TU closure, for example,
          // summarizes mutations across the complete translation unit and can
          // therefore report directives that are wholly outside this narrow
          // token run.  Treating that owner-wide summary as segment-local
          // evidence would reject every valid TU deletion around untouched
          // preprocessing structure.
          //
          // The segment-local proof is stronger: exact monotone token mappings
          // form maximal physical runs, every intervening protected byte is a
          // `PreservedInPlace` gap, `PlanTUByteSpan()` revalidates each emitted
          // carrier, and the final assembler audit keeps every preserved gap
          // disjoint from every normalized `TextEdit`.  Retain the historical
          // mutation check for mixed-realizer deletion tilings, where owner
          // state remains part of that older composition theorem.
          if (!edge.allowEmptyBEnvelope ||
              (reason !=
                   StructuralTilingReason::PreservedPreprocessingStructure &&
               (deps_.ownerStateProof.OwnerStateDeltaMutatesAnyState(
                    edge.closure->stateIn) ||
                deps_.ownerStateProof.OwnerStateDeltaMutatesAnyState(
                    edge.closure->stateOut)))) {
            return std::nullopt;
          }
        }

        // Value-level state interpretation is intentionally unnecessary for a
        // preserved gap: the exact source transition continues to execute in
        // place.  The complete ordered chain is composed only when the physical
        // source cover, gap order, and global token-segment disjointness proofs
        // all agree.  With no state gaps the chain is vacuously composed.
        preservedStateChainComposed =
            !sawStateGap ||
            (sourceByteCoverComplete && preservedGapSourceOrderProven &&
             preservedGapsDisjointFromTokenSegments);
        if (!preservedStateChainComposed)
          return std::nullopt;

        sharedEmptyBEnvelopeProven = true;
        sharedEmptyBBoundary = sharedBoundary;
      }

      std::string stateCompositionReason;
      if (!mixedOwnerTilingStateSummariesCompose(h, path,
                                                 &stateCompositionReason)) {
        return std::nullopt;
      }

      StructuralPartition partition;
      partition.edges = std::move(path);
      partition.reason = reason;
      partition.uniquePartition = true;
      partition.physicalSourceRunCount = physicalSourceRunCount;
      partition.physicalSourceRunsProven = physicalSourceRunsProven;
      partition.uniqueMinimumFragmentPartition =
          uniqueMinimumFragmentPartition;
      partition.boundaryProjections = std::move(boundaryProjections);
      partition.uniqueBoundaryProjectionProven =
          uniqueBoundaryProjectionProven;
      partition.sharedEmptyBEnvelopeProven =
          sharedEmptyBEnvelopeProven;
      partition.sharedEmptyBBoundary = sharedEmptyBBoundary;
      partition.preservedStateChainComposed =
          preservedStateChainComposed;
      partition.sourceByteCoverComplete = sourceByteCoverComplete;
      partition.preservedGapSourceOrderProven =
          preservedGapSourceOrderProven;
      partition.preservedGapsDisjointFromTokenSegments =
          preservedGapsDisjointFromTokenSegments;
      return partition;
    };

    bool changed = true;
    while (changed) {
      changed = false;
      std::vector<diffutils::Hunk> splitHunks;
      splitHunks.reserve(hunks.size());

      for (const auto &h : hunks) {
        auto partition = tryBuildStructuralPartition(h);
        if (!partition) {
          splitHunks.push_back(h);
          continue;
        }

        // Persist the full ordered proof path before lowering the partition
        // back into ordinary token hunks.  Each emitted token segment gets a
        // reverse binding to this witness so later macro/include/TU accepted
        // candidates can report the structural reason that justified the split,
        // including proof-only state-gap edges that are not emitted.
        const size_t mixedWitnessIndex = deps_.mixedOwnerTilingWitnesses.size();
        const uint64_t mixedWitnessId =
            static_cast<uint64_t>(mixedWitnessIndex) + 1;
        MixedOwnerTilingWitness witness =
            buildStructuralTilingWitness(h, *partition, mixedWitnessId);
        const bool preservesStructure =
            partition->reason ==
                StructuralTilingReason::PreservedPreprocessingStructure ||
            partition->reason == StructuralTilingReason::
                                     MixedRealizersAndPreservedStructure;
        if (preservesStructure &&
            !structuralPreservedSourceTopologyIsComplete(witness)) {
          // The partition search is not proof authority by itself. If the
          // durable edge chain cannot independently prove that each directive
          // remains between the same token-bearing fragments, withdraw the
          // normalization attempt and leave the original hunk to the ordinary
          // macro/include/expansion/fallback lattice.
          splitHunks.push_back(h);
          continue;
        }
        logAcceptedStructuralTiling(witness);
        deps_.mixedOwnerTilingWitnesses.push_back(std::move(witness));

        uint32_t mixedSegmentIndex = 0;
        for (const PartitionEdge &edge : partition->edges) {
          const uint32_t currentSegmentIndex = mixedSegmentIndex++;
          if (edge.IsStateGap()) {
            continue;
          }

          deps_.mixedOwnerTilingSegmentBindings.push_back(
              MixedOwnerTilingSegmentBinding{
                  edge.aStart, edge.aEnd, edge.bStart, edge.bEnd,
                  mixedWitnessIndex, mixedWitnessId, currentSegmentIndex});
          splitHunks.push_back(
              diffutils::Hunk{edge.aStart, edge.aEnd, edge.bStart, edge.bEnd});
        }

        changed = true;
      }

      if (changed) {
        hunks = std::move(splitHunks);
        deps_.abTokHunks = hunks;
      }
    }
  }

  // Refresh the token-level hunk cache after normalization.
  deps_.abTokHunks = hunks;

  MixedOwnerTilingPlan plan;
  plan.hunks = std::move(hunks);
  plan.mixedOwnerWitnessCount = deps_.mixedOwnerTilingWitnesses.size();
  plan.segmentBindingCount = deps_.mixedOwnerTilingSegmentBindings.size();
  return plan;
}

} // namespace refold
} // namespace clang
