//===--- RefoldMacroTupleHelpers.cpp ----------------------------*- C++ -*-===//
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

} // namespace refold
} // namespace clang
