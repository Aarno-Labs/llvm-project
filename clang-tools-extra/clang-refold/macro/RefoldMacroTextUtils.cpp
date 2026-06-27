//===--- RefoldMacroTextUtils.cpp -----------------------------*- C++ -*-===//
//
// Shared macro-text lexer predicates.
//
//===----------------------------------------------------------------------===//

#include "macro/RefoldMacroTextUtils.h"

#include "clang/Basic/SourceLocation.h"
#include "clang/Basic/TokenKinds.h"
#include "clang/Lex/Lexer.h"

#include <string>

using namespace llvm;

namespace clang {
namespace refold {

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

} // namespace refold
} // namespace clang
