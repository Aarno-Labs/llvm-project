//===--- RefoldMacroTupleHelpers.cpp ---------------------------*- C++ -*-===//
//
// Implementation of the shared caller-tuple parsing helpers used by macro
// replay paths that need lexer-aware top-level tuple element splitting and
// element-local trivia trimming.
//
//===----------------------------------------------------------------------===//

#include "macro/RefoldMacroTupleHelpers.h"

#include "util/StringUtils.h"

#include "clang/Basic/SourceLocation.h"
#include "clang/Basic/TokenKinds.h"
#include "clang/Lex/Lexer.h"

using namespace llvm;

namespace clang {
namespace refold {

namespace {

/// Build a tuple-element slice and reject elements whose trimmed payload is
/// empty.  Empty elements would make old/new tuple matching ambiguous.
bool computeTrimmedTupleElement(StringRef text, size_t begin, size_t end,
                                TupleElementSlice &out) {
  out.begin = begin;
  out.end = end;
  out.trimBegin = begin;
  out.trimEnd = end;
  std::tie(out.trimBegin, out.trimEnd) =
      stringutils::trimWsRange(text, begin, end);
  return out.trimBegin != out.trimEnd;
}

} // namespace

bool splitTopLevelTupleElementsWithLexer(
    StringRef text, const LangOptions &lang,
    SmallVectorImpl<TupleElementSlice> &out) {
  out.clear();

  if (text.empty())
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
        if (!computeTrimmedTupleElement(text, elementBegin, tokBegin, elem))
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
  if (!computeTrimmedTupleElement(text, elementBegin, text.size(), elem))
    return false;
  out.push_back(elem);

  return true;
}

} // namespace refold
} // namespace clang
