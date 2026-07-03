//===--- RefoldBInsertionLedger.cpp ----------------------------*- C++ -*-===//
//
// Implements B-token pure-insertion provenance and claim clipping.
//
//===----------------------------------------------------------------------===//

#include "edit/RefoldBInsertionLedger.h"
#include "core/RefoldLog.h"
#include "core/RefoldOwnerClassifier.h"
#include "macro/RefoldMacroReplay.h"
#include "macro/RefoldMacroTopology.h"
#include "source/RefoldSourceMapper.h"

#include "llvm/Support/FormatVariadic.h"

#include <algorithm>

using namespace llvm;
using namespace clang::refold;

RefoldBInsertionLedger::RefoldBInsertionLedger(Deps deps) : deps_(deps) {}

void RefoldBInsertionLedger::BuildProvenance(ArrayRef<diffutils::Hunk> hunks) {
  // Build a structural provenance map for token-level pure insertions.
  //
  // A pure insertion hunk is one where no A tokens are deleted and the edit
  // consists entirely of B tokens:
  //   h.aStart == h.aEnd && h.bStart < h.bEnd
  //
  // The ledger records a compact insertion table, a per-B-token reverse index,
  // and a per-hunk insertion index.  Those indices let later proof paths
  // enforce the global "emit each B-only segment exactly once" invariant
  // without borrowing RefoldEngine state.
  bInsertions_.clear();
  bTokToInsertionId_.assign(deps_.bTokens.size(), -1);
  hunkToInsertionId_.assign(hunks.size(), -1);

  for (size_t hi = 0; hi < hunks.size(); ++hi) {
    const auto &h = hunks[hi];
    if (!h.isInsertOnly())
      continue;

    const size_t b0 = static_cast<size_t>(h.bStart);
    const size_t b1 = static_cast<size_t>(h.bEnd);
    if (b1 > deps_.bTokens.size()) {
      REFOLD_LOG_FATAL(
          "prov/ins",
          "insertion hunk out of B bounds: hunk#{0} b=[{1},{2}) bToks={3}", hi,
          b0, b1, deps_.bTokens.size());
    }

    const size_t insId = bInsertions_.size();
    BInsertionProv ins;
    ins.aGap = h.aStart;
    ins.hunkIndex = hi;
    ins.b0 = b0;
    ins.b1 = b1;
    bInsertions_.push_back(ins);
    hunkToInsertionId_[hi] = static_cast<int32_t>(insId);

    for (size_t bj = b0; bj < b1; ++bj) {
      if (bTokToInsertionId_[bj] != -1) {
        REFOLD_LOG_FATAL(
            "prov/ins",
            "overlapping insertion hunks at B tok {0}: existingIns={1} "
            "newIns={2}",
            bj, bTokToInsertionId_[bj], static_cast<int32_t>(insId));
      }
      bTokToInsertionId_[bj] = static_cast<int32_t>(insId);
    }
  }
}

void RefoldBInsertionLedger::Claim(size_t insId, BInsertionClaim claim,
                                   StringRef why) {
  // First claimant fixes the emission owner.  Re-claiming the same insertion
  // with a different claim kind is a hard error because it would permit
  // duplicate emission of the same B-only segment.
  if (insId >= bInsertions_.size())
    return;
  BInsertionProv &ins = bInsertions_[insId];
  if (ins.claim == BInsertionClaim::Unclaimed) {
    ins.claim = claim;
    return;
  }
  if (ins.claim != claim) {
    REFOLD_LOG_FATAL(
        "prov/claim",
        "double-claim insertion ins#{0} hunk#{1} AGap={2} B=[{3},{4}) "
        "existing={5} new={6} why={7}",
        insId, ins.hunkIndex, ins.aGap, ins.b0, ins.b1,
        static_cast<unsigned>(ins.claim), static_cast<unsigned>(claim), why);
  }
}

