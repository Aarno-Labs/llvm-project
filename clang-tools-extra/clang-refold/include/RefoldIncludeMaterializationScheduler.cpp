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
#include "include/RefoldPragmaOnceGuardRewriter.h"
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
#include "llvm/ADT/StringSet.h"

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
      pragmaOnceGuards_(
          requireNonNull(deps_.pragmaOnceGuards,
                         "include scheduler requires pragma-once guards")),
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
  AddGuardReentryMaterializationSeeds(seeds);
  AddAncestorMaterializationSeeds(seeds);
  SmallVector<uint64_t, 32> orderedSeeds = OrderedMaterializationSeeds(seeds);
  // The guard catalog must know which physical headers may be inlined before any
  // body text is built, because a clean include can need wrapping purely because
  // another instance of the same physical header was inlined elsewhere.
  RecordActiveGuardedHeaders(orderedSeeds);
  MaterializeOrderedSeeds(orderedSeeds);

  // A body realized from B carries no directives, so any header whose content it
  // emitted has lost its own include-guard protection.  This must be proven
  // before the realized text is committed.
  if (!ProveNoReentryIntoUnprotectedInlinedHeaders()) {
    REFOLD_LOG_DEBUG("fallback",
                     "single-pass refold aborted: a surviving include can "
                     "re-enter an inlined header that lost its include guard");
    return false;
  }

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

void RefoldIncludeMaterializationScheduler::CollectSubtreePhysicalPaths(
    const DenseSet<uint64_t> &seeds, std::vector<std::string> &paths) const {
  paths.clear();
  DenseSet<uint64_t> visited;
  SmallVector<uint64_t, 32> worklist(seeds.begin(), seeds.end());

  while (!worklist.empty()) {
    const uint64_t includeId = worklist.pop_back_val();
    if (!visited.insert(includeId).second)
      continue;
    if (const RefoldModel::IncludeItem *include =
            model_.GetIncludeById(includeId))
      if (include->openedPath && !include->openedPath->empty())
        paths.push_back(include->openedPath->str());
    if (auto it = children_.find(includeId); it != children_.end())
      for (const RefoldModel::IncludeItem *child : it->second)
        worklist.push_back(child->id);
  }

  llvm::sort(paths);
  paths.erase(std::unique(paths.begin(), paths.end()), paths.end());
}

void RefoldIncludeMaterializationScheduler::AddGuardReentryMaterializationSeeds(
    DenseSet<uint64_t> &seeds) const {
  // A guard lives in TU byte-space, so it cannot reach a re-entry that happens
  // *inside* an unmodified header: wrapping `#include "outer.h"` protects
  // `outer.h`, but `outer.h` on disk may include an already-inlined once-header,
  // whose real `#pragma once` never fired because the inlined copy was spliced
  // as text rather than included.  clang-refold never edits headers, so the only
  // sound response is to materialize the offending include as well.  Its nested
  // directive then becomes a surviving include inside materialized text, which
  // the ordinary wrapper *can* guard.
  //
  // Materializing an include enlarges the inlined set, which can expose further
  // re-entries, so this iterates to a fixed point.  Termination is guaranteed:
  // every round either adds an include edge or stops, and the edge set is finite.
  std::vector<std::string> onceHeaderPaths;
  std::vector<std::string> inlinedPaths;

  for (unsigned round = 0;; ++round) {
    CollectSubtreePhysicalPaths(seeds, inlinedPaths);

    onceHeaderPaths.clear();
    for (const std::string &path : inlinedPaths)
      if (pragmaOnceGuards_.HeaderEstablishesOnceState(path))
        onceHeaderPaths.push_back(path);
    if (onceHeaderPaths.empty())
      return;

    bool addedAny = false;
    for (const RefoldModel::IncludeItem &include : model_.GetIncludes()) {
      if (seeds.count(include.id))
        continue;
      // Only edges that survive as directives can re-enter.  An edge inside a
      // subtree already scheduled for materialization is handled by the
      // materializer's own child recursion.
      std::string reentered;
      if (!pragmaOnceGuards_.IncludeClosureReentersHeader(
              include, onceHeaderPaths, &reentered)) {
        continue;
      }
      REFOLD_LOG_TRACE(
          "pragma/once/guard",
          "forcing materialization of inc#{0} target='{1}': its closure "
          "re-enters inlined once-header '{2}' (round {3})",
          include.id, include.target, reentered, round);
      seeds.insert(include.id);
      addedAny = true;
    }

    if (!addedAny)
      return;
  }
}

