//===--- RefoldMacroWholeCoverPlanBuilder.h --------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM
// Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Whole-cover replacement plan computation for clang-refold.
//
// A `WholeCoverPlan` is the certified projection of one macro invocation's
// A-token cover onto the B-token material that a whole-cover realization
// would replay.  Computing it needs only the invocation, the static
// whole-cover proof helpers (`RefoldMacroWholeCoverProof`), the A->B source
// mapper, and the B-insertion ledger; it needs neither the proof lattice nor
// any part of the macro patch planner.
//
// That is why this is a service of its own.  The plan query has consumers on
// both sides of the planner/lattice construction cycle — the lattice's
// `BuildWholeCoverReplacementText`, the text-edit assembler's emitted-range
// recovery, and three macro-side phase services — and while it lived on
// `RefoldMacroWholeCoverOrchestrator` every one of them had to reach it
// through a late-bound `std::function` installed after construction.  Split
// out, it is constructible before both the lattice and the planner, and each
// consumer holds a direct reference.
//
// Do not confuse this with `macro/RefoldMacroWholeCoverPlanningContext.h`,
// which is the per-call mutable state carrier for
// `BuildMacroInvocationPatchWholeCover`.  This service computes the plan; that
// header carries the state of a single orchestration pass.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDMACROWHOLECOVERPLANBUILDER_H
#define LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDMACROWHOLECOVERPLANBUILDER_H

#include "core/RefoldModel.h"
#include "edit/RefoldPatchTypes.h"
#include "proof/RefoldMacroPatchTypes.h"

#include <cstdint>
#include <optional>

namespace clang {
namespace refold {

class RefoldBInsertionLedger;
class RefoldSourceMapper;

/// Computes the whole-cover replacement plan for a macro invocation, and
/// checks an existing patch against one.
///
/// The service is stateless: it borrows the two read-only services the
/// projection consults and holds no run state of its own.  Both are borrowed
/// live rather than snapshotted, because the B-insertion ledger's claims grow
/// as candidates are accepted and the plan must be clipped against the claims
/// standing at the moment it is asked for.
class RefoldMacroWholeCoverPlanBuilder {
public:
  /// Borrowed inputs of the whole-cover projection.
  ///
  /// Both outlive the builder: the source mapper is an engine member and the
  /// insertion ledger is constructed before every consumer of this service.
  struct Dependencies {
    const RefoldSourceMapper &sourceMapper;
    const RefoldBInsertionLedger &bInsertionLedger;
  };

  explicit RefoldMacroWholeCoverPlanBuilder(Dependencies deps) : deps_(deps) {}

  /// Compute the whole-cover replacement plan for \p m, or std::nullopt when
  /// the invocation has no admissible whole-cover realization.
  ///
  /// The plan is refused — rather than approximated — when the cover is not
  /// self-contained in the invocation's own provenance, when the A cover does
  /// not project to a B-token envelope, or when clipping the envelope against
  /// already-claimed boundary insertions leaves anything other than exactly
  /// one contiguous run.  Each refusal is documented at its check.
  std::optional<WholeCoverPlan>
  ComputeWholeCoverPlan(const RefoldModel::MacroInvocation &m) const;

  /// Return true when \p patch is a whole-cover realization of
  /// \p rootMacroId whose recorded cover coordinates still equal those of
  /// \p plan.
  ///
  /// This is a pure comparison of the patch's proof carrier against the plan;
  /// it consults neither borrowed service, and is static to say so.  Reuse
  /// admission uses it to confirm that a patch built in an earlier pass still
  /// describes the plan the current claim state produces.
  static bool WholeCoverPatchMatchesPlan(const MacroPatch &patch,
                                         const WholeCoverPlan &plan,
                                         uint64_t rootMacroId);

private:
  Dependencies deps_;
};

} // namespace refold
} // namespace clang

#endif // LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDMACROWHOLECOVERPLANBUILDER_H
