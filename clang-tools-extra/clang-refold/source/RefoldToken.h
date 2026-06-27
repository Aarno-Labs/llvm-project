//===--- RefoldToken.h ------------------------------------------*- C++ -*-===//
//
// Shared lightweight token carrier used by clang-refold subsystems.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDTOKEN_H
#define LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDTOKEN_H

#include <string>

namespace clang {
namespace refold {

struct PPTok {
  // Clang token kind (for example, "identifier", "numeric_constant",
  // "string_literal", or "l_paren"). This is sourced from the token dump and
  // intentionally remains a simple string so refolding does not depend on
  // Clang's in-process token enum values.
  std::string kind;

  // Exact token spelling in the preprocessed stream.
  std::string spelling;
};

} // namespace refold
} // namespace clang

#endif // LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDTOKEN_H
