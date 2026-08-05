//===--- RefoldTokenTextAnalysis.cpp ----------------------------*- C++ -*-===//
//
// Raw-lexer based token/text observation predicates.
//
//===----------------------------------------------------------------------===//

#include "source/RefoldTokenTextAnalysis.h"

#include "source/RefoldPreprocessingDirectiveScanner.h"

#include "clang/Basic/SourceLocation.h"
#include "clang/Basic/TokenKinds.h"
#include "clang/Lex/Lexer.h"

#include <string>

using namespace llvm;

namespace clang {
namespace refold {
namespace {

/// Return whether \p text contains a preprocessing directive introducer.
///
/// The shared logical-line scanner applies escaped-newline deletion, treats
/// comments as whitespace, honours the active digraph/trigraph mode, and
/// excludes `#` spellings inside comments and literals.
///
/// A scanner inconsistency reports true.  Callers use this as a macro-state
/// neutrality predicate, so an unrecovered directive interval must stay
/// fail-closed rather than read as "no directive here".
static bool lineHasPreprocessingDirectiveIntroducer(
    StringRef text, const clang::LangOptions &lexLang) {
  PreprocessingDirectiveScanResult scan =
      scanPreprocessingDirectives(text, lexLang);
  return !scan.directives.empty() || !scan.diagnostics.empty();
}

/// Convert a raw-lexer token location into an offset relative to the scratch
/// buffer's artificial base location.
static size_t tokenOffsetFromBase(const Token &token, SourceLocation baseLoc) {
  return token.getLocation().getRawEncoding() - baseLoc.getRawEncoding();
}

/// Return the one-past-end byte offset for a raw-lexer token in the same
/// scratch-buffer coordinate system as tokenOffsetFromBase().
static size_t tokenEndOffsetFromBase(const Token &token,
                                     SourceLocation baseLoc) {
  return tokenOffsetFromBase(token, baseLoc) + token.getLength();
}

} // namespace

RefoldTokenTextAnalysis::RefoldTokenTextAnalysis(
    const clang::LangOptions &lexLang)
    : lexLang_(lexLang) {}

std::optional<size_t>
RefoldTokenTextAnalysis::FirstRawIdentifierObservationOffsetInText(
    StringRef name, StringRef text) const {
  if (name.empty() || text.empty())
    return std::nullopt;

  const SourceLocation baseLoc = SourceLocation::getFromRawEncoding(1);
  std::string lexBuf = text.str();
  lexBuf.push_back('\0');

  const char *bufStart = lexBuf.data();
  const char *bufEnd = bufStart + text.size();
  Lexer lexer(baseLoc, lexLang_, bufStart, bufStart, bufEnd);
  lexer.SetCommentRetentionState(true);

  Token token;
  while (true) {
    lexer.LexFromRawLexer(token);
    if (token.is(tok::eof))
      return std::nullopt;

    // Comments are retained so the raw lexer can step over them explicitly, but
    // macro-state observations inside comments are irrelevant.
    if (token.is(tok::comment))
      continue;

    if (!token.is(tok::raw_identifier) && !token.is(tok::identifier))
      continue;

    const size_t localBegin = tokenOffsetFromBase(token, baseLoc);
    const size_t localEnd = tokenEndOffsetFromBase(token, baseLoc);
    if (localEnd < localBegin || localEnd > text.size())
      continue;

    if (text.slice(localBegin, localEnd) == name)
      return localBegin;
  }
}

bool RefoldTokenTextAnalysis::RawIdentifierAppearsInText(StringRef name,
                                                         StringRef text) const {
  return FirstRawIdentifierObservationOffsetInText(name, text).has_value();
}

std::optional<size_t>
RefoldTokenTextAnalysis::FirstFunctionLikeInvocationOffsetInText(
    StringRef name, StringRef text, StringRef suffix) const {
  if (name.empty() || text.empty())
    return std::nullopt;

  const SourceLocation baseLoc = SourceLocation::getFromRawEncoding(1);
  std::string lexBuf;
  lexBuf.reserve(text.size() + suffix.size() + 1);
  lexBuf.append(text.begin(), text.end());
  lexBuf.append(suffix.begin(), suffix.end());
  lexBuf.push_back('\0');

  const char *bufStart = lexBuf.data();
  const char *bufEnd = bufStart + text.size() + suffix.size();
  Lexer lexer(baseLoc, lexLang_, bufStart, bufStart, bufEnd);
  lexer.SetCommentRetentionState(true);

  std::optional<size_t> pendingNameBegin;
  Token token;
  while (true) {
    lexer.LexFromRawLexer(token);
    if (token.is(tok::eof))
      return std::nullopt;

    // Whitespace is not returned by the raw lexer, and comments are not
    // preprocessing tokens for function-like invocation adjacency.
    if (token.is(tok::comment))
      continue;

    if (pendingNameBegin) {
      if (token.is(tok::l_paren))
        return *pendingNameBegin;
      pendingNameBegin.reset();
    }

    if (!token.is(tok::raw_identifier) && !token.is(tok::identifier))
      continue;

    const size_t localBegin = tokenOffsetFromBase(token, baseLoc);
    const size_t localEnd = tokenEndOffsetFromBase(token, baseLoc);
    if (localEnd < localBegin || localEnd > text.size())
      continue;

    if (StringRef(lexBuf).slice(localBegin, localEnd) == name)
      pendingNameBegin = localBegin;
  }
}

bool RefoldTokenTextAnalysis::FunctionLikeInvocationAppearsInText(
    StringRef name, StringRef text, StringRef suffix) const {
  return FirstFunctionLikeInvocationOffsetInText(name, text, suffix)
      .has_value();
}

bool RefoldTokenTextAnalysis::TextMentionsLineObserver(StringRef text) const {
  return RawIdentifierAppearsInText("__LINE__", text);
}

bool RefoldTokenTextAnalysis::TextMentionsFileObserver(StringRef text) const {
  return RawIdentifierAppearsInText("__FILE__", text) ||
         RawIdentifierAppearsInText("__FILE_NAME__", text) ||
         RawIdentifierAppearsInText("__BASE_FILE__", text);
}

bool RefoldTokenTextAnalysis::TextMentionsCounterObserver(
    StringRef text) const {
  return RawIdentifierAppearsInText("__COUNTER__", text);
}

bool RefoldTokenTextAnalysis::TextContainsDirectiveLine(StringRef text) const {
  return lineHasPreprocessingDirectiveIntroducer(text, lexLang_);
}

} // namespace refold
} // namespace clang
