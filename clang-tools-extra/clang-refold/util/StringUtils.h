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
//      - isWs(StringRef)             : true iff every byte is PP whitespace
//  • Identifier predicates (ASCII):
//      - isIdentStart(char)          : '_' or ASCII letter
//      - isIdentPart(char)           : start-char or digit
//      - isIdentifierOnly(StringRef) : trimmed text is one identifier token
//      - isIdentifierOrSimpleCallExpr(StringRef): identifier or IDENT(...)
//  • Index scans:
//      - firstNonWsIdx(StringRef)    : first non-WS index or nullopt
//      - lastNonWsIdx(StringRef)     : last non-WS index or nullopt
//  • Debug formatting helpers:
//      - showWs(StringRef)           : visualize whitespace (·, \t, \n, \r, \f,
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
//   if (isWs(S)) { … }
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

/// True for PP whitespace other than a line-feed.  This intentionally keeps
/// carriage return in the accepted set to preserve existing byte-level scans
/// that treat LF as the only directive-line terminator.
inline constexpr bool isWsNoLF(char c) noexcept { return isWs(c) && c != '\n'; }

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
inline bool isWs(StringRef s) noexcept {
  return all_of(s, [](char c) { return isWs(c); });
}

/// Returns true for predefined macros whose expansion can change when inserted
/// or removed `#line` directives alter the logical source location.
inline bool isLineDirectiveSensitiveBuiltin(StringRef name) {
  return name == "__LINE__" || name == "__FILE__" || name == "__FILE_NAME__" ||
         name == "__BASE_FILE__";
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
size_t skipWsAndComments(StringRef s, size_t i);

/// Find the matching right parenthesis for \p lParenIdx, ignoring comments and
/// string/character literal contents.
size_t findMatchingRParen(StringRef s, size_t lParenIdx);

/// Advance `pos` over whitespace that does not cross a source-line boundary.
inline constexpr void skipNonNewlineWs(StringRef text, size_t &pos) {
  while (pos < text.size() && isNonNewlineWs(text[pos]))
    ++pos;
}

/// Return whether `[begin,end)` is the only non-whitespace content on its
/// physical line in \p text.
///
/// A caller that rewrites an interval into a *directive* needs this: a
/// directive is one only when it begins a line, so an interval sharing its line
/// with other source cannot be replaced by one.  A `#pragma` directive line
/// satisfies this by construction; a `_Pragma` operator is an expression-like
/// spelling that may sit anywhere a token may, so it must be checked.
///
/// `\r` terminates the scan in both directions, so a CRLF line behaves like an
/// LF one: scanning backwards it is the CR of the preceding terminator, and
/// scanning forwards it opens this line's terminator.  An empty or
/// out-of-bounds range is not alone on its line.
inline constexpr bool intervalIsAloneOnItsLine(StringRef text, uint64_t begin,
                                               uint64_t end) {
  if (begin >= end || end > text.size())
    return false;

  for (uint64_t pos = begin; pos > 0;) {
    --pos;
    const char c = text[pos];
    if (c == '\n' || c == '\r')
      break;
    if (!isNonNewlineWs(c))
      return false;
  }

  for (uint64_t pos = end; pos < text.size(); ++pos) {
    const char c = text[pos];
    if (c == '\n' || c == '\r')
      break;
    if (!isNonNewlineWs(c))
      return false;
  }

  return true;
}

/// Advance over one C backslash-newline splice at \p pos.
///
/// The splice must be fully contained in the half-open byte range
/// `[0, limit)`.  Both `\\\n` and `\\\r\n` are recognized.  The helper is
/// deliberately byte-oriented and does not interpret comments or literals; the
/// caller decides where backslash-newline splicing is admissible for its
/// proof domain.
template <typename OffsetT>
inline bool skipBackslashNewlineSplice(StringRef text, OffsetT limit,
                                       OffsetT &pos) {
  const OffsetT size = static_cast<OffsetT>(text.size());
  if (pos >= limit || pos >= size)
    return false;
  if (text[static_cast<size_t>(pos)] != '\\')
    return false;

  if (pos + 1 < limit && pos + 1 < size &&
      text[static_cast<size_t>(pos + 1)] == '\n') {
    pos += 2;
    return true;
  }
  if (pos + 2 < limit && pos + 2 < size &&
      text[static_cast<size_t>(pos + 1)] == '\r' &&
      text[static_cast<size_t>(pos + 2)] == '\n') {
    pos += 3;
    return true;
  }
  return false;
}

/// Skip horizontal preprocessing whitespace in the half-open range [pos, end).
inline void skipWsNoLF(StringRef text, size_t &pos, size_t end) {
  end = std::min(end, text.size());
  while (pos < end && isWsNoLF(text[pos]))
    ++pos;
}

/// Consume one ASCII identifier token from [pos, end).
inline bool consumeIdentifier(StringRef text, size_t &pos, size_t end,
                              StringRef &identifier) {
  end = std::min(end, text.size());
  if (pos >= end || !isIdentStart(text[pos]))
    return false;
  const size_t begin = pos++;
  while (pos < end && isIdentPart(text[pos]))
    ++pos;
  identifier = text.slice(begin, pos);
  return true;
}

/// Consume a preprocessing directive introducer and following horizontal space.
inline bool consumeDirectiveHash(StringRef text, size_t &pos, size_t end) {
  skipWsNoLF(text, pos, end);
  if (pos >= end || text[pos] != '#')
    return false;
  ++pos;
  skipWsNoLF(text, pos, end);
  return true;
}

/// Copy or skip one C/C++ string/character literal at \p pos.
bool copyQuotedLiteral(StringRef text, size_t &pos, std::string &out);
inline bool skipQuotedLiteral(StringRef text, size_t &pos) {
  std::string ignored;
  return copyQuotedLiteral(text, pos, ignored);
}

/// Replace each complete comment outside literals with one ASCII space.
std::string replaceCommentsWithWhitespacePreservingLiterals(StringRef text);

/// Return the basename portion of a slash- or backslash-separated path.
inline StringRef pathBasename(StringRef path) {
  const size_t slash = path.find_last_of("/\\");
  return slash == StringRef::npos ? path : path.drop_front(slash + 1);
}

/// Escape/quote a logical filename for a #line double-quoted operand.
std::string escapeLineDirectivePath(StringRef path);
inline std::string quoteLineDirectivePath(StringRef path) {
  std::string out;
  out.reserve(path.size() + 2);
  out.push_back('"');
  out += escapeLineDirectivePath(path);
  out.push_back('"');
  return out;
}

/// True iff the bytes before \p pos on the same LF-delimited line are indent.
inline bool startsAfterLineIndent(StringRef text, size_t pos) {
  pos = std::min(pos, text.size());
  size_t lineBegin = pos;
  while (lineBegin > 0 && text[lineBegin - 1] != '\n')
    --lineBegin;
  for (size_t i = lineBegin; i < pos; ++i)
    if (!isNonNewlineWs(text[i]))
      return false;
  return true;
}

bool containsAtLineStartAfterIndent(StringRef text, StringRef needle);
bool lineStartsWithDirectiveKeyword(StringRef line, StringRef keyword);
bool physicalLineEndsWithSplice(StringRef bytes, uint64_t lineBegin,
                                uint64_t lineEnd);
uint64_t lineBeginContainingOffset(StringRef bytes, uint64_t byte);
uint64_t extendLineToLogicalDirective(StringRef bytes, uint64_t lineBegin);
uint64_t extendRangeToLogicalDirective(StringRef bytes, uint64_t begin,
                                       uint64_t end);

// --------------------- Diagnostics helpers (pure string) ---------------------

/// Render whitespace visibly for diagnostics while preserving non-whitespace.
inline std::string showWs(StringRef s) {
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
inline std::string showWsWithClip(StringRef s, size_t n) {
  return showWs(clip(s, n));
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

inline StringRef trimHorizontal(StringRef text) {
  while (!text.empty() &&
         (text.front() == ' ' || text.front() == '\t' || text.front() == '\r' ||
          text.front() == '\f' || text.front() == '\v'))
    text = text.drop_front();
  while (!text.empty() &&
         (text.back() == ' ' || text.back() == '\t' || text.back() == '\r' ||
          text.back() == '\f' || text.back() == '\v'))
    text = text.drop_back();
  return text;
}

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

/// True iff the newline at \p nlIdx is escaped by a line splice.
///
/// Clang accepts the common extension where horizontal whitespace separates the
/// backslash from the physical newline, warning but still deleting the whole
/// backslash-whitespace-newline sequence before directive recognition.  Refold
/// proof code must follow that behavior when extending directive source ranges;
/// otherwise materializing `#\   \ninclude "h"` can replace only the first
/// physical line and leave `include "h"` behind as ordinary source text.
inline constexpr bool isLineSplice(StringRef s, size_t nlIdx) {
  if (nlIdx == 0 || nlIdx > s.size())
    return false;

  size_t cursor = nlIdx;
  if (cursor > 0 && s[cursor - 1] == '\r')
    --cursor;

  while (cursor > 0) {
    char prev = s[cursor - 1];
    if (isNonNewlineWs(prev)) {
      --cursor;
      continue;
    }
    return prev == '\\';
  }

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

/// Clamp a possibly unordered half-open byte range into \p text.
inline std::pair<size_t, size_t> clampUnorderedRange(StringRef text,
                                                     size_t begin, size_t end) {
  begin = std::min(begin, text.size());
  end = std::min(end, text.size());
  if (end < begin)
    std::swap(begin, end);
  return {begin, end};
}

/// True iff the clamped byte range [begin, end) contains a line-feed.
///
/// The endpoints are first clamped into [0, text.size()]. If the resulting end
/// precedes the begin, the bounds are swapped so callers may pass unordered
/// byte offsets without changing the queried set of bytes.
inline bool rangeContainsNewline(StringRef text, size_t begin, size_t end) {
  const auto range = clampUnorderedRange(text, begin, end);
  return text.substr(range.first, range.second - range.first).find('\n') !=
         StringRef::npos;
}

/// Return a view with leading/trailing PP whitespace other than LF removed.
inline StringRef trimWsNoLF(StringRef text) {
  size_t begin = 0;
  while (begin < text.size() && isWsNoLF(text[begin]))
    ++begin;

  size_t end = text.size();
  while (end > begin && isWsNoLF(text[end - 1]))
    --end;
  return text.slice(begin, end);
}

/// Return a view with only *leading* PP whitespace other than LF removed.
///
/// Use this instead of trimWsNoLF() when the trailing whitespace run is
/// significant.  Indentation is not token-bearing and may legitimately differ
/// between two spellings of the same text, but the whitespace that terminates
/// a run separates it from whatever follows: dropping it would let `int ` and
/// `int` compare equal even though only one of them keeps the following bytes
/// in a separate token.
inline StringRef trimLeadingWsNoLF(StringRef text) {
  size_t begin = 0;
  while (begin < text.size() && isWsNoLF(text[begin]))
    ++begin;
  return text.substr(begin);
}

/// True iff the clamped byte range [begin, end) contains only PP whitespace.
///
/// The accepted whitespace set is the same ASCII set as isWs(): space, HT, LF,
/// VT, FF, and CR. The endpoints are clamped and unordered bounds are swapped
/// using the same policy as rangeContainsNewline().
inline bool rangeContainsOnlyWs(StringRef text, size_t begin, size_t end) {
  const auto range = clampUnorderedRange(text, begin, end);
  return isWs(text.substr(range.first, range.second - range.first));
}

/// Return the byte offset of the start of the physical line containing \p pos.
///
/// The input offset is clamped into [0, text.size()]. Both LF and CR terminate
/// a line for this byte-level helper.
inline constexpr size_t lineStartOffset(StringRef text, size_t pos) {
  pos = std::min(pos, text.size());
  while (pos > 0 && text[pos - 1] != '\n' && text[pos - 1] != '\r')
    --pos;
  return pos;
}

/// Return the byte offset one-past the last non-newline byte of the physical
/// line containing \p pos.
///
/// The input offset is clamped into [0, text.size()]. Both LF and CR terminate
/// a line for this byte-level helper.
inline constexpr size_t lineEndOffset(StringRef text, size_t pos) {
  pos = std::min(pos, text.size());
  while (pos < text.size() && text[pos] != '\n' && text[pos] != '\r')
    ++pos;
  return pos;
}

/// True iff only line whitespace appears between the containing line start and
/// \p offset.
inline bool beginsLineAfterWs(StringRef text, size_t offset) {
  return rangeContainsOnlyWs(text, lineStartOffset(text, offset), offset);
}

/// True iff only line whitespace appears between \p offset and the containing
/// line end.
inline bool endsLineBeforeWs(StringRef text, size_t offset) {
  return rangeContainsOnlyWs(text, offset, lineEndOffset(text, offset));
}

/// True iff \p replacement is a parenthesized callable head followed by one or
/// more balanced call-suffix groups, with no trailing non-comment content.
bool shouldPreserveFinalCallSuffixGroup(StringRef replacement);

/// Extend an invocation end offset over trailing chained-call suffix groups
/// when the replacement is no longer directly callable.
uint64_t extendChainedCallEnd(StringRef fileText, uint64_t invEnd,
                              StringRef replacement);

/// Return true iff the first non-whitespace bytes of \p s start with \p lit.
bool startsWithAfterWs(StringRef s, StringRef lit);

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

/// True iff \p offset is a beginning-of-line in \p text.
inline bool isBOL(StringRef text, size_t offset) {
  size_t o = std::clamp(offset, size_t(0), text.size());
  return (o == 0) || (text[o - 1] == '\n');
}

/// True iff appending at the end of \p s would occur at beginning-of-line.
inline bool outAtBOL(StringRef s) { return s.empty() || s.back() == '\n'; }

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

} // namespace stringutils
} // namespace refold
} // namespace clang

#endif // LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_STRINGUTILS_H
