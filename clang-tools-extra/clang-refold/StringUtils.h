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
//                                      CR)
//      - isWhitespace(StringRef)     : true iff every byte is PP whitespace
//  • Identifier predicates (ASCII):
//      - isIdentStart(char)          : '_' or ASCII letter
//      - isIdentPart(char)           : start-char or digit
//      - isIdentifierOnly(StringRef) : trimmed text is one identifier token
//      - isIdentifierOrSimpleCallExpr(StringRef): identifier or IDENT(...)
//  • Index scans:
//      - firstNonWsIdx(StringRef)    : first non-WS index or nullopt
//      - lastNonWsIdx(StringRef)     : last non-WS index or nullopt
//  • Debug formatting helpers:
//      - showWS(StringRef)           : visualize whitespace (·, \t, \n, \r, \f,
//                                      \v)
//      - clip(StringRef, size_t)     : truncate with length suffix
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
//   if (isWhitespace(S)) { … }
//   bool glue = isIdentPart(L) && isIdentPart(R);  // conservative boundary
//   auto first = firstNonWsIdx(Txt);               // nullopt if all whitespace
//
// Policy
// ------
//  • Keep helpers minimal and allocation-free where practical.
//  • Behavior must not depend on locale or environment.
//
// Author:
//   jeikenberry
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_STRINGUTILS_H
#define LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_STRINGUTILS_H

#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/ADT/Twine.h"
#include "llvm/Support/FormatVariadic.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

using namespace llvm;

