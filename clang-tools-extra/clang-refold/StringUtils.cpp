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

#include "StringUtils.h"
#include "llvm/ADT/StringRef.h"

#include <cstdio>
#include <string>

namespace clang {
namespace refold {
namespace stringutils {

/// Return true when the trimmed spelling is exactly one ASCII identifier.
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

/// Return true when a replacement can safely keep following call suffixes.
bool isIdentifierOrSimpleCallExpr(StringRef replacement) {
  StringRef s = replacement.trim();
  if (s.empty())
    return false;

  if (isIdentifierOnly(trimEdgeSpaces(s)))
    return true;

  // Accept the common "IDENT(...)" shape (with balanced parens and only
  // trailing whitespace).
  size_t i = 0;
  if (!isIdentStart(s[i]))
    return false;
  ++i;
  while (i < s.size() && isIdentPart(s[i]))
    ++i;

  i = skipWsAndComments(s, i);
  if (i >= s.size() || s[i] != '(')
    return false;

  size_t rparen = findMatchingRParen(s, i);
  if (rparen == StringRef::npos)
    return false;

  i = skipWsAndComments(s, rparen + 1);
  return i == s.size();
}

/// Advance over whitespace and complete line/block comments from \p i.
size_t skipWsAndComments(StringRef s, size_t i) {
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

/// Find the matching ')' for a '(' while treating comments and literals as opaque.
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

/// Trim only ASCII spaces and tabs from both ends of \p s.
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

/// Return one StringRef slice per byte in \p s.
std::vector<StringRef> splitChars(StringRef s) {
  std::vector<StringRef> refs;
  refs.reserve(s.size());
  for (size_t i = 0; i < s.size(); ++i)
    refs.push_back(s.substr(i, 1));
  return refs;
}

/// Canonicalize raw text recovered from an inverse stringification witness.
std::optional<std::string> canonicalizeStringifyInversePayload(StringRef raw0) {
  StringRef raw = raw0.trim();

  enum class LexState { Normal, String, Char };
  LexState state = LexState::Normal;

  std::string out;
  out.reserve(raw.size());

  // Outside literals, stringify collapses each run of whitespace to one space
  // between tokens. Delay emission until the next real byte proves it is needed.
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
      // Normalize token-separating whitespace, reject comments, and otherwise
      // copy bytes verbatim until entering a literal state.
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
      // Literal bodies are preserved byte-for-byte except that a dangling escape
      // makes the inverse witness invalid.
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

/// Decide whether the last chained-call suffix belongs to the replacement.
bool shouldPreserveFinalCallSuffixGroup(StringRef replacement) {
  StringRef s = replacement.trim();
  if (s.empty())
    return false;

  size_t pos = skipWsAndComments(s, 0);
  if (pos >= s.size() || s[pos] != '(')
    return false;

  const size_t headEnd = findMatchingRParen(s, pos);
  if (headEnd == StringRef::npos)
    return false;

  const size_t firstInside = skipWsAndComments(s, pos + 1);
  if (firstInside >= headEnd)
    return false;

  pos = skipWsAndComments(s, headEnd + 1);
  bool sawCallGroup = false;
  while (pos < s.size() && s[pos] == '(') {
    const size_t groupEnd = findMatchingRParen(s, pos);
    if (groupEnd == StringRef::npos)
      return false;
    sawCallGroup = true;
    pos = skipWsAndComments(s, groupEnd + 1);
  }

  return sawCallGroup && pos == s.size();
}

/// Extend an invocation byte end over chained-call suffixes when required.
uint64_t extendChainedCallEnd(StringRef fileText, uint64_t invEnd,
                              StringRef replacement) {
  if (invEnd > fileText.size())
    return invEnd;

  if (isIdentifierOrSimpleCallExpr(replacement))
    return invEnd;

  const bool preserveFinalSuffixGroup =
      shouldPreserveFinalCallSuffixGroup(replacement);

  size_t pos = skipWsAndComments(fileText, static_cast<size_t>(invEnd));
  if (pos >= fileText.size() || fileText[pos] != '(')
    return invEnd;

  std::vector<uint64_t> groupEnds;
  // Collect each immediately adjacent balanced call-suffix group. The caller
  // later decides whether the final group must remain attached to the new head.
  while (pos < fileText.size() && fileText[pos] == '(') {
    const size_t r = findMatchingRParen(fileText, pos);
    if (r == StringRef::npos)
      break;
    groupEnds.push_back(static_cast<uint64_t>(r + 1));
    pos = skipWsAndComments(fileText, r + 1);
  }

  if (groupEnds.empty())
    return invEnd;

  size_t consume = groupEnds.size();
  if (preserveFinalSuffixGroup && consume > 0)
    consume -= 1;
  return (consume == 0) ? invEnd : groupEnds[consume - 1];
}

/// Return true iff \p lit appears after leading whitespace in \p s.
bool startsWithAfterWs(StringRef s, StringRef lit) {
  size_t i = 0;
  while (i < s.size()) {
    char c = s[i];
    if (c != ' ' && c != '\t' && c != '\r' && c != '\n')
      break;
    ++i;
  }
  return s.drop_front(i).starts_with(lit);
}

/// Quote and escape \p s as a C string literal spelling.
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

/// Quote \p s as a minimal C string literal spelling.
std::string quoteCStringLiteral(StringRef s) {
  std::string out;
  out.reserve(s.size() + 2);
  out.push_back('"');
  for (char c : s) {
    if (c == '\\' || c == '"')
      out.push_back('\\');
    out.push_back(c);
  }
  out.push_back('"');
  return out;
}

} // namespace stringutils
} // namespace refold
} // namespace clang
