//===--- RefoldSourceGraphWriter.cpp ----------------------------*- C++ -*-===//
//
// Source-graph filesystem output writer for clang-refold.
//
//===----------------------------------------------------------------------===//

#include "include/RefoldSourceGraphWriter.h"

#include "core/RefoldLog.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/raw_ostream.h"

#include <map>
#include <system_error>

namespace clang {
namespace refold {

namespace {

static bool pathSpellingMatchesAfterAbsolute(llvm::StringRef a,
                                             llvm::StringRef b) {
  if (a.empty() || b.empty())
    return false;

  llvm::SmallString<256> AbsA(a);
  llvm::SmallString<256> AbsB(b);
  if (std::error_code EC = llvm::sys::fs::make_absolute(AbsA))
    return false;
  if (std::error_code EC = llvm::sys::fs::make_absolute(AbsB))
    return false;
  llvm::sys::path::remove_dots(AbsA, /*remove_dot_dot=*/true);
  llvm::sys::path::remove_dots(AbsB, /*remove_dot_dot=*/true);
  return AbsA == AbsB;
}

/// Validate a source-graph output path relative to the final `.c.mod`
/// directory.
///
/// This is intentionally only a writer-side filesystem containment check.  It
/// does not prove include replay identity, include search-chain equivalence, or
/// observer stability; those decisions are made earlier by the proof/planning
/// layer.  The writer rejects absolute paths, empty components, `.`/`..`, and
/// spellings that would escape or corrupt a quoted include operand.
static bool validateSourceGraphRelativeOutputPath(llvm::StringRef rel) {
  llvm::SmallVector<llvm::StringRef, 8> components;
  rel.split(components, '/', /*MaxSplit=*/-1, /*KeepEmpty=*/true);
  const bool hasUnsafeComponent =
      llvm::any_of(components, [](llvm::StringRef C) {
        return C.empty() || C == "." || C == "..";
      });
  return !rel.empty() && !llvm::sys::path::is_absolute(rel) &&
         !hasUnsafeComponent && !rel.contains('\\') && !rel.contains('"');
}

} // namespace

SourceGraphWriteOptions makeSourceGraphWriteOptionsForModifiedSourcePath(
    llvm::StringRef modifiedSrcPath) {
  llvm::SmallString<256> OutputDir(modifiedSrcPath);
  llvm::sys::path::remove_filename(OutputDir);
  if (OutputDir.empty())
    OutputDir = ".";

  SourceGraphWriteOptions options;
  options.outputDirectory = OutputDir.str().str();
  return options;
}

void writeSourceGraphOutputs(llvm::ArrayRef<SourceGraphOutput> outputs,
                             const SourceGraphWriteOptions &options) {
  llvm::StringRef OutputDir = options.outputDirectory.empty()
                                  ? llvm::StringRef(".")
                                  : llvm::StringRef(options.outputDirectory);

  std::map<std::string, SourceGraphOutput> cleanupOutputs;
  std::map<std::string, std::string> uniqueOutputs;
  for (const SourceGraphOutput &output : outputs) {
    llvm::StringRef Rel(output.relativePath);
    if (!validateSourceGraphRelativeOutputPath(Rel))
      REFOLD_LOG_FATAL("source-graph/write",
                       "refusing unsafe source-graph output path: {0}",
                       output.relativePath);

    if (output.cleanupOnly) {
      // Multiple rejected include sites can point at the same stale sidecar.
      // Keeping the first candidate is enough: cleanup is byte-exact, so a
      // nonmatching file is left alone rather than guessed about.
      cleanupOutputs.insert({output.relativePath, output});
      continue;
    }

    auto [It, Inserted] =
        uniqueOutputs.insert({output.relativePath, output.bytes});
    if (!Inserted && It->second != output.bytes)
      REFOLD_LOG_FATAL("source-graph/write",
                       "conflicting source-graph contents for path: {0}",
                       output.relativePath);
  }

  for (const auto &entry : cleanupOutputs) {
    if (uniqueOutputs.count(entry.first))
      continue;

    const SourceGraphOutput &cleanup = entry.second;
    llvm::SmallString<256> Path(OutputDir);
    llvm::sys::path::append(Path, entry.first);

    if (!cleanup.resolvedPath.empty() &&
        pathSpellingMatchesAfterAbsolute(Path, cleanup.resolvedPath)) {
      REFOLD_LOG_DEBUG(
          "source-graph/write",
          "skip stale cleanup for {0}: output path names producer header {1}",
          Path, cleanup.resolvedPath);
      continue;
    }

    auto existingOrErr = llvm::MemoryBuffer::getFile(Path);
    if (!existingOrErr)
      continue;

    if ((*existingOrErr)->getBuffer() != cleanup.bytes) {
      REFOLD_LOG_DEBUG(
          "source-graph/write",
          "leave possible stale source-graph file {0}: bytes no longer match "
          "rejected generated body for include #{1}",
          Path, cleanup.includeId);
      continue;
    }

    if (std::error_code EC = llvm::sys::fs::remove(Path))
      REFOLD_LOG_FATAL("source-graph/write",
                       "cannot remove stale source-graph file {0}: {1}", Path,
                       EC.message());
    REFOLD_LOG_INFO("finished", "removed stale source-graph header: {0}", Path);
  }

  for (const auto &entry : uniqueOutputs) {
    llvm::SmallString<256> Path(OutputDir);
    llvm::sys::path::append(Path, entry.first);

    if (auto existingOrErr = llvm::MemoryBuffer::getFile(Path)) {
      if ((*existingOrErr)->getBuffer() != entry.second)
        REFOLD_LOG_FATAL(
            "source-graph/write",
            "refusing to overwrite existing different source-graph file: {0}",
            Path);
      REFOLD_LOG_INFO("finished", "source-graph header already up to date: {0}",
                      Path);
      continue;
    }

    llvm::SmallString<256> Parent(Path);
    llvm::sys::path::remove_filename(Parent);
    if (std::error_code EC = llvm::sys::fs::create_directories(Parent))
      REFOLD_LOG_FATAL("source-graph/write", "cannot create {0}: {1}", Parent,
                       EC.message());

    std::error_code EC;
    llvm::raw_fd_ostream OS(Path, EC, llvm::sys::fs::OF_Text);
    if (EC)
      REFOLD_LOG_FATAL("source-graph/write", "cannot write {0}: {1}", Path,
                       EC.message());
    OS << entry.second;
    OS.close();
    REFOLD_LOG_INFO("finished", "wrote source-graph header: {0}", Path);
  }
}

} // namespace refold
} // namespace clang
