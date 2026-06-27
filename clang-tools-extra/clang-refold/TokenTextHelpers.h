//===--- TokenTextHelpers.h --------------------------*- C++ -*-===//
//
// Private token-text helpers shared by RefoldEngine translation units after the
// TailUtilities split.  They keep lexical-boundary and B-token slicing proofs in
// one deterministic implementation instead of relying on a text-included .inc
// fragment to see RefoldEngine.cpp's anonymous-namespace helpers.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_TOKENTEXTHELPERS_H
#define LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_TOKENTEXTHELPERS_H

#include "RefoldEngine.h"

#include "clang/Basic/LangOptions.h"
#include "clang/Basic/SourceLocation.h"
#include "clang/Basic/TokenKinds.h"
#include "clang/Lex/Lexer.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>

namespace clang {
namespace refold {

/// Convert a raw-lexer token location into an offset relative to the scratch
/// buffer's artificial base location.
///
/// Several refold helpers lex short synthetic buffers with
/// `SourceLocation::getFromRawEncoding(1)` as their base.  Keeping the offset
/// calculation in this shared private header avoids relying on anonymous
/// namespace helpers in whichever engine translation unit used to text-include
/// the old `.inc` fragment.
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
/// This mirrors the old RefoldEngine.cpp anonymous-namespace record so moved
/// engine translation units can prove token-boundary preservation without
/// depending on textual inclusion order.
struct RefoldLexBoundaryToken {
  tok::TokenKind Kind = tok::unknown;
  std::string Spelling;
  size_t Begin = 0;
  size_t End = 0;
};

/// Lex a snippet into non-comment boundary tokens for maximal-munch checks.
inline void refoldLexBoundaryTokens(StringRef text, const LangOptions &lang,
                                    SmallVectorImpl<RefoldLexBoundaryToken> &out) {
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
                   offset + token.getLength()});
  }
}

/// Return the first lexer-visible boundary token in `text`, if any.
inline std::optional<RefoldLexBoundaryToken>
refoldFirstLexToken(StringRef text, const LangOptions &lang) {
  SmallVector<RefoldLexBoundaryToken, 8> toks;
  refoldLexBoundaryTokens(text, lang, toks);
  if (toks.empty())
    return std::nullopt;
  return toks.front();
}

/// Return the last lexer-visible boundary token in `text`, if any.
inline std::optional<RefoldLexBoundaryToken>
refoldLastLexToken(StringRef text, const LangOptions &lang) {
  SmallVector<RefoldLexBoundaryToken, 16> toks;
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
  const std::string noSpace = left.Spelling + right.Spelling;
  const std::string withSpace = left.Spelling + " " + right.Spelling;

  SmallVector<RefoldLexBoundaryToken, 8> noSpaceToks;
  SmallVector<RefoldLexBoundaryToken, 8> withSpaceToks;
  refoldLexBoundaryTokens(noSpace, lang, noSpaceToks);
  refoldLexBoundaryTokens(withSpace, lang, withSpaceToks);

  if (noSpaceToks.size() != withSpaceToks.size())
    return true;
  for (size_t i = 0; i < noSpaceToks.size(); ++i) {
    if (noSpaceToks[i].Kind != withSpaceToks[i].Kind ||
        noSpaceToks[i].Spelling != withSpaceToks[i].Spelling)
      return true;
  }
  return false;
}

/// Slice the exact byte coverage of tokens [startTok,endTok).
///
/// The returned range begins at the first token's byte offset and ends at the
/// last token's spelling end, excluding trailing inter-token whitespace that
/// belongs to later untouched text.
inline StringRef refoldSliceExactTokenCoverage(ArrayRef<size_t> tokOff,
                                               ArrayRef<PPTok> toks,
                                               StringRef source,
                                               uint64_t startTok,
                                               uint64_t endTok) {
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
inline StringRef refoldSliceTokenEnvelope(ArrayRef<size_t> tokOff,
                                          StringRef source,
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
