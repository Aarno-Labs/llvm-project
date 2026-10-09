//===--- RefoldArgTextRecovery.cpp ------------------------------*- C++ -*-===//
//
// Macro-argument text recovery service implementation.
//
// This file implements the raw-lexing operations that recover macro actual
// source ranges and invert stringified literal tokens back into argument text
// when the inversion can be proven from Clang tokenization rules.
//
//===----------------------------------------------------------------------===//

#include "macro/RefoldArgTextRecovery.h"

#include "support/StringUtils.h"

#include "clang/Basic/SourceLocation.h"
#include "clang/Basic/TokenKinds.h"
#include "clang/Lex/Lexer.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/SmallVector.h"

#include <algorithm>
#include <string>

using namespace llvm;

namespace clang {
namespace refold {

RefoldArgTextRecovery::RefoldArgTextRecovery(const LangOptions &lexLang)
    : lexLang_(lexLang) {}

std::optional<std::string> RefoldArgTextRecovery::UnstringifyLiteralToArgText(
    StringRef literalTok, bool allowTopLevelComma) const {
  StringRef s = literalTok.trim();
  if (s.empty())
    return std::nullopt;

  // Locate the opening quote after any accepted string-literal prefix.  The
  // caller has already selected a single token spelling; this check rejects
  // malformed or non-string tokens rather than attempting recovery by
  // guesswork.
  size_t q = s.find('"');
  if (q == StringRef::npos)
    return std::nullopt;

  // Accept only the ordinary/wide/Unicode prefixes recognized by this
  // inverse string-literal path. Unknown prefixes fail closed so this inverse
  // operation never invents source spelling for an unsupported literal kind.
  StringRef prefix = s.substr(0, q);
  if (!prefix.empty()) {
    if (prefix != "L" && prefix != "u" && prefix != "U" && prefix != "u8")
      return std::nullopt;
  }

  // Require a closing quote and a syntactically non-empty quote pair.
  if (s.size() < q + 2 || s.back() != '"')
    return std::nullopt;

  StringRef body = s.slice(q + 1, s.size() - 1);

  std::string out;
  out.reserve(body.size());

  for (size_t i = 0; i < body.size(); ++i) {
    const char c = body[i];
    if (c == '\\' && i + 1 < body.size()) {
      const char n = body[i + 1];
      // Stringification introduces escapes for backslashes and quotes.  Undo
      // exactly those escapes; preserve all other escape sequences verbatim so
      // the recovered argument does not silently change token spelling.
      if (n == '\\' || n == '"') {
        out.push_back(n);
        ++i;
        continue;
      }
      out.push_back(c);
      out.push_back(n);
      ++i;
      continue;
    }
    out.push_back(c);
  }

  // A recovered argument that contains a top-level comma cannot be safely
  // reinserted into a synthesized macro call unless the caller is only using
  // the value for comparison/normalization.  Macro argument collection is
  // protected by parentheses only; brackets and braces intentionally do not
  // affect this depth check.
  if (!allowTopLevelComma) {
    const SourceLocation baseLoc = SourceLocation::getFromRawEncoding(1);
    std::string lexBuf = out;
    lexBuf.push_back('\0');
    const char *bufStart = lexBuf.data();
    const char *bufEnd = bufStart + out.size();
    Lexer lexer(baseLoc, lexLang_, bufStart, bufStart, bufEnd);

    int parenDepth = 0;
    Token token;

    while (true) {
      lexer.LexFromRawLexer(token);
      if (token.is(tok::eof))
        break;
      if (token.is(tok::comment))
        continue;

      switch (token.getKind()) {
      case tok::l_paren:
        ++parenDepth;
        break;
      case tok::r_paren:
        if (parenDepth > 0)
          --parenDepth;
        break;
      case tok::comma:
        if (parenDepth == 0)
          return std::nullopt;
        break;
      default:
        break;
      }
    }
  }

  // Raw physical newlines cannot appear in a macro invocation actual unless
  // represented through source continuation.  Do not synthesize such text from
  // an edited string literal.
  if (out.find('\n') != std::string::npos ||
      out.find('\r') != std::string::npos)
    return std::nullopt;

  return out;
}

std::optional<std::vector<std::pair<size_t, size_t>>>
RefoldArgTextRecovery::LexMacroInvocationActualContentRanges(
    StringRef invText, const LangOptions &lang) {
  const SourceLocation baseLoc = SourceLocation::getFromRawEncoding(1);
  std::string lexBuf = invText.str();
  lexBuf.push_back('\0');
  const char *bufStart = lexBuf.data();
  const char *bufEnd = bufStart + invText.size();
  Lexer lexer(baseLoc, lang, bufStart, bufStart, bufEnd);

  auto tokenOffset = [&](const Token &token) -> std::optional<size_t> {
    const unsigned raw = token.getLocation().getRawEncoding();
    const unsigned base = baseLoc.getRawEncoding();
    if (raw < base)
      return std::nullopt;
    const size_t off = static_cast<size_t>(raw - base);
    if (off > invText.size() || off + token.getLength() > invText.size())
      return std::nullopt;
    return off;
  };

  std::vector<std::pair<size_t, size_t>> out;
  Token token;
  bool sawOpenParen = false;
  bool sawAnyTokenInCurrentActual = false;
  size_t argStart = 0;
  unsigned parenDepth = 0;

  while (true) {
    lexer.LexFromRawLexer(token);
    if (token.is(tok::eof))
      break;
    if (token.is(tok::comment))
      continue;

    std::optional<size_t> offOpt = tokenOffset(token);
    if (!offOpt)
      return std::nullopt;
    const size_t off = *offOpt;

    if (!sawOpenParen) {
      if (token.is(tok::l_paren)) {
        sawOpenParen = true;
        argStart = off + token.getLength();
      }
      continue;
    }

    if (token.is(tok::l_paren)) {
      ++parenDepth;
      sawAnyTokenInCurrentActual = true;
      continue;
    }

    if (token.is(tok::r_paren)) {
      if (parenDepth == 0) {
        if (!sawAnyTokenInCurrentActual && out.empty())
          return out;
        out.push_back(stringutils::trimWsRange(invText, argStart, off));
        return out;
      }
      --parenDepth;
      sawAnyTokenInCurrentActual = true;
      continue;
    }

    // Macro argument collection is governed by nested parentheses only.
    // Brackets and braces are ordinary preprocessing tokens for this purpose:
    // `M(arr[1, 2], 3)` has three actuals, while `M((1, 2), 3)` has two.
    if (token.is(tok::comma) && parenDepth == 0) {
      out.push_back(stringutils::trimWsRange(invText, argStart, off));
      argStart = off + token.getLength();
      sawAnyTokenInCurrentActual = false;
      continue;
    }

    sawAnyTokenInCurrentActual = true;
  }

  return std::nullopt;
}

bool refoldMacroActualHasTopLevelComma(StringRef text,
                                       const LangOptions &lang) {
  // RawLexer needs a stable, nul-terminated scratch buffer and an artificial
  // source location so token byte offsets can be recovered deterministically.
  const SourceLocation baseLoc = SourceLocation::getFromRawEncoding(1);
  std::string lexBuf = text.str();
  lexBuf.push_back('\0');
  const char *bufStart = lexBuf.data();
  const char *bufEnd = bufStart + text.size();
  Lexer lexer(baseLoc, lang, bufStart, bufStart, bufEnd);
  lexer.SetCommentRetentionState(true);

  int parenDepth = 0;
  Token token;

  while (true) {
    lexer.LexFromRawLexer(token);
    if (token.is(tok::eof))
      return false;
    if (token.is(tok::comment))
      continue;

    if (token.is(tok::comma) && parenDepth == 0)
      return true;

    // Macro argument collection protects commas only with parentheses.  Update
    // the nesting state after the comma test so the comma itself is classified
    // using the depth that was active before it was consumed.
    switch (token.getKind()) {
    case tok::l_paren:
      ++parenDepth;
      break;
    case tok::r_paren:
      if (parenDepth > 0)
        --parenDepth;
      break;
    default:
      break;
    }
  }
}

bool refoldMacroActualSubrangeIsFreeStanding(StringRef actual, size_t begin,
                                             size_t end,
                                             const LangOptions &lang) {
  if (begin > end || end > actual.size())
    return false;

  const SourceLocation baseLoc = SourceLocation::getFromRawEncoding(1);
  std::string lexBuf = actual.str();
  lexBuf.push_back('\0');
  const char *bufStart = lexBuf.data();
  const char *bufEnd = bufStart + actual.size();
  Lexer lexer(baseLoc, lang, bufStart, bufStart, bufEnd);
  lexer.SetCommentRetentionState(true);

  // Depth of the parentheses open before `begin`, and of those opened inside
  // the run.  The run is free-standing when the first is zero at `begin` and
  // the second never closes a parenthesis it did not open and ends at zero.
  unsigned outerDepth = 0;
  unsigned runDepth = 0;
  Token token;
  while (true) {
    lexer.LexFromRawLexer(token);
    if (token.is(tok::eof))
      break;

    const unsigned raw = token.getLocation().getRawEncoding();
    if (raw < baseLoc.getRawEncoding())
      return false;
    const size_t tokBegin = raw - baseLoc.getRawEncoding();
    const size_t tokEnd = tokBegin + token.getLength();
    if (tokEnd > actual.size())
      return false;

    // A comment is a token boundary like any other: a run boundary inside one
    // is not a token boundary of the actual.
    if ((tokBegin < begin && begin < tokEnd) ||
        (tokBegin < end && end < tokEnd))
      return false;

    const bool inRun = begin <= tokBegin && tokEnd <= end && begin < end;
    if (token.is(tok::l_paren)) {
      if (inRun)
        ++runDepth;
      else if (tokEnd <= begin)
        ++outerDepth;
    } else if (token.is(tok::r_paren)) {
      if (inRun) {
        if (runDepth == 0)
          return false;
        --runDepth;
      } else if (tokEnd <= begin && outerDepth > 0) {
        --outerDepth;
      }
    }
  }

  return outerDepth == 0 && runDepth == 0;
}

namespace {

/// One raw-lexer token of a spelling: its kind, its spelling, and its
/// half-open byte range in that spelling.
struct SpellingToken {
  tok::TokenKind kind = tok::unknown;
  std::string spelling;
  size_t begin = 0;
  size_t end = 0;
};

/// Raw-lex \p text into tokens, keeping comments only when \p keepComments.
SmallVector<SpellingToken, 32>
lexSpellingTokens(StringRef text, const LangOptions &lang, bool keepComments) {
  SmallVector<SpellingToken, 32> tokens;
  const SourceLocation baseLoc = SourceLocation::getFromRawEncoding(1);
  std::string lexBuf = text.str();
  lexBuf.push_back('\0');
  const char *bufStart = lexBuf.data();
  Lexer lexer(baseLoc, lang, bufStart, bufStart, bufStart + text.size());
  lexer.SetCommentRetentionState(keepComments);
  Token token;
  while (true) {
    lexer.LexFromRawLexer(token);
    if (token.is(tok::eof))
      break;
    const size_t begin =
        token.getLocation().getRawEncoding() - baseLoc.getRawEncoding();
    const size_t end = std::min(begin + token.getLength(), text.size());
    tokens.push_back(
        {token.getKind(), text.slice(begin, end).str(), begin, end});
  }
  return tokens;
}

/// Return whether \p left and \p right, adjacent across an edit edge, spell an
/// identifier followed by `(`: the shape that invokes a function-like macro.
bool spellsCalleeBeforeArgumentList(const SpellingToken *left,
                                    const SpellingToken *right) {
  return left && right && left->kind == tok::raw_identifier &&
         right->kind == tok::l_paren;
}

/// Return whether either edge of \p middle, placed between \p prefix and
/// \p suffix, joins an identifier to a `(`.
bool editEdgeSpellsInvocation(ArrayRef<SpellingToken> prefix,
                              ArrayRef<SpellingToken> middle,
                              ArrayRef<SpellingToken> suffix) {
  const SpellingToken *prefixLast = prefix.empty() ? nullptr : &prefix.back();
  const SpellingToken *suffixFirst = suffix.empty() ? nullptr : &suffix.front();
  const SpellingToken *middleFirst =
      middle.empty() ? suffixFirst : &middle.front();
  const SpellingToken *middleLast =
      middle.empty() ? prefixLast : &middle.back();
  return spellsCalleeBeforeArgumentList(prefixLast, middleFirst) ||
         spellsCalleeBeforeArgumentList(middleLast, suffixFirst);
}

/// Return whether \p tokens are parenthesis-balanced, and report through
/// \p topLevelComma whether a comma appears outside every parenthesis they
/// open.
bool runIsParenBalanced(ArrayRef<SpellingToken> tokens, bool &topLevelComma) {
  topLevelComma = false;
  unsigned depth = 0;
  for (const SpellingToken &token : tokens) {
    if (token.kind == tok::l_paren) {
      ++depth;
    } else if (token.kind == tok::r_paren) {
      if (depth == 0)
        return false;
      --depth;
    } else if (token.kind == tok::comma && depth == 0) {
      topLevelComma = true;
    }
  }
  return depth == 0;
}

/// Return the depth of the parentheses \p tokens leave open.
unsigned openParenDepth(ArrayRef<SpellingToken> tokens) {
  unsigned depth = 0;
  for (const SpellingToken &token : tokens) {
    if (token.kind == tok::l_paren)
      ++depth;
    else if (token.kind == tok::r_paren && depth > 0)
      --depth;
  }
  return depth;
}

/// Return whether two token sequences have the same kinds and spellings.
bool sameTokenSpellings(ArrayRef<SpellingToken> lhs,
                        ArrayRef<SpellingToken> rhs) {
  if (lhs.size() != rhs.size())
    return false;
  for (size_t i = 0; i < lhs.size(); ++i)
    if (lhs[i].kind != rhs[i].kind || lhs[i].spelling != rhs[i].spelling)
      return false;
  return true;
}

} // namespace

bool refoldMacroActualRunReplacementPreservesInvocations(
    StringRef actual, size_t begin, size_t end, StringRef replacement,
    const LangOptions &lang) {
  if (begin > end || end > actual.size())
    return false;

  // Widen the edit to whole tokens.  The bytes a widened edge crosses are
  // carried into the replacement, so the edited text is the same.
  size_t runBegin = begin;
  size_t runEnd = end;
  for (const SpellingToken &token :
       lexSpellingTokens(actual, lang, /*keepComments=*/true)) {
    const bool splitsBegin = token.begin < begin && begin < token.end;
    const bool splitsEnd = token.begin < end && end < token.end;
    if (!splitsBegin && !splitsEnd)
      continue;
    if (token.kind == tok::comment)
      return false;
    if (splitsBegin)
      runBegin = token.begin;
    if (splitsEnd)
      runEnd = token.end;
  }

  const StringRef prefix = actual.take_front(runBegin);
  const StringRef suffix = actual.drop_front(runEnd);
  std::string newRun = actual.slice(runBegin, begin).str();
  newRun += replacement;
  newRun += actual.slice(end, runEnd);
  const SmallVector<SpellingToken, 32> prefixTokens =
      lexSpellingTokens(prefix, lang, /*keepComments=*/false);
  const SmallVector<SpellingToken, 32> suffixTokens =
      lexSpellingTokens(suffix, lang, /*keepComments=*/false);
  const SmallVector<SpellingToken, 32> oldRunTokens = lexSpellingTokens(
      actual.slice(runBegin, runEnd), lang, /*keepComments=*/false);
  const SmallVector<SpellingToken, 32> newRunTokens =
      lexSpellingTokens(newRun, lang, /*keepComments=*/false);

  // The edited spelling must lex as its three pieces do apart: `PAREN` then
  // `11` must not become `PAREN11`.
  SmallVector<SpellingToken, 64> expected(prefixTokens.begin(),
                                          prefixTokens.end());
  expected.append(newRunTokens.begin(), newRunTokens.end());
  expected.append(suffixTokens.begin(), suffixTokens.end());
  if (!sameTokenSpellings(
          lexSpellingTokens(prefix.str() + newRun + suffix.str(), lang,
                            /*keepComments=*/false),
          expected))
    return false;

  if (editEdgeSpellsInvocation(prefixTokens, oldRunTokens, suffixTokens) ||
      editEdgeSpellsInvocation(prefixTokens, newRunTokens, suffixTokens))
    return false;

  const bool enclosed = openParenDepth(prefixTokens) > 0;
  for (ArrayRef<SpellingToken> run : {ArrayRef<SpellingToken>(oldRunTokens),
                                      ArrayRef<SpellingToken>(newRunTokens)}) {
    bool topLevelComma = false;
    if (!runIsParenBalanced(run, topLevelComma) || (enclosed && topLevelComma))
      return false;
  }
  return true;
}

} // namespace refold
} // namespace clang
