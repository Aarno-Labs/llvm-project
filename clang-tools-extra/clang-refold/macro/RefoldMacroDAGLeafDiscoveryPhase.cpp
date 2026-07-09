//===--- RefoldMacroDAGLeafDiscoveryPhase.cpp -------------------*- C++ -*-===//
//
// Implementation of DAG leaf discovery for args-only whole-cover replay.
// Inputs are read from `RefoldMacroWholeCoverPlanningContext`, outputs are
// written through `DAGLeafDiscoveryResult`, and planner-owned lexical helpers
// are reached through explicit dependency callbacks supplied to the phase.
//
//===----------------------------------------------------------------------===//

#include "macro/RefoldMacroDAGLeafDiscoveryPhase.h"

#include "core/RefoldLog.h"
#include "core/RefoldModel.h"
#include "line-control/RefoldLineObserverLayout.h"
#include "macro/RefoldArgTextRecovery.h"
#include "macro/RefoldMacroDAGSharedHelpers.h"
#include "macro/RefoldMacroOccurrenceProofValidator.h"
#include "macro/RefoldMacroPlannerHelpers.h"
#include "macro/RefoldMacroSubtreeReplayValidator.h"
#include "macro/RefoldMacroTopology.h"
#include "macro/RefoldMacroWholeCoverPlanningContext.h"
#include "source/RefoldSourceMapper.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallVector.h"

#include <algorithm>
#include <utility>

using namespace llvm;

