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
RefoldEngine::ParseMacroInvocationArgContentRanges(StringRef invText) {
  // Locate the start of the argument list.
  size_t open = invText.find('(');
  if (open == StringRef::npos)
    return std::nullopt;

  std::vector<std::pair<size_t, size_t>> out;
  size_t n = invText.size();

  // Special-case: an empty argument list "()" (or only whitespace/comments
  // inside) denotes zero macro arguments, not one empty argument. That matters
  // for args-only lifting of zero-arg call chains such as "PICK1()(10)".
  const size_t afterOpen = open + 1;
  const size_t firstTok = stringutils::skipWsAndComments(invText, afterOpen);
  if (firstTok < n && invText[firstTok] == ')')
    return out;

  // Macro argument collection is governed by parenthesis nesting only.  A comma
  // separates arguments unless it is inside a nested parenthesized token group;
  // brackets and braces are ordinary preprocessing tokens for this purpose.
  // This is deliberately different from C expression/list splitting helpers,
  // where `{...}` and `[...]` may form balanced subexpressions.  For example,
  // `M(arr[1, 2], 3)` supplies three macro arguments (`arr[1`, `2]`, `3`),
  // while `M((1, 2), 3)` supplies two.
  uint32_t parenDepth = 0;
  bool inS = false;   // Inside a single-quoted character literal.
  bool inD = false;   // Inside a double-quoted string literal.

  size_t argStart = open + 1;
  for (size_t i = argStart; i < n; i++) {
    char c = invText[i];

    // Handle escaping and termination inside character literals.
    if (inS) {
      if (c == '\\' && i + 1 < n) {
        i++; // Skip escaped characters.
        continue;
      }
      if (c == '\'')
        inS = false;
      continue;
    }

    // Handle escaping and termination inside string literals.
    if (inD) {
      if (c == '\\' && i + 1 < n) {
        i++; // Skip escaped characters.
        continue;
      }
      if (c == '"')
        inD = false;
      continue;
    }

    // Enter literal state if a quote is encountered.
    if (c == '\'') {
      inS = true;
      continue;
    }
    if (c == '"') {
      inD = true;
      continue;
    }

    // Comments are opaque preprocessing tokens.  Delimiters and commas inside
    // them must not affect macro-argument boundaries.
    if (c == '/' && i + 1 < n) {
      if (invText[i + 1] == '*') {
        const size_t close = invText.find("*/", i + 2);
        if (close == StringRef::npos)
          return std::nullopt;
        i = close + 1;
        continue;
      }
      if (invText[i + 1] == '/') {
        const size_t nl = invText.find('\n', i + 2);
        if (nl == StringRef::npos)
          return std::nullopt;
        i = nl;
        continue;
      }
    }

    // Only nested parentheses protect commas during macro-argument
    // collection.  Braces and brackets intentionally do not participate here.
    if (c == '(') {
      parenDepth++;
      continue;
    }

    // Handle the closing of a group or the entire argument list.
    if (c == ')') {
      if (parenDepth == 0) {
        // Final argument reached at the closing parenthesis of the call.
        out.push_back(stringutils::trimWsRange(invText, argStart, i));
        return out;
      }
      parenDepth--;
      continue;
    }

    // A comma at depth 0 signifies the end of one macro argument.
    if (c == ',' && parenDepth == 0) {
      out.push_back(stringutils::trimWsRange(invText, argStart, i));
      argStart = i + 1;
    }
  }

  // If the loop finishes without hitting the final ')', the syntax is invalid.
  return std::nullopt;
}
