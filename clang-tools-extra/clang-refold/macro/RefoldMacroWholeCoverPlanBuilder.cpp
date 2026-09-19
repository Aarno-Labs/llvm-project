//===--- RefoldMacroWholeCoverPlanBuilder.cpp ------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM
// Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "macro/RefoldMacroWholeCoverPlanBuilder.h"

#include "edit/RefoldBInsertionLedger.h"
#include "macro/RefoldMacroReplay.h"
#include "macro/RefoldMacroTopology.h"
#include "proof/RefoldSidebandReplayProof.h"
#include "source/RefoldSourceMapper.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"

#include <cstddef>
#include <string>
#include <utility>

using namespace llvm;

namespace clang {
namespace refold {

std::optional<WholeCoverPlan>
RefoldMacroWholeCoverPlanBuilder::ComputeWholeCoverPlan(
    const RefoldModel::MacroInvocation &m) const {
  auto range = RefoldMacroWholeCoverProof::GetWholeCoverATokRange(m);
  if (!range)
    return std::nullopt;

  WholeCoverPlan plan;
  plan.covLoA = range->first;
  plan.covHiA = range->second;

  // Function-like macros with no formal parameters can have a useful body-span
  // range that is narrower than the producer's invocation cover. Record that
  // distinction so diagnostics can explain why the whole-cover domain came from
  // body material rather than the raw cover.
  plan.usedBodyRange =
      (m.subkind == "func" && m.defParams.empty() && !m.bodySpans.empty() &&
       (plan.covLoA != m.cover.begin || plan.covHiA != m.cover.end));

  // Whole-cover replay is admissible only when the selected A range is
  // explained by this invocation's own detailed macro provenance. Descendant
  // spans may justify a structure-preserving DAG lift, but coarse containment
  // alone does not prove that this callsite can be replaced by realized
  // whole-cover output.
  plan.selfContained =
      RefoldMacroWholeCoverProof::MacroWholeCoverIsSelfContained(m);
  if (!plan.selfContained) {
    return std::nullopt;
  }

  // Map the accepted A-token cover into B while preserving boundary insertions.
  // This gives the raw B envelope that the whole-cover candidate will replay.
  // Use the token-level projection here: whole-cover replaces the invocation's
  // entire expansion envelope, and the byte-level mapper can drift the envelope
  // start into an unrelated statement insertion that happens to precede a
  // re-materialized value token (repeated `int `/` = ` byte runs), causing the
  // realized text to swallow the inserted tokens.
  auto bEnv = deps_.sourceMapper.MapATokRangeToBTokenEnvelopeByTokenDiff(
      plan.covLoA, plan.covHiA);
  if (!bEnv)
    return std::nullopt;

  plan.rawBTokStart = bEnv->first;
  plan.rawBTokEnd = bEnv->second;
  plan.bTokStart = plan.rawBTokStart;
  plan.bTokEnd = plan.rawBTokEnd;
  if (plan.bTokEnd <= plan.bTokStart)
    return std::nullopt;

  // The raw envelope from MapATokRangeToBTokenEnvelopeByTokenDiff is the
  // certified whole-cover image: the token-level diff is atomic and already
  // anchors a genuine boundary insertion exactly at the cover's start/end
  // token, and downstream claim-clipping removes any B-only payload that a
  // stronger/narrower candidate owns.  We deliberately do NOT nudge the seam by
  // spelling equality with a neighboring B token: a repeated spelling is not
  // provenance (a whole-cover expansion such as `"[" #x "]"` legitimately
  // repeats the argument spelling), so a spelling-based ±1 shift can split the
  // envelope at an uncertified token match and drop a real cover token.  The
  // seam therefore stays exactly where the certified projection placed it.
  plan.adjustedLeft = false;
  plan.adjustedRight = false;

  // Claim-clip the whole-cover B envelope: any boundary insertion already
  // claimed by a stronger/narrower candidate is dropped so it is not emitted
  // twice.  The surviving B tokens must form exactly one contiguous run; fail
  // closed (offer no whole-cover candidate) otherwise, because:
  //   * more than one kept segment means a claimed edit was removed from the
  //     *interior* of the envelope, so the emitted source would splice the two
  //     surviving sides directly together -- an unproven join that can paste
  //     adjacent tokens and represents a foreign edit conflicting inside this
  //     macro's realized expansion; and
  //   * zero kept segments means the whole envelope was claimed away, leaving
  //     nothing for this cover to materialize.
  // Whole-cover has no proof that either shape reproduces the cover's B tokens,
  // so a weaker sound path (or the conservative fallback) must handle the
  // region.  This is a self-contained soundness gate: it does not rely on the
  // downstream global certifier to reject an unsound splice after the fact.
  llvm::SmallVector<std::pair<size_t, size_t>, 4> keptSegments =
      deps_.bInsertionLedger.ClipBTokenRangeAgainstClaims(plan.bTokStart,
                                                          plan.bTokEnd);
  if (keptSegments.size() != 1)
    return std::nullopt;

  // The single kept segment is the sound whole-cover B image.  It equals the
  // raw envelope exactly when nothing was claimed away; a narrower segment
  // means this candidate yielded a boundary insertion to an existing claim.
  const std::pair<size_t, size_t> &keptSegment = keptSegments.front();
  plan.claimsClipped = (keptSegment.first != plan.bTokStart ||
                        keptSegment.second != plan.bTokEnd);

  // Materialize the kept segment from the bytes its B tokens occupy, not from
  // the first byte of the token that follows it.  The trailing trivia is not
  // part of what this cover realizes, and after sideband pragma normalization
  // it can hold a whole directive line: the sideband path removed those lines
  // from the token stream and pairs each one with the source directive that
  // produced it, so carrying the bytes here would emit the directive a second
  // time while that source directive still stands.  The realized text is
  // trimmed either way, so this changes nothing for a range whose trailing
  // trivia is whitespace.
  std::optional<StringRef> material = deps_.sourceMapper.SliceBTokenMaterial(
      keptSegment.first, keptSegment.second);
  if (!material)
    return std::nullopt;
  plan.clippedText = material->trim().str();
  ReplayProducedPragmaLines(m, keptSegment, material->trim(), plan);

  return plan;
}

void RefoldMacroWholeCoverPlanBuilder::ReplayProducedPragmaLines(
    const RefoldModel::MacroInvocation &m,
    std::pair<size_t, size_t> keptSegment, StringRef material,
    WholeCoverPlan &plan) const {
  const uint64_t materialBegin =
      static_cast<uint64_t>(material.data() - deps_.bSource.data());
  const uint64_t materialEnd = materialBegin + material.size();

  SmallVector<const SidebandPragmaLinePairing *, 2> leading, interior, trailing;
  for (const SidebandPragmaLinePairing &line :
       deps_.sidebandPragmaLinePairings) {
    if (!line.bNormalTokenGap)
      continue;
    const std::optional<ArrayRef<uint64_t>> ancestors =
        deps_.topology.PragmaExpansionAncestorsAtGap(line.aNormalTokenGap);
    if (!ancestors || !llvm::is_contained(*ancestors, m.id))
      continue;
    const uint64_t bGap = *line.bNormalTokenGap;
    if (bGap == keptSegment.first)
      leading.push_back(&line);
    else if (bGap == keptSegment.second)
      trailing.push_back(&line);
    else if (keptSegment.first < bGap && bGap < keptSegment.second &&
             materialBegin <= line.bLineBegin && line.bLineEnd <= materialEnd)
      interior.push_back(&line);
    else
      return; // B prints it outside this cover; the plan cannot replay it.
  }

  // The lines at one edge, in B order, with only whitespace between them and
  // the kept material; std::nullopt when anything else lies there.
  auto edgeBytes =
      [&](SmallVectorImpl<const SidebandPragmaLinePairing *> &lines,
          bool before) -> std::optional<StringRef> {
    if (lines.empty())
      return StringRef();
    llvm::sort(lines, [](const auto *lhs, const auto *rhs) {
      return lhs->bLineBegin < rhs->bLineBegin;
    });
    uint64_t cursor = before ? lines.front()->bLineBegin : materialEnd;
    const uint64_t stop = before ? materialBegin : lines.back()->bLineEnd;
    for (const SidebandPragmaLinePairing *line : lines) {
      if (line->bLineBegin < cursor ||
          !deps_.bSource.slice(cursor, line->bLineBegin).trim().empty())
        return std::nullopt;
      cursor = line->bLineEnd;
    }
    if (stop < cursor || !deps_.bSource.slice(cursor, stop).trim().empty())
      return std::nullopt;
    return deps_.bSource.slice(before ? lines.front()->bLineBegin : materialEnd,
                               stop);
  };
  std::optional<StringRef> prefix = edgeBytes(leading, /*before=*/true);
  std::optional<StringRef> suffix = edgeBytes(trailing, /*before=*/false);
  if (!prefix || !suffix)
    return;

  // A leading directive needs its own line: the callsite may sit mid-line.
  // A trailing one already starts on a fresh line and ends with its newline.
  if (!prefix->empty())
    plan.clippedText = "\n" + prefix->str() + plan.clippedText;
  plan.clippedText += suffix->str();
  for (const auto *lines : {&leading, &interior, &trailing})
    for (const SidebandPragmaLinePairing *line : *lines)
      plan.replayedPragmaLines.push_back({line->bLineBegin, line->bLineEnd});
}

bool RefoldMacroWholeCoverPlanBuilder::WholeCoverPatchMatchesPlan(
    const MacroPatch &patch, const WholeCoverPlan &plan, uint64_t rootMacroId) {
  if (patch.proof.kind != MacroPatchProofKind::WholeCoverRealization ||
      patch.proof.preservesInvocationStructure ||
      patch.proof.proofRootMacroId != rootMacroId)
    return false;
  return patch.wholeCover.usedBodyRange == plan.usedBodyRange &&
         patch.wholeCover.selfContained == plan.selfContained &&
         patch.wholeCover.adjustedLeft == plan.adjustedLeft &&
         patch.wholeCover.adjustedRight == plan.adjustedRight &&
         patch.wholeCover.claimsClipped == plan.claimsClipped &&
         patch.wholeCover.aLo == plan.covLoA &&
         patch.wholeCover.aHi == plan.covHiA &&
         patch.wholeCover.bRawLo == plan.rawBTokStart &&
         patch.wholeCover.bRawHi == plan.rawBTokEnd &&
         patch.wholeCover.bAdjLo == plan.bTokStart &&
         patch.wholeCover.bAdjHi == plan.bTokEnd;
}

std::optional<std::string>
RefoldMacroWholeCoverPlanBuilder::BuildWholeCoverReplacementText(
    const RefoldModel::MacroInvocation &m) const {
  // Reuse the same whole-cover planning path used by patch construction so the
  // returned replacement text obeys the same clipping/envelope policy.
  auto plan = ComputeWholeCoverPlan(m);
  if (!plan)
    return std::nullopt;
  return plan->clippedText;
}

} // namespace refold
} // namespace clang
