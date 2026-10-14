//===--- TokenTextHelpers.h --------------------------*- C++ -*-===//
//
// Token-text helpers shared by proof and edit-construction services.
//
// They keep lexical-boundary and B-token slicing proofs in one deterministic
// implementation so callers do not duplicate token-boundary padding or slicing
// rules.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_TOKENTEXTHELPERS_H
#define LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_TOKENTEXTHELPERS_H

#include "model/RefoldToken.h"
#include "support/StringUtils.h"

#include "clang/Basic/LangOptions.h"
#include "clang/Basic/SourceLocation.h"
#include "clang/Basic/TokenKinds.h"
#include "clang/Lex/Lexer.h"
#include "clang/Lex/Token.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

namespace clang {
namespace refold {

/// Return true iff a retained raw-lexer comment token has a complete spelling.
///
/// Clang's raw lexer owns the hard parts of raw-source normalization, including
/// escaped-newline handling and language-mode details.  This helper is only a
/// defensive completeness check before treating a comment token as ignorable
/// source trivia: block comments must have a real closing delimiter, while line
/// comments are complete at either newline or end-of-buffer.
inline bool rawLexerCommentTokenIsComplete(llvm::StringRef spelling) {
  if (spelling.starts_with("//"))
    return true;
  if (spelling.starts_with("/*"))
    return spelling.find("*/", 2) != llvm::StringRef::npos;
  return false;
}

/// Convert a raw-lexer token location into an offset relative to the scratch
/// buffer's artificial base location.
///
/// Several refold helpers lex short synthetic buffers with
/// `SourceLocation::getFromRawEncoding(1)` as their base.  Keeping the offset
/// calculation in this shared private header avoids relying on anonymous
/// namespace helpers in an engine translation unit.
inline size_t refoldTokenOffsetFromBase(const Token &token,
                                        SourceLocation baseLoc) {
  return token.getLocation().getRawEncoding() - baseLoc.getRawEncoding();
}

/// Return the one-past-end byte offset for a raw-lexer token in the same
/// scratch buffer coordinate system as `refoldTokenOffsetFromBase`.
inline size_t refoldTokenEndOffsetFromBase(const Token &token,
                                           SourceLocation baseLoc) {
  return refoldTokenOffsetFromBase(token, baseLoc) + token.getLength();
}

/// Minimal raw-lexer token record used for boundary hygiene checks.
///
/// Callers across the refold pipeline use this record to prove token-boundary
/// preservation without depending on textual inclusion order.
struct RefoldLexBoundaryToken {
  tok::TokenKind kind = tok::unknown;
  std::string spelling;
  size_t begin = 0;
  size_t end = 0;
  /// Whether white space or a comment precedes the token after line splicing,
  /// as the lexer reports it.  This is the fact stringification reads.
  bool leadingSpace = false;
};

/// Lex a snippet into non-comment boundary tokens for maximal-munch checks.
inline void
refoldLexBoundaryTokens(llvm::StringRef text, const LangOptions &lang,
                        llvm::SmallVectorImpl<RefoldLexBoundaryToken> &out) {
  out.clear();
  if (text.empty())
    return;

  const SourceLocation baseLoc = SourceLocation::getFromRawEncoding(1);
  std::string lexBuf = text.str();
  lexBuf.push_back('\0');
  const char *bufStart = lexBuf.data();
  const char *bufEnd = bufStart + text.size();
  Lexer lexer(baseLoc, lang, bufStart, bufStart, bufEnd);
  Token token;

  while (true) {
    lexer.LexFromRawLexer(token);
    if (token.is(tok::eof))
      break;
    if (token.is(tok::comment))
      continue;

    const unsigned offset =
        static_cast<unsigned>(refoldTokenOffsetFromBase(token, baseLoc));
    out.push_back({token.getKind(),
                   std::string(text.substr(offset, token.getLength())), offset,
                   offset + token.getLength(), token.hasLeadingSpace()});
  }
}

/// Return the first lexer-visible boundary token in `text`, if any.
inline std::optional<RefoldLexBoundaryToken>
refoldFirstLexToken(llvm::StringRef text, const LangOptions &lang) {
  llvm::SmallVector<RefoldLexBoundaryToken, 8> toks;
  refoldLexBoundaryTokens(text, lang, toks);
  if (toks.empty())
    return std::nullopt;
  return toks.front();
}

/// Return the last lexer-visible boundary token in `text`, if any.
inline std::optional<RefoldLexBoundaryToken>
refoldLastLexToken(llvm::StringRef text, const LangOptions &lang) {
  llvm::SmallVector<RefoldLexBoundaryToken, 16> toks;
  refoldLexBoundaryTokens(text, lang, toks);
  if (toks.empty())
    return std::nullopt;
  return toks.back();
}

/// Return true iff placing `left` and `right` adjacent without whitespace would
/// change lexical tokenization compared to placing a single space between them.
inline bool refoldNeedsLexicalSeparator(const RefoldLexBoundaryToken &left,
                                        const RefoldLexBoundaryToken &right,
                                        const LangOptions &lang) {
  const std::string noSpace = left.spelling + right.spelling;
  const std::string withSpace = left.spelling + " " + right.spelling;

  llvm::SmallVector<RefoldLexBoundaryToken, 8> noSpaceToks;
  llvm::SmallVector<RefoldLexBoundaryToken, 8> withSpaceToks;
  refoldLexBoundaryTokens(noSpace, lang, noSpaceToks);
  refoldLexBoundaryTokens(withSpace, lang, withSpaceToks);

  if (noSpaceToks.size() != withSpaceToks.size())
    return true;
  for (size_t i = 0; i < noSpaceToks.size(); ++i) {
    if (noSpaceToks[i].kind != withSpaceToks[i].kind ||
        noSpaceToks[i].spelling != withSpaceToks[i].spelling)
      return true;
  }
  return false;
}

/// Return the offsets among `seams` where `text` needs one separating space
/// to keep the tokens on the two sides of each seam apart.
///
/// `seams` are sorted byte offsets into `text`, each a token boundary of the
/// pieces it joins.  Clang lexes `text` by maximal munch, so where nothing
/// separates two pieces, spellings that combine (`p`+`t`, `+`+`+`, `/`+`*`)
/// become one token or comment that straddles the seam.  The raw lexer, with
/// comments retained, finds each such straddle.  A space at the seam splits
/// it there and can change only what follows, so lexing resumes at that seam
/// as if the space were present.
inline llvm::SmallVector<uint64_t, 4>
refoldSeamsNeedingSeparator(const std::string &text,
                            llvm::ArrayRef<uint64_t> seams,
                            const LangOptions &lang) {
  llvm::SmallVector<uint64_t, 4> separated;
  const SourceLocation baseLoc = SourceLocation::getFromRawEncoding(1);
  const char *bufStart = text.c_str();
  size_t nextSeam = 0;
  size_t resumeAt = 0;
  while (nextSeam < seams.size()) {
    Lexer lexer(baseLoc, lang, bufStart, bufStart + resumeAt,
                bufStart + text.size());
    lexer.SetCommentRetentionState(true);
    Token token;
    bool straddled = false;
    while (!straddled && nextSeam < seams.size()) {
      lexer.LexFromRawLexer(token);
      if (token.is(tok::eof))
        return separated;
      const size_t begin = refoldTokenOffsetFromBase(token, baseLoc);
      while (nextSeam < seams.size() && seams[nextSeam] <= begin)
        ++nextSeam;
      if (nextSeam < seams.size() &&
          seams[nextSeam] < begin + token.getLength()) {
        resumeAt = seams[nextSeam++];
        separated.push_back(resumeAt);
        straddled = true;
      }
    }
  }
  return separated;
}

/// Append `piece` to `text`, with one space between them when the last token
/// of `text` and the first token of `piece` would otherwise lex as one.
///
/// For a repair that re-spells source beside an edit's text: the join is a
/// token boundary of both sides, as `refoldSeamsNeedingSeparator` requires.
inline void refoldAppendLexicallySeparated(std::string &text,
                                           llvm::StringRef piece,
                                           const LangOptions &lang) {
  const uint64_t seam = text.size();
  text.append(piece.data(), piece.size());
  if (seam == 0 || seam == text.size() || stringutils::isWs(text[seam - 1]) ||
      stringutils::isWs(text[seam]))
    return;
  const uint64_t seams[] = {seam};
  if (!refoldSeamsNeedingSeparator(text, seams, lang).empty())
    text.insert(seam, 1, ' ');
}

/// Slice the exact byte coverage of tokens [startTok,endTok).
///
/// The returned range begins at the first token's byte offset and ends at the
/// last token's spelling end, excluding trailing inter-token whitespace that
/// belongs to later untouched text.
inline llvm::StringRef refoldSliceExactTokenCoverage(
    llvm::ArrayRef<size_t> tokOff, llvm::ArrayRef<PPTok> toks,
    llvm::StringRef source, uint64_t startTok, uint64_t endTok) {
  if (tokOff.empty() || toks.empty() || source.empty() || endTok <= startTok)
    return "";

  const uint64_t tokCount = static_cast<uint64_t>(toks.size());
  uint64_t loTok = std::clamp(startTok, static_cast<uint64_t>(0), tokCount);
  uint64_t hiTok = std::clamp(endTok, loTok, tokCount);
  if (hiTok <= loTok || loTok >= tokCount)
    return "";

  size_t lo = tokOff[static_cast<size_t>(loTok)];
  const size_t lastTok = static_cast<size_t>(hiTok - 1);
  size_t hi = tokOff[lastTok] + toks[lastTok].spelling.size();

  const size_t sourceLen = source.size();
  lo = std::clamp(lo, size_t(0), sourceLen);
  hi = std::clamp(hi, lo, sourceLen);
  return source.substr(lo, hi - lo);
}

/// Slice the full byte envelope of tokens [startTok,endTok).
///
/// Unlike exact token coverage, this preserves trailing whitespace or newlines
/// up to the next token boundary because B-side insertion payloads may rely on
/// that trivia for stable physical layout.
inline llvm::StringRef refoldSliceTokenEnvelope(llvm::ArrayRef<size_t> tokOff,
                                                llvm::StringRef source,
                                                uint64_t startTok,
                                                uint64_t endTok) {
  if (tokOff.empty() || source.empty() || endTok <= startTok)
    return "";

  const uint64_t tokCount = static_cast<uint64_t>(tokOff.size());
  uint64_t loTok = std::clamp(startTok, static_cast<uint64_t>(0), tokCount);
  uint64_t hiTok = std::clamp(endTok, loTok, tokCount);
  if (hiTok <= loTok || loTok >= tokCount)
    return "";

  const size_t sourceLen = source.size();
  size_t lo =
      std::clamp(tokOff[static_cast<size_t>(loTok)], size_t(0), sourceLen);
  size_t hi = sourceLen;
  if (hiTok < tokCount)
    hi = std::clamp(tokOff[static_cast<size_t>(hiTok)], lo, sourceLen);
  return source.substr(lo, hi - lo);
}

} // namespace refold
} // namespace clang

#endif // LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_TOKENTEXTHELPERS_H
