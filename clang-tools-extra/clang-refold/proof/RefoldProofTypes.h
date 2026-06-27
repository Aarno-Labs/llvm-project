//===--- RefoldProofTypes.h ------------------------------------*- C++ -*-===//
//
// Shared proof vocabulary for clang-refold.
//
// This header contains value-only theorem/proof carrier types that are shared
// across proof modules.  It must stay free of RefoldEngine orchestration state:
// no edit emission, fallback requests, materialization recursion, or mutable
// engine caches belong here.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDPROOFTYPES_H
#define LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDPROOFTYPES_H

#include "core/RefoldModel.h"
#include "util/StringUtils.h"

#include "llvm/ADT/StringRef.h"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

namespace clang {
namespace refold {

/// Return the explicit producer-entered filename spelling for an include edge.
///
/// New-schema maps carry this exact `__FILE__` entry spelling in
/// entered_file_spelling.  This helper intentionally does not fall back to
/// resolved_path: file-spelling proofs need to distinguish an explicit producer
/// fact from legacy spelling data and from independently recovered observer or
/// replay witnesses.
inline llvm::StringRef explicitProducerEnteredFileSpelling(
    const RefoldModel::IncludeItem &include) {
  return include.enteredFileSpelling ? *include.enteredFileSpelling
                                     : llvm::StringRef();
}

/// Return the legacy spelling-oriented include path, if present.
///
/// Older maps only had resolved_path, whose meaning drifted between physical
/// identity and entered-file spelling.  New proof code should consult this
/// helper only after exhausting stronger new-schema metadata and recovered
/// observer/replay witnesses.
inline llvm::StringRef
legacyResolvedIncludePath(const RefoldModel::IncludeItem &include) {
  return include.resolvedPath ? *include.resolvedPath : llvm::StringRef();
}

/// Return the best available producer-entered filename spelling for an include.
///
/// This is a convenience for legacy-neutral callers that only need a spelling
/// anchor.  Proof-sensitive code that implements the full file-spelling
/// fallback hierarchy should prefer explicitProducerEnteredFileSpelling(), then
/// any context-specific observer/replay witnesses, and only then
/// legacyResolvedIncludePath().
inline llvm::StringRef
producerEnteredFileSpelling(const RefoldModel::IncludeItem &include) {
  llvm::StringRef explicitSpelling =
      explicitProducerEnteredFileSpelling(include);
  return !explicitSpelling.empty() ? explicitSpelling
                                   : legacyResolvedIncludePath(include);
}

/// Return the exact producer `__FILE_NAME__` entry spelling when available.
///
/// entered_file_name is emitted by the producer using Clang's own
/// processPathToFileName() logic.  If an old/new map lacks it, fall back to the
/// deterministic refolder basename helper over entered_file_spelling / legacy
/// resolved_path.  This fallback is compatibility-only; new maps should carry
/// entered_file_name whenever the include was actually entered.
inline llvm::StringRef
producerEnteredFileName(const RefoldModel::IncludeItem &include) {
  if (include.enteredFileName)
    return *include.enteredFileName;
  llvm::StringRef fileSpelling = producerEnteredFileSpelling(include);
  return fileSpelling.empty() ? llvm::StringRef()
                              : stringutils::pathBasename(fileSpelling);
}

/// Return the producer-side path spelling used as input to physical identity.
///
/// New maps carry opened_path for physical/FileEntry identity.  Legacy maps
/// fall back to resolved_path, and callers must canonicalize only inside the
/// physical proof path, e.g. through RefoldPathIdentity::PathsEqual().  Never use
/// entered_file_spelling here: observer spelling and filesystem identity are
/// intentionally separate proof domains.
inline std::optional<std::filesystem::path>
producerPhysicalIncludePath(const RefoldModel::IncludeItem &include) {
  llvm::StringRef path = include.openedPath ? *include.openedPath
                                            : legacyResolvedIncludePath(include);
  if (path.empty())
    return std::nullopt;
  return std::filesystem::path(path.str());
}

/// Proof-audit mode for the witness resolver.
///
/// Default derives from normal refolding mode: strict refolding enables
/// authoritative proof audit, while non-strict refolding leaves the resolver
/// off unless the caller explicitly requests probe or strict audit.
enum class ProofAuditMode : uint8_t { Default, Off, Probe, Strict };

/// Final source location used by include replay proofs.
///
/// This models the source file that the driver will write to `--out` and
/// subsequently pass to `--check`.  Direct quoted include lookup starts from
/// this directory.  It must not silently fall back to the producer TU path: the
/// producer source location and the emitted `.c.mod` location can differ, and
/// proving an operand from the wrong directory is precisely the class of
/// relocation bug this surface prevents.
struct FinalReplaySurface {
  std::filesystem::path outputPath;
  std::filesystem::path outputDirectory;
  std::filesystem::path originalWorkingDirectory;
  std::string outputDirectorySpelling;
};

/// Describes which components of the logical location are observed by preserved
/// location-sensitive builtins in an owner suffix.
///
/// The type is shared because extracted read-only proof modules exchange it
/// through service callbacks; the queries that compute it remain engine-owned.
struct LineStateObserverDemand {
  bool needsLine = false;
  bool needsFile = false;
  bool needsFileName = false;

  // True when at least one demand witness needs model metadata rather than
  // direct lexical final-source spelling.  This is construction metadata for
  // producing a repair obligation, not a late pruning veto.
  bool hasModelBackedLineStateDemand = false;

  bool Any() const { return needsLine || needsFile || needsFileName; }
  bool PrunableByCompactFinalLineControl() const { return Any(); }
};

} // namespace refold
} // namespace clang

#endif // LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDPROOFTYPES_H
