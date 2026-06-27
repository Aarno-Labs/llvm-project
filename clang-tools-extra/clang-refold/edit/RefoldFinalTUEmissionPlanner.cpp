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
      const RefoldFinalTUEmissionPlanner::Dependencies &Deps,
      const RefoldFinalTUEmissionPlanner::EmissionRequest &Request)
      : deps_(Deps), request_(Request), model_(*Deps.Model),
        macroTopology_(*Deps.MacroTopology),
        lineControlProof_(*Deps.LineControlProof), lineDirs_(*Deps.LineDirs),
        macroStateRepairPlanner_(*Deps.MacroStateRepairPlanner),
        textEditAssembler_(*Deps.TextEditAssembler),
        terminalSink_(*Deps.TerminalSink),
        structuralHunkDispatcher_(*Request.StructuralHunkDispatcher),
        includeMaterializationScheduler_(*Request.IncludeMaterializationScheduler),
        macroStatePlan_(*Request.MacroStatePlan),
        macroStateRequest_(*Request.MacroStateRequest),
        tuEdits_(structuralHunkDispatcher_.MutableTUEditsForRepairAndEmission()),
        finalLineControlPruneCandidates_(*Deps.FinalLineControlPruneCandidates),
        finalLineControlSourceMappings_(*Deps.FinalLineControlSourceMappings) {}

  /// Runs the complete final TU emission phase and returns emitted text plus
  /// final macro-expansion statistics.
  RefoldFinalTUEmissionPlanner::EmissionResult Run();

private:
  /// Converts finalized TU-owned macro patches into concrete TU byte edits.
  void StageTUMacroPatchEdits();

  /// Returns whether \p Patch is contained in an already accepted TU macro
  /// interval, while preserving the original fail-fast check for partial
  /// overlap in accepted-interval order.
  bool MacroPatchIsShadowedByAccepted(
      const MacroPatch &Patch, uint64_t PatchEnd,
      llvm::ArrayRef<std::pair<uint64_t, uint64_t>> Accepted) const;

  /// Creates the concrete TU text edit for one accepted TU macro patch.
  TextEdit BuildTUMacroPatchEdit(const MacroPatch &Patch, uint64_t PatchEnd) const;

  /// Runs the post-macro-edit macro-state carry pass and then lowers realized
  /// TU-root include expansions into TU edits.
  bool StageTURootIncludeExpansionEdits();

  /// Emits the debug summary for the fully staged final TU edit plan.
  void LogEmissionPlan() const;

  /// Applies final TU text edits with pending-resync handling.
  std::string ApplyFinalTUEdits() const;

  /// Adds an initial TU #line directive when preserved TU-local file observers
  /// would otherwise see the checker output path instead of the producer TU.
  void RepairTUPrologueForPreservedFileObservers(std::string &TUResult) const;

  /// Returns true when a macro invocation site is covered by a materialized
  /// macro patch in its owner bucket.
  bool SiteHasMaterializedMacroPatch(
      const RefoldModel::MacroInvocation &Site) const;

  /// Returns whether a preserved file observer projected through \p Site needs
  /// the synthetic TU prologue to preserve checker replay semantics.
  bool SiteNeedsTUPrologue(const RefoldModel::MacroInvocation &Site) const;

  /// Returns whether any preserved TU-local file observer requires a synthetic
  /// TU prologue before final replay.
  bool NeedsTUPrologueForPreservedFileObservers() const;

  /// Inserts the synthetic TU prologue and shifts existing final line-control
  /// prune candidates to account for the new bytes.
  void InsertTUPrologue(std::string &TUResult) const;

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
  std::vector<FinalLineControlPruneCandidate>
      &finalLineControlPruneCandidates_;
  std::vector<FinalLineControlSourceMapping> &finalLineControlSourceMappings_;
};

