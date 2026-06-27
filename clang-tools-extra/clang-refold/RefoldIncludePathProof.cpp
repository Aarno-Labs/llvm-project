//===--- RefoldIncludePathProof.cpp ----------------------------*- C++ -*-===//
//
// Shared syntactic include-operand path proof helpers for clang-refold.
//
//===----------------------------------------------------------------------===//

#include "RefoldIncludePathProof.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Support/Path.h"

#include <cctype>

namespace clang {
namespace refold {

namespace {

static bool isSafeIncludePathChar(char C) {
  return std::isalnum(static_cast<unsigned char>(C)) || C == '_' ||
         C == '-' || C == '.' || C == '/';
}

/// Return true when \p Path uses only the restricted ASCII include-path
/// spelling alphabet that the refolder is willing to synthesize.  This is only
/// a spelling predicate; it intentionally does not consult the filesystem or
/// model any include search path.
static bool hasSafeIncludePathSpelling(llvm::StringRef Path) {
  return !Path.empty() && !Path.contains('\\') && !Path.contains('"') &&
         llvm::all_of(Path, [](char C) { return isSafeIncludePathChar(C); });
}

/// Validate slash-separated include operand components under the policy used
/// for synthesized relative include operands.  Source-graph side files and
/// emitted rewrite operands both require a relative, non-escaping spelling:
/// no absolute leading component, no empty components, and no `.` or `..`.
static bool hasSafeSynthesizedRelativeComponents(llvm::StringRef Path) {
  if (llvm::sys::path::is_absolute(Path))
    return false;

  llvm::SmallVector<llvm::StringRef, 8> Components;
  Path.split(Components, '/', /*MaxSplit=*/-1, /*KeepEmpty=*/true);
  for (llvm::StringRef Component : Components) {
    if (Component.empty() || Component == "." || Component == "..")
      return false;
  }
  return true;
}

} // namespace

bool includeOperandHasParentComponent(llvm::StringRef Path) {
  llvm::SmallVector<llvm::StringRef, 8> Components;
  Path.split(Components, '/', /*MaxSplit=*/-1, /*KeepEmpty=*/true);
  return llvm::is_contained(Components, llvm::StringRef(".."));
}

bool safeSynthesizedRelativeIncludeOperandPath(llvm::StringRef Path) {
  return hasSafeIncludePathSpelling(Path) &&
         hasSafeSynthesizedRelativeComponents(Path);
}

bool safeSynthesizedRelativeIncludeOperand(llvm::StringRef Path) {
  return safeSynthesizedRelativeIncludeOperandPath(Path);
}

} // namespace refold
} // namespace clang
