#include "LineDirectiveInserter.h"
#include "RefoldLog.h"
#include "StringUtils.h"
#include <algorithm>
#include <cctype>
#include <llvm/ADT/SmallString.h>
#include <llvm/ADT/StringRef.h>
#include <llvm/Support/FileSystem.h>

using namespace llvm;

namespace clang {
namespace refold {

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

LineDirectiveLocation LineDirectiveInserter::LogicalLocationAtOffset(
    StringRef src, uint64_t offset, StringRef defaultFileSpelling) {
  const size_t clampedOffset =
      static_cast<size_t>(std::min<uint64_t>(offset, src.size()));
  StringRef prefix = src.take_front(clampedOffset);

  // Recover the active source-authored line-control state by scanning the
  // complete source prefix in source order.  A backward search is insufficient
  // for `#line <n>` with no filename operand, because that directive preserves
  // the previously active logical file.
  std::optional<std::string> activeFile;
  size_t activeLineAfterDirective = 0;
  size_t activeAfterDirectiveIdx = 0;
  bool sawLineDirective = false;

  for (size_t lineStart = 0; lineStart < prefix.size();) {
    size_t lineEnd = prefix.find('\n', lineStart);
    if (lineEnd == StringRef::npos)
      lineEnd = prefix.size();

    if (std::optional<LineDirectiveState> state =
            ParseLineDirective(prefix, lineStart, lineEnd)) {
      sawLineDirective = true;
      if (state->hasFileSpelling)
        activeFile = state->fileSpelling;
      activeLineAfterDirective = state->lineAfterDir;
      activeAfterDirectiveIdx = state->afterDirIdx;
    }

    if (lineEnd == prefix.size())
      break;
    lineStart = lineEnd + 1;
  }

  if (sawLineDirective) {
    const size_t delta = stringutils::countNonSplicedNewlines(
        prefix, activeAfterDirectiveIdx, prefix.size());
    StringRef file = activeFile ? StringRef(*activeFile) : defaultFileSpelling;
    return LineDirectiveLocation(file, activeLineAfterDirective + delta);
  }

  return LineDirectiveLocation(defaultFileSpelling,
                               stringutils::lineAtOffset(src, clampedOffset));
}

std::string LineDirectiveInserter::WrapIncludeExpansion(
    StringRef childFileSpelling, StringRef parentFileSpelling,
    size_t parentResumeLineNo, StringRef childBody) const {
  if (!enabled_)
    return childBody.str();

  std::string result;
  result.reserve(childBody.size() + 128);

  result += FormatLineDirective(1, childFileSpelling);
  result += childBody.str();

  // Ensure the resume directive begins on its own line even when the included
  // body did not end with a newline.
  if (!childBody.empty() && childBody.back() != '\n') {
    result += '\n';
  }

  result += FormatLineDirective(parentResumeLineNo, parentFileSpelling);
  return result;
}

std::string LineDirectiveInserter::MaybeAppendResyncAfterReplacement(
    StringRef originalFileText, uint64_t s, uint64_t e, StringRef replacement,
    const LineDirectiveLocation &resumeLoc) const {
  if (!enabled_)
    return replacement.str();

  const size_t resumeLine = resumeLoc.lineNo;
  StringRef fileSpellingForDirective(resumeLoc.fileSpelling);

  // A resync directive is safe only if it rejoins the untouched original file
  // at a physical line boundary. We also allow rejoining before indentation-only
  // tail bytes, because the directive can be inserted before that indentation
  // without being stranded in the middle of a source line.
  auto rejoinsUntouchedTailSafelyAtBOL = [&](uint64_t editEnd) -> bool {
    const size_t n = originalFileText.size();
    const size_t pos = (editEnd >= static_cast<uint64_t>(n))
                           ? n
                           : static_cast<size_t>(editEnd);

    if (pos == n || stringutils::isBOL(originalFileText, pos))
      return true;

    size_t nl = originalFileText.find('\n', pos);
    if (nl == StringRef::npos)
      nl = n;

    return stringutils::isIndentOnly(originalFileText, pos, nl);
  };

  // No line directive is needed when the replacement preserves the original
  // physical newline count across the edited byte range.
  size_t origNl = stringutils::countNewlines(originalFileText, s, e);
  size_t replNl = stringutils::countNewlines(replacement);

  if (origNl == replNl) {
    trace("linedir/local",
          "skip (no newline drift): s={0} e={1} origNl={2} replNl={3} file={4}",
          s, e, origNl, replNl, fileSpellingForDirective);
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
        !rejoinsUntouchedTailSafelyAtBOL(e)) {
      trace("linedir/local",
            "cannot inject (empty replacement rejoins mid-line original): "
            "resumeLine={0} file={1} start={2} end={3}",
            resumeLine, fileSpellingForDirective, s, e);
      return replacement.str();
    }

    trace("linedir/local",
          "inject (empty replacement): resumeLine={0} file={1}", resumeLine,
          fileSpellingForDirective);
    return directive;
  }

  // If the replacement already ends at BOL, append the directive after it. The
  // untouched original tail must also rejoin safely at a line boundary.
  if (replacement.back() == '\n') {
    if (!rejoinsUntouchedTailSafelyAtBOL(e)) {
      trace("linedir/local",
            "cannot inject at end (replacement rejoins mid-line original): "
            "resumeLine={0} file={1} end={2}",
            resumeLine, fileSpellingForDirective, e);
      return replacement.str();
    }

    // Idempotence: avoid appending the same directive twice when this helper is
    // reached repeatedly for an already-resynced replacement.
    if (replacement.ends_with(directive)) {
      trace("linedir/local",
            "skip (already endsWith directive): resumeLine={0} file={1}",
            resumeLine, fileSpellingForDirective);
      return replacement.str();
    }
    trace("linedir/local", "inject (append at end): resumeLine={0} file={1}",
          resumeLine, fileSpellingForDirective);
    return replacement.str() + directive;
  }

  // Otherwise, the remaining safe insertion points are inside the
  // replacement, immediately before bytes that are proved to be a carried
  // prefix of the untouched original suffix line, or before a trailing
  // indentation-only suffix.
  size_t lastNl = replacement.rfind('\n');
  if (lastNl != StringRef::npos) {
    size_t bol = lastNl + 1;

    // Some token-hunk repairs deliberately absorb the first bytes of the
    // untouched suffix line into the replacement so that lexical adjacency is
    // preserved.  Example shape:
    //
    //     replacement: "...\nint"
    //     original[e:]: " keep = ..."
    //
    // The final "int" is not new edited payload; it is the byte-for-byte
    // prefix of the original suffix line [lineStart(e), e).  If the edit also
    // removed physical lines before that suffix, the correct local resync point
    // is before the carried prefix, not after the entire replacement.  This is
    // a proof, not a formatting preference: the replacement suffix must exactly
    // equal the original line prefix that will be rejoined with original[e:].
    if (e <= originalFileText.size() && bol < replacement.size() &&
        !stringutils::isLineSplice(replacement, lastNl)) {
      const size_t resumePrefixBegin = stringutils::lineStartOffset(
          originalFileText, static_cast<size_t>(e));
      const bool resumePrefixStartsLogicalLine =
          resumePrefixBegin == 0 ||
          !stringutils::isLineSplice(originalFileText, resumePrefixBegin - 1);
      if (resumePrefixBegin < e && resumePrefixStartsLogicalLine) {
        StringRef originalResumePrefix =
            originalFileText.slice(resumePrefixBegin, static_cast<size_t>(e));
        StringRef replacementResumePrefix = replacement.substr(bol);
        if (!originalResumePrefix.empty() &&
            replacementResumePrefix == originalResumePrefix) {
          trace("linedir/local",
                "inject (before carried suffix prefix): resumeLine={0} "
                "file={1} lastNl={2} bol={3} prefix={4}",
                resumeLine, fileSpellingForDirective, lastNl, bol,
                stringutils::showWs(stringutils::clip(replacementResumePrefix,
                                                     80)));

          // Idempotence: if the prefix is already preceded by this exact
          // directive, do not duplicate it.
          if (replacement.substr(0, bol).ends_with(directive)) {
            trace("linedir/local",
                  "skip (directive already present before carried suffix "
                  "prefix): resumeLine={0} file={1}",
                  resumeLine, fileSpellingForDirective);
            return replacement.str();
          }

          std::string res = replacement.substr(0, bol).str();
          res += directive;
          res += replacement.substr(bol).str();
          return res;
        }
      }
    }

    if (stringutils::isIndentOnly(replacement, bol, replacement.size())) {
      if (!rejoinsUntouchedTailSafelyAtBOL(e)) {
        trace("linedir/local",
              "cannot inject before indent-only suffix (replacement rejoins "
              "mid-line original): resumeLine={0} file={1} end={2}",
              resumeLine, fileSpellingForDirective, e);
        return replacement.str();
      }

      trace("linedir/local",
            "inject (between last NL and indent-only suffix): resumeLine={0} "
            "file={1} lastNl={2} bol={3}",
            resumeLine, fileSpellingForDirective, lastNl, bol);

      // Idempotence: if the prior line is already the same directive, do not
      // emit it again before the indentation-only suffix.
      if (replacement.substr(0, bol).ends_with(directive)) {
        trace("linedir/local",
              "skip (directive already present immediately before indent-only "
              "suffix): resumeLine={0} file={1}",
              resumeLine, fileSpellingForDirective);
        return replacement.str();
      }

      std::string res = replacement.substr(0, bol).str();
      res += directive;
      res += replacement.substr(bol).str();
      return res;
    }

    // There is a newline, but the tail after it contains substantive text. A
    // directive inserted there would split replacement text rather than cleanly
    // resume the original file.
    if (inTraceMode()) {
      const size_t start =
          static_cast<size_t>(std::clamp(bol, size_t(0), replacement.size()));
      StringRef replFromBol = replacement.substr(start);
      trace("linedir/local",
            "cannot inject at tail (non-indent suffix): resumeLine={0} "
            "file={1} lastNl={2} tail={3}",
            resumeLine, fileSpellingForDirective, lastNl,
            stringutils::showWs(stringutils::clip(replFromBol, 80)));
    }
  } else {
    // With no newline in the replacement, there is no BOL insertion point for
    // the directive.
    trace("linedir/local",
          "cannot inject (no newline in replacement): resumeLine={0} file={1} "
          "replTail={2}",
          resumeLine, fileSpellingForDirective,
          stringutils::showWs(stringutils::clip(replacement, 80)));
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

    if (auto st = ParseLineDirective(src, lineStart, scanEnd))
      return st;

    if (prevNl == StringRef::npos)
      break;

    scanEnd = prevNl;
    while (scanEnd > 0 && src[scanEnd - 1] == '\n')
      --scanEnd;
  }
  return std::nullopt;
}

std::optional<LineDirectiveState>
LineDirectiveInserter::ParseLineDirective(StringRef src, size_t from,
                                          size_t to) {
  if (from > to || to > src.size())
    return std::nullopt;

  size_t p = from;

  // A preprocessing directive may be preceded by horizontal whitespace.  Do
  // not cross a physical newline; `from/to` already delimit one source line.
  while (p < to && stringutils::isWs(src[p]) && src[p] != '\n')
    ++p;

  if (p >= to || src[p] != '#')
    return std::nullopt;
  ++p;

  // Both `#line` and `# line` are accepted spellings.  Clang/GCC also accept
  // the numeric line-control form `# 123 "file"`, so leave `p` at the digits
  // when there is no `line` keyword.
  while (p < to && stringutils::isWs(src[p]) && src[p] != '\n')
    ++p;

  if (p + 4 <= to && src.substr(p, 4) == "line") {
    p += 4;
    if (p >= to || !stringutils::isWs(src[p]))
      return std::nullopt;
    while (p < to && stringutils::isWs(src[p]) && src[p] != '\n')
      ++p;
  }

  const size_t lineStart = p;
  while (p < to && std::isdigit(static_cast<unsigned char>(src[p])))
    ++p;

  if (p == lineStart)
    return std::nullopt;

  size_t lineAfter;
  if (src.slice(lineStart, p).getAsInteger(10, lineAfter))
    return std::nullopt;

  while (p < to && stringutils::isWs(src[p]) && src[p] != '\n')
    ++p;

  // Parse the optional quoted filename operand.  Absence of this operand is
  // semantically meaningful: `#line 200` changes only the logical line number
  // and preserves the active logical file.
  llvm::SmallString<64> fileSpelling;
  bool hasFileSpelling = false;
  if (p < to && src[p] == '"') {
    hasFileSpelling = true;
    ++p; // consume opening quote
    while (p < to) {
      char c = src[p++];
      if (c == '"')
        break;
      if (c == '\\' && p < to) {
        char escaped = src[p++];
        switch (escaped) {
        case '"':
        case '\\':
          fileSpelling.push_back(escaped);
          break;
        case 'a':
          fileSpelling.push_back('\a');
          break;
        case 'b':
          fileSpelling.push_back('\b');
          break;
        case 'e':
        case 'E':
          fileSpelling.push_back(static_cast<char>(0x1b));
          break;
        case 'f':
          fileSpelling.push_back('\f');
          break;
        case 'n':
          fileSpelling.push_back('\n');
          break;
        case 'r':
          fileSpelling.push_back('\r');
          break;
        case 't':
          fileSpelling.push_back('\t');
          break;
        case 'v':
          fileSpelling.push_back('\v');
          break;
        default:
          fileSpelling.push_back(escaped);
          break;
        }
      } else {
        fileSpelling.push_back(c);
      }
    }
  }

  const size_t afterDirectiveIdx =
      (to < src.size() && src[to] == '\n') ? to + 1 : to;

  return LineDirectiveState(std::string(fileSpelling.str()), lineAfter,
                            afterDirectiveIdx, hasFileSpelling);
}

std::string LineDirectiveInserter::EscapeForLineDirective(StringRef path) {
  if (path.empty())
    return "";

  llvm::SmallString<64> escaped;
  escaped.reserve(path.size());
  for (char c : path) {
    switch (c) {
    case '\\':
      // #line filenames are emitted inside double quotes, so preserve a literal
      // backslash by escaping it in the directive spelling.
      escaped.append("\\\\");
      break;
    case '\"':
      // Keep embedded quotes from terminating the quoted filename.
      escaped.append("\\\"");
      break;
    case '\a':
      escaped.append("\\a");
      break;
    case '\b':
      escaped.append("\\b");
      break;
    case '\f':
      escaped.append("\\f");
      break;
    case '\n':
      // Never place a physical newline inside a #line filename; spell the
      // logical filename byte as a C escape so the directive remains one
      // preprocessing line.
      escaped.append("\\n");
      break;
    case '\r':
      escaped.append("\\r");
      break;
    case '\t':
      escaped.append("\\t");
      break;
    case '\v':
      escaped.append("\\v");
      break;
    case static_cast<char>(0x1b):
      // Clang accepts both \e and \E; use one canonical spelling when replaying
      // an escape byte recovered from a source-only line-control filename.
      escaped.append("\\e");
      break;
    default:
      escaped.push_back(c);
      break;
    }
  }

  return std::string(escaped.str());
}

} // namespace refold
} // namespace clang
