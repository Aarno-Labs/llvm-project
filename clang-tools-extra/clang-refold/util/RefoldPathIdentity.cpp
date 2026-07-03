//===--- RefoldPathIdentity.cpp --------------------------------*- C++ -*-===//
//
// Path canonicalization and include-edge identity proof implementation.
//
//===----------------------------------------------------------------------===//

#include "util/RefoldPathIdentity.h"
#include "core/RefoldLog.h"
#include "proof/RefoldProofVocabulary.h"

#include <filesystem>
#include <system_error>

using namespace llvm;

namespace clang {
namespace refold {

RefoldPathIdentity::RefoldPathIdentity(const RefoldModel &model,
                                       StringRef originalWorkingDirectory,
                                       bool emitAbsPaths) {
  // The current path-identity predicates are intentionally limited to
  // canonical path comparison and include-edge metadata.  Keep the constructor
  // shape aligned with the future service boundary without introducing unused
  // member state or changing behavior.
  (void)model;
  (void)originalWorkingDirectory;
  (void)emitAbsPaths;
}

void RefoldPathIdentity::CacheCanonicalPath(StringRef path) const {
  auto it = canonicalPathCache_.find(path);
  if (it != canonicalPathCache_.end())
    return;

  std::error_code ec;
  const auto canonical =
      std::filesystem::weakly_canonical(std::filesystem::path(path.str()), ec);
  if (ec) {
    REFOLD_LOG_FATAL("path/canon", "failed to canonicalize '{0}': {1}", path,
                     ec.message());
  }

  canonicalPathCache_.insert({path, canonical.string()});
}

bool RefoldPathIdentity::PathsEqual(StringRef lhs, StringRef rhs) const {
  if (lhs.empty() || rhs.empty())
    return lhs == rhs;

  CacheCanonicalPath(lhs);
  CacheCanonicalPath(rhs);

  return canonicalPathCache_.find(lhs)->getValue() ==
         canonicalPathCache_.find(rhs)->getValue();
}

std::optional<std::string> RefoldPathIdentity::ProducerPhysicalIncludePath(
    const RefoldModel::IncludeItem &include) const {
  std::optional<std::filesystem::path> producerPath =
      ::clang::refold::producerPhysicalIncludePath(include);
  if (!producerPath)
    return std::nullopt;
  return producerPath->string();
}

std::optional<std::string> RefoldPathIdentity::ProducerEnteredFileSpelling(
    const RefoldModel::IncludeItem &include) const {
  StringRef producerSpelling =
      ::clang::refold::producerEnteredFileSpelling(include);
  if (producerSpelling.empty())
    return std::nullopt;
  return producerSpelling.str();
}

bool RefoldPathIdentity::SamePhysicalIncludeFile(
    StringRef candidatePath, const RefoldModel::IncludeItem &include) const {
  std::optional<std::string> producerPath =
      ProducerPhysicalIncludePath(include);
  if (!producerPath)
    return candidatePath.empty();

  return PathsEqual(candidatePath, *producerPath);
}

bool RefoldPathIdentity::SameEnteredFileSpelling(
    StringRef candidateSpelling,
    const RefoldModel::IncludeItem &include) const {
  std::optional<std::string> producerSpelling =
      ProducerEnteredFileSpelling(include);
  if (!producerSpelling)
    return candidateSpelling.empty();

  return candidateSpelling == StringRef(*producerSpelling);
}

} // namespace refold
} // namespace clang
