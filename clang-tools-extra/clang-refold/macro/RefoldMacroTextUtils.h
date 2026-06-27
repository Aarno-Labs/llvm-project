//===--- RefoldMacroTextUtils.h -------------------------------*- C++ -*-===//
//
// Shared lexical text predicates used by macro patch planning and the
// remaining RefoldEngine orchestration code that arbitrates TU-vs-macro edits.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDMACROTEXTUTILS_H
#define LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDMACROTEXTUTILS_H

#include "llvm/ADT/StringRef.h"

namespace clang {
class LangOptions;

namespace refold {

/// Return true when \p text contains a comma that would split a single
/// function-like macro actual.
///
/// This deliberately models macro-argument collection rather than general C
/// expression tuple parsing: nested parentheses protect commas, while brackets
/// and braces do not.  Comments and literals are tokenized as opaque raw-lexer
/// tokens so commas inside them cannot produce a false positive.
bool refoldMacroActualHasTopLevelComma(llvm::StringRef text,
                                       const LangOptions &lang);

} // namespace refold
} // namespace clang

#endif // LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDMACROTEXTUTILS_H
