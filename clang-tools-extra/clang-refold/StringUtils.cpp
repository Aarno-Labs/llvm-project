//===--- StringUtils.cpp ----------------------------------------*- C++ -*-===//
//
// ASCII string utilities for refolding and token-boundary hygiene.
//
// Overview
// --------
// A small collection of deterministic, ASCII-only helpers used throughout the
// refolding pipeline. These routines mirror the C/C++ preprocessor’s notion of
// whitespace and identifier characters and intentionally avoid locale/Unicode-
// dependent behavior to keep results stable across platforms and toolchains.
//  
// What’s here
// -----------
//  • Whitespace predicates:
//      - isWs(char)                  : PP whitespace (space, HT, LF, VT, FF,
//      CR)
//      - isAsciiWhitespace(StringRef): true iff every char is PP whitespace
//  • Identifier predicates (ASCII):
//      - isAsciiAlpha(char), isAsciiDigit(char)
//      - isAsciiIdentStart(char)     : '_' or ASCII letter
//      - isAsciiIdentChar(char)      : start-char or digit
//      - isIdentChar(char)           : same as isAsciiIdentChar (for glue
//      checks)
//      - isIdentifierOnly(StringRef) : non-empty, first not digit, all ident
//      chars
//  • Index scans:
//      - firstNonWsIdx(StringRef)    : first non-WS index or -1
//      - lastNonWsIdx(StringRef)     : last  non-WS index or -1
//  • Debug formatting helpers:
//      - showWS(StringRef)           : visualize whitespace (·, \t, \n, \r, \f,
//      \v)
//      - clip(StringRef, int)        : truncate with length suffix
//  
// Design notes
// ------------
//  • ASCII by construction: no std::is* or Unicode categories.
//  • Deterministic and side-effect free; O(1) or O(n) time.
//  • Used for boundary padding decisions to avoid token “glue” without a full
//    lexer (maximal-munch hygiene).
//
// Usage
// -----
//   if (isAsciiWhitespace(S)) { … }
//   bool glue = isIdentChar(L) && isIdentChar(R);  // conservative boundary
//   check int f = firstNonWsIdx(Txt);                    // -1 if all
//   whitespace 
//  
// Policy
// ------
//  • Keep helpers minimal and header-only for inlining.
//  • Behavior must not depend on locale or environment.
//
// Author:               
//   jeikenberry 
//
//===----------------------------------------------------------------------===//

#include "StringUtils.h"
#include "llvm/ADT/StringRef.h"

#include <cstdio>
#include <string>

namespace clang {
namespace refold {
namespace stringutils {

bool isIdentifierOnly(StringRef s) {
  s = s.trim();
  if (s.empty())
    return false;
  if (!isIdentStart(s.front()))
    return false;
  for (char ch : s.drop_front()) {
    if (!isIdentPart(ch))
      return false;
  }
  return true;
}

bool isIdentifierOrSimpleCallExpr(StringRef replacement) {
  StringRef s = replacement.trim();
  if (s.empty())
    return false;

  if (stringutils::isIdentifierOnly(stringutils::trimEdgeSpaces(s)))
    return true;

  // Accept the common "IDENT(...)" shape (with balanced parens and only
  // trailing whitespace).
  size_t i = 0;
  if (!stringutils::isIdentStart(s[i]))
    return false;
  ++i;
  while (i < s.size() && stringutils::isIdentPart(s[i]))
    ++i;

  i = stringutils::skipWSAndComments(s, i);
  if (i >= s.size() || s[i] != '(')
    return false;

  size_t rparen = stringutils::findMatchingRParen(s, i);
  if (rparen == StringRef::npos)
    return false;

  i = stringutils::skipWSAndComments(s, rparen + 1);
  return i == s.size();
}

size_t skipWSAndComments(StringRef s, size_t i) {
  const size_t n = s.size();
  while (i < n) {
    if (isWs(s[i])) {
      ++i;
      continue;
    }
    if (s[i] == '/' && i + 1 < n) {
      if (s[i + 1] == '/') {
        i += 2;
        while (i < n && s[i] != '\n')
          ++i;
        continue;
      }
      if (s[i + 1] == '*') {
        i += 2;
        while (i + 1 < n && !(s[i] == '*' && s[i + 1] == '/'))
          ++i;
        if (i + 1 < n)
          i += 2;
        continue;
      }
    }
    break;
  }
  return i;
}

size_t findMatchingRParen(StringRef s, size_t lParenIdx) {
  const size_t n = s.size();
  if (lParenIdx >= n || s[lParenIdx] != '(')
    return StringRef::npos;

  unsigned depth = 0;
  for (size_t i = lParenIdx; i < n; ++i) {
    char c = s[i];

    // Skip comments.
    if (c == '/' && i + 1 < n) {
      if (s[i + 1] == '/') {
        i += 2;
        while (i < n && s[i] != '\n')
          ++i;
        if (i >= n)
          break;
        continue;
      }
      if (s[i + 1] == '*') {
        i += 2;
        while (i + 1 < n && !(s[i] == '*' && s[i + 1] == '/'))
          ++i;
        if (i + 1 < n)
          i += 1; // loop will ++i
        continue;
      }
    }

    // Skip string/char literals (simple escape-aware scan).
    if (c == '"' || c == '\'') {
      const char quote = c;
      ++i;
      while (i < n) {
        char q = s[i];
        if (q == '\\') {
          if (i + 1 < n)
            i += 2;
          else
            ++i;
          continue;
        }
        if (q == quote)
          break;
        ++i;
      }
      continue;
    }

    if (c == '(') {
      ++depth;
      continue;
    }
    if (c == ')') {
      if (depth == 0)
        return StringRef::npos;
      --depth;
      if (depth == 0)
        return i;
      continue;
    }
  }
  return StringRef::npos;
}

StringRef trimEdgeSpaces(StringRef s) {
  size_t lo = 0;
  size_t hi = s.size();

  while (lo < hi) {
    char c = s[lo];
    if (c != ' ' && c != '\t')
      break;
    ++lo;
  }

  while (hi > lo) {
    char c = s[hi - 1];
    if (c != ' ' && c != '\t')
      break;
    --hi;
  }

  if (lo == 0 && hi == s.size())
    return s; // no trimming needed; return the original string ref

  return s.substr(lo, hi - lo); // return trimmed portion
}

std::vector<StringRef> splitChars(StringRef s) {
  std::vector<StringRef> refs;
  refs.reserve(s.size());
  for (size_t i = 0; i < s.size(); ++i)
    refs.push_back(s.substr(i, 1));
  return refs;
}

std::optional<std::string> canonicalizeStringifyInversePayload(StringRef raw0) {
  StringRef raw = raw0.trim();

  enum class LexState { Normal, String, Char };
  LexState state = LexState::Normal;

  std::string out;
  out.reserve(raw.size());

  bool pendingSpace = false;
  auto flushPendingSpace = [&]() {
    if (pendingSpace && !out.empty())
      out.push_back(' ');
    pendingSpace = false;
  };

  for (size_t i = 0; i < raw.size(); ++i) {
    const char c = raw[i];

    switch (state) {
    case LexState::Normal:
      if (isWs(c)) {
        pendingSpace = !out.empty();
        continue;
      }

      if (c == '/' && i + 1 < raw.size()) {
        const char n = raw[i + 1];
        if (n == '/' || n == '*')
          return std::nullopt;
      }

      flushPendingSpace();
      out.push_back(c);
      if (c == '"')
        state = LexState::String;
      else if (c == '\'')
        state = LexState::Char;
      continue;

    case LexState::String:
      out.push_back(c);
      if (c == '\\') {
        if (i + 1 >= raw.size())
          return std::nullopt;
        out.push_back(raw[++i]);
        continue;
      }
      if (c == '"')
        state = LexState::Normal;
      continue;

    case LexState::Char:
      out.push_back(c);
      if (c == '\\') {
        if (i + 1 >= raw.size())
          return std::nullopt;
        out.push_back(raw[++i]);
        continue;
      }
      if (c == '\'')
        state = LexState::Normal;
      continue;
    }
  }

  if (state != LexState::Normal)
    return std::nullopt;

  return out;
}

namespace {

std::pair<size_t, size_t> clampUnorderedRange(StringRef text, size_t begin,
                                             size_t end) {
  begin = std::min(begin, text.size());
  end = std::min(end, text.size());
  if (end < begin)
    std::swap(begin, end);
  return {begin, end};
}

} // namespace

bool rangeContainsNewline(StringRef text, size_t begin, size_t end) {
  const auto range = clampUnorderedRange(text, begin, end);
  return text.substr(range.first, range.second - range.first).find('\n') !=
         StringRef::npos;
}

bool rangeContainsOnlyWhitespace(StringRef text, size_t begin, size_t end) {
  const auto range = clampUnorderedRange(text, begin, end);
  return isWhitespace(text.substr(range.first, range.second - range.first));
}

size_t lineStartOffset(StringRef text, size_t pos) {
  pos = std::min(pos, text.size());
  while (pos > 0 && text[pos - 1] != '\n' && text[pos - 1] != '\r')
    --pos;
  return pos;
}

size_t lineEndOffset(StringRef text, size_t pos) {
  pos = std::min(pos, text.size());
  while (pos < text.size() && text[pos] != '\n' && text[pos] != '\r')
    ++pos;
  return pos;
}

bool beginsLineAfterWhitespace(StringRef text, size_t offset) {
  return rangeContainsOnlyWhitespace(text, lineStartOffset(text, offset),
                                     offset);
}

bool endsLineBeforeWhitespace(StringRef text, size_t offset) {
  return rangeContainsOnlyWhitespace(text, offset, lineEndOffset(text, offset));
}

bool shouldPreserveFinalCallSuffixGroup(StringRef replacement) {
  StringRef s = replacement.trim();
  if (s.empty())
    return false;

  size_t pos = skipWSAndComments(s, 0);
  if (pos >= s.size() || s[pos] != '(')
    return false;

  const size_t headEnd = findMatchingRParen(s, pos);
  if (headEnd == StringRef::npos)
    return false;

  const size_t firstInside = skipWSAndComments(s, pos + 1);
  if (firstInside >= headEnd)
    return false;

  pos = skipWSAndComments(s, headEnd + 1);
  bool sawCallGroup = false;
  while (pos < s.size() && s[pos] == '(') {
    const size_t groupEnd = findMatchingRParen(s, pos);
    if (groupEnd == StringRef::npos)
      return false;
    sawCallGroup = true;
    pos = skipWSAndComments(s, groupEnd + 1);
  }

  return sawCallGroup && pos == s.size();
}

uint64_t extendChainedCallEnd(StringRef fileText, uint64_t invEnd,
                              StringRef replacement) {
  if (invEnd > fileText.size())
    return invEnd;

  if (isIdentifierOrSimpleCallExpr(replacement))
    return invEnd;

  const bool preserveFinalSuffixGroup =
      shouldPreserveFinalCallSuffixGroup(replacement);

  size_t pos = skipWSAndComments(fileText, static_cast<size_t>(invEnd));
  if (pos >= fileText.size() || fileText[pos] != '(')
    return invEnd;

  std::vector<uint64_t> groupEnds;
  while (pos < fileText.size() && fileText[pos] == '(') {
    const size_t r = findMatchingRParen(fileText, pos);
    if (r == StringRef::npos)
      break;
    groupEnds.push_back(static_cast<uint64_t>(r + 1));
    pos = skipWSAndComments(fileText, r + 1);
  }

  if (groupEnds.empty())
    return invEnd;

  size_t consume = groupEnds.size();
  if (preserveFinalSuffixGroup && consume > 0)
    consume -= 1;
  return (consume == 0) ? invEnd : groupEnds[consume - 1];
}

bool startsWithAfterWhitespace(StringRef s, StringRef lit) {
  size_t i = 0;
  while (i < s.size()) {
    char c = s[i];
    if (c != ' ' && c != '\t' && c != '\r' && c != '\n')
      break;
    ++i;
  }
  return s.drop_front(i).starts_with(lit);
}

std::string quoteCString(StringRef s) {
  std::string res = "\"";
  for (char c : s) {
    switch (c) {
    case '\\':
      res += "\\\\";
      break;
    case '"':
      res += "\\\"";
      break;
    case '\n':
      res += "\\n";
      break;
    case '\r':
      res += "\\r";
      break;
    case '\t':
      res += "\\t";
      break;
    case '\b':
      res += "\\b";
      break;
    case '\f':
      res += "\\f";
      break;
    default:
      if (static_cast<unsigned char>(c) < 0x20 ||
          static_cast<unsigned char>(c) == 0x7F) {
        char buf[10];
        snprintf(buf, sizeof(buf), "\\x%02X", static_cast<unsigned char>(c));
        res += buf;
      } else {
        res += c;
      }
      break;
    }
  }
  res += "\"";
  return res;
}

} // namespace stringutils
} // namespace refold
} // namespace clang
