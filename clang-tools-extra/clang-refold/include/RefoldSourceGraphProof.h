//===--- RefoldSourceGraphProof.h -----------------------------*- C++ -*-===//
//
// Source-graph preservation proof helpers for clang-refold.
//
// This module decides whether a dirty include owner may remain represented as
// a source-graph sidecar instead of being materialized into the refolded TU.
// It is deliberately side-effect-free: it does not emit edits, mutate
// RefoldEngine state, or write filesystem outputs.  Callers remain responsible
// for acting on the returned plan.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDSOURCEGRAPHPROOF_H
#define LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDSOURCEGRAPHPROOF_H

#include "core/RefoldModel.h"
#include "util/RefoldPathIdentity.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/STLFunctionalExtras.h"
#include "llvm/ADT/StringRef.h"

#include <cstdint>
#include <optional>
#include <string>

namespace clang {
namespace refold {

/// One modified include owner emitted next to the refolded TU.
///
/// The normal clang-refold backend is a single-output backend: dirty include
/// owners are materialized into the TU.  A strictly narrower proof class can
/// preserve the include edge when the dirty header's active source `#line`
/// directive depends on macro state supplied by the immediate includer.  In
/// that case, materializing the header into the TU is token-sound but loses the
/// source-graph fact that the edit belongs to the header.  This side channel
/// writes the modified header bytes under the same quoted relative include path
/// beside the `.c.mod` output, so replay sees the edited owner while the TU
/// include spelling remains unchanged.
struct SourceGraphOutput {
  uint64_t includeId = 0;
  std::string relativePath;
  std::string originalTarget;
  std::string resolvedPath;
  std::string bytes;

  /// True when this entry is not an emitted sidecar, but a cleanup proof for a
  /// stale sidecar from an earlier run.  Source-graph selection is deliberately
  /// dynamic: a path that was admissible in one run can become inadmissible
  /// once another same-spelling include survives.  The driver may remove the
  /// stale file only if its bytes still exactly match \c bytes and it is not
  /// the producer-resolved input header.
  bool cleanupOnly = false;
};

namespace source_graph {

/// A surviving include directive found inside materialized owner bytes, as
/// classified for source-graph sidecar alias safety.
enum class MaterializedIncludeReplayAlias {
  /// No replayed include in the materialized text is relevant to the sidecar
  /// path.
  None,

  /// The materialized text contains a literal quoted include of exactly the
  /// sidecar path, so writing the sidecar would change that replay.
  SamePath,

  /// The materialized text contains an include directive whose operand cannot
  /// be proven to avoid the sidecar path.  Source-graph preservation must fail
  /// closed in this case.
  UnprovenInclude
};

/// Immutable source-graph facts consumed by the proof layer.
///
/// The proof module receives these as explicit inputs instead of reaching back
/// into RefoldEngine.  They describe the already-computed model and include
/// materialization bytes; they do not authorize mutation of source-graph output
/// records, edit maps, fallback state, or filesystem contents.
struct SourceGraphProofInputs {
  const RefoldModel &Model;
  llvm::StringRef TuPath;
  const llvm::DenseMap<uint64_t, std::string> &IncludeExpansion;
};

/// Read-only services supplied by RefoldEngine for policy decisions that still
/// depend on engine-local proof facts.  These callbacks must not mutate engine
/// state, emit edits, request fallbacks, recurse materialization, or write
/// source-graph outputs.
struct SourceGraphProofServices {
  /// Path identity service used for source-graph alias proofs.
  ///
  /// Passing the concrete service keeps this proof layer independent from
  /// RefoldEngine while avoiding one-off forwarding lambdas at each call site.
  const RefoldPathIdentity &Paths;

  /// Return true when the include has producer-proven source line-control state
  /// whose macro operands are supplied by the immediate includer.
  llvm::function_ref<bool(const RefoldModel::IncludeItem &)>
      IncludeHasIncluderSuppliedLineControlMacroState;

