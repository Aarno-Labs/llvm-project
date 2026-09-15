//===--- RefoldPathIdentity.h ----------------------------------*- C++ -*-===//
//
// Path identity service for clang-refold.
//
// This service owns filesystem path canonicalization and include-edge identity
// predicates.  It deliberately separates physical file identity from
// producer-observed filename spelling: physical checks canonicalize filesystem
// paths, while spelling checks compare the exact entered spelling recorded by
// the producer metadata / legacy compatibility fallback.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDPATHIDENTITY_H
#define LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDPATHIDENTITY_H

#include "model/RefoldModel.h"
#include "support/StringUtils.h"

#include "llvm/ADT/StringRef.h"

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
inline llvm::StringRef
explicitProducerEnteredFileSpelling(const RefoldModel::IncludeItem &include) {
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
/// physical proof path, e.g. through RefoldPathIdentity::PathsEqual().  Never
/// use entered_file_spelling here: observer spelling and filesystem identity
/// are intentionally separate proof domains.
inline std::optional<std::filesystem::path>
producerPhysicalIncludePath(const RefoldModel::IncludeItem &include) {
  llvm::StringRef path = include.openedPath
                             ? *include.openedPath
                             : legacyResolvedIncludePath(include);
  if (path.empty())
    return std::nullopt;
  return std::filesystem::path(path.str());
}

/// Read-only path identity oracle for one refold run.
///
/// Proof and planning services receive this oracle directly when they need
/// canonical path comparison or include-edge identity checks.  Canonicalization
/// itself is answered from the one process-wide cache in
/// `support/RefoldPathCanonicalization.h`, which layers below this one -- the model
/// among them -- also use, so a path is resolved once no matter who asks.
class RefoldPathIdentity {
public:
  RefoldPathIdentity(const RefoldModel &model,
                     llvm::StringRef originalWorkingDirectory,
                     bool emitAbsPaths);

  /// Return the cached weakly-canonical spelling for one non-empty path.
  ///
  /// The returned view remains valid for the lifetime of the process. Empty
  /// paths produce an empty view; canonicalization failures are fatal, matching
  /// the physical-identity policy used by `PathsEqual()`.
  llvm::StringRef GetCanonicalPath(llvm::StringRef path) const;

  /// Compare two paths for equality after weak canonicalization.
  ///
  /// Empty paths are compared by spelling.  Non-empty paths are canonicalized
  /// with std::filesystem::weakly_canonical() and compared by canonical
  /// spelling, preserving the refolder's fail-closed path identity policy.
  bool PathsEqual(llvm::StringRef lhs, llvm::StringRef rhs) const;

  /// Return the producer-side path spelling used for physical include identity.
  ///
  /// New maps use opened_path.  Legacy maps fall back to resolved_path only for
  /// this physical proof path; filename-observer checks must use
  /// ProducerEnteredFileSpelling() instead.
  std::optional<std::string>
  ProducerPhysicalIncludePath(const RefoldModel::IncludeItem &include) const;

  /// Return the best available producer-entered filename spelling.
  ///
  /// This is the spelling observed by preserved __FILE__ sites.  It prefers
  /// entered_file_spelling and falls back to legacy resolved_path only for maps
  /// that predate the split physical/spelling metadata.
  std::optional<std::string>
  ProducerEnteredFileSpelling(const RefoldModel::IncludeItem &include) const;

  /// Compare a candidate include replay path against the producer include edge
  /// by physical identity.
  bool SamePhysicalIncludeFile(llvm::StringRef candidatePath,
                               const RefoldModel::IncludeItem &include) const;

};

} // namespace refold
} // namespace clang

#endif // LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDPATHIDENTITY_H
