//===--- RefoldEngine.ExpansionFallback.inc ------------------*- C++ -*-===//
//
// `RefoldEngine` member definitions physically split out of
// `RefoldEngine.cpp` to reduce the size of the main implementation file.
//
// This fragment contains the post-structural, pre-terminal fallback stage for
// proof-backed partial expansion. These members do not participate in normal
// structural candidate construction; they are only consulted after the main
// structural refolding pass has already requested terminal fallback and before
// the engine emits raw edited preprocessed text `B`.
//
// This is a physical extraction only:
//   - the methods remain `RefoldEngine` members
//   - the file is included directly into `RefoldEngine.cpp`
//   - private access, translation-unit scope, and behavior are unchanged
//
// The purpose of this fragment is organizational only: keep the explicit
// partial-expansion fallback seam and its witness-formatting helpers out of the
// main engine file while preserving the exact implementation structure and
// semantics.
//
//===----------------------------------------------------------------------===//

/// Return true iff `text` is only whitespace and complete C/C++ comments.
///
/// This is intentionally a lexical-trivia predicate, not a preprocessing
/// predicate. Include-closure may carry these bytes through a replacement
/// because comments and whitespace do not contribute preprocessing tokens.
/// Anything that would lex as a real token, including malformed or
/// unterminated comments, remains outside this proof class.
static bool isWsOrCompleteCommentTrivia(StringRef text) {
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
static bool parseLiteralEmptyConditionalDirectiveLine(StringRef line,
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
static bool isPreservableIncludeClosureGapTrivia(StringRef text) {
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

/// Return true when preserved trivia begins with a preprocessing directive after
/// optional horizontal whitespace. Such trivia must be placed at the start of a
/// physical line when appended after materialized B tokens.
static bool startsWithPreprocessorDirectiveTrivia(StringRef text) {
  StringRef rest = text.ltrim(" \t\v\f");
  return rest.starts_with("#");
}


struct SourceLineDirectiveGapResume {
  size_t lineAtResume = 0;
  std::string fileSpelling;

  /// Optional numeric line-marker flags to replay with the resume directive.
  /// Empty means the canonical `#line` spelling is sufficient.
  std::string lineMarkerFlags;
};

struct ParsedSourceLineDirectiveLogicalLine {
  size_t lineAfterDirective = 0;
  std::string fileSpelling;

  /// True iff this directive resets the active numeric line-marker flags.
  /// Canonical `#line` directives and filename-less numeric markers preserve
  /// the prior flag state; numeric markers with an explicit filename establish
  /// a fresh flag state, even when that state is empty.
  bool updatesLineMarkerFlags = false;

  /// Canonicalized replayable flags from a numeric line marker.  This accepts
  /// only flags that can be locally re-emitted without needing to prove a
  /// preprocessor include-stack pop.
  std::string lineMarkerFlags;
};

struct SourceLineDirectiveLogicalLineRewrite {
  std::string line;
  SmallVector<uint64_t, 4> macroInvocationIds;
};

struct SourceLineDirectiveMacroReplacement {
  std::string text;
  SmallVector<uint64_t, 4> macroInvocationIds;
};

using SourceLineDirectiveBuiltinMacroResolver =
    std::function<std::optional<std::string>(
        const RefoldModel::MacroInvocation &)>;

using SourceLineDirectiveLogicalLineRewriter =
    std::function<std::optional<SourceLineDirectiveLogicalLineRewrite>(
        StringRef, ArrayRef<uint64_t>,
        SourceLineDirectiveBuiltinMacroResolver)>;

/// Return `text` after applying the translation-phase-2 splice deletion rule.
///
/// The owner-envelope line-control proof operates on a phase-2 logical directive
/// line, while the producer records macro invocation and argument byte ranges in
/// the original source spelling.  When a recorded invocation crosses a literal
/// backslash-newline pair, compare and substitute the post-splice spelling rather
/// than requiring physically contiguous source bytes.
static std::optional<std::string>
removePhase2SplicesForLineControl(StringRef text) {
  std::string out;
  out.reserve(text.size());

  enum class ScanState { Ordinary, StringLiteral, CharLiteral, BlockComment };
  ScanState state = ScanState::Ordinary;

  auto skipSplice = [&](size_t &pos) -> bool {
    return stringutils::skipPhase2LineSplice(text, text.size(), pos);
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
      // Translation phase 2 removes backslash-newline pairs before phase 3
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

/// Build the translation-phase-2 logical spelling of one source line.
///
/// Source-level `#line` gaps are parsed from raw file bytes because they do not
/// have ordinary PP tokens in the refold map.  Directive recognition occurs
/// after C line splicing, so a `#` line whose trailing backslash splices the
/// following `line 123` text is the same logical directive as `#line 123`.
/// Block comments inside a directive are recognized before choosing the
/// terminating newline, because phase 3 replaces a complete comment with one
/// whitespace character; physical newlines inside such comments are therefore
/// part of the directive spelling, not the directive terminator.  This helper
/// removes only literal backslash-newline and backslash-CRLF pairs that are
/// fully contained in the proved source gap, then returns the byte just after
/// the terminating non-spliced newline.
static bool collectPhase2LogicalLine(
    StringRef fileText, uint64_t lineBegin, uint64_t limit,
    std::string &logicalLine, uint64_t &afterLine,
    SmallVectorImpl<uint64_t> *logicalLineSourceOffsets = nullptr) {
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

  auto skipPhase2Splice = [&](uint64_t &pos) -> bool {
    return stringutils::skipPhase2LineSplice(fileText, limit, pos);
  };

  for (uint64_t pos = lineBegin; pos < limit;) {
    char ch = fileText[pos];

    // C translation phase 2 deletes backslash-newline pairs before
    // preprocessing directives are recognized.  Require the whole pair to be
    // inside the already-proved source gap so the closure does not depend on
    // bytes outside its envelope.
    if (skipPhase2Splice(pos))
      continue;

    // A block comment that starts on a directive line is translated to one
    // whitespace character before macro expansion.  Newlines inside the
    // complete comment therefore do not terminate the directive.  Keep the raw
    // comment spelling and source-offset map so later stringification proofs
    // can apply their own phase-3 whitespace normalization to the recorded
    // macro argument.
    if (ch == '/' && pos + 1 < limit && fileText[pos + 1] == '*') {
      appendSourceByte(pos++);
      appendSourceByte(pos++);
      bool closed = false;
      while (pos < limit) {
        if (skipPhase2Splice(pos))
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

/// Return the raw source offset from which physical newlines inside a proved
/// line-control directive affect the resumed line state.
///
/// `collectPhase2LogicalLine()` removes backslash-newline splices before the
/// directive is parsed, but not every removed physical newline advances the
/// presumed line established by the directive.  Splices that merely form the
/// directive introducer itself, such as `#\nline 123` or `#li\nne 123`, are
/// consumed before the line operand is recognized and do not add to the resumed
/// value.  Physical newlines after the directive has reached its operand field,
/// including line continuations inside the filename macro argument and newlines
/// inside block comments, do advance the line observed by the copied suffix.
///
/// The returned offset is just after the whitespace that separates the directive
/// keyword/marker from its line operand.  Counting physical LF bytes from that
/// raw offset through the collected directive line, minus the terminating
/// directive newline, gives exactly the internal physical-line advance that must
/// be added to the parsed line number.
static std::optional<uint64_t> sourceLineDirectiveResumeCountBegin(
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

/// Count physical source lines inside a complete line-control directive that
/// are part of the directive body rather than the terminating directive newline.
static size_t countLineControlDirectiveBodyPhysicalNewlines(
    StringRef fileText, uint64_t countBegin, uint64_t afterLine) {
  if (countBegin >= afterLine || afterLine > fileText.size())
    return 0;

  size_t count = stringutils::countNewlines(fileText, countBegin, afterLine);
  if (count != 0 && afterLine != 0 && fileText[afterLine - 1] == '\n')
    --count;
  return count;
}

/// Return true if the source prefix may already have changed the presumed
/// line-control state before a zero-token `__LINE__` operand is encountered.
///
/// Predefined `__LINE__` expands to the current presumed line number, not
/// blindly to the physical file line.  The refold map records the builtin
/// invocation site, but not a first-class line-control state snapshot for that
/// site.  Therefore this local proof uses the physical line number only when
/// the same source file prefix contains no earlier line-control directive.
/// Inside a source gap, later directives are handled by
/// computeSourceLineDirectiveGapResume()'s running state before the builtin is
/// substituted.
static bool sourcePrefixMayContainLineControlDirective(StringRef fileText,
                                                       uint64_t limit) {
  limit = std::min<uint64_t>(limit, fileText.size());

  for (uint64_t lineBegin = 0; lineBegin < limit;) {
    std::string logicalLine;
    uint64_t afterLine = lineBegin;
    if (!collectPhase2LogicalLine(fileText, lineBegin, limit, logicalLine,
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

/// Return true iff `c` is one of the hexadecimal digits accepted by C escape
/// sequences.  Keep this helper local to the line-control proof so the escape
/// decoder below is byte-oriented and independent from any host locale state.
static bool isLineControlHexDigit(char c) {
  return ('0' <= c && c <= '9') || ('a' <= c && c <= 'f') ||
         ('A' <= c && c <= 'F');
}

/// Convert a hexadecimal digit already accepted by isLineControlHexDigit().
static unsigned lineControlHexValue(char c) {
  if ('0' <= c && c <= '9')
    return static_cast<unsigned>(c - '0');
  if ('a' <= c && c <= 'f')
    return static_cast<unsigned>(c - 'a' + 10);
  return static_cast<unsigned>(c - 'A' + 10);
}

/// Decode a greedy hexadecimal escape value and advance `rest` past every hex
/// digit consumed by the C string-literal grammar.  Values outside one byte are
/// rejected before arithmetic can overflow.
static std::optional<unsigned>
decodeBoundedLineControlHexEscapeValue(StringRef &rest) {
  if (rest.empty() || !isLineControlHexDigit(rest.front()))
    return std::nullopt;

  unsigned value = 0;
  do {
    const unsigned digit = lineControlHexValue(rest.front());
    if (value > (0xffu - digit) / 16u)
      return std::nullopt;
    value = (value * 16u) + digit;
    rest = rest.drop_front();
  } while (!rest.empty() && isLineControlHexDigit(rest.front()));

  return value;
}

/// Decode exactly `digits` hexadecimal digits after a universal-character-name
/// introducer and advance `rest` past the digits on success.
static std::optional<uint32_t>
decodeLineControlUniversalCharacterNameValue(StringRef &rest,
                                             unsigned digits) {
  if (rest.size() < digits)
    return std::nullopt;

  uint32_t value = 0;
  for (unsigned i = 0; i < digits; ++i) {
    const char ch = rest[i];
    if (!isLineControlHexDigit(ch))
      return std::nullopt;
    value = (value << 4) | lineControlHexValue(ch);
  }

  rest = rest.drop_front(digits);
  return value;
}

/// Return true iff a decoded universal-character-name is within the stable
/// Unicode scalar range this filename proof can replay as UTF-8 bytes.
///
/// C universal-character names for the basic source character set and control
/// characters are intentionally left outside this proof.  Admitting only scalar
/// values at or above U+00A0 matches the portable UCN domain used for ordinary
/// source text and avoids collisions with the existing exact tables for quotes,
/// backslashes, physical newlines, and named control escapes.
static bool isReplayableLineControlUniversalCharacterName(uint32_t value) {
  return 0x00a0u <= value && value <= 0x10ffffu &&
         !(0xd800u <= value && value <= 0xdfffu);
}

/// Append the UTF-8 spelling Clang uses for an admitted universal-character-name
/// in a narrow #line filename string literal.
static bool appendLineControlUTF8(std::string &out, uint32_t value) {
  if (!isReplayableLineControlUniversalCharacterName(value))
    return false;

  if (value <= 0x7fu) {
    out.push_back(static_cast<char>(value));
    return true;
  }
  if (value <= 0x7ffu) {
    out.push_back(static_cast<char>(0xc0u | (value >> 6)));
    out.push_back(static_cast<char>(0x80u | (value & 0x3fu)));
    return true;
  }
  if (value <= 0xffffu) {
    out.push_back(static_cast<char>(0xe0u | (value >> 12)));
    out.push_back(static_cast<char>(0x80u | ((value >> 6) & 0x3fu)));
    out.push_back(static_cast<char>(0x80u | (value & 0x3fu)));
    return true;
  }

  out.push_back(static_cast<char>(0xf0u | (value >> 18)));
  out.push_back(static_cast<char>(0x80u | ((value >> 12) & 0x3fu)));
  out.push_back(static_cast<char>(0x80u | ((value >> 6) & 0x3fu)));
  out.push_back(static_cast<char>(0x80u | (value & 0x3fu)));
  return true;
}

/// Return true iff a decoded numeric filename byte can be replayed by this
/// proof's #line formatter without introducing an unmodeled filename state.
///
/// Numeric C escapes in a macro-stringified #line filename denote filename
/// bytes once the generated string literal is interpreted.  Single-byte ASCII
/// values are replayable when they are either ordinary printable bytes or one
/// of the bounded control bytes for which EscapeForLineDirective() has an
/// explicit one-line escape spelling.  High bytes are only provisionally
/// admitted here: parseLineControlFilenameLiteral() validates that the final
/// filename byte string is well-formed UTF-8 before accepting the directive.
/// That lets byte-oriented spellings such as `\303\251` round-trip as the
/// same filename state as the UCN spelling `\u00E9`, while malformed high-byte
/// sequences still fail closed.
static bool isReplayableLineControlNumericFilenameByte(unsigned value) {
  if (0x20 <= value && value <= 0x7e)
    return true;

  if (0x80 <= value && value <= 0xff)
    return true;

  switch (value) {
  case '\a':
  case '\b':
  case '\f':
  case '\n':
  case '\r':
  case '\t':
  case '\v':
  case 0x1b:
    return true;
  default:
    return false;
  }
}

/// Return true iff a decoded filename byte is non-ASCII and therefore requires
/// the final filename spelling to be validated as UTF-8 before replay.
static bool lineControlFilenameByteRequiresUTF8Validation(unsigned value) {
  return 0x80 <= value && value <= 0xff;
}

/// Validate the UTF-8 subset that this proof may replay directly in a #line
/// filename.
///
/// This is deliberately just a structural UTF-8 validator.  It rejects
/// overlong encodings, surrogate code points, out-of-range scalars, and
/// truncated continuation sequences.  The line-control proof calls it only when
/// a numeric escape contributed at least one high byte, preserving the previous
/// behavior for already source-spelled filename bytes while keeping escaped
/// high-byte sequences from smuggling malformed text into an emitted directive.
static bool isValidLineControlUTF8(StringRef text) {
  for (size_t i = 0; i < text.size();) {
    const unsigned char lead = static_cast<unsigned char>(text[i]);
    if (lead < 0x80) {
      ++i;
      continue;
    }

    auto continuation = [&](size_t index) -> std::optional<unsigned char> {
      if (index >= text.size())
        return std::nullopt;
      const unsigned char byte = static_cast<unsigned char>(text[index]);
      if ((byte & 0xc0u) != 0x80u)
        return std::nullopt;
      return byte;
    };

    if (0xc2 <= lead && lead <= 0xdf) {
      if (!continuation(i + 1))
        return false;
      i += 2;
      continue;
    }

    if (lead == 0xe0) {
      std::optional<unsigned char> b1 = continuation(i + 1);
      if (!b1 || *b1 < 0xa0 || !continuation(i + 2))
        return false;
      i += 3;
      continue;
    }

    if ((0xe1 <= lead && lead <= 0xec) || (0xee <= lead && lead <= 0xef)) {
      if (!continuation(i + 1) || !continuation(i + 2))
        return false;
      i += 3;
      continue;
    }

    if (lead == 0xed) {
      std::optional<unsigned char> b1 = continuation(i + 1);
      if (!b1 || *b1 >= 0xa0 || !continuation(i + 2))
        return false;
      i += 3;
      continue;
    }

    if (lead == 0xf0) {
      std::optional<unsigned char> b1 = continuation(i + 1);
      if (!b1 || *b1 < 0x90 || !continuation(i + 2) ||
          !continuation(i + 3))
        return false;
      i += 4;
      continue;
    }

    if (0xf1 <= lead && lead <= 0xf3) {
      if (!continuation(i + 1) || !continuation(i + 2) ||
          !continuation(i + 3))
        return false;
      i += 4;
      continue;
    }

    if (lead == 0xf4) {
      std::optional<unsigned char> b1 = continuation(i + 1);
      if (!b1 || *b1 > 0x8f || !continuation(i + 2) ||
          !continuation(i + 3))
        return false;
      i += 4;
      continue;
    }

    return false;
  }

  return true;
}

struct DecodedLineControlFilenameEscape {
  std::string bytes;
  bool requiresUTF8Validation = false;
};

/// Decode the bounded subset of C string-literal escapes that this source-only
/// line-control proof can replay exactly for a filename operand.
///
/// The parser accepts:
/// * ordinary unknown escapes, whose observable spelling is the escaped byte;
/// * escaped quote, question mark, and backslash;
/// * bounded octal escapes that decode to replayable filename bytes;
/// * bounded hexadecimal escapes that decode to replayable filename bytes; and
/// * universal-character names that decode to replayable Unicode scalars.
///
/// Named control escapes (`\n`, `\t`, etc.) are decoded through an explicit
/// table and later replayed by LineDirectiveInserter::EscapeForLineDirective().
/// Universal-character names are emitted as UTF-8 bytes, which the #line
/// formatter already preserves byte-for-byte.  Empty hex escapes, out-of-range
/// numeric escapes, invalid UCN scalars, and physical line breaks fail closed.
static std::optional<DecodedLineControlFilenameEscape>
decodeLineControlFilenameEscape(StringRef &rest) {
  if (rest.empty())
    return std::nullopt;

  const char escaped = rest.front();
  if (escaped == '\n' || escaped == '\r')
    return std::nullopt;

  if ('0' <= escaped && escaped <= '7') {
    unsigned value = 0;
    unsigned digits = 0;
    while (!rest.empty() && digits < 3 && '0' <= rest.front() &&
           rest.front() <= '7') {
      value = (value * 8) + static_cast<unsigned>(rest.front() - '0');
      rest = rest.drop_front();
      ++digits;
    }
    if (!isReplayableLineControlNumericFilenameByte(value))
      return std::nullopt;
    return DecodedLineControlFilenameEscape{
        std::string(1, static_cast<char>(value)),
        lineControlFilenameByteRequiresUTF8Validation(value)};
  }

  if (escaped == 'x') {
    rest = rest.drop_front();
    std::optional<unsigned> value =
        decodeBoundedLineControlHexEscapeValue(rest);
    if (!value || !isReplayableLineControlNumericFilenameByte(*value))
      return std::nullopt;
    return DecodedLineControlFilenameEscape{
        std::string(1, static_cast<char>(*value)),
        lineControlFilenameByteRequiresUTF8Validation(*value)};
  }

  if (escaped == 'u' || escaped == 'U') {
    rest = rest.drop_front();
    std::optional<uint32_t> value =
        decodeLineControlUniversalCharacterNameValue(
            rest, escaped == 'u' ? 4u : 8u);
    if (!value)
      return std::nullopt;

    std::string utf8;
    if (!appendLineControlUTF8(utf8, *value))
      return std::nullopt;
    return DecodedLineControlFilenameEscape{utf8, false};
  }

  switch (escaped) {
  case '"':
  case '?':
  case '\\':
    rest = rest.drop_front();
    return DecodedLineControlFilenameEscape{std::string(1, escaped), false};
  case 'a':
    rest = rest.drop_front();
    return DecodedLineControlFilenameEscape{std::string(1, '\a'), false};
  case 'b':
    rest = rest.drop_front();
    return DecodedLineControlFilenameEscape{std::string(1, '\b'), false};
  case 'e':
  case 'E':
    // Clang accepts \e/\E as an extension and records the same escape byte
    // that it later prints for __FILE__.  Model it explicitly instead of
    // treating it as a warningful unknown escape.
    rest = rest.drop_front();
    return DecodedLineControlFilenameEscape{
        std::string(1, static_cast<char>(0x1b)), false};
  case 'f':
    rest = rest.drop_front();
    return DecodedLineControlFilenameEscape{std::string(1, '\f'), false};
  case 'n':
    rest = rest.drop_front();
    return DecodedLineControlFilenameEscape{std::string(1, '\n'), false};
  case 'r':
    rest = rest.drop_front();
    return DecodedLineControlFilenameEscape{std::string(1, '\r'), false};
  case 't':
    rest = rest.drop_front();
    return DecodedLineControlFilenameEscape{std::string(1, '\t'), false};
  case 'v':
    rest = rest.drop_front();
    return DecodedLineControlFilenameEscape{std::string(1, '\v'), false};
  default:
    // Clang's warningful ordinary unknown escapes in #line filenames delete the
    // backslash and preserve the escaped byte.  This matches the prior bounded
    // raw-backslash proof and is still reparsed by the strict directive parser.
    rest = rest.drop_front();
    return DecodedLineControlFilenameEscape{std::string(1, escaped), false};
  }
}

/// Parse and decode a quoted #line filename operand.
///
/// This is deliberately narrower than a general C string-literal parser.  It is
/// the inverse of the filename spellings this proof may synthesize and replay:
/// exact ordinary bytes, conservative quote/backslash escapes, warningful
/// ordinary unknown escapes, and numeric escapes that decode to replayable
/// filename bytes.  Unsupported escape forms fail closed so the
/// owner-envelope closure can fall back instead of guessing the filename state
/// observed by later `__FILE__` uses.
static std::optional<std::string>
parseLineControlFilenameLiteral(StringRef &rest) {
  if (!rest.consume_front("\""))
    return std::nullopt;

  std::string fileSpelling;
  bool closedQuote = false;
  bool requiresUTF8Validation = false;
  while (!rest.empty()) {
    char ch = rest.front();
    rest = rest.drop_front();
    if (ch == '\"') {
      closedQuote = true;
      break;
    }
    if (ch == '\n' || ch == '\r')
      return std::nullopt;
    if (ch == '\\') {
      std::optional<DecodedLineControlFilenameEscape> decoded =
          decodeLineControlFilenameEscape(rest);
      if (!decoded)
        return std::nullopt;
      requiresUTF8Validation |= decoded->requiresUTF8Validation;
      fileSpelling += decoded->bytes;
      continue;
    }
    fileSpelling.push_back(ch);
  }

  if (!closedQuote)
    return std::nullopt;
  if (requiresUTF8Validation && !isValidLineControlUTF8(fileSpelling))
    return std::nullopt;
  return fileSpelling;
}

/// Parse one source-spelled line-control directive logical line.
///
/// The refold map does not currently record line-control directives as
/// first-class artifacts because they contribute no ordinary PP tokens.  Mixed
/// source-envelope proofs therefore recover this narrow directive shape
/// directly from the source bytes when a source-only line-control line lies
/// between otherwise required owner pieces.
///
/// The input is a translation-phase-2 logical line: any C backslash-newline
/// splices have already been removed by the caller.  This parser is still
/// intentionally stricter than the general preprocessor grammar: it accepts
/// only source-spelled `#line <digits> ["file"]` and the Clang/GCC numeric
/// line-marker spelling `# <digits> ["file"]`, both with optional horizontal
/// whitespace.  Numeric line-marker flags are accepted only for the replayable
/// flag forms that can be re-emitted as the local resume directive;
/// macro-dependent spellings and include-stack-pop flag forms remain outside
/// this proof class.
static std::optional<ParsedSourceLineDirectiveLogicalLine>
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


/// Return true iff a macro invocation produced ordinary PP material that would
/// need the normal macro-realization machinery rather than line-control repair.
static bool sourceLineDirectiveMacroHasMaterializedPPTokens(
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

/// Return true iff the untouched suffix may observe the current presumed file.
///
/// Volatile string-valued predefined macros such as `__DATE__` can be valid
/// filename operands in a source-only line-control directive, but the refold map
/// does not record their concrete expansion value.  Such a gap is still
/// token-preservable when the remaining source can only observe the resumed line
/// number.  This conservative predicate rejects any later `__FILE__` or
/// `__FILE_NAME__` use recorded by the producer, and also rejects raw source
/// spellings of those builtins in the suffix as a fail-closed backstop for
/// cases not represented by an invocation with a usable source range.
static bool sourceSuffixMayObservePresumedFileSpelling(
    const RefoldModel &model, StringRef file, uint64_t resumeOffset,
    const std::function<bool(StringRef, StringRef)> &pathsEqual,
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

  for (const RefoldModel::MacroInvocation &macro : model.GetMacroInvocations()) {
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

/// Return the end offset of an admitted raw backslash escape in an argument
/// that is about to be macro-stringified for a line-control filename operand.
///
/// C macro stringification only doubles backslashes that occur inside string or
/// character literals.  A backslash that appears as its own preprocessing-token
/// spelling outside a literal is copied into the generated filename string
/// literal verbatim.  This helper admits exactly the raw escape spellings that
/// parseLineControlFilenameLiteral() can decode and
/// formatSourceLineDirectiveGapResume() can replay without guessing:
/// * warningful ordinary unknown escapes;
/// * escaped quote, question mark, backslash, and horizontal whitespace; and
/// * bounded octal/hexadecimal escapes that decode to replayable filename
///   bytes;
/// * named C escapes whose decoded bytes can be replayed exactly by the #line
///   formatter; and
/// * universal-character names whose decoded scalar can be replayed as UTF-8.
///
/// Invalid universal-character names remain fail-closed.  The admitted subset
/// is deliberately limited to valid Unicode scalars outside the basic/control
/// character range so replay can preserve the filename bytes without guessing.
static std::optional<size_t>
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

/// Produce the string literal spelling for a macro argument used by the `#`
/// operator inside a source-only line-control directive.
///
/// This is the small, proof-local subset of C macro stringification needed for
/// line-control operands.  It mirrors the observable pieces of the preprocessor
/// stringification rule that can be proved from the complete recorded argument
/// spelling: trim leading/trailing whitespace, collapse each run of inter-token
/// whitespace to one space, preserve string/character literal token spelling,
/// and finally quote the resulting character sequence as a C string token.
///
/// Complete block comments are modeled as the single whitespace character
/// produced before macro replacement, even when the comment body spans physical
/// source lines.  A raw backslash outside a literal is admitted only when the
/// resulting filename string-literal escape is decoded by the bounded
/// line-control filename parser above; all other escape forms fail closed and
/// must be handled by a wider proof or terminal fallback.  Named C escapes are
/// admitted because the parser decodes them to exact bytes and the #line
/// formatter knows how to spell those bytes again.  Universal-character names
/// are admitted only when their decoded scalar can be replayed as UTF-8 bytes.
static std::optional<std::string>
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
    // stringification.  Because collectPhase2LogicalLine has already proved
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
      // generated filename string literal rather than doubling it.  Preserve the
      // whole admitted raw escape spelling and let the strict filename-literal
      // parser decode the same bounded subset that was proved above.
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

/// Return true iff a recorded macro argument contains at least one preprocessing
/// token after the local phase-3 comment-to-whitespace rule.
///
/// `__VA_OPT__` is controlled by whether the variadic tail contains any
/// preprocessing tokens, not by whether the raw argument spelling is non-empty.
/// The line-control proof only has source slices, so it accepts the bounded
/// subset it already knows how to reason about: whitespace plus complete block
/// comments are token-empty, a line comment or malformed comment fails closed,
/// and any other byte proves that at least one token-like spelling is present.
static std::optional<bool>
lineControlArgumentContainsPPTokens(StringRef argument) {
  for (size_t pos = 0; pos < argument.size();) {
    if (stringutils::isWs(argument[pos])) {
      ++pos;
      continue;
    }

    if (pos + 1 < argument.size() && argument[pos] == '/') {
      if (argument[pos + 1] == '*') {
        pos += 2;
        bool closed = false;
        while (pos + 1 < argument.size()) {
          if (argument[pos] == '*' && argument[pos + 1] == '/') {
            pos += 2;
            closed = true;
            break;
          }
          ++pos;
        }
        if (!closed)
          return std::nullopt;
        continue;
      }
      if (argument[pos + 1] == '/')
        return std::nullopt;
    }

    return true;
  }

  return false;
}

/// Return the raw payload inside one `__VA_OPT__(...)` occurrence.
///
/// This scans only enough replacement-list structure to find the matching
/// close-paren without being confused by parentheses inside string/character
/// literals or complete block comments.  The payload is not interpreted here;
/// after `__VA_OPT__` is erased or exposed, the normal bounded line-control
/// replacement-list evaluator still performs argument substitution,
/// stringification, paste-witness replay, and final strict #line parsing.
static std::optional<std::pair<StringRef, size_t>>
parseLineControlVaOptPayload(StringRef replacement, size_t openParen) {
  if (openParen >= replacement.size() || replacement[openParen] != '(')
    return std::nullopt;

  const size_t payloadBegin = openParen + 1;
  unsigned depth = 1;
  for (size_t pos = payloadBegin; pos < replacement.size();) {
    char ch = replacement[pos];

    if (ch == '\n' || ch == '\r')
      return std::nullopt;

    if (ch == '"' || ch == '\'') {
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
      continue;
    }

    if (pos + 1 < replacement.size() && ch == '/') {
      if (replacement[pos + 1] == '*') {
        pos += 2;
        bool closed = false;
        while (pos + 1 < replacement.size()) {
          if (replacement[pos] == '*' && replacement[pos + 1] == '/') {
            pos += 2;
            closed = true;
            break;
          }
          ++pos;
        }
        if (!closed)
          return std::nullopt;
        continue;
      }
      if (replacement[pos + 1] == '/')
        return std::nullopt;
    }

    if (ch == '(') {
      ++depth;
      ++pos;
      continue;
    }
    if (ch == ')') {
      --depth;
      if (depth == 0)
        return std::make_pair(replacement.slice(payloadBegin, pos), pos + 1);
      ++pos;
      continue;
    }

    ++pos;
  }

  return std::nullopt;
}

/// Expand top-level `__VA_OPT__(...)` operators in a recorded replacement list.
///
/// This is not a general macro expander.  It implements only the local C99/C23
/// variadic gate needed before the existing line-control evaluator runs: if the
/// producer-recorded variadic tail contains tokens, expose the `__VA_OPT__`
/// payload; otherwise erase it.  The exposed payload is then processed by the
/// normal proof path, so unsupported contents still fail closed at argument
/// substitution, paste-witness replay, or the final strict line-control parse.
static std::optional<std::string> expandLineControlVaOptOperators(
    StringRef replacement, ArrayRef<RefoldModel::MacroDefParam> params,
    ArrayRef<StringRef> invocationArgTexts) {
  if (params.size() != invocationArgTexts.size())
    return std::nullopt;

  std::optional<bool> variadicTailHasTokens;
  for (size_t i = 0; i != params.size(); ++i) {
    if (!params[i].variadic)
      continue;
    if (variadicTailHasTokens)
      return std::nullopt;
    std::optional<bool> hasTokens =
        lineControlArgumentContainsPPTokens(invocationArgTexts[i]);
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
          parseLineControlVaOptPayload(replacement, openParen);
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

/// Substitute invocation arguments through a simple function-like replacement
/// list used inside a source-only line-control directive.
///
/// This is intentionally a token-spelling substitution, not a general macro
/// expander.  The caller supplies exact source spellings for complete invocation
/// arguments that are wholly inside the already-proved logical directive line.
/// Direct formal substitution is supported, and `# formal` is supported through
/// the conservative line-control stringification helper above.  The resulting
/// replacement text is still parsed by the strict line-control parser, so any
/// unsupported macro semantics remain fail-closed.
static std::optional<std::string> substituteLineControlMacroParameters(
    StringRef replacement, ArrayRef<RefoldModel::MacroDefParam> params,
    ArrayRef<StringRef> invocationArgTexts,
    ArrayRef<RefoldModel::PasteToken> pasteTokens) {
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
                                        invocationArgTexts);
    if (!expandedVaOpt)
      return std::nullopt;
    return substituteLineControlMacroParameters(*expandedVaOpt, params,
                                                invocationArgTexts,
                                                pasteTokens);
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

/// Recover a simple macro replacement spelling that can participate in a
/// source-only line-control directive.
///
/// This is intentionally not a general macro expander.  The refold map records
/// the macro invocation, the defining directive, and exact invocation argument
/// byte ranges, but it does not record a dedicated expanded token stream for
/// #line directive operands.  Therefore the repair only substitutes complete
/// callsites whose replacement list is spelled directly in the recorded #define
/// text.  For function-like macros with parameters, direct formal-token
/// substitution, conservative `# formal` stringification, and producer-witnessed
/// `##` paste runs are allowed.  Variadic formals use the producer-recorded
/// tail argument as an ordinary substitution operand; comments, multiline
/// replacement lists, and any resulting non-line-control spelling fail closed
/// when the strict parser is re-run on the rewritten directive line.
static std::optional<std::string> simpleLineControlMacroReplacementText(
    const RefoldModel &model, const RefoldModel::MacroInvocation &macro,
    ArrayRef<StringRef> invocationArgTexts) {
  if (macro.subkind != "obj" && macro.subkind != "func")
    return std::nullopt;
  if (!macro.definitionDirectiveId ||
      sourceLineDirectiveMacroHasMaterializedPPTokens(macro))
    return std::nullopt;

  const RefoldModel::MacroDirective *definition = nullptr;
  for (const RefoldModel::MacroDirective &directive : model.GetMacroDirectives())
    if (directive.id == *macro.definitionDirectiveId) {
      definition = &directive;
      break;
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
      !macro.pasteTokens.empty())
    return substituteLineControlMacroParameters(replacement, macro.defParams,
                                                invocationArgTexts,
                                                macro.pasteTokens);

  if (!invocationArgTexts.empty())
    return std::nullopt;
  return replacement.str();
}

/// Find whole-token occurrences of `name` inside recovered replacement text.
///
/// Recursive line-control macro repair never treats these textual occurrences
/// as independent expansion candidates.  The producer has already recorded the
/// child macro invocations that occurred while expanding the parent macro; this
/// helper only recovers the corresponding byte ranges in the parent's recovered
/// replacement-list spelling.
static SmallVector<std::pair<size_t, size_t>, 4>
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

/// Locate the replacement-list occurrence that corresponds to one recorded
/// child macro invocation.
///
/// The earlier proof required the child invocation spelling to occur exactly
/// once in the parent's recovered replacement text.  That was sound but too
/// narrow for definitions such as `#define FILE_NAME F F`: the refold map
/// records both child invocations, and their source byte order disambiguates the
/// two identical `F` spellings deterministically.  When repeated spellings are
/// present, require a one-to-one correspondence between recorded same-spelling
/// children and textual whole-token occurrences, then match them by source order
/// and occurrence order.  Any unrecorded extra occurrence, missing source range,
/// or ordering ambiguity still fails closed.
static std::optional<std::pair<size_t, size_t>>
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

/// Recover invocation argument spellings for a macro invocation that happened
/// while expanding another macro replacement list.
///
/// The producer records `normalized_inv_text` for macro calls after macro
/// argument prescan.  When the child call itself has parameters, those
/// normalized argument ranges are the only source-local spellings this narrow
/// line-control proof can use without running a full preprocessor.  If any
/// argument is missing or malformed, the caller fails closed.
static std::optional<SmallVector<StringRef, 4>>
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

/// Recursively expand the bounded macro DAG recorded for a line-control operand.
///
/// This is still not a general macro expander.  It starts from the same simple
/// replacement-list recovery used for directly spelled macro operands, then
/// follows only producer-recorded child macro invocations whose `caller_macro_id`
/// is the macro being expanded.  Each child must occur unambiguously as a whole
/// identifier in the current replacement text and must itself satisfy the same
/// simple line-control replacement rules, or be a recorded predefined builtin
/// accepted by the caller's line-control builtin resolver.  The builtin hook is
/// needed for forms such as `#line LINE_NO(__LINE__)`: after substituting
/// `LINE_NO(x)` with its argument, the producer-recorded child `__LINE__`
/// invocation is the only deterministic witness for the numeric operand.
/// Cycles, unsupported macro operators, ambiguous repeated child spellings, and
/// excessive expansion depth all fail closed before the strict line-control
/// parser sees the rewritten directive.
static std::optional<SourceLineDirectiveMacroReplacement>
expandLineControlMacroReplacementText(
    const RefoldModel &model, const RefoldModel::MacroInvocation &macro,
    ArrayRef<StringRef> invocationArgTexts,
    SmallVectorImpl<uint64_t> &activeMacroIds,
    SourceLineDirectiveBuiltinMacroResolver builtinMacroResolver = nullptr) {
  if (activeMacroIds.size() > 32)
    return std::nullopt;
  if (std::find(activeMacroIds.begin(), activeMacroIds.end(), macro.id) !=
      activeMacroIds.end())
    return std::nullopt;

  std::optional<std::string> replacement =
      simpleLineControlMacroReplacementText(model, macro, invocationArgTexts);
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
            model, child, *childArgTexts, activeMacroIds,
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

/// Substitute simple recorded macro invocations into one source-only
/// line-control logical line.
///
/// The substitution is used only as a precursor to the strict line-control
/// parser.  It accepts complete callsites whose recorded source spelling maps
/// into the phase-2 logical line after deleting literal backslash-newline
/// splices, and whose replacement text is recovered from a simple recorded
/// #define.  Partial callsites, nested/overlapping source ranges, unsupported
/// parameterized replacement semantics, and unrecorded expansions all fail
/// closed.  Variadic formals are allowed only as ordinary recorded argument
/// substitutions: the producer has already collapsed the variadic tail into the
/// matching argument range, and the final strict line-control parse remains the
/// authority on whether the substituted spelling is a valid directive operand.
static std::optional<SourceLineDirectiveLogicalLineRewrite>
rewriteSourceLineDirectiveLogicalLineMacros(
    const RefoldModel &model, StringRef file, StringRef logicalLine,
    ArrayRef<uint64_t> logicalLineSourceOffsets,
    const std::function<bool(StringRef, StringRef)> &pathsEqual,
    std::optional<uint64_t> ownerIncludeId = std::nullopt,
    SourceLineDirectiveBuiltinMacroResolver builtinMacroResolver = nullptr) {
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

  auto findLogicalEndIndex = [&](uint64_t sourceOffset) -> std::optional<size_t> {
    auto it = std::lower_bound(logicalLineSourceOffsets.begin(),
                               logicalLineSourceOffsets.end(), sourceOffset);
    return static_cast<size_t>(it - logicalLineSourceOffsets.begin());
  };

  SmallVector<ReplacementPiece, 4> pieces;
  for (const RefoldModel::MacroInvocation &macro : model.GetMacroInvocations()) {
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
    std::optional<std::string> phase2Invocation =
        removePhase2SplicesForLineControl(*macro.invText);
    if (!phase2Invocation || logicalInvocation != *phase2Invocation)
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

        std::optional<size_t> argBeginIndex =
            findLogicalIndex(*argRange.first);
        std::optional<size_t> argEndIndex =
            findLogicalEndIndex(*argRange.second);
        if (!argBeginIndex || !argEndIndex || *argBeginIndex > *argEndIndex ||
            *argEndIndex > logicalLine.size())
          return std::nullopt;

        StringRef logicalArg =
            logicalLine.substr(*argBeginIndex, *argEndIndex - *argBeginIndex);

        // Macro argument ranges are recorded in the physical source spelling,
        // while this proof substitutes from the phase-2 logical directive line.
        // A backslash-newline inside the argument is therefore safe only when
        // the recorded invocation text, after phase-2 splice deletion, matches
        // the exact logical slice we are about to substitute.
        const uint64_t relativeArgBegin = *argRange.first - *macro.invB;
        const uint64_t relativeArgEnd = *argRange.second - *macro.invB;
        if (relativeArgBegin > relativeArgEnd ||
            relativeArgEnd > macro.invText->size())
          return std::nullopt;
        std::optional<std::string> phase2Arg =
            removePhase2SplicesForLineControl(macro.invText->slice(
                static_cast<size_t>(relativeArgBegin),
                static_cast<size_t>(relativeArgEnd)));
        if (!phase2Arg || logicalArg != *phase2Arg)
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
          model, macro, invocationArgTexts, activeMacroIds,
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

/// Format the local resume directive for a consumed source-only line-control
/// gap.  Canonical #line spelling is used unless the consumed state carries
/// numeric line-marker flags that must be replayed to preserve the suffix state.
static std::string
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

/// Prove that a source gap is made only of trivia plus complete source-spelled
/// `#line` directives, and compute the logical file/line state at `resumeOffset`.
///
/// The returned directive state is the state that must be re-established before
/// the untouched suffix beginning at `resumeOffset` is copied.  Counting continues
/// from the last accepted `#line` directive through the consumed source interval,
/// so a directive before a consumed include line is rewritten to the line that the
/// suffix would have observed after that include line had executed.
static std::optional<SourceLineDirectiveGapResume>
computeSourceLineDirectiveGapResume(
    StringRef fileText, uint64_t gapBegin, uint64_t gapEnd,
    uint64_t resumeOffset, StringRef defaultFileSpelling,
    SourceLineDirectiveLogicalLineRewriter logicalLineRewriter = nullptr,
    SmallVectorImpl<uint64_t> *acceptedMacroInvocationIds = nullptr,
    StringRef baseFileSpelling = StringRef(),
    bool allowUnknownFilenameOperand = false) {
  if (gapBegin >= gapEnd || gapEnd > fileText.size() ||
      resumeOffset < gapEnd || resumeOffset > fileText.size())
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
    if (!collectPhase2LogicalLine(fileText, cursor, gapEnd, logicalLine,
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
    // block comments, which phase 3 replaces with whitespace, and to phase-2
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
    lastLineAfterDirective =
        state->lineAfterDirective + directiveBodyNewlines;
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

std::string RefoldEngine::FormatExpansionClosureWitness(
    const ExpansionClosureWitness &witness) const {
  // Keep fallback-witness logging compact and mechanically comparable so the
  // acceptance path can be audited from traces without inspecting the full
  // in-memory witness object.
  std::string out = formatv("kind={0}", witness.kind).str();
  if (witness.hasRootMacroId)
    out += formatv(" rootMacro={0}", witness.rootMacroId).str();
  if (witness.hasOwnerIncludeId)
    out += formatv(" ownerInclude={0}", witness.ownerIncludeId).str();
  out += formatv(" source=[{0},{1})", witness.sourceByteBegin,
                 witness.sourceByteEnd)
             .str();
  out += formatv(" coverA=[{0},{1})", witness.coverATokBegin,
                 witness.coverATokEnd)
             .str();
  out += formatv(" B=[{0},{1})", witness.bTokBegin, witness.bTokEnd).str();
  out += formatv(" selfContained={0} claimsClipped={1}",
                 witness.selfContained ? 1 : 0,
                 witness.claimsClipped ? 1 : 0)
             .str();
  return out;
}

std::optional<RefoldEngine::ExpansionClosureWitness>
RefoldEngine::BuildMacroOwnerExpansionClosureWitness() const {
  if (!terminalFallbackRequested_) {
    trace("fallback", "macro-owner closure witness rejected: terminal fallback "
                      "not requested");
    return std::nullopt;
  }

  if (abTokHunks_.empty()) {
    trace("fallback",
          "macro-owner closure witness rejected: no token hunks available");
    return std::nullopt;
  }

  const StringRef tuPath = model_.GetSourcePath();
  if (tuPath.empty()) {
    trace("fallback",
          "macro-owner closure witness rejected: empty TU source path");
    return std::nullopt;
  }

  struct MacroOwnerClosureCandidate {
    const RefoldModel::MacroInvocation *root = nullptr;
    WholeCoverPlan plan;
  };

  // This witness deliberately proves only a whole-owner closure. A candidate root
  // is admissible only if every observed token hunk fits completely inside the
  // root's whole-cover envelope.
  //
  // For non-insert hunks, touching the boundary is allowed because the cover is
  // claimed inclusively over the replaced A-range. For insert-only hunks, we
  // require strict interior placement so the witness does not rely on a
  // boundary-adjacent ownership guess.
  auto hunkIsClosedByPlan = [&](const diffutils::Hunk &h,
                                const WholeCoverPlan &plan) -> bool {
    const bool aClosed =
        h.isInsertOnly() ? (plan.covLoA < h.aStart && h.aStart < plan.covHiA)
                         : (plan.covLoA <= h.aStart && h.aEnd <= plan.covHiA);
    if (!aClosed)
      return false;

    if (h.bStart < h.bEnd &&
        (h.bStart < plan.bTokStart || h.bEnd > plan.bTokEnd))
      return false;

    return true;
  };

  // Use explicit interval-level guards instead of relying on the
  // shape of the surviving candidate set alone. The fallback proof class is
  // only in-domain when its chosen owner frontier does not interact with any
  // foreign owner frontier.
  auto intervalsOverlap = [](uint64_t loA, uint64_t hiA, uint64_t loB,
                             uint64_t hiB) -> bool {
    return loA < hiB && loB < hiA;
  };

  auto isDescendantOf = [&](const RefoldModel::MacroInvocation &node,
                            const RefoldModel::MacroInvocation &root) -> bool {
    std::optional<uint64_t> cur = node.callerMacroId;
    DenseSet<uint64_t> visited;
    while (cur) {
      if (!visited.insert(*cur).second)
        break;
      if (*cur == root.id)
        return true;
      const RefoldModel::MacroInvocation *parent =
          FindMacroInvocationById(*cur);
      if (!parent)
        break;
      cur = parent->callerMacroId;
    }
    return false;
  };

  auto sourceIntervalIsSelfContainedTUOwned =
      [&](const RefoldModel::MacroInvocation &root) -> bool {
    if (!root.invB || !root.invE || *root.invE <= *root.invB)
      return false;

    ArrayRef<RefoldModel::Segment> segs = model_.GetSegmentsForFile(tuPath);
    if (segs.empty()) {
      trace("fallback",
            "macro-owner closure root rejected: id={0} name='{1}' has no TU "
            "segments for source interval",
            root.id, root.name);
      return false;
    }

    // The first synthesized fallback class splices bytes directly back into the
    // original TU. Make that source frontier explicit: the full callsite byte
    // interval must be covered by TU-owned segments only, with no include-owner
    // transitions and no conditional-arm changes inside the splice span.
    uint64_t cur = *root.invB;
    std::optional<uint64_t> condArmId;
    bool sawCondArmId = false;
    bool sawCoveringSeg = false;
    for (const auto &seg : segs) {
      if (seg.e <= cur)
        continue;
      if (seg.b >= *root.invE)
        break;
      if (!intervalsOverlap(cur, *root.invE, seg.b, seg.e))
        continue;
      if (seg.b > cur) {
        trace("fallback",
              "macro-owner closure root rejected: id={0} name='{1}' source "
              "interval [{2},{3}) has uncovered gap before segment [{4},{5})",
              root.id, root.name, *root.invB, *root.invE, seg.b, seg.e);
        return false;
      }
      if (seg.ownerIncludeId) {
        trace("fallback",
              "macro-owner closure root rejected: id={0} name='{1}' source "
              "interval [{2},{3}) crosses include-owned segment [{4},{5}) "
              "includeId={6}",
              root.id, root.name, *root.invB, *root.invE, seg.b, seg.e,
              *seg.ownerIncludeId);
        return false;
      }
      if (!sawCondArmId) {
        condArmId = seg.ownerCondArmId;
        sawCondArmId = true;
      } else if (condArmId != seg.ownerCondArmId) {
        trace("fallback",
              "macro-owner closure root rejected: id={0} name='{1}' source "
              "interval [{2},{3}) crosses conditional-arm boundary between {4} "
              "and {5}",
              root.id, root.name, *root.invB, *root.invE, condArmId.value_or(0),
              seg.ownerCondArmId.value_or(0));
        return false;
      }
      sawCoveringSeg = true;
      cur = std::max<uint64_t>(cur, seg.e);
      if (cur >= *root.invE)
        return true;
    }

    if (!sawCoveringSeg) {
      trace("fallback",
            "macro-owner closure root rejected: id={0} name='{1}' source "
            "interval [{2},{3}) has no covering TU segment",
            root.id, root.name, *root.invB, *root.invE);
      return false;
    }

    trace("fallback",
          "macro-owner closure root rejected: id={0} name='{1}' source "
          "interval [{2},{3}) is not fully covered by TU-owned segments",
          root.id, root.name, *root.invB, *root.invE);
    return false;
  };

  // Explicitly reject foreign macro interaction. For the first proof class,
  // the chosen owner closure must be the only macro-owner region participating
  // in the run; any intersecting non-descendant macro makes the frontier
  // ambiguous or mixed-owner and therefore out-of-domain.
  auto hasForeignOverlapOrMixedOwnership =
      [&](const RefoldModel::MacroInvocation &root,
          const WholeCoverPlan &plan) -> bool {
    for (const auto &other : model_.GetMacroInvocations()) {
      if (other.id == root.id)
        continue;

      const bool descendant = isDescendantOf(other, root);
      const bool sourceOverlap =
          other.invB && other.invE && *other.invE > *other.invB &&
          other.invFile && PathsEqual(*other.invFile, tuPath) &&
          intervalsOverlap(*root.invB, *root.invE, *other.invB, *other.invE);
      const bool coverOverlap =
          other.cover.IsValid() &&
          intervalsOverlap(plan.covLoA, plan.covHiA, other.cover.begin,
                           other.cover.end);

      if (!sourceOverlap && !coverOverlap)
        continue;

      if (descendant)
        continue;

      trace("fallback",
            "macro-owner closure root rejected: id={0} name='{1}' overlaps "
            "foreign macro id={2} name='{3}' sourceOverlap={4} "
            "coverOverlap={5} otherOwnerInclude={6}",
            root.id, root.name, other.id, other.name, sourceOverlap ? 1 : 0,
            coverOverlap ? 1 : 0, other.ownerIncludeId.value_or(0));
      return true;
    }
    return false;
  };

  SmallVector<MacroOwnerClosureCandidate, 4> candidates;
  for (const auto &m : model_.GetMacroInvocations()) {
    // The first fallback proof class is intentionally narrow: a single
    // top-level TU callsite macro whose whole-cover plan closes every edited
    // hunk by itself. Nested roots, include-owned callsites, define-body
    // invocations, and any clipped / non-self-contained plans remain
    // out-of-domain for this fallback proof.
    if (m.callerMacroId)
      continue;

    // The witness has to name an exact source callsite interval. If callsite
    // provenance is incomplete, we cannot soundly materialize the owner later.
    if (!m.invText || !m.invFile || !m.invB || !m.invE) {
      trace("fallback",
            "macro-owner closure root rejected: id={0} name='{1}' missing "
            "callsite provenance",
            m.id, m.name);
      continue;
    }

    if (!PathsEqual(*m.invFile, tuPath)) {
      trace("fallback",
            "macro-owner closure root rejected: id={0} name='{1}' callsite "
            "file='{2}' not TU='{3}'",
            m.id, m.name, StringRef(*m.invFile), tuPath);
      continue;
    }

    if (m.ownerIncludeId) {
      trace("fallback",
            "macro-owner closure root rejected: id={0} name='{1}' belongs to "
            "include #{2}",
            m.id, m.name, *m.ownerIncludeId);
      continue;
    }

    if (*m.invE <= *m.invB) {
      trace("fallback",
            "macro-owner closure root rejected: id={0} name='{1}' "
            "empty/invalid source range [{2},{3})",
            m.id, m.name, *m.invB, *m.invE);
      continue;
    }

    if (IsInvocationInsideDefineDirective(m)) {
      trace("fallback",
            "macro-owner closure root rejected: id={0} name='{1}' invocation "
            "inside define directive",
            m.id, m.name);
      continue;
    }

    if (!InvocationSpanMatchesCallsitePrefix(*m.invText, m)) {
      trace("fallback",
            "macro-owner closure root rejected: id={0} name='{1}' invocation "
            "text does not match callsite prefix",
            m.id, m.name);
      continue;
    }

    // Reuse the existing whole-cover computation as the closure/frontier
    // witness. Accept only roots whose existing plan already proves a
    // self-contained, unclipped edited B-surface.
    auto plan = ComputeWholeCoverPlan(m);
    if (!plan) {
      trace("fallback",
            "macro-owner closure root rejected: id={0} name='{1}' has no "
            "whole-cover plan",
            m.id, m.name);
      continue;
    }

    if (!plan->selfContained || plan->claimsClipped ||
        plan->bTokEnd <= plan->bTokStart) {
      trace("fallback",
            "macro-owner closure root rejected: id={0} name='{1}' "
            "selfContained={2} claimsClipped={3} B=[{4},{5})",
            m.id, m.name, plan->selfContained ? 1 : 0,
            plan->claimsClipped ? 1 : 0, plan->bTokStart, plan->bTokEnd);
      continue;
    }

    if (!sourceIntervalIsSelfContainedTUOwned(m))
      continue;

    // Make several previously implicit exclusions explicit. Reject any
    // candidate whose owner frontier overlaps a foreign macro closure or whose
    // callsite/source frontier crosses a mixed-ownership boundary. The single-
    // owner fallback class remains valid only when the proved closure is the
    // one and only owner region needed for the run.
    if (hasForeignOverlapOrMixedOwnership(m, *plan))
      continue;

    // The proof class is "single owner closes the entire edit set." Any hunk
    // left outside the candidate plan means this root is insufficient and must
    // be rejected instead of heuristically expanded upward.
    bool closesAllHunks = true;
    for (const auto &h : abTokHunks_) {
      if (hunkIsClosedByPlan(h, *plan))
        continue;

      trace(
          "fallback",
          "macro-owner closure root rejected: id={0} name='{1}' does not close "
          "hunk A=[{2},{3}) B=[{4},{5}) coverA=[{6},{7}) coverB=[{8},{9})",
          m.id, m.name, h.aStart, h.aEnd, h.bStart, h.bEnd, plan->covLoA,
          plan->covHiA, plan->bTokStart, plan->bTokEnd);
      closesAllHunks = false;
      break;
    }

    if (!closesAllHunks)
      continue;

    trace("fallback",
          "macro-owner closure root accepted: id={0} name='{1}' "
          "source=[{2},{3}) coverA=[{4},{5}) B=[{6},{7})",
          m.id, m.name, *m.invB, *m.invE, plan->covLoA, plan->covHiA,
          plan->bTokStart, plan->bTokEnd);
    candidates.push_back(MacroOwnerClosureCandidate{&m, *plan});
  }

  if (candidates.empty()) {
    trace("fallback", "macro-owner closure witness rejected: no single-owner "
                      "root candidate survived");
    return std::nullopt;
  }

  // Ambiguity is out-of-domain for the first proof class. The stage only
  // accepts one surviving root; multiple survivors require a later explicit
  // compatibility / minimal-frontier proof.
  if (candidates.size() != 1) {
    std::string ids;
    for (const auto &cand : candidates) {
      if (!ids.empty())
        ids += ",";
      ids += llvm::utostr(cand.root->id);
    }
    trace("fallback",
          "macro-owner closure witness rejected: ambiguous surviving roots={0}",
          ids);
    return std::nullopt;
  }

  const auto &cand = candidates.front();
  ExpansionClosureWitness witness;

  // Record only the proved owner interval and the cover/B frontiers needed by
  // later synthesis. This witness builder does not attempt to materialize text yet.
  witness.kind = ExpansionClosureKind::MacroOwnerWholeExpansion;
  witness.hasRootMacroId = true;
  witness.rootMacroId = cand.root->id;
  witness.hasOwnerIncludeId = false;
  witness.ownerIncludeId = 0;
  witness.sourceByteBegin = *cand.root->invB;
  witness.sourceByteEnd = *cand.root->invE;
  witness.coverATokBegin = cand.plan.covLoA;
  witness.coverATokEnd = cand.plan.covHiA;
  witness.bTokBegin = cand.plan.bTokStart;
  witness.bTokEnd = cand.plan.bTokEnd;
  witness.selfContained = cand.plan.selfContained;
  witness.claimsClipped = cand.plan.claimsClipped;
  return witness;
}

bool RefoldEngine::MacroOwnerExpansionClosureWitnessIsDominated(
    const ExpansionClosureWitness &witness, std::string &detail) const {
  auto reject = [&](std::string why) -> bool {
    detail = std::move(why);
    return false;
  };

  // This audit is intentionally definitionally aligned with the fallback
  // witness domain. Any other witness kind is not evidence about the legacy
  // macro-owner expansion closure path, so it cannot prove dominance.
  if (witness.kind != ExpansionClosureKind::MacroOwnerWholeExpansion ||
      !witness.hasRootMacroId || witness.hasOwnerIncludeId) {
    return reject(
        formatv("witness is outside macro-owner whole-expansion domain: {0}",
                FormatExpansionClosureWitness(witness))
            .str());
  }

  const RefoldModel::MacroInvocation *root =
      FindMacroInvocationById(witness.rootMacroId);
  if (!root) {
    return reject(formatv("root macro id {0} is not present in the model",
                          witness.rootMacroId)
                      .str());
  }

  if (!root->invB || !root->invE || *root->invE <= *root->invB) {
    return reject(formatv("root macro id {0} name='{1}' lacks a valid callsite "
                          "source interval",
                          root->id, root->name)
                      .str());
  }

  // Recompute the same whole-cover plan used by normal macro candidate
  // construction. A dominated fallback witness must be reproducible from the
  // ordinary selector path without relying on post-terminal state.
  auto plan = ComputeWholeCoverPlan(*root);
  if (!plan) {
    return reject(formatv("root macro id {0} name='{1}' no longer has a normal "
                          "whole-cover plan",
                          root->id, root->name)
                      .str());
  }

  const bool planMatchesWitness =
      plan->covLoA == witness.coverATokBegin &&
      plan->covHiA == witness.coverATokEnd &&
      plan->bTokStart == witness.bTokBegin &&
      plan->bTokEnd == witness.bTokEnd &&
      plan->selfContained == witness.selfContained &&
      plan->claimsClipped == witness.claimsClipped;
  if (!planMatchesWitness) {
    return reject(
        formatv("normal whole-cover plan for root macro id {0} name='{1}' does "
                "not match fallback witness: plan coverA=[{2},{3}) B=[{4},{5}) "
                "selfContained={6} claimsClipped={7}; witness={8}",
                root->id, root->name, plan->covLoA, plan->covHiA,
                plan->bTokStart, plan->bTokEnd, plan->selfContained ? 1 : 0,
                plan->claimsClipped ? 1 : 0,
                FormatExpansionClosureWitness(witness))
            .str());
  }

  // Restamp the recomputed plan exactly as
  // BuildMacroInvocationPatchWholeCover() does for its normal
  // WholeCoverRealization candidate, then require that the normalized proof
  // carrier discharges. This proves the witness is not merely a raw replacement
  // string; it is visible through the first-class macro realization proof
  // class.
  MacroPatch patch{*root->invB, *root->invE, plan->clippedText, root->id};
  StampMacroWholeCoverRealizationPatch(patch, *plan, root->id);
  const AcceptedResultCandidate candidate = BuildAcceptedMacroCandidate(patch);
  const ProofSummary &summary = candidate.proofSummary;
  if (candidate.kind != AcceptedResultCandidateKind::MacroPatch ||
      summary.inventory.currentPath !=
          AcceptedPathKind::MacroWholeCoverRealization ||
      summary.discharge.status != ProofDischargeStatus::Discharged) {
    return reject(
        formatv("normal whole-cover candidate for root macro id {0} name='{1}' "
                "did not discharge as MacroWholeCoverRealization: {2}",
                root->id, root->name, FormatAcceptedResultCandidate(candidate))
            .str());
  }

  // Return true when the hunk is fully contained by the whole-cover plan's
  // A-side cover and, for non-empty B ranges, by the plan's B-token envelope.
  auto hunkIsClosedByPlan = [&](const diffutils::Hunk &h) -> bool {
    const bool aClosed =
        h.isInsertOnly() ? (plan->covLoA < h.aStart && h.aStart < plan->covHiA)
                         : (plan->covLoA <= h.aStart && h.aEnd <= plan->covHiA);
    if (!aClosed)
      return false;
    if (h.bStart < h.bEnd &&
        (h.bStart < plan->bTokStart || h.bEnd > plan->bTokEnd))
      return false;
    return true;
  };

  // Return true when `selected` reaches `root` by following ordinary
  // callerMacroId links without cycles or broken parent references.
  auto reachesRootThroughNormalClimb =
      [&](const RefoldModel::MacroInvocation *selected) -> bool {
    DenseSet<uint64_t> visited;
    for (const RefoldModel::MacroInvocation *cur = selected; cur;) {
      if (!visited.insert(cur->id).second)
        return false;
      if (cur->id == root->id)
        return true;
      if (!cur->callerMacroId)
        return false;
      cur = FindMacroInvocationById(*cur->callerMacroId);
    }
    return false;
  };

  const StringRef tuPath = model_.GetSourcePath();
  if (tuPath.empty())
    return reject(
        "empty TU source path while auditing macro-owner fallback dominance");

  unsigned closedHunks = 0;
  for (const diffutils::Hunk &h : abTokHunks_) {
    if (!hunkIsClosedByPlan(h)) {
      return reject(
          formatv(
              "root macro id {0} name='{1}' normal plan does not close hunk "
              "A=[{2},{3}) B=[{4},{5}) coverA=[{6},{7}) coverB=[{8},{9})",
              root->id, root->name, h.aStart, h.aEnd, h.bStart, h.bEnd,
              plan->covLoA, plan->covHiA, plan->bTokStart, plan->bTokEnd)
              .str());
    }

    // Mirror the ordinary hunk-classification selector exactly: classify the
    // hunk's current owner, ask for the smallest patchable covering macro, then
    // allow the existing normal path's caller-chain climb to reach the root.
    Owner owner = ClassifyOwnerWithSegments(tuPath, h);
    const RefoldModel::MacroInvocation *selected =
        SmallestCoveringPatchableMacro(h.aStart, h.aEnd, owner.includeId);
    if (!selected) {
      return reject(
          formatv("normal selector found no covering macro for hunk "
                  "A=[{0},{1}) B=[{2},{3}) ownerKind={4} ownerInclude={5}",
                  h.aStart, h.aEnd, h.bStart, h.bEnd, owner.kind,
                  owner.includeId.value_or(0))
              .str());
    }

    if (!selected->invB || !selected->invE) {
      return reject(formatv("normal selector chose macro id {0} name='{1}' for "
                            "hunk A=[{2},{3}) but it lacks callsite provenance",
                            selected->id, selected->name, h.aStart, h.aEnd)
                        .str());
    }

    if (!reachesRootThroughNormalClimb(selected)) {
      return reject(formatv("normal selector chose macro id {0} name='{1}' for "
                            "hunk A=[{2},{3}), but its caller chain does not "
                            "reach fallback root id {4} name='{5}'",
                            selected->id, selected->name, h.aStart, h.aEnd,
                            root->id, root->name)
                        .str());
    }
    ++closedHunks;
  }

  detail =
      formatv(
          "dominated by normal MacroWholeCoverRealization root id={0} "
          "name='{1}' source=[{2},{3}) coverA=[{4},{5}) B=[{6},{7}) hunks={8}",
          root->id, root->name, *root->invB, *root->invE, plan->covLoA,
          plan->covHiA, plan->bTokStart, plan->bTokEnd, closedHunks)
          .str();
  return true;
}

std::optional<RefoldEngine::TextEdit>
RefoldEngine::BuildTUIncludeClosureEditForUnresolvedHunk(
    const diffutils::Hunk &h, StringRef tuPath, StringRef tuBytes,
    ArrayRef<std::pair<uint64_t, uint64_t>> stagedSourceIntervals) const {
  // This helper is intentionally narrower than ordinary include realization.
  // It is the first hybrid source-closure class used to keep already-proved
  // structural artifacts (for example a preserved macro callsite rewrite)
  // while materializing one otherwise-unresolved PP region directly into the
  // TU source. Stay fail-closed unless the closure is exact or can be widened
  // without absorbing any independent token diff.
  if (h.isInsertOnly()) {
    trace("fallback",
          "TU include-closure rejected: insert-only hunk A=[{0},{1}) "
          "B=[{2},{3}) is outside the first closure-edit domain",
          h.aStart, h.aEnd, h.bStart, h.bEnd);
    return std::nullopt;
  }

  SmallVector<const RefoldModel::IncludeItem *, 8> touched;
  for (const auto &inc : model_.GetIncludes()) {
    // Only top-level includes spelled directly in the TU are eligible for this
    // first source-closure class. Nested includes already have a dedicated
    // include-materialization path, while non-TU sites would need a different
    // embedding proof.
    if (inc.parent || !PathsEqual(inc.sitePath, tuPath))
      continue;
    if (inc.cover.end <= h.aStart || inc.cover.begin >= h.aEnd)
      continue;
    touched.push_back(&inc);
  }

  if (touched.empty()) {
    trace("fallback",
          "TU include-closure rejected: no top-level TU include touches hunk "
          "A=[{0},{1})",
          h.aStart, h.aEnd);
    return std::nullopt;
  }

  // Process touched includes in TU order so the closure can be proven as one
  // contiguous include cover rather than as a set of unrelated include ranges.
  llvm::sort(touched, [](const RefoldModel::IncludeItem *lhs,
                         const RefoldModel::IncludeItem *rhs) {
    if (lhs->siteB != rhs->siteB)
      return lhs->siteB < rhs->siteB;
    return lhs->id < rhs->id;
  });

  const RefoldModel::IncludeItem *first = touched.front();
  const RefoldModel::IncludeItem *last = touched.back();

  uint64_t coverBegin = std::numeric_limits<uint64_t>::max();
  uint64_t coverEnd = 0;
  for (const RefoldModel::IncludeItem *inc : touched) {
    if (inc->cover.end <= inc->cover.begin) {
      trace("fallback",
            "TU include-closure rejected: touched include inc#{0} has empty "
            "or invalid A-cover [{1},{2})",
            inc->id, inc->cover.begin, inc->cover.end);
      return std::nullopt;
    }
    coverBegin = std::min(coverBegin, inc->cover.begin);
    coverEnd = std::max(coverEnd, inc->cover.end);
  }

  // Pure include closure is the old include-only proof: touched include covers
  // must form one contiguous A-token run so their B material can be projected as
  // a single include realization.  Mixed TU/include closure is broader.  It may
  // touch several include runs separated by TU-owned material, but only if the
  // later whole-envelope proof consumes that intervening material as part of the
  // same source replacement.  Do not reject split include runs here; treating
  // them as mixed gives the material-gap verifier a chance to prove or reject
  // the complete source interval deterministically.
  SmallVector<const RefoldModel::IncludeItem *, 8> coverOrdered = touched;
  llvm::sort(coverOrdered, [](const RefoldModel::IncludeItem *lhs,
                              const RefoldModel::IncludeItem *rhs) {
    if (lhs->cover.begin != rhs->cover.begin)
      return lhs->cover.begin < rhs->cover.begin;
    if (lhs->cover.end != rhs->cover.end)
      return lhs->cover.end < rhs->cover.end;
    return lhs->id < rhs->id;
  });

  bool includeCoversContiguous = true;
  uint64_t expectedCoverBegin = coverBegin;
  for (const RefoldModel::IncludeItem *inc : coverOrdered) {
    if (inc->cover.begin != expectedCoverBegin) {
      includeCoversContiguous = false;
      trace("fallback",
            "TU include-closure treating touched includes as mixed: split "
            "A-cover at inc#{0} cover=[{1},{2}) expectedBegin={3}",
            inc->id, inc->cover.begin, inc->cover.end, expectedCoverBegin);
      break;
    }
    expectedCoverBegin = inc->cover.end;
  }

  // `exactCover` means the edit already matches the include closure. The
  // wider `hunkInsideCover` case is a controlled widening: the hunk may be
  // absorbed only if later checks prove the wider include closure is safe. Split
  // include runs are always mixed, even when the hunk lies inside the min/max
  // include cover, because the intervening TU-owned material must be consumed by
  // the whole-envelope proof rather than by pure include projection.
  const bool exactCover = includeCoversContiguous && coverBegin == h.aStart &&
                          coverEnd == h.aEnd;
  const bool hunkInsideCover = includeCoversContiguous &&
                               coverBegin <= h.aStart && h.aEnd <= coverEnd;

  // Return true when `cand` would be absorbed by the closed A-side range
  // [begin, end]. Token-consuming edits use interval overlap; pure insertions
  // use the closed insertion-gap test.
  auto hunkTouchesAClosedRange = [](const diffutils::Hunk &cand, uint64_t begin,
                                    uint64_t end) -> bool {
    if (end < begin)
      end = begin;

    // A-consuming edits touch the extra cover when their token intervals
    // overlap it. Pure insertions touch the cover when their insertion anchor
    // lies in the closed gap interval: a widened include materialization would
    // otherwise absorb the insertion payload without going through the normal
    // candidate-conflict law.
    if (cand.aStart < cand.aEnd)
      return cand.aStart < end && begin < cand.aEnd;
    return cand.bStart < cand.bEnd && cand.aStart >= begin &&
           cand.aStart <= end;
  };

  // Return true when the proposed widened A range would cover any other token
  // diff hunk besides the hunk currently being realized.
  auto rangeHasForeignTokenDiff = [&](uint64_t begin, uint64_t end) -> bool {
    for (const diffutils::Hunk &cand : abTokHunks_) {
      if (cand == h)
        continue;
      if (hunkTouchesAClosedRange(cand, begin, end))
        return true;
    }
    return false;
  };

  // Return true when the proposed widened source range would overlap an edit
  // that has already been staged. Zero-width staged edits are treated as closed
  // insertion points.
  auto sourceTouchesStagedEdit = [&](uint64_t begin, uint64_t end) -> bool {
    for (const auto &interval : stagedSourceIntervals) {
      if (interval.first < interval.second) {
        if (interval.first < end && begin < interval.second)
          return true;
        continue;
      }

      if (begin <= interval.first && interval.first <= end)
        return true;
    }
    return false;
  };

  // The include-only closure case below materializes exactly the include cover.
  // A mixed TU/include hunk is wider: the edit consumes tokens on both sides of
  // the include cover, so the A materialization range must close over the whole
  // touched token interval.
  const uint64_t closureBegin = std::min(h.aStart, coverBegin);
  const uint64_t closureEnd = std::max(h.aEnd, coverEnd);
  const bool mixedTUIncludeClosure = !hunkInsideCover;

  // Defaults for the pure include-closure case.  The mixed case widens both the
  // materialized A token range and the source byte interval below.
  uint64_t materialBeginA = coverBegin;
  uint64_t materialEndA = coverEnd;
  uint64_t sourceBegin = first->siteB;
  uint64_t sourceEnd = last->siteE;

  // Return true iff the PP token is part of one of the include expansions that
  // this closure is explicitly consuming.
  auto tokenCoveredByTouchedInclude = [&](uint64_t pp) -> bool {
    for (const RefoldModel::IncludeItem *inc : touched)
      if (inc->cover.begin <= pp && pp < inc->cover.end)
        return true;
    return false;
  };

  // Return true iff this include directive belongs to the touched include run
  // whose expansion participates in the closure proof.
  auto includeIsTouched = [&](const RefoldModel::IncludeItem &inc) -> bool {
    for (const RefoldModel::IncludeItem *touchedInc : touched)
      if (touchedInc->id == inc.id)
        return true;
    return false;
  };

  // Return true iff the macro invocation is part of the source spelling of a
  // touched include directive.  Macro-expanded include targets, for example
  //
  //   #define HDR "two.inc"
  //   #include HDR
  //
  // are control spelling for the include directive, not independent TU material.
  // When the closure consumes the touched include site, complete macro calls
  // wholly inside that site are consumed with it.  Partial overlaps remain
  // rejected by the ordinary macro-overlap guard below.
  auto macroInvocationIsInsideTouchedIncludeDirective =
      [&](const RefoldModel::MacroInvocation &m) -> bool {
    if (!m.invFile || m.invFile->empty() || !PathsEqual(*m.invFile, tuPath))
      return false;
    if (!m.invB || !m.invE || *m.invB >= *m.invE)
      return false;

    for (const RefoldModel::IncludeItem *inc : touched) {
      if (!PathsEqual(inc->sitePath, tuPath))
        continue;
      if (inc->siteB <= *m.invB && *m.invE <= inc->siteE)
        return true;
    }
    return false;
  };

  // Return true iff the pragma is explicitly classified as source-neutral when
  // its containing include directive is consumed by a zero-token gap closure.
  //
  // Keep this whitelist deliberately small.  Most pragmas model compiler state
  // rather than token material, and the refold map does not currently carry the
  // state-region information needed to replay or repair them.  `#pragma once`
  // is the one admitted spelling because, for an otherwise source-neutral
  // zero-token include, suppressing future textual inclusion of the same
  // zero-token file cannot change the PP token stream.
  auto pragmaIsConsumableIncludeLocalState =
      [&](const RefoldModel::PragmaDirective &pragma) -> bool {
    StringRef text = pragma.text.trim();
    size_t pos = 0;

    auto consumeWord = [&](StringRef word) {
      if (!text.substr(pos).starts_with(word))
        return false;
      pos += word.size();
      if (pos < text.size() && stringutils::isIdentPart(text[pos]))
        return false;
      return true;
    };

    stringutils::skipNonNewlineWs(text, pos);
    if (pos >= text.size() || text[pos] != '#')
      return false;
    ++pos;

    stringutils::skipNonNewlineWs(text, pos);
    if (!consumeWord("pragma"))
      return false;

    stringutils::skipNonNewlineWs(text, pos);
    if (!consumeWord("once"))
      return false;

    stringutils::skipNonNewlineWs(text, pos);

    // Treat comments after `once` as directive trivia, not as another pragma
    // operand.  Clang accepts `#pragma once /* ... */`, and for refolding it
    // has the same include-local state effect as the bare spelling.  Reuse the
    // existing trivia recognizer so incomplete comments remain fail-closed.
    return isWsOrCompleteCommentTrivia(text.drop_front(pos));
  };

  // Return true iff the include has structure other than an explicitly
  // consumable pragma.  This is stricter than the general zero-token include
  // proof: `#pragma once` is considered source-neutral only for an otherwise
  // empty include.  If a pragma appears next to macro-state, conditionals,
  // nested includes, declarations, or any other recorded structure, keep the
  // include outside this proof class until the refold map records enough
  // pragma-state information to prove the combined effect.
  auto includeHasNonPragmaStructure =
      [&](const RefoldModel::IncludeItem &inc) -> bool {
    if (!inc.decls.empty())
      return true;

    for (const auto &child : model_.GetIncludes())
      if (child.parent && *child.parent == inc.id)
        return true;

    for (const auto &directive : model_.GetMacroDirectives())
      if (directive.ownerIncludeId && *directive.ownerIncludeId == inc.id)
        return true;

    for (const auto &group : model_.GetConds())
      if (group.parentIncludeId && *group.parentIncludeId == inc.id)
        return true;

    return false;
  };

  auto includePathForDiagnostic = [](const RefoldModel::IncludeItem &inc) {
    return inc.resolvedPath ? *inc.resolvedPath : inc.target;
  };

  // Return the first recorded reason that prevents treating this include as a
  // consumable zero-token source gap, or std::nullopt when the include tree is
  // source-neutral for this closure proof.
  //
  // The predicate is intentionally transitive: a top-level zero-token include
  // may contain nested zero-token includes, and the outer directive is still
  // safe to consume when every descendant is itself source-neutral.  Returning a
  // concrete reason keeps the later terminal-fallback trace actionable instead
  // of merely saying that an unrelated include was absorbed.
  std::function<std::optional<std::string>(const RefoldModel::IncludeItem &,
                                           DenseSet<uint64_t> &)>
      includeRecordedSideEffectReasonImpl;
  includeRecordedSideEffectReasonImpl =
      [&](const RefoldModel::IncludeItem &inc,
          DenseSet<uint64_t> &visiting) -> std::optional<std::string> {
    // A cycle would indicate malformed include-parent metadata.  Do not try to
    // prove closure through it; reject rather than deleting uncertain state.
    if (!visiting.insert(inc.id).second)
      return llvm::formatv("include-parent cycle reaches include id={0}",
                           inc.id)
          .str();

    auto finish = [&](std::optional<std::string> reason) {
      visiting.erase(inc.id);
      return reason;
    };

    // Header declarations are semantic structure, even if this include did not
    // contribute tokens to the particular A-side hunk being refolded.
    if (!inc.decls.empty())
      return finish(llvm::formatv("include id={0} path='{1}' has {2} "
                                  "recorded header declaration(s)",
                                  inc.id, includePathForDiagnostic(inc),
                                  inc.decls.size())
                        .str());

    // A nested include is not inherently a side effect.  It is consumable when
    // its own expansion is empty and its descendants/directives are consumable
    // by the same zero-token include-gap proof.  Record which child blocked the
    // proof so the mixed-closure rejection points at the real source structure.
    for (const auto &child : model_.GetIncludes()) {
      if (!child.parent || *child.parent != inc.id)
        continue;
      if (child.cover.IsValid())
        return finish(llvm::formatv("child include id={0} path='{1}' "
                                    "materializes PP cover=[{2},{3})",
                                    child.id, includePathForDiagnostic(child),
                                    child.cover.begin, child.cover.end)
                          .str());
      if (std::optional<std::string> childReason =
              includeRecordedSideEffectReasonImpl(child, visiting))
        return finish(llvm::formatv("child include id={0} path='{1}' rejected: "
                                    "{2}",
                                    child.id, includePathForDiagnostic(child),
                                    *childReason)
                          .str());
    }

    // Include-owned #define/#undef directives are macro-state transitions, not
    // PP tokens.  When their containing zero-token include lies inside the
    // replacement envelope, the directive is consumed with that envelope.  Any
    // surviving source that still observes the removed macro state is handled
    // later by the macro-state liveness repair passes: consumed #defines can be
    // preserved or replaced with whole-cover callsite realizations, and
    // consumed #undefs can be preserved when removing them would resurrect an
    // earlier definition.  Directives outside that nameable macro-state class
    // remain side-effect-bearing structure that this closure must not erase.
    for (const auto &directive : model_.GetMacroDirectives()) {
      if (directive.ownerIncludeId && *directive.ownerIncludeId == inc.id &&
          directive.subkind != "#define" && directive.subkind != "#undef") {
        return finish(
            llvm::formatv("include id={0} path='{1}' owns "
                          "non-consumable macro directive id={2} "
                          "subkind='{3}' text='{4}'",
                          inc.id, includePathForDiagnostic(inc), directive.id,
                          directive.subkind,
                          stringutils::showWsWithClip(directive.text, 120))
                .str());
      }
    }

    // Conditional control directives are also source structure, but an
    // include-local conditional group with no materialized A tokens is source
    // order only: it contributed no PP material and any real side effects
    // inside the group are represented separately as macro directives, child
    // includes, pragmas, or declarations.  When such a group lies inside the
    // source envelope being replaced, consuming it is the same kind of closure
    // as consuming an empty include gap.  Reject only a conditional group that
    // owns materialized tokens; that would make this a token-producing include,
    // not a zero-token gap.
    for (const auto &group : model_.GetConds()) {
      if (!group.parentIncludeId || *group.parentIncludeId != inc.id)
        continue;

      for (const RefoldModel::CondArm &arm : group.arms) {
        if (arm.span && arm.span->IsValid() &&
            arm.span->begin < arm.span->end) {
          return finish(llvm::formatv("include id={0} path='{1}' owns "
                                      "conditional group id={2} arm id={3} "
                                      "with materialized PP span=[{4},{5})",
                                      inc.id, includePathForDiagnostic(inc),
                                      group.id, arm.id, arm.span->begin,
                                      arm.span->end)
                            .str());
        }
      }
    }

    // Pragmas are keyed by physical file path rather than include id, so check
    // the resolved header path when available.  Unknown pragmas are always
    // side-effect-bearing here: they can affect diagnostics, layout,
    // optimization, visibility, attributes, or later preprocessing state while
    // contributing no PP tokens.  The only pragma admitted by this closure is
    // an explicitly parsed `#pragma once` in an otherwise source-neutral
    // zero-token include.  Anything richer must wait for a pragma-state proof
    // or a repair mechanism analogous to macro-state liveness.
    if (inc.resolvedPath)
      for (const auto &pragma : model_.GetPragmas()) {
        if (!PathsEqual(pragma.sitePath, *inc.resolvedPath))
          continue;
        if (!pragmaIsConsumableIncludeLocalState(pragma)) {
          return finish(
              llvm::formatv("include id={0} path='{1}' contains "
                            "non-consumable pragma id={2} text='{3}'",
                            inc.id, includePathForDiagnostic(inc), pragma.id,
                            stringutils::showWsWithClip(pragma.text, 120))
                  .str());
        }
        if (includeHasNonPragmaStructure(inc)) {
          return finish(llvm::formatv("include id={0} path='{1}' contains "
                                      "#pragma once with additional recorded "
                                      "source structure",
                                      inc.id, includePathForDiagnostic(inc))
                            .str());
        }
      }

    return finish(std::nullopt);
  };

  auto includeRecordedSideEffectReason =
      [&](const RefoldModel::IncludeItem &inc) -> std::optional<std::string> {
    DenseSet<uint64_t> visiting;
    return includeRecordedSideEffectReasonImpl(inc, visiting);
  };

  auto includeHasRecordedSideEffects =
      [&](const RefoldModel::IncludeItem &inc) -> bool {
    return includeRecordedSideEffectReason(inc).has_value();
  };

  auto rawIdentifierAppearsInHunkReplacement = [&](StringRef name) -> bool {
    if (name.empty())
      return false;

    StringRef text = SliceBSource(h.bStart, h.bEnd);
    if (text.empty())
      return false;

    const SourceLocation baseLoc = SourceLocation::getFromRawEncoding(1);
    std::string lexBuf = text.str();
    lexBuf.push_back('\0');

    const char *bufStart = lexBuf.data();
    const char *bufEnd = bufStart + text.size();
    Lexer lexer(baseLoc, lexLang_, bufStart, bufStart, bufEnd);
    lexer.SetCommentRetentionState(true);

    Token token;
    while (true) {
      lexer.LexFromRawLexer(token);
      if (token.is(tok::eof))
        return false;
      if (token.is(tok::comment))
        continue;
      if (!token.is(tok::raw_identifier) && !token.is(tok::identifier))
        continue;

      const size_t localBegin = tokenOffsetFromBase(token, baseLoc);
      const size_t localEnd = tokenEndOffsetFromBase(token, baseLoc);
      if (localEnd < localBegin || localEnd > text.size())
        continue;
      if (text.slice(localBegin, localEnd) == name)
        return true;
    }
  };

  auto functionLikeInvocationAppearsInHunkReplacement =
      [&](StringRef name) -> bool {
        if (name.empty())
          return false;

        StringRef text = SliceBSource(h.bStart, h.bEnd);
        if (text.empty())
          return false;

        const SourceLocation baseLoc = SourceLocation::getFromRawEncoding(1);
        std::string lexBuf = text.str();
        lexBuf.push_back('\0');

        const char *bufStart = lexBuf.data();
        const char *bufEnd = bufStart + text.size();
        Lexer lexer(baseLoc, lexLang_, bufStart, bufStart, bufEnd);
        lexer.SetCommentRetentionState(true);

        bool pendingName = false;
        Token token;
        while (true) {
          lexer.LexFromRawLexer(token);
          if (token.is(tok::eof))
            return false;
          if (token.is(tok::comment))
            continue;

          if (pendingName) {
            if (token.is(tok::l_paren))
              return true;
            pendingName = false;
          }

          if (!token.is(tok::raw_identifier) && !token.is(tok::identifier))
            continue;

          const size_t localBegin = tokenOffsetFromBase(token, baseLoc);
          const size_t localEnd = tokenEndOffsetFromBase(token, baseLoc);
          if (localEnd < localBegin || localEnd > text.size())
            continue;
          pendingName = text.slice(localBegin, localEnd) == name;
        }
      };

  auto macroDirectiveNameFromRecordedText =
      [&](const RefoldModel::MacroDirective &directive)
          -> std::optional<std::string> {
    if (directive.subkind != "#define" && directive.subkind != "#undef")
      return std::nullopt;

    StringRef text = directive.text.ltrim();
    if (!text.consume_front("#"))
      return std::nullopt;
    text = text.ltrim();

    StringRef keyword = directive.subkind.drop_front();
    if (!text.consume_front(keyword))
      return std::nullopt;
    if (!text.empty() && stringutils::isIdentPart(text.front()))
      return std::nullopt;

    text = text.ltrim();
    if (text.empty() || !stringutils::isIdentStart(text.front()))
      return std::nullopt;

    size_t end = 1;
    while (end < text.size() && stringutils::isIdentPart(text[end]))
      ++end;
    return text.take_front(end).str();
  };

  auto macroDirectiveIsFunctionLikeDefinition =
      [&](const RefoldModel::MacroDirective &directive, StringRef macroName) {
        if (directive.subkind != "#define")
          return false;

        StringRef text = directive.text;
        size_t pos = 0;
        stringutils::skipNonNewlineWs(text, pos);
        if (pos >= text.size() || text[pos] != '#')
          return false;
        ++pos;
        stringutils::skipNonNewlineWs(text, pos);

        StringRef keyword = "define";
        if (!text.substr(pos).starts_with(keyword))
          return false;
        pos += keyword.size();
        if (pos < text.size() && stringutils::isIdentPart(text[pos]))
          return false;
        stringutils::skipNonNewlineWs(text, pos);

        const size_t nameBegin = pos;
        if (pos >= text.size() || !stringutils::isIdentStart(text[pos]))
          return false;
        ++pos;
        while (pos < text.size() && stringutils::isIdentPart(text[pos]))
          ++pos;

        return text.slice(nameBegin, pos) == macroName && pos < text.size() &&
               text[pos] == '(';
      };

  // Return true when the B-side replacement hunk can observe a macro-state
  // transition preserved after that hunk. Object-like definitions and #undef
  // transitions are conservatively keyed by identifier occurrence; a
  // function-like #define is keyed by actual invocation syntax instead.
  auto hunkReplacementObservesMacroStateDirective =
      [&](const RefoldModel::MacroDirective &directive) {
        std::optional<std::string> name =
            macroDirectiveNameFromRecordedText(directive);
        if (!name)
          return true;
        if (macroDirectiveIsFunctionLikeDefinition(directive, *name))
          return functionLikeInvocationAppearsInHunkReplacement(*name);
        return rawIdentifierAppearsInHunkReplacement(*name);
      };

  auto includeIsDescendantOf = [&](const RefoldModel::IncludeItem &candidate,
                                   const RefoldModel::IncludeItem &root) {
    std::optional<uint64_t> cur = candidate.parent;
    while (cur) {
      if (*cur == root.id)
        return true;
      const RefoldModel::IncludeItem *parent = nullptr;
      for (const auto &inc : model_.GetIncludes())
        if (inc.id == *cur) {
          parent = &inc;
          break;
        }
      if (!parent)
        return false;
      cur = parent->parent;
    }
    return false;
  };

  // Return true iff a complete include directive can be carried forward as part
  // of a preserved conditional island even though its subtree has modeled
  // macro-state effects.
  //
  // This is deliberately stricter than ordinary include preservation and
  // different from the zero-token include consumption proof above.  The include
  // is not deleted: its directive remains in the emitted conditional island, so
  // #define/#undef state transitions and #pragma once remain in source order.
  // Still fail closed for declarations, token-producing descendants, generic
  // pragmas, non-macro-state directives, and replacement text that mentions a
  // macro whose state would be introduced only after the replacement payload.
  std::function<bool(const RefoldModel::IncludeItem &, DenseSet<uint64_t> &)>
      includeIsPreservableConditionalStateIncludeImpl;
  includeIsPreservableConditionalStateIncludeImpl =
      [&](const RefoldModel::IncludeItem &inc, DenseSet<uint64_t> &visiting)
          -> bool {
    if (inc.cover.IsValid())
      return false;
    if (!visiting.insert(inc.id).second)
      return false;

    auto finish = [&](bool result) {
      visiting.erase(inc.id);
      return result;
    };

    if (!inc.decls.empty())
      return finish(false);

    for (const auto &child : model_.GetIncludes()) {
      if (!child.parent || *child.parent != inc.id)
        continue;
      if (!includeIsPreservableConditionalStateIncludeImpl(child, visiting))
        return finish(false);
    }

    for (const auto &directive : model_.GetMacroDirectives()) {
      if (!directive.ownerIncludeId)
        continue;
      bool ownedByInclude = *directive.ownerIncludeId == inc.id;
      if (!ownedByInclude)
        for (const auto &child : model_.GetIncludes())
          if (child.id == *directive.ownerIncludeId &&
              includeIsDescendantOf(child, inc)) {
            ownedByInclude = true;
            break;
          }
      if (!ownedByInclude)
        continue;

      if (hunkReplacementObservesMacroStateDirective(directive))
        return finish(false);
    }

    for (const auto &group : model_.GetConds()) {
      if (!group.parentIncludeId)
        continue;
      bool ownedByInclude = *group.parentIncludeId == inc.id;
      if (!ownedByInclude)
        for (const auto &child : model_.GetIncludes())
          if (child.id == *group.parentIncludeId &&
              includeIsDescendantOf(child, inc)) {
            ownedByInclude = true;
            break;
          }
      if (!ownedByInclude)
        continue;
      for (const RefoldModel::CondArm &arm : group.arms)
        if (arm.span && arm.span->IsValid() && arm.span->begin < arm.span->end)
          return finish(false);
    }

    for (const auto &macro : model_.GetMacroInvocations()) {
      if (!macro.ownerIncludeId)
        continue;
      bool ownedByInclude = *macro.ownerIncludeId == inc.id;
      if (!ownedByInclude)
        for (const auto &child : model_.GetIncludes())
          if (child.id == *macro.ownerIncludeId &&
              includeIsDescendantOf(child, inc)) {
            ownedByInclude = true;
            break;
          }
      if (!ownedByInclude)
        continue;
      if (macro.cover.IsValid())
        return finish(false);
      for (const auto &span : macro.stringifySpans)
        if (span.IsValid())
          return finish(false);
      for (const auto &span : macro.pasteSpans)
        if (span.IsValid())
          return finish(false);
    }

    if (inc.resolvedPath)
      for (const auto &pragma : model_.GetPragmas())
        if (PathsEqual(pragma.sitePath, *inc.resolvedPath) &&
            !pragmaIsConsumableIncludeLocalState(pragma))
          return finish(false);

    return finish(true);
  };

  auto includeIsPreservableConditionalStateInclude =
      [&](const RefoldModel::IncludeItem &inc) -> bool {
    DenseSet<uint64_t> visiting;
    return includeIsPreservableConditionalStateIncludeImpl(inc, visiting);
  };

  struct MacroDefinitionReplacementList {
    const RefoldModel::MacroDirective *directive = nullptr;
    size_t nameTextBegin = 0;
    size_t replacementTextBegin = 0;
    uint64_t fileBase = 0;
    uint64_t fileBegin = 0;
    uint64_t fileEnd = 0;
  };

  auto findMacroDefinitionDirective =
      [&](const RefoldModel::MacroInvocation &m)
          -> const RefoldModel::MacroDirective * {
    if (!m.definitionDirectiveId)
      return nullptr;

    for (const auto &directive : model_.GetMacroDirectives())
      if (directive.id == *m.definitionDirectiveId)
        return &directive;
    return nullptr;
  };

  // Parse the recorded #define text and return the replacement-list range in
  // both directive-text coordinates and source-file byte coordinates.
  //
  // MacroDirective::text contains the full directive spelling, but siteB/siteE
  // anchor only the directive payload in the source file: siteB is the byte of
  // the macro name, not the byte of '#'.  Keep the text offset of the name so
  // nested macro callsites recorded inside a replacement list can be compared
  // against the correct source-file byte interval.
  auto macroDefinitionReplacementList =
      [&](const RefoldModel::MacroInvocation &m)
          -> std::optional<MacroDefinitionReplacementList> {
    const RefoldModel::MacroDirective *definition =
        findMacroDefinitionDirective(m);
    if (!definition || definition->subkind != "#define")
      return std::nullopt;

    StringRef text = definition->text;
    size_t pos = 0;

    // Re-parse only the directive prefix we need: optional horizontal/trivia
    // whitespace, '#', optional whitespace, and the 'define' directive keyword.
    stringutils::skipNonNewlineWs(text, pos);
    if (pos >= text.size() || text[pos] != '#')
      return std::nullopt;
    ++pos;
    stringutils::skipNonNewlineWs(text, pos);

    // Require the exact preprocessing directive keyword, not an identifier that
    // merely starts with "define".
    if (!text.substr(pos).starts_with("define"))
      return std::nullopt;
    pos += StringRef("define").size();
    if (pos < text.size() && stringutils::isIdentPart(text[pos]))
      return std::nullopt;
    stringutils::skipNonNewlineWs(text, pos);

    // The next token must be the macro name recorded on the invocation.  Record
    // its directive-text offset so we can translate later replacement-list
    // offsets back into source-file byte coordinates.
    const size_t nameTextBegin = pos;
    if (!text.substr(pos).starts_with(m.name))
      return std::nullopt;
    pos += m.name.size();

    // Avoid accepting a prefix of a longer macro identifier.
    if (pos < text.size() && stringutils::isIdentPart(text[pos]))
      return std::nullopt;

    if (m.subkind == "func") {
      // Function-like macro definitions require the parameter list to start
      // immediately after the name.  Skip the balanced parameter list; the
      // replacement list begins immediately after the closing paren.
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

    // `siteB` is expected to be the source byte for the macro name.  If a map
    // violates that contract, fail closed rather than comparing nested callsite
    // bytes against a synthetic coordinate system.
    if (definition->siteB < nameTextBegin)
      return std::nullopt;

    MacroDefinitionReplacementList result;
    result.directive = definition;
    result.nameTextBegin = nameTextBegin;
    result.replacementTextBegin = pos;
    result.fileBase = definition->siteB - nameTextBegin;
    result.fileBegin = result.fileBase + pos;
    result.fileEnd = definition->siteE;
    if (result.fileBegin > result.fileEnd ||
        result.fileEnd > result.fileBase + text.size())
      return std::nullopt;
    return result;
  };

  // Return true iff this invocation contributed no A-side PP material.  Such a
  // callsite is source structure rather than token material for the current
  // hunk; if the complete invocation lies inside a proved replacement envelope,
  // the envelope may consume it just like a zero-token include gap.
  auto macroInvocationHasMaterializedPPTokens =
      [&](const RefoldModel::MacroInvocation &m) -> bool {
    if (m.cover.IsValid())
      return true;
    for (const auto &span : m.stringifySpans)
      if (span.IsValid())
        return true;
    for (const auto &span : m.pasteSpans)
      if (span.IsValid())
        return true;
    return false;
  };

  // Recursively prove that a zero-token macro invocation is source-neutral.
  //
  // The old admissible case was a literal empty-replacement-list macro:
  //
  //   #define EMPTY
  //   EMPTY
  //
  // This additionally admits object-like wrapper macros whose replacement list
  // is tiled by recorded nested zero-token macro invocations, for example:
  //
  //   #define EMPTY
  //   #define WRAP_EMPTY EMPTY
  //   WRAP_EMPTY
  //
  // The proof remains intentionally narrow.  Every non-trivia byte in the
  // wrapper's replacement list must belong to a nested macro invocation
  // recorded in the refold map, and every nested invocation must satisfy this
  // same proof. That avoids treating arbitrary non-empty replacement lists,
  // literals, operators, `_Pragma`, or unrecorded identifiers as source-neutral
  // merely because the final PP cover happens to be empty.
  std::function<bool(const RefoldModel::MacroInvocation &,
                     DenseSet<uint64_t> &)>
      macroInvocationIsSourceNeutralZeroTokenImpl;
  macroInvocationIsSourceNeutralZeroTokenImpl =
      [&](const RefoldModel::MacroInvocation &m,
          DenseSet<uint64_t> &visiting) -> bool {
    // A consumable gap macro must not contribute any final PP-token material.
    // If the invocation produced tokens, it is source-bearing and this proof
    // does not apply.
    if (macroInvocationHasMaterializedPPTokens(m))
      return false;

    // Reject recursive/cyclic macro invocation graphs fail-closed.  The proof
    // is structural recursion over the recorded macro DAG, so revisiting the
    // same invocation would mean the map cannot establish a finite neutral
    // tiling.
    if (!visiting.insert(m.id).second)
      return false;

    auto finish = [&](bool result) {
      visiting.erase(m.id);
      return result;
    };

    // Recover the defining #define and the replacement-list byte range. Without
    // a schema-valid definition anchor, nested invocation byte intervals cannot
    // be compared against the replacement list soundly.
    std::optional<MacroDefinitionReplacementList> replacement =
        macroDefinitionReplacementList(m);
    if (!replacement)
      return finish(false);

    StringRef replacementText =
        replacement->directive->text.substr(replacement->replacementTextBegin);

    // A literal empty replacement list is source-neutral by construction.
    if (isWsOrCompleteCommentTrivia(replacementText))
      return finish(true);

    struct ChildPiece {
      uint64_t begin;
      uint64_t end;
      const RefoldModel::MacroInvocation *child;
    };

    auto sortChildren = [](SmallVectorImpl<ChildPiece> &children) {
      llvm::sort(children, [](const ChildPiece &lhs, const ChildPiece &rhs) {
        if (lhs.begin != rhs.begin)
          return lhs.begin < rhs.begin;
        if (lhs.end != rhs.end)
          return lhs.end < rhs.end;
        return lhs.child->id < rhs.child->id;
      });
    };

    enum class ZeroTokenRangeSurface {
      DefinitionReplacementList,
      InvocationArgument
    };

    auto sourceRangeIsTiledByNeutralNestedMacros =
        [&](uint64_t begin, uint64_t end, StringRef rangeFile,
            ZeroTokenRangeSurface surface) -> bool {
      if (begin > end)
        return false;

      // The definition replacement list and the invocation argument can be
      // spelled in the same physical TU file.  Keep the source surface explicit:
      // replacement-list tiling slices the producer-recorded directive text,
      // while invocation-argument tiling slices the materialized TU source.
      auto sliceTrivia = [&](uint64_t sliceBegin, uint64_t sliceEnd)
          -> std::optional<StringRef> {
        if (sliceEnd < sliceBegin)
          return std::nullopt;

        switch (surface) {
        case ZeroTokenRangeSurface::DefinitionReplacementList: {
          if (!PathsEqual(rangeFile, replacement->directive->sitePath))
            return std::nullopt;
          const uint64_t textBegin = replacement->fileBase;
          const uint64_t textEnd =
              replacement->fileBase + replacement->directive->text.size();
          if (sliceBegin < textBegin || sliceEnd > textEnd)
            return std::nullopt;
          return replacement->directive->text.slice(
              static_cast<size_t>(sliceBegin - replacement->fileBase),
              static_cast<size_t>(sliceEnd - replacement->fileBase));
        }
        case ZeroTokenRangeSurface::InvocationArgument:
          if (!PathsEqual(rangeFile, tuPath) || sliceEnd > tuBytes.size())
            return std::nullopt;
          return tuBytes.slice(sliceBegin, sliceEnd);
        }

        return std::nullopt;
      };

      SmallVector<ChildPiece, 8> children;
      for (const auto &candidate : model_.GetMacroInvocations()) {
        if (!candidate.callerMacroId || *candidate.callerMacroId != m.id)
          continue;

        // Only nested callsites physically spelled on this source surface can
        // tile it.  Malformed or out-of-surface candidates are harmless here:
        // if they were needed to cover non-trivia bytes, the later trivia-gap
        // check will reject the range fail-closed.
        if (!candidate.invFile || candidate.invFile->empty() ||
            !PathsEqual(*candidate.invFile, rangeFile))
          continue;
        if (!candidate.invB || !candidate.invE ||
            *candidate.invB >= *candidate.invE)
          continue;
        if (*candidate.invB < begin || end < *candidate.invE)
          continue;
        if (!sliceTrivia(*candidate.invB, *candidate.invE))
          continue;

        children.push_back({*candidate.invB, *candidate.invE, &candidate});
      }

      sortChildren(children);

      uint64_t cursor = begin;
      for (const ChildPiece &piece : children) {
        if (piece.begin < cursor)
          return false;

        std::optional<StringRef> gap = sliceTrivia(cursor, piece.begin);
        if (!gap || !isWsOrCompleteCommentTrivia(*gap))
          return false;

        if (!macroInvocationIsSourceNeutralZeroTokenImpl(*piece.child, visiting))
          return false;

        cursor = piece.end;
      }

      std::optional<StringRef> suffix = sliceTrivia(cursor, end);
      return suffix && isWsOrCompleteCommentTrivia(*suffix);
    };

    // Function-like zero-token wrappers are admissible only in a narrow
    // forwarding domain.  The replacement list may mention formals, and after
    // substitution each used argument must itself be tiled by recorded
    // zero-token macro invocations plus trivia.  This admits wrappers such as
    // FORWARD(EMPTY) and VAR_FORWARD(EMPTY) without trying to model arbitrary
    // macro semantics.
    if (m.subkind == "func") {
      if (m.defParams.empty() || m.invArgRanges.size() < m.defParams.size())
        return finish(false);

      DenseMap<StringRef, unsigned> paramIndexByName;
      std::optional<unsigned> variadicParamIndex;
      for (unsigned i = 0, e = m.defParams.size(); i != e; ++i) {
        // Variadic formals are safe in the same proof domain as ordinary
        // formals: the replacement list may only forward the formal spelling,
        // and the corresponding invocation argument range must be wholly tiled
        // by recorded nested zero-token calls plus trivia.  `__VA_OPT__` is
        // admitted only in the erased case, where the same variadic argument
        // range proves token-empty before the payload is ignored.
        paramIndexByName.try_emplace(m.defParams[i].name, i);
        if (m.defParams[i].variadic) {
          if (variadicParamIndex)
            return finish(false);
          variadicParamIndex = i;
        }
      }

      auto argumentRangeIsNeutral = [&](unsigned argIdx) -> bool {
        const auto &range = m.invArgRanges[argIdx];
        if (!range.first || !range.second || *range.first > *range.second)
          return false;
        if (!m.invFile || m.invFile->empty())
          return false;
        return sourceRangeIsTiledByNeutralNestedMacros(
            *range.first, *range.second, *m.invFile,
            ZeroTokenRangeSurface::InvocationArgument);
      };

      std::optional<bool> variadicTailIsNeutral;
      auto proveVariadicTailNeutral = [&]() -> bool {
        if (variadicTailIsNeutral)
          return *variadicTailIsNeutral;
        if (!variadicParamIndex || *variadicParamIndex >= m.invArgRanges.size()) {
          variadicTailIsNeutral = false;
          return false;
        }
        variadicTailIsNeutral = argumentRangeIsNeutral(*variadicParamIndex);
        return *variadicTailIsNeutral;
      };

      auto parseVaOptPayloadEnd = [&](size_t openParen) -> std::optional<size_t> {
        StringRef text = replacement->directive->text;
        if (openParen >= text.size() || text[openParen] != '(')
          return std::nullopt;

        unsigned depth = 1;
        for (size_t pos = openParen + 1; pos < text.size();) {
          const char ch = text[pos];
          if (ch == '\n' || ch == '\r')
            return std::nullopt;

          if (ch == '"' || ch == '\'') {
            const char quote = ch;
            ++pos;
            bool closed = false;
            while (pos < text.size()) {
              const char litCh = text[pos++];
              if (litCh == '\n' || litCh == '\r')
                return std::nullopt;
              if (litCh == '\\') {
                if (pos >= text.size())
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
            continue;
          }

          if (pos + 1 < text.size() && ch == '/') {
            if (text[pos + 1] == '*') {
              pos += 2;
              bool closed = false;
              while (pos + 1 < text.size()) {
                if (text[pos] == '*' && text[pos + 1] == '/') {
                  pos += 2;
                  closed = true;
                  break;
                }
                ++pos;
              }
              if (!closed)
                return std::nullopt;
              continue;
            }
            if (text[pos + 1] == '/')
              return std::nullopt;
          }

          if (ch == '(') {
            ++depth;
            ++pos;
            continue;
          }
          if (ch == ')') {
            --depth;
            if (depth == 0)
              return pos + 1;
            ++pos;
            continue;
          }
          ++pos;
        }
        return std::nullopt;
      };

      SmallVector<unsigned, 4> usedParams;
      StringRef text = replacement->directive->text;

      auto replacementFragmentIsTiledByNeutralNestedMacros =
          [&](size_t begin, size_t end) -> bool {
        if (begin > end || end > text.size())
          return false;
        return sourceRangeIsTiledByNeutralNestedMacros(
            replacement->fileBase + static_cast<uint64_t>(begin),
            replacement->fileBase + static_cast<uint64_t>(end),
            replacement->directive->sitePath,
            ZeroTokenRangeSurface::DefinitionReplacementList);
      };

      auto skipReplacementTriviaUntil = [&](size_t &pos,
                                            size_t limit) -> bool {
        if (limit > text.size())
          return false;
        while (pos < limit) {
          if (stringutils::isWs(text[pos])) {
            ++pos;
            continue;
          }

          StringRef tail = text.substr(pos, limit - pos);
          if (tail.starts_with("/*")) {
            const size_t commentEnd = text.find("*/", pos + 2);
            if (commentEnd == StringRef::npos || commentEnd + 2 > limit)
              return false;
            pos = commentEnd + 2;
            continue;
          }
          if (tail.starts_with("//")) {
            const size_t commentEnd = text.find('\n', pos + 2);
            if (commentEnd != StringRef::npos && commentEnd < limit)
              return false;
            pos = limit;
            continue;
          }
          return true;
        }
        return true;
      };

      auto neutralNestedChildAt = [&](size_t pos,
                                      size_t limit) -> std::optional<size_t> {
        if (limit > text.size())
          return std::nullopt;

        const uint64_t filePos =
            replacement->fileBase + static_cast<uint64_t>(pos);
        const uint64_t fileLimit =
            replacement->fileBase + static_cast<uint64_t>(limit);

        const RefoldModel::MacroInvocation *matched = nullptr;
        for (const auto &candidate : model_.GetMacroInvocations()) {
          if (!candidate.callerMacroId || *candidate.callerMacroId != m.id)
            continue;
          if (!candidate.invFile || candidate.invFile->empty() ||
              !PathsEqual(*candidate.invFile, replacement->directive->sitePath))
            continue;
          if (!candidate.invB || !candidate.invE ||
              *candidate.invB >= *candidate.invE)
            continue;
          if (*candidate.invB != filePos || *candidate.invE > fileLimit)
            continue;

          // Two direct children cannot soundly claim the same source spelling
          // in this local replacement-list proof.  Reject duplicate ownership
          // rather than choosing one heuristically.
          if (matched)
            return std::nullopt;
          matched = &candidate;
        }

        if (!matched)
          return std::nullopt;
        if (!macroInvocationIsSourceNeutralZeroTokenImpl(*matched, visiting))
          return std::nullopt;
        return static_cast<size_t>(*matched->invE - replacement->fileBase);
      };

      std::function<bool(size_t, size_t)> replacementFragmentIsNeutral;
      replacementFragmentIsNeutral = [&](size_t begin, size_t end) -> bool {
        if (begin > end || end > text.size())
          return false;

        // A fragment made entirely of nested recorded zero-token macro
        // invocations plus trivia is neutral regardless of whether it is the
        // whole replacement list or the selected payload of __VA_OPT__.
        if (replacementFragmentIsTiledByNeutralNestedMacros(begin, end))
          return true;

        size_t pos = begin;
        while (pos < end) {
          if (!skipReplacementTriviaUntil(pos, end))
            return false;
          if (pos >= end)
            break;

          if (std::optional<size_t> childEnd = neutralNestedChildAt(pos, end)) {
            pos = *childEnd;
            continue;
          }

          if (!stringutils::isIdentStart(text[pos]))
            return false;
          const size_t identBegin = pos++;
          while (pos < end && stringutils::isIdentPart(text[pos]))
            ++pos;
          StringRef ident = text.slice(identBegin, pos);

          if (ident == "__VA_OPT__") {
            if (!skipReplacementTriviaUntil(pos, end))
              return false;
            std::optional<size_t> afterVaOpt = parseVaOptPayloadEnd(pos);
            if (!afterVaOpt || *afterVaOpt > end)
              return false;

            const size_t payloadBegin = pos + 1;
            const size_t payloadEnd = *afterVaOpt - 1;

            // __VA_OPT__ is neutral in either of two proof-bounded cases:
            //   * the variadic tail is proved token-empty, so the payload is
            //     erased and need not be inspected; or
            //   * the payload itself is proved zero-token, so exposing it also
            //     contributes no PP tokens.
            // This branch-independent rule covers both erased forwarding forms
            // and non-erased payloads such as __VA_OPT__(EMPTY), without
            // evaluating arbitrary payload semantics.
            if (!proveVariadicTailNeutral() &&
                !replacementFragmentIsNeutral(payloadBegin, payloadEnd))
              return false;

            pos = *afterVaOpt;
            continue;
          }

          // Outside __VA_OPT__, the only non-trivia replacement-list
          // identifiers admitted by the function-like wrapper proof are formal
          // parameter names.  Their invocation argument ranges are checked once
          // after the fragment walk completes.
          auto it = paramIndexByName.find(ident);
          if (it == paramIndexByName.end())
            return false;

          // Token pasting can still be source-neutral in the placemarker
          // domain.  The shared helper admits only paste chains whose operands
          // are formals with token-empty invocation arguments; any paste that
          // could synthesize real PP-token material remains outside this proof.
          if (consumeNeutralPlacemarkerPasteChain(
                  text, pos, end, it->second, paramIndexByName,
                  skipReplacementTriviaUntil, argumentRangeIsNeutral,
                  usedParams))
            continue;

          usedParams.push_back(it->second);
        }

        return true;
      };

      if (!replacementFragmentIsNeutral(replacement->replacementTextBegin,
                                        text.size()))
        return finish(false);

      llvm::sort(usedParams);
      usedParams.erase(std::unique(usedParams.begin(), usedParams.end()),
                       usedParams.end());

      for (unsigned argIdx : usedParams)
        if (!argumentRangeIsNeutral(argIdx))
          return finish(false);

      return finish(true);
    }

    if (m.subkind != "obj")
      return finish(false);

    // Collect the nested macro invocations that the producer recorded inside
    // this macro's replacement list.  These are the only non-trivia source
    // bytes a non-empty object-like wrapper is allowed to contain.
    SmallVector<ChildPiece, 8> children;
    for (const auto &candidate : model_.GetMacroInvocations()) {
      if (!candidate.callerMacroId || *candidate.callerMacroId != m.id)
        continue;

      // Nested invocations must be physically spelled in the defining directive
      // for this macro.  Otherwise they cannot tile this replacement list.
      if (!candidate.invFile || candidate.invFile->empty() ||
          !PathsEqual(*candidate.invFile, replacement->directive->sitePath))
        return finish(false);

      // Require a concrete, non-empty source callsite interval for the child.
      // Zero-token refers to the produced PP coverage, not to the source
      // spelling of the nested macro name.
      if (!candidate.invB || !candidate.invE ||
          *candidate.invB >= *candidate.invE)
        return finish(false);

      // The child callsite must lie wholly inside the replacement-list source
      // interval.  A partial overlap would mean the wrapper contains source
      // bytes that this proof has not accounted for.
      if (*candidate.invB < replacement->fileBegin ||
          replacement->fileEnd < *candidate.invE)
        return finish(false);

      children.push_back({*candidate.invB, *candidate.invE, &candidate});
    }

    // A non-empty object-like wrapper with no recorded children contains
    // unaccounted source text, so it is not source-neutral under this proof.
    if (children.empty())
      return finish(false);

    sortChildren(children);

    uint64_t cursor = replacement->fileBegin;
    for (const ChildPiece &piece : children) {
      // Overlapping or out-of-order child intervals would make the tiling
      // ambiguous.  Reject rather than trying to normalize it heuristically.
      if (piece.begin < cursor)
        return finish(false);

      // Every byte before the next child invocation must be pure trivia.  Any
      // identifier, literal, operator, or _Pragma-like spelling here would be
      // source-bearing replacement-list material.
      const size_t gapBegin =
          static_cast<size_t>(cursor - replacement->fileBase);
      const size_t gapEnd =
          static_cast<size_t>(piece.begin - replacement->fileBase);
      StringRef gap = replacement->directive->text.slice(gapBegin, gapEnd);
      if (!isWsOrCompleteCommentTrivia(gap))
        return finish(false);

      // The child itself must satisfy the same zero-token source-neutral proof.
      // This is what admits WRAP_EMPTY -> EMPTY, while still rejecting wrappers
      // that eventually expand to real replacement-list text.
      if (!macroInvocationIsSourceNeutralZeroTokenImpl(*piece.child, visiting))
        return finish(false);

      cursor = piece.end;
    }

    // After the final child, the replacement-list suffix must also be trivia.
    // This completes the tiling proof for the wrapper's entire replacement
    // list.
    const size_t suffixBegin =
        static_cast<size_t>(cursor - replacement->fileBase);
    StringRef suffix = replacement->directive->text.substr(suffixBegin);
    if (!isWsOrCompleteCommentTrivia(suffix))
      return finish(false);

    return finish(true);
  };

  auto macroInvocationIsSourceNeutralZeroToken =
      [&](const RefoldModel::MacroInvocation &m) -> bool {
    DenseSet<uint64_t> visiting;
    return macroInvocationIsSourceNeutralZeroTokenImpl(m, visiting);
  };

  // Return true iff the macro invocation is a complete zero-token TU callsite
  // inside the candidate replacement envelope.  Partial overlaps remain
  // non-refoldable here because deleting only part of a macro call would not be
  // a proved source closure.
  auto macroInvocationIsConsumableZeroTokenGap =
      [&](const RefoldModel::MacroInvocation &m, uint64_t begin,
          uint64_t end) -> bool {
    if (m.invFile && !m.invFile->empty() && !PathsEqual(*m.invFile, tuPath))
      return false;
    if (!m.invB || !m.invE || *m.invB >= *m.invE)
      return false;
    if (*m.invB < begin || end < *m.invE)
      return false;
    return macroInvocationIsSourceNeutralZeroToken(m);
  };

  struct MacroDirectiveSourceInterval {
    uint64_t begin = 0;
    uint64_t end = 0;
  };

  // Recover the full physical source interval for a TU-spelled macro-state
  // directive.  MacroDirective::siteB is anchored at the macro name rather than
  // at '#', so use the recorded directive spelling to translate the name offset
  // back to the directive's real source begin.
  auto macroDirectiveFullSourceInterval =
      [&](const RefoldModel::MacroDirective &directive)
          -> std::optional<MacroDirectiveSourceInterval> {
    if (!PathsEqual(directive.sitePath, tuPath) || directive.ownerIncludeId)
      return std::nullopt;
    if (directive.subkind != "#define" && directive.subkind != "#undef")
      return std::nullopt;

    StringRef text = directive.text;
    if (text.empty())
      return std::nullopt;

    size_t pos = 0;
    stringutils::skipNonNewlineWs(text, pos);
    if (pos >= text.size() || text[pos] != '#')
      return std::nullopt;
    ++pos;
    stringutils::skipNonNewlineWs(text, pos);

    // Match the recorded directive kind exactly; accepting an identifier prefix
    // here would make the later byte-coordinate reconstruction unsound.
    StringRef keyword = directive.subkind.drop_front();
    if (!text.substr(pos).starts_with(keyword))
      return std::nullopt;
    pos += keyword.size();
    if (pos < text.size() && stringutils::isIdentPart(text[pos]))
      return std::nullopt;
    stringutils::skipNonNewlineWs(text, pos);

    // The next token is the macro name.  Its text offset is the bridge between
    // directive-text coordinates and source-file byte coordinates because the
    // producer records siteB at this token.
    const size_t nameTextBegin = pos;
    if (pos >= text.size() || !stringutils::isIdentStart(text[pos]))
      return std::nullopt;
    ++pos;
    while (pos < text.size() && stringutils::isIdentPart(text[pos]))
      ++pos;

    if (directive.siteB < nameTextBegin)
      return std::nullopt;

    const uint64_t fileBegin = directive.siteB - nameTextBegin;
    const uint64_t fileEnd = fileBegin + text.size();
    if (fileBegin >= fileEnd || fileEnd > tuBytes.size())
      return std::nullopt;

    // Require an exact byte match so this proof consumes the directive that the
    // producer recorded, not a synthetic interval inferred from stale anchors.
    if (tuBytes.slice(fileBegin, fileEnd) != text)
      return std::nullopt;

    return MacroDirectiveSourceInterval{fileBegin, fileEnd};
  };

  auto macroDirectiveIsConsumableStateGap =
      [&](const RefoldModel::MacroDirective &directive, uint64_t begin,
          uint64_t end) -> std::optional<MacroDirectiveSourceInterval> {
    std::optional<MacroDirectiveSourceInterval> interval =
        macroDirectiveFullSourceInterval(directive);
    if (!interval)
      return std::nullopt;
    if (interval->begin < begin || end < interval->end)
      return std::nullopt;
    return interval;
  };

  // Return a TU-spelled pragma that is wholly contained in the physical source
  // line being scanned, if the refold map recorded one there.
  //
  // Unlike #define/#undef, pragmas are deliberately not treated as consumable
  // source-neutral artifacts here.  A pragma may mutate compiler state without
  // producing PP tokens, and the refold map currently records only its spelling
  // and source anchors, not the state region needed to prove deletion safe.
  // This helper is therefore diagnostic-only: it lets the rejection log name the
  // exact pragma that blocked the closure instead of reporting an opaque
  // non-trivia gap.
  auto findTUPragmaOnSourceLine = [&](uint64_t lineBegin, uint64_t lineEnd)
      -> const RefoldModel::PragmaDirective * {
    for (const auto &pragma : model_.GetPragmas()) {
      if (!PathsEqual(pragma.sitePath, tuPath))
        continue;
      if (lineBegin <= pragma.siteB && pragma.siteE <= lineEnd &&
          pragma.siteB < pragma.siteE)
        return &pragma;
    }
    return nullptr;
  };

  // Return a concrete diagnostic when a TU gap contains a recorded pragma that
  // blocks closure.  This is checked at the outer rejection site as well as in
  // the line scanner, because a pragma-only gap has no consumable recorded
  // pieces; without this fast path the scanner is never entered and the user
  // only sees a generic "non-trivia gap" rejection.
  auto nonConsumableTUPragmaGapReason = [&](uint64_t gapBegin,
                                            uint64_t gapEnd)
      -> std::optional<std::string> {
    for (const auto &pragma : model_.GetPragmas()) {
      if (!PathsEqual(pragma.sitePath, tuPath))
        continue;
      if (gapBegin <= pragma.siteB && pragma.siteE <= gapEnd &&
          pragma.siteB < pragma.siteE) {
        return llvm::formatv("source gap [{0},{1}) contains non-consumable "
                             "TU pragma id={2} site=[{3},{4}) text='{5}'",
                             gapBegin, gapEnd, pragma.id, pragma.siteB,
                             pragma.siteE,
                             stringutils::showWsWithClip(pragma.text, 120))
            .str();
      }
    }
    return std::nullopt;
  };

  // Return true iff a recorded TU conditional group may be preserved as
  // inert inter-include source in a pure include-closure replacement.
  //
  // This is the preservation-side counterpart to the mixed-envelope conditional
  // consumption proof below.  The static textual parser intentionally accepts
  // only a tiny literal grammar (`#if 0`, `#if 1`, ...), because it has no
  // preprocessor state.  For source forms such as `#ifdef ENABLE_GAP`, the
  // refold map already gives us the deterministic proof object: a complete
  // conditional-group byte interval and the PP spans materialized by each arm.
  //
  // Pure include closure does not delete these bytes.  It projects the touched
  // include material into B and carries source-neutral inter-include control
  // structure forward verbatim.  Therefore a recorded conditional group is
  // preservable when it is complete, TU-spelled, wholly inside the gap, and no
  // arm materialized PP tokens in A.  Active source-bearing artifacts inside the
  // group remain outside this proof; they require their own preservation/repair
  // model rather than being hidden inside an opaque conditional gap.
  auto conditionalGroupIsPreservableIncludeClosureGap =
      [&](const RefoldModel::CondGroup &group, uint64_t gapBegin,
          uint64_t gapEnd) -> bool {
    auto groupBelongs = [&](const RefoldModel::CondGroup &candidate) {
      return PathsEqual(candidate.file, tuPath) && !candidate.parentIncludeId;
    };
    auto includeBelongs = [&](const RefoldModel::IncludeItem &inc) {
      return PathsEqual(inc.sitePath, tuPath);
    };
    auto includeIsNeutral = [&](const RefoldModel::IncludeItem &inc) {
      return (!inc.cover.IsValid() && !includeHasRecordedSideEffects(inc)) ||
             includeIsPreservableConditionalStateInclude(inc);
    };
    auto directiveBelongs = [&](const RefoldModel::MacroDirective &directive) {
      return PathsEqual(directive.sitePath, tuPath);
    };
    auto pragmaBelongs = [&](const RefoldModel::PragmaDirective &pragma) {
      return PathsEqual(pragma.sitePath, tuPath);
    };
    auto macroBelongs = [&](const RefoldModel::MacroInvocation &macro) {
      return macro.invFile && PathsEqual(*macro.invFile, tuPath);
    };
    auto macroIsNeutral = [&](const RefoldModel::MacroInvocation &macro) {
      return macroInvocationIsSourceNeutralZeroToken(macro);
    };

    NeutralConditionalIslandPolicy policy{
        tuBytes,
        /*requireGroupBeginAtLineStart=*/true,
        NeutralConditionalArmSpanMode::AllArms,
        groupBelongs,
        includeBelongs,
        includeIsNeutral,
        directiveBelongs,
        pragmaBelongs,
        macroBelongs,
        macroIsNeutral};
    return conditionalGroupIsNeutralIsland(model_, group, gapBegin, gapEnd,
                                           policy);
  };

  // Return true iff a pure include-closure inter-include gap can be preserved
  // verbatim by tiling it with lexical trivia and complete recorded zero-token
  // conditional groups.
  //
  // This extends `isPreservableIncludeClosureGapTrivia()` without weakening its
  // no-heuristics contract.  The directive spelling inside each conditional
  // group can be macro-dependent (`#ifdef`, `#ifndef`, `#if defined(...)`, ...)
  // because the producer has already recorded the complete group boundaries and
  // which arms materialized A tokens for this preprocessing run.  We preserve
  // the original bytes rather than consuming them, so this helper is used only
  // by the pure include-closure gap path.
  auto gapIsPreservableRecordedConditionalIncludeClosure =
      [&](uint64_t gapBegin, uint64_t gapEnd) -> bool {
    if (gapBegin >= gapEnd || gapEnd > tuBytes.size())
      return false;

    struct ConditionalPiece {
      uint64_t begin;
      uint64_t end;
      uint64_t id;
    };

    SmallVector<ConditionalPiece, 8> pieces;
    for (const auto &group : model_.GetConds())
      if (conditionalGroupIsPreservableIncludeClosureGap(group, gapBegin,
                                                        gapEnd))
        pieces.push_back({group.groupB, group.groupE, group.id});

    if (pieces.empty())
      return false;

    if (!proveOwnerProofGap(
            pieces, gapBegin, gapEnd,
            [](const ConditionalPiece &, const ConditionalPiece &) {
              return false;
            },
            [](const ConditionalPiece &outer, const ConditionalPiece &piece) {
              if (outer.begin <= piece.begin && piece.end <= outer.end) {
                // Nested conditional groups are represented by the producer as
                // separate records.  Once the outer complete group is
                // preserved verbatim, nested conditional records inside its
                // bytes are not independent gap pieces.
                return true;
              }
              return false;
            },
            [&](uint64_t begin, uint64_t end) {
              return isWsOrCompleteCommentTrivia(tuBytes.slice(begin, end));
            },
            [](const ConditionalPiece &) {}))
      return false;

    trace("fallback",
          "TU include-closure preserving recorded conditional gap "
          "source=[{0},{1}) conditionalGroups={2}",
          gapBegin, gapEnd, pieces.size());
    return true;
  };

  // Return true iff a complete TU conditional group is source-neutral
  // inside a mixed TU/include source envelope.
  //
  // This is the recorded-structure proof for conditional islands that produce
  // no A-side tokens.  Unlike the literal textual #if/#endif scanner, it does
  // not try to evaluate the condition.  The producer has already recorded the
  // selected arm and the arm token spans; this proof only checks that replaying
  // the complete group verbatim is a neutral source gap.  Macro invocations in
  // conditional-control lines are therefore allowed because they remain inside
  // the preserved group.  Recorded artifacts in arm bodies still fail closed:
  // those may carry source tokens, macro state, include effects, or pragmas that
  // require their own proof.
  auto conditionalGroupIsConsumableZeroTokenGap =
      [&](const RefoldModel::CondGroup &group, uint64_t begin,
          uint64_t end) -> bool {
    auto groupBelongs = [&](const RefoldModel::CondGroup &candidate) {
      return PathsEqual(candidate.file, tuPath) && !candidate.parentIncludeId;
    };
    auto includeBelongs = [&](const RefoldModel::IncludeItem &inc) {
      return PathsEqual(inc.sitePath, tuPath);
    };
    auto includeIsNeutral = [&](const RefoldModel::IncludeItem &inc) {
      return (!inc.cover.IsValid() && !includeHasRecordedSideEffects(inc)) ||
             includeIsPreservableConditionalStateInclude(inc);
    };
    auto directiveBelongs = [&](const RefoldModel::MacroDirective &directive) {
      return PathsEqual(directive.sitePath, tuPath);
    };
    auto pragmaBelongs = [&](const RefoldModel::PragmaDirective &pragma) {
      return PathsEqual(pragma.sitePath, tuPath);
    };
    auto macroBelongs = [&](const RefoldModel::MacroInvocation &macro) {
      return macro.invFile && PathsEqual(*macro.invFile, tuPath);
    };
    auto macroIsNeutral = [&](const RefoldModel::MacroInvocation &macro) {
      return macroInvocationIsSourceNeutralZeroToken(macro);
    };

    NeutralConditionalIslandPolicy policy{
        tuBytes,
        /*requireGroupBeginAtLineStart=*/false,
        NeutralConditionalArmSpanMode::SelectedArmsOnly,
        groupBelongs,
        includeBelongs,
        includeIsNeutral,
        directiveBelongs,
        pragmaBelongs,
        macroBelongs,
        macroIsNeutral};
    return conditionalGroupIsNeutralIsland(model_, group, begin, end, policy);
  };

  // Prove that a source gap inside a replacement envelope consists only of
  // lexical trivia plus complete source-neutral artifacts: zero-token include
  // trees, source-neutral zero-token macro invocation trees, and TU-spelled
  // macro-state directives whose later liveness obligations are handled by the
  // macro-state repair pass.
  //
  // These artifacts have no PP tokens, so token overlap alone cannot make them
  // part of the touched hunk.  If they lie physically between required source
  // pieces of a proved mixed closure, however, they are inside the source
  // interval being replaced and may be consumed with the B-side material.
  //
  // The conditional-control proof is deliberately mode-sensitive.  Mixed
  // whole-envelope closures may consume complete TU conditional groups because
  // their surrounding source interval is itself replaced.  Pure include-closure
  // inter-include gaps, however, project include material and preserve inert
  // source gaps; they must not delete an otherwise-preservable empty #if/#endif
  // island just because it has no PP tokens.
  auto gapIsConsumableZeroTokenSourceClosure =
      [&](uint64_t gapBegin, uint64_t gapEnd,
          bool allowTUConditionalControl) -> bool {
    if (gapBegin >= gapEnd || gapEnd > tuBytes.size())
      return false;

    struct GapPiece {
      uint64_t begin;
      uint64_t end;
      uint64_t id;
      StringRef kind;
    };

    SmallVector<GapPiece, 8> pieces;
    for (const auto &inc : model_.GetIncludes()) {
      if (!PathsEqual(inc.sitePath, tuPath) || includeIsTouched(inc))
        continue;
      if (inc.cover.IsValid() || includeHasRecordedSideEffects(inc))
        continue;
      if (gapBegin <= inc.siteB && inc.siteE <= gapEnd &&
          inc.siteB < inc.siteE)
        pieces.push_back({inc.siteB, inc.siteE, inc.id, "include"});
    }

    for (const auto &m : model_.GetMacroInvocations())
      if (macroInvocationIsConsumableZeroTokenGap(m, gapBegin, gapEnd))
        pieces.push_back({*m.invB, *m.invE, m.id, "macro"});

    for (const auto &directive : model_.GetMacroDirectives())
      if (std::optional<MacroDirectiveSourceInterval> interval =
              macroDirectiveIsConsumableStateGap(directive, gapBegin, gapEnd))
        pieces.push_back({interval->begin, interval->end, directive.id,
                          "macro-directive"});

    if (allowTUConditionalControl)
      for (const auto &group : model_.GetConds())
        if (conditionalGroupIsConsumableZeroTokenGap(group, gapBegin, gapEnd))
          pieces.push_back({group.groupB, group.groupE, group.id,
                            "conditional"});

    uint64_t scanCursor = gapBegin;
    uint64_t includeCount = 0;
    uint64_t macroCount = 0;
    uint64_t macroDirectiveCount = 0;
    uint64_t conditionalGroupCount = 0;
    uint64_t conditionalDirectiveCount = 0;
    unsigned conditionalDepth = 0;

    // Scan source bytes that are not covered by recorded zero-token artifacts.
    // These interstitial bytes may contain ordinary lexical trivia and literal
    // conditional-control directives.  The conditional depth is carried across
    // consumed artifacts, which is what admits shapes such as:
    //
    //   #if 1
    //   #define GAP_VALUE 99
    //   #endif
    //
    // when the #define line is itself a proved consumable macro-state artifact.
    bool atLineStart = stringutils::beginsLineAfterWs(tuBytes, gapBegin);
    auto scanNeutralControlTrivia = [&](uint64_t begin, uint64_t limit) -> bool {
      scanCursor = begin;
      while (scanCursor < limit) {
        char ch = tuBytes[scanCursor];
        if (stringutils::isWs(ch)) {
          atLineStart = ch == '\n' || ch == '\r';
          ++scanCursor;
          continue;
        }

        if (ch == '/') {
          const uint64_t before = scanCursor;
          StringRef rest = tuBytes.drop_front(scanCursor);
          if (rest.starts_with("/*")) {
            scanCursor += 2;
            bool closed = false;
            while (scanCursor + 1 < limit) {
              if (tuBytes[scanCursor] == '*' && tuBytes[scanCursor + 1] == '/') {
                scanCursor += 2;
                closed = true;
                break;
              }
              ++scanCursor;
            }
            if (!closed)
              return false;
            StringRef skipped = tuBytes.slice(before, scanCursor);
            atLineStart = skipped.ends_with("\n") || skipped.ends_with("\r");
            continue;
          }

          if (rest.starts_with("//")) {
            scanCursor += 2;
            while (scanCursor < limit && tuBytes[scanCursor] != '\n' &&
                   tuBytes[scanCursor] != '\r')
              ++scanCursor;

            // A line comment may end at the end of the whole gap, but it may
            // not run into a following recorded artifact.  In that case the
            // artifact's spelling would be inside the comment, contradicting the
            // proof that it is an independently recorded source artifact.
            if (scanCursor >= limit)
              return limit == gapEnd;

            ++scanCursor;
            atLineStart = true;
            continue;
          }
        }

        // Literal conditional-control directives are source-neutral only at a
        // physical directive boundary.  Any other directive or token spelling is
        // real source that this closure has not proved safe to erase.
        if (ch != '#' || !atLineStart)
          return false;

        uint64_t lineEnd = scanCursor;
        while (lineEnd < limit && tuBytes[lineEnd] != '\n')
          ++lineEnd;
        if (lineEnd < limit)
          ++lineEnd;

        if (!allowTUConditionalControl ||
            !parseLiteralEmptyConditionalDirectiveLine(
                tuBytes.slice(scanCursor, lineEnd), conditionalDepth)) {
          if (const RefoldModel::PragmaDirective *pragma =
                  findTUPragmaOnSourceLine(scanCursor, lineEnd)) {
            trace("fallback",
                  "TU/include closure rejected: source gap [{0},{1}) contains "
                  "non-consumable TU pragma id={2} site=[{3},{4}) text='{5}'",
                  gapBegin, gapEnd, pragma->id, pragma->siteB, pragma->siteE,
                  stringutils::showWsWithClip(pragma->text, 120));
          }
          return false;
        }

        ++conditionalDirectiveCount;
        scanCursor = lineEnd;
        atLineStart = true;
      }
      return scanCursor == limit;
    };

    if (pieces.empty())
      return false;

    if (!proveOwnerProofGap(
            pieces, gapBegin, gapEnd,
            [](const GapPiece &lhs, const GapPiece &rhs) {
              return lhs.kind < rhs.kind;
            },
            [](const GapPiece &outer, const GapPiece &piece) {
              if (outer.kind == "macro" && piece.kind == "macro") {
                // A function-like zero-token macro such as FORWARD(EMPTY)
                // records both the outer callsite and the nested EMPTY callsite
                // in the same TU gap. Once the outer macro has been proved
                // source-neutral, the nested callsite is part of that proof,
                // not a second independent gap piece.
                return true;
              }

              if (outer.kind == "conditional" &&
                  piece.kind == "conditional") {
                // A complete inactive conditional group may contain nested
                // conditional groups that the producer also recorded. The outer
                // zero-token group proof owns all bytes in the inactive island,
                // so nested conditional records are not separate top-level gap
                // pieces.
                return true;
              }

              // Any other overlap would make the source tiling ambiguous:
              // includes, directives, and unrelated macro calls must remain
              // disjoint at this level of the closure proof.
              return false;
            },
            [&](uint64_t begin, uint64_t end) {
              return scanNeutralControlTrivia(begin, end);
            },
            [&](const GapPiece &piece) {
              // The artifact itself has already been independently proved
              // zero-token and complete. Consume it as one opaque
              // source-neutral piece, then continue scanning surrounding
              // control/trivia bytes at the same conditional depth.
              StringRef pieceBytes = tuBytes.slice(piece.begin, piece.end);
              atLineStart =
                  pieceBytes.ends_with("\n") || pieceBytes.ends_with("\r");
              if (piece.kind == "include")
                ++includeCount;
              else if (piece.kind == "macro")
                ++macroCount;
              else if (piece.kind == "macro-directive")
                ++macroDirectiveCount;
              else
                ++conditionalGroupCount;
            }) ||
        conditionalDepth != 0)
      return false;

    trace("fallback",
          "TU/include closure consuming zero-token source gap "
          "source=[{0},{1}) includes={2} macros={3} macroDirectives={4} "
          "conditionalGroups={5} conditionalDirectives={6}",
          gapBegin, gapEnd, includeCount, macroCount, macroDirectiveCount,
          conditionalGroupCount, conditionalDirectiveCount);
    return true;
  };

  struct TUPreservedGapPiece {
    enum class Kind { ZeroTokenMacroInvocation, ZeroTokenConditionalGroup };

    Kind kind = Kind::ZeroTokenMacroInvocation;
    uint64_t begin = 0;
    uint64_t end = 0;
    uint64_t id = 0;
  };

  auto preservedTUGapPieceText = [&](const TUPreservedGapPiece &piece) {
    if (piece.end <= tuBytes.size() && piece.begin <= piece.end)
      return tuBytes.slice(piece.begin, piece.end).str();
    return std::string();
  };

  auto collectPreservableZeroTokenGapPieces =
      [&](uint64_t gapBegin, uint64_t gapEnd,
          SmallVectorImpl<TUPreservedGapPiece> &out) -> bool {
    if (gapBegin >= gapEnd || gapEnd > tuBytes.size())
      return false;

    struct GapPiece {
      uint64_t begin;
      uint64_t end;
      uint64_t id;
      TUPreservedGapPiece::Kind kind;
    };

    SmallVector<GapPiece, 8> pieces;
    for (const auto &m : model_.GetMacroInvocations())
      if (macroInvocationIsConsumableZeroTokenGap(m, gapBegin, gapEnd))
        pieces.push_back({*m.invB, *m.invE, m.id,
                          TUPreservedGapPiece::Kind::
                              ZeroTokenMacroInvocation});

    for (const auto &group : model_.GetConds())
      if (conditionalGroupIsConsumableZeroTokenGap(group, gapBegin, gapEnd))
        pieces.push_back({group.groupB, group.groupE, group.id,
                          TUPreservedGapPiece::Kind::
                              ZeroTokenConditionalGroup});

    if (pieces.empty())
      return false;

    SmallVector<TUPreservedGapPiece, 8> accepted;
    if (!proveOwnerProofGap(
            pieces, gapBegin, gapEnd,
            [](const GapPiece &lhs, const GapPiece &rhs) {
              return lhs.kind < rhs.kind;
            },
            [](const GapPiece &outer, const GapPiece &piece) {
              if (outer.begin <= piece.begin && piece.end <= outer.end) {
                if (outer.kind == TUPreservedGapPiece::Kind::
                                      ZeroTokenMacroInvocation &&
                    piece.kind == TUPreservedGapPiece::Kind::
                                      ZeroTokenMacroInvocation) {
                  // Nested zero-token macro invocations are already part of
                  // the outer invocation's source-neutral proof. Preserve only
                  // the outer callsite text so the refolded source does not
                  // duplicate the same gap structure.
                  return true;
                }

                if (outer.kind == TUPreservedGapPiece::Kind::
                                      ZeroTokenConditionalGroup) {
                  // A preserved complete conditional group owns everything in
                  // its source island, including control-line macro uses and
                  // nested inactive conditional records.  Those inner records
                  // are not independently emitted as gap pieces.
                  return true;
                }
              }
              return false;
            },
            [&](uint64_t begin, uint64_t end) {
              return isWsOrCompleteCommentTrivia(tuBytes.slice(begin, end));
            },
            [&](const GapPiece &piece) {
              accepted.push_back({piece.kind, piece.begin, piece.end,
                                  piece.id});
            }))
      return false;

    out.append(accepted.begin(), accepted.end());
    trace("fallback",
          "TU/include closure preserving zero-token source gap "
          "source=[{0},{1}) pieces={2}",
          gapBegin, gapEnd, accepted.size());
    return true;
  };

  std::string mixedPreservedConditionalControlTrivia;
  SmallVector<TUPreservedGapPiece, 8> mixedPreservedZeroTokenGapPieces;
  SmallVector<std::pair<uint64_t, uint64_t>, 4>
      mixedPreservedSourceLineDirectiveGapPieces;
  std::optional<SourceLineDirectiveGapResume> mixedSourceLineDirectiveResume;

  auto lineDirectiveStartsAtPrefix = [&](uint64_t pos) -> bool {
    if (pos >= tuBytes.size())
      return true;
    if (stringutils::isBOL(tuBytes, static_cast<size_t>(pos)))
      return true;

    size_t lineStart = static_cast<size_t>(pos);
    while (lineStart > 0 && tuBytes[lineStart - 1] != '\n')
      --lineStart;
    return stringutils::isIndentOnly(tuBytes, lineStart,
                                     static_cast<size_t>(pos));
  };

  auto canStartLineDirectiveWithOptionalLeadingNewline =
      [&](uint64_t pos) -> bool {
    if (lineDirectiveStartsAtPrefix(pos))
      return true;

    // A delete-only closure may need to synthesize a leading newline before the
    // replacement-local #line directive. Reject the only local case where that
    // newline would be swallowed as a line splice instead of starting a fresh
    // preprocessing directive.
    return pos == 0 || tuBytes[pos - 1] != '\\';
  };

  // Prove a mixed TU/include closure.
  //
  // This handles a single replacement hunk whose A-side tokens are split
  // between ordinary TU spelling and the complete expansion of a contiguous
  // top-level include run, for example:
  //
  //   int arr[] = { 1,
  //   #include "two.inc"
  //   };
  //
  // rewritten from preprocessed tokens:
  //
  //   { 1, 2 }  ->  { 3 }
  //
  // Replacing only the TU-owned bytes for "1," is unsound: the include
  // directive would remain in the source and replay its old "2" token.  The
  // only source realization we accept here is therefore one TU byte interval
  // that spans all TU tokens consumed by the hunk and all touched include
  // directive sites.
  if (mixedTUIncludeClosure) {
    materialBeginA = closureBegin;
    materialEndA = closureEnd;

    // Collect the source intervals that must be consumed by the closure:
    //   * every touched include directive site, and
    //   * every TU-owned token in the widened A token interval.
    //
    // Later checks prove these pieces merge into one safe TU replacement span
    // with only whitespace between pieces and no unrelated artifacts absorbed.
    SmallVector<std::pair<uint64_t, uint64_t>, 16> sourcePieces;
    sourcePieces.reserve(touched.size() + (closureEnd - closureBegin));
    for (const RefoldModel::IncludeItem *inc : touched)
      sourcePieces.push_back({inc->siteB, inc->siteE});

    bool sawTUToken = false;
    const auto &tokmapByPP = model_.GetTokmapByPP();
    for (uint64_t pp = closureBegin; pp < closureEnd; ++pp) {
      auto it = tokmapByPP.find(pp);
      if (it == tokmapByPP.end())
        continue;

      const auto &entry = it->second;
      if (PathsEqual(entry.file, tuPath)) {
        sourcePieces.push_back({entry.b, entry.e});
        sawTUToken = true;
        continue;
      }

      // Any non-TU token in the closure must be explained by one of the touched
      // include covers.  Otherwise this is not a TU/include closure; it would
      // be a mixed-owner edit involving some third artifact that this proof
      // does not know how to realize soundly.
      if (!tokenCoveredByTouchedInclude(pp)) {
        trace("fallback",
              "TU/include closure rejected: A token {0} maps to file={1} "
              "outside the touched include run A=[{2},{3})",
              pp, entry.file, coverBegin, coverEnd);
        return std::nullopt;
      }
    }

    auto ppSpanInsideMaterial = [&](const RefoldModel::PPSpan &span) -> bool {
      if (!span.IsValid() || span.begin >= span.end)
        return true;
      return materialBeginA <= span.begin && span.end <= materialEndA;
    };

    auto armIsSameOrNestedUnder = [&](const RefoldModel::ArmRef &candidate,
                                      uint64_t ancestorArmId) -> bool {
      if (!candidate.group || !candidate.arm)
        return false;
      if (candidate.arm->id == ancestorArmId)
        return true;

      // `FindArmRefAtPP()` intentionally returns the innermost selected arm for
      // a PP token.  When deciding whether an outer selected arm is completely
      // consumed, tokens owned by nested selected arms still belong to the
      // outer arm's effective material.  Walk parent-arm links from the
      // innermost group back toward the TU and accept any descendant of the
      // queried arm.
      const RefoldModel::CondGroup *group = candidate.group;
      while (group && group->parentArmId) {
        if (*group->parentArmId == ancestorArmId)
          return true;
        std::optional<RefoldModel::ArmRef> parent =
            model_.GetArmRefById(*group->parentArmId);
        if (!parent || !parent->group)
          break;
        group = parent->group;
      }
      return false;
    };

    auto selectedArmEffectiveMaterialInside =
        [&](const RefoldModel::CondArm &arm) -> bool {
      bool sawEffectiveMaterial = false;
      for (uint64_t pp = 0; pp < aToks_.size(); ++pp) {
        std::optional<RefoldModel::ArmRef> owner = model_.FindArmRefAtPP(pp);
        if (!owner || !armIsSameOrNestedUnder(*owner, arm.id))
          continue;

        sawEffectiveMaterial = true;
        if (pp < materialBeginA || pp >= materialEndA)
          return false;
      }

      // Prefer the effective ownership proof when it found material.  For older
      // maps that cannot answer PP ownership, fall back to the recorded direct
      // span so existing exact whole-arm closures keep their previous domain.
      if (sawEffectiveMaterial)
        return true;
      return arm.span && arm.span->IsValid() &&
             arm.span->begin < arm.span->end && ppSpanInsideMaterial(*arm.span);
    };

    auto macroInvocationMaterialIsConsumed =
        [&](const RefoldModel::MacroInvocation &m,
            std::optional<uint64_t> sourceBeginBound = std::nullopt,
            std::optional<uint64_t> sourceEndBound = std::nullopt) -> bool {
      if (!m.invFile || m.invFile->empty() || !PathsEqual(*m.invFile, tuPath))
        return false;
      if (!m.invB || !m.invE || *m.invB >= *m.invE || *m.invE > tuBytes.size())
        return false;
      if (sourceBeginBound && *m.invB < *sourceBeginBound)
        return false;
      if (sourceEndBound && *sourceEndBound < *m.invE)
        return false;

      bool sawMaterializedSpan = false;
      auto checkSpan = [&](const RefoldModel::PPSpan &span) -> bool {
        if (!span.IsValid() || span.begin >= span.end)
          return true;
        sawMaterializedSpan = true;
        return ppSpanInsideMaterial(span);
      };

      // A source-bearing macro call may be consumed by this replacement only
      // when every PP span the producer attributes to that invocation is wholly
      // inside the A-side material being replaced. Then the callsite is not an
      // unrelated artifact: it is one of the source pieces whose produced tokens
      // are removed by the hunk. Any span escaping the material range means
      // deleting the call would also delete surviving PP tokens, so reject.
      for (const auto &span : m.spans)
        if (!checkSpan(span))
          return false;
      for (const auto &span : m.bodySpans)
        if (!checkSpan(span))
          return false;
      for (const auto &span : m.argSpans)
        if (!checkSpan(span))
          return false;
      for (const auto &span : m.stringifySpans)
        if (!checkSpan(span))
          return false;
      for (const auto &span : m.pasteSpans)
        if (!checkSpan(span))
          return false;

      return sawMaterializedSpan;
    };

    // If the consumed A-token material came from inside a TU conditional arm,
    // consume the complete conditional group as a required source piece.  The
    // byte-level gap proof can already consume neutral conditional-control
    // islands whose bodies produce no tokens, but it cannot balance an opening
    // #if in one inter-piece gap against a closing #endif after a real source
    // piece.  A selected arm whose complete PP span is inside the replacement
    // material is different: its directive wrapper is part of the same source
    // interval whose produced tokens are being replaced, so the whole group must
    // be considered a required piece of the closure rather than trivia around
    // neighboring token pieces.
    for (const auto &group : model_.GetConds()) {
      if (!PathsEqual(group.file, tuPath) || group.parentIncludeId)
        continue;

      bool consumesSelectedArm = false;
      for (const RefoldModel::CondArm &arm : group.arms) {
        if (!arm.selected)
          continue;
        if (selectedArmEffectiveMaterialInside(arm)) {
          consumesSelectedArm = true;
          break;
        }
      }

      if (!consumesSelectedArm)
        continue;
      if (group.groupB >= group.groupE || group.groupE > tuBytes.size()) {
        trace("fallback",
              "TU/include closure rejected: invalid consumed conditional "
              "group id={0} source=[{1},{2}) fileLen={3}",
              group.id, group.groupB, group.groupE, tuBytes.size());
        return std::nullopt;
      }

      sourcePieces.push_back({group.groupB, group.groupE});
    }

    // Collapse any complete source-bearing macro invocation whose produced PP
    // material is wholly replaced by this hunk into one required source piece.
    // Token-level mappings inside macro expansions often point only at the
    // macro name or argument bytes; if we kept only those token pieces, the
    // verifier would later see punctuation from the callsite, such as the
    // parentheses in KEEP(2), as unexplained interstitial source.  The complete
    // macro callsite is the actual source artifact whose material is consumed,
    // so prove it once and tile the envelope with that whole interval.
    for (const auto &m : model_.GetMacroInvocations())
      if (macroInvocationMaterialIsConsumed(m))
        sourcePieces.push_back({*m.invB, *m.invE});

    // The mixed proof is only for hunks that actually consume ordinary TU
    // spelling.  If no TU token was seen, the pure include-closure path is the
    // appropriate proof shape instead.
    if (!sawTUToken) {
      trace("fallback",
            "TU/include closure rejected: mixed closure A=[{0},{1}) has no "
            "TU-owned source token",
            closureBegin, closureEnd);
      return std::nullopt;
    }

    // Normalize the collected source pieces so the rest of the proof can reason
    // about the single candidate replacement envelope.
    llvm::sort(sourcePieces, [](const auto &lhs, const auto &rhs) {
      if (lhs.first != rhs.first)
        return lhs.first < rhs.first;
      return lhs.second < rhs.second;
    });

    SmallVector<std::pair<uint64_t, uint64_t>, 16> mergedPieces;
    for (const auto &piece : sourcePieces) {
      if (piece.first >= piece.second || piece.second > tuBytes.size()) {
        trace("fallback",
              "TU/include closure rejected: invalid source piece [{0},{1}) "
              "fileLen={2}",
              piece.first, piece.second, tuBytes.size());
        return std::nullopt;
      }
      if (mergedPieces.empty() || piece.first > mergedPieces.back().second) {
        mergedPieces.push_back(piece);
        continue;
      }
      mergedPieces.back().second =
          std::max(mergedPieces.back().second, piece.second);
    }

    // The replacement will cover the whole byte envelope from the first
    // required source piece through the last. `mergedPieces` is already sorted,
    // non-empty, and coalesced by source interval.
    if (mergedPieces.empty()) {
      trace("fallback",
            "TU/include closure rejected: no normalized source envelope");
      return std::nullopt;
    }
    sourceBegin = mergedPieces.front().first;
    sourceEnd = mergedPieces.back().second;

    auto gapHasMappedTUToken = [&](uint64_t gapBegin,
                                   uint64_t gapEnd) -> bool {
      for (const auto &entry : model_.GetTokmap()) {
        if (!PathsEqual(entry.file, tuPath))
          continue;
        if (entry.b < gapEnd && gapBegin < entry.e)
          return true;
      }
      return false;
    };

    auto gapHasRecordedNonConditionalArtifact = [&](uint64_t gapBegin,
                                                    uint64_t gapEnd) -> bool {
      auto overlapsGap = [&](uint64_t begin, uint64_t end) {
        return begin < gapEnd && gapBegin < end;
      };

      for (const auto &inc : model_.GetIncludes())
        if (PathsEqual(inc.sitePath, tuPath) && overlapsGap(inc.siteB, inc.siteE))
          return true;
      for (const auto &m : model_.GetMacroInvocations())
        if (m.invFile && PathsEqual(*m.invFile, tuPath) && m.invB && m.invE &&
            overlapsGap(*m.invB, *m.invE))
          return true;
      for (const auto &directive : model_.GetMacroDirectives())
        if (PathsEqual(directive.sitePath, tuPath) &&
            overlapsGap(directive.siteB, directive.siteE))
          return true;
      for (const auto &pragma : model_.GetPragmas())
        if (PathsEqual(pragma.sitePath, tuPath) &&
            overlapsGap(pragma.siteB, pragma.siteE))
          return true;
      return false;
    };

    auto gapIsRecordedConditionalControlTail = [&](uint64_t gapBegin,
                                                   uint64_t gapEnd) -> bool {
      if (!h.isDeleteOnly() || gapBegin >= gapEnd || gapEnd > tuBytes.size())
        return false;
      if (gapHasMappedTUToken(gapBegin, gapEnd) ||
          gapHasRecordedNonConditionalArtifact(gapBegin, gapEnd))
        return false;

      const RefoldModel::CondGroup *owningGroup = nullptr;
      for (const auto &group : model_.GetConds()) {
        if (!PathsEqual(group.file, tuPath) || group.parentIncludeId)
          continue;
        if (group.groupB < gapBegin && gapBegin < group.groupE &&
            gapEnd == group.groupE) {
          owningGroup = &group;
          break;
        }
      }
      if (!owningGroup)
        return false;

      StringRef text = tuBytes.slice(gapBegin, gapEnd);
      size_t cursor = 0;
      bool sawEndif = false;
      bool atLineStart = stringutils::beginsLineAfterWs(tuBytes, gapBegin);
      while (cursor < text.size()) {
        char ch = text[cursor];
        if (stringutils::isWs(ch)) {
          atLineStart = ch == '\n' || ch == '\r';
          ++cursor;
          continue;
        }
        if (ch != '#' || !atLineStart)
          return false;

        size_t lineEnd = cursor;
        while (lineEnd < text.size() && text[lineEnd] != '\n')
          ++lineEnd;
        const bool hasNewline = lineEnd < text.size();
        if (hasNewline)
          ++lineEnd;

        StringRef line = text.slice(cursor, lineEnd);
        if (line.ends_with("\n"))
          line = line.drop_back();
        if (line.ends_with("\r"))
          line = line.drop_back();
        StringRef rest = line.ltrim(" \t\v\f");
        if (!rest.consume_front("#"))
          return false;
        rest = rest.ltrim(" \t\v\f");
        if (!rest.consume_front("endif"))
          return false;
        rest = rest.trim(" \t\v\f");
        if (!rest.empty())
          return false;

        sawEndif = true;
        cursor = lineEnd;
        atLineStart = true;
      }

      if (!sawEndif)
        return false;

      trace("fallback",
            "TU/include closure preserving conditional-control tail "
            "source=[{0},{1}) group={2}",
            gapBegin, gapEnd, owningGroup->id);
      return true;
    };

    auto includeIsInsidePreservableZeroTokenConditionalGap =
        [&](const RefoldModel::IncludeItem &inc) -> bool {
      for (const auto &group : model_.GetConds()) {
        if (!conditionalGroupIsConsumableZeroTokenGap(group, sourceBegin,
                                                      sourceEnd))
          continue;
        if (group.groupB <= inc.siteB && inc.siteB < inc.siteE &&
            inc.siteE <= group.groupE)
          return true;
      }
      return false;
    };

    // Do not silently delete any include directive other than the run whose
    // expansion is explicitly part of this closure.  A complete unrelated
    // include with an empty PP cover may be consumed only when it is fully
    // inside the same source envelope and all recorded structure is either
    // absent or limited to source-neutral conditional control, nested
    // zero-token includes, and consumable macro-state directives.  That makes
    // it part of the replaced source interval, not trivia to preserve or
    // reorder around the B-side material.
    for (const auto &inc : model_.GetIncludes()) {
      if (!PathsEqual(inc.sitePath, tuPath))
        continue;
      if (inc.siteB >= sourceEnd || sourceBegin >= inc.siteE)
        continue;
      if (includeIsTouched(inc))
        continue;
      std::optional<std::string> sideEffectReason =
          includeRecordedSideEffectReason(inc);
      if (!inc.cover.IsValid() && !sideEffectReason &&
          sourceBegin <= inc.siteB && inc.siteE <= sourceEnd)
        continue;
      if (sideEffectReason &&
          includeIsInsidePreservableZeroTokenConditionalGap(inc))
        continue;

      if (sideEffectReason) {
        trace("fallback",
              "TU/include closure rejected: source interval [{0},{1}) would "
              "absorb unrelated include id={2} site=[{3},{4}) cover=[{5},{6}): "
              "{7}",
              sourceBegin, sourceEnd, inc.id, inc.siteB, inc.siteE,
              inc.cover.begin, inc.cover.end, *sideEffectReason);
      } else {
        trace("fallback",
              "TU/include closure rejected: source interval [{0},{1}) would "
              "absorb unrelated include id={2} site=[{3},{4}) cover=[{5},{6})",
              sourceBegin, sourceEnd, inc.id, inc.siteB, inc.siteE,
              inc.cover.begin, inc.cover.end);
      }
      return std::nullopt;
    }

    SourceLineDirectiveLogicalLineRewriter sourceLineDirectiveLineRewriter =
        [&](StringRef logicalLine, ArrayRef<uint64_t> sourceOffsets,
            SourceLineDirectiveBuiltinMacroResolver builtinMacroResolver)
            -> std::optional<SourceLineDirectiveLogicalLineRewrite> {
      return rewriteSourceLineDirectiveLogicalLineMacros(
          model_, tuPath, logicalLine, sourceOffsets,
          [&](StringRef lhs, StringRef rhs) { return PathsEqual(lhs, rhs); },
          std::nullopt, std::move(builtinMacroResolver));
    };

    DenseSet<uint64_t> mixedSourceLineDirectiveMacroIds;
    auto recordSourceLineDirectiveMacroIds =
        [&](ArrayRef<uint64_t> macroIds) {
      for (uint64_t macroId : macroIds)
        mixedSourceLineDirectiveMacroIds.insert(macroId);
    };

    auto macroInvocationIsInsideConsumableSourceLineDirectiveGap =
        [&](const RefoldModel::MacroInvocation &macro) -> bool {
      if (!macro.invFile || macro.invFile->empty() ||
          !PathsEqual(*macro.invFile, tuPath))
        return false;
      if (!macro.invB || !macro.invE || *macro.invB >= *macro.invE)
        return false;

      for (size_t idx = 1; idx < mergedPieces.size(); ++idx) {
        const uint64_t gapBegin = mergedPieces[idx - 1].second;
        const uint64_t gapEnd = mergedPieces[idx].first;
        if (*macro.invB < gapBegin || gapEnd < *macro.invE)
          continue;

        SmallVector<uint64_t, 4> acceptedMacroIds;
        if (!computeSourceLineDirectiveGapResume(
                tuBytes, gapBegin, gapEnd, sourceEnd, tuPath,
                sourceLineDirectiveLineRewriter, &acceptedMacroIds,
                StringRef(),
                !sourceSuffixMayObservePresumedFileSpelling(
                    model_, tuPath, sourceEnd,
                    [&](StringRef lhs, StringRef rhs) {
                      return PathsEqual(lhs, rhs);
                    },
                    tuBytes)))
          continue;

        recordSourceLineDirectiveMacroIds(acceptedMacroIds);
        for (uint64_t macroId : acceptedMacroIds)
          if (macroId == macro.id)
            return true;
      }
      return false;
    };

    auto macroInvocationIsInsidePreservableZeroTokenConditionalGap =
        [&](const RefoldModel::MacroInvocation &macro) -> bool {
      if (!macro.invFile || macro.invFile->empty() ||
          !PathsEqual(*macro.invFile, tuPath))
        return false;
      if (!macro.invB || !macro.invE || *macro.invB >= *macro.invE)
        return false;

      for (const auto &group : model_.GetConds()) {
        if (!conditionalGroupIsConsumableZeroTokenGap(group, sourceBegin,
                                                      sourceEnd))
          continue;
        if (group.groupB <= *macro.invB && *macro.invB < *macro.invE &&
            *macro.invE <= group.groupE)
          return true;
      }
      return false;
    };

    // Do not delete or partially overwrite unrelated macro invocations.  Four
    // complete-callsite cases are allowed inside the replacement envelope:
    //
    //   * macro-expanded include-target spelling inside a touched include
    //     directive, because the directive site is already a required piece;
    //   * zero-token source-neutral macro gaps, which contributed no PP tokens;
    //   * macro uses in preserved zero-token conditional-control islands;
    //   * source-bearing macro calls whose entire produced PP material is
    //     already inside this hunk's A-side replacement range.
    //
    // The last case is the source-bearing analogue of the zero-token gap: the
    // macro invocation is not being preserved or moved, it is part of the source
    // interval whose materialized tokens the user replaced.
    for (const auto &m : model_.GetMacroInvocations()) {
      if (m.invFile && !m.invFile->empty() && !PathsEqual(*m.invFile, tuPath))
        continue;
      if (!m.invB || !m.invE)
        continue;
      if (*m.invB < sourceEnd && sourceBegin < *m.invE) {
        if (macroInvocationIsInsideTouchedIncludeDirective(m) ||
            macroInvocationIsConsumableZeroTokenGap(m, sourceBegin,
                                                   sourceEnd) ||
            macroInvocationMaterialIsConsumed(m, sourceBegin, sourceEnd) ||
            mixedSourceLineDirectiveMacroIds.contains(m.id) ||
            macroInvocationIsInsideConsumableSourceLineDirectiveGap(m) ||
            macroInvocationIsInsidePreservableZeroTokenConditionalGap(m))
          continue;
        trace("fallback",
              "TU/include closure rejected: source interval [{0},{1}) would "
              "absorb macro invocation id={2} name='{3}' site=[{4},{5})",
              sourceBegin, sourceEnd, m.id, m.name, *m.invB, *m.invE);
        return std::nullopt;
      }
    }

    // The envelope may widen across physical gaps between required pieces, but
    // only across source-neutral control/trivia or complete zero-token source
    // artifacts.  Complete comments and literal empty conditional-control
    // islands are consumed with the replacement for the same reason as
    // whitespace: they contribute no PP tokens and are physically inside the
    // source interval whose A-side material was replaced by the B-side hunk.
    // Any other byte would be meaningful source text that the proof has not
    // justified replacing.
    // The source envelope was already computed from the normalized piece list
    // above.  At this point the only remaining common owner-proof step is the
    // inter-piece gap proof; avoid recomputing the same envelope here.
    if (!proveOwnerSourceEnvelopeGaps(
            mergedPieces, [&](uint64_t gapBegin, uint64_t gapEnd) {
              StringRef gap = tuBytes.slice(gapBegin, gapEnd);
              const bool preserveConditionalControlTail =
                  gapIsRecordedConditionalControlTail(gapBegin, gapEnd);
              if (preserveConditionalControlTail) {
                mixedPreservedConditionalControlTrivia.append(gap.begin(),
                                                             gap.end());
                return true;
              }
              if (isPreservableIncludeClosureGapTrivia(gap))
                return true;

              SmallVector<TUPreservedGapPiece, 4> preservedZeroTokenPieces;
              if (collectPreservableZeroTokenGapPieces(
                      gapBegin, gapEnd, preservedZeroTokenPieces)) {
                mixedPreservedZeroTokenGapPieces.append(
                    preservedZeroTokenPieces.begin(),
                    preservedZeroTokenPieces.end());
                return true;
              }

              if (gapIsConsumableZeroTokenSourceClosure(
                      gapBegin, gapEnd,
                      /*allowTUConditionalControl=*/true))
                return true;

              SmallVector<uint64_t, 4> acceptedMacroIds;
              if (std::optional<SourceLineDirectiveGapResume> lineResume =
                      computeSourceLineDirectiveGapResume(
                          tuBytes, gapBegin, gapEnd, sourceEnd, tuPath,
                          sourceLineDirectiveLineRewriter, &acceptedMacroIds,
                          StringRef(),
                          !sourceSuffixMayObservePresumedFileSpelling(
                              model_, tuPath, sourceEnd,
                              [&](StringRef lhs, StringRef rhs) {
                                return PathsEqual(lhs, rhs);
                              },
                              tuBytes))) {
                // A source-spelled line-control directive contributes no PP
                // tokens, but it is not disposable trivia.  If the copied
                // suffix has no live line-state observer, preserve the original
                // source directive spelling as source material.  If the suffix
                // does observe line state, emit a local resume that restores the
                // net state at the suffix after the consumed envelope.
                recordSourceLineDirectiveMacroIds(acceptedMacroIds);
                if (!OwnerSuffixLineStateObserverDemand(std::nullopt, tuPath,
                                                        sourceEnd)
                         .Any()) {
                  mixedPreservedSourceLineDirectiveGapPieces.push_back(
                      {gapBegin, gapEnd});
                  trace("fallback",
                        "TU/include closure preserving source #line spelling "
                        "from gap=[{0},{1})",
                        gapBegin, gapEnd);
                } else {
                  mixedSourceLineDirectiveResume = std::move(lineResume);
                  trace("fallback",
                        "TU/include closure preserving source #line state from "
                        "gap=[{0},{1}) resumeLine={2} file={3}",
                        gapBegin, gapEnd,
                        mixedSourceLineDirectiveResume->lineAtResume,
                        mixedSourceLineDirectiveResume->fileSpelling);
                }
                return true;
              }

              if (std::optional<std::string> pragmaReason =
                      nonConsumableTUPragmaGapReason(gapBegin, gapEnd)) {
                trace("fallback", "TU/include closure rejected: {0}",
                      *pragmaReason);
              } else {
                trace("fallback",
                      "TU/include closure rejected: non-trivia TU gap inside "
                      "mixed source interval [{0},{1}) gap='{2}'",
                      gapBegin, gapEnd,
                      stringutils::showWsWithClip(gap, 120));
              }
              return false;
            }))
      return std::nullopt;

    if (mixedSourceLineDirectiveResume && !lineDirs_.Enabled() &&
        sourceEnd < tuBytes.size()) {
      trace("fallback",
            "TU/include closure rejected: source #line gap needs enabled "
            "line directives to preserve the untouched suffix");
      return std::nullopt;
    }

    if (lineDirs_.Enabled() && mixedSourceLineDirectiveResume &&
        !lineDirectiveStartsAtPrefix(sourceEnd)) {
      trace("fallback",
            "TU/include closure rejected: source #line state from consumed "
            "gap cannot be restored before non-BOL suffix at source={0}",
            sourceEnd);
      return std::nullopt;
    }

    if (lineDirs_.Enabled() && mixedSourceLineDirectiveResume &&
        h.isDeleteOnly() &&
        !canStartLineDirectiveWithOptionalLeadingNewline(sourceBegin)) {
      trace("fallback",
            "TU/include closure rejected: delete-only source #line resume "
            "would be line-spliced at source={0}",
            sourceBegin);
      return std::nullopt;
    }

    trace("fallback",
          "TU/include closure source interval accepted: source=[{0},{1}) "
          "A=[{2},{3}) includeCover=[{4},{5})",
          sourceBegin, sourceEnd, materialBeginA, materialEndA, coverBegin,
          coverEnd);
  }

  if (!exactCover) {
    if (!hunkInsideCover && !mixedTUIncludeClosure) {
      trace("fallback",
            "TU include-closure rejected: touched include cover A=[{0},{1}) "
            "does not contain hunk A=[{2},{3})",
            coverBegin, coverEnd, h.aStart, h.aEnd);
      return std::nullopt;
    }

    // Widening from the unresolved hunk to the complete closure A-range is
    // admissible only when the widened-away prefix/suffix are observationally
    // unchanged in the token diff. That keeps this as a source-closure proof,
    // not a hidden multi-region merge: any independent token edit or insertion
    // in the extra cover must be handled by the normal partition/lattice path.
    const bool prefixClean = !rangeHasForeignTokenDiff(materialBeginA, h.aStart);
    const bool suffixClean = !rangeHasForeignTokenDiff(h.aEnd, materialEndA);
    if (!prefixClean || !suffixClean) {
      trace("fallback",
            "TU include-closure rejected: widening A=[{0},{1}) around hunk "
            "A=[{2},{3}) would absorb foreign token edits prefixClean={4} "
            "suffixClean={5}",
            coverBegin, coverEnd, h.aStart, h.aEnd, prefixClean ? 1 : 0,
            suffixClean ? 1 : 0);
      return std::nullopt;
    }

    if (sourceTouchesStagedEdit(sourceBegin, sourceEnd)) {
      trace("fallback",
            "TU include-closure rejected: widened source interval [{0},{1}) "
            "would absorb an already-staged source edit",
            sourceBegin, sourceEnd);
      return std::nullopt;
    }

    trace("fallback",
          "TU include-closure widening accepted: hunk A=[{0},{1}) widened to "
          "closure A=[{2},{3}) source=[{4},{5})",
          h.aStart, h.aEnd, materialBeginA, materialEndA, sourceBegin,
          sourceEnd);
  }

  if (sourceBegin >= sourceEnd || sourceEnd > tuBytes.size()) {
    trace("fallback",
          "TU include-closure rejected: invalid TU source interval [{0},{1}) "
          "fileLen={2}",
          sourceBegin, sourceEnd, tuBytes.size());
    return std::nullopt;
  }

  std::string preservedGapTrivia;
  uint64_t sourceCursor = first->siteB;
  for (const RefoldModel::IncludeItem *inc : touched) {
    if (inc->siteB < sourceCursor || inc->siteE > tuBytes.size() ||
        inc->siteB >= inc->siteE) {
      trace("fallback",
            "TU include-closure rejected: invalid include site for inc#{0} "
            "site=[{1},{2}) cursor={3} fileLen={4}",
            inc->id, inc->siteB, inc->siteE, sourceCursor, tuBytes.size());
      return std::nullopt;
    }

    // Inter-include source bytes are handled differently for pure and mixed
    // closures.  In a pure include closure, the source edit starts at the first
    // include directive and projects include material only, so inert comments or
    // directive-control trivia between adjacent include directives may need to
    // be preserved verbatim.  In a mixed TU/include closure, the earlier
    // whole-envelope verifier has already proved every gap between required
    // source pieces, including bytes between split include runs, and those bytes
    // are consumed by the B-side replacement.  Revalidating or preserving them
    // here would reintroduce the old contiguous-include assumption.
    if (!mixedTUIncludeClosure && inc->siteB > sourceCursor) {
      StringRef gap = tuBytes.slice(sourceCursor, inc->siteB);
      if (!gap.empty() && !stringutils::isWs(gap)) {
        if (gapIsConsumableZeroTokenSourceClosure(
                sourceCursor, inc->siteB,
                /*allowTUConditionalControl=*/false)) {
          trace("fallback",
                "TU include-closure consuming zero-token source gap before "
                "inc#{0} gap='{1}'",
                inc->id, stringutils::showWsWithClip(gap, 120));
        } else if (!isPreservableIncludeClosureGapTrivia(gap) &&
                   !gapIsPreservableRecordedConditionalIncludeClosure(
                       sourceCursor, inc->siteB)) {
          trace("fallback",
                "TU include-closure rejected: non-trivia TU gap between "
                "include directives before inc#{0} gap='{1}'",
                inc->id, stringutils::showWsWithClip(gap, 120));
          return std::nullopt;
        } else {
          preservedGapTrivia.append(gap.begin(), gap.end());
          trace("fallback",
                "TU include-closure preserving inert trivia/control gap before "
                "inc#{0} gap='{1}'",
                inc->id, stringutils::showWsWithClip(gap, 120));
        }
      }
    }

    sourceCursor = inc->siteE;
  }

  IncludeRealizationEvidenceKind evidenceKind =
      IncludeRealizationEvidenceKind::Unknown;
  auto bEnvelope = ResolveIncludeRealizationBTokenEnvelope(materialBeginA,
                                                           materialEndA,
                                                           &evidenceKind);
  if (h.isDeleteOnly() && materialBeginA == h.aStart &&
      materialEndA == h.aEnd) {
    // A delete-only exact closure has no edited B material by definition.  Do
    // not let a neighbor-based cover projection reintroduce adjacent surviving
    // tokens into the replacement; any source-control bytes that must survive
    // are carried explicitly as preserved trivia below.
    bEnvelope = std::make_pair(static_cast<size_t>(h.bStart),
                               static_cast<size_t>(h.bEnd));
    evidenceKind = IncludeRealizationEvidenceKind::CanonicalBCoverEnvelope;
    trace("fallback",
          "TU/include closure using exact delete hunk B envelope: "
          "A=[{0},{1}) B=[{2},{3})",
          materialBeginA, materialEndA, h.bStart, h.bEnd);
  } else if (!bEnvelope && materialBeginA == h.aStart &&
             materialEndA == h.aEnd) {
    bEnvelope = std::make_pair(static_cast<size_t>(h.bStart),
                               static_cast<size_t>(h.bEnd));
    evidenceKind = IncludeRealizationEvidenceKind::CanonicalBCoverEnvelope;
    trace("fallback",
          "TU/include closure using exact diff hunk B envelope: "
          "A=[{0},{1}) B=[{2},{3})",
          materialBeginA, materialEndA, h.bStart, h.bEnd);
  }
  if (!bEnvelope) {
    trace("fallback",
          "TU include-closure rejected: no canonical-or-consensus B envelope "
          "for A-cover [{0},{1})",
          materialBeginA, materialEndA);
    return std::nullopt;
  }

  auto bMaterialBegin = bEnvelope->first;
  auto bMaterialEnd = bEnvelope->second;

  // Pure include-closure projection can include stable boundary tokens that are
  // outside the include cover, such as the `{` before an include run inside an
  // initializer. The source edit starts at the first include directive, so such
  // outside anchors must not be materialized or they would be duplicated in the
  // TU. A mixed TU/include closure consumes those TU-side boundary tokens as
  // part of the same source interval, so it keeps the full closure envelope.
  if (!mixedTUIncludeClosure) {
    auto bTokenMapsOutsideCover = [&](size_t bTok) -> bool {
      if (bTok >= abTokMapB2A_.size())
        return false;
      const int64_t mappedA = abTokMapB2A_[bTok];
      if (mappedA < 0)
        return false;
      const uint64_t aTok = static_cast<uint64_t>(mappedA);
      return aTok < coverBegin || aTok >= coverEnd;
    };

    while (bMaterialBegin < bMaterialEnd &&
           bTokenMapsOutsideCover(bMaterialBegin))
      ++bMaterialBegin;
    while (bMaterialBegin < bMaterialEnd &&
           bTokenMapsOutsideCover(bMaterialEnd - 1))
      --bMaterialEnd;
  }

  if (h.bStart < bMaterialBegin || h.bEnd > bMaterialEnd) {
    trace("fallback",
          "TU include-closure rejected: hunk B=[{0},{1}) is outside trimmed "
          "closure B material [{2},{3}) from envelope [{4},{5})",
          h.bStart, h.bEnd, bMaterialBegin, bMaterialEnd, bEnvelope->first,
          bEnvelope->second);
    return std::nullopt;
  }

  std::string rawReplacement = SliceBSource(bMaterialBegin, bMaterialEnd).str();
  std::string replacement = rawReplacement;

  // Delete-only mixed closures may span a conditional-control tail that must
  // remain in the TU even though the source pieces on both sides are consumed.
  // Such bytes are admitted only when they are a complete recorded #endif tail
  // with no mapped PP tokens or unrelated source artifacts in the gap.
  if (!mixedPreservedConditionalControlTrivia.empty()) {
    if (startsWithPreprocessorDirectiveTrivia(
            mixedPreservedConditionalControlTrivia) &&
        !replacement.empty() && replacement.back() != '\n')
      replacement.push_back('\n');
    replacement.append(mixedPreservedConditionalControlTrivia.begin(),
                       mixedPreservedConditionalControlTrivia.end());
  }

  // Preserve legacy inert trivia between touched include directives.  Empty
  // includes admitted by the gap proof above are deliberately not appended here:
  // they lie inside the source envelope replaced by this closure, so the B-side
  // material consumes them along with the adjacent TU/include tokens.
  if (!preservedGapTrivia.empty()) {
    if (startsWithPreprocessorDirectiveTrivia(preservedGapTrivia) &&
        !replacement.empty() && replacement.back() != '\n')
      replacement.push_back('\n');
    replacement.append(preservedGapTrivia.begin(), preservedGapTrivia.end());
  }

  // Mixed TU/include closures may span source-neutral macro callsites or complete
  // conditional islands that contributed no PP tokens. These pieces are not
  // stale source: preserving them keeps the refolded source closer to the
  // original while the zero-token proof above guarantees that replaying them
  // cannot change the B token stream. Emit only the normalized top-level gap
  // pieces, not the surrounding trivia, mirroring the header owner-envelope
  // preservation path.
  if (!mixedPreservedZeroTokenGapPieces.empty()) {
    if (!replacement.empty() && replacement.back() != '\n')
      replacement.push_back('\n');
    for (const TUPreservedGapPiece &piece :
         mixedPreservedZeroTokenGapPieces) {
      std::string pieceText = preservedTUGapPieceText(piece);
      if (pieceText.empty())
        continue;
      replacement += pieceText;
      if (replacement.empty() || replacement.back() != '\n')
        replacement.push_back('\n');
    }
  }

  if (!mixedPreservedSourceLineDirectiveGapPieces.empty()) {
    for (const auto &piece : mixedPreservedSourceLineDirectiveGapPieces) {
      if (piece.first > piece.second || piece.second > tuBytes.size())
        continue;

      StringRef pieceText = tuBytes.slice(piece.first, piece.second);

      // The preserved source gap is sliced from the original file bytes.  When
      // the preceding replacement material already ends at the beginning of a
      // fresh physical line, a leading line break in the source slice is the
      // same separator, not an additional intentional blank line.  Drop only
      // that duplicated separator; any further blank lines remain preserved.
      if (!replacement.empty() && replacement.back() == '\n') {
        if (pieceText.starts_with("\r\n"))
          pieceText = pieceText.drop_front(2);
        else if (pieceText.starts_with("\n") || pieceText.starts_with("\r"))
          pieceText = pieceText.drop_front(1);
      }

      if (!replacement.empty() && replacement.back() != '\n' &&
          !pieceText.starts_with("\n") && !pieceText.starts_with("\r"))
        replacement.push_back('\n');

      replacement.append(pieceText.begin(), pieceText.end());
      if (replacement.empty() || replacement.back() != '\n')
        replacement.push_back('\n');
    }
  }

  // The closure begins at the proved source interval.  For a pure include
  // closure that is the include-directive run; for a mixed TU/include closure it
  // also includes the directly consumed TU tokens adjacent to that run.  We may
  // later extend the right edge over immediately following untouched TU line(s)
  // so any required physical-line compensation lands after copied suffix lines
  // such as `};` instead of inside the realized replacement itself.
  uint64_t closureSourceEnd = sourceEnd;
  std::string padded = PadAtBoundaries(
      tuBytes, static_cast<size_t>(sourceBegin),
      static_cast<size_t>(closureSourceEnd), std::move(replacement),
      /*allowLeft=*/true, /*allowRight=*/true);

  const bool emitsSourceLineDirectiveResume =
      lineDirs_.Enabled() && mixedSourceLineDirectiveResume &&
      sourceEnd < tuBytes.size();
  std::vector<FinalLineControlPruneCandidate> sourceLineResumeCandidates;
  if (emitsSourceLineDirectiveResume) {
    // The consumed source envelope contained a source-spelled line-control
    // directive.  Re-emit only its net state, adjusted through the consumed
    // envelope, so the untouched suffix observes the same presumed file/line as
    // it did in the original source. Replacement hunks normally carry edited B
    // material before the resync; delete-only hunks have no such payload, so
    // they may need a leading physical newline to make the resync a standalone
    // preprocessing directive rather than gluing `#line` onto the preceding
    // source line.
    if (padded.empty()) {
      if (!lineDirectiveStartsAtPrefix(sourceBegin))
        padded.push_back('\n');
    } else if (padded.back() != '\n') {
      padded.push_back('\n');
    }

    const uint64_t resumeBegin = static_cast<uint64_t>(padded.size());
    padded +=
        formatSourceLineDirectiveGapResume(*mixedSourceLineDirectiveResume);
    const uint64_t resumeEnd = static_cast<uint64_t>(padded.size());
    (void)resumeBegin;
    (void)resumeEnd;
    trace("fallback",
          "TU/include closure emitted source #line resume line={0} file={1}",
          mixedSourceLineDirectiveResume->lineAtResume,
          mixedSourceLineDirectiveResume->fileSpelling);
  }

  // This closure class replaces only a contiguous run of top-level include
  // directives plus admitted inert trivia between them. Preserve the original
  // physical line count of the replaced source so the untouched suffix can
  // resume on its original logical line without needing a #line directive.
  // When there is a deficit, first try to sink it past immediately following
  // untouched TU
  // line(s) by absorbing those exact source bytes into the same TU edit.
  const size_t origNl =
      stringutils::countNewlines(tuBytes, sourceBegin, closureSourceEnd);
  const size_t initialReplNl = stringutils::countNewlines(padded);
  if (!emitsSourceLineDirectiveResume && origNl > initialReplNl) {
    size_t remainingPadNl = origNl - initialReplNl;

    // Return true when the source interval overlaps a spelled include directive
    // or macro invocation in this TU. Widened include-closure edits must not
    // absorb such artifacts implicitly.
    auto intervalOverlapsSpelledArtifact = [&](uint64_t begin,
                                               uint64_t end) -> bool {
      for (const auto &inc : model_.GetIncludes()) {
        if (!PathsEqual(inc.sitePath, tuPath))
          continue;
        if (inc.siteB < end && begin < inc.siteE)
          return true;
      }
      for (const auto &m : model_.GetMacroInvocations()) {
        if (m.invFile && !m.invFile->empty() && !PathsEqual(*m.invFile, tuPath))
          continue;
        if (!m.invB || !m.invE)
          continue;
        if (*m.invB < end && begin < *m.invE)
          return true;
      }
      return false;
    };

    if (remainingPadNl > 0 && closureSourceEnd < tuBytes.size()) {
      const uint64_t lineBegin = closureSourceEnd;
      uint64_t lineEnd = lineBegin;
      while (lineEnd < tuBytes.size() && tuBytes[lineEnd] != '\n')
        ++lineEnd;
      if (lineEnd < tuBytes.size())
        ++lineEnd;

      // Sink the unavoidable physical-line compensation past at most one
      // immediately following substantive TU line. This is enough to move the
      // blank-line surrogate past a copied suffix line such as `};`, while
      // remaining conservative: copied-line absorption is newline-deficit
      // invariant, so absorbing additional lines would only push the eventual
      // compensation point farther down the file without improving correctness.
      if (lineEnd > lineBegin &&
          !intervalOverlapsSpelledArtifact(lineBegin, lineEnd)) {
        StringRef line = tuBytes.slice(lineBegin, lineEnd);
        StringRef trimmed = line.trim();
        if (!trimmed.empty() && trimmed.front() != '#') {
          StringRef copiedLine = line;

          // The sink may start in the middle of a physical source line, for
          // example after the TU/include closure consumed the leading token of
          // `int between = 0;`.  In that case `padded` already contains the
          // B-side spelling and any B-side separator after that token, while
          // `line` begins with the original source separator before the next
          // token.  Keeping both separators manufactures visible whitespace
          // such as `int  between`.  Collapse exactly one copied horizontal
          // source byte only when the sink is a same-line continuation and the
          // replacement already ends in horizontal whitespace.  Remaining
          // source whitespace, if any, is preserved byte-for-byte.
          if (lineBegin > 0 && tuBytes[lineBegin - 1] != '\n' &&
              tuBytes[lineBegin - 1] != '\r' && !padded.empty() &&
              stringutils::isNonNewlineWs(padded.back()) &&
              !copiedLine.empty() &&
              stringutils::isNonNewlineWs(copiedLine.front())) {
            copiedLine = copiedLine.drop_front();
          }

          padded.append(copiedLine.begin(), copiedLine.end());
          closureSourceEnd = lineEnd;

          // Re-emitting one untouched trailing line verbatim moves the eventual
          // physical-line compensation point to just after that copied line,
          // but it does not reduce the outstanding newline deficit. The
          // original source span grows by the same newline count as the
          // replacement text, so the difference between them is invariant under
          // this exact copied-line absorption.
          trace("fallback",
                "TU include-closure newline sink: absorbed source=[{0},{1}) "
                "line='{2}' copiedLine='{3}' remainingPad={4} "
                "(single-line invariant-preserving sink)",
                lineBegin, lineEnd, stringutils::showWsWithClip(line, 120),
                stringutils::showWsWithClip(copiedLine, 120), remainingPadNl);
        }
      }
    }

    if (remainingPadNl > 0) {
      if (lineDirs_.Enabled()) {
        // Keep line-preservation semantic rather than physical in normal mode:
        // suppress synthetic blank padding here and let ApplyResyncOrPend()
        // observe the unchanged newline deficit over the final edit span. If a
        // resync is needed, it will be emitted as a #line directive after any
        // copied suffix line absorbed above.
        trace("fallback",
              "TU include-closure physical newline pad deferred to #line "
              "resync: source=[{0},{1}) suppressedNl={2}",
              sourceBegin, closureSourceEnd, remainingPadNl);
      } else {
        // In --no-lines mode, clang-refold intentionally does not preserve
        // source-location semantics. The verification path already masks
        // location-sensitive predefined macro differences for this mode, so
        // newline-deficit padding would only manufacture visible blank lines in
        // the refolded source. Boundary padding above is still responsible for
        // lexical separation; this padding is solely for physical line count.
        trace("fallback",
              "TU include-closure physical newline pad suppressed by "
              "--no-lines: source=[{0},{1}) suppressedNl={2}",
              sourceBegin, closureSourceEnd, remainingPadNl);
      }
      remainingPadNl = 0;
    }

    if (remainingPadNl > 0)
      padded.append(remainingPadNl, '\n');

    trace("fallback",
          "TU include-closure newline pad: source=[{0},{1}) origNl={2} "
          "replNl={3} physicalPadNl={4} finalSourceEnd={5} result='{6}'",
          sourceBegin, sourceEnd, origNl, initialReplNl, remainingPadNl,
          closureSourceEnd, stringutils::showWsWithClip(padded, 120));
  }

  // If this closure deletes the include instance that originally entered a
  // `#pragma once` header, a later same-header include that was token-empty in
  // A can become live in the refolded source.  Leaving that later directive in
  // place would replay declarations/tokens from the header that are not present
  // in B.  This is a source-order effect of `#pragma once`, not an ordinary
  // token-cover issue: the skipped include has no A cover, so it will not be
  // found by the hunk overlap logic above.
  //
  // The deterministic repair for this TU include-closure class is to extend the
  // same TU byte replacement over any immediately-reachable skipped same-header
  // include directives.  We only cross trivia between the current closure end
  // and the skipped directive.  If substantive source lies between them, this
  // proof class cannot decide whether that source observes the moved header
  // state, so it rejects instead of emitting a refolding that would reactivate
  // the include.
  // A consumed include only creates the reactivation hazard when the
  // physical header is known to carry include-local once state.  We use the
  // recorded pragma table instead of guessing from include emptiness: an empty
  // later include can have other causes, but a consumable `#pragma once` in the
  // resolved header gives the exact source-order state transition we must
  // preserve.
  auto pathHasPragmaOnce = [&](StringRef resolvedPath) -> bool {
    for (const auto &pragma : model_.GetPragmas())
      if (PathsEqual(pragma.sitePath, resolvedPath) &&
          pragmaIsConsumableIncludeLocalState(pragma))
        return true;
    return false;
  };

  // Interpret "consumed" in the final widened source interval, not in the
  // original hunk.  The closure may have already been padded/widened above, and
  // the reactivation proof must reason about the exact bytes that will be
  // replaced.
  auto includeIntervalIsConsumed = [&](const RefoldModel::IncludeItem &inc) {
    return sourceBegin <= inc.siteB && inc.siteE <= closureSourceEnd;
  };

  // Collect each resolved `#pragma once` header whose entering include is
  // removed by this closure.  Later skipped includes are matched by resolved
  // path, so repeated textual spellings such as "x.h" and "./x.h" collapse
  // to the same include-once state when the producer resolved them that way.
  SmallVector<StringRef, 4> consumedPragmaOncePaths;
  for (const RefoldModel::IncludeItem *inc : touched) {
    if (!inc->resolvedPath || !includeIntervalIsConsumed(*inc))
      continue;
    if (!pathHasPragmaOnce(*inc->resolvedPath))
      continue;
    if (!llvm::any_of(consumedPragmaOncePaths, [&](StringRef path) {
          return PathsEqual(path, *inc->resolvedPath);
        }))
      consumedPragmaOncePaths.push_back(*inc->resolvedPath);
  }

  auto includeMatchesConsumedPragmaOncePath =
      [&](const RefoldModel::IncludeItem &inc) -> bool {
    if (!inc.resolvedPath)
      return false;
    return llvm::any_of(consumedPragmaOncePaths, [&](StringRef path) {
      return PathsEqual(path, *inc.resolvedPath);
    });
  };

  // A skipped include is only dangerous if removing the closure would make it
  // the first preserved include of that resolved header.  If some earlier
  // same-header include remains before the candidate, that prior include still
  // establishes the `#pragma once` state and the candidate remains skipped.
  auto hasPreservedPriorSameHeaderInclude =
      [&](const RefoldModel::IncludeItem &candidate) -> bool {
    if (!candidate.resolvedPath)
      return false;
    for (const auto &prior : model_.GetIncludes()) {
      if (prior.id == candidate.id || prior.parent ||
          !PathsEqual(prior.sitePath, tuPath) || !prior.resolvedPath)
        continue;
      if (!PathsEqual(*prior.resolvedPath, *candidate.resolvedPath))
        continue;
      if (prior.siteE > candidate.siteB)
        continue;
      if (!includeIntervalIsConsumed(prior))
        return true;
    }
    return false;
  };

  auto extendOverReactivatedPragmaOnceIncludes = [&]() -> bool {
    if (consumedPragmaOncePaths.empty())
      return true;

    while (true) {
      const RefoldModel::IncludeItem *next = nullptr;

      // Pick the earliest skipped same-header include that would become live
      // after the current closure.  The loop repeats because consuming that
      // directive can expose another immediately-following skipped include of
      // the same header.  Ties are broken by item id to keep the proof
      // deterministic for equal byte offsets.
      for (const auto &inc : model_.GetIncludes()) {
        if (inc.parent || !PathsEqual(inc.sitePath, tuPath))
          continue;
        if (inc.siteB < closureSourceEnd)
          continue;
        // Only originally-skipped includes have no A-token cover.  Includes
        // with a cover already participated in normal token/hunk closure logic.
        if (inc.cover.IsValid())
          continue;
        if (!includeMatchesConsumedPragmaOncePath(inc))
          continue;
        if (hasPreservedPriorSameHeaderInclude(inc))
          continue;
        // Do not silently erase a skipped include if the map recorded effects
        // on that include instance.  This repair is only for the pure
        // `#pragma once` reactivation case where the directive was token-empty
        // because a prior include had already entered the header.
        if (includeHasRecordedSideEffects(inc))
          continue;
        if (!next || inc.siteB < next->siteB ||
            (inc.siteB == next->siteB && inc.id < next->id))
          next = &inc;
      }

      if (!next)
        return true;

      if (next->siteB > tuBytes.size() || next->siteE > tuBytes.size() ||
          next->siteB > next->siteE) {
        trace("fallback",
              "TU include-closure rejected: skipped pragma-once include "
              "inc#{0} has invalid site=[{1},{2})",
              next->id, next->siteB, next->siteE);
        return false;
      }

      // Crossing only trivia is what makes the widened edit local: no
      // preserved source between the deleted entering include and the skipped
      // include can observe the header macro state at the old position.
      StringRef gap = tuBytes.slice(closureSourceEnd, next->siteB);
      if (!isWsOrCompleteCommentTrivia(gap)) {
        trace("fallback",
              "TU include-closure rejected: consuming earlier #pragma once "
              "include would reactivate skipped include inc#{0}, but gap "
              "source=[{1},{2}) is not trivia: '{3}'",
              next->id, closureSourceEnd, next->siteB,
              stringutils::showWsWithClip(gap, 120));
        return false;
      }

      if (sourceTouchesStagedEdit(sourceBegin, next->siteE)) {
        trace("fallback",
              "TU include-closure rejected: extending over reactivated "
              "#pragma once include inc#{0} to sourceEnd={1} would overlap "
              "an already-staged source edit",
              next->id, next->siteE);
        return false;
      }

      trace("fallback",
            "TU include-closure extending over skipped #pragma once include "
            "inc#{0} path='{1}' source=[{2},{3}) after consuming earlier "
            "include instance",
            next->id, next->resolvedPath ? *next->resolvedPath : next->target,
            next->siteB, next->siteE);
      closureSourceEnd = next->siteE;
    }
  };

  if (!extendOverReactivatedPragmaOnceIncludes())
    return std::nullopt;

  debug("fallback",
        "TU include-closure accepted: includes=[{0},{1}] source=[{2},{3}) "
        "A=[{4},{5}) B=[{6},{7}) evidence={8} raw='{9}' padded='{10}'",
        first->id, last->id, sourceBegin, closureSourceEnd, materialBeginA,
        materialEndA, bEnvelope->first, bEnvelope->second, evidenceKind,
        stringutils::showWsWithClip(rawReplacement, 120),
        stringutils::showWsWithClip(padded, 120));

  ResyncOutcome ro = emitsSourceLineDirectiveResume
                         ? ResyncOutcome(padded, std::nullopt)
                         : ApplyResyncOrPend(tuBytes, sourceBegin,
                                             closureSourceEnd, padded, tuPath);
  TextEdit edit{sourceBegin,           closureSourceEnd, std::move(ro.text),
                std::move(ro.pending), std::nullopt,     {}, {}, {}};
  edit.lineControlPruneCandidates = emitsSourceLineDirectiveResume
                                        ? std::move(sourceLineResumeCandidates)
                                        : std::move(ro.lineControlPruneCandidates);
  StampTextEditMaterializedBTokenRange(edit, bEnvelope->first,
                                       bEnvelope->second);

  // The emitted artifact is still a deterministic TU byte replacement even
  // though the witness came from an exact include-closure proof. Reuse the TU
  // textual-edit carrier until a dedicated closure-edit proof class exists.
  AttachAcceptedResultCarrier(
      edit, BuildAcceptedTUTextEditCandidate(
                AcceptedPathKind::TUByteSpanConservativeEdit, sourceBegin,
                closureSourceEnd, StringRef(rawReplacement)));
  return edit;
}

bool RefoldEngine::ExpansionFallbackReplacementIsReplayStable(
    StringRef replacementText) const {
  if (replacementText.empty()) {
    trace("fallback",
          "macro-owner fallback synthesis rejected: empty replacement text");
    return false;
  }

  // Expansion fallback emits ordinary source text, not a preprocessor token
  // stream with the original hide-set context. Therefore the replacement must
  // be stable if it is lexed again as source: it must not contain tokens that
  // could participate in another macro-expansion decision.
  //
  // For the current fallback class, reject raw identifiers and preprocessor
  // paste/directive operators. Accepting those would require a stronger proof
  // that rescanning the emitted text cannot trigger a different macro expansion
  // than the original expansion witness.
  std::string buffer = replacementText.str();
  if (buffer.empty() || buffer.back() != '\n')
    buffer.push_back('\n');

  using namespace clang;
  DiagnosticOptions diagOpts;
  IntrusiveRefCntPtr<DiagnosticIDs> diagIDs(new DiagnosticIDs());
  auto *client = new IgnoringDiagConsumer();
  DiagnosticsEngine diags(diagIDs, diagOpts, client,
                          /*ShouldOwnClient=*/true);

  FileSystemOptions fso;
  FileManager fm(fso);
  SourceManager sm(diags, fm);
  auto mb =
      llvm::MemoryBuffer::getMemBufferCopy(buffer, "<expansion-fallback>");
  FileID fid = sm.createFileID(std::move(mb));

  // Raw-lex the synthesized replacement under the same language options used
  // by the recorded preprocessor model. This checks the text as source
  // spelling, without macro expansion or preprocessor state.
  const LangOptions lang = MakeLexLangOptions(model_.GetPPLang());
  Lexer lex(fid, sm.getBufferOrFake(fid), sm, lang);
  lex.SetKeepWhitespaceMode(false);

  const StringRef fileBytes = sm.getBufferOrFake(fid).getBuffer();
  Token token;
  while (true) {
    lex.LexFromRawLexer(token);
    if (token.is(clang::tok::eof))
      break;

    // These token kinds are replay-unstable in ordinary source text: an
    // identifier may expand as a macro, and #/## have preprocessor semantics
    // that are not justified by this fallback proof.
    if (token.is(clang::tok::raw_identifier) || token.is(clang::tok::hash) ||
        token.is(clang::tok::hashhash)) {
      const char *ptr = sm.getCharacterData(token.getLocation());
      const size_t off = static_cast<size_t>(ptr - fileBytes.data());
      const StringRef spelling(ptr, token.getLength());
      trace("fallback",
            "macro-owner fallback synthesis rejected: replay-unstable token "
            "kind={0} off={1} spelling='{2}'",
            clang::tok::getTokenName(token.getKind()), off,
            stringutils::showWsWithClip(spelling, 80));
      return false;
    }
  }

  return true;
}

std::optional<std::string>
RefoldEngine::SynthesizeMacroOwnerExpansionClosureSource(
    const ExpansionClosureWitness &witness) const {
  // This synthesis path is intentionally narrow: it only handles a single
  // self-contained top-level macro-owner expansion in the TU. Include-owned
  // witnesses, clipped witnesses, and multi-owner closures must use their
  // normal structural/whole-cover paths instead.
  if (witness.kind != ExpansionClosureKind::MacroOwnerWholeExpansion ||
      !witness.hasRootMacroId || witness.hasOwnerIncludeId) {
    trace("fallback", "macro-owner fallback synthesis rejected: witness not in "
                      "single-root macro domain");
    return std::nullopt;
  }

  // The witness must describe one complete, unclipped source interval and one
  // non-empty B-token envelope. If any of these invariants are missing, the
  // fallback would not be a closed owner splice.
  if (!witness.selfContained || witness.claimsClipped ||
      witness.sourceByteEnd <= witness.sourceByteBegin ||
      witness.bTokEnd <= witness.bTokBegin) {
    trace("fallback",
          "macro-owner fallback synthesis rejected: invalid witness bounds or "
          "clipped/selfContained flags source=[{0},{1}) B=[{2},{3}) "
          "selfContained={4} claimsClipped={5}",
          witness.sourceByteBegin, witness.sourceByteEnd, witness.bTokBegin,
          witness.bTokEnd, witness.selfContained ? 1 : 0,
          witness.claimsClipped ? 1 : 0);
    return std::nullopt;
  }

  const StringRef tuPath = model_.GetSourcePath();
  if (tuPath.empty()) {
    trace("fallback",
          "macro-owner fallback synthesis rejected: empty TU source path");
    return std::nullopt;
  }

  // Read the original TU source because this fallback emits a complete TU text,
  // not just a local replacement fragment.
  const std::string absTuPath = lineDirs_.ToAbsolutePath(tuPath);
  auto tuBufferOrErr = llvm::MemoryBuffer::getFile(absTuPath);
  if (!tuBufferOrErr) {
    debug("fallback",
          "macro-owner fallback synthesis rejected: failed to read TU source "
          "'{0}' ({1})",
          absTuPath, tuBufferOrErr.getError().message());
    return std::nullopt;
  }
  const StringRef tuBytes = (*tuBufferOrErr)->getBuffer();

  if (witness.sourceByteEnd > tuBytes.size()) {
    trace("fallback",
          "macro-owner fallback synthesis rejected: source interval [{0},{1}) "
          "exceeds TU size={2}",
          witness.sourceByteBegin, witness.sourceByteEnd, tuBytes.size());
    return std::nullopt;
  }

  // Materialize the witness's full proved B frontier. Unlike structural
  // whole-cover patches, this fallback is a single closed owner splice, so it
  // must not inherit standalone-claim clipping that is only valid when B
  // insertions are emitted through separate structural edits.
  std::string replacement =
      SliceBSource(witness.bTokBegin, witness.bTokEnd).str();
  if (replacement.empty()) {
    trace("fallback",
          "macro-owner fallback synthesis rejected: materialization for "
          "B=[{0},{1}) is empty",
          witness.bTokBegin, witness.bTokEnd);
    return std::nullopt;
  }

  // The synthesized source must be safe to replay as ordinary source text. In
  // particular, it must not rely on hidden preprocessor state from the original
  // expansion.
  if (!ExpansionFallbackReplacementIsReplayStable(replacement))
    return std::nullopt;

  // Re-embed the materialized owner into the TU using the same boundary
  // spacing and line-drift machinery as ordinary TU byte edits. This keeps the
  // synthesis class deterministic and avoids a separate emission path with
  // subtly different token-adjacency or #line behavior.
  std::string padded =
      PadAtBoundaries(tuBytes, static_cast<size_t>(witness.sourceByteBegin),
                      static_cast<size_t>(witness.sourceByteEnd),
                      std::move(replacement), /*allowLeft=*/true,
                      /*allowRight=*/true);
  ResyncOutcome ro = ApplyResyncOrPend(tuBytes, witness.sourceByteBegin,
                                       witness.sourceByteEnd, padded, tuPath);

  // Assemble the final TU text while preserving pending line-resync state
  // across copied original slices. The only changed region is the witnessed
  // source byte interval.
  SmallString<0> out;
  out.reserve(tuBytes.size() -
              (witness.sourceByteEnd - witness.sourceByteBegin) +
              ro.text.size() + 64);
  std::optional<PendingResync> pending;
  pending = AppendOriginalSliceWithPending(
      out, tuBytes, 0, witness.sourceByteBegin, std::move(pending));
  const uint64_t emittedBegin = static_cast<uint64_t>(out.size());
  out.append(ro.text);
  const uint64_t emittedEnd = static_cast<uint64_t>(out.size());
  if (materializedEditMappings_) {
    materializedEditMappings_->clear();
    if (auto bBytes = BTokenRangeToByteRange(witness.bTokBegin,
                                             witness.bTokEnd)) {
      materializedEditMappings_->push_back(MaterializedEditMapping{
          bBytes->first, bBytes->second, emittedBegin, emittedEnd});
    } else {
      fatal("edit-map", "invalid expansion fallback B token range [{0},{1})",
            witness.bTokBegin, witness.bTokEnd);
    }
  }
  pending = ro.pending;
  pending = AppendOriginalSliceWithPending(out, tuBytes, witness.sourceByteEnd,
                                           tuBytes.size(), std::move(pending));

  return std::string(out.str());
}

std::optional<std::string> RefoldEngine::TryExpansionClosureFallback() {
  if (!terminalFallbackRequested_)
    return std::nullopt;

  // Try the post-terminal expansion-closure safety net.
  //
  // This path is still proof-driven, but it is no longer allowed to be an
  // independent completeness mechanism. A synthesized result is admissible only
  // if:
  //
  //   1. a declared single-owner macro expansion-closure witness exists,
  //   2. that witness is dominated by the normal MacroWholeCoverRealization
  //      candidate path, and
  //   3. the synthesized source text is replay-stable when re-lexed as
  //      ordinary source.
  //
  // In other words, this stage may materialize a dominated fallback witness,
  // but it must not discover a refolding class that the normal candidate system
  // failed to expose.
  if (auto witness = BuildMacroOwnerExpansionClosureWitness()) {
    // Record the intermediate fallback witness for theorem-audit coverage. This
    // does not restamp the witness as a theorem-facing accepted carrier; it
    // only reports that the fallback stage found a declared witness to inspect.
    ++lastTheoremAudit_.expansionFallbackWitnesses;
    debug("fallback", "expansion-closure witness discovered: {0}",
          FormatExpansionClosureWitness(*witness));

    // The dominance check is the key guardrail: if this witness was not visible
    // through the ordinary macro whole-cover candidate path, then this fallback
    // would be adding a separate completeness mechanism instead of merely
    // materializing an already-dominated proof shape.
    std::string dominanceDetail;
    if (MacroOwnerExpansionClosureWitnessIsDominated(*witness,
                                                     dominanceDetail)) {
      debug("fallback/audit",
            "post-terminal macro expansion closure is dominated: {0}",
            dominanceDetail);
    } else {
      const std::string diagnostic =
          formatv("post-terminal macro expansion closure witness was not "
                  "visible through the normal MacroWholeCoverRealization "
                  "candidate path: witness={0}; reason={1}",
                  FormatExpansionClosureWitness(*witness), dominanceDetail)
              .str();
      if (strict_)
        fatal("fallback/audit", "{0}", diagnostic);
      warn("fallback/audit", "{0}", diagnostic);
    }

    // Even a dominated witness must synthesize into replay-stable source. This
    // prevents emitting expansion text that would rely on original preprocessor
    // state such as hide sets.
    if (auto source = SynthesizeMacroOwnerExpansionClosureSource(*witness)) {
      ++lastTheoremAudit_.expansionFallbackSynthesisSuccesses;
      debug("fallback",
            "expansion-closure source synthesized: rootMacro={0} "
            "source=[{1},{2}) B=[{3},{4}) outLen={5}",
            witness->rootMacroId, witness->sourceByteBegin,
            witness->sourceByteEnd, witness->bTokBegin, witness->bTokEnd,
            source->size());

      // The synthesized output is the original TU with exactly one proved
      // top-level macro owner expanded at its callsite, not a terminal fallback
      // to the edited preprocessed stream.
      ResetAttemptStats();
      lastStats_.expandedMacros = 1;
      lastStats_.terminalFallbackToB = false;
      return source;
    }

    ++lastTheoremAudit_.expansionFallbackSynthesisRejections;
    debug("fallback", "expansion-closure stage: witness could not be "
                      "synthesized into replay-stable source");
  } else {
    debug("fallback", "expansion-closure stage: no proved single-owner macro "
                      "fallback witness available");
  }

  return std::nullopt;
}

std::string RefoldEngine::ResolvePostStructuralFallback() {
  if (!terminalFallbackRequested_)
    return std::string();

  // Once structural emission has failed closed, retain the old
  // expansion-closure safety net before raw-B terminal fallback, but require
  // any discovered witness to pass the dominance audit in
  // TryExpansionClosureFallback().
  if (auto expanded = TryExpansionClosureFallback()) {
    // Count the one case where the proved intermediate expansion stage
    // successfully intercepts a raw-B terminal fallback. This remains coverage
    // only: the synthesized source is not a theorem-facing carrier, but the
    // audit should still say that the terminal request was resolved by the
    // declared intermediate fallback class instead of by raw B.
    ++lastTheoremAudit_.expansionFallbackTerminalRescues;
    return *expanded;
  }

  // Reaching this point means both structural emission and the intermediate
  // proof-backed expansion stage failed closed. Only now do we normalize the
  // run onto the one explicit terminal out-of-domain carrier.
  RecordTerminalFallbackTheoremAudit();

  debug("fallback",
        "terminal fallback: emitting fully expanded edited preprocessed "
        "stream (B). reasons={0}",
        terminalFallbackReasons_.size());
  const TerminalFallbackWitness terminalWitness =
      BuildTerminalFallbackWitness();
  const AcceptedResultCandidate terminalCandidate =
      BuildAcceptedTerminalCandidate(terminalWitness);
  debug("proof/inventory", "terminal result {0}",
        FormatAcceptedResultCandidate(terminalCandidate));
  for (const auto &r : terminalFallbackReasons_)
    debug("fallback", "  {0}", r);

  // The raw-B terminal carrier is the fully expanded surface, so every include
  // and every top-level macro root remains expanded in the final result.
  ResetAttemptStats();
  lastStats_.expandedIncludes = lastStats_.totalIncludes;
  lastStats_.expandedMacros = lastStats_.totalMacros;
  lastStats_.terminalFallbackToB = true;
  if (materializedEditMappings_) {
    materializedEditMappings_->clear();
    materializedEditMappings_->push_back(MaterializedEditMapping{
        0, static_cast<uint64_t>(bSource_.size()), 0,
        static_cast<uint64_t>(bSource_.size())});
  }
  return bSource_.str();
}
