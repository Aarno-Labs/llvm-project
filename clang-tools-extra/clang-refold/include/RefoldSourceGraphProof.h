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
  const RefoldModel &model;
  llvm::StringRef tuPath;
  const llvm::DenseMap<uint64_t, std::string> &includeExpansion;
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
  const RefoldPathIdentity &paths;

  /// Return true when the include has producer-proven source line-control state
  /// whose macro operands are supplied by the immediate includer.
  llvm::function_ref<bool(const RefoldModel::IncludeItem &)>
      includeHasIncluderSuppliedLineControlMacroState;

  /// Return true when this include subtree carries an include-site-local layout
  /// materialization obligation that a path-level sidecar cannot discharge.
  llvm::function_ref<bool(uint64_t)>
      includeSubtreeHasLayoutOnlyMaterializationSeed;
};

/// Decision returned by source-graph owner preservation planning.
struct SourceGraphOwnerPreservationPlan {
  /// Relative sidecar path to emit when the include may remain a source-graph
  /// owner.  Empty means the caller must use the normal materialization path.
  std::optional<std::string> preservedRelativePath;

  /// Relative sidecar path that the driver may clean up if it still contains
  /// exactly the rejected bytes.  This is only bookkeeping for stale sidecars;
  /// the proof module does not delete or write files.
  std::optional<std::string> rejectedCleanupRelativePath;
};

/// Ready-to-append output records derived from a source-graph preservation
/// decision.  This keeps carrier construction policy with the source-graph
/// proof layer while leaving ownership of destination vectors and filesystem
/// writes with the caller and RefoldSourceGraphWriter.
struct SourceGraphOwnerPreservationOutputPlan {
  std::optional<SourceGraphOutput> preservedOutput;
  std::optional<SourceGraphOutput> rejectedCleanupOutput;
};

/// Return the quoted include operand as a safe relative path for automatic
/// source-graph side output.
///
/// The automatic source-graph backend writes files beside `--out` using the
/// original quoted include spelling.  That is only safe for simple relative
/// include names.  Angle includes, absolute paths, backslashes, quotes, empty
/// components, and `.`/`..` components stay in the normal single-output
/// materialization path.
std::optional<std::string>
safeSourceGraphRelativeIncludePath(const RefoldModel::IncludeItem &include);

/// True when `text` contains a preprocessing directive introducer after applying
/// the small amount of preprocessing needed for directive recognition by this
/// proof layer: escaped-newline deletion and comment-as-whitespace treatment.
///
/// This helper recognizes the directive introducer only; it does not parse or
/// validate the directive body.  Source-graph alias proof uses it to find
/// include-looking directives inside already-materialized owner bytes.
bool lineHasPreprocessingDirectiveIntroducer(llvm::StringRef text);

/// Classify whether materialized include-owner bytes contain a surviving
/// preprocessing include directive that could observe a generated source-graph
/// sidecar written under `sourceGraphPath`.
///
/// A generated source-graph header is a path-level edit in the final replay
/// surface.  If some other include owner is materialized into the TU and its
/// materialized text still contains `#include "same/path.h"`, that nested
/// include is no longer resolved relative to the original header's directory;
/// it is replayed from the refolded TU output directory and will observe the
/// sidecar.  Non-literal include operands cannot be proven not to name the
/// sidecar, so the proof fails closed for them as well.
MaterializedIncludeReplayAlias
classifyMaterializedIncludeReplayAlias(llvm::StringRef materializedText,
                                       llvm::StringRef sourceGraphPath);

/// Return true when preserving `include` as a source-graph sidecar under
/// `sourceGraphPath` cannot conflict with another top-level same-path include or
/// with a surviving include directive inside materialized owner bytes.
///
/// This is an alias-safety proof, not a path-normalization heuristic.  It must
/// reject preservation whenever a same-spelling top-level include names a
/// different producer file or whenever materialized text may replay an include
/// that observes the generated sidecar.
bool sourceGraphIncludePathIsUnaliasedOrCoherent(
    const SourceGraphProofInputs &inputs,
    const RefoldModel::IncludeItem &include, llvm::StringRef sourceGraphPath,
    llvm::StringRef candidateBytes, const SourceGraphProofServices &services);

/// Build a source-graph output carrier for `include` without mutating the
/// caller's output vector.
SourceGraphOutput makeSourceGraphOutput(const RefoldModel::IncludeItem &include,
                                        llvm::StringRef relativePath,
                                        llvm::StringRef bytes,
                                        bool cleanupOnly = false);

/// Decide whether `include` may remain a source-graph owner.  The returned plan
/// contains either an emitted sidecar path, a cleanup-only rejected sidecar
/// path, or neither.  This is a pure planning helper; the caller owns all state
/// mutation and filesystem policy.
SourceGraphOwnerPreservationPlan
planSourceGraphOwnerPreservation(const SourceGraphProofInputs &inputs,
                                 const RefoldModel::IncludeItem &include,
                                 llvm::StringRef candidateBytes,
                                 const SourceGraphProofServices &services);

/// Decide whether `include` may remain a source-graph owner and construct the
/// corresponding output carriers.  The helper is still side-effect-free: it
/// does not append to RefoldEngine storage and does not write or delete files.
SourceGraphOwnerPreservationOutputPlan planSourceGraphOwnerPreservationOutput(
    const SourceGraphProofInputs &inputs,
    const RefoldModel::IncludeItem &include, llvm::StringRef candidateBytes,
    const SourceGraphProofServices &services);

} // namespace source_graph
} // namespace refold
} // namespace clang

#endif // LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDSOURCEGRAPHPROOF_H
