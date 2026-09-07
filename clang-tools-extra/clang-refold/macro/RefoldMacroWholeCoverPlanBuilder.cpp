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
#include "source/RefoldSourceMapper.h"

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

  return plan;
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

} // namespace refold
} // namespace clang
