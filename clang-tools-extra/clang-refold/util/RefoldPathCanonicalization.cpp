//===--- RefoldPathCanonicalization.cpp ------------------------*- C++ -*-===//
//
// Filesystem path identity primitive for clang-refold.
//
//===----------------------------------------------------------------------===//

#include "util/RefoldPathCanonicalization.h"

#include "core/RefoldLog.h"

#include "llvm/ADT/StringMap.h"

#include <filesystem>
#include <system_error>

using namespace llvm;

namespace clang {
namespace refold {

std::string refoldWeaklyCanonicalPath(StringRef path) {
  std::error_code ec;
  const auto canonical =
      std::filesystem::weakly_canonical(std::filesystem::path(path.str()), ec);
  if (ec) {
    REFOLD_LOG_FATAL("path/canon", "failed to canonicalize '{0}': {1}", path,
                     ec.message());
  }
  return canonical.string();
}

StringRef refoldCanonicalPath(StringRef path) {
  if (path.empty())
    return {};

  static StringMap<std::string> canonicalCache;
  auto it = canonicalCache.find(path);
  if (it == canonicalCache.end())
    it = canonicalCache.insert({path, refoldWeaklyCanonicalPath(path)}).first;
  return it->getValue();
}

bool refoldPathsEqual(StringRef lhs, StringRef rhs) {
  if (lhs.empty() || rhs.empty())
    return lhs == rhs;
  // Identical spellings need no filesystem access.
  if (lhs == rhs)
    return true;

  return refoldCanonicalPath(lhs) == refoldCanonicalPath(rhs);
}

} // namespace refold
} // namespace clang
