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

#include "source/RefoldToken.h"
#include "util/StringUtils.h"

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
/// This mirrors the old RefoldEngine.cpp anonymous-namespace record so moved
/// callers can prove token-boundary preservation without
/// depending on textual inclusion order.
struct RefoldLexBoundaryToken {
  tok::TokenKind kind = tok::unknown;
  std::string spelling;
  size_t begin = 0;
  size_t end = 0;
};

/// Lex a snippet into non-comment boundary tokens for maximal-munch checks.
inline void refoldLexBoundaryTokens(llvm::StringRef text, const LangOptions &lang,
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
                   offset + token.getLength()});
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


/// Return \p text with at most one synthetic space added at either outer
/// boundary when direct juxtaposition with \p base would change lexical
/// tokenization.  This is a lexical hygiene helper only: callers must already
/// have proven that the surrounding TU/include/macro replacement is valid.
inline std::string refoldPadAtBoundaries(llvm::StringRef base, size_t start,
                                         size_t end, std::string text,
                                         bool allowLeft, bool allowRight,
                                         const LangOptions &lang) {
  const auto f = stringutils::firstNonWsIdx(text);
  const auto l = stringutils::lastNonWsIdx(text);

  // Empty or whitespace-only replacements have no token edge that can glue to
  // the surrounding owner text.
  if (!f || !l)
    return text;

  // If the replacement already carries whitespace on an edge, that side is
  // already separated.  Never add a second synthetic padding space there.
  const bool hasLeadingWs = (*f > 0);
  const bool hasTrailingWs = (*l + 1 < text.size());

  std::optional<RefoldLexBoundaryToken> textFirstTok =
      refoldFirstLexToken(llvm::StringRef(text), lang);
  std::optional<RefoldLexBoundaryToken> textLastTok =
      refoldLastLexToken(llvm::StringRef(text), lang);

  const std::optional<char> leftChar =
      (start > 0 && start <= base.size()) ? std::optional<char>(base[start - 1])
                                          : std::nullopt;
  const std::optional<char> rightChar =
      (end < base.size()) ? std::optional<char>(base[end]) : std::nullopt;

  bool addLeftSpace = false;
  if (allowLeft && !hasLeadingWs && start > 0 && start <= base.size() &&
      textFirstTok && (!leftChar || !stringutils::isWs(*leftChar))) {
    if (std::optional<RefoldLexBoundaryToken> leftTok =
            refoldLastLexToken(base.take_front(start), lang)) {
      addLeftSpace = refoldNeedsLexicalSeparator(*leftTok, *textFirstTok, lang);
    }
  }

  bool addRightSpace = false;
  if (allowRight && !hasTrailingWs && end < base.size() && textLastTok &&
      (!rightChar || !stringutils::isWs(*rightChar))) {
    if (std::optional<RefoldLexBoundaryToken> rightTok =
            refoldFirstLexToken(base.drop_front(end), lang)) {
      addRightSpace = refoldNeedsLexicalSeparator(*textLastTok, *rightTok, lang);
    }
  }

  if (addLeftSpace)
    text.insert(0, 1, ' ');

  if (addRightSpace)
    text.push_back(' ');

  return text;
}

/// Slice the exact byte coverage of tokens [startTok,endTok).
///
/// The returned range begins at the first token's byte offset and ends at the
/// last token's spelling end, excluding trailing inter-token whitespace that
/// belongs to later untouched text.
inline llvm::StringRef refoldSliceExactTokenCoverage(llvm::ArrayRef<size_t> tokOff,
                                               llvm::ArrayRef<PPTok> toks,
                                               llvm::StringRef source,
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
