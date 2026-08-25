//===--- RefoldPreprocessingDirectiveScanner.h -----------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Shared exact lexical recognition for preprocessing directive lines and
// directly spelled pragma operators.
//
// Preprocessing structure cannot be recognized safely with regular-expression
// or physical-line matching.  Directive recognition occurs after phase-two
// escaped-newline deletion and phase-three comment replacement, while token
// spellings such as strings, raw strings, and comments can themselves contain
// `#` or physical newlines.  This scanner combines Clang's raw lexer with a
// small byte-domain logical-line state machine so every consumer applies the
// same language-mode-aware theorem.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_PREPROCESSING_DIRECTIVE_SCANNER_H
#define LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_PREPROCESSING_DIRECTIVE_SCANNER_H

#include "llvm/ADT/StringRef.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace clang {

class LangOptions;

namespace refold {

/// Lexical class of the first significant token after a directive introducer.
enum class PreprocessingDirectiveHeadKind {
  /// The logical directive line has no significant token after `#`.
  Empty,
  /// The directive head is an identifier token; `keyword` carries its spelling.
  Identifier,
  /// The directive head is a preprocessing numeric token, as in a GNU marker.
  Numeric,
  /// The directive head exists but is neither an identifier nor numeric token.
  Other,
};

/// Classification of a direct include operand recovered without expansion.
enum class PreprocessingIncludeOperandKind {
  /// This is not an `include` directive head.
  NotApplicable,
  /// The operand is exactly one simple quoted header-name spelling.
  SimpleQuotedHeader,
  /// The operand is absent, angled, macro-derived, malformed, or otherwise not
  /// provably one simple quoted header name.
  Other,
};

/// One exact complete logical preprocessing directive line.
///
/// `[begin,end)` starts at the beginning of the physical logical-line prefix
/// that contains the directive introducer.  It therefore includes leading
/// whitespace, comments, and phase-two splices before `#`.  The interval ends
/// after the complete logical directive, including its terminating unspliced
/// newline when one exists.  `introducerBegin` names the exact `#`, `%:`, or
/// enabled trigraph spelling that Clang tokenized as `tok::hash`.
struct PreprocessingDirectiveLine {
  uint64_t begin = 0;
  uint64_t introducerBegin = 0;
  uint64_t introducerEnd = 0;
  uint64_t end = 0;
  PreprocessingDirectiveHeadKind headKind =
      PreprocessingDirectiveHeadKind::Empty;
  std::string keyword;
  PreprocessingIncludeOperandKind includeOperandKind =
      PreprocessingIncludeOperandKind::NotApplicable;
  std::optional<std::string> simpleQuotedIncludePath;

  /// Return whether the scanner produced a nonempty in-bounds logical line.
  bool IsValid() const {
    return begin <= introducerBegin && introducerBegin < introducerEnd &&
           introducerEnd <= end;
  }
};

/// Directly spelled pragma-operator syntax outside a directive line.
enum class PreprocessingPragmaOperatorKind {
  StandardPragma,
  MicrosoftPragma,
};

/// Exact raw-token interval for one directly spelled pragma operator.
struct PreprocessingPragmaOperatorInterval {
  PreprocessingPragmaOperatorKind kind =
      PreprocessingPragmaOperatorKind::StandardPragma;
  uint64_t begin = 0;
  uint64_t end = 0;

  bool IsValid() const { return begin < end; }
};

/// Exact physical spelling interval of one ordinary whole-source raw token.
///
/// Direct TU token realization may cite only one complete lexical token; a
/// producer range that begins or ends inside an identifier, literal, or
/// punctuator is not an exact source mapping.  Comments and optional raw
/// whitespace tokens remain in the trivia inventory instead.  Keeping these
/// classes separate lets consumers distinguish one complete A-token spelling
/// from an arbitrary range whose endpoints merely happen to be legal lexical
/// boundaries.
struct PreprocessingLexicalTokenInterval {
  uint64_t begin = 0;
  uint64_t end = 0;

  bool IsValid() const { return begin < end; }

  bool Equals(uint64_t queryBegin, uint64_t queryEnd) const {
    return begin == queryBegin && end == queryEnd;
  }

  bool ContainsInteriorBoundary(uint64_t offset) const {
    return begin < offset && offset < end;
  }
};

/// Maximal physical source interval containing only preprocessing trivia.
///
/// Trivia is recognized in the same whole-buffer raw-lexer pass used for
/// directive discovery.  The interval may contain horizontal/vertical
/// whitespace, phase-two escaped newlines, and complete comment tokens, but it
/// never contains an ordinary preprocessing token.  Recording maximal ranges
/// lets byte-span consumers prove internal source gaps without reparsing a
/// substring out of its lexical context or mistaking a partial comment/literal
/// for removable trivia.
struct PreprocessingTriviaInterval {
  uint64_t begin = 0;
  uint64_t end = 0;

  bool IsValid() const { return begin < end; }

  /// Return whether this trivia interval completely contains `[queryBegin,
  /// queryEnd)`.
  bool Contains(uint64_t queryBegin, uint64_t queryEnd) const {
    return begin <= queryBegin && queryEnd <= end;
  }
};

/// Trivia component whose physical spelling cannot be cut at an interior byte.
///
/// Complete comments, phase-two escaped-newline spellings, and physical CRLF
/// pairs are preprocessing trivia as wholes, but an arbitrary byte subrange of
/// any such component is not.  These intervals preserve the boundary
/// information that a maximal trivia range alone would lose.
struct PreprocessingIndivisibleTriviaInterval {
  uint64_t begin = 0;
  uint64_t end = 0;

  bool IsValid() const { return begin < end; }

  /// Return whether `offset` cuts the physical spelling of this component.
  bool ContainsInteriorBoundary(uint64_t offset) const {
    return begin < offset && offset < end;
  }
};

/// Complete result of one physical-source lexical census.
struct PreprocessingDirectiveScanResult {
  std::vector<PreprocessingDirectiveLine> directives;
  std::vector<PreprocessingPragmaOperatorInterval> pragmaOperators;
  std::vector<PreprocessingLexicalTokenInterval> lexicalTokenIntervals;
  std::vector<PreprocessingTriviaInterval> triviaIntervals;
  std::vector<PreprocessingIndivisibleTriviaInterval>
      indivisibleTriviaIntervals;
  std::vector<std::string> diagnostics;

  /// Scanner diagnostics indicate that exact lexical coverage was not proven.
  bool IsComplete() const { return diagnostics.empty(); }
};

/// Return whether `sourceBytes` ends in a physical newline that survives
/// phase-two escaped-newline deletion.
///
/// CRLF is treated as one physical newline.  A preceding backslash, optional
/// extension whitespace, or enabled trigraph backslash makes the final newline
/// a splice and therefore not a logical-line boundary.
bool sourceTextEndsWithNonSplicedPhysicalNewline(
    llvm::StringRef sourceBytes, const clang::LangOptions &lexLang);

/// Return whether a byte appended after `sourceBytes` would be the first byte
/// of a translated logical line.
///
/// This is `sourceTextEndsWithNonSplicedPhysicalNewline` weakened by the one
/// spelling C allows between a logical-line boundary and a directive
/// introducer: horizontal white-space.  A trailing space/tab/vertical-tab/
/// form-feed run is removed before the newline test, which leaves the splice
/// analysis exact -- that run can contain no escaped newline, because neither a
/// backslash nor a trigraph introducer is horizontal white-space and either one
/// stops the scan.  A trailing comment would also preserve the boundary and is
/// deliberately still rejected; no proof needs it.
bool sourceTextEndsAtLogicalLineBeginning(llvm::StringRef sourceBytes,
                                          const clang::LangOptions &lexLang);

/// Return whether `insertion` begins by terminating the logical line at the
/// end of `sourcePrefix`.
///
/// This cross-boundary form is required for an insertion after an EOF directive:
/// a leading newline in the insertion does not terminate the directive when the
/// source prefix ends in a phase-two backslash spelling.
bool insertionBeginsWithNonSplicedPhysicalNewline(
    llvm::StringRef sourcePrefix, llvm::StringRef insertion,
    const clang::LangOptions &lexLang);

/// Scan one physical source buffer using the supplied active language mode.
///
/// The scanner recognizes directive introducers only when all preceding bytes
/// on the translated logical line are preprocessing whitespace/comments.  It
/// handles leading comments, escaped newlines, CRLF, directive continuations,
/// digraphs, enabled trigraphs, and `#` characters embedded in tokens without
/// regex matching or source-nearest guesses.  Directly spelled `_Pragma` and
/// Microsoft `__pragma` expressions outside directive lines are returned in a
/// separate exact interval list.
PreprocessingDirectiveScanResult
scanPreprocessingDirectives(llvm::StringRef sourceBytes,
                            const clang::LangOptions &lexLang);

} // namespace refold
} // namespace clang

#endif // LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_PREPROCESSING_DIRECTIVE_SCANNER_H
