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

#include <cctype>
#include <cstdio>
#include <string>

namespace clang {
namespace refold {
namespace stringutils {

bool isIdentifierOnly(StringRef s) {
  s = s.trim();
  if (s.empty())
    return false;
  auto isIdentStart = [](unsigned char c) -> bool {
    return std::isalpha(c) || c == '_';
  };
  auto isIdentCont = [](unsigned char c) -> bool {
    return std::isalnum(c) || c == '_';
  };
  if (!isIdentStart(static_cast<unsigned char>(s.front())))
    return false;
  for (char ch : s.drop_front()) {
    if (!isIdentCont(static_cast<unsigned char>(ch)))
      return false;
  }
  return true;
}

size_t skipWSAndComments(StringRef s, size_t i) {
  const size_t n = s.size();
  while (i < n) {
    unsigned char c = static_cast<unsigned char>(s[i]);
    if (std::isspace(c)) {
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
