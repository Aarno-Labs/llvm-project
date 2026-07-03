//===--- RefoldMacroDAGLeafDiscoveryPhase.h --------------------*- C++ -*-===//
//
// Discovery half of the DAG-chained args-only attempt for the whole-cover
// orchestrator.
//
// Searches for nested-callee leaf invocations whose argument edits can be
// lifted back to the current callsite, and produces the inputs the
// lifting phase (`RefoldMacroDAGLiftingPhase`) consumes:
//
//   * root invocation parsing preconditions (callsite spelling, formal
//     argument byte ranges)
//   * an invocation-id -> invocation lookup map and a
//     `MacroSubtreeReplayValidationContext`
//   * argument-like span collection
//   * sibling pure-insertion split-insertion root candidates (used later
//     when the lifting path replays a wider envelope)
//   * candidate leaf discovery and depth-first ordering
//   * a cache of the root invocation's current per-formal argument text
//
// `Run` publishes all of that in a `DAGLeafDiscoveryResult` carrier that
// the lifting phase reads through local aliases.
//
// The phase has no back-reference to the planner: planner-side helpers
// (`GetMacroInvocationFormalArgContentRanges`,
// `BuildMacroInvocationPatchArgsOnly`) are reached through std::function
// callbacks supplied at construction; the
// `RefoldMacroSubtreeReplayValidator` and
// `RefoldMacroOccurrenceProofValidator` services are passed by
// reference / constructed from explicit deps.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDMACRODAGLEAFDISCOVERYPHASE_H
#define LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDMACRODAGLEAFDISCOVERYPHASE_H

#include "core/RefoldModel.h"
#include "edit/RefoldPatchTypes.h"
#include "source/DiffAlgorithms.h"
#include "source/RefoldToken.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <utility>
#include <vector>

namespace clang {
namespace refold {

struct RefoldMacroWholeCoverPlanningContext;
class RefoldArgTextRecovery;
class RefoldMacroSubtreeReplayValidator;
class RefoldMacroTopology;
class RefoldSourceMapper;

/// One candidate leaf invocation whose argument-like spans are touched by
/// the current hunk.  The discovery phase records this carrier before the
/// lifting phase proves whether the candidate can be replayed at the root.
struct DAGLeafCandidate {
  const RefoldModel::MacroInvocation *inv;
  /// Distance from this leaf to the root invocation: 1..N.
  unsigned depth;
  /// Smallest argument-like span (in producer bytes) covered by the
  /// touched portion of this candidate.  Tie-breaker for ordering.
  uint64_t smallestSpanBytes;
  /// All argument-like spans (standard / stringify / paste) on this
  /// candidate, after sanitization.
  llvm::SmallVector<RefoldModel::PPArgSpan, 8> argLike;
  /// One-per-formal touched flag.  `touched[i]` is true iff some argLike
  /// span with `argIdx == i` is covered by the current hunk.
  llvm::SmallVector<char, 8> touched;
};

/// Root candidate built from a paired pure-insertion envelope inside the
/// leaf-discovery phase.  Carried forward so the lifting phase can merge
/// it into the eventual root patch without re-doing the partner search.
struct DAGSplitInsertionRootCandidate {
  MacroPatch patch;
  /// Root formal indices that the trimmed combined insertion envelope
  /// touched.  The lifting phase defers occurrence-level consistency
  /// checks for these formals exactly once.
  llvm::SmallVector<uint32_t, 8> deferOccurrenceArgIdxs;
};

/// Output bundle from the DAG leaf-discovery phase.
struct DAGLeafDiscoveryResult {
  /// True when discovery decided that DAG chaining cannot proceed (root
  /// callsite text does not match a callsite prefix, or formal-arg
  /// content-range recovery failed).  The lifting half should propagate
  /// the equivalent of the lambda's early `return std::nullopt` whenever
  /// this is set.
  bool aborted = false;