namespace clang {
namespace refold {

RefoldMacroDAGLeafDiscoveryPhase::RefoldMacroDAGLeafDiscoveryPhase(
    Dependencies deps)
    : deps_(std::move(deps)) {}

void RefoldMacroDAGLeafDiscoveryPhase::Run(
    const RefoldMacroWholeCoverPlanningContext &planningCtx,
    DAGLeafDiscoveryResult &result) const {
  // Mirror the lambda's outer-scope captures with local aliases so the
  // body below reads identically to the original code.
  const RefoldModel::MacroInvocation &m = planningCtx.m;
  const diffutils::Hunk &h = planningCtx.h;
  const diffutils::Hunk &hEff = planningCtx.hEff;
  StringRef baseInvText = planningCtx.baseInvText;
  ArrayRef<RefoldModel::PPArgSpan> argLikeSpans = planningCtx.argLikeSpans;
  const bool rootHasDirectArgLikeSurface =
      planningCtx.rootHasDirectArgLikeSurface;

  // On-demand occurrence-proof validator (the planner constructs one on
  // every accessor call; the phase mirrors that pattern but builds it
  // from explicit deps instead of going through a back-reference).
  RefoldMacroOccurrenceProofValidator occurrenceProofValidator(
      RefoldMacroOccurrenceProofValidator::Dependencies{&deps_.model,
                                                        &deps_.macroTopology});

  // --- Root invocation parsing preconditions -----------------------------
  //
  // We can only "lift" edits back into the current root invocation (m) if
  // we can reliably parse *its current spelling* into formal argument byte
  // ranges. This must be the callsite text (not some expanded body text).
  result.invSpanText = !baseInvText.empty() ? baseInvText
                                            : (m.invText ? StringRef(*m.invText)
                                                         : StringRef(""));
  StringRef invSpanText = result.invSpanText;

  if (!RefoldLineObserverLayout::InvocationSpanMatchesCallsitePrefix(
          invSpanText, m)) {
    result.aborted = true;
    return;
  }

  // Parse the byte ranges of each *formal argument* within invSpanText.
  // This is the target surface we will rewrite if lifting succeeds.
  auto invArgRangesOpt =
      deps_.getMacroInvocationFormalArgContentRanges(m, invSpanText);
  if (!invArgRangesOpt) {
    result.aborted = true;
    return;
  }
  result.invArgRanges = std::move(*invArgRangesOpt);
  ArrayRef<std::pair<size_t, size_t>> invArgRanges = result.invArgRanges;
  const size_t numArgs = invArgRanges.size();

  // --- DAG traversal lookup structures ----------------------------------
  //
  // We lift edits along caller/callee edges between MacroInvocation items.
  // Build a fast lookup map from invocation id -> invocation* so we can
  // climb parent pointers without repeated O(N) scans.
  result.invById.reserve(deps_.model.GetMacroInvocations().size());
  for (const auto &mi : deps_.model.GetMacroInvocations())
    result.invById[mi.id] = &mi;
  auto &invById = result.invById;

  const MacroSubtreeReplayValidationContext subtreeValidationCtx{
      m, invSpanText, invArgRanges, invById};

  // Result-bound aliases so the body writes through the original local
  // names without a final copy step.
  result.directRootPreservationInadmissible = false;
  bool &directRootPreservationInadmissible =
      result.directRootPreservationInadmissible;
  auto &leafCands = result.leafCands;
  auto &splitInsertionRootCandidates = result.splitInsertionRootCandidates;

  // --- Sibling pure-insertion split-insertion partner scan --------------
  //
  // When the current hunk is a pure insertion, look for a sibling pure
  // insertion inside the same macro cover whose combined envelope falls
  // inside an arg-like surface.  Each such pair becomes a candidate root
  // patch carried into the lifting phase via
  // `splitInsertionRootCandidates`.
  if (hEff.aStart == hEff.aEnd && !deps_.abTokHunks.empty()) {
    SmallVector<diffutils::Hunk, 8> partnerInsertions;
    for (const auto &hh : deps_.abTokHunks) {
      if (hh.aStart != hh.aEnd)
        continue;
      if (hh.aStart < m.cover.begin || hh.aEnd > m.cover.end)
        continue;
      if (hh.aStart == hEff.aStart && hh.bStart == hEff.bStart &&
          hh.bEnd == hEff.bEnd)
        continue;
      partnerInsertions.push_back(hh);
    }

    for (const auto &partner : partnerInsertions) {
      // Combine the current insertion with its partner to see whether the
      // pair exposes an argument-local edit envelope.  The trimmed
      // envelope is the proof candidate passed to the args-only builder.
      const diffutils::Hunk env = buildCombinedInsertionEnvelope(hEff, partner);
      const diffutils::Hunk envTrim =
          trimCommonEdgeTokens(env, deps_.aToks, deps_.bToks);

      SmallVector<char, 16> envTrimRootTouched(argLikeSpans.size(), 0);
      const bool envTrimRootWithinArgLike =
          !argLikeSpans.empty() &&
          deps_.sourceMapper.HunkFullyWithinArgSpans(envTrim, argLikeSpans,
                                                     envTrimRootTouched);

      std::optional<MacroPatch> pairRootPatch;
      if (!argLikeSpans.empty() && envTrimRootWithinArgLike &&
          RefoldLineObserverLayout::InvocationSpanMatchesCallsitePrefix(
              invSpanText, m)) {
        // Once the paired insertion envelope trims down to a valid
        // argument-local hunk, delegate to the standard args-only builder
        // for the actual rewrite and validation.
        pairRootPatch =
            deps_.buildMacroInvocationPatchArgsOnly(m, envTrim, baseInvText);
      }

      if (pairRootPatch) {
        DAGSplitInsertionRootCandidate candidate;
        candidate.patch = std::move(*pairRootPatch);

        // Collect the root formal argument indices touched by the trimmed
        // combined insertion envelope.  Store formal arg indices, not raw
        // arg-like span indices, because a single formal may appear
        // multiple times at the root callsite.
        for (size_t idx = 0; idx < envTrimRootTouched.size(); ++idx) {
          if (!envTrimRootTouched[idx] || idx >= argLikeSpans.size())
            continue;
          const uint32_t argIdx = argLikeSpans[idx].argIdx;
          if (argIdx >= numArgs)
            continue;
          if (!llvm::is_contained(candidate.deferOccurrenceArgIdxs, argIdx))
            candidate.deferOccurrenceArgIdxs.push_back(argIdx);
        }

        // Keep the deferred formal set stable and deterministic so later
        // validation and tracing do not depend on discovery order.
        llvm::sort(candidate.deferOccurrenceArgIdxs);
        splitInsertionRootCandidates.push_back(std::move(candidate));
      }
    }
  }

  // --- Leaf candidates touched by this hunk ------------------------------
  //
  // We search for descendant invocations whose argument-like spans are
  // fully covered by the hunk and exhibit an A->B text difference.
  //
  // We prefer deeper leaves (closest to the actual edited text), because
  // lifting from a deeper leaf tends to be more local and less ambiguous.
  for (const auto &cand : deps_.model.GetMacroInvocations()) {
    // Only consider invocations that are strict descendants of the
    // validated root by producer-recorded caller ancestry.  Keep the
    // computed depth because leaf ordering uses the original distance-
    // to-root tie-breaker.
    std::optional<unsigned> candidateDepth =
        occurrenceProofValidator.CandidateDepthInValidatedSubtree(
            subtreeValidationCtx, cand);
    if (!candidateDepth)
      continue;
    const bool hunkWithinCandCover = cand.cover.begin <= h.aStart &&
                                     h.aEnd <= cand.cover.end &&
                                     cand.cover.begin < cand.cover.end;

    SmallVector<RefoldModel::PPArgSpan, 8> candArgLikeRaw;
    gatherArgLikeSpans(cand, candArgLikeRaw);
    SmallVector<RefoldModel::PPArgSpan, 8> candArgLike = candArgLikeRaw;
    sanitizeArgLikeSpans(deps_.aToks, candArgLike);

    if (!deps_.subtreeReplayValidator.SubtreePathHasProvableCalleeClosure(
            subtreeValidationCtx, cand)) {
      // Unsupported descendant structure only blocks direct root replay
      // when the edit cannot already be represented by one of the root's
      // own argument-like spans.  If the root has a direct args-only
      // proof surface, keep that candidate alive and let DAG lifting
      // compete normally instead of forcing whole-cover expansion.
      if (hunkWithinCandCover && !rootHasDirectArgLikeSurface)
        directRootPreservationInadmissible = true;
      continue;
    }

    // Candidate must have argument-like spans; otherwise there's nothing
    // concrete to map an edit to.
    if (candArgLike.empty())
      continue;

    // Determine how many formals this invocation "effectively" has,
    // because spans might reference argIdx beyond invArgRanges size.
    unsigned candFormalCount = (unsigned)cand.invArgRanges.size();
    for (const auto &sp : candArgLike)
      candFormalCount = std::max(candFormalCount, (unsigned)sp.argIdx + 1);

    // Mark which concrete arg-like span occurrences are touched by the
    // hunk, then compress that to a per-formal touched set.  The helper
    // expects one flag per span occurrence, while the later DAG logic
    // reasons per formal arg index.
    SmallVector<char, 8> candTouchedBySpan(candArgLike.size(), 0);
    if (!deps_.sourceMapper.HunkFullyWithinArgSpans(h, candArgLike,
                                                    candTouchedBySpan)) {
      continue;
    }

    SmallVector<char, 8> candTouched(candFormalCount, 0);
    for (size_t si = 0; si < candArgLike.size(); ++si) {
      if (!candTouchedBySpan[si])
        continue;
      const auto &sp = candArgLike[si];
      if (sp.argIdx < candTouched.size())
        candTouched[sp.argIdx] = 1;
    }

    bool anyTouched = false;
    for (char t : candTouched)
      if (t) {
        anyTouched = true;
        break;
      }
    if (!anyTouched) {
      continue;
    }

    // Now verify there's an actual A->B difference within at least one
    // touched arg-like span (otherwise lifting would be a no-op).
    bool anyDiff = false;
    uint64_t bestSpan = ~uint64_t(0);
    for (const auto &sp : candArgLike) {
      if (sp.argIdx >= candTouched.size() || !candTouched[sp.argIdx])
        continue;
      bestSpan = std::min(bestSpan, ppArgSpanProofWidth(sp));

      auto aTxt = extractDAGSpanText(deps_.sourceMapper, sp, /*fromB=*/false);
      auto bTxt = extractDAGSpanText(deps_.sourceMapper, sp, /*fromB=*/true);
      if (!aTxt || !bTxt)
        continue;

      if (bTxt->reliable) {
        // Compare normalized old/new argument text.  For variadic formals,
        // allow top-level commas when unstringifying.
        auto aLift =
            normalizeDAGLiftText(deps_.argTextRecovery, &cand, sp, aTxt->text,
                                 /*allowTopLevelComma=*/true);
        bool allowComma = sp.argIdx < cand.defParams.size() &&
                          cand.defParams[sp.argIdx].variadic;
        auto bLift =
            normalizeDAGLiftText(deps_.argTextRecovery, &cand, sp, bTxt->text,
                                 /*allowTopLevelComma=*/allowComma);
        if (aLift && bLift && *aLift != *bLift)
          anyDiff = true;
      } else if (sp.kind == PPArgSpanKind::Paste && sp.byteBegin &&
                 sp.byteEnd) {
        // Paste-subrange extraction may be unreliable after edits (token
        // has changed).  We still treat this as a potential edit; later
        // we only accept it if token-level splitting is uniquely
        // determined.
        anyDiff = true;
      }
    }

    if (!anyDiff) {
      continue;
    }

    // Candidate leaf accepted: store its arg-like spans and which formals
    // are touched, plus depth and a locality tie-breaker.
    leafCands.push_back(DAGLeafCandidate{&cand, *candidateDepth, bestSpan,
                                         std::move(candArgLike),
                                         std::move(candTouched)});
  }

  // Order leaves from most promising to least:
  //   (1) deepest first (closest to the actual edit)
  //   (2) smaller span first (more local pp coverage)
  llvm::sort(leafCands,
             [](const DAGLeafCandidate &a, const DAGLeafCandidate &b) {
               if (a.depth != b.depth)
                 return a.depth > b.depth; // deepest first
               return a.smallestSpanBytes < b.smallestSpanBytes;
             });

  // Cache the root invocation's *current* argument texts (trimmed).
  // These are used for final-hop two-parent splitting and for validation.
  for (uint32_t i = 0; i < numArgs; ++i) {
    result.rootArgText[i] =
        invSpanText.slice(invArgRanges[i].first, invArgRanges[i].second).trim();
  }
}

} // namespace refold
} // namespace clang
