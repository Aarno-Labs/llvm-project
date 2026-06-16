//===--- RefoldEngine.ArgTextRecovery.inc -------------------*- C++ -*-===//
//
// `RefoldEngine` members physically split out of `RefoldEngine.cpp`.
//
// This fragment groups methods that recover or normalize raw macro argument
// text from textual spellings. These helpers answer "what is the raw argument
// content represented by this invocation text or stringified literal?"
//
// This file is included directly by `RefoldEngine.cpp`, so this is only a
// physical file split. No ownership, access, or behavioral semantics change.
//
//===----------------------------------------------------------------------===//

std::optional<std::string>
RefoldEngine::UnstringifyLiteralToArgText(StringRef literalTok,
                                          bool allowTopLevelComma) const {
  StringRef s = literalTok.trim();
  if (s.empty())
    return std::nullopt;

  // Find the opening quote (after any optional prefix)
  size_t q = s.find('\"');
  if (q == StringRef::npos)
    return std::nullopt;

  // Validate the prefix (L, u, U, u8)
  StringRef prefix = s.substr(0, q);
  if (!prefix.empty()) {
    if (prefix != "L" && prefix != "u" && prefix != "U" && prefix != "u8")
      return std::nullopt;
  }

  // Ensure it has a closing quote and is at least ""
  if (s.size() < q + 2 || s.back() != '\"')
    return std::nullopt;

  // Extract content between the quotes
  StringRef body = s.slice(q + 1, s.size() - 1);

  std::string out;
  out.reserve(body.size());

  for (size_t i = 0; i < body.size(); ++i) {
    const char c = body[i];
    if (c == '\\' && i + 1 < body.size()) {
      const char n = body[i + 1];
      // Stringification only escapes backslashes and quotes
      if (n == '\\' || n == '\"') {
        out.push_back(n);
        i++;
        continue;
      }
      // Preserve other escape sequences (like \n, \t) as-is
      out.push_back(c);
      out.push_back(n);
      i++;
      continue;
    }
    out.push_back(c);
  }

  // Safety check: only commas inside nested parentheses remain inside one
  // function-like macro actual when we synthesize invocation text. Brackets and
  // braces are ordinary preprocessing tokens for argument collection, so a
  // comma in `arr[1, 2]` or `{1, 2}` would still split the call-site argument
  // list. Allow callers that are only normalizing for comparison (not
  // re-synthesizing invocation text) to opt out.
  if (!allowTopLevelComma) {
    const SourceLocation BaseLoc = SourceLocation::getFromRawEncoding(1);
    std::string LexBuf = out;
    LexBuf.push_back('\0');
    const char *BufStart = LexBuf.data();
    const char *BufEnd = BufStart + out.size();
    Lexer Lex(BaseLoc, lexLang_, BufStart, BufStart, BufEnd);

    int ParenDepth = 0;
    Token Tok;

    while (true) {
      Lex.LexFromRawLexer(Tok);
      if (Tok.is(tok::eof))
        break;
      if (Tok.is(tok::comment))
        continue;

      switch (Tok.getKind()) {
      case tok::l_paren:
        ++ParenDepth;
        break;
      case tok::r_paren:
        if (ParenDepth > 0)
          --ParenDepth;
        break;
      case tok::comma:
        if (ParenDepth == 0)
          return std::nullopt;
        break;
      default:
        break;
      }
    }
  }

  // Macros cannot have raw newlines in arguments unless escaped/continued
  if (out.find('\n') != std::string::npos ||
      out.find('\r') != std::string::npos)
    return std::nullopt;

  return out;
}

std::optional<std::vector<std::pair<size_t, size_t>>>
RefoldEngine::LexMacroInvocationActualContentRanges(StringRef invText,
                                                    const LangOptions &lang) {
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
