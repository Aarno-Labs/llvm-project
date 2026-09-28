//===--- LineDirectiveInserter.cpp ------------------------------*- C++ -*-===//
//
// Source #line directive formatting, parsing, and local resync helpers.
//
// This file implements LineDirectiveInserter: directive formatting, the parser
// for already-emitted `#line` text that suppresses no-op resyncs, and local
// resync insertion.  The logical location at a source offset comes from the
// producer's line-control events (RefoldLineControlProof::OwnerLineStateAt),
// not from this file.
//
//===----------------------------------------------------------------------===//

#include "line-control/LineDirectiveInserter.h"

#include "support/StringUtils.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <llvm/ADT/SmallString.h>
#include <llvm/ADT/StringRef.h>
#include <llvm/Support/FileSystem.h>
#include <optional>
#include <string>
#include <utility>

using namespace llvm;

namespace clang {
namespace refold {

namespace {

static void appendDecodedLineControlFilenameEscape(StringRef src, size_t &p,
                                                   size_t to,
                                                   SmallString<64> &out) {
  char escaped = src[p++];
  if (escaped == 'x' || escaped == 'X') {
    unsigned value = 0;
    bool sawHex = false;
    while (p < to && std::isxdigit(static_cast<unsigned char>(src[p]))) {
      sawHex = true;
      char h = src[p++];
      value *= 16;
      if (h >= '0' && h <= '9')
        value += static_cast<unsigned>(h - '0');
      else if (h >= 'a' && h <= 'f')
        value += static_cast<unsigned>(10 + h - 'a');
      else if (h >= 'A' && h <= 'F')
        value += static_cast<unsigned>(10 + h - 'A');
    }
    out.push_back(static_cast<char>((sawHex ? value : escaped) & 0xff));
    return;
  }

  if (escaped >= '0' && escaped <= '7') {
    unsigned value = static_cast<unsigned>(escaped - '0');
    for (unsigned digits = 1;
         digits < 3 && p < to && src[p] >= '0' && src[p] <= '7'; ++digits)
      value = value * 8 + static_cast<unsigned>(src[p++] - '0');
    out.push_back(static_cast<char>(value & 0xff));
    return;
  }

  switch (escaped) {
  case '"':
  case '\\':
  case '?':
  case '\'':
    out.push_back(escaped);
    break;
  case 'a':
    out.push_back('\a');
    break;
  case 'b':
    out.push_back('\b');
    break;
  case 'e':
  case 'E':
    out.push_back(static_cast<char>(0x1b));
    break;
  case 'f':
    out.push_back('\f');
    break;
  case 'n':
    out.push_back('\n');
    break;
  case 'r':
    out.push_back('\r');
    break;
  case 't':
    out.push_back('\t');
    break;
  case 'v':
    out.push_back('\v');
    break;
  default:
    out.push_back(escaped);
    break;
  }
}

// Parse one logical preprocessing line as a line-control directive.
//
// Expected source line-control shapes:
//
//     #line <digits> ["file"]
//     # line <digits> ["file"]
//     # <digits> ["file"]
//
// Within the quoted file spelling, it decodes C string-literal escapes used by
// line-control filename operands. The optional filename operand is tracked
// separately from an explicitly empty filename string so callers can model
// `#line 200` as preserving the current file spelling.
//
static std::optional<LineDirectiveState>
parseLineDirectiveForLineControl(StringRef src, size_t from, size_t to) {
  if (from > to || to > src.size())
    return std::nullopt;

  size_t p = from;

  // A preprocessing directive may be preceded by horizontal whitespace.  Do
  // not cross a physical newline; `from/to` already delimit one source line.
  stringutils::skipWsNoLF(src, p, to);

  if (p >= to || src[p] != '#')
    return std::nullopt;
  ++p;

  // Both `#line` and `# line` are accepted spellings.  Clang/GCC also accept
  // the numeric line-control form `# 123 "file"`, so leave `p` at the digits
  // when there is no `line` keyword.
  stringutils::skipWsNoLF(src, p, to);

  bool usedLineKeyword = false;
  if (stringutils::directiveKeywordAt(src, p, to, "line")) {
    usedLineKeyword = true;
    p += 4;
    stringutils::skipWsNoLF(src, p, to);
  }

  const size_t lineStart = p;
  while (p < to && std::isdigit(static_cast<unsigned char>(src[p])))
    ++p;

  if (p == lineStart)
    return std::nullopt;

  size_t lineAfter;
  if (src.slice(lineStart, p).getAsInteger(10, lineAfter))
    return std::nullopt;

  stringutils::skipWsNoLF(src, p, to);

  // Parse the optional quoted filename operand.  Absence of this operand is
  // semantically meaningful: `#line 200` changes only the logical line number
  // and preserves the active logical file.
  llvm::SmallString<64> fileSpelling;
  bool hasFileSpelling = false;
  if (p < to && src[p] == '"') {
    hasFileSpelling = true;
    bool closedFileQuote = false;
    ++p; // consume opening quote
    while (p < to) {
      char c = src[p++];
      if (c == '"') {
        closedFileQuote = true;
        break;
      }
      if (c == '\\' && p < to)
        appendDecodedLineControlFilenameEscape(src, p, to, fileSpelling);
      else
        fileSpelling.push_back(c);
    }

    if (!closedFileQuote)
      return std::nullopt;
  }

  stringutils::skipWsNoLF(src, p, to);

  // GNU/Clang line-marker directives emitted by preprocessors can carry
  // numeric flags after the optional filename, e.g. `# 1 "file" 2 3`.  Those
  // flags are not part of standard `#line` and do not change the logical
  // file/line state modeled here, so accept them only for the numeric form.
  if (!usedLineKeyword) {
    while (p < to) {
      if (!std::isdigit(static_cast<unsigned char>(src[p])))
        break;
      while (p < to && std::isdigit(static_cast<unsigned char>(src[p])))
        ++p;
      while (p < to && stringutils::isWsNoLF(src[p]))
        ++p;
    }
  }

  if (p != to)
    return std::nullopt;

  const size_t afterDirectiveIdx =
      (to < src.size() && src[to] == '\n') ? to + 1 : to;

  return LineDirectiveState(std::string(fileSpelling.str()), lineAfter,
                            afterDirectiveIdx, hasFileSpelling);
}

} // namespace

LineDirectiveInserter::LineDirectiveInserter(bool enabled, StringRef cwd)
    : enabled_(enabled), cwd_(cwd) {}

std::string LineDirectiveInserter::ToAbsolutePath(StringRef spelledPath) const {
  llvm::SmallString<256> path(spelledPath);

  if (!llvm::sys::path::is_absolute(path)) {
    if (cwd_.empty()) {
      llvm::sys::fs::make_absolute(path);
    } else {
      // Use the preprocessor working directory captured for this refold run
      // instead of the process CWD, which may differ during replay/testing.
      llvm::SmallString<256> base(cwd_);
      llvm::sys::path::append(base, path);
      path = base;
    }
  }

  llvm::sys::path::remove_dots(path, /*remove_dot_dot=*/true);
  return std::string(path.str());
}

static bool rejoinsUntouchedTailSafelyAtBOL(StringRef originalFileText,
                                            uint64_t editEnd) {
  const size_t n = originalFileText.size();
  const size_t pos =
      (editEnd >= static_cast<uint64_t>(n)) ? n : static_cast<size_t>(editEnd);

  if (pos == n || stringutils::isBOL(originalFileText, pos))
    return true;

  size_t nl = originalFileText.find('\n', pos);
  if (nl == StringRef::npos)
    nl = n;
  return stringutils::isIndentOnly(originalFileText, pos, nl);
}

static std::string insertLineDirectiveAt(StringRef replacement,
                                         StringRef directive, size_t offset) {
  std::string res = replacement.substr(0, offset).str();
  res += directive;
  res += replacement.substr(offset);
  return res;
}

static std::optional<size_t> findCarriedSuffixPrefixInsertionOffset(
    StringRef originalFileText, uint64_t s, uint64_t e, StringRef replacement,
    size_t replacementPrefixOffset, bool requireDeletedLineFromBOL) {
  if (e > originalFileText.size() ||
      replacementPrefixOffset >= replacement.size())
    return std::nullopt;

  const size_t editBegin = static_cast<size_t>(s);
  const size_t editEnd = static_cast<size_t>(e);
  const size_t resumePrefixBegin =
      stringutils::lineStartOffset(originalFileText, editEnd);
  if (resumePrefixBegin >= editEnd)
    return std::nullopt;

  if (resumePrefixBegin != 0 &&
      stringutils::isLineSplice(originalFileText, resumePrefixBegin - 1))
    return std::nullopt;

  if (requireDeletedLineFromBOL) {
    if (editBegin > resumePrefixBegin)
      return std::nullopt;
    // The directive is inserted at the front of the replacement, so it needs to
    // begin a logical line -- not to begin a physical one.  An edit that starts
    // just past its line's indentation leaves that indentation copied verbatim
    // ahead of the directive, and horizontal whitespace before `#` still
    // introduces a directive.  Requiring a strict BOL here refuses the
    // insertion point whenever the aligner anchors the edit after the indent,
    // which leaves the repair with no placement before the observers it
    // protects.
    if (!stringutils::beginsLineAfterWs(originalFileText, editBegin))
      return std::nullopt;
    if (stringutils::countNonSplicedNewlines(originalFileText, editBegin,
                                             resumePrefixBegin) == 0)
      return std::nullopt;
  }

  StringRef originalResumePrefix =
      originalFileText.slice(resumePrefixBegin, editEnd);
  if (originalResumePrefix.empty())
    return std::nullopt;

  // The replacement tail must carry the same tokens as the original prefix it
  // stands in for, but not necessarily the same indentation: the edited stream
  // routinely respells leading whitespace, and requiring byte identity would
  // refuse this insertion point whenever it does.  Only the *leading* run is
  // ignored -- the trailing run separates the carried prefix from the untouched
  // bytes resumed at `editEnd`, so it must still match exactly.
  //
  // Ignoring indentation here cannot change the emitted bytes.  This predicate
  // only chooses where the directive is spliced into a replacement that is
  // emitted either way, and the splice point is a beginning-of-line, so the
  // carried prefix keeps its own indentation on the line the directive
  // introduces.
  StringRef replacementTail = replacement.substr(replacementPrefixOffset);
  if (stringutils::trimLeadingWsNoLF(replacementTail) !=
      stringutils::trimLeadingWsNoLF(originalResumePrefix))
    return std::nullopt;
  return replacementPrefixOffset;
}

std::string LineDirectiveInserter::MaybeAppendResyncAfterReplacement(
    StringRef originalFileText, uint64_t s, uint64_t e, StringRef replacement,
    const LineDirectiveLocation &resumeLoc) const {
  if (!enabled_)
    return replacement.str();

  const size_t resumeLine = resumeLoc.lineNo;
  StringRef fileSpellingForDirective(resumeLoc.fileSpelling);

  // No line directive is needed when the replacement preserves the original
  // physical newline count across the edited byte range.
  size_t origNl = stringutils::countNewlines(originalFileText, s, e);
  size_t replNl = stringutils::countNewlines(replacement);

  if (origNl == replNl) {
    return replacement.str();
  }

  // The resume directive points at the logical source location where the
  // untouched suffix begins after the replacement, not merely at its physical
  // source line.
  std::string directive =
      FormatLineDirective(resumeLine, fileSpellingForDirective);

  // Empty replacements can only be replaced by a bare directive when both sides
  // of the deletion are line-safe. Otherwise the directive would be injected
  // into the middle of an existing physical line.
  if (replacement.empty()) {
    if (!stringutils::isBOL(originalFileText, static_cast<size_t>(s)) ||
        !rejoinsUntouchedTailSafelyAtBOL(originalFileText, e)) {
      return replacement.str();
    }

    return directive;
  }

  // Token-LCS normalization can express a line deletion as replacing the
  // deleted line plus the first token(s) of the surviving suffix line with the
  // same suffix-line prefix.  If the replacement is exactly that carried
  // prefix, the directive belongs before the replacement because the next
  // untouched slice resumes mid-line.
  if (std::optional<size_t> offset = findCarriedSuffixPrefixInsertionOffset(
          originalFileText, s, e, replacement, /*replacementPrefixOffset=*/0,
          /*requireDeletedLineFromBOL=*/true)) {
    return insertLineDirectiveAt(replacement, directive, *offset);
  }

  // If the replacement already ends at BOL, append the directive after it. The
  // untouched original tail must also rejoin safely at a line boundary.
  if (replacement.back() == '\n') {
    if (!rejoinsUntouchedTailSafelyAtBOL(originalFileText, e)) {
      return replacement.str();
    }

    // Idempotence: avoid appending the same directive twice when this helper is
    // reached repeatedly for an already-resynced replacement.
    if (replacement.ends_with(directive)) {
      return replacement.str();
    }
    return replacement.str() + directive;
  }

  // Otherwise, the remaining safe insertion points are inside the replacement,
  // immediately before bytes that are proved to be a carried prefix of the
  // untouched original suffix line, or before a trailing indentation-only
  // suffix.
  size_t lastNl = replacement.rfind('\n');
  if (lastNl != StringRef::npos) {
    size_t bol = lastNl + 1;

    if (!stringutils::isLineSplice(replacement, lastNl)) {
      if (std::optional<size_t> offset = findCarriedSuffixPrefixInsertionOffset(
              originalFileText, s, e, replacement, bol,
              /*requireDeletedLineFromBOL=*/false)) {
        // Idempotence: if the prefix is already preceded by this exact
        // directive, do not duplicate it.
        if (replacement.substr(0, *offset).ends_with(directive)) {
          return replacement.str();
        }

        return insertLineDirectiveAt(replacement, directive, *offset);
      }

      if (stringutils::isIndentOnly(replacement, bol, replacement.size())) {
        // The directive is inserted *before* the replacement's trailing
        // indentation, so the untouched original tail is emitted on its own
        // physical line immediately after the directive.  Its logical line is
        // therefore exactly `resumeLine`, whether or not that tail began at a
        // BOL in the original file.
        //
        // `rejoinsUntouchedTailSafelyAtBOL()` is the precondition for
        // *appending* a directive after a replacement, where the untouched tail
        // keeps its original physical line; it is not an obligation for this
        // insertion point, which gives the tail a fresh line either way.
        //
        // Discharging the resync locally is what keeps the repair ordered
        // before the observers it protects: a refused injection can only be
        // deferred to the next safe BOL in the emitted output, and that BOL may
        // lie after a preserved `__LINE__` observer the drift already moved.
        //
        // Idempotence: if the prior line is already the same directive, do not
        // emit it again before the indentation-only suffix.
        if (replacement.substr(0, bol).ends_with(directive)) {
          return replacement.str();
        }

        return insertLineDirectiveAt(replacement, directive, bol);
      }
    }

    // There is a newline, but the tail after it contains substantive text. A
    // directive inserted there would split replacement text rather than cleanly
    // resume the original file.
  } else {
    // With no newline in the replacement, there is no BOL insertion point for
    // the directive.
  }

  return replacement.str();
}

std::optional<LineDirectiveState>
LineDirectiveInserter::FindLastLineDirectiveState(StringRef src) {
  if (src.empty())
    return std::nullopt;

  // Scan the full original-prefix text.  This is intentionally not a bounded
  // lookback: source-authored line-control directives establish semantic
  // preprocessor state, so missing an old directive can make `__LINE__` or
  // `__FILE__` replay wrong.
  size_t scanEnd = src.size();
  while (scanEnd > 0 && src[scanEnd - 1] == '\n')
    --scanEnd;

  while (scanEnd > 0) {
    size_t prevNl = stringutils::lastIndexOfChar(src, '\n', scanEnd - 1);
    size_t lineStart = (prevNl == StringRef::npos) ? 0 : prevNl + 1;

    if (auto st = parseLineDirectiveForLineControl(src, lineStart, scanEnd))
      return st;

    if (prevNl == StringRef::npos)
      break;

    scanEnd = prevNl;
    while (scanEnd > 0 && src[scanEnd - 1] == '\n')
      --scanEnd;
  }
  return std::nullopt;
}

std::string LineDirectiveInserter::EscapeForLineDirective(StringRef path) {
  return stringutils::escapeLineDirectivePath(path);
}

} // namespace refold
} // namespace clang
