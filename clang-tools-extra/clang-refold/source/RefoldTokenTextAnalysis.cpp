//===--- RefoldTokenTextAnalysis.cpp ----------------------------*- C++ -*-===//
//
// Raw-lexer based token/text observation predicates.
//
//===----------------------------------------------------------------------===//

#include "source/RefoldTokenTextAnalysis.h"

#include "core/RefoldLog.h"
#include "source/RefoldPreprocessingDirectiveScanner.h"

#include "clang/Basic/SourceLocation.h"
#include "clang/Basic/TokenKinds.h"
#include "clang/Lex/Lexer.h"

#include "llvm/ADT/STLExtras.h"

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

  // The only way this returns an offset is a token whose bytes, taken from
  // `text`, equal `name`.  So `name` occurring nowhere in `text` as a plain
  // substring is a complete answer, and it is one a byte search gives without
  // copying the payload and lexing it to the end.  The reverse does not hold --
  // a substring hit may be part of a longer identifier, inside a string
  // literal, or in a comment -- so a hit still has to be lexed.  This only
  // decides the negative case, which is the common one: most payloads mention
  // most macros not at all.
  if (text.find(name) == StringRef::npos)
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

void RefoldTokenTextAnalysis::CollectRawIdentifiersInText(
    StringRef text, SmallVectorImpl<StringRef> &out) const {
  out.clear();
  if (text.empty())
    return;

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
      return;

    // Comments are retained so the raw lexer steps over them explicitly; an
    // identifier spelled inside one is not a preprocessing token.
    if (token.is(tok::comment))
      continue;
    if (!token.is(tok::raw_identifier) && !token.is(tok::identifier))
      continue;

    const size_t localBegin = tokenOffsetFromBase(token, baseLoc);
    const size_t localEnd = tokenEndOffsetFromBase(token, baseLoc);
    if (localEnd < localBegin || localEnd > text.size())
      continue;

    // The spelling is taken from `text`, not from the scratch copy, so the
    // returned references stay valid for the caller's buffer.
    const StringRef spelling = text.slice(localBegin, localEnd);
    if (!llvm::is_contained(out, spelling))
      out.push_back(spelling);
  }
}

std::optional<size_t>
RefoldTokenTextAnalysis::FirstFunctionLikeInvocationOffsetInText(
    StringRef name, StringRef text, StringRef suffix) const {
  if (name.empty() || text.empty())
    return std::nullopt;

  // The NAME token must start in `text` and the loop below rejects any token
  // reaching past `text`, so a match requires `name` to occur in `text` as a
  // plain substring -- the suffix contributes only the following `(`.  Deciding
  // that negative case with a byte search avoids concatenating the payload and
  // suffix into a fresh buffer and lexing all of it.  A substring hit proves
  // nothing on its own and is still lexed.
  if (text.find(name) == StringRef::npos)
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

bool RefoldTokenTextAnalysis::DirectiveLineInTextCouldObserveDefinedness(
    StringRef name, StringRef text) const {
  if (name.empty() || text.empty())
    return false;

  const PreprocessingDirectiveScanResult scan =
      scanPreprocessingDirectives(text, lexLang_);
  if (!scan.IsComplete())
    return true;

  for (const PreprocessingDirectiveLine &line : scan.directives) {
    if (!line.IsValid() || line.end > text.size())
      return true;
    const StringRef directiveText = text.slice(line.begin, line.end);
    if (!FirstRawIdentifierObservationOffsetInText(name, directiveText))
      continue;

    // A `#define` stores its replacement list as tokens and does not expand it
    // at definition time, so naming the macro in a macro body is not a test of
    // whether that macro is defined -- the name is expanded later, at each
    // invocation, exactly as it would have been originally.  Every other
    // directive either tests definedness directly (`#if`, `#ifdef`, `#elif`
    // and friends) or macro-expands its operands (`#include`, `#line`,
    // `#pragma`), so naming the macro there can change what the directive
    // does.
    if (line.keyword != "define")
      return true;

    // `#define HAS(x) defined(x)` smuggles a definedness test into a body that
    // a later `#if` evaluates.  A body that spells `defined` at all is refused
    // rather than analysed.
    if (FirstRawIdentifierObservationOffsetInText("defined", directiveText))
      return true;
  }
  return false;
}

} // namespace refold
} // namespace clang
