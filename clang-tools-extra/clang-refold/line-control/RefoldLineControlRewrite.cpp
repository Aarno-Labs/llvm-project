//===--- RefoldLineControlRewrite.cpp --------------------------*- C++ -*-===//
//
// Source-line directive parsing and rewriting helpers.
//
// Include materialization and expansion fallback share these definitions when
// they need to parse, preserve, or rewrite source-authored line-control
// directives.  The implementation lives in one translation unit so every caller
// uses the same deterministic parsing rules.
//
//===----------------------------------------------------------------------===//

#include "line-control/SourceLineDirectiveHelpers.h"
#include "proof/RefoldOwnerStateProof.h"

namespace clang {
namespace refold {

bool isWsOrCompleteCommentTrivia(StringRef text) {
  size_t i = 0;
  const size_t n = text.size();

  while (i < n) {
    // Ordinary whitespace is inert and may be skipped byte-for-byte.
    if (stringutils::isWs(text[i])) {
      ++i;
      continue;
    }

    // The only non-whitespace spelling admitted here is a complete comment.
    // A lone '/' or any other token spelling is therefore a hard rejection.
    if (i + 1 >= n || text[i] != '/')
      return false;

    if (text[i + 1] == '*') {
      i += 2;
      bool closed = false;
      while (i + 1 < n) {
        if (text[i] == '*' && text[i + 1] == '/') {
          i += 2;
          closed = true;
          break;
        }
        ++i;
      }

      // Do not treat a malformed block comment as trivia; fail closed so the
      // caller can choose a wider structural fallback or terminal B.
      if (!closed)
        return false;

      continue;
    }

    if (text[i + 1] == '/') {
      i += 2;

      // A line comment is complete at the physical newline, or at EOF for the
      // isolated gap buffer. The caller preserves the original bytes, so the
      // newline, if present, is retained.
      while (i < n && text[i] != '\n')
        ++i;
      if (i < n)
        ++i;

      continue;
    }

    return false;
  }

  return true;
}

bool parseLiteralEmptyConditionalDirectiveLine(StringRef line,
                                                      unsigned &depth) {
  if (line.ends_with("\n"))
    line = line.drop_back();
  if (line.ends_with("\r"))
    line = line.drop_back();

  // A trailing backslash would splice this physical line with the next one.
  // That makes the gap non-local, so it is outside the preserved-trivia proof.
  StringRef noTrailingHorizontalWs = line.rtrim(" \t\v\f");
  if (noTrailingHorizontalWs.ends_with("\\"))
    return false;

  StringRef rest = line.ltrim(" \t\v\f");
  if (!rest.consume_front("#"))
    return false;
  rest = rest.ltrim(" \t\v\f");

  // Keep directive recognition syntactic and state-free. We only need the
  // directive keyword and the remaining literal condition, not macro expansion.
  StringRef keyword = rest.take_while([](char c) {
    return ('a' <= c && c <= 'z') || ('A' <= c && c <= 'Z') ||
           ('0' <= c && c <= '9') || c == '_';
  });
  rest = rest.drop_front(keyword.size()).trim(" \t\v\f");

  auto isLiteralBit = [](StringRef text) { return text == "0" || text == "1"; };

  if (keyword == "if") {
    if (!isLiteralBit(rest))
      return false;
    ++depth;
    return true;
  }

  if (keyword == "elif") {
    return depth > 0 && isLiteralBit(rest);
  }

  if (keyword == "else") {
    return depth > 0 && rest.empty();
  }

  if (keyword == "endif") {
    if (depth == 0 || !rest.empty())
      return false;
    --depth;
    return true;
  }

  return false;
}

bool isPreservableIncludeClosureGapTrivia(StringRef text) {
  if (isWsOrCompleteCommentTrivia(text))
    return true;

  size_t i = 0;
  const size_t n = text.size();
  unsigned depth = 0;
  bool sawConditionalDirective = false;

  auto skipCompleteComment = [&](size_t &cursor) -> bool {
    if (cursor + 1 >= n || text[cursor] != '/')
      return false;

    if (text[cursor + 1] == '*') {
      cursor += 2;
      while (cursor + 1 < n) {
        if (text[cursor] == '*' && text[cursor + 1] == '/') {
          cursor += 2;
          return true;
        }
        ++cursor;
      }
      return false;
    }

    if (text[cursor + 1] == '/') {
      cursor += 2;
      while (cursor < n && text[cursor] != '\n')
        ++cursor;
      if (cursor < n)
        ++cursor;
      return true;
    }

    return false;
  };

  bool atLineStart = true;
  while (i < n) {
    if (stringutils::isWs(text[i])) {
      // Directives are only admitted at physical line start, modulo horizontal
      // whitespace already consumed by the directive parser.
      atLineStart = text[i] == '\n';
      ++i;
      continue;
    }

    if (text[i] == '/') {
      const size_t before = i;
      if (!skipCompleteComment(i))
        return false;

      // A line comment consumes its terminating newline when present, so the
      // next non-whitespace byte is physically at the start of a new line. A
      // block comment may contain newlines too; preserve directive legality by
      // checking the skipped spelling.
      StringRef skipped = text.slice(before, i);
      atLineStart = skipped.ends_with("\n");
      continue;
    }

    // Do not accept a '#' embedded after real spelling on the same line. That
    // would not be a preprocessing directive in the preserved source.
    if (!atLineStart && text[i] != '#')
      return false;

    size_t lineEnd = i;
    while (lineEnd < n && text[lineEnd] != '\n')
      ++lineEnd;
    if (lineEnd < n)
      ++lineEnd;

    // Only balanced, empty, literal conditional-control directives are
    // preservable. All other directives are macro/preprocessor state and must
    // remain outside this include-closure proof.
    if (!parseLiteralEmptyConditionalDirectiveLine(text.slice(i, lineEnd),
                                                   depth)) {
      return false;
    }

    sawConditionalDirective = true;
    atLineStart = true;
    i = lineEnd;
  }

  return sawConditionalDirective && depth == 0;
}

bool startsWithPreprocessorDirectiveTrivia(StringRef text) {
  StringRef rest = text.ltrim(" \t\v\f");
  return rest.starts_with("#");
}

std::optional<std::string>
removeLineSplicesForLineControl(StringRef text) {
  std::string out;
  out.reserve(text.size());

  enum class ScanState { Ordinary, StringLiteral, CharLiteral, BlockComment };
  ScanState state = ScanState::Ordinary;

  auto skipSplice = [&](size_t &pos) -> bool {
    return stringutils::skipBackslashNewlineSplice(text, text.size(), pos);
  };

  for (size_t pos = 0; pos < text.size();) {
    if (state == ScanState::Ordinary) {
      if (skipSplice(pos))
        continue;
      if (text[pos] == '"') {
        state = ScanState::StringLiteral;
        out.push_back(text[pos++]);
        continue;
      }
      if (text[pos] == '\'') {
        state = ScanState::CharLiteral;
        out.push_back(text[pos++]);
        continue;
      }
      if (pos + 1 < text.size() && text[pos] == '/' &&
          text[pos + 1] == '*') {
        state = ScanState::BlockComment;
        out.push_back(text[pos++]);
        out.push_back(text[pos++]);
        continue;
      }

      out.push_back(text[pos++]);
      continue;
    }

    if (state == ScanState::BlockComment) {
      // Backslash-newline pairs are deleted before comment replacement
      // replaces a complete block comment with whitespace.  This local proof
      // therefore may delete splice pairs inside a recorded block comment while
      // still leaving the comment itself for the later stringification helper
      // to collapse to one whitespace unit.  Literal splices remain rejected
      // below because they affect token spelling rather than comment erasure.
      if (skipSplice(pos))
        continue;
      if (pos + 1 < text.size() && text[pos] == '*' && text[pos + 1] == '/') {
        out.push_back(text[pos++]);
        out.push_back(text[pos++]);
        state = ScanState::Ordinary;
        continue;
      }
      out.push_back(text[pos++]);
      continue;
    }

    // Preserve literal spellings opaquely, but reject line-spliced literals so
    // this proof does not broaden the earlier literal-aware stringification
    // rule beyond complete source-spelled literal tokens.
    if (skipSplice(pos))
      return std::nullopt;

    char ch = text[pos++];
    out.push_back(ch);
    if (ch == '\\') {
      if (pos >= text.size())
        return std::nullopt;
      out.push_back(text[pos++]);
      continue;
    }
    if ((state == ScanState::StringLiteral && ch == '"') ||
        (state == ScanState::CharLiteral && ch == '\''))
      state = ScanState::Ordinary;
  }

  if (state != ScanState::Ordinary)
    return std::nullopt;
  return out;
}

bool collectLineSpliceLogicalLine(
    StringRef fileText, uint64_t lineBegin, uint64_t limit,
    std::string &logicalLine, uint64_t &afterLine,
    SmallVectorImpl<uint64_t> *logicalLineSourceOffsets) {
  if (lineBegin > limit || limit > fileText.size())
    return false;

  logicalLine.clear();
  if (logicalLineSourceOffsets)
    logicalLineSourceOffsets->clear();

  auto appendSourceByte = [&](uint64_t pos) {
    logicalLine.push_back(fileText[pos]);
    if (logicalLineSourceOffsets)
      logicalLineSourceOffsets->push_back(pos);
  };

  auto skipLineSplice = [&](uint64_t &pos) -> bool {
    return stringutils::skipBackslashNewlineSplice(fileText, limit, pos);
  };

  for (uint64_t pos = lineBegin; pos < limit;) {
    char ch = fileText[pos];

    // C backslash-newline splicing deletes splice pairs before
    // preprocessing directives are recognized.  Require the whole pair to be
    // inside the already-proved source gap so the closure does not depend on
    // bytes outside its envelope.
    if (skipLineSplice(pos))
      continue;

    // A block comment that starts on a directive line is translated to one
    // whitespace character before macro expansion.  Newlines inside the
    // complete comment therefore do not terminate the directive.  Keep the raw
    // comment spelling and source-offset map so later stringification proofs
    // can apply their own comment-to-whitespace normalization to the recorded
    // macro argument.
    if (ch == '/' && pos + 1 < limit && fileText[pos + 1] == '*') {
      appendSourceByte(pos++);
      appendSourceByte(pos++);
      bool closed = false;
      while (pos < limit) {
        if (skipLineSplice(pos))
          continue;
        if (pos + 1 < limit && fileText[pos] == '*' &&
            fileText[pos + 1] == '/') {
          appendSourceByte(pos++);
          appendSourceByte(pos++);
          closed = true;
          break;
        }
        appendSourceByte(pos++);
      }
      if (!closed)
        return false;
      continue;
    }

    if (ch == '\n') {
      afterLine = pos + 1;
      return true;
    }

    appendSourceByte(pos++);
  }

  // Preserve the previous EOF-without-newline behavior for a complete final
  // directive line.  A dangling trailing backslash remains in logicalLine and
  // will be rejected by the strict #line parser below.
  afterLine = limit;
  return true;
}

std::optional<uint64_t> sourceLineDirectiveResumeCountBegin(
    StringRef logicalLine, ArrayRef<uint64_t> logicalLineSourceOffsets) {
  if (logicalLine.size() != logicalLineSourceOffsets.size() ||
      logicalLine.empty())
    return std::nullopt;

  size_t pos = 0;
  stringutils::skipNonNewlineWs(logicalLine, pos);
  if (pos >= logicalLine.size() || logicalLine[pos] != '#')
    return std::nullopt;

  const size_t hashIndex = pos;
  ++pos;
  const size_t afterHash = pos;
  stringutils::skipNonNewlineWs(logicalLine, pos);

  if (pos < logicalLine.size() && logicalLine.substr(pos).starts_with("line")) {
    const size_t afterLineKeyword = pos + StringRef("line").size();
    if (afterLineKeyword < logicalLine.size() &&
        stringutils::isIdentPart(logicalLine[afterLineKeyword]))
      return std::nullopt;
    if (afterLineKeyword >= logicalLine.size() ||
        !stringutils::isNonNewlineWs(logicalLine[afterLineKeyword]))
      return std::nullopt;

    size_t afterSeparator = afterLineKeyword;
    stringutils::skipNonNewlineWs(logicalLine, afterSeparator);
    if (afterSeparator == afterLineKeyword)
      return std::nullopt;

    return logicalLineSourceOffsets[afterSeparator - 1] + 1;
  }

  if (pos < logicalLine.size() && isDigit(logicalLine[pos])) {
    // Numeric line markers may be written either as `#123` or `# 123`.  When
    // whitespace separates `#` from the number, count from just after that
    // whitespace; otherwise count from just after `#` itself.
    if (pos > afterHash)
      return logicalLineSourceOffsets[pos - 1] + 1;
    return logicalLineSourceOffsets[hashIndex] + 1;
  }

  return std::nullopt;
}

size_t countLineControlDirectiveBodyPhysicalNewlines(
    StringRef fileText, uint64_t countBegin, uint64_t afterLine) {
  if (countBegin >= afterLine || afterLine > fileText.size())
    return 0;

  size_t count = stringutils::countNewlines(fileText, countBegin, afterLine);
  if (count != 0 && afterLine != 0 && fileText[afterLine - 1] == '\n')
    --count;
  return count;
}

bool sourcePrefixMayContainLineControlDirective(StringRef fileText,
                                                       uint64_t limit) {
  limit = std::min<uint64_t>(limit, fileText.size());

  for (uint64_t lineBegin = 0; lineBegin < limit;) {
    std::string logicalLine;
    uint64_t afterLine = lineBegin;
    if (!collectLineSpliceLogicalLine(fileText, lineBegin, limit, logicalLine,
                                  afterLine))
      return true;

    StringRef rest = StringRef(logicalLine).ltrim(" \t\v\f");
    if (rest.consume_front("#")) {
      rest = rest.ltrim(" \t\v\f");
      if (rest.starts_with("line") &&
          (rest.size() == StringRef("line").size() ||
           !stringutils::isIdentPart(rest[StringRef("line").size()])))
        return true;
      if (!rest.empty() && '0' <= rest.front() && rest.front() <= '9')
        return true;
    }

    if (afterLine <= lineBegin)
      return true;
    lineBegin = afterLine;
  }

  return false;
}

// Filename string-literal decoding lives in RefoldLineControlFilename.cpp so
// include replay and source-line rewrite proofs share exactly one grammar.
std::optional<ParsedSourceLineDirectiveLogicalLine>
parseSourceLineDirectiveLogicalLine(StringRef line,
                                    StringRef currentFileSpelling) {
  if (line.ends_with("\n"))
    line = line.drop_back();
  if (line.ends_with("\r"))
    line = line.drop_back();

  if (line.rtrim(" \t\v\f").ends_with("\\"))
    return std::nullopt;

  StringRef rest = line.ltrim(" \t\v\f");
  if (!rest.consume_front("#"))
    return std::nullopt;
  rest = rest.ltrim(" \t\v\f");

  // Accept both standard-looking `#line 123 "file"` and the numeric
  // preprocessed line-marker form `# 123 "file"`.  Both establish the same
  // logical file/line state for the following source line, but neither leaves
  // ordinary PP tokens that the refold map can recover from A/B.
  bool isNumericLineMarker = false;
  if (rest.consume_front("line")) {
    if (!rest.empty() && stringutils::isIdentPart(rest.front()))
      return std::nullopt;
    if (rest.empty() || !stringutils::isWs(rest.front()))
      return std::nullopt;
    rest = rest.ltrim(" \t\v\f");
  } else if (!rest.empty() && '0' <= rest.front() && rest.front() <= '9') {
    isNumericLineMarker = true;
  } else {
    return std::nullopt;
  }

  StringRef digits = rest.take_while([](char c) { return '0' <= c && c <= '9'; });
  if (digits.empty())
    return std::nullopt;

  size_t lineAfterDirective = 0;
  if (digits.getAsInteger(10, lineAfterDirective) || lineAfterDirective == 0)
    return std::nullopt;
  rest = rest.drop_front(digits.size()).ltrim(" \t\v\f");

  std::string fileSpelling = currentFileSpelling.str();
  bool hasExplicitFileSpelling = false;
  if (!rest.empty()) {
    hasExplicitFileSpelling = true;
    std::optional<std::string> parsedFileSpelling =
        parseLineControlFilenameLiteral(rest);
    if (!parsedFileSpelling)
      return std::nullopt;
    fileSpelling = std::move(*parsedFileSpelling);
  }

  std::string lineMarkerFlags;
  rest = rest.ltrim(" \t\v\f");
  if (!rest.empty()) {
    // Clang accepts extra preprocessing tokens after the filename operand of
    // the canonical `#line` spelling, diagnoses them, and preserves only the
    // first string literal as the presumed filename.  Source-only gap repair
    // must model that state rather than reject the whole owner envelope: a
    // macro-expanded filename can legally produce adjacent string literals
    // such as `#line 123 "gap" ".c"`, where the suffix observes `"gap"`.
    // Keep this limited to canonical `#line` with an explicit filename.  The
    // numeric line-marker spelling uses trailing tokens as semantic flags and
    // must remain strictly parsed below.
    if (!isNumericLineMarker && hasExplicitFileSpelling)
      rest = StringRef();

    if (rest.empty())
      return ParsedSourceLineDirectiveLogicalLine{
          lineAfterDirective, fileSpelling,
          isNumericLineMarker && hasExplicitFileSpelling, lineMarkerFlags};

    if (!isNumericLineMarker)
      return std::nullopt;

    // Clang/GCC line markers may carry flags after the filename.  Replaying
    // flag 1 (new file), 3 (system header), and 4 (extern C system header) is
    // locally deterministic because those flags describe the state that the
    // following suffix observes.  Flag 2 pops the preprocessor include stack;
    // proving that stack operation after moving the directive would require
    // additional context, so leave that form outside this proof class.
    bool sawFlag1 = false;
    bool sawFlag3 = false;
    bool sawFlag4 = false;
    while (!rest.empty()) {
      StringRef flagText = rest.take_while([](char c) {
        return '0' <= c && c <= '9';
      });
      if (flagText.empty())
        return std::nullopt;

      unsigned flag = 0;
      if (flagText.getAsInteger(10, flag))
        return std::nullopt;
      switch (flag) {
      case 1:
        if (sawFlag1 || sawFlag3 || sawFlag4)
          return std::nullopt;
        sawFlag1 = true;
        break;
      case 3:
        if (sawFlag3 || sawFlag4)
          return std::nullopt;
        sawFlag3 = true;
        break;
      case 4:
        if (!sawFlag3 || sawFlag4)
          return std::nullopt;
        sawFlag4 = true;
        break;
      default:
        return std::nullopt;
      }

      if (!lineMarkerFlags.empty())
        lineMarkerFlags.push_back(' ');
      lineMarkerFlags += std::to_string(flag);

      rest = rest.drop_front(flagText.size());
      if (rest.empty())
        break;
      if (!stringutils::isWs(rest.front()))
        return std::nullopt;
      rest = rest.ltrim(" \t\v\f");
    }
  }

  return ParsedSourceLineDirectiveLogicalLine{
      lineAfterDirective, fileSpelling,
      isNumericLineMarker && hasExplicitFileSpelling, lineMarkerFlags};
}

bool sourceLineDirectiveMacroHasMaterializedPPTokens(
    const RefoldModel::MacroInvocation &macro) {
  if (macro.cover.IsValid())
    return true;
  for (const RefoldModel::PPSpan &span : macro.stringifySpans)
    if (span.IsValid())
      return true;
  for (const RefoldModel::PPSpan &span : macro.pasteSpans)
    if (span.IsValid())
      return true;
  return false;
}

bool sourceSuffixMayObservePresumedFileSpelling(
    const RefoldModel &model, StringRef file, uint64_t resumeOffset,
    llvm::function_ref<bool(StringRef, StringRef)> pathsEqual,
    StringRef fileText) {
  auto findMacroById = [&](uint64_t id)
      -> const RefoldModel::MacroInvocation * {
    for (const RefoldModel::MacroInvocation &candidate :
         model.GetMacroInvocations())
      if (candidate.id == id)
        return &candidate;
    return nullptr;
  };

  std::function<bool(const RefoldModel::MacroInvocation &,
                     SmallVectorImpl<uint64_t> &)>
      invocationMayOccurInSuffix =
          [&](const RefoldModel::MacroInvocation &macro,
              SmallVectorImpl<uint64_t> &activeIds) -> bool {
    if (std::find(activeIds.begin(), activeIds.end(), macro.id) !=
        activeIds.end())
      return true;

    activeIds.push_back(macro.id);
    auto popAndReturn = [&](bool value) {
      activeIds.pop_back();
      return value;
    };

    if (macro.invFile && !macro.invFile->empty() &&
        pathsEqual(*macro.invFile, file)) {
      if (!macro.invB)
        return popAndReturn(true);
      if (*macro.invB >= resumeOffset)
        return popAndReturn(true);
    }

    if (macro.callerMacroId) {
      const RefoldModel::MacroInvocation *caller =
          findMacroById(*macro.callerMacroId);
      if (!caller)
        return popAndReturn(true);
      return popAndReturn(invocationMayOccurInSuffix(*caller, activeIds));
    }

    if (!macro.invFile || macro.invFile->empty())
      return popAndReturn(true);
    return popAndReturn(false);
  };

  for (const RefoldModel::MacroInvocation &macro :
       model.GetMacroInvocations()) {
    if (macro.name != "__FILE__" && macro.name != "__FILE_NAME__")
      continue;

    SmallVector<uint64_t, 8> activeIds;
    if (invocationMayOccurInSuffix(macro, activeIds))
      return true;
  }

  if (resumeOffset < fileText.size()) {
    StringRef suffix = fileText.substr(static_cast<size_t>(resumeOffset));
    if (suffix.contains("__FILE__") || suffix.contains("__FILE_NAME__"))
      return true;
  }

  return false;
}

std::optional<size_t>
findAdmittedLineControlRawStringifyEscapeEnd(StringRef text,
                                             size_t backslashPos) {
  if (backslashPos >= text.size() || text[backslashPos] != '\\' ||
      backslashPos + 1 >= text.size())
    return std::nullopt;

  size_t cursor = backslashPos + 1;
  const char escaped = text[cursor];
  if (escaped == '\n' || escaped == '\r')
    return std::nullopt;

  if ('0' <= escaped && escaped <= '7') {
    unsigned value = 0;
    unsigned digits = 0;
    while (cursor < text.size() && digits < 3 && '0' <= text[cursor] &&
           text[cursor] <= '7') {
      value = (value * 8) + static_cast<unsigned>(text[cursor] - '0');
      ++cursor;
      ++digits;
    }
    if (!isReplayableLineControlNumericFilenameByte(value))
      return std::nullopt;
    return cursor;
  }

  if (escaped == 'x') {
    StringRef tail = text.substr(cursor + 1);
    std::optional<unsigned> value =
        decodeBoundedLineControlHexEscapeValue(tail);
    if (!value || !isReplayableLineControlNumericFilenameByte(*value))
      return std::nullopt;
    return text.size() - tail.size();
  }

  if (escaped == 'u' || escaped == 'U') {
    StringRef tail = text.substr(cursor + 1);
    std::optional<uint32_t> value =
        decodeLineControlUniversalCharacterNameValue(
            tail, escaped == 'u' ? 4u : 8u);
    if (!value || !isReplayableLineControlUniversalCharacterName(*value))
      return std::nullopt;
    return text.size() - tail.size();
  }

  if (escaped == '\"' || escaped == '?' || escaped == '\\' ||
      stringutils::isNonNewlineWs(escaped))
    return backslashPos + 2;

  switch (escaped) {
  case 'a':
  case 'b':
  case 'e':
  case 'E':
  case 'f':
  case 'n':
  case 'r':
  case 't':
  case 'v':
    return backslashPos + 2;
  case 'u':
  case 'U':
    return std::nullopt;
  default:
    return backslashPos + 2;
  }
}

std::optional<std::string>
stringifyLineControlMacroArgument(StringRef argument) {
  StringRef trimmed = argument.trim(" \t\v\f\r\n");
  std::string stringified = "\"";
  bool hasPayload = false;
  bool pendingSpace = false;

  auto appendStringifiedPayloadByte = [&](char byte) {
    std::string quoted = stringutils::quoteCString(StringRef(&byte, 1));
    stringified.append(quoted.begin() + 1, quoted.end() - 1);
    hasPayload = true;
  };

  auto appendPendingSpace = [&]() {
    if (pendingSpace && hasPayload)
      appendStringifiedPayloadByte(' ');
    pendingSpace = false;
  };

  for (size_t pos = 0; pos < trimmed.size();) {
    char ch = trimmed[pos];

    if (stringutils::isWs(ch)) {
      pendingSpace = hasPayload;
      ++pos;
      continue;
    }

    // Comments are replaced by one whitespace character before macro
    // stringification.  Because collectLineSpliceLogicalLine has already proved
    // that a block comment in a directive gap is complete and wholly inside the
    // logical directive, it is safe to model the whole comment as one pending
    // inter-token space and normalize it with adjacent whitespace below.  Line
    // comments remain outside this local proof because they terminate at the
    // physical directive-line boundary.
    if (ch == '/' && pos + 1 < trimmed.size()) {
      if (trimmed[pos + 1] == '*') {
        const size_t commentBegin = pos;
        pos += 2;
        bool closed = false;
        while (pos + 1 < trimmed.size()) {
          if (trimmed[pos] == '*' && trimmed[pos + 1] == '/') {
            pos += 2;
            closed = true;
            break;
          }
          ++pos;
        }
        if (!closed)
          return std::nullopt;
        if (commentBegin != pos)
          pendingSpace = hasPayload;
        continue;
      }
      if (trimmed[pos + 1] == '/')
        return std::nullopt;
    }

    if (ch == '\\') {
      std::optional<size_t> escapeEnd =
          findAdmittedLineControlRawStringifyEscapeEnd(trimmed, pos);
      if (!escapeEnd)
        return std::nullopt;

      appendPendingSpace();
      // Outside a literal, macro stringification copies this backslash into the
      // generated filename string literal rather than doubling it.  Preserve
      // the whole admitted raw escape spelling and let the strict
      // filename-literal parser decode the same bounded subset that was proved
      // above.
      stringified.append(trimmed.begin() + pos, trimmed.begin() + *escapeEnd);
      hasPayload = true;
      pos = *escapeEnd;
      continue;
    }

    if (ch == '\"' || ch == '\'') {
      appendPendingSpace();

      const char quote = ch;
      const size_t literalBegin = pos;
      ++pos;
      bool closed = false;
      while (pos < trimmed.size()) {
        char litCh = trimmed[pos++];
        if (litCh == '\n' || litCh == '\r')
          return std::nullopt;
        if (litCh == '\\') {
          if (pos >= trimmed.size())
            return std::nullopt;
          char escaped = trimmed[pos++];
          if (escaped == '\n' || escaped == '\r')
            return std::nullopt;
          continue;
        }
        if (litCh == quote) {
          closed = true;
          break;
        }
      }
      if (!closed)
        return std::nullopt;

      for (char literalByte : trimmed.slice(literalBegin, pos))
        appendStringifiedPayloadByte(literalByte);
      continue;
    }

    appendPendingSpace();
    appendStringifiedPayloadByte(ch);
    ++pos;
  }

  stringified.push_back('\"');
  return stringified;
}

std::optional<bool>
lineControlArgumentContainsPPTokens(StringRef argument,
                                    const LangOptions &lang) {
  const SourceLocation baseLoc = SourceLocation::getFromRawEncoding(1);
  std::string lexBuf = argument.str();
  lexBuf.push_back('\0');
  const char *bufStart = lexBuf.data();
  const char *bufEnd = bufStart + argument.size();
  Lexer lexer(baseLoc, lang, bufStart, bufStart, bufEnd);
  lexer.SetCommentRetentionState(true);

  Token token;
  while (true) {
    lexer.LexFromRawLexer(token);
    if (token.is(tok::eof))
      return false;

    if (!token.is(tok::comment))
      return true;

    const size_t tokenBegin = std::min(refoldTokenOffsetFromBase(token, baseLoc),
                                       argument.size());
    const size_t tokenEnd = std::min(refoldTokenEndOffsetFromBase(token, baseLoc),
                                     argument.size());
    StringRef spelling = argument.slice(tokenBegin, tokenEnd);
    if (spelling.starts_with("//") ||
        !rawLexerCommentTokenIsComplete(spelling))
      return std::nullopt;
  }
}

std::optional<std::pair<StringRef, size_t>>
parseLineControlVaOptPayload(StringRef replacement, size_t openParen,
                             const LangOptions &lang) {
  if (openParen >= replacement.size() || replacement[openParen] != '(' ||
      replacement.contains('\n') || replacement.contains('\r'))
    return std::nullopt;

  const SourceLocation baseLoc = SourceLocation::getFromRawEncoding(1);
  std::string lexBuf = replacement.str();
  lexBuf.push_back('\0');
  const char *bufStart = lexBuf.data();
  const char *bufEnd = bufStart + replacement.size();
  Lexer lexer(baseLoc, lang, bufStart, bufStart, bufEnd);
  lexer.SetCommentRetentionState(true);

  bool sawOpenParen = false;
  unsigned depth = 0;
  const size_t payloadBegin = openParen + 1;

  Token token;
  while (true) {
    lexer.LexFromRawLexer(token);
    if (token.is(tok::eof))
      return std::nullopt;

    const size_t tokenBegin = std::min(refoldTokenOffsetFromBase(token, baseLoc),
                                       replacement.size());
    const size_t tokenEnd = std::min(refoldTokenEndOffsetFromBase(token, baseLoc),
                                     replacement.size());

    if (tokenEnd <= openParen)
      continue;
    if (tokenBegin < openParen)
      return std::nullopt;

    if (!sawOpenParen) {
      if (tokenBegin != openParen || token.isNot(tok::l_paren))
        return std::nullopt;
      sawOpenParen = true;
      depth = 1;
      continue;
    }

    if (token.is(tok::comment)) {
      StringRef spelling = replacement.slice(tokenBegin, tokenEnd);
      if (spelling.starts_with("//") ||
          !rawLexerCommentTokenIsComplete(spelling))
        return std::nullopt;
      continue;
    }

    if (token.is(tok::l_paren)) {
      ++depth;
      continue;
    }
    if (token.is(tok::r_paren)) {
      if (depth == 0)
        return std::nullopt;
      --depth;
      if (depth == 0)
        return std::make_pair(replacement.slice(payloadBegin, tokenBegin),
                              tokenEnd);
    }
  }
}

std::optional<std::string> expandLineControlVaOptOperators(
    StringRef replacement, ArrayRef<RefoldModel::MacroDefParam> params,
    ArrayRef<StringRef> invocationArgTexts, const LangOptions &lang) {
  if (params.size() != invocationArgTexts.size())
    return std::nullopt;

  std::optional<bool> variadicTailHasTokens;
  for (size_t i = 0; i != params.size(); ++i) {
    if (!params[i].variadic)
      continue;
    if (variadicTailHasTokens)
      return std::nullopt;
    std::optional<bool> hasTokens =
        lineControlArgumentContainsPPTokens(invocationArgTexts[i], lang);
    if (!hasTokens)
      return std::nullopt;
    variadicTailHasTokens = *hasTokens;
  }

  std::string out;
  out.reserve(replacement.size());
  bool sawVaOpt = false;

  for (size_t pos = 0; pos < replacement.size();) {
    char ch = replacement[pos];

    if (ch == '\n' || ch == '\r')
      return std::nullopt;

    if (ch == '"' || ch == '\'') {
      const size_t literalBegin = pos;
      const char quote = ch;
      ++pos;
      bool closed = false;
      while (pos < replacement.size()) {
        char litCh = replacement[pos++];
        if (litCh == '\n' || litCh == '\r')
          return std::nullopt;
        if (litCh == '\\') {
          if (pos >= replacement.size())
            return std::nullopt;
          ++pos;
          continue;
        }
        if (litCh == quote) {
          closed = true;
          break;
        }
      }
      if (!closed)
        return std::nullopt;
      out.append(replacement.begin() + literalBegin, replacement.begin() + pos);
      continue;
    }

    if (stringutils::isIdentStart(ch)) {
      size_t identEnd = pos + 1;
      while (identEnd < replacement.size() &&
             stringutils::isIdentPart(replacement[identEnd]))
        ++identEnd;
      StringRef ident = replacement.slice(pos, identEnd);
      if (ident != "__VA_OPT__") {
        out.append(ident.begin(), ident.end());
        pos = identEnd;
        continue;
      }

      if (!variadicTailHasTokens)
        return std::nullopt;
      size_t openParen = identEnd;
      stringutils::skipNonNewlineWs(replacement, openParen);
      std::optional<std::pair<StringRef, size_t>> payload =
          parseLineControlVaOptPayload(replacement, openParen, lang);
      if (!payload)
        return std::nullopt;

      sawVaOpt = true;
      if (*variadicTailHasTokens)
        out.append(payload->first.begin(), payload->first.end());
      pos = payload->second;
      continue;
    }

    out.push_back(ch);
    ++pos;
  }

  if (!sawVaOpt)
    return std::nullopt;
  return out;
}

std::optional<std::string> substituteLineControlMacroParameters(
    StringRef replacement, ArrayRef<RefoldModel::MacroDefParam> params,
    ArrayRef<StringRef> invocationArgTexts,
    ArrayRef<RefoldModel::PasteToken> pasteTokens, const LangOptions &lang) {
  if (params.size() != invocationArgTexts.size())
    return std::nullopt;

  // `__VA_OPT__(...)` gates a replacement-list fragment on the presence of
  // tokens in the variadic tail.  Normalize that operator before the ordinary
  // parameter-substitution walk below; the exposed payload still passes through
  // the same formal substitution, stringification, paste-witness replay, and
  // strict final line-control parse as any other replacement-list text.
  if (replacement.contains("__VA_OPT__")) {
    std::optional<std::string> expandedVaOpt =
        expandLineControlVaOptOperators(replacement, params,
                                        invocationArgTexts, lang);
    if (!expandedVaOpt)
      return std::nullopt;
    return substituteLineControlMacroParameters(*expandedVaOpt, params,
                                                invocationArgTexts,
                                                pasteTokens, lang);
  }

  // Variadic formals are safe to substitute in this bounded proof when the
  // producer recorded exactly one argument spelling for the variadic tail.
  // That is the same shape used by the normal macro machinery: `__VA_ARGS__`
  // or a named GNU variadic formal behaves like another formal whose argument
  // range covers the complete tail spelling, including any top-level commas.
  // We do not try to interpret the tail here; the rewritten directive is still
  // parsed by the strict line-control parser, which rejects invalid comma
  // sequences, empty operands, or any unsupported result.

  auto lookupParam = [&](StringRef ident) -> std::optional<StringRef> {
    for (size_t i = 0; i != params.size(); ++i)
      if (params[i].name == ident)
        return invocationArgTexts[i];
    return std::nullopt;
  };

  auto parseReplacementAtom =
      [&](size_t atomBegin)
          -> std::optional<std::tuple<size_t, std::string, bool>> {
    if (atomBegin >= replacement.size())
      return std::nullopt;

    const char atomCh = replacement[atomBegin];
    if (atomCh == '#')
      return std::nullopt;

    // Copy string and character literals opaquely so formal names inside a
    // literal are not substituted as identifiers.  Literals can participate in
    // a pasted-token witness only when the producer recorded the resulting
    // paste spelling, which is still certified by the final line-control parse.
    if (atomCh == '"' || atomCh == '\'') {
      const char quote = atomCh;
      size_t end = atomBegin + 1;
      bool closed = false;
      while (end < replacement.size()) {
        char litCh = replacement[end++];
        if (litCh == '\\') {
          if (end >= replacement.size())
            return std::nullopt;
          ++end;
          continue;
        }
        if (litCh == quote) {
          closed = true;
          break;
        }
      }
      if (!closed)
        return std::nullopt;
      return std::make_tuple(
          end, replacement.slice(atomBegin, end).str(), false);
    }

    if (stringutils::isIdentStart(atomCh)) {
      size_t end = atomBegin + 1;
      while (end < replacement.size() &&
             stringutils::isIdentPart(replacement[end]))
        ++end;
      StringRef ident = replacement.slice(atomBegin, end);
      if (std::optional<StringRef> argText = lookupParam(ident))
        return std::make_tuple(end, argText->str(), true);
      return std::make_tuple(end, ident.str(), false);
    }

    // Consume a small pp-number spelling as one atom so literal paste operands
    // such as `12 ## b` can use the same producer paste-token witness path as
    // formal-parameter operands.  The final strict line-control parse still
    // decides whether the pasted spelling is usable in this directive.
    if (isDigit(atomCh) || atomCh == '.') {
      size_t end = atomBegin + 1;
      while (end < replacement.size() &&
             (stringutils::isIdentPart(replacement[end]) ||
              replacement[end] == '.'))
        ++end;
      return std::make_tuple(end, replacement.slice(atomBegin, end).str(),
                             false);
    }

    // Treat one non-whitespace, non-comment punctuation byte as an atom.
    if (!stringutils::isWs(atomCh) && atomCh != '/')
      return std::make_tuple(atomBegin + 1, std::string(1, atomCh), false);

    return std::nullopt;
  };

  auto appendPasteRunIfPresent = [&](size_t atomBegin, std::string &out,
                                     size_t &nextPos,
                                     size_t &pasteTokenIndex) -> bool {
    std::optional<std::tuple<size_t, std::string, bool>> atom =
        parseReplacementAtom(atomBegin);
    if (!atom)
      return false;

    size_t probe = std::get<0>(*atom);
    stringutils::skipNonNewlineWs(replacement, probe);
    if (probe + 1 >= replacement.size() || replacement[probe] != '#' ||
        replacement[probe + 1] != '#')
      return false;

    // A replacement-list ## run is deterministic here because the producer
    // serialized the exact pasted-token spelling for this invocation.  Consume
    // the whole adjacent paste chain and emit one recorded pasted token, then
    // let the strict line-control parser reject anything that is not a valid
    // directive operand.
    size_t cursor = probe;
    do {
      cursor += 2;
      stringutils::skipNonNewlineWs(replacement, cursor);
      std::optional<std::tuple<size_t, std::string, bool>> rhs =
          parseReplacementAtom(cursor);
      if (!rhs)
        return false;
      cursor = std::get<0>(*rhs);
      probe = cursor;
      stringutils::skipNonNewlineWs(replacement, probe);
    } while (probe + 1 < replacement.size() && replacement[probe] == '#' &&
             replacement[probe + 1] == '#');

    if (pasteTokenIndex >= pasteTokens.size())
      return false;
    out.append(pasteTokens[pasteTokenIndex].spelling.begin(),
               pasteTokens[pasteTokenIndex].spelling.end());
    ++pasteTokenIndex;
    nextPos = cursor;
    return true;
  };

  std::string out;
  out.reserve(replacement.size());
  size_t pasteTokenIndex = 0;
  for (size_t pos = 0; pos < replacement.size();) {
    char ch = replacement[pos];

    size_t pasteRunEnd = pos;
    if (appendPasteRunIfPresent(pos, out, pasteRunEnd, pasteTokenIndex)) {
      pos = pasteRunEnd;
      continue;
    }

    if (ch == '#') {
      // Token pasting is handled above as a whole replacement-list paste run
      // using the producer's exact paste-token witness.  A bare `##` here means
      // the replacement list was malformed or the run could not be proved.
      if (pos + 1 < replacement.size() && replacement[pos + 1] == '#')
        return std::nullopt;

      // The stringification operator is deterministic for complete recorded
      // arguments.  Accept only `# formal` and let the final line-control parser
      // certify that the produced string literal is legal in this directive.
      size_t identBegin = pos + 1;
      stringutils::skipNonNewlineWs(replacement, identBegin);
      if (identBegin >= replacement.size() ||
          !stringutils::isIdentStart(replacement[identBegin]))
        return std::nullopt;
      size_t identEnd = identBegin + 1;
      while (identEnd < replacement.size() &&
             stringutils::isIdentPart(replacement[identEnd]))
        ++identEnd;

      StringRef ident = replacement.slice(identBegin, identEnd);
      std::optional<StringRef> argText = lookupParam(ident);
      if (!argText)
        return std::nullopt;
      std::optional<std::string> stringified =
          stringifyLineControlMacroArgument(*argText);
      if (!stringified)
        return std::nullopt;
      out += *stringified;
      pos = identEnd;
      continue;
    }

    // Comments in a replacement list are translated to whitespace before macro
    // replacement.  The refold map currently gives us the recorded directive
    // spelling rather than a tokenized replacement list, so avoid guessing.
    if (ch == '/' && pos + 1 < replacement.size() &&
        (replacement[pos + 1] == '/' || replacement[pos + 1] == '*'))
      return std::nullopt;

    // Copy string and character literals opaquely so formal names inside a
    // literal are not substituted as identifiers.
    if (ch == '"' || ch == '\'') {
      const char quote = ch;
      out.push_back(ch);
      ++pos;
      bool closed = false;
      while (pos < replacement.size()) {
        char litCh = replacement[pos++];
        out.push_back(litCh);
        if (litCh == '\\') {
          if (pos >= replacement.size())
            return std::nullopt;
          out.push_back(replacement[pos++]);
          continue;
        }
        if (litCh == quote) {
          closed = true;
          break;
        }
      }
      if (!closed)
        return std::nullopt;
      continue;
    }

    if (stringutils::isIdentStart(ch)) {
      size_t end = pos + 1;
      while (end < replacement.size() &&
             stringutils::isIdentPart(replacement[end]))
        ++end;
      StringRef ident = replacement.slice(pos, end);
      if (std::optional<StringRef> argText = lookupParam(ident))
        out.append(argText->begin(), argText->end());
      else
        out.append(ident.begin(), ident.end());
      pos = end;
      continue;
    }

    out.push_back(ch);
    ++pos;
  }

  if (pasteTokenIndex != pasteTokens.size())
    return std::nullopt;

  return out;
}

std::optional<std::string> simpleLineControlMacroReplacementText(
    const RefoldModel &model, const RefoldModel::MacroInvocation &macro,
    ArrayRef<StringRef> invocationArgTexts, const LangOptions &lang) {
  if (macro.subkind != "obj" && macro.subkind != "func")
    return std::nullopt;
  if (!macro.definitionDirectiveId ||
      sourceLineDirectiveMacroHasMaterializedPPTokens(macro))
    return std::nullopt;

  const RefoldModel::MacroDirective *definition = nullptr;
  for (const RefoldModel::MacroDirective &directive :
       model.GetMacroDirectives()) {
    if (directive.id == *macro.definitionDirectiveId) {
      definition = &directive;
      break;
    }
  }
  if (!definition || definition->subkind != "#define")
    return std::nullopt;

  StringRef text = definition->text;
  size_t pos = 0;
  stringutils::skipNonNewlineWs(text, pos);
  if (pos >= text.size() || text[pos] != '#')
    return std::nullopt;
  ++pos;
  stringutils::skipNonNewlineWs(text, pos);

  if (!text.substr(pos).starts_with("define"))
    return std::nullopt;
  pos += StringRef("define").size();
  if (pos < text.size() && stringutils::isIdentPart(text[pos]))
    return std::nullopt;
  stringutils::skipNonNewlineWs(text, pos);

  if (!text.substr(pos).starts_with(macro.name))
    return std::nullopt;
  pos += macro.name.size();
  if (pos < text.size() && stringutils::isIdentPart(text[pos]))
    return std::nullopt;

  if (macro.subkind == "obj") {
    if (!invocationArgTexts.empty())
      return std::nullopt;
  } else {
    if (pos >= text.size() || text[pos] != '(')
      return std::nullopt;
    unsigned depth = 0;
    while (pos < text.size()) {
      char ch = text[pos++];
      if (ch == '(') {
        ++depth;
        continue;
      }
      if (ch == ')') {
        if (depth == 0)
          return std::nullopt;
        --depth;
        if (depth == 0)
          break;
      }
    }
    if (depth != 0)
      return std::nullopt;
  }

  StringRef replacement = text.substr(pos).trim(" \t\v\f\r\n");

  // An empty replacement list is a legitimate zero-token macro expansion.  In
  // a source-only line-control gap it can expose the real directive operands,
  // for example `#line EMPTY 123 "file"`.  Keep the substitution explicit and
  // let the final strict line-control parser decide whether deleting the
  // invocation produces a valid directive.

  // Multi-line replacement lists and line continuations require a real macro
  // expansion model for directive operands.  Keep this proof to one recorded
  // replacement-list line and let the final line-control parser reject anything
  // that is not a valid numeric/string line-control operand sequence.
  if (replacement.contains('\n') || replacement.contains('\r') ||
      replacement.rtrim(" \t\v\f").ends_with("\\"))
    return std::nullopt;

  if ((macro.subkind == "func" && !macro.defParams.empty()) ||
      !macro.pasteTokens.empty()) {
    return substituteLineControlMacroParameters(replacement, macro.defParams,
                                                invocationArgTexts,
                                                macro.pasteTokens, lang);
  }

  if (!invocationArgTexts.empty())
    return std::nullopt;
  return replacement.str();
}

SmallVector<std::pair<size_t, size_t>, 4>
findLineControlMacroNameOccurrences(StringRef text, StringRef name) {
  SmallVector<std::pair<size_t, size_t>, 4> occurrences;
  if (name.empty())
    return occurrences;

  for (size_t pos = 0; pos < text.size();) {
    size_t match = text.find(name, pos);
    if (match == StringRef::npos)
      break;

    const size_t end = match + name.size();
    const bool beginsAtIdentBoundary =
        match == 0 || !stringutils::isIdentPart(text[match - 1]);
    const bool endsAtIdentBoundary =
        end == text.size() || !stringutils::isIdentPart(text[end]);
    if (beginsAtIdentBoundary && endsAtIdentBoundary)
      occurrences.push_back(std::make_pair(match, end));

    pos = match + 1;
  }

  return occurrences;
}

std::optional<std::pair<size_t, size_t>>
findLineControlMacroOccurrenceForChild(
    const RefoldModel &model, const RefoldModel::MacroInvocation &parent,
    const RefoldModel::MacroInvocation &child, StringRef text) {
  if (!child.invText || child.invText->empty())
    return std::nullopt;

  SmallVector<std::pair<size_t, size_t>, 4> occurrences =
      findLineControlMacroNameOccurrences(text, *child.invText);
  if (occurrences.empty())
    return std::nullopt;
  if (occurrences.size() == 1)
    return occurrences.front();

  SmallVector<const RefoldModel::MacroInvocation *, 4> siblings;
  for (const RefoldModel::MacroInvocation &candidate :
       model.GetMacroInvocations()) {
    if (!candidate.callerMacroId || *candidate.callerMacroId != parent.id)
      continue;
    if (!candidate.invText || *candidate.invText != *child.invText)
      continue;
    if (!candidate.invFile || !candidate.invB || !candidate.invE)
      return std::nullopt;
    siblings.push_back(&candidate);
  }

  // Do not guess which occurrence is semantic when the replacement spelling
  // contains additional same-spelled identifiers that the producer did not
  // record as child macro invocations.
  if (siblings.size() != occurrences.size())
    return std::nullopt;

  llvm::sort(siblings, [](const RefoldModel::MacroInvocation *lhs,
                          const RefoldModel::MacroInvocation *rhs) {
    if (*lhs->invFile != *rhs->invFile)
      return lhs->invFile->compare(*rhs->invFile) < 0;
    if (*lhs->invB != *rhs->invB)
      return *lhs->invB < *rhs->invB;
    if (*lhs->invE != *rhs->invE)
      return *lhs->invE < *rhs->invE;
    return lhs->id < rhs->id;
  });

  for (size_t i = 0; i != siblings.size(); ++i)
    if (siblings[i]->id == child.id)
      return occurrences[i];

  return std::nullopt;
}

std::optional<SmallVector<StringRef, 4>>
lineControlNestedMacroInvocationArgTexts(
    const RefoldModel::MacroInvocation &macro) {
  SmallVector<StringRef, 4> args;
  if (macro.subkind != "func" || macro.defParams.empty())
    return args;

  if (!macro.normalizedInvText ||
      macro.normalizedInvArgTextRanges.size() != macro.defParams.size())
    return std::nullopt;

  StringRef normalized = *macro.normalizedInvText;
  for (const RefoldModel::MacroInvocation::OptByteRange &range :
       macro.normalizedInvArgTextRanges) {
    if (!range.first || !range.second || *range.first > *range.second ||
        *range.second > normalized.size())
      return std::nullopt;
    args.push_back(normalized.slice(*range.first, *range.second));
  }

  return args;
}

std::optional<SourceLineDirectiveMacroReplacement>
expandLineControlMacroReplacementText(
    const RefoldModel &model, const RefoldModel::MacroInvocation &macro,
    ArrayRef<StringRef> invocationArgTexts,
    SmallVectorImpl<uint64_t> &activeMacroIds, const LangOptions &lang,
    SourceLineDirectiveBuiltinMacroResolver builtinMacroResolver) {
  if (activeMacroIds.size() > 32)
    return std::nullopt;
  if (std::find(activeMacroIds.begin(), activeMacroIds.end(), macro.id) !=
      activeMacroIds.end())
    return std::nullopt;

  std::optional<std::string> replacement =
      simpleLineControlMacroReplacementText(model, macro, invocationArgTexts, lang);
  if (!replacement) {
    if (!builtinMacroResolver)
      return std::nullopt;

    // Predefined builtins have no defining directive to recover through the
    // normal replacement-list path.  They can still be valid line-control
    // operands when the producer recorded the invocation site and the caller's
    // resolver can prove the concrete spelling from source-location state.
    replacement = builtinMacroResolver(macro);
    if (!replacement)
      return std::nullopt;
  }

  SourceLineDirectiveMacroReplacement out;
  out.text = std::move(*replacement);
  out.macroInvocationIds.push_back(macro.id);

  activeMacroIds.push_back(macro.id);
  struct ChildReplacementPiece {
    size_t begin = 0;
    size_t end = 0;
    uint64_t macroId = 0;
    std::string replacement;
    SmallVector<uint64_t, 4> macroInvocationIds;
  };

  SmallVector<ChildReplacementPiece, 4> childPieces;
  for (const RefoldModel::MacroInvocation &child : model.GetMacroInvocations()) {
    if (!child.callerMacroId || *child.callerMacroId != macro.id)
      continue;
    if (!child.invText || child.invText->empty()) {
      activeMacroIds.pop_back();
      return std::nullopt;
    }

    std::optional<std::pair<size_t, size_t>> occurrence =
        findLineControlMacroOccurrenceForChild(model, macro, child, out.text);
    if (!occurrence) {
      activeMacroIds.pop_back();
      return std::nullopt;
    }

    std::optional<SmallVector<StringRef, 4>> childArgTexts =
        lineControlNestedMacroInvocationArgTexts(child);
    if (!childArgTexts) {
      activeMacroIds.pop_back();
      return std::nullopt;
    }

    std::optional<SourceLineDirectiveMacroReplacement> childReplacement =
        expandLineControlMacroReplacementText(
            model, child, *childArgTexts, activeMacroIds, lang,
            builtinMacroResolver);
    if (!childReplacement) {
      activeMacroIds.pop_back();
      return std::nullopt;
    }

    childPieces.push_back({occurrence->first, occurrence->second, child.id,
                           std::move(childReplacement->text),
                           std::move(childReplacement->macroInvocationIds)});
  }
  activeMacroIds.pop_back();

  if (childPieces.empty())
    return out;

  llvm::sort(childPieces, [](const ChildReplacementPiece &lhs,
                             const ChildReplacementPiece &rhs) {
    if (lhs.begin != rhs.begin)
      return lhs.begin < rhs.begin;
    if (lhs.end != rhs.end)
      return lhs.end < rhs.end;
    return lhs.macroId < rhs.macroId;
  });

  std::string expanded;
  size_t cursor = 0;
  for (const ChildReplacementPiece &piece : childPieces) {
    if (piece.begin < cursor)
      return std::nullopt;
    expanded.append(out.text.begin() + cursor, out.text.begin() + piece.begin);
    expanded += piece.replacement;
    out.macroInvocationIds.append(piece.macroInvocationIds.begin(),
                                  piece.macroInvocationIds.end());
    cursor = piece.end;
  }
  expanded.append(out.text.begin() + cursor, out.text.end());
  out.text = std::move(expanded);
  return out;
}

std::optional<SourceLineDirectiveLogicalLineRewrite>
rewriteSourceLineDirectiveLogicalLineMacros(
    const RefoldModel &model, StringRef file, StringRef logicalLine,
    ArrayRef<uint64_t> logicalLineSourceOffsets,
    llvm::function_ref<bool(StringRef, StringRef)> pathsEqual,
    const LangOptions &lang,
    std::optional<uint64_t> ownerIncludeId,
    SourceLineDirectiveBuiltinMacroResolver builtinMacroResolver) {
  if (logicalLineSourceOffsets.size() != logicalLine.size())
    return std::nullopt;

  bool logicalLineSpellingIsContiguous = true;
  for (size_t i = 1; i < logicalLineSourceOffsets.size(); ++i) {
    if (logicalLineSourceOffsets[i] != logicalLineSourceOffsets[i - 1] + 1) {
      logicalLineSpellingIsContiguous = false;
      break;
    }
  }

  struct ReplacementPiece {
    size_t begin = 0;
    size_t end = 0;
    uint64_t macroId = 0;
    std::string replacement;
    SmallVector<uint64_t, 4> macroInvocationIds;
  };

  auto logicalSliceIsSourceContiguous = [&](size_t begin, size_t end) -> bool {
    if (begin > end || end > logicalLineSourceOffsets.size())
      return false;
    for (size_t i = begin + 1; i < end; ++i)
      if (logicalLineSourceOffsets[i] != logicalLineSourceOffsets[i - 1] + 1)
        return false;
    return true;
  };

  auto findLogicalIndex = [&](uint64_t sourceOffset) -> std::optional<size_t> {
    auto it = std::lower_bound(logicalLineSourceOffsets.begin(),
                               logicalLineSourceOffsets.end(), sourceOffset);
    if (it == logicalLineSourceOffsets.end() || *it != sourceOffset)
      return std::nullopt;
    return static_cast<size_t>(it - logicalLineSourceOffsets.begin());
  };

  auto findLogicalEndIndex =
      [&](uint64_t sourceOffset) -> std::optional<size_t> {
    auto it = std::lower_bound(logicalLineSourceOffsets.begin(),
                               logicalLineSourceOffsets.end(), sourceOffset);
    return static_cast<size_t>(it - logicalLineSourceOffsets.begin());
  };

  SmallVector<ReplacementPiece, 4> pieces;
  for (const RefoldModel::MacroInvocation &macro :
       model.GetMacroInvocations()) {
    // Replacement pieces for nested macro invocations are produced by walking
    // the recorded expansion DAG from their source-spelled root.  Adding them
    // again as independent logical-line pieces would create overlapping edits
    // for forms such as `#line LINE_NO(__LINE__)`, where the builtin is both
    // physically inside the root invocation spelling and semantically a child
    // of that invocation.
    if (macro.callerMacroId)
      continue;
    if (!macro.invFile || macro.invFile->empty() ||
        !pathsEqual(*macro.invFile, file))
      continue;
    if (ownerIncludeId &&
        (!macro.ownerIncludeId || *macro.ownerIncludeId != *ownerIncludeId))
      continue;
    if (!macro.invB || !macro.invE || *macro.invB >= *macro.invE)
      continue;

    std::optional<size_t> beginIndex = findLogicalIndex(*macro.invB);
    std::optional<size_t> endIndex = findLogicalEndIndex(*macro.invE);
    if (!beginIndex || !endIndex || *beginIndex >= *endIndex ||
        *endIndex > logicalLine.size())
      continue;

    StringRef logicalInvocation =
        logicalLine.substr(*beginIndex, *endIndex - *beginIndex);
    const bool macroLogicalSliceIsContiguous =
        logicalSliceIsSourceContiguous(*beginIndex, *endIndex);
    SourceLineDirectiveBuiltinMacroResolver macroBuiltinMacroResolver =
        (logicalLineSpellingIsContiguous || macroLogicalSliceIsContiguous)
            ? builtinMacroResolver
            : nullptr;
    if (!macro.invText)
      continue;
    std::optional<std::string> splicedInvocation =
        removeLineSplicesForLineControl(*macro.invText);
    if (!splicedInvocation || logicalInvocation != *splicedInvocation)
      continue;

    SmallVector<StringRef, 4> invocationArgTexts;
    if (macro.subkind == "func" && !macro.defParams.empty()) {
      if (macro.invArgRanges.size() != macro.defParams.size())
        return std::nullopt;

      for (const RefoldModel::MacroInvocation::OptByteRange &argRange :
           macro.invArgRanges) {
        if (!argRange.first || !argRange.second ||
            *argRange.first > *argRange.second)
          return std::nullopt;

        std::optional<size_t> argBeginIndex = findLogicalIndex(*argRange.first);
        std::optional<size_t> argEndIndex =
            findLogicalEndIndex(*argRange.second);
        if (!argBeginIndex || !argEndIndex || *argBeginIndex > *argEndIndex ||
            *argEndIndex > logicalLine.size())
          return std::nullopt;

        StringRef logicalArg =
            logicalLine.substr(*argBeginIndex, *argEndIndex - *argBeginIndex);

        // Macro argument ranges are recorded in the physical source spelling,
        // while this proof substitutes from the line-spliced directive-logical
        // line.
        // A backslash-newline inside the argument is therefore safe only when
        // the recorded invocation text after backslash-newline splice deletion
        // matches the exact logical slice we are about to substitute.
        const uint64_t relativeArgBegin = *argRange.first - *macro.invB;
        const uint64_t relativeArgEnd = *argRange.second - *macro.invB;
        if (relativeArgBegin > relativeArgEnd ||
            relativeArgEnd > macro.invText->size())
          return std::nullopt;
        std::optional<std::string> splicedArg =
            removeLineSplicesForLineControl(
                macro.invText->slice(static_cast<size_t>(relativeArgBegin),
                                     static_cast<size_t>(relativeArgEnd)));
        if (!splicedArg || logicalArg != *splicedArg)
          return std::nullopt;

        invocationArgTexts.push_back(logicalArg);
      }
    }

    std::optional<SourceLineDirectiveMacroReplacement> replacement;

    // Predefined location macros have no #define directive to recover, but the
    // producer still records their complete callsite in the source-only
    // directive gap.  Accept such a builtin only through the caller-provided
    // resolver after either the whole logical line or this invocation's own
    // logical slice is source-contiguous.  That admits splices outside the
    // builtin callsite, such as `#\\nline __LINE__`, but still fails closed
    // when the builtin invocation itself cannot be mapped to a stable source
    // location spelling.
    if (macroBuiltinMacroResolver) {
      if (std::optional<std::string> builtinReplacement =
              macroBuiltinMacroResolver(macro)) {
        SourceLineDirectiveMacroReplacement builtin;
        builtin.text = std::move(*builtinReplacement);
        builtin.macroInvocationIds.push_back(macro.id);
        replacement = std::move(builtin);
      }
    }

    if (!replacement) {
      SmallVector<uint64_t, 8> activeMacroIds;
      replacement = expandLineControlMacroReplacementText(
          model, macro, invocationArgTexts, activeMacroIds, lang,
          macroBuiltinMacroResolver);
    }
    if (!replacement)
      return std::nullopt;

    pieces.push_back({*beginIndex, *endIndex, macro.id,
                      std::move(replacement->text),
                      std::move(replacement->macroInvocationIds)});
  }

  if (pieces.empty())
    return std::nullopt;

  llvm::sort(pieces, [](const ReplacementPiece &lhs,
                        const ReplacementPiece &rhs) {
    if (lhs.begin != rhs.begin)
      return lhs.begin < rhs.begin;
    if (lhs.end != rhs.end)
      return lhs.end < rhs.end;
    return lhs.macroId < rhs.macroId;
  });

  SourceLineDirectiveLogicalLineRewrite out;
  size_t cursor = 0;
  for (const ReplacementPiece &piece : pieces) {
    if (piece.begin < cursor)
      return std::nullopt;
    out.line.append(logicalLine.begin() + cursor,
                    logicalLine.begin() + piece.begin);
    out.line += piece.replacement;
    out.macroInvocationIds.append(piece.macroInvocationIds.begin(),
                                  piece.macroInvocationIds.end());
    cursor = piece.end;
  }
  out.line.append(logicalLine.begin() + cursor, logicalLine.end());
  return out;
}

std::string
formatSourceLineDirectiveGapResume(const SourceLineDirectiveGapResume &resume) {
  if (resume.lineMarkerFlags.empty())
    return LineDirectiveInserter::FormatLineDirective(resume.lineAtResume,
                                                      resume.fileSpelling);

  std::string result = "# ";
  result += std::to_string(resume.lineAtResume);
  result += " \"";
  result += LineDirectiveInserter::EscapeForLineDirective(resume.fileSpelling);
  result += "\" ";
  result += resume.lineMarkerFlags;
  result += '\n';
  return result;
}

std::optional<SourceLineDirectiveGapResume>
computeSourceLineDirectiveGapResume(
    StringRef fileText, uint64_t gapBegin, uint64_t gapEnd,
    uint64_t resumeOffset, StringRef defaultFileSpelling,
    SourceLineDirectiveLogicalLineRewriter logicalLineRewriter,
    SmallVectorImpl<uint64_t> *acceptedMacroInvocationIds,
    StringRef baseFileSpelling,
    bool allowUnknownFilenameOperand) {
  if (gapBegin >= gapEnd || gapEnd > fileText.size() || resumeOffset < gapEnd ||
      resumeOffset > fileText.size())
    return std::nullopt;

  uint64_t cursor = gapBegin;
  bool atLineStart = stringutils::beginsLineAfterWs(fileText, gapBegin);
  bool sawLineDirective = false;
  size_t lastLineAfterDirective = 0;
  uint64_t lastAfterDirectiveOffset = gapBegin;
  std::string currentFileSpelling = defaultFileSpelling.str();
  std::string currentLineMarkerFlags;
  const bool prefixHasLineControl =
      sourcePrefixMayContainLineControlDirective(fileText, gapBegin);

  auto skipCompleteComment = [&](uint64_t &pos) -> bool {
    if (pos + 1 >= gapEnd || fileText[pos] != '/')
      return false;

    if (fileText[pos + 1] == '*') {
      const uint64_t before = pos;
      pos += 2;
      bool closed = false;
      while (pos + 1 < gapEnd) {
        if (fileText[pos] == '*' && fileText[pos + 1] == '/') {
          pos += 2;
          closed = true;
          break;
        }
        ++pos;
      }
      if (!closed)
        return false;
      StringRef skipped = fileText.slice(before, pos);
      atLineStart = skipped.ends_with("\n") || skipped.ends_with("\r");
      return true;
    }

    if (fileText[pos + 1] == '/') {
      pos += 2;
      while (pos < gapEnd && fileText[pos] != '\n' && fileText[pos] != '\r')
        ++pos;
      if (pos < gapEnd)
        ++pos;
      atLineStart = true;
      return true;
    }

    return false;
  };

  while (cursor < gapEnd) {
    char ch = fileText[cursor];
    if (stringutils::isWs(ch)) {
      // Horizontal indentation before a directive is still directive-line
      // prefix, while a physical newline starts a fresh directive line.
      if (ch == '\n' || ch == '\r')
        atLineStart = true;
      ++cursor;
      continue;
    }

    if (ch == '/') {
      if (!skipCompleteComment(cursor))
        return std::nullopt;
      continue;
    }

    if (ch != '#' || !atLineStart)
      return std::nullopt;

    std::string logicalLine;
    SmallVector<uint64_t, 64> logicalLineSourceOffsets;
    uint64_t afterLine = cursor;
    if (!collectLineSpliceLogicalLine(fileText, cursor, gapEnd, logicalLine,
                                  afterLine, &logicalLineSourceOffsets))
      return std::nullopt;

    std::optional<SourceLineDirectiveLogicalLineRewrite> rewrite;
    std::optional<ParsedSourceLineDirectiveLogicalLine> state =
        parseSourceLineDirectiveLogicalLine(logicalLine, currentFileSpelling);
    if (!state && logicalLineRewriter) {
      SourceLineDirectiveBuiltinMacroResolver builtinMacroResolver =
          [&](const RefoldModel::MacroInvocation &macro)
          -> std::optional<std::string> {
        if (macro.subkind != "obj" ||
            sourceLineDirectiveMacroHasMaterializedPPTokens(macro) ||
            !macro.invB || !macro.invE || *macro.invB >= *macro.invE)
          return std::nullopt;

        // Predefined location macros inside #line operands expand according to
        // the presumed location state at the invocation site.  When this gap
        // proof has already accepted an earlier line-control directive in the
        // same consumed interval, the running state below is exact.  Otherwise
        // use the physical source location only if the prefix contains no
        // earlier line-control directive that could have changed the presumed
        // file/line state outside the gap being proved.
        auto currentPresumedFileSpelling = [&]() -> std::optional<std::string> {
          if (sawLineDirective)
            return currentFileSpelling;
          if (prefixHasLineControl)
            return std::nullopt;
          if (macro.invFile && !macro.invFile->empty())
            return macro.invFile->str();
          return defaultFileSpelling.str();
        };

        if (macro.name == "__LINE__") {
          if (sawLineDirective) {
            const size_t delta = stringutils::countNonSplicedNewlines(
                fileText, static_cast<size_t>(lastAfterDirectiveOffset),
                static_cast<size_t>(*macro.invB));
            return std::to_string(lastLineAfterDirective + delta);
          }

          if (prefixHasLineControl)
            return std::nullopt;

          return std::to_string(
              stringutils::lineAtOffset(fileText, *macro.invB));
        }

        auto formatLineControlFileOperand =
            [](StringRef fileSpelling) -> std::optional<std::string> {
          // The rewritten directive is parsed by the same narrow #line parser
          // used for source-spelled gaps.  Keep that parser's inverse exact:
          // quote the file operand and escape only bytes meaningful inside the
          // directive string.  Physical newlines cannot appear inside the
          // single logical directive line being proved.
          if (fileSpelling.contains('\n') || fileSpelling.contains('\r'))
            return std::nullopt;

          std::string replacement = "\"";
          replacement +=
              LineDirectiveInserter::EscapeForLineDirective(fileSpelling);
          replacement += "\"";
          return replacement;
        };

        if (macro.name == "__FILE__") {
          std::optional<std::string> fileSpelling =
              currentPresumedFileSpelling();
          if (!fileSpelling)
            return std::nullopt;
          return formatLineControlFileOperand(*fileSpelling);
        }

        if (macro.name == "__FILE_NAME__") {
          std::optional<std::string> fileSpelling =
              currentPresumedFileSpelling();
          if (!fileSpelling)
            return std::nullopt;
          return formatLineControlFileOperand(
              sys::path::filename(StringRef(*fileSpelling)));
        }

        if (macro.name == "__BASE_FILE__") {
          // __BASE_FILE__ is not affected by prior #line state.  It names the
          // top-level source file for the preprocessing run, so header closure
          // callers pass that spelling explicitly instead of deriving it from
          // the current presumed file.
          StringRef baseSpelling =
              baseFileSpelling.empty() ? defaultFileSpelling : baseFileSpelling;
          if (baseSpelling.empty())
            return std::nullopt;
          return formatLineControlFileOperand(baseSpelling);
        }

        if (macro.name == "__DATE__" || macro.name == "__TIME__" ||
            macro.name == "__TIMESTAMP__") {
          // These predefined macros expand to volatile string literals.  The
          // current refold map records the source invocation but not the exact
          // expansion text that was used when A was produced.  Therefore they
          // are admitted only when the caller has proved that the untouched
          // suffix cannot observe the filename component of this line-control
          // state.  In that case any stable filename spelling is token-
          // equivalent; the resumed line number is the only observable state.
          if (!allowUnknownFilenameOperand)
            return std::nullopt;
          return formatLineControlFileOperand(defaultFileSpelling);
        }

        return std::nullopt;
      };

      rewrite = logicalLineRewriter(logicalLine, logicalLineSourceOffsets,
                                    builtinMacroResolver);
      if (rewrite)
        state = parseSourceLineDirectiveLogicalLine(rewrite->line,
                                                    currentFileSpelling);
    }
    if (!state)
      return std::nullopt;
    if (rewrite && acceptedMacroInvocationIds)
      acceptedMacroInvocationIds->append(rewrite->macroInvocationIds.begin(),
                                         rewrite->macroInvocationIds.end());

    sawLineDirective = true;
    currentFileSpelling = state->fileSpelling;
    if (state->updatesLineMarkerFlags)
      currentLineMarkerFlags = state->lineMarkerFlags;

    // A source-spelled line-control directive normally resumes the next
    // physical line at the parsed line number.  If the directive logical line
    // spans multiple physical source lines after the line operand field begins,
    // those physical newlines still advance the presumed line counter before
    // the following source line.  That applies both to newlines inside complete
    // block comments, which comment replacement turns into whitespace, and to
    // backslash-newline splices inside directive operands.
    //
    // Do not simply count every newline from the initial `#`: splices that form
    // the directive introducer itself, such as `#\nline 123`, are consumed
    // before the line operand is recognized and do not change the resumed line.
    // Count from the raw source offset that begins the operand field instead.
    std::optional<uint64_t> directiveCountBegin =
        sourceLineDirectiveResumeCountBegin(logicalLine,
                                            logicalLineSourceOffsets);
    if (!directiveCountBegin)
      return std::nullopt;
    const size_t directiveBodyNewlines =
        countLineControlDirectiveBodyPhysicalNewlines(
            fileText, *directiveCountBegin, afterLine);
    lastLineAfterDirective = state->lineAfterDirective + directiveBodyNewlines;
    lastAfterDirectiveOffset = afterLine;
    cursor = afterLine;
    atLineStart = true;
  }

  if (!sawLineDirective)
    return std::nullopt;

  const size_t delta = stringutils::countNonSplicedNewlines(
      fileText, static_cast<size_t>(lastAfterDirectiveOffset),
      static_cast<size_t>(resumeOffset));
  return SourceLineDirectiveGapResume{lastLineAfterDirective + delta,
                                      std::move(currentFileSpelling),
                                      std::move(currentLineMarkerFlags)};
}

} // namespace refold
} // namespace clang
