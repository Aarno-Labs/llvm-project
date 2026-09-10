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

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"

#include <string>

namespace clang {
namespace refold {

/// Construct the LangOptions used by all raw-lexer helper paths.
///
/// The producer records both the language spelling and the exact cc1 command
/// line it preprocessed with.  Replaying that command line through
/// CompilerInvocation, with the recorded language spelling re-asserted on top,
/// keeps every tokenization decision aligned with the map -- literal and
/// comment handling, and equally the options that decide where a logical line
/// ends, such as `-std=` and `-ftrigraphs`.
///
/// `producerArgv` may be empty, which reproduces the language token alone.
clang::LangOptions
makeRefoldLexLangOptions(llvm::StringRef langName,
                         llvm::ArrayRef<std::string> producerArgv);

} // namespace refold
} // namespace clang

#endif // LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDLANGOPTIONS_H
