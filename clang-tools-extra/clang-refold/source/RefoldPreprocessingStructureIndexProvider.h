//===--- RefoldPreprocessingStructureIndexProvider.h ------------*- C++ -*-===//
//
// Provides shared, read-only access to preprocessing-structure indexes for the
// translation unit and concrete header inclusion occurrences.
//
// Header source buffers are loaded lazily and cached by canonical physical path,
// while occurrence-local indexes remain keyed by both path and include identity
// so repeated inclusions never share producer bindings.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_STRUCTUREINDEXPROVIDER_H
#define LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_STRUCTUREINDEXPROVIDER_H

#include "llvm/ADT/StringRef.h"

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>

namespace llvm {
class MemoryBuffer;
}

namespace clang {
class LangOptions;

namespace refold {

class LineDirectiveInserter;
class RefoldMacroStateProof;
class RefoldModel;
class RefoldPathIdentity;
class RefoldPreprocessingStructureIndex;

/// Read-only provider for exact physical preprocessing-structure indexes.
///
/// The engine-owned translation-unit index is returned directly. Header indexes
/// are built lazily and cached by canonical physical path plus concrete include
/// occurrence, so repeated visits share source bytes but never share producer
/// bindings across different include instances. A missing source file produces
/// an explicit incomplete-evidence result; no approximate index or boundary is
/// manufactured.
class RefoldPreprocessingStructureIndexProvider {
public:
  /// Borrowed services required to build occurrence-local header indexes.
  struct Dependencies {
    const RefoldModel &model;
    const RefoldPathIdentity &pathIdentity;
    const LineDirectiveInserter &lineDirs;
    const RefoldMacroStateProof &macroStateProof;
    const clang::LangOptions &lexLang;
  };

  /// Result of one exact source-owner index lookup.
  ///
  /// String views refer to provider-owned cache entries and remain valid for
  /// the provider lifetime. `index == nullptr` means the physical source could
  /// not be loaded; `incompleteEvidenceReason` then describes that exact
  /// failure.
  struct LookupResult {
    const RefoldPreprocessingStructureIndex *index = nullptr;
    llvm::StringRef physicalSourcePath;
    llvm::StringRef incompleteEvidenceReason;

    bool HasIndex() const { return index != nullptr; }
  };

  RefoldPreprocessingStructureIndexProvider(
      Dependencies deps, llvm::StringRef tuPath,
      const RefoldPreprocessingStructureIndex &tuIndex);
  ~RefoldPreprocessingStructureIndexProvider();

  RefoldPreprocessingStructureIndexProvider(
      const RefoldPreprocessingStructureIndexProvider &) = delete;
  RefoldPreprocessingStructureIndexProvider &operator=(
      const RefoldPreprocessingStructureIndexProvider &) = delete;

  /// Return the immutable engine-owned translation-unit structure index.
  const RefoldPreprocessingStructureIndex &GetTUIndex() const {
    return tuIndex_;
  }

  /// Return the exact index for one physical source-owner occurrence.
  ///
  /// Header source bytes are loaded once per canonical physical path. Indexes
  /// are cached independently per `(physical path, owner include id)` pair.
  LookupResult
  Get(llvm::StringRef sourcePath,
      std::optional<uint64_t> ownerIncludeId = std::nullopt) const;

private:
  using OccurrenceKey =
      std::pair<std::string, std::optional<uint64_t>>;

  struct SourceBufferCacheEntry {
    std::unique_ptr<llvm::MemoryBuffer> buffer;
    std::string incompleteEvidenceReason;
  };

  /// Return the canonical physical path used by both caches.
  std::string CanonicalPhysicalPath(llvm::StringRef sourcePath) const;

  Dependencies deps_;
  const RefoldPreprocessingStructureIndex &tuIndex_;
  std::string tuPhysicalSourcePath_;

  mutable std::map<std::string, SourceBufferCacheEntry> sourceBufferCache_;
  mutable std::map<OccurrenceKey,
                   std::unique_ptr<RefoldPreprocessingStructureIndex>>
      occurrenceIndexCache_;
};

} // namespace refold
} // namespace clang

#endif // LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_STRUCTUREINDEXPROVIDER_H
