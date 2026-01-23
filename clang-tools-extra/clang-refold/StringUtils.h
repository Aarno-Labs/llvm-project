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
#include "llvm/ADT/Twine.h"
#include "llvm/Support/FormatVariadic.h"
#include "llvm/Support/raw_ostream.h"

#include <cstddef>
#include <cstdint>

using namespace llvm;

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
  return (Twine(s.substr(0, n)) + "…(" + Twine(s.size()) + ")").str();
}

inline std::string showWSWithClip(StringRef s, int n) {
  return showWS(clip(s, n));
}

inline std::string escape(StringRef s) {
  std::string buffer;
  llvm::raw_string_ostream os(buffer);

  os.write_escaped(s);

  os.flush();
  return buffer;
}

/// \brief Adjusts the boundaries of a character range to exclude leading and
/// trailing whitespace.
///
/// Given a string and a half-open interval `[b, e)`, this method increments
/// the start index and decrements the end index until they point to
/// non-whitespace characters or meet in the middle. This is useful for
/// normalizing macro arguments or code segments before performing comparisons
/// or replacements.
///
/// \param s The source string containing the range to be trimmed.
/// \param b The initial starting index (inclusive).
/// \param e The initial ending index (exclusive).
/// \return A pair `{newB, newE}` representing the trimmed half-open interval.
///         If the entire range consists of whitespace, `newB` will equal
///         `newE`.
inline std::pair<int, int> trimWsRange(StringRef s, int b, int e) {
  while (b < e && isWs(s[b]))
    b++;
  while (e > b && isWs(s[e - 1]))
    e--;
  return {b, e};
}

/// \brief Like Java's String::trim(), but only trims ASCII space and tab.
///
/// This removes leading and trailing `' '` and `'\t'` characters and returns a
/// view into the original string (no allocation).
///
/// \param S The input string.
/// \returns A StringRef with leading/trailing spaces and tabs removed.
inline StringRef trimEdgeSpaces(StringRef s) {
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
    return s; // no trimming needed; return the original string ref

  return s.substr(lo, hi - lo); // return trimmed portion
}

inline bool isIdentBoundary(StringRef s, int idx) {
  if (idx < 0 || idx >= static_cast<int>(s.size()))
    return true;
  return !isAsciiIdentChar(s[idx]);
}

inline bool isLineSplice(StringRef s, int nlIdx) {
  if (nlIdx <= 0)
    return false;
  char prev = s[nlIdx - 1];
  if (prev == '\\')
    return true;
  // Windows form: "\\\r\n"
  if (prev == '\r' && nlIdx >= 2 && s[nlIdx - 2] == '\\')
    return true;
  return false;
}

/// True iff s[from:to) contains only horizontal whitespace (and optional '\r').
inline bool isIndentOnly(StringRef s, int from, int to) {
  int n = static_cast<int>(s.size());
  int start = std::clamp(from, 0, n);
  int end = std::clamp(to, start, n);

  // slice(start, end) creates a reference to the substring
  return s.slice(start, end).find_first_not_of(" \t\r") == StringRef::npos;
}

