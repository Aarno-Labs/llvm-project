//===--- RefoldMacroFinalCandidateSelector.h ------------------*- C++ -*-===//
//
// Final macro candidate selector service.
//
// Runs after the orchestrator has populated the per-call planning
// context's candidate slots (direct args-only, DAG-root replay, selector
// substitution, existing-callsite reuse, existing-expanded reuse,
// whole-cover realization).  `Run(planningCtx)` admits the surviving
// candidates, ranks them through the proof lattice, and certifies the
// winner, returning the final `MacroPatch` or `std::nullopt` when no
// candidate survives final admission.
//
// The selector reaches everything it uses through `Dependencies`: the
// borrowed proof services, the orchestrator-owned
// `RefoldMacroPatchReusePhase`, proof certifier and replay-stability
// validator, the whole-cover plan builder, and three planner-helper
// callbacks.  It holds no reference to the planner, and no friend access is
// used or needed.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDMACROFINALCANDIDATESELECTOR_H
#define LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDMACROFINALCANDIDATESELECTOR_H

#include "edit/RefoldPatchTypes.h"
#include "macro/RefoldMacroOccurrenceProofValidator.h"
#include "model/RefoldModel.h"
#include "model/RefoldToken.h"
#include "proof/RefoldMacroPatchTypes.h"
#include "source/RefoldDiffTypes.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <utility>
#include <vector>

namespace clang {
class LangOptions;

namespace refold {

struct RefoldMacroWholeCoverPlanningContext;
class RefoldMacroPatchProofCertifier;
class RefoldMacroPatchReusePhase;
class RefoldMacroReplayStabilityValidator;
class RefoldMacroTopology;
class RefoldMacroWholeCoverPlanBuilder;
class RefoldAcceptedCandidateBuilder;
class RefoldAcceptedResultRanker;
struct SidebandPragmaLinePairing;

/// Final whole-cover-family candidate selector.
///
/// The selector is constructed once by the orchestrator with borrowed proof
/// services and a small set of planner-callback hooks; `Run` consumes the
/// already-populated planning context and returns the selected patch.
class RefoldMacroFinalCandidateSelector {
public:
  /// Borrowed inputs needed by the final candidate selector.  All
  /// references must outlive the selector; the orchestrator owns them.
  struct Dependencies {
    const RefoldModel &model;
    const RefoldMacroTopology &topology;
    const RefoldAcceptedCandidateBuilder &acceptedCandidateBuilder;
    const RefoldAcceptedResultRanker &acceptedResultRanker;
    const RefoldMacroReplayStabilityValidator &replayStabilityValidator;
    const RefoldMacroPatchProofCertifier &proofCertifier;
    const RefoldMacroPatchReusePhase &patchReusePhase;

    /// Root invocations ruled out from keeping their callsite, or null when
    /// none are.  Borrowed from the planner's dependency bundle.
    const llvm::DenseSet<uint64_t> *ownersMustExpand = nullptr;

    /// Where each `#pragma` line printed into A survived in B; see
    /// `SidebandPragmaLinePairing`.  Borrowed from the planner's bundle.
    llvm::ArrayRef<SidebandPragmaLinePairing> sidebandPragmaLinePairings;

    /// Whole-cover plan computation, and the plan/patch consistency check
    /// that gates reuse of an existing whole-cover patch for the same root.
    const RefoldMacroWholeCoverPlanBuilder &wholeCoverPlanBuilder;

    /// Delegates to `RefoldMacroPatchPlanner::MacroPatchOwnerMatches`.
    std::function<bool(const MacroPatch &, const Owner &)>
        macroPatchOwnerMatches;

    /// Delegates to `RefoldMacroPatchPlanner::IsParenthesizedTuple`.
    std::function<bool(llvm::StringRef)> isParenthesizedTuple;

    /// Delegates to
    /// `RefoldMacroPatchPlanner::GetMacroInvocationFormalArgContentRanges`.
    std::function<std::optional<std::vector<std::pair<size_t, size_t>>>(
        const RefoldModel::MacroInvocation &, llvm::StringRef)>
        getMacroInvocationFormalArgContentRanges;

    /// Language options used to compare actual spellings token by token.
    const clang::LangOptions &lexLang;

    /// The A and B token streams and the pass's token hunks, against which a
    /// structure-preserving candidate's re-expansion is refuted.
    llvm::ArrayRef<PPTok> aToks;
    llvm::ArrayRef<PPTok> bToks;
    llvm::ArrayRef<diffutils::Hunk> hunks;
  };

  explicit RefoldMacroFinalCandidateSelector(Dependencies deps);

  /// Run final candidate admission + ranking over the planning context
  /// that the earlier phases have already populated.  Reads the candidate
  /// slots (`argsOnlyCandidate`, `dagRootCandidate`), the conflict flags
  /// (`directRootPreservationInadmissible`,
  /// `conflictingConcreteSubtreeWitnessForcesWholeCover`,
  /// `reuseExistingCallsitePatch`,
  /// `existingCallsitePatchAbsorbedByDirectCandidate`), and the reuse
  /// admission context.  May mutate the candidate slots and reuse flags
  /// as part of the pre-selection direct/existing merge.
  std::optional<MacroPatch>
  Run(RefoldMacroWholeCoverPlanningContext &planningCtx) const;

private:
  /// Return the paired `#pragma` lines that \p m's expansion printed: those
  /// whose `_Pragma` has \p m among its callers.  A gap whose caller chain
  /// does not resolve counts when it lies within \p m's own A-token extent,
  /// so an unknown producer is never assumed absent.
  llvm::SmallVector<const SidebandPragmaLinePairing *, 2>
  PrintedPragmaLinesOf(const RefoldModel::MacroInvocation &m) const;

  /// Return, ascending, the formals whose actual \p patch spells differently,
  /// token by token, from \p m's original A-side spelling, or nullopt when
  /// either spelling does not parse into \p m's formal slots.
  std::optional<llvm::SmallVector<uint32_t, 4>>
  FormalsRespelledByPatch(const RefoldModel::MacroInvocation &m,
                          const MacroPatch &patch) const;

  /// Return whether structure-preserving \p patch, emitted for \p m's
  /// callsite slot, may still realize every hunk in \p stagedHunks.
  ///
  /// The slot holds one patch, so the selected patch replaces the one that
  /// realized the hunks staged there earlier.  The answer is false only when
  /// `RefoldMacroRewriteRealizationProof` refutes \p patch's re-expansion
  /// against B; a patch it cannot model keeps relying on the proof that built
  /// it.
  bool
  PatchCanRealizeStagedHunks(const RefoldModel::MacroInvocation &m,
                             const MacroPatch &patch,
                             llvm::ArrayRef<ATokenRange> stagedHunks) const;

  Dependencies deps_;
};

} // namespace refold
} // namespace clang

#endif // LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDMACROFINALCANDIDATESELECTOR_H
