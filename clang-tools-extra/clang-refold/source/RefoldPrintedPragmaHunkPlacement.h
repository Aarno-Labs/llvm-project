//===--- RefoldPrintedPragmaHunkPlacement.h --------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Final placement check and repair of printed `#pragma` lines against the
// structurally tiled token hunks.  The structural tiler runs it on its finished
// hunk list; it reads producer facts only and builds no source edits.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDPRINTEDPRAGMAHUNKPLACEMENT_H
#define LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDPRINTEDPRAGMAHUNKPLACEMENT_H

#include "source/RefoldDiffTypes.h"

#include "llvm/ADT/ArrayRef.h"

#include <cstdint>
#include <optional>
#include <set>
#include <tuple>
#include <vector>

namespace clang {
namespace refold {

class RefoldMacroTopology;
class RefoldModel;
struct PrintedPragmaCarrier;
struct SidebandPragmaEdit;
struct SidebandPragmaLinePairing;

/// A surviving translation-unit `#pragma` directive at A gap `aGap` that B
/// prints at B gap `bGap`, which no repair of the hunks around the gap can
/// realize.  `bLo`/`bHi` bound the B gaps the final hunks do realize there.
struct PrintedPragmaPlacementViolation {
  uint64_t aGap = 0;
  uint64_t bGap = 0;
  uint64_t bLo = 0;
  uint64_t bHi = 0;
};

/// Producer facts `enforcePrintedPragmaPlacement` reads, borrowed from the
/// structural tiler's dependencies.
struct PrintedPragmaHunkPlacementDependencies {
  const RefoldModel &model;
  const RefoldMacroTopology &macroTopology;
  /// Where each `#pragma` line printed into A survived in B.
  llvm::ArrayRef<SidebandPragmaLinePairing> sidebandPragmaLinePairings;
  /// Sideband source edits; a replaying one prints B's lines at its own
  /// source position.
  llvm::ArrayRef<SidebandPragmaEdit> sidebandPragmaEdits;
  /// Source carriers of the paired lines.
  llvm::ArrayRef<PrintedPragmaCarrier> printedPragmaCarriers;
};

/// Make \p hunks print every printed pragma line whose B position is known
/// where B prints it, or report the first they cannot.
///
/// A paired line keeps its source, and a replaying sideband edit writes B's
/// lines at a source position, so each is printed wherever that position
/// lands among the realized hunks: before a hunk that starts at its A gap,
/// after one that ends there, and on the side a pure insertion at the gap is
/// placed on.  The B gap must be one of those.  Pairing cannot guarantee
/// that -- it is decided before, and independently of, the alignment the
/// hunks come from -- so it is checked here, on the final hunks.
///
/// A line printed by a relocatable carrier (see
/// `PrintedPragmaCarrier::relocatable`) may be repaired: when B prints it
/// inside a hunk bordering its gap, B's token order forces the tokens on the
/// far side into a pure insertion at the gap, placed by
/// `placeTUInsertionAmongPrintedPragmas`.  When no hunk borders the gap, the
/// unchanged tokens between A's and B's positions are first stated as an
/// identity hunk, so a moved directive becomes those tokens deleted on one
/// side and inserted on the other.  Hunks in \p boundHunks, which a tiling
/// witness is bound to, are never changed.
///
/// Every other paired line -- a header directive, a line a macro expansion
/// that is not relocatable produced -- and the lines a sideband edit writes
/// are checked only, because the insertion placement proves an order only
/// against relocatable carriers: each must sit at the single B gap the hunks
/// leave at its A gap.  A line whose macro
/// caller chain does not resolve has no known carrier, so no hunk may border
/// its gap.  Every disagreement is returned, and must fail closed.
///
/// This is necessary, not sufficient, for a line whose carrier a realizer
/// may replace -- a macro invocation rewritten or expanded from B, or a
/// header materialized around a sideband edit.  Where such a realizer puts
/// the line is decided after planning and is not checked here.
std::optional<PrintedPragmaPlacementViolation> enforcePrintedPragmaPlacement(
    const PrintedPragmaHunkPlacementDependencies &deps,
    std::vector<diffutils::Hunk> &hunks,
    const std::set<std::tuple<uint64_t, uint64_t, uint64_t, uint64_t>>
        &boundHunks);

} // namespace refold
} // namespace clang

#endif // LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDPRINTEDPRAGMAHUNKPLACEMENT_H
