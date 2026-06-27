//===--- RefoldEngine.PathIdentity.cpp ---------------------------*- C++ -*-===//
//
// Path canonicalization and physical include identity helpers for RefoldEngine.
// These members remain engine-owned; the split only removes their dependency on
// the old TailUtilities text include.
//
//===----------------------------------------------------------------------===//

#include "RefoldEngine.h"
#include "RefoldLog.h"
#include "RefoldIncludePathProof.h"

#include <filesystem>
#include <optional>
#include <string>
#include <system_error>

using namespace llvm;

namespace clang {
namespace refold {

void RefoldEngine::CacheCanonicalPath(StringRef path) const {
  auto it = canonicalPathCache_.find(path);
  if (it != canonicalPathCache_.end())
    return;

  std::error_code ec;
  const auto canonical = std::filesystem::weakly_canonical(
      std::filesystem::path(path.str()), ec);
  if (ec) {
    REFOLD_LOG_FATAL("path/canon", "failed to canonicalize '{0}': {1}", path,
          ec.message());
  }

  canonicalPathCache_.insert({path, canonical.string()});
}

bool RefoldEngine::PathsEqual(StringRef a, StringRef b) const {
  if (a.empty() || b.empty())
    return a == b;

  CacheCanonicalPath(a);
  CacheCanonicalPath(b);

  return canonicalPathCache_.find(a)->getValue() ==
         canonicalPathCache_.find(b)->getValue();
}

bool RefoldEngine::samePhysicalIncludeFile(
    StringRef candidatePath, const RefoldModel::IncludeItem &include) const {
  std::optional<std::filesystem::path> producerPath =
      producerPhysicalIncludePath(include);
  if (!producerPath)
    return candidatePath.empty();

  const std::string producerPathSpelling = producerPath->string();
  return PathsEqual(candidatePath, producerPathSpelling);
}

} // namespace refold
} // namespace clang
