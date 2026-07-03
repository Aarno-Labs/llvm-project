//===--- RefoldSourceGraphWriter.h ---------------------------*- C++ -*-===//
//
// Source-graph filesystem output writer for clang-refold.
//
// This module performs only the I/O side of source-graph refolding: validating
// already-planned relative output paths, removing stale generated sidecars when
// byte-exact cleanup proof records permit it, and writing generated sidecar
// files.  It does not decide whether an include may be preserved as a
// source-graph owner; that proof/planning work belongs in
// RefoldSourceGraphProof.*.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDSOURCEGRAPHWRITER_H
#define LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDSOURCEGRAPHWRITER_H

#include "include/RefoldSourceGraphProof.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"

#include <string>

namespace clang {
namespace refold {

/// Filesystem options for writing already-decided source-graph sidecars.
///
/// OutputDirectory is the directory that contains the emitted `.c.mod` file;
/// source-graph relative paths are resolved beneath it, matching quoted include
/// lookup from the final refolded source.  Verbose is reserved for caller-side
/// reporting controls; diagnostics use the shared RefoldLog channels.
struct SourceGraphWriteOptions {
  std::string outputDirectory;
  bool verbose = false;
};

/// Build writer options from the final refolded source path.
///
/// Sidecars are emitted relative to the directory containing `--out`, and a
/// basename-only output path writes sidecars relative to `.`. Keeping this
/// path policy beside the writer keeps source-graph I/O localized.
SourceGraphWriteOptions makeSourceGraphWriteOptionsForModifiedSourcePath(
    llvm::StringRef ModifiedSrcPath);

/// Write source-graph sidecar files and remove byte-exact stale sidecars.
///
/// The output records must already be the result of source-graph proof
/// planning and include-materialization scheduling.  This function performs no
/// proof decisions and never calls back into planning services; it only
/// validates paths and applies the requested filesystem writes/cleanup with the
/// existing fail-closed logging behavior.  Cleanup records are honored only
/// when the on-disk sidecar still matches the recorded bytes exactly.
void writeSourceGraphOutputs(llvm::ArrayRef<SourceGraphOutput> Outputs,
                             const SourceGraphWriteOptions &Options);

} // namespace refold
} // namespace clang

#endif // LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDSOURCEGRAPHWRITER_H
