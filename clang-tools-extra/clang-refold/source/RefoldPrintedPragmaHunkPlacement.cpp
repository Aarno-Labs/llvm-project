//===--- RefoldPrintedPragmaHunkPlacement.cpp ------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Final placement check and repair of printed `#pragma` lines against the
// structurally tiled token hunks.
//
//===----------------------------------------------------------------------===//

#include "source/RefoldPrintedPragmaHunkPlacement.h"

#include "macro/RefoldMacroTopology.h"
#include "model/RefoldModel.h"
#include "proof/RefoldSidebandReplayProof.h"
#include "support/RefoldLog.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallVector.h"

#include <algorithm>
#include <map>
#include <utility>

using namespace llvm;

namespace clang {
namespace refold {

namespace {

/// Return whether some hunk consumes A tokens of a root invocation among
/// \p ancestors, which hands that invocation to a macro realizer.
bool rootInvocationTouchedByHunk(const RefoldMacroTopology &topology,
                                 ArrayRef<uint64_t> ancestors,
                                 ArrayRef<diffutils::Hunk> hunks) {
  for (uint64_t id : ancestors) {
    const RefoldModel::MacroInvocation *root =
        topology.FindMacroInvocationById(id);
    if (!root || root->callerMacroId)
      continue;
    for (const RefoldModel::PPSpan &span : root->spans)
      for (const diffutils::Hunk &h : hunks)
        if (h.aStart < span.end && span.begin < h.aEnd)
          return true;
  }
  return false;
}

/// Return the include occurrence owning the `#pragma` directive printed as
/// \p line, when that directive is spelled in a header; std::nullopt for a
/// translation-unit line, an operator, or a record that does not bind.
std::optional<uint64_t>
headerDirectiveOwnerOfPrintedLine(const RefoldModel &model,
                                  const SidebandPragmaLinePairing &line) {
  const RefoldModel::PragmaDirective *found = nullptr;
  for (const RefoldModel::PragmaDirective &pragma : model.GetPragmas()) {
    if (!pragma.HasEmittedImage() || *pragma.ppByteBegin < line.aLineBegin ||
        line.aLineEnd < *pragma.ppByteEnd)
      continue;
    if (found)
      return std::nullopt;
    found = &pragma;
  }
  if (!found || found->viaPragmaOperator)
    return std::nullopt;
  return found->ownerIncludeId;
}

} // namespace

std::optional<PrintedPragmaPlacementViolation> enforcePrintedPragmaPlacement(
    const PrintedPragmaHunkPlacementDependencies &deps,
    std::vector<diffutils::Hunk> &hunks,
    const std::set<std::tuple<uint64_t, uint64_t, uint64_t, uint64_t>>
        &boundHunks) {
  // A line printed by a relocatable carrier may be repaired.  Every other
  // printed line with a known B position -- a header directive, a line a
  // non-relocatable macro expansion printed, or the B lines a sideband edit
  // writes -- is checked only.
  std::map<uint64_t, std::pair<uint64_t, uint64_t>> bRangeByAGap;
  // A checked line, and the include occurrence whose expansion prints it when
  // it is written inside a header.
  struct CheckedLine {
    SidebandPragmaEdit::PrintedGaps gaps;
    std::optional<uint64_t> ownerIncludeId;
  };
  SmallVector<CheckedLine, 4> checkedGaps;
  for (const SidebandPragmaLinePairing &line :
       deps.sidebandPragmaLinePairings) {
    if (!line.bNormalTokenGap)
      continue;
    const uint64_t aGap = line.aNormalTokenGap;
    const uint64_t bGap = *line.bNormalTokenGap;
    const PrintedPragmaCarrier *carrier =
        findUniquePrintedPragmaCarrier(deps.printedPragmaCarriers, line);
    // A header's carrier is repaired at a gap inside that header or on its
    // boundary.  Inside, the insertion the repair makes is owned by the header
    // and placed among its own lines.  On the boundary, B's order decides: a
    // payload B prints between the line and the rest of the header is forced
    // into the header (`IncludeHoldingPayloadBesidePrintedPragma`), and one it
    // prints outside stays with the parent, before or after the whole header.
    // A header with no tokens sits exactly at the gap its own lines print at.
    const RefoldModel::IncludeItem *carrierInclude =
        carrier && carrier->ownerIncludeId
            ? deps.model.GetIncludeById(*carrier->ownerIncludeId)
            : nullptr;
    const bool repairable =
        carrier && carrier->relocatable &&
        (!carrier->ownerIncludeId ||
         (carrierInclude && (carrierInclude->spans.empty() ||
                             (carrierInclude->cover.begin <= aGap &&
                              aGap <= carrierInclude->cover.end))));
    if (!repairable) {
      const std::optional<ArrayRef<uint64_t>> expansionAncestors =
          deps.macroTopology.PragmaExpansionAncestorsAtGap(aGap);
      checkedGaps.push_back(
          {{aGap, bGap, bGap},
           expansionAncestors && expansionAncestors->empty()
               ? headerDirectiveOwnerOfPrintedLine(deps.model, line)
               : std::nullopt});
      continue;
    }
    auto [it, inserted] = bRangeByAGap.try_emplace(aGap, bGap, bGap);
    if (!inserted) {
      it->second.first = std::min(it->second.first, bGap);
      it->second.second = std::max(it->second.second, bGap);
    }
  }
  for (const SidebandPragmaEdit &edit : deps.sidebandPragmaEdits)
    if (edit.GetPrintedGaps())
      checkedGaps.push_back({*edit.GetPrintedGaps(), edit.OwnerIncludeId()});

  auto isBound = [&](const diffutils::Hunk &h) {
    return boundHunks.count(
               std::make_tuple(h.aStart, h.aEnd, h.bStart, h.bEnd)) != 0;
  };

  for (const auto &[aGap, wanted] : bRangeByAGap) {
    // Locate the hunks touching the gap: one ending at it, one pure insertion
    // at it, one starting at it.  A hunk strictly across it would carry the
    // directive's source inside one edit, which is never a placement.
    std::optional<size_t> left, insertion, right;
    int64_t delta = 0;
    bool ambiguous = false;
    for (size_t i = 0; i < hunks.size(); ++i) {
      const diffutils::Hunk &h = hunks[i];
      std::optional<size_t> *slot = nullptr;
      if (h.aStart < aGap && aGap < h.aEnd)
        ambiguous = true;
      else if (h.aStart == aGap && h.aEnd == aGap)
        slot = &insertion;
      else if (h.aEnd == aGap)
        slot = &left;
      else if (h.aStart == aGap)
        slot = &right;
      else if (h.aEnd < aGap)
        delta += static_cast<int64_t>(h.bEnd - h.bStart) -
                 static_cast<int64_t>(h.aEnd - h.aStart);
      if (slot) {
        ambiguous |= slot->has_value();
        *slot = i;
      }
    }

    // The B gaps the hunks currently realize for the directive.
    uint64_t curLo = 0;
    if (insertion)
      curLo = hunks[*insertion].bStart;
    else if (left)
      curLo = hunks[*left].bEnd;
    else if (right)
      curLo = hunks[*right].bStart;
    else
      curLo = static_cast<uint64_t>(static_cast<int64_t>(aGap) + delta);
    const uint64_t curHi = insertion ? hunks[*insertion].bEnd : curLo;
    PrintedPragmaPlacementViolation violation{aGap, wanted.first, curLo, curHi};
    if (wanted.first < curLo)
      violation.bGap = wanted.first;
    else if (wanted.second > curHi)
      violation.bGap = wanted.second;
    else if (!ambiguous)
      continue;
    if (ambiguous)
      return violation;
    if ((left && hunks[*left].bEnd != curLo) ||
        (right && hunks[*right].bStart != curHi))
      return violation;

    // A directive B prints across tokens no hunk changes is moved by deleting
    // those tokens on one side of it and inserting them again on the other.
    // Being unchanged, they map one-to-one, so an identity hunk over them
    // states exactly that; it is made only over tokens no other hunk touches.
    auto tokensAreUntouched = [&](uint64_t lo, uint64_t hi) {
      return llvm::none_of(hunks, [&](const diffutils::Hunk &h) {
        return h.aStart == h.aEnd ? lo < h.aStart && h.aStart < hi
                                  : h.aStart < hi && lo < h.aEnd;
      });
    };
    if (!left && !insertion && wanted.first < curLo) {
      const uint64_t k = curLo - wanted.first;
      if (k > aGap || !tokensAreUntouched(aGap - k, aGap))
        return violation;
      const size_t at =
          static_cast<size_t>(llvm::find_if(hunks,
                                            [&](const diffutils::Hunk &h) {
                                              return h.aStart >= aGap;
                                            }) -
                              hunks.begin());
      hunks.insert(hunks.begin() + static_cast<std::ptrdiff_t>(at),
                   diffutils::Hunk{aGap - k, aGap, curLo - k, curLo});
      left = at;
      if (right)
        ++*right;
    }
    if (!right && !insertion && wanted.second > curHi) {
      const uint64_t k = wanted.second - curHi;
      if (aGap + k > deps.model.GetTokensCountA() ||
          !tokensAreUntouched(aGap, aGap + k))
        return violation;
      const size_t at =
          static_cast<size_t>(llvm::find_if(hunks,
                                            [&](const diffutils::Hunk &h) {
                                              return h.aStart > aGap;
                                            }) -
                              hunks.begin());
      hunks.insert(hunks.begin() + static_cast<std::ptrdiff_t>(at),
                   diffutils::Hunk{aGap, aGap + k, curHi, curHi + k});
      right = at;
    }

    // Move the far side of each directive into the insertion at the gap.
    const uint64_t newLo = std::min(curLo, wanted.first);
    const uint64_t newHi = std::max(curHi, wanted.second);
    if ((newLo < curLo &&
         (!left || isBound(hunks[*left]) || newLo < hunks[*left].bStart)) ||
        (newHi > curHi &&
         (!right || isBound(hunks[*right]) || hunks[*right].bEnd < newHi)) ||
        (insertion && isBound(hunks[*insertion])))
      return violation;

    REFOLD_LOG_TRACE("tiling/printed-pragma",
                     "A gap {0} holds surviving directive(s) B prints at "
                     "B=[{1},{2}] but the hunks print at B=[{3},{4}]; moving "
                     "B=[{5},{6}) into a pure insertion at the gap",
                     aGap, wanted.first, wanted.second, curLo, curHi, newLo,
                     newHi);
    if (left)
      hunks[*left].bEnd = newLo;
    if (right)
      hunks[*right].bStart = newHi;
    if (insertion) {
      hunks[*insertion].bStart = newLo;
      hunks[*insertion].bEnd = newHi;
      continue;
    }
    const size_t at = right ? *right : (left ? *left + 1 : hunks.size());
    hunks.insert(hunks.begin() + static_cast<std::ptrdiff_t>(at),
                 diffutils::Hunk{aGap, aGap, newLo, newHi});
  }

  // A checked line is printed where its source site lands: the single B gap
  // the hunks leave at its A gap.  A line whose macro caller chain does not
  // resolve has no known carrier, so no hunk may border its gap either.
  //
  // A pure insertion at the gap has no proved order against a line written in
  // translation-unit source.  Against a line written inside a header it does,
  // at the header's own boundary: the owner classifier assigns an insertion
  // the least common ancestor of the includes on either side of its gap, and
  // at an include's first or last gap one side lies outside that include.  So
  // the insertion is realized outside the header -- before all of its lines
  // at the first gap, after them at the last.  A header covering no tokens has
  // one gap for both and stays refused.
  for (const CheckedLine &checked : checkedGaps) {
    const uint64_t aGap = checked.gaps.aGap;
    const std::optional<ArrayRef<uint64_t>> expansionAncestors =
        deps.macroTopology.PragmaExpansionAncestorsAtGap(aGap);
    const bool unknownCarrier = !expansionAncestors;
    // A line a macro expansion printed stays where it is only while its root
    // invocation does.  A hunk inside that invocation hands it to a macro
    // realizer, which rewrites the callsite; where the line lands is then the
    // realizer's to prove -- its candidate admission and the emission audit
    // both require B's copy to be replayed -- and not this gap's.
    if (expansionAncestors &&
        rootInvocationTouchedByHunk(deps.macroTopology, *expansionAncestors,
                                    hunks))
      continue;
    std::optional<size_t> left, insertion, right;
    int64_t delta = 0;
    bool refused = false;
    for (size_t i = 0; i < hunks.size(); ++i) {
      const diffutils::Hunk &h = hunks[i];
      std::optional<size_t> *slot = nullptr;
      if (h.aStart < aGap && aGap < h.aEnd)
        refused = true;
      else if (h.aStart == aGap && h.aEnd == aGap)
        slot = &insertion;
      else if (h.aEnd == aGap)
        slot = &left;
      else if (h.aStart == aGap)
        slot = &right;
      else if (h.aEnd < aGap)
        delta += static_cast<int64_t>(h.bEnd - h.bStart) -
                 static_cast<int64_t>(h.aEnd - h.aStart);
      if (slot) {
        refused |= slot->has_value() || unknownCarrier;
        *slot = i;
      }
    }

    uint64_t bGap = static_cast<uint64_t>(static_cast<int64_t>(aGap) + delta);
    if (insertion) {
      const RefoldModel::IncludeItem *owner =
          checked.ownerIncludeId
              ? deps.model.GetIncludeById(*checked.ownerIncludeId)
              : nullptr;
      const bool atFirstGap = owner && owner->cover.IsValid() &&
                              owner->cover.begin < owner->cover.end &&
                              aGap == owner->cover.begin;
      const bool atLastGap = owner && owner->cover.IsValid() &&
                             owner->cover.begin < owner->cover.end &&
                             aGap == owner->cover.end;
      refused |= !atFirstGap && !atLastGap;
      bGap = atFirstGap ? hunks[*insertion].bEnd : hunks[*insertion].bStart;
    } else if (left) {
      bGap = hunks[*left].bEnd;
    } else if (right) {
      bGap = hunks[*right].bStart;
    }
    if (left && right && !insertion &&
        hunks[*left].bEnd != hunks[*right].bStart)
      refused = true;
    if (refused || checked.gaps.bGapFirst != bGap ||
        checked.gaps.bGapLast != bGap)
      return PrintedPragmaPlacementViolation{aGap,
                                             checked.gaps.bGapFirst != bGap
                                                 ? checked.gaps.bGapFirst
                                                 : checked.gaps.bGapLast,
                                             bGap, bGap};
  }
  return std::nullopt;
}

} // namespace refold
} // namespace clang
