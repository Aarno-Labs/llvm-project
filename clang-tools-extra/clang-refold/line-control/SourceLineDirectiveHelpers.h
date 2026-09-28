//===--- SourceLineDirectiveHelpers.h --------------*- C++ -*-===//
//
// Source-line directive and trivia helpers.
//
// Include materialization and expansion fallback use these routines to prove
// include-closure gap trivia and to spell the directive that resumes the line
// state of a consumed source line-control gap.  The state itself comes from
// producer line-control events, through
// RefoldLineControlProof::LineControlGapResume().
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_SOURCELINEDIRECTIVEHELPERS_H
#define LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_SOURCELINEDIRECTIVEHELPERS_H

#include "model/RefoldModel.h"

#include "llvm/ADT/StringRef.h"

#include <cstdint>
#include <string>

namespace clang {
namespace refold {

class RefoldPathIdentity;

using llvm::StringRef;

/// Return true iff `text` is only whitespace and complete C/C++ comments.
///
/// This is intentionally a lexical-trivia predicate, not a preprocessing
/// predicate. Include-closure may carry these bytes through a replacement
/// because comments and whitespace do not contribute preprocessing tokens.
/// Anything that would lex as a real token, including malformed or
/// unterminated comments, remains outside this proof class.
bool isWsOrCompleteCommentTrivia(StringRef text);

/// Parse one admitted conditional-control directive line.
///
/// This recognizes only the small directive grammar that include-closure is
/// allowed to preserve as an inert gap:
///
///   #if 0
///   #if 1
///   #elif 0
///   #elif 1
///   #else
///   #endif
///
/// The `depth` argument tracks balance across the preserved gap. The function
/// rejects escaped physical lines, macro-dependent conditions, trailing tokens,
/// and every directive with side effects. In particular, this is not a general
/// preprocessor directive parser.
bool parseLiteralEmptyConditionalDirectiveLine(StringRef line, unsigned &depth);

/// Return true iff `text` may be carried through a TU include-closure edit.
///
/// This predicate defines the source-preservation proof for bytes between
/// touched top-level include directives. The gap is accepted only when every
/// byte is either:
///
///   * whitespace/comment trivia, or
///   * part of a complete empty literal conditional-control island.
///
/// Accepted bytes are preserved verbatim in the emitted replacement. They are
/// never silently deleted. Anything that can affect macro state, include state,
/// diagnostics, line state, or later preprocessing remains outside this proof
/// class and forces the caller to fail closed.
bool isPreservableIncludeClosureGapTrivia(StringRef text);

/// Return true when preserved trivia begins with a preprocessing directive
/// after optional horizontal whitespace. Such trivia must be placed at the
/// start of a physical line when appended after materialized B tokens.
bool startsWithPreprocessorDirectiveTrivia(StringRef text);

/// Logical file/line state that must be re-established after consuming a
/// source-only line-control gap.
struct SourceLineDirectiveGapResume {
  size_t lineAtResume = 0;
  std::string fileSpelling;

  /// Optional numeric line-marker flags to replay with the resume directive.
  /// Empty means the canonical `#line` spelling is sufficient.
  std::string lineMarkerFlags;
};

/// Return true iff the untouched suffix may observe the current presumed file.
///
/// Volatile string-valued predefined macros such as `__DATE__` can be valid
/// filename operands in a source-only line-control directive, and the name
/// they produced depends on when A was preprocessed.  Such a gap is still
/// token-preservable when the remaining source can only observe the resumed
/// line number.  This conservative predicate rejects any later `__FILE__` or
/// `__FILE_NAME__` use recorded by the producer, and also rejects raw source
/// spellings of those builtins in the suffix as a fail-closed backstop for
/// cases not represented by an invocation with a usable source range.
bool sourceSuffixMayObservePresumedFileSpelling(
    const RefoldModel &model, StringRef file, uint64_t resumeOffset,
    const RefoldPathIdentity &paths, StringRef fileText);

/// Format the local resume directive for a consumed source-only line-control
/// gap.  Canonical #line spelling is used unless the consumed state carries
/// numeric line-marker flags that must be replayed to preserve the suffix
/// state.
std::string
formatSourceLineDirectiveGapResume(const SourceLineDirectiveGapResume &resume);

} // namespace refold
} // namespace clang

#endif // LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_SOURCELINEDIRECTIVEHELPERS_H