RefoldFinalTUEmissionPlanner::EmissionResult FinalTUEmissionContext::Run() {
  RefoldFinalTUEmissionPlanner::EmissionResult result;

  StageTUMacroPatchEdits();
  if (!StageTURootIncludeExpansionEdits())
    return result;

  LogEmissionPlan();

  result.TUText = ApplyFinalTUEdits();
  if (terminalSink_.HasRequest())
    return result;

  RepairTUPrologueForPreservedFileObservers(result.TUText);
  result.ExpandedMacroCount = RecordExpandedMacroRoots();
  result.Success = true;
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
              if (lhs.invStart != rhs.invStart)
                return lhs.invStart < rhs.invStart;
              return lhs.invEnd > rhs.invEnd;
            });

  llvm::SmallVector<std::pair<uint64_t, uint64_t>, 16> accepted;
  for (const MacroPatch &patch : tuMacroPatches) {
    const uint64_t patchEnd = stringutils::extendChainedCallEnd(
        request_.TUBytes, patch.invEnd, patch.replacement);

    if (MacroPatchIsShadowedByAccepted(patch, patchEnd, accepted))
      continue;

    accepted.push_back({patch.invStart, patchEnd});
    structuralHunkDispatcher_.AddTUEdit(BuildTUMacroPatchEdit(patch, patchEnd));
  }
}

bool FinalTUEmissionContext::MacroPatchIsShadowedByAccepted(
    const MacroPatch &Patch, uint64_t PatchEnd,
    llvm::ArrayRef<std::pair<uint64_t, uint64_t>> Accepted) const {
  for (const auto &accepted : Accepted) {
    if (Patch.invStart >= accepted.first && PatchEnd <= accepted.second)
      return true;

    // Partial overlaps should never occur (macro invocation sites are either
    // disjoint or nested). If they do, fail fast rather than producing
    // order-dependent behavior.
    if (Patch.invStart < accepted.second && accepted.first < PatchEnd) {
      REFOLD_LOG_FATAL("macro/tu",
                       "overlapping TU macro patches: mp=[{0},{1}) "
                       "acc=[{2},{3})",
                       Patch.invStart, PatchEnd, accepted.first,
                       accepted.second);
    }
  }
  return false;
}

TextEdit FinalTUEmissionContext::BuildTUMacroPatchEdit(
    const MacroPatch &Patch, uint64_t PatchEnd) const {
  ResyncOutcome resync = textEditAssembler_.ApplyResyncOrPend(
      request_.TUBytes, Patch.invStart, PatchEnd, Patch.replacement,
      request_.TUPath);

  TextEdit edit{Patch.invStart,
                PatchEnd,
                std::move(resync.text),
                std::move(resync.pending),
                macroTopology_.MacroPatchRemainsExpanded(Patch)
                    ? std::make_optional(
                          macroTopology_.GetRootMacroId(Patch.macroId))
                    : std::nullopt,
                {},
                {},
                {}};
  edit.lineControlPruneCandidates =
      std::move(resync.lineControlPruneCandidates);

  if (auto bRange = textEditAssembler_.MacroPatchMaterializedBByteRange(Patch)) {
    textEditAssembler_.StampTextEditMaterializedBByteRange(
        edit, bRange->first, bRange->second);
  }
  if (auto outRange =
          textEditAssembler_.MacroPatchMaterializedOutputTextRange(Patch)) {
    textEditAssembler_.StampTextEditMaterializedOutputTextRange(
        edit, outRange->first, outRange->second);
  }

  textEditAssembler_.AttachAcceptedResultCarrier(
      edit, deps_.ProofLattice->BuildAcceptedEmittedMacroCandidate(Patch));
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
      request_.TUBytes, tuEdits_,
      &structuralHunkDispatcher_.MutableAppliedExpandedMacroRootIds(),
      request_.TUPath, std::nullopt, deps_.MaterializedEditMappings,
      &finalLineControlPruneCandidates_, &finalLineControlSourceMappings_);
}

void FinalTUEmissionContext::RepairTUPrologueForPreservedFileObservers(
    std::string &TUResult) const {
  if (!lineDirs_.Enabled() || TUResult.empty())
    return;
  if (!NeedsTUPrologueForPreservedFileObservers())
    return;
  if (stringutils::startsWithAfterWs(llvm::StringRef(TUResult), "#line"))
    return;

  InsertTUPrologue(TUResult);
}

