//===--- RefoldMacroSubtreeReplayValidator.h --------------------*- C++ -*-===//
//
// Heavy macro-subtree replay-stability validator for clang-refold.
//
// Owns the three predicates that decide whether a candidate DAG-subtree
// macro patch is replay-safe with respect to its proof root:
//
//   * `SubtreePathHasProvableCalleeClosure` — every generated-callee hop
//     from a descendant up to the candidate root is either justified by a
//     literal callee spelling or by a proven whole-formal caller-forwarding
//     rule.
//   * `SubtreeReplayDoesNotContradictSiblingSurface` — for a local
//     `DagSubtreeRoot` proof, the candidate root's argument substitution
//     does not change the producer-recorded sibling stringify surface that
//     remains observable in B.
//   * `ClaimedWholeEnvelopeIsReplaySafe` — claimed whole-envelope replay
//     backed by a local DAG subtree proof is admissible only when the
//     sibling-surface gate above is satisfied.
//
// These predicates keep subtree replay validation separate from patch
// construction; the lightweight membership and ancestry predicates remain on
// RefoldMacroOccurrenceProofValidator.
//
// The validator does NOT mutate state.  It reads model/topology/source
// facts and returns deterministic boolean verdicts.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDMACROSUBTREEREPLAYVALIDATOR_H
#define LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDMACROSUBTREEREPLAYVALIDATOR_H

#include "core/RefoldModel.h"
#include "edit/RefoldPatchTypes.h"
#include "macro/RefoldMacroOccurrenceProofValidator.h"
#include "source/DiffAlgorithms.h"

#include "llvm/ADT/ArrayRef.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace clang {

class LangOptions;

namespace refold {

class RefoldArgTextRecovery;
class RefoldMacroTopology;
class RefoldSourceMapper;

/// Validates subtree replay constraints for macro DAG and whole-cover paths.
///
/// The validator checks whether a claimed subtree rewrite contradicts sibling
/// surfaces or exceeds the validated invocation envelope.
class RefoldMacroSubtreeReplayValidator {
public:
  /// Borrowed inputs needed during subtree replay validation.  All references
  /// must outlive the validator; the planner owns all of them.
  ///
  /// `abTokHunks` is a reference to the vector itself, not an `ArrayRef`.
  ///
  /// The engine populates `abTokHunks_` after this service is constructed; an
  /// `ArrayRef` captured at construction would freeze the empty vector's data
  /// pointer and produce stale reads.  Keep this as a vector reference so each
  /// call sees the current contents.
  struct Dependencies {
    /// Model used to walk macro invocations and resolve macro directives.
    const RefoldModel &model;
    /// Topology for root-id lookups on subtree members.
    const RefoldMacroTopology &macroTopology;
    /// Source mapper for B-token envelope projection and B-byte slicing.
    const RefoldSourceMapper &sourceMapper;
    /// Arg-text recovery for inverse-stringify decoding of B-side payloads.
    const RefoldArgTextRecovery &argTextRecovery;
    /// Language options used to construct `RefoldMacroActualLayout` for
    /// formal-actual content-range recovery during subtree replay.
    const clang::LangOptions &lexLang;
    /// A/B token-level hunks used by the hunk-touches-span gate.  Stored as
    /// a vector reference (not `ArrayRef`) because the engine populates this
    /// vector after the planner — and therefore this validator — is
    /// constructed.
    const std::vector<diffutils::Hunk> &abTokHunks;
  };

  explicit RefoldMacroSubtreeReplayValidator(Dependencies deps);

  /// True when every ancestry hop from `candidate` up to the validated root
  /// is replayable.  A hop is admissible either via a literal callee origin
  /// or via a proven single-slot whole-formal caller-forwarding rule.
  bool SubtreePathHasProvableCalleeClosure(
      const MacroSubtreeReplayValidationContext &ctx,
      const RefoldModel::MacroInvocation &candidate) const;

  /// True when the candidate root's argument substitution preserves every
  /// producer-recorded sibling stringify surface that is still observable
  /// in B.  Only applies to local `DagSubtreeRoot` proofs that match the
  /// callsite spelling prefix.
  bool SubtreeReplayDoesNotContradictSiblingSurface(
      const MacroSubtreeReplayValidationContext &ctx,
      const MacroPatch &patch) const;

  /// True when a claimed whole-envelope replay backed only by a local DAG
  /// subtree proof is admissible.  Currently a thin wrapper over the
  /// sibling-surface gate; kept separate so future whole-envelope gates can
  /// be added without changing final-candidate order.
  bool ClaimedWholeEnvelopeIsReplaySafe(
      const MacroSubtreeReplayValidationContext &ctx,
      const MacroPatch &patch) const;

private:
  /// True iff `inv`'s definition has a `# argIdx` stringification site for
  /// the given parameter index.  Validator-private duplicate of the
  /// planner's `DirectlyStringifiesFormal`, kept here so the validator does
  /// not back-reference the planner.
  bool DirectlyStringifiesFormal(const RefoldModel::MacroInvocation &inv,
                                 uint32_t argIdx) const;

  /// True iff any cached A/B token hunk overlaps the half-open span
  /// `[begin, end)`.
  bool HunkTouchesASpan(uint64_t begin, uint64_t end) const;

  /// Replay a single direct-stringify argument through the candidate root's
  /// actuals, returning the canonicalized inverse-stringify payload when the
  /// replay succeeds.  The validator uses this to prove that sibling stringify
  /// surfaces are preserved under the candidate root actuals.
  std::optional<std::string> ReplayStringifyArgumentThroughRoot(
      const RefoldModel::MacroInvocation &inv, uint32_t argIdx,
      llvm::ArrayRef<std::string> rootActuals) const;

  Dependencies deps_;
};

} // namespace refold
} // namespace clang

#endif // LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDMACROSUBTREEREPLAYVALIDATOR_H
