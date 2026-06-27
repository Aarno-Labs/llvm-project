//===--- RefoldSourceGraphTypes.h ---------------------------*- C++ -*-===//
//
// Source-graph carrier types for clang-refold.
//
// This header intentionally contains only POD-like data records shared across
// the engine, proof/planning, CLI, and filesystem writer layers.  It owns no
// source-graph policy, performs no filesystem I/O, emits no edits, and mutates
// no RefoldEngine state.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDSOURCEGRAPHTYPES_H
#define LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDSOURCEGRAPHTYPES_H

#include <cstdint>
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

} // namespace refold
} // namespace clang

#endif // LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDSOURCEGRAPHTYPES_H
