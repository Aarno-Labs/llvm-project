//===--- IncludeSpellingHelpers.h ------------------*- C++ -*-===//
//
// Private include-spelling helpers shared by RefoldEngine translation units
// after the TailUtilities split.  These helpers keep the logical filename
// spelling used for line-control/source-observer restoration separate from the
// physical path used to load materialized header bytes.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_INCLUDESPELLINGHELPERS_H
#define LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_INCLUDESPELLINGHELPERS_H

#include "RefoldProofTypes.h"
#include "StringUtils.h"

#include <filesystem>
#include <optional>
#include <string>

namespace clang {
namespace refold {

/// Return the logical filename spelling to restore for an include edge.
///
/// This is the spelling observed by preserved `__FILE__` / `__FILE_NAME__`
/// sites when line-control repair wraps a materialized include body.  Prefer
/// producer-entered spelling metadata and keep the old target-token fallback
/// only for legacy maps that lack split include metadata.
inline std::string
refoldIncludeEnteredFileSpelling(const RefoldModel::IncludeItem &include) {
  llvm::StringRef producerSpelling = producerEnteredFileSpelling(include);
  return !producerSpelling.empty()
             ? producerSpelling.str()
             : stringutils::stripHeaderToken(include.target).str();
}

/// Return the physical-ish path used to load a materialized include body.
///
/// Do not use this value as a `#line` filename or file-observer proof target.
/// New maps deliberately split physical identity (`opened_path`) from the
/// producer-entered filename spelling (`entered_file_spelling`); conflating the
/// two would either make `__FILE__` observe a canonicalized path or fail to read
/// headers whose entered spelling was only meaningful inside Clang's search
/// context.
inline std::string
refoldIncludeLoadPath(const RefoldModel::IncludeItem &include) {
  if (std::optional<std::filesystem::path> physical =
          producerPhysicalIncludePath(include))
    return physical->string();
  return refoldIncludeEnteredFileSpelling(include);
}

} // namespace refold
} // namespace clang

#endif // LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_INCLUDESPELLINGHELPERS_H
