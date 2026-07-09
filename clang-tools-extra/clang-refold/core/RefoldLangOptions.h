//===--- RefoldLangOptions.h ------------------------------------*- C++ -*-===//
//
// Shared raw-lexer language option construction for clang-refold.
//
// This utility owns the small CompilerInvocation-based translation from the
// producer-recorded preprocessing language spelling into the LangOptions used
// by clang-refold's raw lexer helpers.  Keeping this logic outside
// RefoldEngine lets independent validation/pruning code tokenize with the same
// language policy without depending on the engine orchestration surface.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDLANGOPTIONS_H
#define LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDLANGOPTIONS_H

#include "clang/Basic/LangOptions.h"

#include "llvm/ADT/StringRef.h"

namespace clang {
namespace refold {

/// Construct the LangOptions used by all raw-lexer helper paths.
///
/// The producer records the language spelling that was used to preprocess the
/// TU.  Replaying that spelling through CompilerInvocation keeps tokenization
/// decisions, especially literal and comment handling, aligned with the map.
clang::LangOptions makeRefoldLexLangOptions(llvm::StringRef langName);

} // namespace refold
} // namespace clang

#endif // LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDLANGOPTIONS_H
