//===--- RefoldMacroTupleHelpers.cpp ----------------------------*- C++ -*-===//
//
// Implementation of the shared caller-tuple parsing helpers used by macro
// replay paths that need lexer-aware top-level tuple element splitting and
// element-local trivia trimming, and of chained-call suffix extension, whose
// balanced-group scanners are file-local.
//
//===----------------------------------------------------------------------===//

#include "macro/RefoldMacroTupleHelpers.h"

#include "support/StringUtils.h"

#include "clang/Basic/SourceLocation.h"
#include "clang/Basic/TokenKinds.h"
#include "clang/Lex/Lexer.h"

using namespace llvm;

namespace clang {
namespace refold {

namespace {

/// Build one comma-separated element slice and optionally accept an empty
/// trimmed payload.
///
/// Caller tuple elements reject empty payloads because an empty tuple slot does
/// not identify a source-edit obligation by itself.  Macro-call actual lists are
/// different: an empty actual is a real positional argument, as in `F(, x)`, and
/// must be preserved so the callee's formal slots remain aligned.
bool computeTrimmedCommaElement(StringRef text, size_t begin, size_t end,
                                bool allowEmpty, TupleElementSlice &out) {
  out.begin = begin;
  out.end = end;
  out.trimBegin = begin;
  out.trimEnd = end;
  std::tie(out.trimBegin, out.trimEnd) =
      stringutils::trimWsRange(text, begin, end);
  return allowEmpty || out.trimBegin != out.trimEnd;
}

/// Lexer-aware top-level comma splitter shared by tuple and macro-actual
/// parsing.  `allowEmpty` is the only policy difference between the two uses.
bool splitTopLevelCommaSeparatedElements(
    StringRef text, const LangOptions &lang, bool allowEmpty,
    SmallVectorImpl<TupleElementSlice> &out) {
  out.clear();

  if (text.empty() && !allowEmpty)
    return false;

  const SourceLocation baseLoc = SourceLocation::getFromRawEncoding(1);
  std::string lexBuf = text.str();
  lexBuf.push_back('\0');
  const char *bufStart = lexBuf.data();
  const char *bufEnd = bufStart + text.size();
  Lexer lexer(baseLoc, lang, bufStart, bufStart, bufEnd);

  size_t elementBegin = 0;

  int parenDepth = 0;
  int bracketDepth = 0;
  int braceDepth = 0;
  Token token;

  while (true) {
    lexer.LexFromRawLexer(token);
    if (token.is(tok::eof))
      break;

    if (token.is(tok::comment))
      continue;

    const size_t tokBegin =
        token.getLocation().getRawEncoding() - baseLoc.getRawEncoding();
    const size_t tokEnd = tokBegin + token.getLength();

    switch (token.getKind()) {
    case tok::l_paren:
      ++parenDepth;
      break;
    case tok::r_paren:
      if (parenDepth > 0)
        --parenDepth;
      break;
    case tok::l_square:
      ++bracketDepth;
      break;
    case tok::r_square:
      if (bracketDepth > 0)
        --bracketDepth;
      break;
    case tok::l_brace:
      ++braceDepth;
      break;
    case tok::r_brace:
      if (braceDepth > 0)
        --braceDepth;
      break;
    case tok::comma:
      if (parenDepth == 0 && bracketDepth == 0 && braceDepth == 0) {
        TupleElementSlice elem;
        if (!computeTrimmedCommaElement(text, elementBegin, tokBegin,
                                        allowEmpty, elem))
          return false;
        out.push_back(elem);

        elementBegin = tokEnd;
      }
      break;
    default:
      break;
    }
  }

  TupleElementSlice elem;
  if (!computeTrimmedCommaElement(text, elementBegin, text.size(), allowEmpty,
                                  elem))
    return false;
  out.push_back(elem);

  return true;
}

} // namespace

bool splitTopLevelTupleElementsWithLexer(
    StringRef text, const LangOptions &lang,
    SmallVectorImpl<TupleElementSlice> &out) {
  return splitTopLevelCommaSeparatedElements(text, lang, /*allowEmpty=*/false,
                                             out);
}

bool splitTopLevelMacroActualsWithLexer(
    StringRef text, const LangOptions &lang,
    SmallVectorImpl<TupleElementSlice> &out) {
  return splitTopLevelCommaSeparatedElements(text, lang, /*allowEmpty=*/true,
                                             out);
}

namespace {

using stringutils::isIdentPart;
using stringutils::isIdentStart;
using stringutils::isWs;
using stringutils::skipQuotedLiteral;

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

/// Find the matching ')' for a '(' while treating comments and literals as
/// opaque.
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

    if (c == '"' || c == '\'') {
      if (skipQuotedLiteral(s, i))
        --i;
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

} // namespace

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

} // namespace refold
} // namespace clang
