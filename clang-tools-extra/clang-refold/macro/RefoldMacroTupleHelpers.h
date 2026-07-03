//===--- RefoldMacroTupleHelpers.h -----------------------------*- C++ -*-===//
//
// Caller-tuple parsing helpers shared by the planner and macro-domain replay
// engines.
//
// Tuple replay edits comma-separated elements inside one recovered caller
// argument.  This header owns the raw-lexer tuple splitter and the byte-range
// carrier used by generated-callee replay, tuple replay, and exact old/new
// element comparison.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDMACROTUPLEHELPERS_H
#define LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDMACROTUPLEHELPERS_H

#include "clang/Basic/LangOptions.h"

#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"

#include <cstddef>

namespace clang {
namespace refold {

/// Byte slice for a single top-level element inside a comma-separated tuple.
///
/// `begin`/`end` cover the full half-open byte range for the element inside
/// the caller argument text. `trimBegin`/`trimEnd` shrink that range to the
/// non-whitespace payload used for exact old/new text comparisons.
struct TupleElementSlice {
  size_t begin = 0;
  size_t end = 0;
  size_t trimBegin = 0;
  size_t trimEnd = 0;
};

/// Split a caller tuple into top-level comma-separated elements using
/// Clang's raw lexer.  Returns false when the input is empty or any element
/// trims to an empty payload.
bool splitTopLevelTupleElementsWithLexer(
    llvm::StringRef text, const clang::LangOptions &lang,
    llvm::SmallVectorImpl<TupleElementSlice> &out);

} // namespace refold
} // namespace clang

#endif // LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDMACROTUPLEHELPERS_H
