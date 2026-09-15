//===--- RefoldToken.h ------------------------------------------*- C++ -*-===//
//
// Shared lightweight token carrier used by clang-refold subsystems, plus the
// free function that produces it from a preprocessed byte buffer.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDTOKEN_H
#define LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDTOKEN_H

#include "clang/Basic/LangOptions.h"

#include <cstddef>
#include <string>
#include <vector>

namespace clang {
namespace refold {

/// Lightweight preprocessed-token carrier used throughout clang-refold.
///
/// `PPTok` is deliberately string-based so the refolding pipeline does not
/// depend on Clang's in-process token-kind enum values: the kind is captured
/// as a deterministic textual label and the spelling preserves the exact byte
/// slice from the preprocessed stream.
struct PPTok {
  /// Clang token kind label (for example, "identifier", "numeric_constant",
  /// "string_literal", "l_paren").  Sourced from the token dump so refolding
  /// remains stable across compiler versions.
  std::string kind;

  /// Exact token spelling in the preprocessed stream.
  std::string spelling;
};

/// Lex a preprocessed byte buffer into `PPTok` tokens using Clang's raw lexer.
///
/// Tokenizes `bytes` so token boundaries match the `-E -P` output stream.  Both
/// output vectors are cleared on entry.  `out` receives one `PPTok` per token
/// with its exact byte spelling, and `startOffs` receives each token's starting
/// byte offset plus a one-past-end sentinel, so
/// `startOffs.size() == out.size() + 1`.
///
/// The implementation temporarily appends a newline when necessary, uses raw
/// lexer mode with whitespace tokens suppressed, treats UTF-8 as raw bytes, and
/// computes offsets from the Clang `SourceManager` so they remain stable and
/// monotone.  Internal failures fatal-error rather than throwing.
void lexPPTokens(const std::string &bytes, std::vector<PPTok> &out,
                 std::vector<std::size_t> &startOffs,
                 const clang::LangOptions &lang);

} // namespace refold
} // namespace clang

#endif // LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDTOKEN_H
