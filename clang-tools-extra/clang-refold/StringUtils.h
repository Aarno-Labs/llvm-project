//===--- StringUtils.h ------------------------------------------*- C++ -*-===//
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
//  • “Type-ish” token check:
//      - isTypeishToken(StringRef)   : "*" or identifier/keyword-like
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

#ifndef LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_STRINGUTILS_H
#define LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_STRINGUTILS_H

#include "llvm/ADT/StringRef.h"

#include <cstddef>
#include <cstdint>

namespace clang {
namespace refold {
namespace stringutils {

// ------------------- Single-char predicates (ASCII only) ---------------------

inline constexpr bool isWs(char c) noexcept {
  // C/C++ PP whitespace: space, HT, LF, VT, FF, CR
  return c == ' ' || c == '\t' || c == '\n' || c == '\v' || c == '\f' ||
         c == '\r';
}

inline constexpr bool isAsciiAlpha(char c) noexcept {
  return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
}

inline constexpr bool isAsciiDigit(char c) noexcept {
  return (c >= '0' && c <= '9');
}

inline constexpr bool isAsciiIdentStart(char c) noexcept {
  return c == '_' || isAsciiAlpha(c);
}

inline constexpr bool isAsciiIdentChar(char c) noexcept {
  return isAsciiIdentStart(c) || isAsciiDigit(c);
}

/** Broad “identifier-ish” (includes digits) for boundary-glue heuristics. */
inline constexpr bool isIdentChar(char c) noexcept {
  return isAsciiIdentChar(c);
}

// ---------------------- String predicates (ASCII only) ----------------------

inline bool isAsciiWhitespace(StringRef s) noexcept {
  for (char c : s) {
    if (!isWs(c))
      return false;
  }
  return true;
}

/** True iff s is a non-empty ASCII identifier (first char not a digit). */
inline bool isIdentifierOnly(StringRef s) noexcept {
  if (s.empty())
    return false;
  if (!isAsciiIdentStart(s.front()))
    return false;
  for (std::size_t i = 1; i < s.size(); ++i) {
    if (!isAsciiIdentChar(s[i]))
      return false;
  }
  return true;
}

/**
 * Treat keywords as identifiers for our purposes; "*" is also considered
 * “type-ish” (pointer glue) to help prefix handling.
 */
inline bool isTypeishToken(StringRef s) noexcept {
  if (s.empty())
    return false;
  if (s.size() == 1 && s[0] == '*')
    return true;
  return isIdentifierOnly(s);
}

// --------------------- Index scans (return -1 if none) ----------------------

inline int firstNonWsIdx(StringRef s) noexcept {
  for (std::size_t i = 0; i < s.size(); ++i) {
    if (!isWs(s[i]))
      return static_cast<int>(i);
  }
  return -1;
}

inline int lastNonWsIdx(StringRef s) noexcept {
  for (std::size_t i = s.size() - 1; i >= 0; --i) {
    if (!isWs(s[i]))
      return static_cast<int>(i);
  }
  return -1;
}

// -------------------- Diagnostics helpers (pure string) ---------------------

inline std::string showWS(StringRef s) {
  std::string out;
  out.reserve(s.size() * 2);
  for (char c : s) {
    switch (c) {
    case ' ':
      out += "·";
      break;
    case '\t':
      out += "\\t";
      break;
    case '\n':
      out += "\\n";
      break;
    case '\r':
      out += "\\r";
      break;
    case '\f':
      out += "\\f";
      break;
    case '\v':
      out += "\\v";
      break;
    default:
      out.push_back(c);
      break;
    }
  }
  return out;
}

inline std::string clip(StringRef s, int n) {
  if (static_cast<int>(s.size()) <= n)
    return s.str();
  return (s.substr(0, n) + "…(" + std::to_string(s.size()) + ")").str();
}

inline std::string trimEdgeSpaces(StringRef s) {
  std::size_t lo = 0;
  std::size_t hi = s.size();

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
    return s.str(); // no trimming needed; copy whole string

  return s.substr(lo, hi - lo).str(); // copy trimmed portion
}

} // namespace stringutils
} // namespace refold
} // namespace clang

#endif // LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_STRINGUTILS_H
