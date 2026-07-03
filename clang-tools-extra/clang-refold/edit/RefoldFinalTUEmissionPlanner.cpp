//===--- RefoldFinalTUEmissionPlanner.cpp ----------------------*- C++ -*-===//

#include "edit/RefoldFinalTUEmissionPlanner.h"

#include "core/RefoldLog.h"
#include "core/RefoldModel.h"
#include "edit/RefoldPatchTypes.h"
#include "edit/RefoldTextEditAssembler.h"
#include "include/RefoldIncludeMaterializationScheduler.h"
#include "line-control/FinalLineControlModel.h"
#include "line-control/LineDirectiveInserter.h"
#include "line-control/RefoldLineControlProof.h"
#include "macro/RefoldMacroTopology.h"
#include "proof/RefoldProofLattice.h"
#include "proof/RefoldTerminalProofSink.h"
#include "source/RefoldStructuralHunkDispatcher.h"
#include "util/StringUtils.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Support/FormatVariadic.h"

#include <algorithm>
#include <optional>
#include <utility>
#include <vector>

namespace clang {
namespace refold {

namespace {

/// Run-scoped implementation context for final TU emission.
class FinalTUEmissionContext {
public:
  /// Binds final-emission dependencies and mutable run state once for all
  /// staging and emission phases.
  FinalTUEmissionContext(
      const RefoldFinalTUEmissionPlanner::Dependencies &deps,
      const RefoldFinalTUEmissionPlanner::EmissionRequest &request)
      : deps_(deps), request_(request), model_(*deps.model),
        macroTopology_(*deps.macroTopology),
        lineControlProof_(*deps.lineControlProof), lineDirs_(*deps.lineDirs),
        macroStateRepairPlanner_(*deps.macroStateRepairPlanner),
        textEditAssembler_(*deps.textEditAssembler),
        terminalSink_(*deps.terminalSink),
        structuralHunkDispatcher_(*request.structuralHunkDispatcher),
        includeMaterializationScheduler_(
            *request.includeMaterializationScheduler),
        macroStatePlan_(*request.macroStatePlan),
        macroStateRequest_(*request.macroStateRequest),
        tuEdits_(
            structuralHunkDispatcher_.MutableTUEditsForRepairAndEmission()),
        finalLineControlPruneCandidates_(*deps.finalLineControlPruneCandidates),
        finalLineControlSourceMappings_(*deps.finalLineControlSourceMappings) {}

  /// Runs complete final TU emission and returns emitted text plus final
  /// macro-expansion statistics.
  RefoldFinalTUEmissionPlanner::EmissionResult Run();

private:
  /// Converts finalized TU-owned macro patches into concrete TU byte edits.
  void StageTUMacroPatchEdits();

  /// Returns whether \p Patch is contained in an already accepted TU macro
  /// interval, while preserving the original fail-fast check for partial
  /// overlap in accepted-interval order.
  bool MacroPatchIsShadowedByAccepted(
      const MacroPatch &patch, uint64_t patchEnd,
      llvm::ArrayRef<std::pair<uint64_t, uint64_t>> acceptedRanges) const;

  /// Creates the concrete TU text edit for one accepted TU macro patch.
  TextEdit BuildTUMacroPatchEdit(const MacroPatch &patch,
                                 uint64_t patchEnd) const;

  /// Runs the post-macro-edit macro-state carry pass and then lowers realized
  /// TU-root include expansions into TU edits.
  bool StageTURootIncludeExpansionEdits();

  /// Emits the debug summary for the fully staged final TU edit plan.
  void LogEmissionPlan() const;

  /// Applies final TU text edits with pending-resync handling.
  std::string ApplyFinalTUEdits() const;

  /// Adds an initial TU #line directive when preserved TU-local file observers
  /// would otherwise see the checker output path instead of the producer TU.
  void RepairTUPrologueForPreservedFileObservers(std::string &tuResult) const;

  /// Returns true when a macro invocation site is covered by a materialized
  /// macro patch in its owner bucket.
  bool
  SiteHasMaterializedMacroPatch(const RefoldModel::MacroInvocation &site) const;