namespace clang {
namespace refold {
namespace stringutils {

// ------------------- Single-char predicates (ASCII only) ---------------------

// Return true for whitespace that can separate tokens without acting as a
// source-line break for this check.  The name intentionally excludes '\n' and
// '\r'; '\v' and '\f' are C/C++ whitespace characters, but they are not treated
// here as newline delimiters
inline constexpr bool isNonNewlineWs(char c) {
  return c == ' ' || c == '\t' || c == '\v' || c == '\f';
}

/// True for the ASCII whitespace set recognized by the C/C++ preprocessor.
inline constexpr bool isWs(char c) noexcept {
  // C/C++ PP whitespace: space, HT, LF, VT, FF, CR
  return c == '\n' || c == '\r' || isNonNewlineWs(c);
}

/// True for the ASCII start character of an identifier-like spelling.
inline constexpr bool isIdentStart(char c) noexcept {
  return c == '_' || isAlpha(c);
}

/// True for an ASCII identifier continuation character.
inline constexpr bool isIdentPart(char c) noexcept {
  return isIdentStart(c) || isDigit(c);
}

// ----------------------- String predicates (ASCII only) ----------------------

// If a macro invocation is immediately followed by one or more parenthesized
// argument lists in the *source file*, it may be a chain of function-like
// macros evaluating to another function-like macro (e.g. INC3()()()(10)).
//
// In that case, a callsite patch that replaces only the first invocation text
// (INC3()) would leave a dangling "(…)" suffix, yielding invalid code.
// When the replacement is not a bare identifier, conservatively consume any
// immediately following "(...)" groups.
/// True iff \p s trims to one identifier token spelling.
bool isIdentifierOnly(StringRef s);

/// True iff \p replacement is either an identifier or a simple callable head.
///
/// This is used by chained-call preservation: an identifier or `IDENT(...)`
/// replacement can still receive following call-suffix groups without needing
/// the refolder to consume them.
bool isIdentifierOrSimpleCallExpr(StringRef replacement);

/// True iff every byte in \p s is PP whitespace.
inline bool isWhitespace(StringRef s) noexcept {
  return all_of(s, [](char c) { return isWs(c); });
}

// ---------------------- Index scans (return -1 if none) ----------------------

/// Return the first non-whitespace byte offset, or nullopt for all-whitespace.
inline std::optional<size_t> firstNonWsIdx(StringRef s) noexcept {
  size_t idx = s.find_first_not_of(" \t\n\v\f\r");
  return (idx == StringRef::npos) ? std::nullopt : std::make_optional(idx);
}

/// Return the last non-whitespace byte offset, or nullopt for all-whitespace.
inline std::optional<size_t> lastNonWsIdx(StringRef s) noexcept {
  size_t idx = s.find_last_not_of(" \t\n\v\f\r");
  return (idx == StringRef::npos) ? std::nullopt : std::make_optional(idx);
}

/// Skip ASCII whitespace and complete C/C++ comments starting at \p i.
size_t skipWSAndComments(StringRef s, size_t i);

/// Find the matching right parenthesis for \p lParenIdx, ignoring comments and
/// string/character literal contents.
size_t findMatchingRParen(StringRef s, size_t lParenIdx);

/// Advance `pos` over whitespace that does not cross a source-line boundary.
inline constexpr void skipNonNewlineWs(StringRef text, size_t &pos) {
  while (pos < text.size() && stringutils::isNonNewlineWs(text[pos]))
    ++pos;
}

// --------------------- Diagnostics helpers (pure string) ---------------------

/// Render whitespace visibly for diagnostics while preserving non-whitespace.
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

/// Clip \p s to at most \p n bytes and append the original length when clipped.
inline std::string clip(StringRef s, size_t n) {
  if (s.size() <= n)
    return s.str();
  return (Twine(s.substr(0, n)) + "…(" + Twine(s.size()) + ")").str();
}

/// Clip a string and then render whitespace visibly for compact diagnostics.
inline std::string showWSWithClip(StringRef s, size_t n) {
  return showWS(clip(s, n));
}

/// Return LLVM's escaped representation of \p s for diagnostics.
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
inline std::pair<size_t, size_t> trimWsRange(StringRef s, size_t b, size_t e) {
  while (b < e && isWs(s[b]))
    b++;
  while (e > b && isWs(s[e - 1]))
    e--;
  return {b, e};
}

/// \brief Trim only ASCII space and tab from both ends.
///
/// This removes leading and trailing `' '` and `'\t'` characters and returns a
/// view into the original string (no allocation). Newlines and other PP
/// whitespace are intentionally preserved.
///
/// \param S The input string.
/// \returns A StringRef with leading/trailing spaces and tabs removed.
StringRef trimEdgeSpaces(StringRef s);

/// \brief Return a view of \p s with any trailing '\n' characters removed.
///
/// Clang's token dump spellings and some macro expansions may include one or
/// more trailing newlines. For certain within-token comparisons (e.g. paste
/// segment rewrites), we want a stable spelling that ignores these trailing
/// line breaks while preserving all other bytes.
///
/// This is ASCII-only and allocation-free.
inline StringRef stripTrailingNewlines(StringRef s) noexcept {
  while (s.ends_with("\n"))
    s = s.drop_back();
  return s;
}

/// True iff the newline at \p nlIdx is escaped by a C line splice.
inline bool isLineSplice(StringRef s, size_t nlIdx) {
  if (nlIdx == 0 || nlIdx > s.size())
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
inline bool isIndentOnly(StringRef s, size_t from, size_t to) {
  const size_t n = s.size();
  const size_t start = std::clamp(from, size_t(0), n);
  const size_t end = std::clamp(to, start, n);

  // slice(start, end) creates a reference to the substring
  return s.slice(start, end).find_first_not_of(" \t\r") == StringRef::npos;
}

/// \brief Quote text as a C string spelling.
///
/// Escapes characters that must be escaped inside a C string token, including
/// backslashes, double quotes, and common control characters, then wraps the
/// result in double quotes.
///
/// \param s The raw text to quote.
/// \returns The escaped C string spelling, including surrounding quotes.
std::string quoteCString(StringRef s);

/// \brief Quote text as a minimally escaped C string literal token.
///
/// This helper wraps \p s in double quotes and escapes only bytes that would
/// terminate or escape the literal spelling (`\\` and `"`). It deliberately
/// preserves all other bytes verbatim. Use quoteCString() when control
/// characters must be rendered as C escape sequences.
///
/// \param s The raw string-literal payload.
/// \returns The minimally escaped string literal spelling, including quotes.
std::string quoteCStringLiteral(StringRef s);

/// Return one StringRef per byte/character in \p s.
///
/// The returned references point into \p s, so callers must keep the source
/// buffer alive while using the vector. This is primarily used to run the
/// generic diff machinery at byte/character granularity without allocating one
/// std::string per character.
std::vector<StringRef> splitChars(StringRef s);

/// Canonicalize the raw payload recovered from a stringification inverse.
///
/// This mirrors the preprocessor's stringify normalization for the supported
/// inverse domain: trim leading/trailing PP whitespace, collapse each
/// inter-token whitespace run to one ASCII space, reject comments outside
/// literals, and preserve escape-aware string/character literal contents.
/// Returns nullopt when the payload is malformed or outside that domain.
std::optional<std::string> canonicalizeStringifyInversePayload(StringRef raw);

/// True iff the clamped byte range [begin, end) contains a line-feed.
///
/// The endpoints are first clamped into [0, text.size()]. If the resulting end
/// precedes the begin, the bounds are swapped so callers may pass unordered
/// byte offsets without changing the queried set of bytes.
bool rangeContainsNewline(StringRef text, size_t begin, size_t end);

/// True iff the clamped byte range [begin, end) contains only PP whitespace.
///
/// The accepted whitespace set is the same ASCII set as isWs(): space, HT, LF,
/// VT, FF, and CR. The endpoints are clamped and unordered bounds are swapped
/// using the same policy as rangeContainsNewline().
bool rangeContainsOnlyWhitespace(StringRef text, size_t begin, size_t end);

/// Return the byte offset of the start of the physical line containing \p pos.
///
/// The input offset is clamped into [0, text.size()]. Both LF and CR terminate
/// a line for this byte-level helper.
size_t lineStartOffset(StringRef text, size_t pos);

/// Return the byte offset one-past the last non-newline byte of the physical
/// line containing \p pos.
///
/// The input offset is clamped into [0, text.size()]. Both LF and CR terminate
/// a line for this byte-level helper.
size_t lineEndOffset(StringRef text, size_t pos);

/// True iff only line whitespace appears between the containing line start and
/// \p offset.
bool beginsLineAfterWhitespace(StringRef text, size_t offset);

/// True iff only line whitespace appears between \p offset and the containing
/// line end.
bool endsLineBeforeWhitespace(StringRef text, size_t offset);

/// True iff \p replacement is a parenthesized callable head followed by one or
/// more balanced call-suffix groups, with no trailing non-comment content.
bool shouldPreserveFinalCallSuffixGroup(StringRef replacement);

/// Extend an invocation end offset over trailing chained-call suffix groups
/// when the replacement is no longer directly callable.
uint64_t extendChainedCallEnd(StringRef fileText, uint64_t invEnd,
                              StringRef replacement);

/// Return true iff the first non-whitespace bytes of \p s start with \p lit.
bool startsWithAfterWhitespace(StringRef s, StringRef lit);

/// Replace the substring in \p s spanning [begin, end) with \p repl.
inline std::string replaceRange(StringRef s, size_t begin, size_t end,
                                StringRef repl) {
  return (s.substr(0, begin).str() + repl.str() + s.substr(end).str());
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

/// Format an unsigned value with left zero padding to \p width columns.
inline std::string zpadUnsigned(size_t v, unsigned width) {
  std::string s = formatv("{0}", v).str();
  if (s.size() < width)
    s.insert(s.begin(), width - s.size(), '0');
  return s;
}

/// Convert supported scalar/logging element types to strings.
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

/// Return a compact escaped tail of an output buffer for trace diagnostics.
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

/// True iff \p offset is a beginning-of-line in \p text.
inline bool isBOL(StringRef text, size_t offset) {
  size_t o = std::clamp(offset, size_t(0), text.size());
  return (o == 0) || (text[o - 1] == '\n');
}

// inline bool outEndsWith(StringRef out, StringRef suffix) {
//   return out.endswith(suffix);
// }

/// True iff appending at the end of \p s would occur at beginning-of-line.
inline bool outAtBOL(StringRef s) { return s.empty() || s.back() == '\n'; }

/// True iff \p s has a final LF byte.
inline bool endsWithLf(StringRef s) { return !s.empty() && s.back() == '\n'; }

/// True iff \p lit appears at byte offset \p offset in \p s.
inline bool startsWith(StringRef s, size_t offset, StringRef lit) {
  const size_t n = s.size();
  const size_t m = lit.size();
  if (offset + m > n)
    return false;
  return s.substr(offset).starts_with(lit);
}

/// Find the last occurrence of \p ch at or before \p fromInclusive.
inline size_t lastIndexOfChar(StringRef s, char ch, size_t fromInclusive) {
  if (s.empty())
    return StringRef::npos;

  const size_t len = s.size();
  const size_t limit = std::min(fromInclusive, len - 1);

  return s.take_front(limit + 1).rfind(ch);
}

/// Return the 1-based physical line number at \p offset.
inline size_t lineAtOffset(StringRef text, uint64_t offset) {
  const size_t o = (offset >= static_cast<uint64_t>(text.size()))
                       ? text.size()
                       : static_cast<size_t>(offset);
  return 1 + text.take_front(o).count('\n');
}

/// Count LF bytes in an entire sequence.
inline size_t countNewlines(StringRef s) { return s.count('\n'); }

/// Count LF bytes in the clamped half-open range [start, end).
inline size_t countNewlines(StringRef text, uint64_t start, uint64_t end) {
  const size_t len = text.size();

  // 1. Clamp start to [0, len].
  const size_t s =
      (start >= static_cast<uint64_t>(len)) ? len : static_cast<size_t>(start);

  // 2. Clamp end to [s, len].
  // This ensures the slice range is always valid (non-negative length).
  size_t e;
  if (end >= static_cast<uint64_t>(len)) {
    e = len;
  } else if (end <= static_cast<uint64_t>(s)) {
    e = s;
  } else {
    e = static_cast<size_t>(end);
  }

  return text.slice(s, e).count('\n');
}

/// Count LF bytes in [from,to) that are not escaped by a C line splice.
inline size_t countNonSplicedNewlines(StringRef s, size_t from, size_t to) {
  const size_t n = s.size();

  // 1. Clamp 'from' to [0, n]
  const size_t a = (from > n) ? n : from;

  // 2. Clamp 'to' to [a, n] to ensure we never have a negative range
  const size_t b = (to < a) ? a : (to > n ? n : to);

  size_t count = 0;
  for (size_t i = a; i < b; ++i) {
    // 3. We know i is valid because it's < b and b <= s.size()
    if (s[i] == '\n') {
      // 4. Pass the index to the splice checker.
      if (!isLineSplice(s, i)) {
        count++;
      }
    }
  }
  return count;
}

/// Render a boolean/byte array as a compact `0`/`1` string.
inline std::string boolArrayToString(ArrayRef<char> touched) {
  std::string res;
  res.reserve(touched.size());
  for (char b : touched)
    res += (b ? '1' : '0');
  return res;
}

/// Format byte ranges and their corresponding slices for diagnostics.
inline std::string
rangesToStringWithSlices(StringRef invText,
                         ArrayRef<std::pair<size_t, size_t>> ranges) {
  std::string sb = "[";
  for (size_t i = 0; i < ranges.size(); ++i) {
    if (i > 0)
      sb += ", ";
    const auto &r = ranges[i];
    sb += "[" + std::to_string(r.first) + "," + std::to_string(r.second) + ")";
    if (!invText.empty() && r.second >= r.first &&
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