void RefoldIncludeMaterializationScheduler::CollectEnteredSubtreePhysicalPaths(
    uint64_t includeId, std::vector<std::string> &paths) const {
  paths.clear();
  DenseSet<uint64_t> visited;
  SmallVector<uint64_t, 16> worklist{includeId};

  while (!worklist.empty()) {
    const uint64_t current = worklist.pop_back_val();
    if (!visited.insert(current).second)
      continue;
    if (const RefoldModel::IncludeItem *include =
            model_.GetIncludeById(current))
      if (include->openedPath && !include->openedPath->empty())
        paths.push_back(include->openedPath->str());
    if (auto it = children_.find(current); it != children_.end())
      for (const RefoldModel::IncludeItem *child : it->second)
        worklist.push_back(child->id);
  }

  llvm::sort(paths);
  paths.erase(std::unique(paths.begin(), paths.end()), paths.end());
}

bool RefoldIncludeMaterializationScheduler::HeaderMacroStateIsObservedOutside(
    StringRef physicalPath) const {
  // Restoring a header's guard suppresses re-entry into that header *and
  // everything it includes*, so the owner domain must be the whole entered
  // subtree.  Considering only the header's own definitions would miss the
  // nested ones -- `bits/types.h` pulls in `bits/typesizes.h` and
  // `bits/wordsize.h`, whose macros glibc uses everywhere.
  DenseSet<uint64_t> ownerIds;
  SmallVector<uint64_t, 16> worklist;
  for (const RefoldModel::IncludeItem &include : model_.GetIncludes())
    if (include.openedPath &&
        pathIdentity_.PathsEqual(*include.openedPath, physicalPath))
      worklist.push_back(include.id);
  while (!worklist.empty()) {
    const uint64_t current = worklist.pop_back_val();
    if (!ownerIds.insert(current).second)
      continue;
    if (auto it = children_.find(current); it != children_.end())
      for (const RefoldModel::IncludeItem *child : it->second)
        worklist.push_back(child->id);
  }
  if (ownerIds.empty())
    return true;

  llvm::StringSet<> definedHere;
  for (const RefoldModel::MacroDirective &directive :
       model_.GetMacroDirectives()) {
    if (directive.ownerIncludeId && ownerIds.count(*directive.ownerIncludeId))
      definedHere.insert(directive.name);
  }
  if (definedHere.empty())
    return false;

  // An invocation of one of those names from outside this header means the
  // definition is live after the header: suppressing a later re-entry would
  // leave that use undefined.  Invocations inside the header's own instances do
  // not count, because a body realized from B already carries their expansion.
  for (const RefoldModel::MacroInvocation &invocation :
       model_.GetMacroInvocations()) {
    if (!definedHere.contains(invocation.name))
      continue;
    if (invocation.ownerIncludeId && ownerIds.count(*invocation.ownerIncludeId))
      continue;
    REFOLD_LOG_TRACE("pragma/once/guard",
                     "header '{0}' macro '{1}' is observed outside it; its "
                     "include-guard state cannot be restored",
                     physicalPath, invocation.name);
    return true;
  }

  return false;
}

