//===--- RefoldMacroWholeCoverPlanningContext.h ---------------*- C++ -*-===//
//
// Per-call state cluster for the whole-cover orchestrator.
//
// `RefoldMacroWholeCoverOrchestrator::BuildMacroInvocationPatchWholeCover`
// threads twelve-plus per-call values (root invocation, hunk, trimmed
// hunk, base invocation text, patch map, existing-patch context, owner,
// invocation byte span, reuse admission context, direct args-only
// candidate, DAG candidate, conflict flags) through its main method
// body and every phase service it calls.
//
// This named carrier lets the orchestrator and each phase service
// (args-only phase, DAG leaf discovery, DAG lifting, patch reuse,
// selector substitution, final candidate selector) take a single
// reference argument instead of re-expanding the parameter list per
// call.
//
// The carrier intentionally borrows its input state by reference from the
// orchestrator method's locals.  Two concerns drive that choice: (a) the
// nested `MacroPatchReuseAdmissionContext` already holds references to the
// invocation and owner, so making the planning context own those values
// would create a reference-dangling pitfall; (b) the orchestrator owns the
// canonical per-call state regardless, so a borrowing carrier is precisely
// what the phase services need without forcing copies.  Only the mutable
// per-call results (`argsOnlyCandidate`, `dagRootCandidate`, conflict
// flags) are owned by value here.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDMACROWHOLECOVERPLANNINGCONTEXT_H
#define LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDMACROWHOLECOVERPLANNINGCONTEXT_H

#include "core/RefoldModel.h"
#include "edit/RefoldPatchTypes.h"
#include "macro/RefoldMacroPatchPlanner.h"
#include "macro/RefoldMacroWholeCoverOrchestrator.h"
#include "proof/RefoldOwnerStateTypes.h"
#include "source/DiffAlgorithms.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"

#include <cstdint>
#include <optional>

namespace clang {
namespace refold {

/// Per-call planning state for the whole-cover orchestrator.
///
/// All `Inputs` fields are borrowed from the caller's locals (either the
/// orchestrator method or the caller of a phase service).  The `Mutable`
/// fields are owned by the planning context and updated in place as
/// phases run.
struct RefoldMacroWholeCoverPlanningContext {
  // ---- Inputs (borrowed from caller-owned storage) ----

  /// Root macro invocation being patched.
  const RefoldModel::MacroInvocation &m;
  /// Original A/B token-level hunk for the edit under consideration.
  const diffutils::Hunk &h;
  /// `h` after common-edge token trimming.  The orchestrator computes this
  /// once and stores it in a local; the planning context borrows it.
  const diffutils::Hunk &hEff;
  /// Source spelling of the invocation callsite.
  llvm::StringRef baseInvText;
  /// All patches already accepted for the current pass, keyed by include
  /// owner then invocation id.  Used for cross-pass reuse decisions.
  const llvm::DenseMap<std::optional<uint64_t>,
                       llvm::DenseMap<uint64_t, MacroPatch>> &patchMap;
  /// Existing patch already associated with this invocation span by the
  /// caller before entering the orchestrator.
  const ExistingMacroPatchContext &existingContext;
  /// Normalized owner certificate for the current edit.  Borrowed from the
  /// orchestrator local so the reuse admission context's `currentPatchOwner`
  /// reference and this field both alias the same Owner instance.
  const Owner &currentPatchOwner;
  /// Half-open invocation byte span in the owning file: `[invStart, invEnd)`.
  uint64_t invStart;
  uint64_t invEnd;
  /// Argument-like spans for the root invocation: normal arg spans,
  /// stringify spans, and paste spans concatenated.  Populated once by the
  /// orchestrator before the args-only phase runs.
  const llvm::SmallVector<RefoldModel::PPArgSpan, 16> &argLikeSpans;
  /// Reuse admission context for the existing-patch / reuse decisions.
  /// Borrowed because its `invocation` and `currentPatchOwner` reference
  /// members must keep referencing the orchestrator's local storage.
  MacroPatchReuseAdmissionContext &reuseAdmissionCtx;

  // ---- Mutable state (owned by the planning context) ----

  /// Direct root args-only candidate produced by the args-only phase.
  /// Held alive until the final macro candidate selector decides whether
  /// to prefer it over a DAG-chained alternative.
  std::optional<MacroPatch> argsOnlyCandidate;

  /// DAG root replay candidate (if any) produced by the DAG lifting phase.
  /// Carried into the final selector instead of being returned early so the
  /// shared arbitration logic runs across all candidate families.
  std::optional<MacroPatch> dagRootCandidate;

  /// Set when two different concrete subtree-backed witnesses for the same
  /// root disagree on an overlapping expected-root formal rewrite.  Forces
  /// whole-cover realization to compete instead of collapsing the
  /// incompatible witnesses into a single root replay.
  bool conflictingConcreteSubtreeWitnessForcesWholeCover = false;

  /// True when the args-only or reuse phase decided that the existing
  /// callsite patch should be reused for this invocation.
  bool reuseExistingCallsitePatch = false;

  /// True when the existing callsite patch was absorbed into the direct
  /// args-only candidate, so the reuse step should not re-emit it.
  bool existingCallsitePatchAbsorbedByDirectCandidate = false;

  /// True when the args-only phase observed direct argument-like surface
  /// at the root.  Drives later "unsupported descendant forces fallback"
  /// arbitration.
  bool rootHasDirectArgLikeSurface = false;

  /// Set by the DAG leaf-discovery phase when the ordinary direct
  /// argument-span proof did not see the edited descendant surface.  Read
  /// by the final candidate selector to decide whether to suppress a
  /// same-root direct args-only candidate that lacks a stronger owner-
  /// level witness.
  bool directRootPreservationInadmissible = false;
};

} // namespace refold
} // namespace clang

#endif // LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDMACROWHOLECOVERPLANNINGCONTEXT_H
