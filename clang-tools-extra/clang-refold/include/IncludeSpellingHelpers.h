//===--- IncludeSpellingHelpers.h ------------------*- C++ -*-===//
//
// Include-spelling helpers shared by include replay and materialization code.
//
// These helpers keep the logical filename spelling used for line-control and
// source-observer restoration separate from the physical path used to load
// materialized header bytes.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_INCLUDESPELLINGHELPERS_H
#define LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_INCLUDESPELLINGHELPERS_H

#include "proof/RefoldProofVocabulary.h"
#include "util/StringUtils.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Path.h"

#include <cctype>
#include <filesystem>
#include <optional>
#include <string>

namespace clang {
namespace refold {

/// Return true when the slash-separated operand contains a parent-directory
/// component.  This is a syntactic check over the operand spelling, not a
/// filesystem normalization or canonicalization step.
inline bool includeOperandHasParentComponent(llvm::StringRef path) {
  llvm::SmallVector<llvm::StringRef, 8> components;
  path.split(components, '/', /*MaxSplit=*/-1, /*KeepEmpty=*/true);
  return llvm::is_contained(components, llvm::StringRef(".."));
}

/// Return true when \p path uses only the restricted ASCII include-path
/// spelling alphabet that clang-refold is willing to synthesize.  This is only
/// a spelling predicate; it intentionally does not consult the filesystem or
/// model any include search path.
inline bool refoldHasSafeIncludePathSpelling(llvm::StringRef path) {
  auto isSafeIncludePathChar = [](char c) {
    return std::isalnum(static_cast<unsigned char>(c)) || c == '_' ||
           c == '-' || c == '.' || c == '/';
  };
  return !path.empty() && !path.contains('\\') && !path.contains('"') &&
         llvm::all_of(path, isSafeIncludePathChar);
}

/// Validate slash-separated include operand components under the policy used
/// for synthesized relative include operands.  Source-graph side files and
/// emitted rewrite operands both require a relative, non-escaping spelling:
/// no absolute leading component, no empty components, and no `.` or `..`.
inline bool refoldHasSafeSynthesizedRelativeComponents(llvm::StringRef path) {
  if (llvm::sys::path::is_absolute(path))
    return false;

  llvm::SmallVector<llvm::StringRef, 8> components;
  path.split(components, '/', /*MaxSplit=*/-1, /*KeepEmpty=*/true);
  for (llvm::StringRef component : components) {
    if (component.empty() || component == "." || component == "..")
      return false;
  }
  return true;
}

/// Return true when \p path is a relative include operand spelling that may be
/// synthesized into emitted source.  The accepted grammar is intentionally
/// narrow: non-empty ASCII path components using [A-Za-z0-9_./-], with no
/// backslashes, quotes, absolute path spelling, empty components, `.`, or `..`.
inline bool safeSynthesizedRelativeIncludeOperandPath(llvm::StringRef path) {
  return refoldHasSafeIncludePathSpelling(path) &&
         refoldHasSafeSynthesizedRelativeComponents(path);
}

/// Predicate for synthesized relative operands that may be emitted into an
/// include directive after the caller has separately proven replay/source-graph
/// semantics.  The caller chooses quoted vs. angled delimiters; this helper
/// only enforces the relative no-escape path contract.
inline bool safeSynthesizedRelativeIncludeOperand(llvm::StringRef path) {
  return safeSynthesizedRelativeIncludeOperandPath(path);
}

/// Return the logical filename spelling to restore for an include edge.
///
/// This is the spelling observed by preserved `__FILE__` / `__FILE_NAME__`
/// sites when line-control repair wraps a materialized include body.  Prefer
/// producer-entered spelling metadata and use the target-token fallback only
/// for legacy maps that lack split include metadata.
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
/// two would either make `__FILE__` observe a canonicalized path or fail to
/// read headers whose entered spelling was only meaningful inside Clang's
/// search context.
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
