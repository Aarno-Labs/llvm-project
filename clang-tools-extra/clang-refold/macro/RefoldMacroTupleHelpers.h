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
// It also owns chained-call suffix extension: how far a callsite patch must
// reach past a macro invocation into the parenthesized groups that follow it.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDMACROTUPLEHELPERS_H
#define LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDMACROTUPLEHELPERS_H

#include "clang/Basic/LangOptions.h"

#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"

#include <cstddef>
#include <cstdint>

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

/// Split a parenthesized macro-call actual payload into top-level actuals.
///
/// Unlike caller tuple elements, macro actual slots may be intentionally empty,
/// as in `F(, x)`.  This helper therefore preserves empty trimmed ranges while
/// retaining the same lexer-aware top-level comma handling used for tuples.
bool splitTopLevelMacroActualsWithLexer(
    llvm::StringRef text, const clang::LangOptions &lang,
    llvm::SmallVectorImpl<TupleElementSlice> &out);

/// Extend an invocation end offset over trailing chained-call suffix groups
/// when the replacement is no longer directly callable.
///
/// A macro invocation immediately followed by parenthesized argument lists in
/// the source file may be a chain of function-like macros evaluating to
/// another function-like macro (e.g. `INC3()()()(10)`).  A callsite patch that
/// replaced only the first invocation (`INC3()`) would leave a dangling
/// `(...)` suffix.  So when \p replacement is neither an identifier nor a
/// simple `IDENT(...)` call, the immediately following groups are consumed.
/// The final group stays attached when \p replacement is itself a
/// parenthesized callable head followed by call-suffix groups.
uint64_t extendChainedCallEnd(llvm::StringRef fileText, uint64_t invEnd,
                              llvm::StringRef replacement);

} // namespace refold
} // namespace clang

#endif // LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDMACROTUPLEHELPERS_H
