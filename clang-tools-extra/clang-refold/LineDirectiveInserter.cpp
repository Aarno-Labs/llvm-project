#include "LineDirectiveInserter.h"
#include "RefoldLog.h"
#include "StringUtils.h"
#include <algorithm>
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
      llvm::SmallString<256> base(cwd_);
      llvm::sys::path::append(base, path);
      path = base;
    }
  }

  llvm::sys::path::remove_dots(path, /*remove_dot_dot=*/true);
  return std::string(path.str());
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

  if (!childBody.empty() && childBody.back() != '\n') {
    result += '\n';
  }

  result += FormatLineDirective(parentResumeLineNo, parentFileSpelling);
  return result;
}

std::string LineDirectiveInserter::MaybeAppendResyncAfterReplacement(
    StringRef originalFileText, uint64_t s, uint64_t e, StringRef replacement,
    StringRef fileSpellingForDirective) const {
  if (!enabled_)
    return replacement.str();

  size_t origNl = stringutils::countNewlines(originalFileText, s, e);
  size_t replNl = stringutils::countNewlines(replacement);

  if (origNl == replNl) {
    trace("linedir/local",
          "skip (no newline drift): s={0} e={1} origNl={2} replNl={3} file={4}",
          s, e, origNl, replNl, fileSpellingForDirective);
    return replacement.str();
  }

  size_t resumeLine = stringutils::lineAtOffset(originalFileText, e);
  std::string directive =
      FormatLineDirective(resumeLine, fileSpellingForDirective);

  // Ensure directive begins at BOL in the emitted output.
  if (replacement.empty()) {
    trace("linedir/local",
          "inject (empty replacement): resumeLine={0} file={1}", resumeLine,
          fileSpellingForDirective);
    return directive;
  }

  if (replacement.back() == '\n') {
    // Idempotence: avoid appending the same directive twice.
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

  size_t lastNl = replacement.rfind('\n');
  if (lastNl != StringRef::npos) {
    size_t bol = lastNl + 1;
    if (stringutils::isIndentOnly(replacement, bol, replacement.size())) {
      trace("linedir/local",
            "inject (between last NL and indent-only suffix): resumeLine={0} "
            "file={1} lastNl={2} bol={3}",
            resumeLine, fileSpellingForDirective, lastNl, bol);

      // Idempotence: if the prior line is already the same directive, don't
      // emit it again.
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

    if (inTraceMode()) {
      const size_t start =
          static_cast<size_t>(std::clamp(bol, size_t(0), replacement.size()));
      StringRef replFromBol = replacement.substr(start);
      trace("linedir/local",
            "cannot inject at tail (non-indent suffix): resumeLine={0} "
            "file={1} lastNl={2} tail={3}",
            resumeLine, fileSpellingForDirective, lastNl,
            stringutils::showWS(stringutils::clip(replFromBol, 80)));
    }
  } else {
    trace("linedir/local",
          "cannot inject (no newline in replacement): resumeLine={0} file={1} "
          "replTail={2}",
          resumeLine, fileSpellingForDirective,
          stringutils::showWS(stringutils::clip(replacement, 80)));
  }

  return replacement.str();
}

std::optional<LineDirectiveState>
LineDirectiveInserter::FindLastLineDirectiveState(StringRef src) {
  if (src.empty())
    return std::nullopt;

  const size_t lookback = 16384;
  const size_t end = src.size();
  const size_t min = (end > lookback) ? (end - lookback) : 0;

  // Skip trailing newlines at the end of the buffer/range.
  size_t scanEnd = end;
  while (scanEnd > min && src[scanEnd - 1] == '\n')
    scanEnd--;

  while (scanEnd > min) {
    // Search backward from the character before the current scanEnd.
    size_t prevNl = stringutils::lastIndexOfChar(src, '\n', scanEnd - 1);
    size_t lineStart = (prevNl == StringRef::npos) ? 0 : prevNl + 1;

    if (stringutils::startsWith(src, lineStart, "#line")) {
      auto st = ParseLineDirective(src, lineStart, scanEnd);
      if (st)
        return st;
    }

    if (prevNl == StringRef::npos)
      break;

    // Move to newline and skip consecutive newlines
    scanEnd = prevNl;
    while (scanEnd > min && src[scanEnd - 1] == '\n')
      scanEnd--;
  }
  return std::nullopt;
}

std::optional<LineDirectiveState>
LineDirectiveInserter::ParseLineDirective(StringRef src, size_t from,
                                          size_t to) {
  // 1. Initial Prefix Check
  if (from + 5 > src.size() || !stringutils::startsWith(src, from, "#line"))
    return std::nullopt;

  size_t p = from + 5;

  // 2. Strict Whitespace Check (Enforce at least one whitespace char after
  // #line)
  if (p >= to || !stringutils::isWs(src[p]))
    return std::nullopt;

  // Skip remaining whitespace
  while (p < to && stringutils::isWs(src[p]))
    p++;

  // 3. Parse Line Number
  size_t lineStart = p;
  while (p < to && isdigit(src[p]))
    p++;

  if (p == lineStart)
    return std::nullopt;

  size_t lineAfter;
  if (src.slice(lineStart, p).getAsInteger(10, lineAfter))
    return std::nullopt;

  // 4. Skip Whitespace after number
  while (p < to && stringutils::isWs(src[p]))
    p++;

  // 5. Parse Quoted File Spelling with Escaping
  llvm::SmallString<64> fileSpelling;
  if (p < to && src[p] == '"') {
    p++; // consume opening quote
    while (p < to) {
      char c = src[p++];
      if (c == '"')
        break; // closing quote
      if (c == '\\' && p < to) {
        fileSpelling.push_back(src[p++]);
      } else {
        fileSpelling.push_back(c);
      }
    }
  }

  // 6. Calculate afterDirectiveIdx (Global context)
  size_t afterDirectiveIdx = to;
  if (to < src.size() && src[to] == '\n') {
    afterDirectiveIdx = to + 1;
  }

  return LineDirectiveState(std::string(fileSpelling.str()), lineAfter,
                            afterDirectiveIdx);
}

std::string LineDirectiveInserter::EscapeForLineDirective(StringRef path) {
  if (path.empty())
    return "";

  llvm::SmallString<64> escaped;
  escaped.reserve(path.size());
  for (char c : path) {
    if (c == '\\') {
      escaped.append("\\\\");
    } else if (c == '\"') {
      escaped.append("\\\"");
    } else {
      escaped.push_back(c);
    }
  }

  // Convert to std::string. This creates the heap-allocated string once.
  return std::string(escaped.str());
}

} // namespace refold
} // namespace clang
