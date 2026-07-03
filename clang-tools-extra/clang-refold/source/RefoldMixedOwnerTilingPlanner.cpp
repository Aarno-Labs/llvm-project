//===--- RefoldMixedOwnerTilingPlanner.cpp ----------------------*- C++ -*-===//
//
// Mixed-owner token-hunk tiling service for clang-refold.
//
// This file contains the deterministic DP normalizer that proves when a single
// token-level hunk may be lowered into multiple owner-specific token hunks.  It
// does not build source edits.  Each emitted segment remains subject to the
// ordinary TU/include/macro proof path after the normalization pass.
//
//===----------------------------------------------------------------------===//

#include "source/RefoldMixedOwnerTilingPlanner.h"

#include "core/RefoldModel.h"
#include "core/RefoldOwnerClassifier.h"
#include "line-control/LineDirectiveInserter.h"
#include "macro/RefoldMacroTopology.h"
#include "proof/RefoldNeutralityProof.h"
#include "proof/RefoldOwnerStateProof.h"
#include "proof/RefoldProofLattice.h"
#include "proof/RefoldWitnessTrace.h"
#include "source/RefoldSourceMapper.h"
#include "util/RefoldPathIdentity.h"

#include "clang/Basic/LangOptions.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/Support/FormatVariadic.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <cassert>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

using namespace llvm;

namespace clang {
namespace refold {

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

  // Mixed-owner witnesses are rebuilt from the current token diff. They
  // describe only mixed-owner partitions proved during this normalization pass
  // and are later attached to accepted candidates by exact A/B token-envelope
  // binding.
  deps_.mixedOwnerTilingWitnesses.clear();
  deps_.mixedOwnerTilingSegmentBindings.clear();

  // Split replace hunks when a deterministic mixed-owner partition can be
  // proven.
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
  //   * the final partition contains at least two different realizers; and
  //   * the minimal tiling is unique. Equal-cost alternatives are rejected
  //     rather than hidden behind an implementation-order tie-breaker.
  //
  // This remains a normalization-only proof. Each emitted sub-hunk is still
  // validated later by the ordinary macro/include/TU classifier before any
  // source edit is accepted. additionally lets the partition carry
  // proof-only zero-token state-gap edges between adjacent token segments.
  // makes those edges part of the searched partition graph: they are
  // attached to token-to-token DP transitions, counted in the proof cost, and
  // included in ambiguity detection even though they are not emitted as hunks.
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
      /// True for the current proof model delete-only token segments.  Such
      /// segments still have an owner-closed A-token cover, but their B
      /// envelope is the empty insertion point at the delete hunk boundary.
      /// Replace/insert tilings must not use this escape hatch.
      bool allowEmptyBEnvelope = false;
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
      bool mixed = false;

      bool operator<(const PartitionStateKey &other) const {
        if (bPos != other.bPos)
          return bPos < other.bPos;
        if (firstRealizer != other.firstRealizer)
          return firstRealizer < other.firstRealizer;
        if (lastRealizer != other.lastRealizer)
          return lastRealizer < other.lastRealizer;
        if (lastTokenEdgeIndex != other.lastTokenEdgeIndex)
          return lastTokenEdgeIndex < other.lastTokenEdgeIndex;
        return mixed < other.mixed;
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

    auto sourceGapTriviaIsIgnorable = [&](const OwnerSourceRange &gapSource,
                                          uint64_t begin, uint64_t end,
                                          std::string *reason) -> bool {
      if (end <= begin)
        return true;

      std::optional<StringRef> bytes = getSourceBytesForGapPath(gapSource.path);
      if (!bytes) {
        if (reason) {
          *reason =
              llvm::formatv("source file {0} could not be read", gapSource.path)
                  .str();
        }
        return false;
      }
      if (end > bytes->size()) {
        if (reason) {
          *reason =
              llvm::formatv(
                  "source trivia interval [{0},{1}) exceeds file size {2}",
                  begin, end, bytes->size())
                  .str();
        }
        return false;
      }

      if (sourceTextIsOnlyIgnorableGapTrivia(bytes->slice(begin, end),
                                             deps_.lexLang))
        return true;

      if (reason) {
        *reason =
            llvm::formatv("source trivia interval [{0},{1}) contains unmodeled "
                          "non-trivia bytes",
                          begin, end)
                .str();
      }
      return false;
    };

    auto sourceGapIsFullyCovered = [&](const OwnerSourceRange &gapSource,
                                       ArrayRef<PartitionEdge> gaps,
                                       std::string *reason) -> bool {
      if (!gapSource.IsComplete()) {
        if (reason)
          *reason = "source gap has incomplete source coordinates";
        return false;
      }

      // A gap proof is byte-complete: every byte between adjacent token owners
      // must either belong to a modeled zero-token state owner or be ignorable
      // preprocessing trivia.  This prevents a mixed-owner split from silently
      // skipping an unmodeled directive, token, or source-state island merely
      // because that byte range contributed no ordinary PP tokens.
      uint64_t cursor = gapSource.begin;
      for (const PartitionEdge &gap : gaps) {
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
                llvm::formatv("state-gap owner [{0},{1}) is outside source gap "
                              "[{2},{3})",
                              owned.begin, owned.end, gapSource.begin,
                              gapSource.end)
                    .str();
          }
          return false;
        }

        // requires zero-token owners to bind to the same include occurrence as
        // the token owners on both sides of the gap.  This is the
        // repeated-header disambiguation point: a path+byte match without the
        // correct include identity is not enough to cross the gap.
        if (owned.includeId != gapSource.includeId) {
          if (reason)
            *reason = "state-gap owner belongs to a different include instance";
          return false;
        }

        if (owned.begin < cursor) {
          if (reason) {
            *reason = llvm::formatv("state-gap owners overlap at byte {0}",
                                    owned.begin)
                          .str();
          }
          return false;
        }

        if (!sourceGapTriviaIsIgnorable(gapSource, cursor, owned.begin, reason))
          return false;

        cursor = owned.end;
      }

