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
// mapper, and the B-insertion ledger; it needs neither the proof services nor
// any part of the macro patch planner.
//
// That is why this is a service of its own.  Its consumers include the
// macro patch planner, the text-edit assembler's emitted-range recovery,
// counter stabilization, and several macro-side phase services.  Because the
// builder needs none of them, it is constructed before all of them, and each
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

#include "edit/RefoldPatchTypes.h"
#include "model/RefoldModel.h"
#include "proof/RefoldMacroPatchTypes.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"

#include <cstdint>
#include <optional>
#include <utility>

namespace clang {
namespace refold {

class RefoldMacroTopology;
struct SidebandPragmaLinePairing;

class RefoldBInsertionLedger;
class RefoldPathIdentity;
class RefoldPreprocessingStructureIndexProvider;
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
    /// Caller chains of macro-produced `_Pragma`s, and where each printed
    /// `#pragma` line survived in B: an invocation replaced by its whole cover
    /// must replay the lines its own expansion printed.
    const RefoldMacroTopology &topology;
    llvm::ArrayRef<SidebandPragmaLinePairing> sidebandPragmaLinePairings;
    llvm::StringRef bSource;
    /// Physical path comparison for the file instance of a callsite.
    const RefoldPathIdentity &pathIdentity;
    /// Preprocessing structure of the source a widened realization replaces.
    const RefoldPreprocessingStructureIndexProvider &structureIndexes;
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

  /// Build the replacement text for a macro whole-cover realization.
  ///
  /// Computes the same whole-cover plan used by macro patch construction and
  /// returns its clipped replacement text. This helper is for callers that need
  /// the materialized whole-cover text without constructing a full
  /// `MacroPatch`.
  std::optional<std::string>
  BuildWholeCoverReplacementText(const RefoldModel::MacroInvocation &m) const;

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
  /// Return the end of the physical source bytes \p m's expansion consumed,
  /// or nullopt when \p m records no source range or the end is not proven.
  ///
  /// It is `invE` unless a call the expansion generates is closed by source
  /// text after \p m's closing parenthesis: `#define CALL(f, a) int f(a`
  /// leaves `G(x` open, and `CALL(G, x))` reads the second `)` to close it.
  /// The cover's B image already holds everything that call produced, so a
  /// realization that stops at `invE` leaves the consumed `)` behind.
  ///
  /// The end is the largest `inv_e` among \p m's descendants spelled in
  /// \p m's own file instance.  Rescanning \p m's expansion reads source
  /// tokens only to complete a call it has already begun, so the bytes from
  /// `invE` to there are a contiguous run of such calls' arguments and
  /// parentheses: a token spelled there is discarded or lands in \p m's
  /// expansion, which the cover's B image realizes.  The end is accepted only
  /// when the structure index proves those bytes hold no preprocessing
  /// structure, which the realization would otherwise remove.  An invocation
  /// spelled inside a `#define` reads no source after itself, and keeps
  /// `invE`.
  std::optional<uint64_t>
  ConsumedSourceEnd(const RefoldModel::MacroInvocation &m) const;

  /// Make \p plan replay the surviving `#pragma` lines \p m's own expansion
  /// printed, and record them.
  ///
  /// Replacing the callsite removes the only source that printed such a line,
  /// so the whole cover must carry B's copy.  A line strictly inside the kept
  /// B segment is already part of \p material.  One at either edge is added,
  /// provided only whitespace and other such lines separate it from
  /// \p material; a leading line is given a line of its own because the
  /// callsite may sit mid-line.  A line B prints outside the segment, or an
  /// edge holding anything else, leaves the plan unextended and unrecorded,
  /// so a candidate built from it is refused rather than dropping the line.
  void ReplayProducedPragmaLines(const RefoldModel::MacroInvocation &m,
                                 std::pair<size_t, size_t> keptSegment,
                                 llvm::StringRef material,
                                 WholeCoverPlan &plan) const;

  Dependencies deps_;
};

} // namespace refold
} // namespace clang

#endif // LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDMACROWHOLECOVERPLANBUILDER_H
