//===--- RefoldMacroArgsOnlyWholeCoverPhase.cpp -----------------*- C++ -*-===//
//
// Implementation of the direct root args-only whole-cover phase.  Inputs
// come from `RefoldMacroWholeCoverPlanningContext`, results are written back
// to the planning context's mutable candidate fields, and calls into shared
// macro patch construction go through explicit dependency callbacks so this
// phase has no planner back-reference.
//
//===----------------------------------------------------------------------===//

#include "macro/RefoldMacroArgsOnlyWholeCoverPhase.h"

#include "core/RefoldModel.h"
#include "edit/RefoldPatchTypes.h"
#include "line-control/RefoldLineObserverLayout.h"
#include "macro/RefoldMacroPlannerHelpers.h"
#include "macro/RefoldMacroWholeCoverPlanningContext.h"
#include "proof/RefoldProofLattice.h"
#include "source/RefoldSourceMapper.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallVector.h"

#include <utility>
#include <vector>

using namespace llvm;

namespace clang {
namespace refold {

RefoldMacroArgsOnlyWholeCoverPhase::RefoldMacroArgsOnlyWholeCoverPhase(
    Dependencies deps)
    : deps_(std::move(deps)) {}

void RefoldMacroArgsOnlyWholeCoverPhase::Run(
    RefoldMacroWholeCoverPlanningContext &planningCtx) const {
  const RefoldModel::MacroInvocation &m = planningCtx.m;
  const diffutils::Hunk &hEff = planningCtx.hEff;
  ArrayRef<RefoldModel::PPArgSpan> argLikeSpans = planningCtx.argLikeSpans;
  StringRef baseInvText = planningCtx.baseInvText;
  const MacroPatchReuseAdmissionContext &reuseAdmission =
      planningCtx.reuseAdmissionCtx;

  SmallVector<char, 16> argTouched(argLikeSpans.size(), 0);
  StringRef invSpanText =
      !baseInvText.empty()
          ? baseInvText
          : (m.invText ? StringRef(*m.invText) : StringRef(""));
  planningCtx.rootHasDirectArgLikeSurface =
      !argLikeSpans.empty() &&
      deps_.sourceMapper.HunkFullyWithinArgSpans(hEff, argLikeSpans,
                                                 argTouched) &&
      RefoldLineObserverLayout::InvocationSpanMatchesCallsitePrefix(invSpanText,
                                                                    m);

  if (planningCtx.rootHasDirectArgLikeSurface) {
    // First try to patch arguments in-place. If that cannot satisfy the
    // edit, DAG lifting may still preserve deeper nested structure, so
    // keep the candidate instead of returning immediately.
    planningCtx.argsOnlyCandidate =
        deps_.buildMacroInvocationPatchArgsOnly(m, hEff, baseInvText);
    if (!planningCtx.argsOnlyCandidate) {
      // If an existing callsite patch already satisfies this trimmed hunk,
      // defer reuse until DAG chaining has had a chance to compete.
      planningCtx.reuseExistingCallsitePatch =
          reuseAdmission.existingPatch && reuseAdmission.existingIsCallsite &&
          !baseInvText.empty() &&
          reuseAdmission.existingPatch->proof.preservesInvocationStructure &&
          reuseAdmission.existingPatch->proof.proofRootMacroId == m.id;
    }
    return;
  }

  if (RefoldLineObserverLayout::InvocationSpanMatchesCallsitePrefix(invSpanText,
                                                                    m)) {
    // The definition-replay proof inside the args-only builder can handle
    // edits whose token hunk spans both argument substitutions and macro-
    // body tokens, most importantly __VA_OPT__ erasure/exposure.
    planningCtx.argsOnlyCandidate =
        deps_.buildMacroInvocationPatchArgsOnly(m, hEff, baseInvText);
    if (!planningCtx.argsOnlyCandidate && !argLikeSpans.empty())
      planningCtx.argsOnlyCandidate =
          TryPairedPureInsertionRootArgsOnly(planningCtx);
  }
}

std::optional<MacroPatch>
RefoldMacroArgsOnlyWholeCoverPhase::TryPairedPureInsertionRootArgsOnly(
    const RefoldMacroWholeCoverPlanningContext &planningCtx) const {
  const RefoldModel::MacroInvocation &m = planningCtx.m;
  const diffutils::Hunk &hEff = planningCtx.hEff;
  ArrayRef<RefoldModel::PPArgSpan> argLikeSpans = planningCtx.argLikeSpans;
  StringRef baseInvText = planningCtx.baseInvText;

  // This path is only for pure insertions in B. Non-insertion edits already
  // carry an A-side range and should use the ordinary args-only path.
  if (hEff.aStart != hEff.aEnd || hEff.bStart >= hEff.bEnd)
    return std::nullopt;
  if (argLikeSpans.empty())
    return std::nullopt;

  // Paste spans need paste-specific replay/invertibility logic. Do not try
  // to explain paste edits by pairing generic insertion frontiers.
  if (!m.pasteSpans.empty())
    return std::nullopt;

  // If the current hunk is already fully contained in an argument span,
  // then it is not the split-frontier case this recovery path is meant for.
  SmallVector<char, 16> curTouched(argLikeSpans.size(), 0);
  if (deps_.sourceMapper.HunkFullyWithinArgSpans(hEff, argLikeSpans,
                                                 curTouched))
    return std::nullopt;

  // Require a real callsite-shaped invocation spelling.  This recovery
  // produces a structure-preserving invocation rewrite, not an already-
  // expanded payload.
  StringRef invSpanText =
      !baseInvText.empty()
          ? baseInvText
          : (m.invText ? StringRef(*m.invText) : StringRef(""));
  if (!RefoldLineObserverLayout::InvocationSpanMatchesCallsitePrefix(
          invSpanText, m))
    return std::nullopt;

  // Candidate ownership is tested against ordinary and stringify argument
  // occurrences.  Paste occurrences were rejected above.
  std::vector<RefoldModel::PPArgSpan> occs;
  append_range(occs, m.argSpans);
  append_range(occs, m.stringifySpans);
  if (occs.empty())
    return std::nullopt;

  for (const auto &partner : deps_.abTokHunks) {
    // Pair only with another pure B insertion.  Replacement/deletion hunks
    // are outside this split-insertion recovery proof.
    if (partner.aStart != partner.aEnd || partner.bStart >= partner.bEnd)
      continue;

    // Do not pair the hunk with itself.
    if (partner.aStart == hEff.aStart && partner.bStart == hEff.bStart &&
        partner.bEnd == hEff.bEnd)
      continue;

    // Both insertion frontiers must live inside this macro invocation cover.
    if (!(m.cover.begin <= partner.aStart && partner.aEnd <= m.cover.end))
      continue;

    auto isCanonicalLeader = [&](const diffutils::Hunk &lhs,
                                 const diffutils::Hunk &rhs) {
      // Each pair is considered once.  The lower A frontier leads; B
      // coordinates provide deterministic tie-breakers for same-gap
      // insertions.
      if (lhs.aStart != rhs.aStart)
        return lhs.aStart < rhs.aStart;
      if (lhs.bStart != rhs.bStart)
        return lhs.bStart < rhs.bStart;
      return lhs.bEnd < rhs.bEnd;
    };
    if (!isCanonicalLeader(hEff, partner))
      continue;

    const diffutils::Hunk env = buildCombinedInsertionEnvelope(hEff, partner);

    // Remove unchanged matching edge tokens so the synthetic envelope
    // exposes only the edited core between the paired insertion frontiers.
    const diffutils::Hunk envTrim =
        trimCommonEdgeTokens(env, deps_.aToks, deps_.bToks);

    // The trimmed synthetic envelope must be fully explainable by argument
    // occurrences.  Otherwise the paired insertions are not an args-only
    // edit.
    std::vector<char> touchedOcc(occs.size(), 0);
    if (!deps_.sourceMapper.HunkFullyWithinArgSpans(envTrim, occs, touchedOcc))
      continue;

    // Require the envelope to touch exactly one formal argument.  If it
    // spans multiple formals, there is no single invocation argument
    // replacement to delegate to the standard args-only builder.
    SmallVector<uint32_t, 4> touchedArgs;
    for (size_t i = 0; i < occs.size(); ++i) {
      if (!touchedOcc[i])
        continue;
      if (!llvm::is_contained(touchedArgs, occs[i].argIdx))
        touchedArgs.push_back(occs[i].argIdx);
    }
    if (touchedArgs.size() != 1)
      continue;

    // Once the paired insertions have been converted into a proof-
    // compatible argument envelope, reuse the ordinary args-only builder
    // and validation.
    auto patch =
        deps_.buildMacroInvocationPatchArgsOnly(m, envTrim, baseInvText);
    if (!patch)
      continue;

    deps_.proofLattice.SetMacroPatchProof(
        *patch, deps_.proofLattice.MakeMacroPatchProof(
                    MacroPatchProofKind::ArgsOnlyPairedPureInsertion,
                    /*preservesInvocationStructure=*/true, m.id));
    return patch;
  }

  return std::nullopt;
}

} // namespace refold
} // namespace clang