inline std::string quoteCString(StringRef s) {
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

/// Replace the substring in \p s spanning [begin, end) with \p repl.
inline std::string replaceRange(StringRef s, int begin, int end,
                                StringRef repl) {
  size_t uBegin = static_cast<size_t>(begin);
  size_t uEnd = static_cast<size_t>(end);
  return (s.substr(0, uBegin).str() + repl.str() + s.substr(uEnd).str());
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
inline StringRef stripHeaderToken(StringRef token) {
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
  } else if constexpr (std::is_same_v<T, StringRef>) {
    return elem.str();
  } else if constexpr (std::is_same_v<T, char>) {
    return std::string(1, elem);
  } else if constexpr (std::is_integral_v<T>) {
    return formatv("{0}", elem).str();
  } else {
    static_assert(!sizeof(T), "Unsupported element type for logFormattedArray");
  }
}

/// \brief Heuristically determines if a preprocessed token spelling looks like
/// a C/C++ string literal.
///
/// This performs a lightweight prefix check to identify standard double-quoted
/// strings as well as C++11/C++20 prefixed literals (L, u8, u, U). It is
/// intended for use in preprocessor-level heuristics (like stringify mapping)
/// where full lexing of the token content is not required.
///
/// \param tok The raw token spelling to examine.
/// \return True if the trimmed token starts with a valid string literal
///         opening sequence; false otherwise.
inline bool looksLikeStringLiteralToken(StringRef tok) {
  // .trim() handles leading/trailing whitespace
  StringRef s = tok.trim();

  if (s.empty())
    return false;

  // Standard string literal
  if (s.starts_with("\""))
    return true;

  // Prefixed string literals (L"", u8"", u"", U"")
  if (s.starts_with("L\"") || s.starts_with("u\"") || s.starts_with("U\"") ||
      s.starts_with("u8\"")) {
    return true;
  }

  return false;
}

inline std::string dbgOutTail(StringRef out) {
  size_t tailStart = out.size() > 140 ? out.size() - 140 : 0;
  StringRef tail = out.drop_front(tailStart);

  std::string result = tail.str();
  size_t pos = 0;
  while ((pos = result.find('\n', pos)) != std::string::npos) {
    result.replace(pos, 1, "\\n");
    pos += 2;
  }
  return result;
}

/** True iff offset is a beginning-of-line in text. */
inline bool isBOL(StringRef text, int offset) {
  int o = std::clamp(offset, 0, static_cast<int>(text.size()));
  return (o == 0) || (text[o - 1] == '\n');
}

// inline bool outEndsWith(StringRef out, StringRef suffix) {
//   return out.endswith(suffix);
// }

inline bool outAtBOL(StringRef s) { return s.empty() || s.back() == '\n'; }

inline bool endsWithLf(StringRef s) { return !s.empty() && s.back() == '\n'; }

inline bool startsWith(StringRef s, int offset, StringRef lit) {
  int n = static_cast<int>(s.size());
  int m = static_cast<int>(lit.size());
  if (offset < 0 || offset + m > n)
    return false;
  return s.substr(offset).starts_with(lit);
}

inline int lastIndexOfChar(StringRef s, char ch, int fromInclusive) {
  if (s.empty())
    return -1;
  int limit = std::min(fromInclusive, static_cast<int>(s.size()) - 1);
  if (limit < 0)
    return -1;

  size_t res = s.take_front(limit + 1).rfind(ch);
  return (res == StringRef::npos) ? -1 : static_cast<int>(res);
}

/** 1-based line number at the given offset. */
inline int lineAtOffset(StringRef text, int offset) {
  int o = std::clamp(offset, 0, static_cast<int>(text.size()));
  return 1 + static_cast<int>(text.take_front(o).count('\n'));
}

/** Count '\n' in an entire sequence. */
inline int countNewlines(StringRef s) {
  return static_cast<int>(s.count('\n'));
}

/** Count '\n' in text from range [start, end). */
inline int countNewlines(StringRef text, int start, int end) {
  int len = static_cast<int>(text.size());
  int s = std::clamp(start, 0, len);
  int e = std::clamp(end, s, len);
  return static_cast<int>(text.slice(s, e).count('\n'));
}

inline int countNonSplicedNewlines(StringRef s, int from, int to) {
  int n = static_cast<int>(s.size());
  int a = std::clamp(from, 0, n);
  int b = std::clamp(to, a, n);
  int c = 0;
  for (int i = a; i < b; i++) {
    if (s[i] == '\n' && !isLineSplice(s, i))
      c++;
  }
  return c;
}

inline std::string boolArrayToString(const std::vector<char> &touched) {
  std::string res;
  res.reserve(touched.size());
  for (char b : touched)
    res += (b ? '1' : '0');
  return res;
}

inline std::string
rangesToStringWithSlices(StringRef invText,
                         const std::vector<std::pair<int, int>> &ranges) {
  std::string sb = "[";
  for (size_t i = 0; i < ranges.size(); ++i) {
    if (i > 0)
      sb += ", ";
    const auto &r = ranges[i];
    sb += "[" + std::to_string(r.first) + "," + std::to_string(r.second) + ")";
    if (!invText.empty() && r.first >= 0 && r.second >= r.first &&
        static_cast<size_t>(r.second) <= invText.size()) {
      sb += "='" +
            showWSWithClip(invText.substr(r.first, r.second - r.first), 200) +
            "'";
    }
  }
  sb += "]";
  return sb;
}

} // namespace stringutils
} // namespace refold
} // namespace clang

#endif // LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_STRINGUTILS_H