void RefoldBInsertionLedger::PreclaimStandaloneInsertions(
    StringRef tuPath, ArrayRef<diffutils::Hunk> hunks) {
  if (bInsertions_.empty())
    return;

  for (size_t hi = 0; hi < hunks.size(); ++hi) {
    int32_t insIdI32 =
        (hi < hunkToInsertionId_.size()) ? hunkToInsertionId_[hi] : -1;
    if (insIdI32 < 0)
      continue;

    const auto &h = hunks[hi];

    // Macro call-sites have priority; if this insertion lies within a patchable
    // macro's cover, leave it unclaimed so the macro patch may absorb it.
    Owner owner = deps_.ownerClassifier.ClassifyOwnerWithSegments(tuPath, h);
    if (auto *m = deps_.macroTopology.SmallestCoveringPatchableMacro(
            h.aStart, h.aEnd, owner.includeId)) {
      if (m->invB && m->invE)
        continue;
    }

    // Right-boundary insertions normally stay standalone.  Leave this insertion
    // unclaimed only when a later macro proof may absorb it as an inactive
    // `__VA_OPT__` tail or generated selector boundary.  This is deliberately
    // narrower than treating every `cover.end` insertion as macro-owned.
    if (h.isInsertOnly() &&
        (deps_.macroBoundarySelector.RightBoundaryVaOptActivationMacro(
             h.aStart, owner.includeId) ||
         deps_.macroBoundarySelector.BoundaryGeneratedSelectorMacro(
             h.aStart, owner.includeId)))
      continue;

    Claim(static_cast<size_t>(insIdI32), BInsertionClaim::Standalone,
          llvm::formatv("preclaim hunk#{0}", hi).str());
  }
}

SmallVector<std::pair<size_t, size_t>, 4>
RefoldBInsertionLedger::ClipBTokenRangeAgainstClaims(size_t bTokStart,
                                                     size_t bTokEnd) const {
  // Return [bTokStart,bTokEnd) with any Standalone-claimed pure-insertion
  // segments removed.  The kept subranges preserve B-token order.
  SmallVector<std::pair<size_t, size_t>, 4> segs;
  if (bTokEnd <= bTokStart)
    return segs;

  const size_t bMax = deps_.bTokens.size();
  bTokStart = std::min(bTokStart, bMax);
  bTokEnd = std::min(bTokEnd, bMax);

  size_t i = bTokStart;
  while (i < bTokEnd) {
    int32_t insIdI32 =
        (i < bTokToInsertionId_.size()) ? bTokToInsertionId_[i] : -1;
    if (insIdI32 >= 0) {
      const BInsertionProv &ins = bInsertions_[static_cast<size_t>(insIdI32)];
      if (ins.claim == BInsertionClaim::Standalone) {
        i = std::min(bTokEnd, ins.b1);
        continue;
      }
    }

    const size_t segStart = i;
    ++i;
    while (i < bTokEnd) {
      int32_t nextId =
          (i < bTokToInsertionId_.size()) ? bTokToInsertionId_[i] : -1;
      if (nextId >= 0) {
        const BInsertionProv &ins = bInsertions_[static_cast<size_t>(nextId)];
        if (ins.claim == BInsertionClaim::Standalone)
          break;
      }
      ++i;
    }
    if (segStart < i)
      segs.push_back({segStart, i});
  }

  return segs;
}

std::string
RefoldBInsertionLedger::SliceBSourceClippedAgainstClaims(size_t bTokStart,
                                                         size_t bTokEnd) const {
  SmallVector<std::pair<size_t, size_t>, 4> segs =
      ClipBTokenRangeAgainstClaims(bTokStart, bTokEnd);
  if (segs.empty())
    return std::string();

  std::string out;
  for (const auto &s : segs) {
    StringRef frag = deps_.sourceMapper.SliceBSource(s.first, s.second);
    out.append(frag.begin(), frag.end());
  }
  return out;
}
