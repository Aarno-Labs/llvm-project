//===--- RefoldTUEditPlanner.cpp --------------------------------*- C++ -*-===//
//
// Translation-unit edit planning service.
//
// TU-side planning lives here: provable insertion anchors, TU byte-span
// planning, pure-insertion include-boundary ownership, direct-TU hunk edit
// plans, and the closed trailing macro-call suffix extension policy.  Final
// TextEdit ordering/application remains outside this service.
//
//===----------------------------------------------------------------------===//

#include "edit/RefoldTUEditPlanner.h"

#include "core/RefoldLog.h"
#include "core/RefoldModel.h"
#include "edit/RefoldTUAnchorProof.h"
#include "line-control/LineDirectiveInserter.h"
#include "line-control/RefoldLineControlProof.h"
#include "macro/RefoldMacroReplay.h"
#include "macro/RefoldMacroTopology.h"
#include "source/RefoldPreprocessingStructureIndex.h"
#include "util/RefoldPathIdentity.h"
#include "util/StringUtils.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringSwitch.h"
#include "llvm/Support/MemoryBuffer.h"

#include <algorithm>
#include <cassert>
#include <limits>
#include <tuple>
#include <utility>

using namespace llvm;

namespace clang {
namespace refold {

namespace {

/// Return the source-level directive spelling used by direct-TU rejection
/// diagnostics.  The stable enum name is logged separately; this spelling
/// makes a protected boundary recognizable without consulting the producer
/// schema.
StringRef directTUDiagnosticDirectiveSpelling(
    PreprocessingStructureKind kind) {
  switch (kind) {
  case PreprocessingStructureKind::ConditionalIf:
    return "#if";
  case PreprocessingStructureKind::ConditionalIfdef:
    return "#ifdef";
  case PreprocessingStructureKind::ConditionalIfndef:
    return "#ifndef";
  case PreprocessingStructureKind::ConditionalElif:
    return "#elif";
  case PreprocessingStructureKind::ConditionalElifdef:
    return "#elifdef";
  case PreprocessingStructureKind::ConditionalElifndef:
    return "#elifndef";
  case PreprocessingStructureKind::ConditionalElse:
    return "#else";
  case PreprocessingStructureKind::ConditionalEndif:
    return "#endif";
  case PreprocessingStructureKind::MacroDefine:
    return "#define";
  case PreprocessingStructureKind::MacroUndef:
    return "#undef";
  case PreprocessingStructureKind::Include:
    return "#include";
  case PreprocessingStructureKind::IncludeNext:
    return "#include_next";
  case PreprocessingStructureKind::Import:
    return "#import";
  case PreprocessingStructureKind::Pragma:
    return "#pragma";
  case PreprocessingStructureKind::PragmaOperator:
    return "_Pragma/__pragma";
  case PreprocessingStructureKind::LineControl:
    return "#line/linemarker";
  case PreprocessingStructureKind::ErrorDirective:
    return "#error";
  case PreprocessingStructureKind::WarningDirective:
    return "#warning";
  case PreprocessingStructureKind::OtherDirective:
    return "#directive";
  }
  llvm_unreachable("invalid preprocessing-structure kind");
}

} // namespace

RefoldTUEditPlanner::RefoldTUEditPlanner(Deps deps) : deps_(std::move(deps)) {
  DenseSet<uint64_t> seenPP;
  for (const RefoldModel::TokMapEntry &entry : deps_.model.GetTokmap()) {
    if (!seenPP.insert(entry.pp).second)
      duplicateTokmapPP_.insert(entry.pp);
  }
}

bool RefoldTUEditPlanner::CollectDirectTUMacroRepairEvidence(
    uint64_t begin, uint64_t end,
    std::vector<const PreprocessingStructureInterval *> &intervals) const {
  const RefoldPreprocessingStructureIndex &structure =
      deps_.preprocessingStructureIndex;
  if (!structure.CollectExactMacroStateIntervals(begin, end, intervals) ||
      intervals.empty()) {
    return false;
  }

  llvm::sort(intervals,
             [](const PreprocessingStructureInterval *lhs,
                const PreprocessingStructureInterval *rhs) {
               if (lhs->begin != rhs->begin)
                 return lhs->begin < rhs->begin;
               if (lhs->end != rhs->end)
                 return lhs->end < rhs->end;
               if (lhs->kind != rhs->kind)
                 return lhs->kind < rhs->kind;
               return lhs->modelItemId < rhs->modelItemId;
             });

  // The structure-index query intentionally reports macro evidence without
  // interpreting other overlapping intervals. Recheck the complete overlap
  // set here so a provisional direct carrier cannot use one exact macro line to
  // absorb a conditional, include, pragma, line-control, or unknown directive.
  // A `_Pragma` token wholly nested in the replacement list is part of that
  // exact physical macro transition and is the sole admitted nested interval.
  for (const PreprocessingStructureInterval *interval :
       structure.FindOverlapping(begin, end)) {
    if (llvm::is_contained(intervals, interval))
      continue;

    const bool nestedPragmaOperator =
        interval->kind == PreprocessingStructureKind::PragmaOperator &&
        llvm::any_of(intervals,
                     [&](const PreprocessingStructureInterval *macro) {
                       return macro->begin <= interval->begin &&
                              interval->end <= macro->end;
                     });
    if (!nestedPragmaOperator) {
      intervals.clear();
      return false;
    }
  }
  return true;
}

bool RefoldTUEditPlanner::CollectDirectTUMacroRepairEvidenceOrEmpty(
    uint64_t begin, uint64_t end,
    std::vector<const PreprocessingStructureInterval *> &intervals) const {
  intervals.clear();
  if (!deps_.preprocessingStructureIndex.HasOverlapping(begin, end))
    return true;
  return CollectDirectTUMacroRepairEvidence(begin, end, intervals);
}

bool RefoldTUEditPlanner::ValidateOrdinaryDirectTUEnvelope(
    StringRef tuPath, uint64_t begin, uint64_t end) const {
  const RefoldPreprocessingStructureIndex &structure =
      deps_.preprocessingStructureIndex;
  if (end < begin || end > deps_.tuSourceBytes.size() ||
      structure.GetOwnerIncludeId() ||
      !deps_.pathIdentity.PathsEqual(tuPath, deps_.model.GetSourcePath()) ||
      !deps_.pathIdentity.PathsEqual(structure.GetSourcePath(), tuPath) ||
      !structure.IsDirectTUProtectionCensusComplete() ||
      !structure.IsExactLexicalBoundary(begin) ||
      !structure.IsExactLexicalBoundary(end)) {
    return false;
  }
  return !structure.HasOverlapping(begin, end);
}

bool RefoldTUEditPlanner::DirectTUEnvelopeRetainsMacroRepairEvidence(
    StringRef tuPath, uint64_t baseBegin, uint64_t baseEnd,
    uint64_t widenedBegin, uint64_t widenedEnd) const {
  if (widenedBegin > baseBegin || baseEnd > widenedEnd ||
      !ValidateDirectTUEnvelope(tuPath, baseBegin, baseEnd) ||
      !ValidateDirectTUEnvelope(tuPath, widenedBegin, widenedEnd)) {
    return false;
  }

  std::vector<const PreprocessingStructureInterval *> baseEvidence;
  std::vector<const PreprocessingStructureInterval *> widenedEvidence;
  if (!CollectDirectTUMacroRepairEvidenceOrEmpty(baseBegin, baseEnd,
                                                 baseEvidence) ||
      !CollectDirectTUMacroRepairEvidenceOrEmpty(widenedBegin, widenedEnd,
                                                 widenedEvidence)) {
    return false;
  }
  return baseEvidence == widenedEvidence;
}

bool RefoldTUEditPlanner::ProveDirectTUGapWithMacroRepairEvidence(
    uint64_t begin, uint64_t end) const {
  const RefoldPreprocessingStructureIndex &structure =
      deps_.preprocessingStructureIndex;
  if (structure.ProveOrdinaryDirectTUInternalGap(begin, end))
    return true;

  std::vector<const PreprocessingStructureInterval *> macroIntervals;
  if (!CollectDirectTUMacroRepairEvidence(begin, end, macroIntervals))
    return false;

  uint64_t coveredEnd = begin;
  for (const PreprocessingStructureInterval *interval : macroIntervals) {
    if (interval->end <= coveredEnd)
      continue;
    if (interval->begin < coveredEnd ||
        (coveredEnd < interval->begin &&
         !structure.ProveOrdinaryDirectTUInternalGap(coveredEnd,
                                                     interval->begin))) {
      return false;
    }
    coveredEnd = interval->end;
  }

  return coveredEnd == end ||
         (coveredEnd < end &&
          structure.ProveOrdinaryDirectTUInternalGap(coveredEnd, end));
}

bool RefoldTUEditPlanner::ValidateDirectTUEnvelope(
    StringRef tuPath, uint64_t begin, uint64_t end) const {
  const RefoldPreprocessingStructureIndex &structure =
      deps_.preprocessingStructureIndex;
  if (end < begin || end > deps_.tuSourceBytes.size() ||
      structure.GetOwnerIncludeId() ||
      !deps_.pathIdentity.PathsEqual(tuPath, deps_.model.GetSourcePath()) ||
      !deps_.pathIdentity.PathsEqual(structure.GetSourcePath(), tuPath) ||
      !structure.IsDirectTUProtectionCensusComplete() ||
      !structure.IsExactLexicalBoundary(begin) ||
      !structure.IsExactLexicalBoundary(end)) {
    return false;
  }

  if (!structure.HasOverlapping(begin, end))
    return true;

  std::vector<const PreprocessingStructureInterval *> macroIntervals;
  return CollectDirectTUMacroRepairEvidence(begin, end, macroIntervals);
}

bool RefoldTUEditPlanner::IsPPGapAtSelectedConditionalArmExit(
    uint64_t ppGap) const {
  if (ppGap == 0)
    return false;

  std::optional<RefoldModel::ArmRef> leftArm =
      deps_.model.FindArmRefAtPP(ppGap - 1);
  if (!leftArm || !leftArm->arm || !leftArm->arm->selected)
    return false;

  if (leftArm->arm->span && leftArm->arm->span->end != ppGap)
    return false;

  std::optional<RefoldModel::ArmRef> rightArm;
  if (ppGap < deps_.model.GetTokensCountA())
    rightArm = deps_.model.FindArmRefAtPP(ppGap);

  if (rightArm && rightArm->arm && rightArm->arm->id == leftArm->arm->id)
    return false;

  return true;
}

std::optional<uint64_t> RefoldTUEditPlanner::AnchorToExactSlotBoundaryFromPPGap(
    StringRef tuPath, uint64_t ppGap, TUAnchorWitness *witness,
    AcceptedResultCandidate *acceptedCandidate) const {
  // Build the normalized accepted carrier at the exact proof site. Callers that
  // only query anchoring still exercise the same proof-discharge/audit path as
  // callers that immediately emit the accepted TU insertion.
  auto recordAcceptedAnchorCandidate =
      [&](AcceptedPathKind path, const TUAnchorWitness &anchorWitness) {
        AcceptedResultCandidate candidate =
            deps_.tuAnchorProof.BuildAcceptedTUAnchorCandidate(path,
                                                               anchorWitness);
        if (acceptedCandidate)
          *acceptedCandidate = candidate;
      };

  struct Cand {
    uint64_t pp;
    uint64_t tuByte;
    const RefoldModel::Slot *slot;

    Cand(uint64_t pp, uint64_t tuByte, const RefoldModel::Slot *slot)
        : pp(pp), tuByte(tuByte), slot(slot) {}
  };

  auto bufOrErr = MemoryBuffer::getFile(deps_.lineDirs.ToAbsolutePath(tuPath));
  if (!bufOrErr) {
    REFOLD_LOG_FATAL("slot/anchor", "unable to read TU: {0}", tuPath);
  }
  StringRef tuText = bufOrErr.get()->getBuffer();

  auto adjustSlot = [&](const RefoldModel::Slot *slot) -> uint64_t {
    uint64_t byte = slot->b;

    // Producer arm_end slots may point at the directive following the selected
    // arm body.  For insertion replay, the equivalent PP gap belongs outside
    // the conditional group, so re-anchor to the group's closing directive when
    // the producer model gives enough evidence to do so.
    if (slot->kind == "arm_end" && slot->pp) {
      const uint64_t pp = *slot->pp;
      if (pp > 0) {
        if (auto armRef = deps_.model.FindArmRefAtPP(pp - 1)) {
          if (armRef->group &&
              deps_.pathIdentity.PathsEqual(armRef->group->file, tuPath)) {
            const uint64_t groupEnd = armRef->group->groupE;
            if (groupEnd <= tuText.size())
              return groupEnd;
          }
        }
      }
      return byte;
    }

    bool needsNoNewlineAdjustment =
        StringSwitch<bool>(slot->kind)
            .Cases("after_include", "after_last_include", true)
            .Default(false);
    if (needsNoNewlineAdjustment || byte >= tuText.size())
      return byte;

    char c = tuText[byte];
    if (c == '\n')
      return byte + 1;
    if (c == '\r') {
      if (static_cast<size_t>(byte + 1) < tuText.size() &&
          tuText[byte + 1] == '\n')
        return byte + 2;
      return byte + 1;
    }
    return byte;
  };

  std::vector<Cand> candidates;
  for (const auto &slot : deps_.model.FindSlots(tuPath, std::nullopt,
                                                std::nullopt, std::nullopt)) {
    if (!slot->pp)
      continue;

    bool isBoundary =
        StringSwitch<bool>(slot->kind)
            .Cases("file_begin", "file_end", "before_include", "after_include",
                   "after_last_include", "arm_begin", "arm_end", true)
            .Default(false);
    if (isBoundary)
      candidates.emplace_back(*slot->pp, adjustSlot(slot), slot);
  }

  if (candidates.empty())
    return std::nullopt;

  std::vector<const Cand *> exact;
  for (const auto &candidate : candidates) {
    if (candidate.pp == ppGap)
      exact.push_back(&candidate);
  }

  if (exact.empty())
    return std::nullopt;

  auto getPriority = [](StringRef kind) -> unsigned {
    if (kind == "before_include")
      return 0;
    if (kind == "after_include")
      return 1;
    if (kind == "after_last_include")
      return 2;
    if (kind == "arm_begin")
      return 3;
    if (kind == "arm_end")
      return 4;
    if (kind == "file_begin")
      return 5;
    if (kind == "file_end")
      return 6;
    return 100;
  };

  const Cand *best = nullptr;
  for (const auto *candidate : exact) {
    if (!best) {
      best = candidate;
      continue;
    }

    unsigned candidatePriority = getPriority(candidate->slot->kind);
    unsigned bestPriority = getPriority(best->slot->kind);
    if (std::tie(candidatePriority, candidate->tuByte, candidate->slot->id) <
        std::tie(bestPriority, best->tuByte, best->slot->id))
      best = candidate;
  }

  if (!best)
    return std::nullopt;

  TUAnchorWitness exactSlotWitness;
  exactSlotWitness.evidence = TUAnchorEvidenceKind::ExactSlotBoundary;
  exactSlotWitness.hasPPGap = true;
  exactSlotWitness.ppGap = ppGap;
  exactSlotWitness.hasTUByte = true;
  exactSlotWitness.tuByte = best->tuByte;
  exactSlotWitness.exactPPMatch = true;
  exactSlotWitness.slotId = best->slot->id;
  exactSlotWitness.slotKind = best->slot->kind.str();
  if (witness)
    *witness = exactSlotWitness;
  recordAcceptedAnchorCandidate(AcceptedPathKind::TUExactSlotBoundary,
                                exactSlotWitness);
  return best->tuByte;
}

std::optional<uint64_t>
RefoldTUEditPlanner::FindExactSlotBoundaryFromPPGap(StringRef tuPath,
                                                    uint64_t ppGap) const {
  return AnchorToExactSlotBoundaryFromPPGap(tuPath, ppGap, /*witness=*/nullptr,
                                            /*acceptedCandidate=*/nullptr);
}

std::optional<uint64_t>
RefoldTUEditPlanner::IncludeIdCoveringPPIndex(uint64_t pp) const {
  std::optional<uint64_t> bestId;
  uint64_t bestLen = std::numeric_limits<uint64_t>::max();

  for (const RefoldModel::IncludeItem &inc : deps_.model.GetIncludes()) {
    if (pp >= inc.cover.begin && pp < inc.cover.end) {
      uint64_t len = inc.cover.end - inc.cover.begin;
      if (len < bestLen) {
        bestLen = len;
        bestId = inc.id;
      }
    }
  }

  return bestId;
}

std::optional<TUInsertionAnchor>
RefoldTUEditPlanner::FindProvableTUInsertionAnchor(
    uint64_t pp, StringRef tuPath, TUAnchorWitness *witness,
    AcceptedResultCandidate *acceptedCandidate) const {
  // First prefer an exact structural slot anchor recorded by the producer.
  // These anchors are the strongest evidence because they identify a specific
  // TU byte boundary corresponding to this PP gap.
  TUAnchorWitness slotWitness;
  AcceptedResultCandidate slotCandidate;
  if (auto slotAnchor = AnchorToExactSlotBoundaryFromPPGap(
          tuPath, pp, &slotWitness, &slotCandidate)) {
    if (witness)
      *witness = slotWitness;
    if (acceptedCandidate)
      *acceptedCandidate = slotCandidate;
    return TUInsertionAnchor(pp, *slotAnchor, slotWitness);
  }

  // Build the accepted TU-anchor carrier at each successful proof site even
  // when the immediate caller only needs the byte anchor.  This helper is the
  // single bridge from local TU-anchor evidence to the normalized
  // accepted-result carrier; the optional out-parameter simply preserves the
  // winning carrier for callers that forward the anchor to emission.
  auto recordAcceptedAnchorCandidate =
      [&](AcceptedPathKind path, const TUAnchorWitness &anchorWitness) {
        AcceptedResultCandidate candidate =
            deps_.tuAnchorProof.BuildAcceptedTUAnchorCandidate(path,
                                                               anchorWitness);
        if (acceptedCandidate)
          *acceptedCandidate = candidate;
      };

  auto includeDirectiveBoundaryAnchor =
      [&]() -> std::optional<TUInsertionAnchor> {
    // Strict consumer-side proof for the narrow include-boundary case where a
    // producer slot would normally be present. A PP gap at the exact boundary
    // between two top-level include expansions denotes the TU source boundary
    // between the two include directive lines, but IncludeIdCoveringPPIndex(pp)
    // treats that same coordinate as being inside the right include's cover.
    // Recognize only the unambiguous form: two adjacent include directives in
    // this TU, with the left include ending at pp, the right include beginning
    // at pp, and no intervening source bytes between the directive records.
    const RefoldModel::IncludeItem *leftInc = nullptr;
    const RefoldModel::IncludeItem *rightInc = nullptr;

    for (const auto &inc : deps_.model.GetIncludes()) {
      if (!inc.cover.IsValid() ||
          !deps_.pathIdentity.PathsEqual(inc.sitePath, tuPath))
        continue;
      if (inc.cover.end == pp) {
        if (!leftInc ||
            std::tie(inc.siteE, inc.id) < std::tie(leftInc->siteE, leftInc->id))
          leftInc = &inc;
      }
      if (inc.cover.begin == pp) {
        if (!rightInc || std::tie(inc.siteB, inc.id) <
                             std::tie(rightInc->siteB, rightInc->id))
          rightInc = &inc;
      }
    }

    if (!leftInc || !rightInc || leftInc->id == rightInc->id)
      return std::nullopt;

    // Do not infer through nested include structure. This helper is only for a
    // TU insertion between two include directive lines spelled in tuPath.
    if (leftInc->parent || rightInc->parent)
      return std::nullopt;

    // If there are comments, blank lines, or any other bytes between the
    // directives, strict mode still requires an exact producer slot. Without
    // that slot there is no canonical byte inside the wider source gap.
    if (leftInc->siteE != rightInc->siteB)
      return std::nullopt;

    TUAnchorWitness includeBoundaryWitness;
    includeBoundaryWitness.evidence =
        TUAnchorEvidenceKind::IncludeDirectiveBoundary;
    includeBoundaryWitness.hasPPGap = true;
    includeBoundaryWitness.ppGap = pp;
    includeBoundaryWitness.hasTUByte = true;
    includeBoundaryWitness.tuByte = leftInc->siteE;
    includeBoundaryWitness.hasLeftNeighbor = true;
    includeBoundaryWitness.leftNeighborPP = pp - 1;
    includeBoundaryWitness.hasRightNeighbor = true;
    includeBoundaryWitness.rightNeighborPP = pp;
    includeBoundaryWitness.outsideIncludeCoverage = false;
    includeBoundaryWitness.ownerDepthStable = true;

    if (witness)
      *witness = includeBoundaryWitness;
    recordAcceptedAnchorCandidate(AcceptedPathKind::TUProvableInsertionAnchor,
                                  includeBoundaryWitness);
    return TUInsertionAnchor(pp, leftInc->siteE, includeBoundaryWitness);
  };

  auto zeroTokenIncludeBoundaryAnchor =
      [&]() -> std::optional<TUInsertionAnchor> {
    struct ZeroTokenIncludeBoundaryCandidate {
      const RefoldModel::IncludeItem *include = nullptr;
      const RefoldModel::Slot *beforeSlot = nullptr;
      const RefoldModel::Slot *afterSlot = nullptr;
      uint64_t inferredPPGap = 0;
    };

    auto firstExactSlot = [&](StringRef kind,
                              uint64_t includeId) -> const RefoldModel::Slot * {
      std::vector<const RefoldModel::Slot *> slots =
          deps_.model.FindSlots(tuPath, kind, includeId, std::nullopt);
      return slots.empty() ? nullptr : slots.front();
    };

    auto inferCollapsedPPGap = [&](const RefoldModel::IncludeItem &include)
        -> std::optional<uint64_t> {
      // A zero-token include has no PP range of its own.  It can still be tied
      // to a concrete PP gap when the nearest PP-producing material before and
      // after the directive agree on the same boundary.  This is a bracketing
      // proof, not a nearest-neighbor guess: disagreement means the zero-token
      // owner lies in a PP interval with real material, so the TU anchor is not
      // uniquely determined here.
      uint64_t leftGap = 0;
      uint64_t rightGap = deps_.model.GetTokensCountA();

      for (const auto &entry : deps_.model.GetTokmap()) {
        if (deps_.pathIdentity.PathsEqual(entry.file, tuPath)) {
          if (entry.e <= include.siteB)
            leftGap = std::max(leftGap, entry.pp + 1);
          if (entry.b >= include.siteE)
            rightGap = std::min(rightGap, entry.pp);
        }
      }

      for (const auto &other : deps_.model.GetIncludes()) {
        if (&other == &include || other.parent ||
            !deps_.pathIdentity.PathsEqual(other.sitePath, tuPath) ||
            !other.cover.IsValid())
          continue;
        if (other.siteE <= include.siteB)
          leftGap = std::max(leftGap, other.cover.end);
        if (other.siteB >= include.siteE)
          rightGap = std::min(rightGap, other.cover.begin);
      }

      if (leftGap != rightGap)
        return std::nullopt;
      return leftGap;
    };

    SmallVector<ZeroTokenIncludeBoundaryCandidate, 4> candidates;
    for (const auto &include : deps_.model.GetIncludes()) {
      // This proof is deliberately limited to top-level TU include directives
      // whose expansion produced no normal PP tokens.  Non-empty includes have
      // token/cover evidence and must use the ordinary include/TU ownership
      // paths; nested zero-token includes need their parent owner to host the
      // insertion rather than a TU byte edit.
      if (include.parent ||
          !deps_.pathIdentity.PathsEqual(include.sitePath, tuPath) ||
          include.cover.IsValid())
        continue;

      const RefoldModel::Slot *beforeSlot =
          firstExactSlot("before_include", include.id);
      const RefoldModel::Slot *afterSlot =
          firstExactSlot("after_include", include.id);
      if (!beforeSlot || !afterSlot)
        continue;

      // The source slots must name the concrete include directive line.  This
      // keeps the proof tied to producer-recorded source structure instead of
      // merely trusting an include item that lacks PP cover.
      if (beforeSlot->b != include.siteB || afterSlot->b != include.siteE)
        continue;

      std::optional<uint64_t> inferredPPGap = inferCollapsedPPGap(include);
      if (!inferredPPGap || *inferredPPGap != pp)
        continue;

      candidates.push_back(ZeroTokenIncludeBoundaryCandidate{
          &include, beforeSlot, afterSlot, *inferredPPGap});
    }

    if (candidates.empty())
      return std::nullopt;

    llvm::sort(candidates, [](const ZeroTokenIncludeBoundaryCandidate &lhs,
                              const ZeroTokenIncludeBoundaryCandidate &rhs) {
      if (lhs.beforeSlot->b != rhs.beforeSlot->b)
        return lhs.beforeSlot->b < rhs.beforeSlot->b;
      return lhs.include->id < rhs.include->id;
    });

    const ZeroTokenIncludeBoundaryCandidate &best = candidates.front();
    TUAnchorWitness zeroTokenWitness;
    zeroTokenWitness.evidence = TUAnchorEvidenceKind::ZeroTokenIncludeBoundary;
    zeroTokenWitness.hasPPGap = true;
    zeroTokenWitness.ppGap = pp;
    zeroTokenWitness.hasTUByte = true;
    zeroTokenWitness.tuByte = best.beforeSlot->b;
    zeroTokenWitness.slotId = best.beforeSlot->id;
    zeroTokenWitness.slotKind = best.beforeSlot->kind.str();
    zeroTokenWitness.outsideIncludeCoverage = true;
    zeroTokenWitness.ownerDepthStable = true;
    if (witness)
      *witness = zeroTokenWitness;
    recordAcceptedAnchorCandidate(AcceptedPathKind::TUProvableInsertionAnchor,
                                  zeroTokenWitness);
    return TUInsertionAnchor(pp, best.beforeSlot->b, zeroTokenWitness);
  };

  // Try the exact include-directive-boundary proof before the generic include
  // coverage wall. The boundary coordinate is allowed to equal the first token
  // of the right include expansion even though that makes
  // IncludeIdCoveringPPIndex report the right include as covering pp.
  if (auto includeAnchor = includeDirectiveBoundaryAnchor())
    return includeAnchor;

  // A zero-token top-level include collapses to an outer TU PP gap.  When the
  // producer supplied before/after include slots and the neighboring PP-bearing
  // material brackets the directive at exactly this gap, preserve the include
  // directive and anchor the insertion at the canonical outer boundary before
  // the zero-token owner.  This prevents an empty conditional/header from being
  // materialized merely to host a boundary-inherent insertion.
  if (auto zeroTokenIncludeAnchor = zeroTokenIncludeBoundaryAnchor())
    return zeroTokenIncludeAnchor;

  // A PP gap that lies inside an include expansion cannot be materialized as a
  // TU insertion. Fail closed before considering weaker local evidence.
  if (IncludeIdCoveringPPIndex(pp))
    return std::nullopt;

  // Look for an exact TU-side macro-projection begin at this PP gap and, when
  // one exists, lift it to the outermost matching caller so the returned anchor
  // is the stable callsite-begin byte for wrapper/deferred-expansion shapes.
  auto exactArgLikeBeginAnchor = [&]() -> std::optional<TUInsertionAnchor> {
    SmallVector<const RefoldModel::MacroInvocation *, 8> cands;
    DenseMap<uint64_t, const RefoldModel::MacroInvocation *> invById;
    invById.reserve(deps_.model.GetMacroInvocations().size());
    for (const auto &mi : deps_.model.GetMacroInvocations())
      invById[mi.id] = &mi;

    auto appendIfExactBegin = [&](const RefoldModel::MacroInvocation &m,
                                  auto &&spans) {
      for (const auto &sp : spans) {
        if (sp.begin == pp) {
          cands.push_back(&m);
          break;
        }
      }
    };

    for (const auto &m : deps_.model.GetMacroInvocations()) {
      // Only consider real TU-side invocations with stable byte-space
      // provenance. Ignore invocations inside macro definitions, since those do
      // not denote a concrete callsite insertion point in TU source.
      if (!m.invFile || !m.invB || !m.invE || !m.invText)
        continue;
      if (!deps_.pathIdentity.PathsEqual(*m.invFile, tuPath))
        continue;
      if (deps_.macroTopology.IsInvocationInsideDefineDirective(m))
        continue;

      // Treat argument, stringify, and paste projection starts as "arg-like"
      // begins. If the PP gap lands exactly on one of these begins, the outer
      // callsite begin can serve as a truthful TU insertion anchor.
      appendIfExactBegin(m, m.argSpans);
      if (!cands.empty() && cands.back() == &m)
        continue;
      appendIfExactBegin(m, m.stringifySpans);
      if (!cands.empty() && cands.back() == &m)
        continue;
      appendIfExactBegin(m, m.pasteSpans);
    }

    if (cands.empty())
      return std::nullopt;

    SmallDenseSet<uint64_t, 8> candIds;
    for (const auto *m : cands)
      candIds.insert(m->id);

    // Walk from a candidate invocation to the outermost candidate-owned caller
    // in the same macro expansion chain. Stop at the first caller that is not
    // part of this candidate set or cannot be resolved in the invocation index.
    auto rootmostCand = [&](const RefoldModel::MacroInvocation *m) {
      const RefoldModel::MacroInvocation *cur = m;
      while (cur && cur->callerMacroId) {
        auto idIt = candIds.find(*cur->callerMacroId);
        if (idIt == candIds.end())
          break;
        auto parentIt = invById.find(*cur->callerMacroId);
        if (parentIt == invById.end())
          break;
        cur = parentIt->second;
      }
      return cur;
    };

    const RefoldModel::MacroInvocation *best = nullptr;
    for (const auto *m : cands) {
      const auto *root = rootmostCand(m);
      if (!root || !root->invB)
        continue;

      // Prefer the outermost candidate among the matching nested invocations,
      // then break ties by earliest callsite begin. This yields the most stable
      // TU anchor for wrapper/deferred-expansion patterns.
      if (!best ||
          std::tie(*root->invB, root->id) < std::tie(*best->invB, best->id)) {
        best = root;
      }
    }

    if (!best)
      return std::nullopt;

    TUAnchorWitness argLikeWitness;
    argLikeWitness.evidence = TUAnchorEvidenceKind::ArgLikeBegin;
    argLikeWitness.hasPPGap = true;
    argLikeWitness.ppGap = pp;
    argLikeWitness.hasTUByte = true;
    argLikeWitness.tuByte = *best->invB;
    argLikeWitness.macroId = best->id;
    argLikeWitness.outsideIncludeCoverage = true;
    if (witness)
      *witness = argLikeWitness;
    recordAcceptedAnchorCandidate(AcceptedPathKind::TUProvableInsertionAnchor,
                                  argLikeWitness);
    return TUInsertionAnchor(pp, *best->invB, argLikeWitness);
  };

  // Next try the exact "arg-like begin" rule used for outer wrapper callsites.
  // This handles empty-gap edits that are semantically attached to the start of
  // a TU macro invocation rather than to an immediately mapped PP token.
  if (auto argAnchor = exactArgLikeBeginAnchor())
    return argAnchor;

  const auto &tokmapByPP = deps_.model.GetTokmapByPP();

  // First try the mapped token immediately to the right of the PP gap. If it
  // belongs to the TU, anchor at that token's begin byte; if it is mapped to a
  // different file, fail closed rather than probing past contradictory local
  // evidence.
  if (pp < deps_.model.GetTokensCountA()) {
    auto rightIt = tokmapByPP.find(pp);
    if (rightIt != tokmapByPP.end()) {
      const auto &right = rightIt->second;
      if (deps_.pathIdentity.PathsEqual(tuPath, right.file)) {
        TUAnchorWitness rightWitness;
        rightWitness.evidence = TUAnchorEvidenceKind::ImmediateRightNeighbor;
        rightWitness.hasPPGap = true;
        rightWitness.ppGap = pp;
        rightWitness.hasTUByte = true;
        rightWitness.tuByte = right.b;
        rightWitness.hasRightNeighbor = true;
        rightWitness.rightNeighborPP = pp;
        rightWitness.outsideIncludeCoverage = true;
        if (witness)
          *witness = rightWitness;
        recordAcceptedAnchorCandidate(
            AcceptedPathKind::TUProvableInsertionAnchor, rightWitness);
        return TUInsertionAnchor(pp, right.b, rightWitness);
      }
      return std::nullopt;
    }
  }

  // Otherwise try the mapped token immediately to the left. If it belongs to
  // the TU, anchor at that token's end byte; if it belongs elsewhere, fail
  // closed.
  if (pp > 0) {
    auto leftIt = tokmapByPP.find(pp - 1);
    if (leftIt != tokmapByPP.end()) {
      const auto &left = leftIt->second;
      if (deps_.pathIdentity.PathsEqual(tuPath, left.file)) {
        TUAnchorWitness leftWitness;
        leftWitness.evidence = TUAnchorEvidenceKind::ImmediateLeftNeighbor;
        leftWitness.hasPPGap = true;
        leftWitness.ppGap = pp;
        leftWitness.hasTUByte = true;
        leftWitness.tuByte = left.e;
        leftWitness.hasLeftNeighbor = true;
        leftWitness.leftNeighborPP = pp - 1;
        leftWitness.outsideIncludeCoverage = true;
        if (witness)
          *witness = leftWitness;
        recordAcceptedAnchorCandidate(
            AcceptedPathKind::TUProvableInsertionAnchor, leftWitness);
        return TUInsertionAnchor(pp, left.e, leftWitness);
      }
      return std::nullopt;
    }
  }

  // Fail closed: without an exact structural anchor, an exact arg-like anchor,
  // or an immediate mapped TU neighbor, the gap is not provably TU-owned.
  // Proximity to a nearby mapped TU token across intervening unmapped positions
  // is corroboration, not provenance -- a nearest-neighbor guess the prime
  // directive forbids -- so we do not invent a zero-width anchor here. The
  // caller falls back to a sound realization instead.
  return std::nullopt;
}

std::optional<TUByteSpanPlan>
RefoldTUEditPlanner::ProjectTUByteEnvelopeForOwnership(
    uint64_t a0, uint64_t a1, StringRef tuPath) const {
  if (a0 > a1)
    std::swap(a0, a1);

  if (a0 == a1) {
    if (auto anchor = FindProvableTUInsertionAnchor(a0, tuPath))
      return TUByteSpanPlan(a0, a1, anchor->tuByteOffset,
                            anchor->tuByteOffset, std::move(anchor));
    return std::nullopt;
  }

  // This is a topology projection only.  It deliberately preserves the legacy
  // min/max TU-owned subset behavior used by owner classification and does not
  // authorize a source edit.  Concrete direct realization must call
  // PlanTUByteSpan(), whose theorem is strictly stronger.
  uint64_t minimumBegin = std::numeric_limits<uint64_t>::max();
  uint64_t maximumEnd = 0;
  bool foundTUToken = false;
  const auto &tokmapByPP = deps_.model.GetTokmapByPP();
  for (uint64_t pp = a0; pp < a1; ++pp) {
    auto entryIt = tokmapByPP.find(pp);
    if (entryIt == tokmapByPP.end())
      continue;
    const RefoldModel::TokMapEntry &entry = entryIt->second;
    if (!deps_.pathIdentity.PathsEqual(entry.file, tuPath))
      continue;
    minimumBegin = std::min(minimumBegin, entry.b);
    maximumEnd = std::max(maximumEnd, entry.e);
    foundTUToken = true;
  }
  if (!foundTUToken)
    return std::nullopt;
  return TUByteSpanPlan(a0, a1, minimumBegin, maximumEnd);
}

std::optional<TUByteSpanPlan>
RefoldTUEditPlanner::PlanTUByteSpan(uint64_t a0, uint64_t a1,
                                    StringRef tuPath) const {
  if (a0 > a1)
    std::swap(a0, a1);

  const bool isEmpty = (a0 == a1);
  const auto &tokmapByPP = deps_.model.GetTokmapByPP();

  // Emit one deterministic rejection block at the exact direct-TU proof
  // boundary.  The diagnostic projection below is evidence-only: it reuses the
  // legacy min/max TU ownership envelope solely to explain what a rejected edit
  // would have consumed.  It never participates in admission.  In particular,
  // listing a protected interval does not manufacture structural-tiling
  // authority; the generalized tiler must still prove an exact partition.
  auto reject = [&](StringRef reason,
                    StringRef requiredAction) -> std::optional<TUByteSpanPlan> {
    if (!inTraceMode())
      return std::nullopt;

    REFOLD_LOG_TRACE("tu/direct-span", "direct TU span rejected:");
    REFOLD_LOG_TRACE("tu/direct-span", "A=[{0},{1})", a0, a1);
    REFOLD_LOG_TRACE("tu/direct-span", "reason={0}", reason);

    bool listedProtectedStructure = false;
    if (!isEmpty) {
      std::optional<TUByteSpanPlan> candidate =
          ProjectTUByteEnvelopeForOwnership(a0, a1, tuPath);
      if (candidate) {
        REFOLD_LOG_TRACE("tu/direct-span", "candidate source=[{0},{1})",
                         candidate->tuByteBegin, candidate->tuByteEnd);
        for (const PreprocessingStructureInterval *interval :
             deps_.preprocessingStructureIndex.FindOverlapping(
                 candidate->tuByteBegin, candidate->tuByteEnd)) {
          listedProtectedStructure = true;
          REFOLD_LOG_TRACE(
              "tu/direct-span",
              "protected structure=[{0},{1}) {2} kind={3} model={4} "
              "modelItem={5} conditionalGroup={6} conditionalArm={7} "
              "ownerArm={8}",
              interval->begin, interval->end,
              directTUDiagnosticDirectiveSpelling(interval->kind),
              interval->kind, interval->modelKind,
              interval->modelItemId, interval->conditionalGroupId,
              interval->conditionalArmId, interval->ownerConditionalArmId);
        }
      } else {
        REFOLD_LOG_TRACE("tu/direct-span", "candidate source=<unavailable>");
      }
    } else {
      REFOLD_LOG_TRACE("tu/direct-span",
                       "candidate source=<zero-width insertion anchor>");
    }

    if (!listedProtectedStructure)
      REFOLD_LOG_TRACE("tu/direct-span", "protected structure=<none listed>");
    REFOLD_LOG_TRACE("tu/direct-span", "required action={0}",
                     requiredAction);
    return std::nullopt;
  };

  if (isEmpty) {
    if (auto anchor = FindProvableTUInsertionAnchor(a0, tuPath))
      return TUByteSpanPlan(a0, a1, anchor->tuByteOffset,
                            anchor->tuByteOffset, std::move(anchor));
    return reject("no provable TU insertion anchor",
                  "ordinary insertion owner/fallback path");
  }

  // A nonempty direct-TU realization is a one-owner physical-source theorem,
  // not a best-effort min/max projection.  Every consumed A token must have one
  // unambiguous, complete, in-bounds TU spelling.  Producer binding failures
  // elsewhere in the file do not poison this local proof: lexically discovered
  // unbound directives remain protected intervals and reject only overlapping
  // spans.  A protection-census failure does poison the proof because some
  // conditional or pragma state then lacks a complete physical boundary.
  if (a1 > deps_.model.GetTokensCountA() ||
      a1 > static_cast<uint64_t>(deps_.aTokens.size())) {
    return reject("A-token envelope is outside the producer/token stream",
                  "safe owner/fallback path");
  }
  if (!deps_.pathIdentity.PathsEqual(tuPath, deps_.model.GetSourcePath()) ||
      !deps_.pathIdentity.PathsEqual(
          tuPath, deps_.preprocessingStructureIndex.GetSourcePath()) ||
      deps_.preprocessingStructureIndex.GetOwnerIncludeId()) {
    return reject("requested source owner is not the direct translation unit",
                  "owner-specific realization/fallback path");
  }
  if (!deps_.preprocessingStructureIndex
           .IsDirectTUProtectionCensusComplete()) {
    return reject("direct-TU preprocessing protection census is incomplete",
                  "safe fallback after structure-census repair");
  }

  uint64_t spanBegin = 0;
  uint64_t spanEnd = 0;
  uint64_t previousBegin = 0;
  uint64_t previousEnd = 0;
  bool havePrevious = false;

  for (uint64_t pp = a0; pp < a1; ++pp) {
    if (duplicateTokmapPP_.count(pp) != 0) {
      return reject("duplicate producer token mapping inside A envelope",
                    "safe owner/fallback path");
    }

    auto entryIt = tokmapByPP.find(pp);
    if (entryIt == tokmapByPP.end()) {
      return reject("missing producer token mapping inside A envelope",
                    "safe owner/fallback path");
    }

    const RefoldModel::TokMapEntry &entry = entryIt->second;
    if (entry.pp != pp ||
        !deps_.pathIdentity.PathsEqual(entry.file, tuPath) ||
        entry.b >= entry.e || entry.e > deps_.tuSourceBytes.size() ||
        !deps_.preprocessingStructureIndex.IsExactTokenSpellingInterval(
            entry.b, entry.e)) {
      return reject("A token lacks one exact in-bounds TU spelling",
                    "owner-specific realization/fallback path");
    }

    if (!havePrevious) {
      spanBegin = entry.b;
      spanEnd = entry.e;
      previousBegin = entry.b;
      previousEnd = entry.e;
      havePrevious = true;
      continue;
    }

    if (entry.b < previousBegin || entry.b < previousEnd) {
      return reject("mapped TU token spellings are nonmonotone or overlap",
                    "safe owner/fallback path");
    }

    if (previousEnd < entry.b &&
        !ProveDirectTUGapWithMacroRepairEvidence(previousEnd, entry.b)) {
      return reject("internal source gap is neither exact ordinary direct-TU "
                    "trivia nor complete macro-state repair evidence",
                    "structural hunk tiling");
    }

    previousBegin = entry.b;
    previousEnd = entry.e;
    spanEnd = entry.e;
  }

  if (!havePrevious) {
    return reject("A envelope contains no directly mapped TU token",
                  "safe owner/fallback path");
  }
  if (!ValidateDirectTUEnvelope(tuPath, spanBegin, spanEnd)) {
    return reject("candidate TU envelope contains unproved protected source",
                  "structural hunk tiling or directive-specific fallback");
  }

  return TUByteSpanPlan(a0, a1, spanBegin, spanEnd);
}

bool RefoldTUEditPlanner::InsertionSuppliesMacroBoundaryLiteral(
    const diffutils::Hunk &hunk) const {
  // The first geometry below is `RefoldMacroDefinitionTapeSolver'`s
  // left-boundary slide predicate: that solver owns the seam, and reaching a
  // direct-TU carrier means it already declined. The second geometry is not
  // one that solver claims, so no service has tried to absorb the token there;
  // this refuses it outright, which is conservative rather than proved
  // complete. The spelling test below keeps both narrow.
  if (!hunk.isInsertOnly() || hunk.bStart >= hunk.bEnd)
    return false;

  // Two gaps admit the slide against one invocation: the gap before the TU
  // token that precedes the expansion, and the gap before the expansion's own
  // first token. Both put the insertion inside the run of identical literals
  // that spans the seam, so probe the invocation from either side.
  for (const uint64_t probe : {hunk.aStart, hunk.aStart + 1}) {
    const RefoldModel::MacroInvocation *invocation =
        deps_.macroTopology.SmallestCoveringPatchableMacro(probe, probe + 1);
    if (!invocation || !invocation->invPPByteBegin || !invocation->invPPByteEnd)
      continue;

    const std::optional<std::pair<uint64_t, uint64_t>> cover =
        RefoldMacroWholeCoverProof::GetWholeCoverATokRange(*invocation);
    if (!cover || cover->first == 0 ||
        (hunk.aStart != cover->first && hunk.aStart + 1 != cover->first) ||
        cover->first >= deps_.aTokens.size())
      continue;

    // The seam is ambiguous only when the two tokens flanking it are spelled
    // identically: the TU token preceding the invocation and the leading fixed
    // token of its expansion. That equality is what lets the token LCS anchor
    // the TU token to the macro-body literal and orphan the invocation's own
    // literal into a pure insertion. When the spellings differ, no slide is
    // possible and the inserted tokens are unambiguously ordinary TU text --
    // for example a `[0]` subscript appearing immediately before a callsite,
    // which must keep its direct-TU carrier.
    if (deps_.aTokens[static_cast<size_t>(cover->first) - 1].spelling !=
        deps_.aTokens[static_cast<size_t>(cover->first)].spelling)
      continue;

    REFOLD_LOG_DEBUG(
        "tu/insert",
        "rejecting direct-TU insertion at A gap {0}: B=[{1},{2}) would supply "
        "a replacement-list literal of macro {3} ({4}) whose expansion cover "
        "is A=[{5},{6})",
        hunk.aStart, hunk.bStart, hunk.bEnd, invocation->id, invocation->name,
        cover->first, cover->second);
    return true;
  }
  return false;
}

bool RefoldTUEditPlanner::ValidateTUOwnerRealizationCarrier(
    const diffutils::Hunk &hunk, const TUByteSpanPlan &span,
    const StructuralHunkSegmentBinding *structuralBinding) const {
  // A direct owner carrier must describe the exact token hunk that produced
  // the edit.  In particular, a valid zero-width A range is a pure insertion,
  // not permission to replace an arbitrary source interval with manufactured
  // A=[0,0) / B=[0,0) proof coordinates.
  if (hunk.aEnd < hunk.aStart || hunk.bEnd < hunk.bStart ||
      hunk.aEnd > static_cast<uint64_t>(deps_.aTokens.size()) ||
      hunk.aEnd > deps_.model.GetTokensCountA() ||
      hunk.bEnd > deps_.bTokenCount ||
      (hunk.aStart == hunk.aEnd && hunk.bStart == hunk.bEnd) ||
      span.aTokenBegin != hunk.aStart || span.aTokenEnd != hunk.aEnd ||
      span.tuByteEnd < span.tuByteBegin) {
    return false;
  }

  if (structuralBinding) {
    // Structural authority is segment-specific.  It may explain why an
    // original hunk was split, but it may not widen this segment's source or
    // token envelopes, and it may not be represented by a bare witness id.
    if (!structuralBinding->IsWellFormed() ||
        structuralBinding->segmentAStart != hunk.aStart ||
        structuralBinding->segmentAEnd != hunk.aEnd ||
        structuralBinding->segmentBStart != hunk.bStart ||
        structuralBinding->segmentBEnd != hunk.bEnd ||
        structuralBinding->sourceBegin != span.tuByteBegin ||
        structuralBinding->sourceEnd != span.tuByteEnd) {
      return false;
    }
  }

  const StringRef tuPath = deps_.model.GetSourcePath();
  std::optional<TUByteSpanPlan> baseSpan =
      PlanTUByteSpan(hunk.aStart, hunk.aEnd, tuPath);
  if (!baseSpan)
    return false;

  if (hunk.isInsertOnly()) {
    // An inserted token that is really a macro replacement-list literal has no
    // direct-TU realization. Reject before the anchor theorems below, which
    // prove where the insertion lands but not that the TU owns what it emits.
    if (InsertionSuppliesMacroBoundaryLiteral(hunk))
      return false;

    // A pure insertion retains the exact producer/provenance anchor even when
    // a separate theorem consumes adjacent trivia or moves the insertion past
    // a preserved source #line prefix.  The base anchor therefore remains the
    // carrier identity; the final physical edit must either contain that anchor
    // or carry the typed line-control adjustment proved below.
    if (!span.insertionAnchor || !baseSpan->insertionAnchor ||
        span.insertionAnchor->ppGap != hunk.aStart ||
        baseSpan->insertionAnchor->ppGap != hunk.aStart ||
        span.insertionAnchor->tuByteOffset != baseSpan->tuByteBegin ||
        baseSpan->insertionAnchor->tuByteOffset != baseSpan->tuByteBegin ||
        !ValidateOrdinaryDirectTUEnvelope(tuPath, span.tuByteBegin,
                                          span.tuByteEnd)) {
      return false;
    }

    const uint64_t baseAnchor = baseSpan->tuByteBegin;
    if (!span.insertionAnchorAdjustment) {
      return span.tuByteBegin <= baseAnchor &&
             baseAnchor <= span.tuByteEnd;
    }

    const TUInsertionAnchorAdjustment &adjustment =
        *span.insertionAnchorAdjustment;
    if (!adjustment.IsValid() ||
        adjustment.kind !=
            TUInsertionAnchorAdjustmentKind::SourceLineControlPrefix ||
        adjustment.originalTUByteOffset != baseAnchor ||
        adjustment.adjustedTUByteOffset != span.tuByteBegin ||
        span.tuByteBegin != span.tuByteEnd) {
      return false;
    }

    // The adjustment theorem leaves the skipped #line structure physically in
    // place.  Revalidate that the complete byte interval between the original
    // and adjusted anchors consists only of exact producer-bound line-control
    // directives and lexer trivia; no other preprocessing state may be crossed.
    uint64_t cursor = adjustment.originalTUByteOffset;
    for (const PreprocessingStructureInterval *interval :
         deps_.preprocessingStructureIndex.FindOverlapping(
             adjustment.originalTUByteOffset,
             adjustment.adjustedTUByteOffset)) {
      if (interval->begin < adjustment.originalTUByteOffset ||
          adjustment.adjustedTUByteOffset < interval->end ||
          interval->kind != PreprocessingStructureKind::LineControl ||
          interval->modelKind !=
              PreprocessingStructureModelKind::LineControlEvent ||
          !interval->modelItemId || interval->begin < cursor ||
          !deps_.preprocessingStructureIndex.IsRangeLexicallyIgnorable(
              cursor, interval->begin)) {
        return false;
      }
      cursor = interval->end;
    }
    return cursor > adjustment.originalTUByteOffset &&
           deps_.preprocessingStructureIndex.IsRangeLexicallyIgnorable(
               cursor, adjustment.adjustedTUByteOffset);
  }

  // Boundary widening performed by a separate suffix/separator theorem may
  // make the emitted source interval larger than the base token carrier. It
  // still has to contain that independently re-derived carrier. Any macro
  // transition in the wider envelope remains provisional evidence only and
  // must later be discharged by the specialized repair planner.
  if (span.tuByteBegin > baseSpan->tuByteBegin ||
      baseSpan->tuByteEnd > span.tuByteEnd) {
    return false;
  }

  return DirectTUEnvelopeRetainsMacroRepairEvidence(
      tuPath, baseSpan->tuByteBegin, baseSpan->tuByteEnd, span.tuByteBegin,
      span.tuByteEnd);
}

std::optional<BoundaryParentIncludePlan>
RefoldTUEditPlanner::FindBoundaryParentIncludeForPureInsertion(
    const diffutils::Hunk &h) const {
  // This helper only applies to a pure insertion: the hunk must consume no
  // A-side tokens, but it must insert at least one B-side token.
  if (!h.isInsertOnly()) {
    return std::nullopt;
  }

  const uint64_t aPos = h.aStart;

  // We deliberately avoid "nearest token" probing here. A pure insertion is
  // attributed to an include only when the PP gap lands exactly on a recorded
  // include boundary.
  //
  // Collect the narrowest include ending at this gap (immediately on the left)
  // and the narrowest include beginning at this gap (immediately on the right).
  // Preferring the narrowest match lets an exact nested boundary beat any
  // enclosing include that shares the same endpoint.
  const RefoldModel::IncludeItem *leftBest = nullptr;
  uint64_t leftWidth = std::numeric_limits<uint64_t>::max();

  const RefoldModel::IncludeItem *rightBest = nullptr;
  uint64_t rightWidth = std::numeric_limits<uint64_t>::max();

  for (const auto &inc : deps_.model.GetIncludes()) {
    if (!inc.cover.IsValid())
      continue;

    const uint64_t width = inc.cover.end - inc.cover.begin;

    // Include immediately to the left of the insertion gap.
    if (inc.cover.end == aPos) {
      if (width < leftWidth) {
        leftBest = &inc;
        leftWidth = width;
      }
    }

    // Include immediately to the right of the insertion gap.
    if (inc.cover.begin == aPos) {
      if (width < rightWidth) {
        rightBest = &inc;
        rightWidth = width;
      }
    }
  }

  const std::optional<uint64_t> leftIncId =
      leftBest ? std::optional<uint64_t>(leftBest->id) : std::nullopt;
  const std::optional<uint64_t> rightIncId =
      rightBest ? std::optional<uint64_t>(rightBest->id) : std::nullopt;

  // If neither side hits an exact include boundary, this insertion cannot be
  // attributed to an include via boundary ownership.
  if (!leftIncId && !rightIncId)
    return std::nullopt;

  // When the gap sits between two include boundaries, attribute it to the
  // structural parent shared by the left and right side. This handles both
  // "between siblings" and "at one side only" cases uniformly.
  const std::optional<uint64_t> parentId =
      deps_.model.LeastCommonAncestorInclude(leftIncId, rightIncId);
  if (!parentId)
    return std::nullopt;

  const RefoldModel::IncludeItem *parent =
      deps_.model.GetIncludeById(*parentId);
  if (!parent)
    return std::nullopt;

  return BoundaryParentIncludePlan(aPos, parent->id, parent->parent);
}

bool RefoldTUEditPlanner::TUReplacementExtensionIsBTokenClosed(
    uint64_t aTokStart, uint64_t oldEnd, uint64_t extEnd, uint64_t bStart,
    uint64_t bEnd, StringRef tuPath) const {
  if (extEnd <= oldEnd)
    return true;
  if (deps_.abTokenMapA2B.empty())
    return false;

  const uint64_t tokCount = static_cast<uint64_t>(deps_.aTokens.size());
  for (uint64_t aTok = std::min(aTokStart, tokCount); aTok < tokCount; ++aTok) {
    std::optional<TUByteSpanPlan> span = PlanTUByteSpan(aTok, aTok + 1, tuPath);
    if (!span)
      continue;

    if (span->tuByteEnd <= oldEnd)
      continue;
    if (span->tuByteBegin >= extEnd)
      break;

    // A partial-token overlap would mean the byte extension cut through an
    // A token. There is no token-closure proof for that shape, so preserve
    // the suffix rather than widening the edit.
    if (span->tuByteBegin < oldEnd || extEnd < span->tuByteEnd)
      return false;

    if (aTok >= static_cast<uint64_t>(deps_.abTokenMapA2B.size()))
      return false;

    const int64_t mappedB = deps_.abTokenMapA2B[static_cast<size_t>(aTok)];
    if (mappedB < 0)
      continue;

    if (static_cast<uint64_t>(mappedB) < bStart ||
        static_cast<uint64_t>(mappedB) >= bEnd)
      return false;
  }

  return true;
}

std::optional<TUTrailingCallSuffixExtension>
RefoldTUEditPlanner::MaybeExtendTUSpanOverClosedTrailingCallSuffix(
    const diffutils::Hunk &h, StringRef tuPath, StringRef tuBytes,
    StringRef replacement, const TUByteSpanPlan &initialSpan) const {
  if (initialSpan.tuByteBegin >= initialSpan.tuByteEnd)
    return std::nullopt;

  const uint64_t oldEnd = initialSpan.tuByteEnd;
  const uint64_t extEnd =
      stringutils::extendChainedCallEnd(tuBytes, oldEnd, replacement);

  // Closed trailing call-suffix extension is not ordinary byte-span widening.
  // It is allowed only when the B-token suffix is independently closed and does
  // not require macro replay proof from the TU replacement itself. Keep this
  // policy isolated so TU edit construction cannot accidentally widen spans.
  const bool closed =
      extEnd != oldEnd && TUReplacementExtensionIsBTokenClosed(
                              h.aEnd, oldEnd, extEnd, h.bStart, h.bEnd, tuPath);
  if (!closed ||
      !ValidateOrdinaryDirectTUEnvelope(tuPath, oldEnd, extEnd))
    return std::nullopt;

  return TUTrailingCallSuffixExtension(h.aEnd, h.aEnd, oldEnd, extEnd, h.bStart,
                                       h.bEnd,
                                       /*bTokenSuffixClosed=*/true);
}

void RefoldTUEditPlanner::MaybeExtendTUSpanOverClosedTrailingCallSuffix(
    const diffutils::Hunk &h, StringRef tuPath, StringRef tuBytes,
    StringRef replacement, std::pair<uint64_t, uint64_t> &span) const {
  TUByteSpanPlan initialSpan(h.aStart, h.aEnd, span.first, span.second);
  if (auto extension = MaybeExtendTUSpanOverClosedTrailingCallSuffix(
          h, tuPath, tuBytes, replacement, initialSpan))
    span.second = extension->extendedTUByteEnd;
}

std::optional<DirectTUHunkEditPlan>
RefoldTUEditPlanner::BuildDirectTUHunkEditPlan(
    const diffutils::Hunk &h, uint64_t hunkIndex,
    const std::pair<uint64_t, uint64_t> &span, ResyncOutcome resync,
    StringRef acceptedPayload, uint64_t rawTUStart, uint64_t rawTUEnd,
    std::optional<uint64_t> materializedBByteBegin,
    std::optional<uint64_t> materializedBByteEnd,
    AcceptedPathKind acceptedPath,
    std::optional<TUInsertionAnchorAdjustment> insertionAnchorAdjustment) const {
  // Re-prove the original A-token carrier at the last direct-plan
  // construction boundary. Callers may legitimately widen the already-proved
  // raw range through a specialized suffix/separator theorem, but they may not
  // manufacture a different raw carrier or let the final physical edit cross
  // preprocessing structure. Pure insertions retain their exact base anchor;
  // movement past source #line prefixes requires the typed adjustment witness.
  std::optional<TUByteSpanPlan> rawSpan = PlanTUByteSpan(
      h.aStart, h.aEnd, deps_.model.GetSourcePath());
  if (!rawSpan || rawSpan->tuByteBegin != rawTUStart ||
      rawSpan->tuByteEnd != rawTUEnd || span.second < span.first) {
    return std::nullopt;
  }

  TUByteSpanPlan spanPlan(h.aStart, h.aEnd, span.first, span.second,
                          rawSpan->insertionAnchor,
                          std::move(insertionAnchorAdjustment));
  if (!ValidateTUOwnerRealizationCarrier(h, spanPlan))
    return std::nullopt;

  // Direct-TU hunk planning records the already-proved TU byte range and all
  // hunk-local attribution needed by the eventual TextEdit. It intentionally
  // does not certify accepted-result carriers or participate in final edit
  // ordering; those remain the assembler/audit boundary's responsibility.
  return DirectTUHunkEditPlan(h, hunkIndex, std::move(spanPlan),
                              std::optional<ResyncOutcome>(std::move(resync)),
                              acceptedPayload.str(), rawTUStart, rawTUEnd,
                              materializedBByteBegin, materializedBByteEnd,
                              acceptedPath);
}

bool maybeAdvanceTUInsertionPastSourceLineControlPrefix(
    const RefoldTUEditPlanner &planner,
    const RefoldLineControlProof &lineControlProof, const diffutils::Hunk &h,
    StringRef tuPath, StringRef tuBytes, std::pair<uint64_t, uint64_t> &span) {
  if (!h.isInsertOnly() || span.first != span.second ||
      planner.IsPPGapAtSelectedConditionalArmExit(h.aStart))
    return false;

  std::optional<uint64_t> exactAnchor =
      planner.AnchorToExactSlotBoundaryFromPPGap(tuPath, h.aStart);
  if (!exactAnchor || *exactAnchor != span.first)
    return false;

  std::optional<uint64_t> advancedAnchor =
      lineControlProof.AdvanceInsertionAnchorPastSourceLineControlPrefix(
          tuPath, std::nullopt, tuBytes, span.first);
  if (!advancedAnchor)
    return false;

  span.first = *advancedAnchor;
  span.second = *advancedAnchor;
  return true;
}

} // namespace refold
} // namespace clang