  /// Returns whether a preserved file observer projected through \p Site needs
  /// the synthetic TU prologue to preserve checker replay semantics.
  bool SiteNeedsTUPrologue(const RefoldModel::MacroInvocation &site) const;

  /// Returns whether any preserved TU-local file observer requires a synthetic
  /// TU prologue before final replay.
  bool NeedsTUPrologueForPreservedFileObservers() const;

  /// Inserts the synthetic TU prologue and shifts existing final line-control
  /// prune candidates to account for the new bytes.
  void InsertTUPrologue(std::string &tuResult) const;

  /// Charges macro roots that remain expanded inside materialized include
  /// bodies and returns the final expanded macro count.
  size_t RecordExpandedMacroRoots() const;

  const RefoldFinalTUEmissionPlanner::Dependencies &deps_;
  const RefoldFinalTUEmissionPlanner::EmissionRequest &request_;

  const RefoldModel &model_;
  const RefoldMacroTopology &macroTopology_;
  const RefoldLineControlProof &lineControlProof_;
  const LineDirectiveInserter &lineDirs_;
  const RefoldMacroStateRepairPlanner &macroStateRepairPlanner_;
  const RefoldTextEditAssembler &textEditAssembler_;
  RefoldTerminalProofSink &terminalSink_;
  RefoldStructuralHunkDispatcher &structuralHunkDispatcher_;
  RefoldIncludeMaterializationScheduler &includeMaterializationScheduler_;
  RefoldMacroStateRepairPlanner::MacroStateRepairPlan &macroStatePlan_;
  const RefoldMacroStateRepairPlanner::MacroStateRepairRequest
      &macroStateRequest_;
  std::vector<TextEdit> &tuEdits_;
  std::vector<FinalLineControlPruneCandidate> &finalLineControlPruneCandidates_;
  std::vector<FinalLineControlSourceMapping> &finalLineControlSourceMappings_;
};

RefoldFinalTUEmissionPlanner::EmissionResult FinalTUEmissionContext::Run() {
  RefoldFinalTUEmissionPlanner::EmissionResult result;

  StageTUMacroPatchEdits();
  if (!StageTURootIncludeExpansionEdits())
    return result;

  LogEmissionPlan();

  result.tuText = ApplyFinalTUEdits();
  if (terminalSink_.HasRequest())
    return result;

  RepairTUPrologueForPreservedFileObservers(result.tuText);
  result.expandedMacroCount = RecordExpandedMacroRoots();
  result.success = true;
  return result;
}

void FinalTUEmissionContext::StageTUMacroPatchEdits() {
  const std::vector<MacroPatch> *tuMacroPatchList =
      structuralHunkDispatcher_.FindFinalMacroPatchesForOwner(std::nullopt);
  if (!tuMacroPatchList || tuMacroPatchList->empty())
    return;

  auto tuMacroPatches = *tuMacroPatchList;
  std::sort(tuMacroPatches.begin(), tuMacroPatches.end(),
            [](const MacroPatch &lhs, const MacroPatch &rhs) {
              if (lhs.invRange.begin != rhs.invRange.begin)
                return lhs.invRange.begin < rhs.invRange.begin;
              return lhs.invRange.end > rhs.invRange.end;
            });

  llvm::SmallVector<std::pair<uint64_t, uint64_t>, 16> accepted;
  for (const MacroPatch &patch : tuMacroPatches) {
    const uint64_t patchEnd = stringutils::extendChainedCallEnd(
        request_.tuBytes, patch.invRange.end, patch.replacement);

    if (MacroPatchIsShadowedByAccepted(patch, patchEnd, accepted))
      continue;

    accepted.push_back({patch.invRange.begin, patchEnd});
    structuralHunkDispatcher_.AddTUEdit(BuildTUMacroPatchEdit(patch, patchEnd));
  }
}

bool FinalTUEmissionContext::MacroPatchIsShadowedByAccepted(
    const MacroPatch &patch, uint64_t patchEnd,
    llvm::ArrayRef<std::pair<uint64_t, uint64_t>> acceptedRanges) const {
  for (const auto &accepted : acceptedRanges) {
    if (patch.invRange.begin >= accepted.first && patchEnd <= accepted.second)
      return true;

    // Partial overlaps should never occur (macro invocation sites are either
    // disjoint or nested). If they do, fail fast rather than producing
    // order-dependent behavior.
    if (patch.invRange.begin < accepted.second && accepted.first < patchEnd) {
      REFOLD_LOG_FATAL("macro/tu",
                       "overlapping TU macro patches: mp=[{0},{1}) "
                       "acc=[{2},{3})",
                       patch.invRange.begin, patchEnd, accepted.first,
                       accepted.second);
    }
  }
  return false;
}

TextEdit
FinalTUEmissionContext::BuildTUMacroPatchEdit(const MacroPatch &patch,
                                              uint64_t patchEnd) const {
  ResyncOutcome resync = textEditAssembler_.ApplyResyncOrPend(
      request_.tuBytes, patch.invRange.begin, patchEnd, patch.replacement,
      request_.tuPath);

  TextEdit edit{
      patch.invRange.begin,
      patchEnd,
      std::move(resync.text),
      std::move(resync.pending),
      macroTopology_.MacroPatchRemainsExpanded(patch)
          ? std::make_optional(macroTopology_.GetRootMacroId(patch.macroId))
          : std::nullopt,
      {},
      {},
      {}};
  edit.lineControlPruneCandidates =
      std::move(resync.lineControlPruneCandidates);

  if (auto bRange =
          textEditAssembler_.MacroPatchMaterializedBByteRange(patch)) {
    textEditAssembler_.CertifyTextEditMaterializedBByteRange(
        edit, bRange->first, bRange->second);
  }
  if (auto outRange =
          textEditAssembler_.MacroPatchMaterializedOutputTextRange(patch)) {
    textEditAssembler_.CertifyTextEditMaterializedOutputTextRange(
        edit, outRange->first, outRange->second);
  }

  textEditAssembler_.AttachAcceptedResultCarrier(
      edit, deps_.proofLattice->AcceptedCandidateBuilder()
                .BuildAcceptedEmittedMacroCandidate(patch));
  return edit;
}

bool FinalTUEmissionContext::StageTURootIncludeExpansionEdits() {
  macroStateRepairPlanner_.CarryObservedGapDefinitionsAfterReplacements(
      macroStatePlan_, macroStateRequest_);

  return includeMaterializationScheduler_.StageTURootIncludeExpansionEdits(
      macroStatePlan_, macroStateRequest_);
}

void FinalTUEmissionContext::LogEmissionPlan() const {
  if (!inDebugMode())
    return;

  debug("emit",
        "emission plan: tuEdits={0} tuMacroPatches={1} includePatchBuckets={2} "
        "includePatches={3} materializedIncludes={4}",
        tuEdits_.size(), structuralHunkDispatcher_.CountTUMacroPatches(),
        structuralHunkDispatcher_.IncludeBucketCount(),
        structuralHunkDispatcher_.CountIncludePatches(),
        includeMaterializationScheduler_.MaterializedIncludeCount());
}

std::string FinalTUEmissionContext::ApplyFinalTUEdits() const {
  return textEditAssembler_.ApplyTextEditsWithPendingResync(
      request_.tuBytes, tuEdits_,
      &structuralHunkDispatcher_.MutableAppliedExpandedMacroRootIds(),
      request_.tuPath, std::nullopt, deps_.materializedEditMappings,
      &finalLineControlPruneCandidates_, &finalLineControlSourceMappings_);
}

void FinalTUEmissionContext::RepairTUPrologueForPreservedFileObservers(
    std::string &tuResult) const {
  if (!lineDirs_.Enabled() || tuResult.empty())
    return;
  if (!NeedsTUPrologueForPreservedFileObservers())
    return;
  if (stringutils::startsWithAfterWs(llvm::StringRef(tuResult), "#line"))
    return;

  InsertTUPrologue(tuResult);
}

bool FinalTUEmissionContext::SiteHasMaterializedMacroPatch(
    const RefoldModel::MacroInvocation &site) const {
  if (!site.invB || !site.invE)
    return false;

  const std::vector<MacroPatch> *patches =
      structuralHunkDispatcher_.FindFinalMacroPatchesForOwner(
          site.ownerIncludeId);
  if (!patches)
    return false;

  for (const MacroPatch &patch : *patches) {
    if (patch.invRange.begin <= *site.invB && *site.invE <= patch.invRange.end)
      return true;
  }
  return false;
}

bool FinalTUEmissionContext::SiteNeedsTUPrologue(
    const RefoldModel::MacroInvocation &site) const {
  if (macroTopology_.IsInvocationInsideDefineDirective(site))
    return false;
  if (SiteHasMaterializedMacroPatch(site))
    return false;
  if (site.ownerIncludeId)
    return false;
  if (!site.invFile)
    return false;

  // Compare absolute normalized paths to avoid relative-spelling mismatches for
  // the physical invocation owner.  Producer spelling (TUPath) is preserved in
  // the emitted directive.
  if (lineDirs_.ToAbsolutePath(*site.invFile) !=
      lineDirs_.ToAbsolutePath(request_.tuPath))
    return false;

  // A source-authored #line before the observer dominates the synthetic TU
  // prologue.  Only observers whose active logical file is still the producer
  // TU need the prologue repair.
  if (site.invB) {
    LineDirectiveLocation loc = LineDirectiveInserter::LogicalLocationAtOffset(
        request_.tuBytes, *site.invB, request_.tuPath, model_, request_.tuPath);
    if (lineDirs_.ToAbsolutePath(loc.fileSpelling) !=
        lineDirs_.ToAbsolutePath(request_.tuPath))
      return false;
  }

  return true;
}

bool FinalTUEmissionContext::NeedsTUPrologueForPreservedFileObservers() const {
  for (const RefoldModel::MacroInvocation &macro :
       model_.GetMacroInvocations()) {
    if (macro.name != "__FILE__" && macro.name != "__FILE_NAME__" &&
        macro.name != "__BASE_FILE__")
      continue;
    if (!lineControlProof_.LineStateBuiltinInvocationIsPreservedObserver(macro))
      continue;

    const RefoldModel::MacroInvocation *site = &macro;
    unsigned depth = 0;
    const unsigned maxDepth =
        static_cast<unsigned>(model_.GetMacroInvocations().size());
    while (site && depth++ <= maxDepth) {
      if (SiteNeedsTUPrologue(*site))
        return true;
      if (!site->callerMacroId)
        break;
      site = macroTopology_.FindMacroInvocationById(*site->callerMacroId);
    }
  }
  return false;
}

void FinalTUEmissionContext::InsertTUPrologue(std::string &tuResult) const {
  std::string directive = lineDirs_.FormatLineDirective(1, request_.tuPath);
  if (directive.empty())
    return;

  const uint64_t insertedBytes = static_cast<uint64_t>(directive.size());
  tuResult.insert(0, directive);
  for (FinalLineControlPruneCandidate &candidate :
       finalLineControlPruneCandidates_) {
    candidate.finalBegin += insertedBytes;
    candidate.finalEnd += insertedBytes;
  }
  finalLineControlPruneCandidates_.push_back(MakeFinalLineControlPruneCandidate(
      /*finalBegin=*/0, insertedBytes,
      FinalLineDirective::Origin::SyntheticTUPrologue,
      FinalLineControlOwnerKey(request_.tuPath.str(), std::nullopt),
      /*producerProven=*/true, FinalLineControlObligation::TUPrologueRepair));
}

size_t FinalTUEmissionContext::RecordExpandedMacroRoots() const {
  structuralHunkDispatcher_.RecordExpandedMacroRootsInMaterializedIncludes(
      model_, macroTopology_,
      includeMaterializationScheduler_.ExpandedIncludeIds());
  return structuralHunkDispatcher_.ExpandedMacroRootCount();
}

} // namespace

RefoldFinalTUEmissionPlanner::RefoldFinalTUEmissionPlanner(Dependencies deps)
    : deps_(std::move(deps)) {}

RefoldFinalTUEmissionPlanner::EmissionResult
RefoldFinalTUEmissionPlanner::PlanAndEmit(
    const EmissionRequest &request) const {
  FinalTUEmissionContext context(deps_, request);
  return context.Run();
}

} // namespace refold
} // namespace clang
