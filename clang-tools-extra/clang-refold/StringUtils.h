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
#include "llvm/Support/raw_ostream.h"

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

// --------------------- Index scans (return -1 if none) ----------------------

inline std::optional<size_t> firstNonWsIdx(StringRef s) noexcept {
  size_t idx = s.find_first_not_of(" \t\n\v\f\r");
  return (idx == StringRef::npos) ? std::nullopt : std::make_optional(idx);
}

inline std::optional<size_t> lastNonWsIdx(StringRef s) noexcept {
  size_t idx = s.find_last_not_of(" \t\n\v\f\r");
  return (idx == StringRef::npos) ? std::nullopt : std::make_optional(idx);
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

inline std::string clip(StringRef s, size_t n) {
  if (s.size() <= n)
    return s.str();
  return (s.substr(0, n) + "…(" + std::to_string(s.size()) + ")").str();
}

inline std::string escape(StringRef s) {
  std::string buffer;
  llvm::raw_string_ostream os(buffer);

  os.write_escaped(s);

  os.flush();
  return buffer;
}

/// \brief Like Java's String::trim(), but only trims ASCII space and tab.
///
/// This removes leading and trailing `' '` and `'\t'` characters and returns a
/// view into the original string (no allocation).
///
/// \param S The input string.
/// \returns A std::string with leading/trailing spaces and tabs removed.
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

/// Normalizes an \c #include target token by stripping the surrounding
/// delimiter characters.
///
/// Clang token spellings for include targets often arrive in one of these
/// forms:
/// - Quoted user headers: \c "\"e.h\""
/// - System headers: \c "<vector>"
///
/// This method trims leading/trailing whitespace and then:
/// - If the token starts with '<' and ends with '>', returns the interior.
/// - If the token starts with '"' and ends with '"', returns the interior.
/// - Otherwise returns the trimmed token unchanged.
///
/// This is a shallow normalization only; it does not attempt to unescape
/// quotes, interpret backslashes, or resolve paths.
///
/// \param token the raw include target token spelling (may be null)
/// \return the include target with surrounding <> or "" removed when present;
///         returns the empty string if \p token is null
inline StringRef stripHeaderToken(llvm::StringRef token) {
  token = token.trim();
  if (token.empty())
    return "";
  if (token.starts_with("<") && token.ends_with(">"))
    return token.drop_front(1).drop_back(1);
  if (token.starts_with("\"") && token.ends_with("\""))
    return token.drop_front(1).drop_back(1);
  return token;
}

inline std::string zpadUnsigned(size_t v, unsigned width) {
  std::string s = formatv("{0}", v).str();
  if (s.size() < width)
    s.insert(s.begin(), width - s.size(), '0');
  return s;
}

template <typename T> std::string stringifyElement(const T &elem) {
  if constexpr (std::is_same_v<T, std::string>) {
    return elem;
  } else if constexpr (std::is_same_v<T, llvm::StringRef>) {
    return elem.str();
  } else if constexpr (std::is_same_v<T, char>) {
    return std::string(1, elem);
  } else if constexpr (std::is_integral_v<T>) {
    return formatv("{0}", elem).str();
  } else {
    static_assert(!sizeof(T), "Unsupported element type for logFormattedArray");
  }
}

} // namespace stringutils
} // namespace refold
} // namespace clang

#endif // LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_STRINGUTILS_H