  /// Return true when this include subtree carries an include-site-local layout
  /// materialization obligation that a path-level sidecar cannot discharge.
  llvm::function_ref<bool(uint64_t)>
      IncludeSubtreeHasLayoutOnlyMaterializationSeed;
};

/// Decision returned by source-graph owner preservation planning.
struct SourceGraphOwnerPreservationPlan {
  /// Relative sidecar path to emit when the include may remain a source-graph
  /// owner.  Empty means the caller must use the normal materialization path.
  std::optional<std::string> PreservedRelativePath;

  /// Relative sidecar path that the driver may clean up if it still contains
  /// exactly the rejected bytes.  This is only bookkeeping for stale sidecars;
  /// the proof module does not delete or write files.
  std::optional<std::string> RejectedCleanupRelativePath;
};

/// Ready-to-append output records derived from a source-graph preservation
/// decision.  This keeps the carrier construction policy with the source-graph
/// proof layer while leaving ownership of the destination vector and filesystem
/// writes with RefoldEngine/RefoldSourceGraphWriter.
struct SourceGraphOwnerPreservationOutputPlan {
  std::optional<SourceGraphOutput> PreservedOutput;
  std::optional<SourceGraphOutput> RejectedCleanupOutput;
};

/// Return the quoted include operand as a safe relative path for automatic
/// source-graph side output, if the include spelling satisfies the narrow
/// no-escape path policy.
std::optional<std::string>
safeSourceGraphRelativeIncludePath(const RefoldModel::IncludeItem &Include);

/// True when Text contains a preprocessing directive introducer after applying
/// the small amount of preprocessing needed for directive recognition by this
/// proof layer: escaped-newline deletion and comment-as-whitespace treatment.
bool lineHasPreprocessingDirectiveIntroducer(llvm::StringRef Text);

/// Classify whether materialized include-owner bytes contain a surviving
/// preprocessing include directive that could observe a generated source-graph
/// sidecar written under SourceGraphPath.
MaterializedIncludeReplayAlias classifyMaterializedIncludeReplayAlias(
    llvm::StringRef MaterializedText, llvm::StringRef SourceGraphPath);

/// Return true when preserving Include as a source-graph sidecar under
/// SourceGraphPath cannot conflict with another top-level same-path include or
/// with a surviving include directive inside materialized owner bytes.
bool sourceGraphIncludePathIsUnaliasedOrCoherent(
    const SourceGraphProofInputs &Inputs,
    const RefoldModel::IncludeItem &Include,
    llvm::StringRef SourceGraphPath, llvm::StringRef CandidateBytes,
    const SourceGraphProofServices &Services);

/// Build a source-graph output carrier for Include without mutating the
/// caller's output vector.
SourceGraphOutput makeSourceGraphOutput(
    const RefoldModel::IncludeItem &Include, llvm::StringRef RelativePath,
    llvm::StringRef Bytes, bool CleanupOnly = false);

/// Decide whether Include may remain a source-graph owner.  The returned plan
/// contains either an emitted sidecar path, a cleanup-only rejected sidecar
/// path, or neither.  This is a pure planning helper; the caller owns all state
/// mutation and filesystem policy.
SourceGraphOwnerPreservationPlan planSourceGraphOwnerPreservation(
    const SourceGraphProofInputs &Inputs,
    const RefoldModel::IncludeItem &Include, llvm::StringRef CandidateBytes,
    const SourceGraphProofServices &Services);

/// Decide whether Include may remain a source-graph owner and construct the
/// corresponding output carriers.  The helper is still side-effect-free: it
/// does not append to RefoldEngine storage and does not write or delete files.
SourceGraphOwnerPreservationOutputPlan planSourceGraphOwnerPreservationOutput(
    const SourceGraphProofInputs &Inputs,
    const RefoldModel::IncludeItem &Include, llvm::StringRef CandidateBytes,
    const SourceGraphProofServices &Services);

} // namespace source_graph
} // namespace refold
} // namespace clang

#endif // LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDSOURCEGRAPHPROOF_H
