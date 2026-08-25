//===--- RefoldPreprocessingDirectiveScanner.cpp ---------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// Shared exact preprocessing lexical scanner implementation.
//
//===----------------------------------------------------------------------===//

#include "source/RefoldPreprocessingDirectiveScanner.h"

#include "clang/Basic/LangOptions.h"
#include "clang/Basic/SourceLocation.h"
#include "clang/Basic/TokenKinds.h"
#include "clang/Lex/Lexer.h"
#include "clang/Lex/Token.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/FormatVariadic.h"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <utility>
#include <vector>

using namespace llvm;

namespace clang {
namespace refold {
namespace {

/// Raw-lexer token coordinates in the scratch-buffer byte domain.
struct RawSourceToken {
  tok::TokenKind kind = tok::unknown;
  uint64_t begin = 0;
  uint64_t end = 0;
};

/// Raw-lexer token index and complete logical-line prefix for one directive.
struct DirectiveIntroducer {
  size_t tokenIndex = 0;
  uint64_t prefixBegin = 0;
};

/// Return whether `[begin,end)` is one nonempty range in `sourceBytes`.
static bool sourceRangeValid(StringRef sourceBytes, uint64_t begin,
                             uint64_t end) {
  return begin < end && end <= sourceBytes.size();
}

/// Return the first byte after a phase-two escaped newline at `begin`.
///
/// Clang accepts horizontal whitespace between a backslash and the physical
/// newline as an extension.  Enabled trigraph `??/` is the equivalent phase-one
/// backslash spelling and is recognized under the active language mode.
static std::optional<size_t>
escapedNewlineEnd(StringRef sourceBytes, size_t begin,
                  const LangOptions &lexLang) {
  size_t cursor = begin;
  if (cursor < sourceBytes.size() && sourceBytes[cursor] == '\\') {
    ++cursor;
  } else if (lexLang.Trigraphs && cursor + 2 < sourceBytes.size() &&
             sourceBytes[cursor] == '?' && sourceBytes[cursor + 1] == '?' &&
             sourceBytes[cursor + 2] == '/') {
    cursor += 3;
  } else {
    return std::nullopt;
  }

  while (cursor < sourceBytes.size() &&
         (sourceBytes[cursor] == ' ' || sourceBytes[cursor] == '\t' ||
          sourceBytes[cursor] == '\v' || sourceBytes[cursor] == '\f')) {
    ++cursor;
  }

  if (cursor < sourceBytes.size() && sourceBytes[cursor] == '\n')
    return cursor + 1;
  if (cursor < sourceBytes.size() && sourceBytes[cursor] == '\r') {
    ++cursor;
    if (cursor < sourceBytes.size() && sourceBytes[cursor] == '\n')
      ++cursor;
    return cursor;
  }
  return std::nullopt;
}

/// Lex one physical source buffer into exact raw-token byte intervals.
///
/// Out-of-range coordinates are diagnosed and omitted.  The caller therefore
/// cannot accidentally turn malformed lexer output into an in-bounds protected
/// source interval.
static std::vector<RawSourceToken>
lexRawSourceTokens(StringRef sourceBytes, const LangOptions &lexLang,
                   std::vector<std::string> &diagnostics) {
  const SourceLocation baseLoc = SourceLocation::getFromRawEncoding(1);
  const uint64_t maximumRepresentableOffset =
      std::numeric_limits<unsigned>::max() - baseLoc.getRawEncoding();
  if (sourceBytes.size() > maximumRepresentableOffset) {
    diagnostics.push_back(
        llvm::formatv("source size {0} exceeds raw SourceLocation capacity",
                      sourceBytes.size())
            .str());
    return {};
  }

  std::string scratch = sourceBytes.str();
  scratch.push_back('\0');

  const char *bufferStart = scratch.data();
  const char *bufferEnd = bufferStart + sourceBytes.size();
  Lexer lexer(baseLoc, lexLang, bufferStart, bufferStart, bufferEnd);
  lexer.SetCommentRetentionState(true);

  std::vector<RawSourceToken> tokens;
  uint64_t previousTokenEnd = 0;
  Token token;
  while (true) {
    lexer.LexFromRawLexer(token);
    if (token.is(tok::eof))
      break;

    const unsigned tokenEncoding = token.getLocation().getRawEncoding();
    const unsigned baseEncoding = baseLoc.getRawEncoding();
    if (tokenEncoding < baseEncoding) {
      diagnostics.push_back(
          "raw lexer produced a token before the scratch-buffer base");
      continue;
    }

    const uint64_t tokenBegin = tokenEncoding - baseEncoding;
    const uint64_t tokenEnd = tokenBegin + token.getLength();
    if (tokenEnd <= tokenBegin || tokenEnd > sourceBytes.size()) {
      diagnostics.push_back(
          llvm::formatv("raw lexer produced out-of-range token [{0},{1}) for "
                        "source size {2}",
                        tokenBegin, tokenEnd, sourceBytes.size())
              .str());
      continue;
    }
    if (!tokens.empty() && tokenBegin < previousTokenEnd) {
      diagnostics.push_back(
          llvm::formatv("raw lexer produced overlapping/nonmonotone token "
                        "[{0},{1}) after byte {2}",
                        tokenBegin, tokenEnd, previousTokenEnd)
              .str());
      continue;
    }

    tokens.push_back(RawSourceToken{token.getKind(), tokenBegin, tokenEnd});
    previousTokenEnd = tokenEnd;
  }
  return tokens;
}

/// Normalize one raw token spelling by applying phase-two line splicing.
static std::string removeEscapedNewlines(StringRef spelling,
                                         const LangOptions &lexLang) {
  std::string normalized;
  normalized.reserve(spelling.size());

  size_t cursor = 0;
  while (cursor < spelling.size()) {
    if (std::optional<size_t> escapedEnd =
            escapedNewlineEnd(spelling, cursor, lexLang)) {
      cursor = *escapedEnd;
      continue;
    }
    normalized.push_back(spelling[cursor]);
    ++cursor;
  }
  return normalized;
}

/// Return whether one raw token is a retained block or line comment.
static bool isCommentToken(const RawSourceToken &token) {
  return token.kind == tok::comment;
}

/// Return whether one raw token consists entirely of preprocessing whitespace.
///
/// Raw lexing normally omits whitespace unless keep-whitespace mode is enabled,
/// but exact recognition must not rely on that incidental lexer configuration.
static bool isRawWhitespaceToken(StringRef sourceBytes,
                                 const RawSourceToken &token,
                                 const LangOptions &lexLang) {
  uint64_t cursor = token.begin;
  while (cursor < token.end) {
    if (std::optional<size_t> escapedEnd =
            escapedNewlineEnd(sourceBytes, cursor, lexLang)) {
      if (*escapedEnd > token.end)
        return false;
      cursor = *escapedEnd;
      continue;
    }

    const char value = sourceBytes[cursor];
    if (value != ' ' && value != '\t' && value != '\v' && value != '\f' &&
        value != '\n' && value != '\r')
      return false;
    ++cursor;
  }
  return true;
}

/// Start a trivia run at `cursor` unless one is already active.
static void beginTriviaRun(std::optional<uint64_t> &runBegin,
                           uint64_t cursor) {
  if (!runBegin)
    runBegin = cursor;
}

/// Finish the active maximal trivia run at `cursor`.
static void endTriviaRun(
    std::optional<uint64_t> &runBegin, uint64_t cursor,
    std::vector<PreprocessingTriviaInterval> &intervals) {
  if (runBegin && *runBegin < cursor)
    intervals.push_back(PreprocessingTriviaInterval{*runBegin, cursor});
  runBegin.reset();
}

/// Record every indivisible splice or CRLF pair in `[begin,end)`.
static void appendIndivisibleWhitespaceComponents(
    StringRef sourceBytes, uint64_t begin, uint64_t end,
    const LangOptions &lexLang,
    std::vector<PreprocessingIndivisibleTriviaInterval> &components) {
  uint64_t cursor = begin;
  while (cursor < end) {
    if (std::optional<size_t> escapedEnd =
            escapedNewlineEnd(sourceBytes, cursor, lexLang)) {
      if (*escapedEnd <= end) {
        components.push_back(PreprocessingIndivisibleTriviaInterval{
            cursor, static_cast<uint64_t>(*escapedEnd)});
        cursor = *escapedEnd;
        continue;
      }
    }
    if (sourceBytes[cursor] == '\r' && cursor + 1 < end &&
        sourceBytes[cursor + 1] == '\n') {
      components.push_back(
          PreprocessingIndivisibleTriviaInterval{cursor, cursor + 2});
      cursor += 2;
      continue;
    }
    ++cursor;
  }
}

/// Build maximal whole-buffer ranges containing only preprocessing trivia.
///
/// This is deliberately derived from the complete source tokenization rather
/// than by lexing each queried gap independently.  A substring can begin inside
/// a comment, string, raw literal, or escaped token spelling and appear harmless
/// when removed from that context. Whole-buffer token extents identify complete
/// trivia components: ordinary tokens close the current run, while comments,
/// physical whitespace, and phase-two splices extend it. Separate indivisible
/// component intervals prevent later consumers from cutting a comment or splice.
static std::vector<PreprocessingTriviaInterval>
findPreprocessingTriviaIntervals(
    StringRef sourceBytes, ArrayRef<RawSourceToken> tokens,
    const LangOptions &lexLang,
    std::vector<PreprocessingIndivisibleTriviaInterval>
        &indivisibleComponents) {
  std::vector<PreprocessingTriviaInterval> intervals;
  std::optional<uint64_t> runBegin;
  size_t tokenIndex = 0;
  uint64_t cursor = 0;

  while (cursor < sourceBytes.size()) {
    while (tokenIndex < tokens.size() && tokens[tokenIndex].end <= cursor)
      ++tokenIndex;

    if (tokenIndex < tokens.size()) {
      const RawSourceToken &token = tokens[tokenIndex];
      if (token.begin <= cursor && cursor < token.end) {
        if (token.begin == cursor && isCommentToken(token)) {
          beginTriviaRun(runBegin, cursor);
          indivisibleComponents.push_back(
              PreprocessingIndivisibleTriviaInterval{token.begin, token.end});
        } else if (token.begin == cursor &&
                   isRawWhitespaceToken(sourceBytes, token, lexLang)) {
          beginTriviaRun(runBegin, cursor);
          appendIndivisibleWhitespaceComponents(
              sourceBytes, token.begin, token.end, lexLang,
              indivisibleComponents);
        } else {
          endTriviaRun(runBegin, cursor, intervals);
        }
        cursor = token.end;
        ++tokenIndex;
        continue;
      }
    }

    if (std::optional<size_t> escapedEnd =
            escapedNewlineEnd(sourceBytes, cursor, lexLang)) {
      beginTriviaRun(runBegin, cursor);
      indivisibleComponents.push_back(
          PreprocessingIndivisibleTriviaInterval{
              cursor, static_cast<uint64_t>(*escapedEnd)});
      cursor = *escapedEnd;
      continue;
    }

    const char value = sourceBytes[cursor];
    if (value == '\r' && cursor + 1 < sourceBytes.size() &&
        sourceBytes[cursor + 1] == '\n') {
      beginTriviaRun(runBegin, cursor);
      indivisibleComponents.push_back(
          PreprocessingIndivisibleTriviaInterval{cursor, cursor + 2});
      cursor += 2;
      continue;
    }
    if (value == ' ' || value == '\t' || value == '\v' || value == '\f' ||
        value == '\n' || value == '\r') {
      beginTriviaRun(runBegin, cursor);
      ++cursor;
      continue;
    }

    // Any remaining byte is either covered by an ordinary raw token whose
    // beginning is before this cursor (already diagnosed as malformed lexer
    // coverage) or is an uncovered nontrivia byte.  In both cases it is a hard
    // boundary and can never become part of a trivia proof.
    endTriviaRun(runBegin, cursor, intervals);
    ++cursor;
  }
  endTriviaRun(runBegin, cursor, intervals);
  return intervals;
}

/// Find every raw-lexer hash token that is a directive introducer after
/// escaped-newline deletion and comment replacement.
///
/// `Token::isAtStartOfLine()` is not sufficient when a retained leading block
/// comment precedes `#`: the comment becomes preprocessing whitespace, but the
/// following token does not necessarily inherit Clang's start-of-line flag.
/// This state machine tracks the translated logical-line prefix directly while
/// raw token extents prevent newlines inside comments/literals from becoming
/// false directive boundaries.
static std::vector<DirectiveIntroducer> findDirectiveIntroducers(
    StringRef sourceBytes, ArrayRef<RawSourceToken> tokens,
    const LangOptions &lexLang, std::vector<std::string> &diagnostics) {
  std::vector<DirectiveIntroducer> introducers;
  bool onlyTriviaOnLogicalLine = true;
  size_t logicalLinePrefixBegin = 0;
  size_t tokenIndex = 0;
  size_t cursor = 0;

  while (cursor < sourceBytes.size()) {
    while (tokenIndex < tokens.size() && tokens[tokenIndex].end <= cursor)
      ++tokenIndex;

    // A raw token may physically begin with a phase-two line splice.  Clang's
    // raw lexer then reports one token spanning both the splice and the token
    // spelling that follows it.  Token coverage must therefore dominate the
    // standalone-splice case; advancing over the splice first would leave the
    // cursor inside that token and incorrectly diagnose uncovered source.
    if (tokenIndex < tokens.size()) {
      const RawSourceToken &token = tokens[tokenIndex];
      if (token.begin <= cursor && cursor < token.end) {
        if (token.begin == cursor) {
          if (isCommentToken(token)) {
            cursor = token.end;
            ++tokenIndex;
            continue;
          }

          if (onlyTriviaOnLogicalLine && token.kind == tok::hash)
            introducers.push_back(
                DirectiveIntroducer{tokenIndex, logicalLinePrefixBegin});

          onlyTriviaOnLogicalLine = false;
        }
        cursor = token.end;
        ++tokenIndex;
        continue;
      }
    }

    if (std::optional<size_t> escapedEnd =
            escapedNewlineEnd(sourceBytes, cursor, lexLang)) {
      cursor = *escapedEnd;
      continue;
    }

    if (sourceBytes[cursor] == '\n') {
      ++cursor;
      onlyTriviaOnLogicalLine = true;
      logicalLinePrefixBegin = cursor;
      continue;
    }
    if (sourceBytes[cursor] == '\r') {
      ++cursor;
      if (cursor < sourceBytes.size() && sourceBytes[cursor] == '\n')
        ++cursor;
      onlyTriviaOnLogicalLine = true;
      logicalLinePrefixBegin = cursor;
      continue;
    }

    if (sourceBytes[cursor] == ' ' || sourceBytes[cursor] == '\t' ||
        sourceBytes[cursor] == '\v' || sourceBytes[cursor] == '\f') {
      ++cursor;
      continue;
    }

    // Clang accepts an initial UTF-8 byte-order mark before the first logical
    // source line.  It remains part of the protected first-line prefix.
    if (cursor == 0 && sourceBytes.size() >= 3 &&
        static_cast<unsigned char>(sourceBytes[0]) == 0xEF &&
        static_cast<unsigned char>(sourceBytes[1]) == 0xBB &&
        static_cast<unsigned char>(sourceBytes[2]) == 0xBF) {
      cursor = 3;
      continue;
    }

    // Any nontrivia byte not represented by the raw lexer is an exactness
    // failure.  Treat it as source text so it cannot manufacture a following
    // directive, and record the failure so producer-authoritative consumers
    // reject this scan rather than relying on incomplete lexical coverage.
    diagnostics.push_back(
        llvm::formatv("raw lexer left nontrivia source byte {0} uncovered",
                      cursor)
            .str());
    onlyTriviaOnLogicalLine = false;
    ++cursor;
  }

  return introducers;
}

/// Recover the complete physical end of one logical directive line.
///
/// Raw-token extents suppress false line termination inside comments, strings,
/// and raw literals.  Escaped physical newlines that lie between tokens are
/// consumed explicitly.
static uint64_t findDirectiveLogicalEnd(
    StringRef sourceBytes, uint64_t introducerBegin,
    ArrayRef<RawSourceToken> tokens, size_t introducerTokenIndex,
    const LangOptions &lexLang) {
  size_t tokenIndex = introducerTokenIndex;
  uint64_t cursor = introducerBegin;
  while (cursor < sourceBytes.size()) {
    while (tokenIndex < tokens.size() && tokens[tokenIndex].end <= cursor)
      ++tokenIndex;

    // As in directive-introducer discovery, a raw token can physically start
    // with a line splice.  Skip the complete token before interpreting any
    // embedded physical newline as the end of the logical directive.
    if (tokenIndex < tokens.size()) {
      const RawSourceToken &token = tokens[tokenIndex];
      if (token.begin <= cursor && cursor < token.end) {
        cursor = token.end;
        ++tokenIndex;
        continue;
      }
    }

    if (std::optional<size_t> escapedEnd =
            escapedNewlineEnd(sourceBytes, cursor, lexLang)) {
      cursor = *escapedEnd;
      continue;
    }

    if (sourceBytes[cursor] == '\n')
      return cursor + 1;
    if (sourceBytes[cursor] == '\r') {
      ++cursor;
      if (cursor < sourceBytes.size() && sourceBytes[cursor] == '\n')
        ++cursor;
      return cursor;
    }

    ++cursor;
  }
  return sourceBytes.size();
}

/// Return the next non-comment, non-whitespace token before `limit`.
static std::optional<size_t> findNextSignificantToken(
    StringRef sourceBytes, ArrayRef<RawSourceToken> tokens, size_t beginIndex,
    uint64_t limit, const LangOptions &lexLang) {
  for (size_t index = beginIndex; index < tokens.size(); ++index) {
    if (tokens[index].begin >= limit)
      return std::nullopt;
    if (isCommentToken(tokens[index]) ||
        isRawWhitespaceToken(sourceBytes, tokens[index], lexLang))
      continue;
    return index;
  }
  return std::nullopt;
}

/// Return whether `kind` is one Clang string-literal preprocessing token.
static bool isStringLiteralToken(tok::TokenKind kind) {
  switch (kind) {
  case tok::string_literal:
  case tok::wide_string_literal:
  case tok::utf8_string_literal:
  case tok::utf16_string_literal:
  case tok::utf32_string_literal:
    return true;
  default:
    return false;
  }
}

/// Return whether `third` completes one translation-phase-one trigraph.
static bool isTrigraphThirdCharacter(char third) {
  switch (third) {
  case '=':
  case '/':
  case '\'':
  case '(':
  case ')':
  case '!':
  case '<':
  case '>':
  case '-':
    return true;
  default:
    return false;
  }
}

/// Recover a simple direct quoted include path from one operand token.
///
/// Source-graph alias proof deliberately accepts only the same conservative
/// spelling class it can compare byte-for-byte: an unprefixed quoted token with
/// no escapes, physical newlines, or enabled trigraphs.  Any enabled trigraph
/// would change the header-name spelling during translation phase one, so raw
/// byte comparison would not prove path identity.  All other include operands
/// remain explicit but unproven.
static std::optional<std::string>
recoverSimpleQuotedIncludePath(StringRef sourceBytes,
                               const RawSourceToken &operand,
                               const LangOptions &lexLang) {
  if (!isStringLiteralToken(operand.kind) || operand.end > sourceBytes.size())
    return std::nullopt;
  StringRef spelling = sourceBytes.slice(operand.begin, operand.end);
  if (spelling.size() < 2 || spelling.front() != '"' ||
      spelling.back() != '"')
    return std::nullopt;

  StringRef path = spelling.drop_front().drop_back();
  for (size_t index = 0; index < path.size(); ++index) {
    if (path[index] == '\\' || path[index] == '\n' || path[index] == '\r')
      return std::nullopt;
    if (lexLang.Trigraphs && index + 2 < path.size() && path[index] == '?' &&
        path[index + 1] == '?' &&
        isTrigraphThirdCharacter(path[index + 2]))
      return std::nullopt;
  }
  return path.str();
}

/// Classify one directive head and, for direct `include`, its exact operand.
static void classifyDirectiveHeadAndOperand(
    StringRef sourceBytes, ArrayRef<RawSourceToken> tokens,
    size_t hashTokenIndex, uint64_t directiveEnd, const LangOptions &lexLang,
    PreprocessingDirectiveLine &directive) {
  std::optional<size_t> headIndex =
      findNextSignificantToken(sourceBytes, tokens, hashTokenIndex + 1,
                               directiveEnd, lexLang);
  if (!headIndex)
    return;

  const RawSourceToken &head = tokens[*headIndex];
  if (head.kind == tok::raw_identifier || head.kind == tok::identifier) {
    directive.headKind = PreprocessingDirectiveHeadKind::Identifier;
    directive.keyword = removeEscapedNewlines(
        sourceBytes.slice(head.begin, head.end), lexLang);
  } else if (head.kind == tok::numeric_constant) {
    directive.headKind = PreprocessingDirectiveHeadKind::Numeric;
    return;
  } else {
    directive.headKind = PreprocessingDirectiveHeadKind::Other;
    return;
  }

  if (directive.keyword != "include")
    return;
  directive.includeOperandKind = PreprocessingIncludeOperandKind::Other;

  std::optional<size_t> operandIndex =
      findNextSignificantToken(sourceBytes, tokens, *headIndex + 1,
                               directiveEnd, lexLang);
  if (!operandIndex)
    return;
  std::optional<size_t> trailingIndex =
      findNextSignificantToken(sourceBytes, tokens, *operandIndex + 1,
                               directiveEnd, lexLang);
  if (trailingIndex)
    return;

  std::optional<std::string> path = recoverSimpleQuotedIncludePath(
      sourceBytes, tokens[*operandIndex], lexLang);
  if (!path)
    return;
  directive.includeOperandKind =
      PreprocessingIncludeOperandKind::SimpleQuotedHeader;
  directive.simpleQuotedIncludePath = std::move(path);
}

/// Return whether `[begin,end)` is contained in one directive logical line.
static bool containedInDirectiveLine(
    ArrayRef<PreprocessingDirectiveLine> directives, uint64_t begin,
    uint64_t end) {
  for (const PreprocessingDirectiveLine &directive : directives) {
    if (directive.begin <= begin && end <= directive.end)
      return true;
  }
  return false;
}

/// Recover the closing token of one standard `_Pragma` expression.
static std::optional<size_t>
findStandardPragmaOperatorEnd(StringRef sourceBytes,
                              ArrayRef<RawSourceToken> tokens,
                              size_t nameTokenIndex,
                              const LangOptions &lexLang) {
  const uint64_t limit = std::numeric_limits<uint64_t>::max();
  std::optional<size_t> leftParenIndex =
      findNextSignificantToken(sourceBytes, tokens, nameTokenIndex + 1, limit,
                               lexLang);
  if (!leftParenIndex || tokens[*leftParenIndex].kind != tok::l_paren)
    return std::nullopt;
  std::optional<size_t> stringIndex =
      findNextSignificantToken(sourceBytes, tokens, *leftParenIndex + 1, limit,
                               lexLang);
  if (!stringIndex || !isStringLiteralToken(tokens[*stringIndex].kind))
    return std::nullopt;
  std::optional<size_t> rightParenIndex =
      findNextSignificantToken(sourceBytes, tokens, *stringIndex + 1, limit,
                               lexLang);
  if (!rightParenIndex || tokens[*rightParenIndex].kind != tok::r_paren)
    return std::nullopt;
  return rightParenIndex;
}

/// Recover the balanced closing token of one Microsoft `__pragma` expression.
static std::optional<size_t>
findMicrosoftPragmaOperatorEnd(StringRef sourceBytes,
                               ArrayRef<RawSourceToken> tokens,
                               size_t nameTokenIndex,
                               const LangOptions &lexLang) {
  const uint64_t limit = std::numeric_limits<uint64_t>::max();
  std::optional<size_t> tokenIndex =
      findNextSignificantToken(sourceBytes, tokens, nameTokenIndex + 1, limit,
                               lexLang);
  if (!tokenIndex || tokens[*tokenIndex].kind != tok::l_paren)
    return std::nullopt;

  unsigned depth = 1;
  size_t searchIndex = *tokenIndex + 1;
  while (true) {
    tokenIndex = findNextSignificantToken(sourceBytes, tokens, searchIndex,
                                          limit, lexLang);
    if (!tokenIndex)
      return std::nullopt;
    if (tokens[*tokenIndex].kind == tok::l_paren) {
      if (depth == std::numeric_limits<unsigned>::max())
        return std::nullopt;
      ++depth;
    } else if (tokens[*tokenIndex].kind == tok::r_paren) {
      --depth;
      if (depth == 0)
        return tokenIndex;
    }
    searchIndex = *tokenIndex + 1;
  }
}

/// Inventory directly spelled `_Pragma` and `__pragma` expressions outside
/// directive lines.
static std::vector<PreprocessingPragmaOperatorInterval>
findPragmaOperators(StringRef sourceBytes, ArrayRef<RawSourceToken> tokens,
                    ArrayRef<PreprocessingDirectiveLine> directives,
                    const LangOptions &lexLang) {
  std::vector<PreprocessingPragmaOperatorInterval> operators;
  for (size_t tokenIndex = 0; tokenIndex < tokens.size(); ++tokenIndex) {
    const RawSourceToken &nameToken = tokens[tokenIndex];
    if (nameToken.kind != tok::raw_identifier &&
        nameToken.kind != tok::identifier)
      continue;

    const std::string normalizedName = removeEscapedNewlines(
        sourceBytes.slice(nameToken.begin, nameToken.end), lexLang);
    std::optional<size_t> rightParenIndex;
    PreprocessingPragmaOperatorKind operatorKind =
        PreprocessingPragmaOperatorKind::StandardPragma;
    if (normalizedName == "_Pragma") {
      rightParenIndex = findStandardPragmaOperatorEnd(
          sourceBytes, tokens, tokenIndex, lexLang);
    } else if (normalizedName == "__pragma" && lexLang.MicrosoftExt) {
      operatorKind = PreprocessingPragmaOperatorKind::MicrosoftPragma;
      rightParenIndex = findMicrosoftPragmaOperatorEnd(
          sourceBytes, tokens, tokenIndex, lexLang);
    } else {
      continue;
    }
    if (!rightParenIndex)
      continue;

    const uint64_t operatorBegin = nameToken.begin;
    const uint64_t operatorEnd = tokens[*rightParenIndex].end;
    if (!sourceRangeValid(sourceBytes, operatorBegin, operatorEnd) ||
        containedInDirectiveLine(directives, operatorBegin, operatorEnd))
      continue;

    operators.push_back(PreprocessingPragmaOperatorInterval{
        operatorKind, operatorBegin, operatorEnd});
  }
  return operators;
}

} // namespace

bool sourceTextEndsWithNonSplicedPhysicalNewline(
    StringRef sourceBytes, const LangOptions &lexLang) {
  if (sourceBytes.empty())
    return false;

  size_t newlineBegin = sourceBytes.size() - 1;
  const char last = sourceBytes.back();
  if (last == '\n' && newlineBegin > 0 && sourceBytes[newlineBegin - 1] == '\r')
    --newlineBegin;
  else if (last != '\n' && last != '\r')
    return false;

  size_t cursor = newlineBegin;
  while (cursor > 0 &&
         (sourceBytes[cursor - 1] == ' ' ||
          sourceBytes[cursor - 1] == '\t' ||
          sourceBytes[cursor - 1] == '\v' ||
          sourceBytes[cursor - 1] == '\f')) {
    --cursor;
  }

  std::optional<size_t> introducer;
  if (cursor > 0 && sourceBytes[cursor - 1] == '\\') {
    introducer = cursor - 1;
  } else if (lexLang.Trigraphs && cursor >= 3 &&
             sourceBytes[cursor - 3] == '?' &&
             sourceBytes[cursor - 2] == '?' &&
             sourceBytes[cursor - 1] == '/') {
    introducer = cursor - 3;
  }

  if (!introducer)
    return true;
  std::optional<size_t> spliceEnd =
      escapedNewlineEnd(sourceBytes, *introducer, lexLang);
  return !spliceEnd || *spliceEnd != sourceBytes.size();
}

bool sourceTextEndsAtLogicalLineBeginning(StringRef sourceBytes,
                                          const LangOptions &lexLang) {
  size_t end = sourceBytes.size();
  while (end > 0 &&
         (sourceBytes[end - 1] == ' ' || sourceBytes[end - 1] == '\t' ||
          sourceBytes[end - 1] == '\v' || sourceBytes[end - 1] == '\f')) {
    --end;
  }
  return sourceTextEndsWithNonSplicedPhysicalNewline(
      sourceBytes.take_front(end), lexLang);
}

bool insertionBeginsWithNonSplicedPhysicalNewline(
    StringRef sourcePrefix, StringRef insertion, const LangOptions &lexLang) {
  if (insertion.empty() ||
      (insertion.front() != '\n' && insertion.front() != '\r'))
    return false;

  size_t cursor = sourcePrefix.size();
  while (cursor > 0 &&
         (sourcePrefix[cursor - 1] == ' ' ||
          sourcePrefix[cursor - 1] == '\t' ||
          sourcePrefix[cursor - 1] == '\v' ||
          sourcePrefix[cursor - 1] == '\f')) {
    --cursor;
  }
  if (cursor > 0 && sourcePrefix[cursor - 1] == '\\')
    return false;
  if (lexLang.Trigraphs && cursor >= 3 && sourcePrefix[cursor - 3] == '?' &&
      sourcePrefix[cursor - 2] == '?' && sourcePrefix[cursor - 1] == '/') {
    return false;
  }
  return true;
}

PreprocessingDirectiveScanResult
scanPreprocessingDirectives(StringRef sourceBytes,
                            const LangOptions &lexLang) {
  PreprocessingDirectiveScanResult result;
  std::vector<RawSourceToken> tokens =
      lexRawSourceTokens(sourceBytes, lexLang, result.diagnostics);
  result.lexicalTokenIntervals.reserve(tokens.size());
  for (const RawSourceToken &token : tokens) {
    // Comments and optional keep-whitespace raw tokens are trivia components,
    // not direct A-token spellings.  Excluding them also preserves byte-level
    // boundaries inside ordinary whitespace when a caller configures the raw
    // lexer to emit an entire whitespace run as one token.
    if (isCommentToken(token) ||
        isRawWhitespaceToken(sourceBytes, token, lexLang)) {
      continue;
    }
    result.lexicalTokenIntervals.push_back(
        PreprocessingLexicalTokenInterval{token.begin, token.end});
  }
  result.triviaIntervals = findPreprocessingTriviaIntervals(
      sourceBytes, tokens, lexLang, result.indivisibleTriviaIntervals);
  std::vector<DirectiveIntroducer> introducers = findDirectiveIntroducers(
      sourceBytes, tokens, lexLang, result.diagnostics);

  result.directives.reserve(introducers.size());
  for (const DirectiveIntroducer &introducer : introducers) {
    if (introducer.tokenIndex >= tokens.size()) {
      result.diagnostics.push_back(
          "directive introducer token index is out of range");
      continue;
    }

    const RawSourceToken &hashToken = tokens[introducer.tokenIndex];
    const uint64_t directiveEnd = findDirectiveLogicalEnd(
        sourceBytes, hashToken.begin, tokens, introducer.tokenIndex, lexLang);
    if (directiveEnd <= hashToken.begin || directiveEnd > sourceBytes.size()) {
      result.diagnostics.push_back(
          llvm::formatv("could not recover complete directive at byte {0}",
                        hashToken.begin)
              .str());
      continue;
    }

    PreprocessingDirectiveLine directive;
    directive.begin = introducer.prefixBegin;
    directive.introducerBegin = hashToken.begin;
    directive.introducerEnd = hashToken.end;
    directive.end = directiveEnd;
    classifyDirectiveHeadAndOperand(sourceBytes, tokens, introducer.tokenIndex,
                                    directiveEnd, lexLang, directive);
    if (!directive.IsValid()) {
      result.diagnostics.push_back(
          llvm::formatv("discarded malformed directive interval [{0},{1})",
                        directive.begin, directive.end)
              .str());
      continue;
    }
    result.directives.push_back(std::move(directive));
  }

  result.pragmaOperators =
      findPragmaOperators(sourceBytes, tokens, result.directives, lexLang);
  return result;
}

} // namespace refold
} // namespace clang
