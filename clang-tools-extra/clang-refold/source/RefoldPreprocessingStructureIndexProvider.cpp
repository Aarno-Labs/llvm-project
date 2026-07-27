//===--- RefoldPreprocessingStructureIndexProvider.cpp ----------*- C++ -*-===//
//
// Provides shared, read-only access to preprocessing-structure indexes for the
// translation unit and concrete header inclusion occurrences.
//
// Header source buffers are loaded lazily and cached by canonical physical path,
// while occurrence-local indexes remain keyed by both path and include identity
// so repeated inclusions never share producer bindings.
//
//===----------------------------------------------------------------------===//

#include "source/RefoldPreprocessingStructureIndexProvider.h"

#include "line-control/LineDirectiveInserter.h"
#include "macro/RefoldMacroStateProof.h"
#include "source/RefoldPreprocessingStructureIndex.h"
#include "util/RefoldPathIdentity.h"

#include "llvm/Support/FormatVariadic.h"
#include "llvm/Support/MemoryBuffer.h"

#include <cassert>

using namespace llvm;

namespace clang {
namespace refold {

RefoldPreprocessingStructureIndexProvider::
    RefoldPreprocessingStructureIndexProvider(
        Dependencies deps, StringRef tuPath,
        const RefoldPreprocessingStructureIndex &tuIndex)
    : deps_(deps), tuIndex_(tuIndex),
      tuPhysicalSourcePath_(CanonicalPhysicalPath(tuPath)) {
  assert(!tuIndex_.GetOwnerIncludeId() &&
         "engine-owned TU structure index must not have an include owner");
  assert(deps_.pathIdentity.PathsEqual(tuIndex_.GetSourcePath(), tuPath) &&
         "engine-owned TU structure index must match the TU path");
}

RefoldPreprocessingStructureIndexProvider::
    ~RefoldPreprocessingStructureIndexProvider() = default;

std::string RefoldPreprocessingStructureIndexProvider::CanonicalPhysicalPath(
    StringRef sourcePath) const {
  if (sourcePath.empty())
    return {};
  const std::string absolutePath = deps_.lineDirs.ToAbsolutePath(sourcePath);
  return deps_.pathIdentity.GetCanonicalPath(absolutePath).str();
}

RefoldPreprocessingStructureIndexProvider::LookupResult
RefoldPreprocessingStructureIndexProvider::Get(
    StringRef sourcePath, std::optional<uint64_t> ownerIncludeId) const {
  if (sourcePath.empty()) {
    return {nullptr, {}, "physical source path is unavailable"};
  }

  std::string physicalSourcePath = CanonicalPhysicalPath(sourcePath);
  if (!ownerIncludeId && physicalSourcePath == tuPhysicalSourcePath_)
    return {&tuIndex_, tuPhysicalSourcePath_, {}};

  auto sourceInsertion = sourceBufferCache_.try_emplace(physicalSourcePath);
  SourceBufferCacheEntry &sourceEntry = sourceInsertion.first->second;
  if (sourceInsertion.second) {
    auto bufferOrError = MemoryBuffer::getFile(physicalSourcePath);
    if (!bufferOrError) {
      sourceEntry.incompleteEvidenceReason =
          formatv("unable to read physical source '{0}': {1}",
                  physicalSourcePath,
                  bufferOrError.getError().message())
              .str();
    } else {
      sourceEntry.buffer = std::move(*bufferOrError);
    }
  }

  const StringRef cachedPhysicalPath = sourceInsertion.first->first;
  if (!sourceEntry.buffer) {
    return {nullptr, cachedPhysicalPath,
            sourceEntry.incompleteEvidenceReason};
  }

  OccurrenceKey key{physicalSourcePath, ownerIncludeId};
  auto found = occurrenceIndexCache_.find(key);
  if (found == occurrenceIndexCache_.end()) {
    auto index = std::make_unique<RefoldPreprocessingStructureIndex>(
        RefoldPreprocessingStructureIndex::Build(
            RefoldPreprocessingStructureIndex::Dependencies{
                deps_.model, deps_.pathIdentity, deps_.macroStateProof,
                deps_.lexLang},
            cachedPhysicalPath, sourceEntry.buffer->getBuffer(),
            ownerIncludeId));
    found = occurrenceIndexCache_
                .emplace(std::move(key), std::move(index))
                .first;
  }

  return {found->second.get(), cachedPhysicalPath, {}};
}

} // namespace refold
} // namespace clang
