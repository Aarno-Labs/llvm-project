//===--- RefoldArgTextRecovery.cpp -----------------------------*- C++ -*-===//
//
// Macro-argument text recovery service implementation.
//
// This file implements the raw-lexing operations that recover macro actual
// source ranges and invert stringified literal tokens back into argument text
// when the inversion can be proven from Clang tokenization rules.
//
//===----------------------------------------------------------------------===//

#include "macro/RefoldArgTextRecovery.h"
#include "util/StringUtils.h"

#include "clang/Basic/SourceLocation.h"
#include "clang/Basic/TokenKinds.h"
#include "clang/Lex/Lexer.h"

using namespace llvm;

namespace clang {
namespace refold {

RefoldArgTextRecovery::RefoldArgTextRecovery(const LangOptions &lexLang)
    : lexLang_(lexLang) {}

std::optional<std::string>
RefoldArgTextRecovery::UnstringifyLiteralToArgText(
    StringRef literalTok, bool allowTopLevelComma) const {
  StringRef s = literalTok.trim();
  if (s.empty())
    return std::nullopt;

  // Locate the opening quote after any accepted string-literal prefix.  The
  // caller has already selected a single token spelling; this check rejects
  // malformed or non-string tokens rather than attempting recovery by guesswork.
  size_t q = s.find('"');
  if (q == StringRef::npos)
    return std::nullopt;

  // Accept only the ordinary/wide/Unicode prefixes that the previous engine
  // helper supported.  Unknown prefixes fail closed so this inverse operation
  // never invents source spelling for an unsupported literal kind.
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

} // namespace refold
} // namespace clang