bool FinalTUEmissionContext::SiteHasMaterializedMacroPatch(
    const RefoldModel::MacroInvocation &Site) const {
  if (!Site.invB || !Site.invE)
    return false;

  const std::vector<MacroPatch> *patches =
      structuralHunkDispatcher_.FindFinalMacroPatchesForOwner(Site.ownerIncludeId);
  if (!patches)
    return false;

  for (const MacroPatch &patch : *patches) {
    if (patch.invStart <= *Site.invB && *Site.invE <= patch.invEnd)
      return true;
  }
  return false;
}

bool FinalTUEmissionContext::SiteNeedsTUPrologue(
    const RefoldModel::MacroInvocation &Site) const {
  if (macroTopology_.IsInvocationInsideDefineDirective(Site))
    return false;
  if (SiteHasMaterializedMacroPatch(Site))
    return false;
  if (Site.ownerIncludeId)
    return false;
  if (!Site.invFile)
    return false;

  // Compare absolute normalized paths to avoid relative-spelling mismatches for
  // the physical invocation owner.  Producer spelling (TUPath) is preserved in
  // the emitted directive.
  if (lineDirs_.ToAbsolutePath(*Site.invFile) !=
      lineDirs_.ToAbsolutePath(request_.TUPath))
    return false;

  // A source-authored #line before the observer dominates the synthetic TU
  // prologue.  Only observers whose active logical file is still the producer TU
  // need the prologue repair.
  if (Site.invB) {
    LineDirectiveLocation loc = LineDirectiveInserter::LogicalLocationAtOffset(
        request_.TUBytes, *Site.invB, request_.TUPath, model_, request_.TUPath);
    if (lineDirs_.ToAbsolutePath(loc.fileSpelling) !=
        lineDirs_.ToAbsolutePath(request_.TUPath))
      return false;
  }

  return true;
}

bool FinalTUEmissionContext::NeedsTUPrologueForPreservedFileObservers() const {
  for (const RefoldModel::MacroInvocation &macro : model_.GetMacroInvocations()) {
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

void FinalTUEmissionContext::InsertTUPrologue(std::string &TUResult) const {
  std::string directive = lineDirs_.FormatLineDirective(1, request_.TUPath);
  if (directive.empty())
    return;

  const uint64_t insertedBytes = static_cast<uint64_t>(directive.size());
  TUResult.insert(0, directive);
  for (FinalLineControlPruneCandidate &candidate :
       finalLineControlPruneCandidates_) {
    candidate.finalBegin += insertedBytes;
    candidate.finalEnd += insertedBytes;
  }
  finalLineControlPruneCandidates_.push_back(MakeFinalLineControlPruneCandidate(
      /*finalBegin=*/0, insertedBytes,
      FinalLineDirective::Origin::SyntheticTUPrologue,
      FinalLineControlOwnerKey(request_.TUPath.str(), std::nullopt),
      /*producerProven=*/true,
      FinalLineControlObligation::TUPrologueRepair));
}

size_t FinalTUEmissionContext::RecordExpandedMacroRoots() const {
  structuralHunkDispatcher_.RecordExpandedMacroRootsInMaterializedIncludes(
      model_, macroTopology_, includeMaterializationScheduler_.ExpandedIncludeIds());
  return structuralHunkDispatcher_.ExpandedMacroRootCount();
}

} // namespace

RefoldFinalTUEmissionPlanner::RefoldFinalTUEmissionPlanner(Dependencies Deps)
    : deps_(std::move(Deps)) {}

RefoldFinalTUEmissionPlanner::EmissionResult
RefoldFinalTUEmissionPlanner::PlanAndEmit(const EmissionRequest &Request) const {
  FinalTUEmissionContext context(deps_, Request);
  return context.Run();
}

} // namespace refold
} // namespace clang