bool RefoldIncludeMaterializationScheduler::HeaderContributedTokens(
    StringRef physicalPath) const {
  for (const RefoldModel::IncludeItem &include : model_.GetIncludes()) {
    if (!include.openedPath ||
        !pathIdentity_.PathsEqual(*include.openedPath, physicalPath)) {
      continue;
    }
    for (const RefoldModel::PPSpan &span : include.spans)
      if (span.begin < span.end)
        return true;
  }
  return false;
}

bool RefoldIncludeMaterializationScheduler::HeaderWasEverSkipped(
    StringRef physicalPath) const {
  // Only a header the preprocessor actually *skipped* was protected in the
  // original run.  If every edge to it was entered, the original emitted its
  // content at each of those positions, so a refolded TU that re-enters it
  // reproduces the original rather than duplicating anything.
  //
  // This distinction matters because a header can legitimately have no
  // protection at all: Clang's `__stddef_*.h` are designed to be re-included
  // under different `__need_*` macros, and `assert.h` re-reads on every `NDEBUG`
  // change.  Treating "no controlling macro" as "unprotected" would fail closed
  // on exactly those headers, which are the common case in any system include.
  for (const RefoldModel::IncludeItem &include : model_.GetIncludes()) {
    if (!include.openedPath ||
        !pathIdentity_.PathsEqual(*include.openedPath, physicalPath)) {
      continue;
    }
    const bool entered =
        include.enteredFileName.has_value() || include.parent.has_value();
    if (!entered)
      return true;
  }
  return false;
}

bool RefoldIncludeMaterializationScheduler::HeaderControllingMacroIsRecorded(
    StringRef physicalPath) const {
  for (const RefoldModel::IncludeItem &include : model_.GetIncludes()) {
    if (!include.openedPath ||
        !pathIdentity_.PathsEqual(*include.openedPath, physicalPath)) {
      continue;
    }
    if (include.controllingMacro && !include.controllingMacro->empty())
      return true;
  }
  return false;
}

