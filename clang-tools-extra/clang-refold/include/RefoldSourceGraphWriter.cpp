//===--- RefoldSourceGraphWriter.cpp -------------------------*- C++ -*-===//
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

static bool pathSpellingMatchesAfterAbsolute(llvm::StringRef A,
                                             llvm::StringRef B) {
  if (A.empty() || B.empty())
    return false;

  llvm::SmallString<256> AbsA(A);
  llvm::SmallString<256> AbsB(B);
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
static bool validateSourceGraphRelativeOutputPath(llvm::StringRef Rel) {
  llvm::SmallVector<llvm::StringRef, 8> Components;
  Rel.split(Components, '/', /*MaxSplit=*/-1, /*KeepEmpty=*/true);
  const bool HasUnsafeComponent = llvm::any_of(
      Components, [](llvm::StringRef C) {
        return C.empty() || C == "." || C == "..";
      });
  return !Rel.empty() && !llvm::sys::path::is_absolute(Rel) &&
         !HasUnsafeComponent && !Rel.contains('\\') && !Rel.contains('"');
}

} // namespace

SourceGraphWriteOptions
makeSourceGraphWriteOptionsForModifiedSourcePath(
    llvm::StringRef ModifiedSrcPath) {
  llvm::SmallString<256> OutputDir(ModifiedSrcPath);
  llvm::sys::path::remove_filename(OutputDir);
  if (OutputDir.empty())
    OutputDir = ".";

  SourceGraphWriteOptions Options;
  Options.OutputDirectory = OutputDir.str().str();
  return Options;
}

void writeSourceGraphOutputs(llvm::ArrayRef<SourceGraphOutput> Outputs,
                             const SourceGraphWriteOptions &Options) {
  llvm::StringRef OutputDir = Options.OutputDirectory.empty()
                                  ? llvm::StringRef(".")
                                  : llvm::StringRef(Options.OutputDirectory);

  std::map<std::string, SourceGraphOutput> CleanupOutputs;
  std::map<std::string, std::string> UniqueOutputs;
  for (const SourceGraphOutput &Output : Outputs) {
    llvm::StringRef Rel(Output.relativePath);
    if (!validateSourceGraphRelativeOutputPath(Rel))
      REFOLD_LOG_FATAL("source-graph/write",
            "refusing unsafe source-graph output path: {0}",
            Output.relativePath);

    if (Output.cleanupOnly) {
      // Multiple rejected include sites can point at the same stale sidecar.
      // Keeping the first candidate is enough: cleanup is byte-exact, so a
      // nonmatching file is left alone rather than guessed about.
      CleanupOutputs.insert({Output.relativePath, Output});
      continue;
    }

    auto [It, Inserted] =
        UniqueOutputs.insert({Output.relativePath, Output.bytes});
    if (!Inserted && It->second != Output.bytes)
      REFOLD_LOG_FATAL("source-graph/write",
            "conflicting source-graph contents for path: {0}",
            Output.relativePath);
  }

  for (const auto &Entry : CleanupOutputs) {
    if (UniqueOutputs.count(Entry.first))
      continue;

    const SourceGraphOutput &Cleanup = Entry.second;
    llvm::SmallString<256> Path(OutputDir);
    llvm::sys::path::append(Path, Entry.first);

    if (!Cleanup.resolvedPath.empty() &&
        pathSpellingMatchesAfterAbsolute(Path, Cleanup.resolvedPath)) {
      REFOLD_LOG_DEBUG("source-graph/write",
            "skip stale cleanup for {0}: output path names producer header {1}",
            Path, Cleanup.resolvedPath);
      continue;
    }

    auto ExistingOrErr = llvm::MemoryBuffer::getFile(Path);
    if (!ExistingOrErr)
      continue;

    if ((*ExistingOrErr)->getBuffer() != Cleanup.bytes) {
      REFOLD_LOG_DEBUG("source-graph/write",
            "leave possible stale source-graph file {0}: bytes no longer match "
            "rejected generated body for include #{1}",
            Path, Cleanup.includeId);
      continue;
    }

    if (std::error_code EC = llvm::sys::fs::remove(Path))
      REFOLD_LOG_FATAL("source-graph/write",
            "cannot remove stale source-graph file {0}: {1}", Path,
            EC.message());
    REFOLD_LOG_INFO("finished", "removed stale source-graph header: {0}", Path);
  }

  for (const auto &Entry : UniqueOutputs) {
    llvm::SmallString<256> Path(OutputDir);
    llvm::sys::path::append(Path, Entry.first);

    if (auto ExistingOrErr = llvm::MemoryBuffer::getFile(Path)) {
      if ((*ExistingOrErr)->getBuffer() != Entry.second)
        REFOLD_LOG_FATAL("source-graph/write",
              "refusing to overwrite existing different source-graph file: {0}",
              Path);
      REFOLD_LOG_INFO("finished",
                      "source-graph header already up to date: {0}", Path);
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
    OS << Entry.second;
    OS.close();
    REFOLD_LOG_INFO("finished", "wrote source-graph header: {0}", Path);
  }
}

} // namespace refold
} // namespace clang
