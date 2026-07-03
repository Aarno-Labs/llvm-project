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

#include "core/RefoldModel.h"

#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/StringRef.h"

#include <optional>
#include <string>

namespace clang {
namespace refold {

/// Read-only path identity oracle for one refold run.
///
/// The canonical-path cache is intentionally owned by this service rather than
/// by RefoldEngine.  Proof and planning services receive this oracle directly
/// when they need canonical path comparison or include-edge identity checks.
class RefoldPathIdentity {
public:
  RefoldPathIdentity(const RefoldModel &model,
                     llvm::StringRef originalWorkingDirectory,
                     bool emitAbsPaths);

  /// Ensure that \p path has a cached weakly-canonical spelling.
  ///
  /// Canonicalization failures are fatal, matching the path-identity
  /// comparison policy used by `PathsEqual()`.
  void CacheCanonicalPath(llvm::StringRef path) const;

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
  /// ProducerEnteredFileSpelling() / SameEnteredFileSpelling() instead.
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

  /// Compare a candidate entered-file spelling against the producer spelling
  /// observed by __FILE__ / __FILE_NAME__-sensitive proofs.
  bool SameEnteredFileSpelling(llvm::StringRef candidateSpelling,
                               const RefoldModel::IncludeItem &include) const;

private:
  mutable llvm::StringMap<std::string> canonicalPathCache_;
};

} // namespace refold
} // namespace clang

#endif // LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDPATHIDENTITY_H