bool RefoldIncludeMaterializationScheduler::
    ProveNoReentryIntoUnprotectedInlinedHeaders() {
  std::vector<std::string> unprotectedPaths;

  // Determinism: iterate realized includes by sorted id.
  SmallVector<uint64_t, 32> realizedIds;
  for (const auto &kv : includeExpansionAcceptedResults_)
    realizedIds.push_back(kv.first);
  llvm::sort(realizedIds);

  for (uint64_t includeId : realizedIds) {
    const AcceptedResultCandidate &candidate =
        includeExpansionAcceptedResults_.find(includeId)->second;
    if (candidate.proofSummary.inventory.currentPath !=
        AcceptedPathKind::IncludeRealizationInlineFromB) {
      continue;
    }
    std::vector<std::string> emitted;
    CollectEnteredSubtreePhysicalPaths(includeId, emitted);
    for (std::string &path : emitted) {
      // A realized-from-B body whose own header carries synthetic once-state is
      // already wrapped by `StageRealizedFromBGuardText()`, so re-entering that
      // header is suppressed by the guard.
      if (pragmaOnceGuards_.HeaderRequiresGuard(path))
        continue;
      // This check exists to prevent *duplicated content*.  A header that
      // contributed no A tokens has no content to duplicate: re-entering it can
      // only re-establish preprocessor state, which moves the refolded TU toward
      // the original rather than away from it.  Feature-test and configuration
      // headers such as `features.h` and `sys/cdefs.h` are entirely directives,
      // and they are precisely the headers a later include must be free to
      // re-enter, because that re-entry is the only remaining source of the
      // macro state a realized-from-B body discarded.
      if (!HeaderContributedTokens(path))
        continue;
      // A header the preprocessor never skipped was never protected, so
      // re-entering it emits exactly what the original emitted.
      if (!HeaderWasEverSkipped(path))
        continue;
      // A recorded controlling macro alone is not sufficient.  Restoring the
      // guard suppresses every later inclusion of the header, and a body
      // realized from B dropped the header's `#define`s along with its guard, so
      // that suppression also removes the only remaining source of the macro
      // state.  Restoration is therefore admitted only when the header's macro
      // state is dead outside it -- true for `bits/types.h`, whose 16 macros are
      // never invoked elsewhere, and false for `sys/cdefs.h`, whose
      // `__GLIBC_USE` every glibc header depends on.
      if (HeaderControllingMacroIsRecorded(path) &&
          !HeaderMacroStateIsObservedOutside(path)) {
        continue;
      }
      unprotectedPaths.push_back(std::move(path));
    }
  }

  if (unprotectedPaths.empty())
    return true;

  llvm::sort(unprotectedPaths);
  unprotectedPaths.erase(
      std::unique(unprotectedPaths.begin(), unprotectedPaths.end()),
      unprotectedPaths.end());

  for (const RefoldModel::IncludeItem &include : model_.GetIncludes()) {
    // Only a surviving directive can re-enter; a materialized edge is replaced
    // by its body.
    if (includeExpansion_.count(include.id))
      continue;

    // The re-entry side is intentionally over-approximated over *all* edges,
    // not just entered ones: once the refolded TU is preprocessed, an edge that
    // the producer skipped may be taken.
    std::string reentered;
    const bool directlyReenters =
        include.openedPath &&
        llvm::any_of(unprotectedPaths, [&](const std::string &path) {
          return pathIdentity_.PathsEqual(*include.openedPath, path);
        });
    if (directlyReenters)
      reentered = include.openedPath->str();
    else if (!pragmaOnceGuards_.IncludeClosureReentersHeader(
                 include, unprotectedPaths, &reentered))
      continue;

    const std::string detail =
        llvm::formatv(
            "surviving include inc#{0} target='{1}' re-enters '{2}', whose "
            "content was inlined from the edited preprocessed stream and "
            "therefore carries none of its own include-guard directives",
            include.id, include.target, reentered)
            .str();
    REFOLD_LOG_TRACE("pragma/once/guard", "{0}", detail);
    terminalSink_.RequestTerminalFallback(
        MakeTerminalFallbackProofFailure(
            TerminalFallbackObligationKind::IncludeGuardStateStabilizable,
            TerminalFallbackFailureReason::IncludeGuardStateNotStabilizable),
        "pragma/once/guard", detail);
    return false;
  }

  return true;
}

void RefoldIncludeMaterializationScheduler::RecordActiveGuardedHeaders(
    ArrayRef<uint64_t> orderedSeeds) {
  // Walk each seed's whole subtree.  Recursive materialization can descend into
  // any descendant include, so every physical header reachable from a seed is a
  // header this run may inline.  Over-approximating here is sound; missing one
  // would leave an occurrence of it unguarded.
  DenseSet<uint64_t> visited;
  SmallVector<uint64_t, 32> worklist(orderedSeeds.begin(), orderedSeeds.end());
  std::vector<std::string> activePaths;

  while (!worklist.empty()) {
    const uint64_t includeId = worklist.pop_back_val();
    if (!visited.insert(includeId).second)
      continue;

    if (const RefoldModel::IncludeItem *include =
            model_.GetIncludeById(includeId)) {
      if (include->openedPath && !include->openedPath->empty())
        activePaths.push_back(include->openedPath->str());
    }

    if (auto it = children_.find(includeId); it != children_.end())
      for (const RefoldModel::IncludeItem *child : it->second)
        worklist.push_back(child->id);
  }

  // Deterministic order keeps guard numbering reproducible.
  llvm::sort(activePaths);
  activePaths.erase(std::unique(activePaths.begin(), activePaths.end()),
                    activePaths.end());

  SmallVector<StringRef, 16> activePathRefs;
  activePathRefs.reserve(activePaths.size());
  for (const std::string &path : activePaths)
    activePathRefs.push_back(path);

  pragmaOnceGuards_.SetActiveGuardedHeaders(activePathRefs);
}

