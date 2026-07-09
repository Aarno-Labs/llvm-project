//===--- RefoldIncludeMaterializationScheduler.cpp --------------*- C++ -*-===//
//
// Include materialization scheduling for clang-refold.
//
//===----------------------------------------------------------------------===//

#include "include/RefoldIncludeMaterializationScheduler.h"

#include "core/RefoldLog.h"
#include "edit/RefoldTextEditAssembler.h"
#include "include/RefoldIncludeInsertionPlanner.h"
#include "include/RefoldIncludeMaterializer.h"
#include "line-control/LineDirectiveInserter.h"
#include "line-control/RefoldLineObserverLayout.h"
#include "macro/RefoldMacroStateRepairPlanner.h"
#include "proof/RefoldProofLattice.h"
#include "proof/RefoldSidebandReplayProof.h"
#include "proof/RefoldTheoremAudit.h"
#include "source/RefoldStructuralHunkDispatcher.h"
#include "util/RefoldPathIdentity.h"
#include "util/StringUtils.h"
#include "llvm/ADT/STLExtras.h"

#include <algorithm>
#include <cassert>
#include <utility>

using namespace llvm;

namespace clang {
namespace refold {

namespace {

template <typename T> T &requireNonNull(T *ptr, const char *message) {
  assert(ptr && message);
  (void)message;
  return *ptr;
}

} // namespace

RefoldIncludeMaterializationScheduler::RefoldIncludeMaterializationScheduler(
    Dependencies deps, IncludeMaterializationRequest request)
    : deps_(std::move(deps)), request_(std::move(request)),
      model_(requireNonNull(deps_.model,
                            "include scheduler requires RefoldModel")),
      pathIdentity_(requireNonNull(deps_.pathIdentity,
                                   "include scheduler requires path identity")),
      includeMaterializer_(
          requireNonNull(deps_.includeMaterializer,
                         "include scheduler requires include materializer")),
      includeInsertionPlanner_(requireNonNull(
          deps_.includeInsertionPlanner,
          "include scheduler requires include insertion planner")),
      lineObserverLayout_(
          requireNonNull(deps_.lineObserverLayout,
                         "include scheduler requires line observer layout")),
      macroStateRepairPlanner_(requireNonNull(
          deps_.macroStateRepairPlanner,
          "include scheduler requires macro-state repair planner")),
      textEditAssembler_(
          requireNonNull(deps_.textEditAssembler,
                         "include scheduler requires text edit assembler")),
      proofLattice_(requireNonNull(deps_.proofLattice,
                                   "include scheduler requires proof lattice")),
      terminalSink_(requireNonNull(deps_.terminalSink,
                                   "include scheduler requires terminal sink")),
      sidebandPragmaEdits_(requireNonNull(
          deps_.sidebandPragmaEdits,
          "include scheduler requires sideband pragma edit list")),
      structuralHunkDispatcher_(
          requireNonNull(request_.structuralHunkDispatcher,
                         "include scheduler requires structural dispatcher")),
      tuEdits_(structuralHunkDispatcher_.MutableTUEditsForRepairAndEmission()) {
  BuildChildrenIndex();
}

bool RefoldIncludeMaterializationScheduler::MaterializeIncludeExpansions() {
  BuildLayoutOnlyIncludeMaterializationSeeds();

  DenseSet<uint64_t> seeds = BuildInitialMaterializationSeeds();
  AddAncestorMaterializationSeeds(seeds);
  SmallVector<uint64_t, 32> orderedSeeds = OrderedMaterializationSeeds(seeds);
  MaterializeOrderedSeeds(orderedSeeds);

  // Global fail-closed composition rule: if include realization requested the
  // terminal fallback in this single pass, stop here rather than continuing to
  // compose or return mixed structural artifacts.
  if (terminalSink_.HasRequest()) {
    REFOLD_LOG_DEBUG("fallback",
                     "single-pass refold aborted after include "
                     "materialization; terminal fallback will be emitted");
    return false;
  }

  RebuildExpandedIncludeIds();
  return true;
}

bool RefoldIncludeMaterializationScheduler::StageTURootIncludeExpansionEdits(
    RefoldMacroStateRepairPlanner::MacroStateRepairPlan &macroStatePlan,
    const RefoldMacroStateRepairPlanner::MacroStateRepairRequest
        &macroStateRequest) {
  // Determinism: includeExpansion_ is a DenseMap, so iterate by sorted id.
  SmallVector<uint64_t, 32> includeIds;
  includeIds.reserve(includeExpansion_.size());
  for (const auto &kv : includeExpansion_)
    includeIds.push_back(kv.first);
  llvm::sort(includeIds);

  for (uint64_t includeId : includeIds) {
    if (!StageTURootIncludeExpansionEdit(includeId, macroStatePlan,
                                         macroStateRequest))
      return false;
  }
  return true;
}

const DenseSet<uint64_t> &
RefoldIncludeMaterializationScheduler::ExpandedIncludeIds() const {
  return expandedIncludeIds_;
}

size_t RefoldIncludeMaterializationScheduler::MaterializedIncludeCount() const {
  return includeExpansion_.size();
}

void RefoldIncludeMaterializationScheduler::BuildChildrenIndex() {
  for (const RefoldModel::IncludeItem &include : model_.GetIncludes()) {
    if (include.parent)
      children_[*include.parent].push_back(&include);
  }
}

const RefoldModel::MacroDirective *
RefoldIncludeMaterializationScheduler::FindMacroDirectiveById(
    uint64_t id) const {
  for (const RefoldModel::MacroDirective &directive :
       model_.GetMacroDirectives()) {
    if (directive.id == id)
      return &directive;
  }
  return nullptr;
}

bool RefoldIncludeMaterializationScheduler::
    DefinitionIsSuppliedByImmediateIncluder(
        const RefoldModel::IncludeItem &include,
        const RefoldModel::MacroDirective &definition) const {
  if (definition.subkind != "#define")
    return false;
  if (!pathIdentity_.PathsEqual(definition.sitePath, include.sitePath))
    return false;
  if (definition.siteE > include.siteB)
    return false;
  return definition.ownerIncludeId == include.parent;
}

bool RefoldIncludeMaterializationScheduler::
    IncludeHasIncluderSuppliedLineControlMacroState(
        const RefoldModel::IncludeItem &include) const {
  for (const RefoldModel::LineControlEvent &event : model_.GetLineControls()) {
    if (!event.active || !event.producerProven)
      continue;
    if (event.ownerIncludeId != std::optional<uint64_t>(include.id))
      continue;
    if (!event.siteB || !event.siteE)
      continue;

    for (const RefoldModel::MacroInvocation &macro :
         model_.GetMacroInvocations()) {
      if (macro.ownerIncludeId != std::optional<uint64_t>(include.id))
        continue;
      if (!macro.invB || !macro.invE || !macro.definitionDirectiveId)
        continue;
      if (*macro.invB < *event.siteB || *macro.invE > *event.siteE)
        continue;

      const RefoldModel::MacroDirective *definition =
          FindMacroDirectiveById(*macro.definitionDirectiveId);
      if (!definition)
        continue;
      if (DefinitionIsSuppliedByImmediateIncluder(include, *definition))
        return true;
    }
  }

  return false;
}

uint64_t RefoldIncludeMaterializationScheduler::TokenEndOffset(
    ArrayRef<PPTok> tokens, ArrayRef<size_t> tokenOffsets, size_t index) {
  return static_cast<uint64_t>(tokenOffsets[index] +
                               tokens[index].spelling.size());
}

std::optional<uint64_t>
RefoldIncludeMaterializationScheduler::FindTokenGapContainingByteRange(
    StringRef source, ArrayRef<PPTok> tokens, ArrayRef<size_t> tokenOffsets,
    uint64_t begin, uint64_t end) const {
  if (begin > end || end > source.size())
    return std::nullopt;

  const size_t tokenCount = tokens.size();
  if (tokenOffsets.size() < tokenCount + 1)
    return std::nullopt;

  size_t gap = 0;
  while (gap < tokenCount && static_cast<uint64_t>(tokenOffsets[gap]) < end)
    ++gap;

  const uint64_t gapBegin =
      gap == 0 ? 0 : TokenEndOffset(tokens, tokenOffsets, gap - 1);
  const uint64_t gapEnd = gap == tokenCount
                              ? static_cast<uint64_t>(source.size())
                              : static_cast<uint64_t>(tokenOffsets[gap]);

  if (begin < gapBegin || end > gapEnd)
    return std::nullopt;
  return static_cast<uint64_t>(gap);
}

std::optional<uint64_t>
RefoldIncludeMaterializationScheduler::LayoutOnlyIncludeSeedForRawByteHunk(
    const diffutils::Hunk &hunk) const {
  if (hunk.aStart > hunk.aEnd || hunk.bStart > hunk.bEnd ||
      hunk.aEnd > request_.aSource.size() ||
      hunk.bEnd > request_.bSource.size())
    return std::nullopt;

  StringRef aSlice = request_.aSource.slice(static_cast<size_t>(hunk.aStart),
                                            static_cast<size_t>(hunk.aEnd));
  StringRef bSlice = request_.bSource.slice(static_cast<size_t>(hunk.bStart),
                                            static_cast<size_t>(hunk.bEnd));
  if (!aSlice.trim().empty() || !bSlice.trim().empty())
    return std::nullopt;

  std::optional<uint64_t> aGap = FindTokenGapContainingByteRange(
      request_.aSource, request_.aTokens, request_.aTokenOffsets, hunk.aStart,
      hunk.aEnd);
  std::optional<uint64_t> bGap = FindTokenGapContainingByteRange(
      request_.bSource, request_.bTokens, request_.bTokenOffsets, hunk.bStart,
      hunk.bEnd);
  if (!aGap || !bGap)
    return std::nullopt;

  const uint64_t tokenCount = static_cast<uint64_t>(request_.aTokens.size());
  std::optional<uint64_t> leftInclude =
      *aGap > 0 ? model_.InnermostIncludeAtPP(*aGap - 1) : std::nullopt;
  std::optional<uint64_t> rightInclude =
      *aGap < tokenCount ? model_.InnermostIncludeAtPP(*aGap) : std::nullopt;

  // Prefix/suffix gaps with a single include neighbor are include-edge layout.
  // Interior gaps whose two sides share the same include are include-local
  // layout.  For nested include boundaries, use the LCA owner so the child edge
  // is repaired on the enclosing materialized surface.  If the only common
  // owner is the TU, leave the hunk unclaimed; choosing left or right would be
  // an implementation-order heuristic.
  if (leftInclude && rightInclude) {
    if (*leftInclude == *rightInclude)
      return leftInclude;
    return model_.LeastCommonAncestorInclude(leftInclude, rightInclude);
  }
  if (leftInclude)
    return leftInclude;
  if (rightInclude)
    return rightInclude;
  return std::nullopt;
}

void RefoldIncludeMaterializationScheduler::
    BuildLayoutOnlyIncludeMaterializationSeeds() {
  layoutOnlyIncludeMaterializationSeeds_.clear();

  // Token diff hunks describe edits to PP-token spellings.  Raw byte hunks
  // additionally expose token-empty layout edits that occur in PP gaps.  A gap
  // can be structurally adjacent to an include edge even when no PP token was
  // edited.  In the byte-only case, preserving the `#include` directive may
  // force Clang to regenerate caller-edge `-E -P` layout that the modified PP
  // stream has explicitly removed, while materializing that include site emits
  // the selected header bytes directly.
  //
  // The important proof boundary is "byte-only".  Once a normal token hunk, a
  // sideband pragma edit, a TU text edit, an include patch, or a macro patch
  // already explains part of the modified stream, an adjacent whitespace hunk
  // is not by itself an owner witness for a clean neighboring include. Treating
  // it as one would be a non-minimal heuristic: leading/trailing `-E -P`
  // newline drift commonly appears around otherwise valid repairs and does not
  // prove that the first or last include must be opened.  Composite layout
  // edits need a real tiling proof before they may force extra materialization;
  // this fallback handles only the fully byte-only surface where no stronger
  // structural repair exists.
  const bool mayUseByteOnlyIncludeLayoutSeed =
      request_.tokenHunks.empty() && sidebandPragmaEdits_.empty() &&
      tuEdits_.empty() &&
      !structuralHunkDispatcher_.IncludeBucketsHavePatches() &&
      !structuralHunkDispatcher_.MacroBucketsHavePatches();

  if (request_.rawByteHunks) {
    for (const diffutils::Hunk &byteHunk : *request_.rawByteHunks) {
      std::optional<uint64_t> seed =
          LayoutOnlyIncludeSeedForRawByteHunk(byteHunk);
      if (!seed)
        continue;

      const RefoldModel::IncludeItem *seedInclude =
          model_.GetIncludeById(*seed);
      const bool lineControlIncludeEdge =
          seedInclude &&
          IncludeHasIncluderSuppliedLineControlMacroState(*seedInclude);

      // Ordinary mixed token/layout edits still must not use neighboring
      // whitespace as a materialization witness: the four lit regressions that
      // motivated the byte-only gate were exactly incidental `-E -P` newline
      // drift next to clean includes while some other owner carried the real
      // edit.  A line-control include edge is different.  Preserving that
      // directive asks Clang to regenerate caller-edge line-control layout, so
      // a raw PP byte hunk in the owned gap is itself an include-site-local
      // obligation even when a sibling or parent token hunk exists.  Keep the
      // proof deterministic by requiring the producer-proven includer-supplied
      // line-control state, rather than selecting arbitrary adjacent includes.
      if (!mayUseByteOnlyIncludeLayoutSeed && !lineControlIncludeEdge)
        continue;

      layoutOnlyIncludeMaterializationSeeds_.insert(*seed);
      REFOLD_LOG_TRACE("include/layout",
                       "seed include materialization from raw layout hunk "
                       "A[{0},{1}) -> B[{2},{3}) inc#{4} byteOnly={5} "
                       "lineControlEdge={6}",
                       byteHunk.aStart, byteHunk.aEnd, byteHunk.bStart,
                       byteHunk.bEnd, *seed, mayUseByteOnlyIncludeLayoutSeed,
                       lineControlIncludeEdge);
    }
  }

  if (inTraceMode() && request_.rawByteHunks &&
      !request_.rawByteHunks->empty() &&
      layoutOnlyIncludeMaterializationSeeds_.empty()) {
    REFOLD_LOG_TRACE(
        "include/layout",
        "skip include layout seeding: tokenHunks={0} sidebandPragmas={1} "
        "tuEdits={2} includePatchBuckets={3} macroPatchBuckets={4} "
        "rawByteHunks={5}",
        request_.tokenHunks.size(), sidebandPragmaEdits_.size(),
        tuEdits_.size(), structuralHunkDispatcher_.IncludeBucketCount(),
        structuralHunkDispatcher_.MacroOwnerBucketCount(),
        request_.rawByteHunks->size());
  }
}

DenseSet<uint64_t>
RefoldIncludeMaterializationScheduler::BuildInitialMaterializationSeeds()
    const {
  DenseSet<uint64_t> seeds;

  // (a) Direct include edits.
  structuralHunkDispatcher_.AddDirectIncludeEditSeeds(seeds);

  // (a.0) Byte-only layout edits at include-owned PP gaps have no token hunk,
  // but can still require the include site to be opened so the final `-E -P`
  // byte stream realizes the selected caller-edge layout.
  seeds.insert(layoutOnlyIncludeMaterializationSeeds_.begin(),
               layoutOnlyIncludeMaterializationSeeds_.end());

  // (a.1) Header-owned sideband pragma edits are zero-normal-token source
  // edits. They do not create an include patch from an A/B hunk, but they still
  // dirty the include instance whose header text contains the pragma. Seed that
  // include so the normal owner-polymorphic materialization path applies the
  // source edit inside the header and then folds the materialized expansion
  // back through its parent include chain.
  for (const SidebandPragmaEdit &sideband : sidebandPragmaEdits_) {
    if (std::optional<uint64_t> owner = sideband.OwnerIncludeId())
      seeds.insert(*owner);
  }

  // (b) Macro-owned work INSIDE headers (ownerIncludeId != null).
  structuralHunkDispatcher_.AddHeaderMacroPatchSeeds(seeds);
  return seeds;
}

void RefoldIncludeMaterializationScheduler::AddAncestorMaterializationSeeds(
    DenseSet<uint64_t> &seeds) const {
  SmallVector<uint64_t, 32> worklist(seeds.begin(), seeds.end());
  for (size_t i = 0; i < worklist.size(); ++i) {
    const RefoldModel::IncludeItem *cur = model_.GetIncludeById(worklist[i]);
    while (cur && cur->parent) {
      uint64_t parentId = *cur->parent;
      auto inserted = seeds.insert(parentId);
      if (!inserted.second)
        break;
      worklist.push_back(parentId);
      cur = model_.GetIncludeById(parentId);
    }
  }
}

unsigned RefoldIncludeMaterializationScheduler::IncludeMaterializationDepth(
    uint64_t includeId) const {
  unsigned depth = 0;
  DenseSet<uint64_t> seen;
  const RefoldModel::IncludeItem *cur = model_.GetIncludeById(includeId);
  while (cur && cur->parent) {
    if (!seen.insert(cur->id).second)
      break;
    ++depth;
    cur = model_.GetIncludeById(*cur->parent);
  }
  return depth;
}

SmallVector<uint64_t, 32>
RefoldIncludeMaterializationScheduler::OrderedMaterializationSeeds(
    const DenseSet<uint64_t> &seeds) const {
  SmallVector<uint64_t, 32> orderedSeeds(seeds.begin(), seeds.end());

  // Materialize each include once.  Process ancestors before descendants rather
  // than iterating the DenseSet directly.  This ordering matters for
  // include-next repair: when an ancestor materialization fails to prove a
  // descendant #include_next replay obligation, the recursive call must be the
  // first one to build/cache that descendant so the forced materialization mode
  // is not bypassed by an earlier child-seed realization.
  llvm::sort(orderedSeeds, [&](uint64_t lhs, uint64_t rhs) {
    const unsigned lhsDepth = IncludeMaterializationDepth(lhs);
    const unsigned rhsDepth = IncludeMaterializationDepth(rhs);
    if (lhsDepth != rhsDepth)
      return lhsDepth < rhsDepth;
    return lhs < rhs;
  });
  return orderedSeeds;
}

void RefoldIncludeMaterializationScheduler::MaterializeOrderedSeeds(
    ArrayRef<uint64_t> orderedSeeds) {
  for (uint64_t includeId : orderedSeeds) {
    includeMaterializer_.MaterializeIncludeExpansion(
        includeId,
        structuralHunkDispatcher_.MutableIncludeEditBucketsForMaterialization(),
        structuralHunkDispatcher_.FinalMacroPatchesByOwnerForMaterialization(),
        children_, includeExpansion_,
        includeExpansionLineControlPruneCandidates_,
        includeExpansionLineControlSourceMappings_,
        includeExpansionStartLineNos_, includeExpansionAcceptedResults_,
        &structuralHunkDispatcher_.MutableAppliedExpandedMacroRootIds());
  }
}

void RefoldIncludeMaterializationScheduler::RebuildExpandedIncludeIds() {
  expandedIncludeIds_.clear();
  for (const auto &kv : includeExpansion_)
    expandedIncludeIds_.insert(kv.first);
}

bool RefoldIncludeMaterializationScheduler::
    IncludeHasLineDirectiveForcingSidebandWork(uint64_t includeId) const {
  return llvm::any_of(sidebandPragmaEdits_,
                      [&](const SidebandPragmaEdit &edit) {
                        return edit.TargetsInclude(includeId) &&
                               edit.ForcesIncludeLineDirectiveWrappers();
                      });
}

bool RefoldIncludeMaterializationScheduler::IncludeHasOrdinaryReplayTokens(
    uint64_t includeId) const {
  if (const RefoldModel::IncludeItem *item = model_.GetIncludeById(includeId))
    return item->cover.end > item->cover.begin;
  return false;
}

bool RefoldIncludeMaterializationScheduler::
    IncludeUsesOnlySidebandReplayEnvelope(uint64_t includeId) const {
  bool sawSideband =
      llvm::any_of(sidebandPragmaEdits_, [&](const SidebandPragmaEdit &edit) {
        return edit.TargetsInclude(includeId);
      });
  if (IncludeHasOrdinaryReplayTokens(includeId))
    return false;
  if (structuralHunkDispatcher_.HasIncludePatchesFor(includeId))
    return false;
  if (structuralHunkDispatcher_.HasFinalMacroPatchesForOwner(
          std::optional<uint64_t>(includeId)))
    return false;
  if (auto childIt = children_.find(includeId); childIt != children_.end()) {
    for (const RefoldModel::IncludeItem *child : childIt->second) {
      if (!IncludeUsesOnlySidebandReplayEnvelope(child->id))
        return false;
      sawSideband = true;
    }
  }
  return sawSideband;
}

RefoldIncludeMaterializationScheduler::TUIncludeMaterializationWorkClass
RefoldIncludeMaterializationScheduler::ClassifyTUIncludeMaterializationWork(
    uint64_t includeId) const {
  if (layoutOnlyIncludeMaterializationSeeds_.contains(includeId))
    return TUIncludeMaterializationWorkClass::Ordinary;

  bool sawSideband =
      llvm::any_of(sidebandPragmaEdits_, [&](const SidebandPragmaEdit &edit) {
        return edit.TargetsInclude(includeId);
      });
  if (IncludeHasLineDirectiveForcingSidebandWork(includeId))
    return TUIncludeMaterializationWorkClass::Ordinary;
  if (sawSideband && IncludeHasOrdinaryReplayTokens(includeId))
    return TUIncludeMaterializationWorkClass::Ordinary;
  if (structuralHunkDispatcher_.HasIncludePatchesFor(includeId))
    return TUIncludeMaterializationWorkClass::Ordinary;
  if (structuralHunkDispatcher_.HasFinalMacroPatchesForOwner(
          std::optional<uint64_t>(includeId)))
    return TUIncludeMaterializationWorkClass::Ordinary;
  if (auto childIt = children_.find(includeId); childIt != children_.end()) {
    for (const RefoldModel::IncludeItem *child : childIt->second) {
      switch (ClassifyTUIncludeMaterializationWork(child->id)) {
      case TUIncludeMaterializationWorkClass::Ordinary:
        return TUIncludeMaterializationWorkClass::Ordinary;
      case TUIncludeMaterializationWorkClass::SidebandPragmaOnly:
        sawSideband = true;
        break;
      case TUIncludeMaterializationWorkClass::None:
        break;
      }
    }
  }
  return sawSideband ? TUIncludeMaterializationWorkClass::SidebandPragmaOnly
                     : TUIncludeMaterializationWorkClass::None;
}

bool RefoldIncludeMaterializationScheduler::
    IncludeSubtreeHasLayoutOnlyMaterializationSeed(uint64_t includeId) const {
  if (layoutOnlyIncludeMaterializationSeeds_.contains(includeId))
    return true;
  if (auto childIt = children_.find(includeId); childIt != children_.end()) {
    for (const RefoldModel::IncludeItem *child : childIt->second) {
      if (child && IncludeSubtreeHasLayoutOnlyMaterializationSeed(child->id))
        return true;
    }
  }
  return false;
}

std::pair<uint64_t, uint64_t>
RefoldIncludeMaterializationScheduler::ExtendedTUSiteRange(
    const RefoldModel::IncludeItem &include) const {
  // The producer's [siteB, siteE) range is supposed to cover the entire
  // physical `#include` directive in the TU. In some cases involving leading
  // line splices just before the directive, that recorded end can stop too
  // early. If we replace only the truncated range, part of the original
  // `#include` can remain in the TU, and checker replay may include the header
  // again.
  uint64_t siteBegin = include.siteB;
  uint64_t siteEnd = include.siteE;
  if (siteBegin < request_.tuBytes.size()) {
    size_t i = static_cast<size_t>(siteBegin);
    while (true) {
      size_t nl = request_.tuBytes.find('\n', i);
      if (nl == StringRef::npos) {
        siteEnd = request_.tuBytes.size();
        break;
      }
      i = nl + 1;
      if (!stringutils::isLineSplice(request_.tuBytes, nl)) {
        uint64_t extended = static_cast<uint64_t>(i);
        if (extended > siteEnd)
          siteEnd = extended;
        break;
      }
    }
  }
  return {siteBegin, siteEnd};
}

bool RefoldIncludeMaterializationScheduler::TryPreserveSourceGraphOutput(
    const RefoldModel::IncludeItem &include, StringRef expansionText) const {
  if (!request_.sourceGraphOutputs)
    return false;

  source_graph::SourceGraphProofInputs sourceGraphProofInputs{
      model_, request_.tuPath, includeExpansion_};

  // function_ref does not own callable storage.  Keep the callback objects in
  // this scope so the proof-service bundle never observes dangling lambdas.
  auto includeHasIncluderSuppliedLineControlMacroState =
      [&](const RefoldModel::IncludeItem &item) {
        return IncludeHasIncluderSuppliedLineControlMacroState(item);
      };
  auto includeSubtreeHasLayoutOnlyMaterializationSeed =
      [&](uint64_t includeId) {
        return IncludeSubtreeHasLayoutOnlyMaterializationSeed(includeId);
      };
  source_graph::SourceGraphProofServices sourceGraphProofServices{
      pathIdentity_, includeHasIncluderSuppliedLineControlMacroState,
      includeSubtreeHasLayoutOnlyMaterializationSeed};

  source_graph::SourceGraphOwnerPreservationOutputPlan sourceGraphPlan =
      source_graph::planSourceGraphOwnerPreservationOutput(
          sourceGraphProofInputs, include, expansionText,
          sourceGraphProofServices);
  if (sourceGraphPlan.rejectedCleanupOutput)
    request_.sourceGraphOutputs->push_back(
        std::move(*sourceGraphPlan.rejectedCleanupOutput));
  if (!sourceGraphPlan.preservedOutput)
    return false;

  request_.sourceGraphOutputs->push_back(
      std::move(*sourceGraphPlan.preservedOutput));
  return true;
}

bool RefoldIncludeMaterializationScheduler::StageTURootIncludeExpansionEdit(
    uint64_t includeId,
    RefoldMacroStateRepairPlanner::MacroStateRepairPlan &macroStatePlan,
    const RefoldMacroStateRepairPlanner::MacroStateRepairRequest
        &macroStateRequest) {
  auto expansionIt = includeExpansion_.find(includeId);
  if (expansionIt == includeExpansion_.end())
    return true;

  const RefoldModel::IncludeItem *include = model_.GetIncludeById(includeId);
  if (!include)
    return true;

  // Only the translation-unit root include is rewritten here. Nested includes
  // are folded into the already-materialized parent expansion.
  if (include->parent ||
      !pathIdentity_.PathsEqual(include->sitePath, request_.tuPath))
    return true;

  std::string expansionText = expansionIt->second;
  auto [siteBegin, siteEnd] = ExtendedTUSiteRange(*include);

  // If the source-graph writer can preserve this root include without replacing
  // the site, it owns the output and no structural TU edit is needed.
  if (TryPreserveSourceGraphOutput(*include, expansionText))
    return true;

  // Repair consumed macro-state directives before wrapping the expansion so the
  // final TU edit carries the macro state required by the materialized header.
  if (!macroStateRepairPlanner_.RepairConsumedDefinitionsForMaterializedInclude(
          macroStatePlan, macroStateRequest, *include, siteBegin, siteEnd,
          expansionText)) {
    return false;
  }

  LineDirectiveLocation parentResume =
      LineDirectiveInserter::LogicalLocationAtOffset(
          request_.tuBytes, siteEnd, request_.tuPath, model_, request_.tuPath);
  const bool sidebandOnly =
      ClassifyTUIncludeMaterializationWork(include->id) ==
      TUIncludeMaterializationWorkClass::SidebandPragmaOnly;
  const size_t childEntryLineNo =
      includeExpansionStartLineNos_.lookup(include->id);

  // Forward any line-control pruning and source-mapping evidence produced while
  // materializing the include body into the final TU wrapper.
  ArrayRef<FinalLineControlPruneCandidate> includeLineCandidates;
  if (auto includeCandidatesIt =
          includeExpansionLineControlPruneCandidates_.find(include->id);
      includeCandidatesIt !=
      includeExpansionLineControlPruneCandidates_.end()) {
    includeLineCandidates = includeCandidatesIt->second;
  }

  ArrayRef<FinalLineControlSourceMapping> includeLineSourceMappings;
  if (auto includeMappingsIt =
          includeExpansionLineControlSourceMappings_.find(include->id);
      includeMappingsIt != includeExpansionLineControlSourceMappings_.end()) {
    includeLineSourceMappings = includeMappingsIt->second;
  }

  LineControlWrappedText wrapped =
      lineObserverLayout_.WrapIncludeExpansionForMaterialization(
          *include, parentResume.fileSpelling, request_.tuPath, std::nullopt,
          siteEnd, childEntryLineNo ? childEntryLineNo : 1, parentResume.lineNo,
          expansionText, includeLineCandidates, includeLineSourceMappings,
          sidebandOnly);

  TextEdit edit{siteBegin,
                siteEnd,
                std::move(wrapped.text),
                std::nullopt,
                std::nullopt,
                {},
                {},
                {}};
  edit.lineControlPruneCandidates =
      std::move(wrapped.lineControlPruneCandidates);
  edit.lineControlSourceMappings = std::move(wrapped.lineControlSourceMappings);

  // Certify the materialized-B extent carried by the edit. Prefer the full
  // include realization envelope, but allow sideband-only materializations to
  // certify the narrower sideband pragma replay range.
  auto acceptedIt = includeExpansionAcceptedResults_.find(includeId);
  if (auto bEnv =
          includeInsertionPlanner_.ResolveIncludeRealizationBTokenEnvelope(
              include->cover.begin, include->cover.end)) {
    textEditAssembler_.CertifyTextEditMaterializedBTokenRange(edit, bEnv->first,
                                                              bEnv->second);
  } else if (sidebandOnly ||
             IncludeUsesOnlySidebandReplayEnvelope(include->id)) {
    if (auto sidebandBRange =
            textEditAssembler_.SidebandPragmaMaterializedBByteRangeForInclude(
                include->id)) {
      textEditAssembler_.CertifyTextEditMaterializedBByteRange(
          edit, sidebandBRange->first, sidebandBRange->second);
    }
  }

  // Preserve the accepted-result proof produced by the include materializer
  // when available; otherwise certify the edit as include materialized
  // expansion.
  if (acceptedIt != includeExpansionAcceptedResults_.end()) {
    textEditAssembler_.AttachAcceptedResultCarrier(edit, acceptedIt->second);
  } else {
    textEditAssembler_.AttachAcceptedResultCarrier(
        edit,
        proofLattice_.AcceptedCandidateBuilder()
            .BuildAcceptedIncludeRealizationCandidate(
                AcceptedPathKind::IncludeMaterializedExpansion, *include));
  }

  structuralHunkDispatcher_.AddTUEdit(std::move(edit));
  return true;
}

} // namespace refold
} // namespace clang
