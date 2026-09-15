//===--- RefoldPathIdentity.cpp ---------------------------------*- C++ -*-===//
//
// Path canonicalization and include-edge identity proof implementation.
//
//===----------------------------------------------------------------------===//

#include "model/RefoldPathIdentity.h"

#include "support/RefoldPathCanonicalization.h"

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

} // namespace refold
} // namespace clang