bool RefoldIncludeMaterializationScheduler::StageTURootSurvivingIncludeGuards() {
  // A TU-owned include that was never materialized survives as a real directive.
  // If its physical header was inlined at some other occurrence, the directive
  // must be guarded so it does not re-enter a header whose body is already in the
  // output.
  SmallVector<const RefoldModel::IncludeItem *, 16> survivors;
  for (const RefoldModel::IncludeItem &include : model_.GetIncludes()) {
    if (includeExpansion_.count(include.id))
      continue;
    // TU-owned sites only.  `parent` alone is not a reliable discriminator here:
    // a skipped nested edge also has no parent, so the site path decides.
    if (!pathIdentity_.PathsEqual(include.sitePath, request_.tuPath))
      continue;
    if (!pragmaOnceGuards_.FindGuardForInclude(include))
      continue;
    survivors.push_back(&include);
  }

  // Deterministic emission order.
  llvm::sort(survivors, [](const RefoldModel::IncludeItem *lhs,
                           const RefoldModel::IncludeItem *rhs) {
    return lhs->id < rhs->id;
  });

  for (const RefoldModel::IncludeItem *include : survivors) {
    auto [siteBegin, siteEnd] = ExtendedTUSiteRange(*include);
    // A TU-owned site has no include ancestry, so its only enclosing arm is the
    // one inside the TU itself.
    const std::optional<uint64_t> ancestorArm =
        includeMaterializer_.ComputeAncestorArmForChildInclude(
            *include, /*ownerIncludeId=*/std::nullopt, std::nullopt);
    if (PragmaOnceGuardEditResult guardResult =
            pragmaOnceGuards_.StageSurvivingIncludeGuardEdit(
                *include, request_.tuPath, std::nullopt, request_.tuBytes,
                siteBegin, siteEnd, ancestorArm, tuEdits_);
        !guardResult.proven) {
      REFOLD_LOG_TRACE("pragma/once/guard",
                       "TU surviving include inc#{0} rejected: {1} ({2})",
                       include->id, toString(guardResult.rejection),
                       guardResult.detail);
      terminalSink_.RequestTerminalFallback(
          MakeTerminalFallbackProofFailure(
              TerminalFallbackObligationKind::IncludeGuardStateStabilizable,
              TerminalFallbackFailureReason::IncludeGuardStateNotStabilizable),
          "pragma/once/guard", guardResult.detail);
      return false;
    }
  }

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

  // Guard TU-owned includes that survive as directives only after every inlined
  // body has been staged, so the surviving set is final.
  return StageTURootSurvivingIncludeGuards();
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
  return model_.GetMacroDirectiveById(id);
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

  // (c) Includes the closing output check ruled out from keeping their
  // directive.  These carry no edit of their own -- the divergence they own
  // was realized somewhere inside them -- but they are seeded through the same
  // path as an edited include, so expanding one is the ordinary
  // materialization rather than a special case.
  if (request_.ownersMustExpand) {
    for (const RefoldModel::IncludeItem &include : model_.GetIncludes())
      if (request_.ownersMustExpand->count(include.id))
        seeds.insert(include.id);
  }
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
        &structuralHunkDispatcher_.MutableAppliedExpandedMacroRootIds(),
        /*materializeIncludeNextInThisSubtree=*/false,
        /*ancestorArmIdAtIncludeSite=*/std::nullopt,
        request_.ownersMustExpand);
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

  const PreprocessingStructureKind includeKinds[] = {
      PreprocessingStructureKind::Include,
      PreprocessingStructureKind::IncludeNext,
      PreprocessingStructureKind::Import};
  if (!textEditAssembler_.AuthorizeProtectedSourceIntervals(
          edit, ProtectedSourceEditAuthorityKind::IncludeMaterialization,
          request_.tuPath, std::nullopt, request_.tuBytes, siteBegin, siteEnd,
          includeKinds))
    return false;

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
