//===--- RefoldPathIdentity.cpp ---------------------------------*- C++ -*-===//
//
// Path canonicalization and include-edge identity proof implementation.
//
//===----------------------------------------------------------------------===//

#include "util/RefoldPathIdentity.h"

#include "util/RefoldPathCanonicalization.h"

#include "proof/RefoldProofVocabulary.h"

#include <filesystem>
#include <system_error>

using namespace llvm;

namespace clang {
namespace refold {

RefoldPathIdentity::RefoldPathIdentity(const RefoldModel & /*model*/,
                                       StringRef /*originalWorkingDirectory*/,
                                       bool /*emitAbsPaths*/) {
  // The path-identity predicates are limited to canonical path comparison and
  // include-edge metadata, neither of which needs per-run state.  The
  // constructor keeps the service's dependency shape without holding any.
}

void RefoldPathIdentity::CacheCanonicalPath(StringRef path) const {
  (void)refoldCanonicalPath(path);
}

StringRef RefoldPathIdentity::GetCanonicalPath(StringRef path) const {
  return refoldCanonicalPath(path);
}

bool RefoldPathIdentity::PathsEqual(StringRef lhs, StringRef rhs) const {
  return refoldPathsEqual(lhs, rhs);
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