      return sourceGapTriviaIsIgnorable(gapSource, cursor, gapSource.end,
                                        reason);
    };

    auto stateGapComposesSafely = [&](const PartitionEdge &gap,
                                      std::string *reason) -> bool {
      if (!gap.IsStateGap() || !gap.closure) {
        if (reason)
          *reason = "state-gap edge has no closure";
        return false;
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

    auto buildMixedOwnerTilingWitness =
        [&](const diffutils::Hunk &h, ArrayRef<PartitionEdge> path,
            uint64_t witnessId) -> MixedOwnerTilingWitness {
      MixedOwnerTilingWitness witness;
      witness.witnessId = witnessId;
      witness.originalAStart = h.aStart;
      witness.originalAEnd = h.aEnd;
      witness.originalBStart = h.bStart;
      witness.originalBEnd = h.bEnd;
      witness.stateSummariesComposed = true;
      witness.ownerBoundariesComposed = true;
      witness.targetTokenStreamComposed = true;
      witness.compositionEdgesProven = true;
      witness.globalTargetPPTokenSignature =
          llvm::formatv("B=[{0},{1}):hash={2}", h.bStart, h.bEnd,
                        RefoldWitnessTrace::FormatWitnessTraceHash(
                            deps_.sourceMapper.SliceBSource(h.bStart, h.bEnd)))
              .str();
      witness.segments.reserve(path.size());

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
        if (edge.closure) {
          proof.before = edge.closure->stateIn;
          proof.after = edge.closure->stateOut;
        }
        return proof;
      };

      uint32_t segmentIndex = 0;
      std::string compositionStorage;
      llvm::raw_string_ostream compositionOS(compositionStorage);
      compositionOS << "tiling=" << witnessId << ":A=[" << h.aStart << ','
                    << h.aEnd << "):B=[" << h.bStart << ',' << h.bEnd << ")";
      for (const PartitionEdge &edge : path) {
        const MixedOwnerTilingEdgeKind edgeKind =
            edge.IsStateGap() ? MixedOwnerTilingEdgeKind::StateGap
                              : MixedOwnerTilingEdgeKind::TokenSegment;

        MixedOwnerTilingSegmentWitness segmentWitness;
        segmentWitness.parentTilingWitnessId = witnessId;
        segmentWitness.segmentIndex = segmentIndex;
        segmentWitness.kind = edgeKind;
        segmentWitness.aStart = edge.aStart;
        segmentWitness.aEnd = edge.aEnd;
        segmentWitness.bStart = edge.bStart;
        segmentWitness.bEnd = edge.bEnd;
        segmentWitness.zeroTokenStateGap = edge.IsStateGap();
        segmentWitness.allowEmptyBEnvelope = edge.allowEmptyBEnvelope;
        segmentWitness.ownerTransitionProof = buildOwnerTransitionProof(edge);
        segmentWitness.targetPPTokenSignature =
            formatBTokenSignature(edge.bStart, edge.bEnd);
        if (edge.closure) {
          segmentWitness.ownerClosureComplete = edge.closure->IsComplete();
          segmentWitness.ownerSignature =
              formatOwnerSignature(edge.closure->owner);
          segmentWitness.sourceSignature =
              formatSourceSignature(edge.closure->source);
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
                      << ":owner=" << segmentWitness.ownerSignature
                      << ":source=" << segmentWitness.sourceSignature
                      << ":producer=" << segmentWitness.producerPathSignature
                      << '}';

        if (edge.IsStateGap()) {
          ++witness.stateGapCount;
        } else {
          ++witness.tokenSegmentCount;
        }
        witness.segments.push_back(std::move(segmentWitness));
        ++segmentIndex;
      }
      compositionOS.flush();
      witness.globalCompositionSignature =
          RefoldWitnessTrace::FormatWitnessTraceHash(compositionStorage);
      return witness;
    };

    auto buildClosedStateGapTransition = [&](const diffutils::Hunk &h,
                                             const PartitionEdge *prev,
                                             const PartitionEdge &cur)
        -> std::optional<SmallVector<PartitionEdge, 4>> {
      SmallVector<PartitionEdge, 4> gaps;

      // The first token segment has no predecessor, and token owners from
      // incomparable source sites do not create a proof obligation here.  They
      // are still checked later by the ordinary owner-realization proofs.
      if (!prev)
        return gaps;
      if (!prev->IsTokenSegment() || !cur.IsTokenSegment() || !prev->closure ||
          !cur.closure)
        return std::nullopt;
      if (!sourceSitesComparable(prev->closure->source, cur.closure->source) ||
          cur.closure->source.begin < prev->closure->source.end)
        return gaps;

      OwnerSourceRange gapSource = OwnerSourceRange::From(
          prev->closure->source.path, prev->closure->source.end,
          cur.closure->source.begin, prev->closure->source.includeId);
      SmallVector<PartitionEdge, 8> collected =
          collectZeroTokenStateGaps(gapSource, prev->aEnd, prev->bEnd);

      std::string coverageReason;
      if (!sourceGapIsFullyCovered(gapSource, collected, &coverageReason)) {
        return std::nullopt;
      }

      uint64_t lastGapEnd = gapSource.begin;
      for (const PartitionEdge &gap : collected) {
        if (!gap.closure || gap.closure->source.begin < lastGapEnd) {
          return std::nullopt;
        }
        lastGapEnd = gap.closure->source.end;

        std::string reason;
        if (!stateGapComposesSafely(gap, &reason)) {
          return std::nullopt;
        }
        gaps.push_back(gap);
      }

      return gaps;
    };

    // boundary: failures while *searching* for a mixed-owner
    // partition are non-applicability, not terminal proof failures.  Until a
    // partition has been accepted and durable segment witnesses have been
    // emitted, the ordinary macro/include/TU owner-realization paths still own
    // the hunk.  Returning std::nullopt here therefore preserves the existing
    // lattice ordering instead of prematurely forcing raw-B terminal output.

    auto tryBuildMixedOwnerPartition = [&](const diffutils::Hunk &h)
        -> std::optional<SmallVector<PartitionEdge, 8>> {
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

          uint64_t edgeBStart = h.bStart;
          uint64_t edgeBEnd = h.bStart;
          if (!deleteOnlyHunk) {
            auto env =
                deps_.sourceMapper
                    .MapATokRangeAToBTokenEnvelopeTrimEdgeInsertions(aLo, aHi);
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

          PartitionEdge edge;
          edge.aStart = aLo;
          edge.aEnd = aHi;
          edge.bStart = edgeBStart;
          edge.bEnd = edgeBEnd;
          edge.realizer = realizer;
          edge.allowEmptyBEnvelope = deleteOnlyHunk;
          edge.closure = buildTokenSegmentClosure(edge);
          if (!edge.closure)
            continue;

          const size_t edgeIndex = edges.size();
          edges.push_back(edge);
          edgesByAOffset[static_cast<size_t>(aLo - h.aStart)].push_back(
              edgeIndex);
        }
      }

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
                buildClosedStateGapTransition(h, prevEdge, edge);
            if (!transitionGaps)
              continue;

            PartitionStateKey nextKey;
            nextKey.bPos = edge.bEnd;
            nextKey.lastRealizer = edge.realizer;
            nextKey.lastTokenEdgeIndex = edgeIndex;

            if (key.firstRealizer.kind == HunkRealizerKind::Unknown) {
              nextKey.firstRealizer = edge.realizer;
              nextKey.mixed = false;
            } else {
              // Do not allow adjacent segments with the same realizer. Such
              // fragments should have been represented by one wider edge, and
              // permitting them can manufacture artificial partitions.
              if (key.lastRealizer == edge.realizer)
                continue;
              nextKey.firstRealizer = key.firstRealizer;
              nextKey.mixed = key.mixed || edge.realizer != key.firstRealizer;
            }

            auto &dst = dp[static_cast<size_t>(edge.aEnd - h.aStart)];
            const unsigned gapEdgeCost =
                static_cast<unsigned>(transitionGaps->size());
            const unsigned nextCost = curCost + 1 + gapEdgeCost;
            auto existing = dst.find(nextKey);
            if (existing == dst.end() || nextCost < existing->second.cost) {
              PartitionParent parent;
              parent.valid = true;
              parent.edgeIndex = edgeIndex;
              parent.prev = key;
              parent.cost = nextCost;
              parent.stateGapsBeforeEdge = *transitionGaps;
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
        if (key.bPos != h.bEnd || !key.mixed)
          continue;

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

      // Reconstruct the unique lowest-cost mixed-realizer path. The DP tracks
      // mixedness as part of the state, rather than choosing the cheapest path
      // first and checking mixedness afterwards. This prevents a coarse TU edge
      // from swallowing a smaller macro/include segment and suppressing a valid
      // structure-preserving split. Equal-cost alternatives are rejected above
      // instead of being hidden behind deterministic map/edge iteration order.
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

      // Re-validate the reconstructed partition as a true mixed-owner tiling.
      // The dynamic-programming search already found a path, but the proof
      // obligation is stronger: each edge must start exactly where the previous
      // edge ended on both A and B, must consume a non-empty A-token envelope,
      // and must have a known realizer.  Replace-hunk token segments also
      // consume a non-empty B envelope; delete-only token segments instead
      // prove the empty B envelope at the deletion boundary.
      uint64_t expectedA = h.aStart;
      uint64_t expectedB = h.bStart;
      std::optional<HunkRealizer> firstRealizer;
      bool sawDifferentRealizer = false;
      for (const PartitionEdge &edge : path) {
        if (edge.IsStateGap()) {
          if (edge.aStart != expectedA || edge.aEnd != expectedA ||
              edge.bStart != expectedB || edge.bEnd != expectedB ||
              !edge.closure || !edge.closure->IsComplete()) {
            return std::nullopt;
          }
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
        expectedA = edge.aEnd;
        expectedB = edge.bEnd;
      }

      // A mixed-owner tiling is admissible only if the edge sequence covers the
      // entire original hunk and actually crosses an owner/realizer boundary.
      // Otherwise this is either an incomplete cover or a single-owner case
      // that should be handled by the ordinary owner-specific realization
      // machinery.
      if (expectedA != h.aEnd || expectedB != h.bEnd || !sawDifferentRealizer) {
        return std::nullopt;
      }

      if (deleteOnlyHunk) {
        // may split a delete-only hunk only when the split is a pure
        // token-cover proof.  Unlike a replacement hunk, every emitted segment
        // has the same empty B envelope, so the split itself cannot replay or
        // repair source-state transitions that are semantically tied to the
        // deleted span.  Stateful delete closures such as `#pragma once`
        // reactivation, include-guard changes, macro-state repair, or preserved
        // `#line` gaps must remain with the older owner-closure/state-repair
        // machinery where the whole deletion boundary is visible.  Therefore a
        // delete-only mixed tiling is admissible here only when no split edge
        // carries a state mutation and there are no intervening state-gap
        // proofs.  Pure observations, such as an ordinary macro invocation
        // depending on the current macro table, do not by themselves make the
        // deletion stateful; they remain subject to the owner-specific proofs
        // that consume each emitted token segment.
        for (const PartitionEdge &edge : path) {
          if (!edge.closure)
            return std::nullopt;
          if (edge.IsStateGap() ||
              deps_.ownerStateProof.OwnerStateDeltaMutatesAnyState(
                  edge.closure->stateIn) ||
              deps_.ownerStateProof.OwnerStateDeltaMutatesAnyState(
                  edge.closure->stateOut)) {
            return std::nullopt;
          }
        }
      }

      std::string stateCompositionReason;
      if (!mixedOwnerTilingStateSummariesCompose(h, path,
                                                 &stateCompositionReason)) {
        return std::nullopt;
      }

      return path;
    };

    bool changed = true;
    while (changed) {
      changed = false;
      std::vector<diffutils::Hunk> splitHunks;
      splitHunks.reserve(hunks.size());

      for (const auto &h : hunks) {
        auto partition = tryBuildMixedOwnerPartition(h);
        if (!partition) {
          splitHunks.push_back(h);
          continue;
        }

        // persists the full ordered proof path before lowering the partition
        // back into ordinary token hunks.  Each emitted token segment gets a
        // reverse binding to this witness so later macro/include/TU accepted
        // candidates can report the mixed-owner proof that justified the split,
        // including state-gap edges that are not emitted.
        const size_t mixedWitnessIndex = deps_.mixedOwnerTilingWitnesses.size();
        const uint64_t mixedWitnessId =
            static_cast<uint64_t>(mixedWitnessIndex) + 1;
        deps_.mixedOwnerTilingWitnesses.push_back(
            buildMixedOwnerTilingWitness(h, *partition, mixedWitnessId));

        uint32_t mixedSegmentIndex = 0;
        for (const PartitionEdge &edge : *partition) {
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