  /// Callsite spelling chosen for argument parsing — either the planning
  /// context's `baseInvText` (when present) or `m.invText`.
  llvm::StringRef invSpanText;

  /// Byte ranges (in `invSpanText`) of each formal-argument content slot.
  std::vector<std::pair<size_t, size_t>> invArgRanges;

  /// Lookup from `MacroInvocation::id` to the corresponding invocation
  /// pointer, populated once at the start of the phase.
  llvm::DenseMap<uint64_t, const RefoldModel::MacroInvocation *> invById;

  /// Sorted leaf candidates (deepest first, smaller span first on ties).
  llvm::SmallVector<DAGLeafCandidate, 8> leafCands;

  /// Split-insertion root candidates pre-built from sibling pure-insertion
  /// pairs.  May be empty.
  llvm::SmallVector<DAGSplitInsertionRootCandidate, 8>
      splitInsertionRootCandidates;

  /// Per-formal cache of the root invocation's current trimmed argument
  /// text, used by lifting for final-hop two-parent splitting and
  /// validation.
  llvm::DenseMap<uint32_t, llvm::StringRef> rootArgText;

  /// Set when an unsupported descendant structure blocks direct root
  /// preservation.  Read by the orchestrator after the lambda returns to
  /// decide whether whole-cover realization should compete.
  bool directRootPreservationInadmissible = false;
};

/// Discovers nested DAG leaf candidates that can be lifted to a root macro
/// invocation.  The phase populates leaf candidates, split-insertion root
/// candidates, and per-formal root text caches used by the lifting phase.
class RefoldMacroDAGLeafDiscoveryPhase {
public:
  /// Borrowed inputs needed by the DAG leaf-discovery phase.  All
  /// references must outlive the phase; the orchestrator owns all of them.
  ///
  /// `abTokHunks` is held by vector reference (not `ArrayRef`) because the
  /// engine populates it after the planner — and therefore this phase — is
  /// constructed; see `feedback_arrayref_captures.md`.
  struct Dependencies {
    const RefoldModel &model;
    const RefoldMacroTopology &macroTopology;
    const RefoldSourceMapper &sourceMapper;
    const RefoldArgTextRecovery &argTextRecovery;
    llvm::ArrayRef<PPTok> aToks;
    llvm::ArrayRef<PPTok> bToks;
    const std::vector<diffutils::Hunk> &abTokHunks;
    const RefoldMacroSubtreeReplayValidator &subtreeReplayValidator;

    /// Delegates to
    /// `RefoldMacroPatchPlanner::GetMacroInvocationFormalArgContentRanges`.
    std::function<std::optional<std::vector<std::pair<size_t, size_t>>>(
        const RefoldModel::MacroInvocation &, llvm::StringRef)>
        getMacroInvocationFormalArgContentRanges;

    /// Delegates to
    /// `RefoldMacroPatchPlanner::BuildMacroInvocationPatchArgsOnly`. Used by
    /// the split-insertion partner scan to convert a paired-pure- insertion
    /// envelope into a structure-preserving root candidate.
    std::function<std::optional<MacroPatch>(
        const RefoldModel::MacroInvocation &, const diffutils::Hunk &,
        llvm::StringRef)>
        buildMacroInvocationPatchArgsOnly;
  };

  explicit RefoldMacroDAGLeafDiscoveryPhase(Dependencies deps);

  /// Run the discovery phase against `planningCtx` and populate `result`.
  /// On early-exit conditions (callsite text isn't a callsite prefix, or
  /// formal-arg recovery fails) `result.aborted` is set and the orchestrator
  /// should treat that as the lambda's early `return std::nullopt`.
  void Run(const RefoldMacroWholeCoverPlanningContext &planningCtx,
           DAGLeafDiscoveryResult &result) const;

private:
  Dependencies deps_;
};

} // namespace refold
} // namespace clang

#endif // LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDMACRODAGLEAFDISCOVERYPHASE_H
