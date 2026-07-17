//===--- RefoldMacroGeneratedLeafReplayEngine.cpp ---------------*- C++ -*-===//
//
// Generated-leaf fallback replay for clang-refold.
//
//===----------------------------------------------------------------------===//

#include "macro/RefoldMacroGeneratedLeafReplayEngine.h"

#include "macro/RefoldArgTextRecovery.h"
#include "macro/RefoldMacroPatchProofCertifier.h"
#include "macro/RefoldMacroPlannerHelpers.h"
#include "macro/RefoldMacroReplay.h"
#include "source/RefoldSourceMapper.h"
#include "source/TokenTextHelpers.h"
#include "util/StringUtils.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/FormatVariadic.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

using namespace llvm;

namespace clang {
namespace refold {

namespace {

/// Boundary-lexed token carrier for leaf-level generated-callee text checks.
struct LeafTok {
  std::string spelling;
};

/// Return the non-empty changed middles after trimming the longest common
/// prefix and suffix from two spellings.
std::optional<std::pair<std::string, std::string>>
changedMiddleSubstrings(StringRef oldValue, StringRef newValue) {
  size_t prefix = 0;
  while (prefix < oldValue.size() && prefix < newValue.size() &&
         oldValue[prefix] == newValue[prefix])
    ++prefix;
  size_t suffix = 0;
  while (suffix + prefix < oldValue.size() &&
         suffix + prefix < newValue.size() &&
         oldValue[oldValue.size() - suffix - 1] ==
             newValue[newValue.size() - suffix - 1])
    ++suffix;
  StringRef oldMiddle = oldValue.slice(prefix, oldValue.size() - suffix);
  StringRef newMiddle = newValue.slice(prefix, newValue.size() - suffix);
  if (oldMiddle.empty() || newMiddle.empty())
    return std::nullopt;
  return std::make_pair(oldMiddle.str(), newMiddle.str());
}

/// Compare a leaf-token sequence against an expected spelling vector.
bool leafTokenSpellingsEqual(ArrayRef<LeafTok> toks,
                             ArrayRef<std::string> expected) {
  if (toks.size() != expected.size())
    return false;
  for (size_t i = 0; i < toks.size(); ++i)
    if (toks[i].spelling != expected[i])
      return false;
  return true;
}

/// Lexes generated-leaf text into boundary-token spellings used by replay proof.
///
/// The helper owns only tokenization of already-selected leaf text.  It does
/// not interpret macro structure, choose candidates, or relax unsupported input:
/// callers keep the same fail-closed checks they performed around the former
/// local lambda.
void lexLeafTokens(StringRef text, const clang::LangOptions &lexLang,
                   SmallVectorImpl<LeafTok> &out) {
  out.clear();
  SmallVector<RefoldLexBoundaryToken, 32> toks;
  refoldLexBoundaryTokens(text, lexLang, toks);
  for (const RefoldLexBoundaryToken &tok : toks)
    out.push_back(LeafTok{tok.spelling});
}

/// Return the value domain used by scalar leaf comparisons.
///
/// Simple string literals compare by decoded payload; every other token keeps
/// its spelling.  This preserves the previous comparison domain exactly and
/// performs no candidate admission on its own.
std::string leafValueForToken(StringRef spelling) {
  if (std::optional<std::string> decoded =
          decodeSimpleStringLiteralToken(spelling))
    return *decoded;
  return spelling.str();
}

/// Compare two generated-leaf texts after boundary lexing.
///
/// Token-equivalence is used only to reject conflicting rewrites that spell the
/// same candidate differently.  It preserves the previous requirement that both
/// sides lex to the same token count and the same token spellings.
bool leafTextsTokenEquivalent(StringRef lhs, StringRef rhs,
                              const clang::LangOptions &lexLang) {
  SmallVector<LeafTok, 16> lhsToks;
  SmallVector<LeafTok, 16> rhsToks;
  lexLeafTokens(lhs, lexLang, lhsToks);
  lexLeafTokens(rhs, lexLang, rhsToks);
  if (lhsToks.size() != rhsToks.size())
    return false;
  for (size_t i = 0; i < lhsToks.size(); ++i)
    if (lhsToks[i].spelling != rhsToks[i].spelling)
      return false;
  return true;
}

/// Append one source actual as replay token spellings.
///
/// This helper is deliberately append-only because replacement-list replay may
/// contribute several parameters and literals to one generated leaf.  Token
/// order and lexer options remain exactly those used by the former local lambda.
void appendLexedLeafArgumentTokens(StringRef text,
                                   const clang::LangOptions &lexLang,
                                   SmallVectorImpl<std::string> &out) {
  SmallVector<RefoldLexBoundaryToken, 16> toks;
  refoldLexBoundaryTokens(text, lexLang, toks);
  for (const RefoldLexBoundaryToken &tok : toks)
    out.push_back(tok.spelling);
}

/// Stringify one source actual using the replay spelling rule.
///
/// Tokens are joined with one space and then quoted as a C string literal body.
/// This keeps stringification evidence separate from selector-rewrite policy and
/// preserves the old escaping and spacing behavior byte-for-byte.
std::string stringifyArgumentForLeafReplay(StringRef text,
                                           const clang::LangOptions &lexLang) {
  SmallVector<RefoldLexBoundaryToken, 16> toks;
  refoldLexBoundaryTokens(text, lexLang, toks);
  std::string body;
  for (const RefoldLexBoundaryToken &tok : toks) {
    if (!body.empty())
      body.push_back(' ');
    body += tok.spelling;
  }
  std::string out = "\"";
  for (char c : body) {
    if (c == '\\' || c == '"')
      out.push_back('\\');
    out.push_back(c);
  }
  out.push_back('"');
  return out;
}

/// Append one paste operand to a generated-leaf replay spelling.
///
/// Paste replay is accepted only for literal operands and in-range parameter
/// references.  Unsupported replacement-list controls reject the entire replay,
/// preserving the previous fail-closed behavior for `#`, `##`, and
/// `__VA_OPT__` inside paste pieces.
bool appendLeafReplayPastePiece(
    const RefoldModel::MacroReplacementToken &piece,
    ArrayRef<StringRef> actuals, std::string &pasted) {
  if (piece.kind == RefoldModel::MacroReplacementTokenKind::ParamRef) {
    if (!piece.paramIndex || *piece.paramIndex >= actuals.size())
      return false;
    pasted += actuals[*piece.paramIndex].trim().str();
    return true;
  }
  if (piece.spelling == "#" || piece.spelling == "##" ||
      piece.spelling == "__VA_OPT__")
    return false;
  pasted += piece.spelling.str();
  return true;
}

/// Replay one function-like replacement list into generated-leaf spellings.
///
/// The helper is context-free: it receives the selected definition, actuals,
/// and lexer options explicitly.  It keeps the old replay order and rejects any
/// malformed stringification, paste, or parameter reference rather than creating
/// a weaker generated-leaf proof.
std::optional<SmallVector<std::string, 16>>
replayFunctionLikeToLeafSpellings(
    const RefoldModel::MacroDirective &definition, ArrayRef<StringRef> actuals,
    const clang::LangOptions &lexLang) {
  if (definition.defParams.size() != actuals.size())
    return std::nullopt;
  SmallVector<std::string, 16> out;
  const auto &toks = definition.replacementTokens;
  for (size_t i = 0; i < toks.size();) {
    const auto &tok = toks[i];
    if (tok.spelling == "#") {
      if (i + 1 >= toks.size() ||
          toks[i + 1].kind !=
              RefoldModel::MacroReplacementTokenKind::ParamRef ||
          !toks[i + 1].paramIndex ||
          *toks[i + 1].paramIndex >= actuals.size())
        return std::nullopt;
      out.push_back(stringifyArgumentForLeafReplay(
          actuals[*toks[i + 1].paramIndex], lexLang));
      i += 2;
      continue;
    }
    if (i + 1 < toks.size() && toks[i + 1].spelling == "##") {
      std::string pasted;
      if (!appendLeafReplayPastePiece(tok, actuals, pasted))
        return std::nullopt;
      i += 2;
      while (true) {
        if (i >= toks.size() ||
            !appendLeafReplayPastePiece(toks[i], actuals, pasted))
          return std::nullopt;
        ++i;
        if (i >= toks.size() || toks[i].spelling != "##")
          break;
        ++i;
      }
      out.push_back(std::move(pasted));
      continue;
    }
    if (tok.kind == RefoldModel::MacroReplacementTokenKind::ParamRef) {
      if (!tok.paramIndex || *tok.paramIndex >= actuals.size())
        return std::nullopt;
      appendLexedLeafArgumentTokens(actuals[*tok.paramIndex], lexLang, out);
      ++i;
      continue;
    }
    if (tok.spelling == "##" || tok.spelling == "__VA_OPT__")
      return std::nullopt;
    out.push_back(tok.spelling.str());
    ++i;
  }
  return out;
}

/// Rewrite a decoded string-literal source occurrence back into source spelling.
///
/// Decoded-payload matching is only evidence for finding the old leaf.  When it
/// solves a new payload, the source actual must remain a string literal rather
/// than silently turning into a raw token sequence.
std::optional<std::string> rewriteSolvedLeafAsSource(
    bool matchedDecodedSourceStringLiteral, StringRef sourceText,
    StringRef solved) {
  if (!matchedDecodedSourceStringLiteral)
    return solved.str();

  StringRef source = sourceText.trim();
  const size_t quote = source.find('"');
  const size_t endQuote = source.rfind('"');
  if (quote == StringRef::npos || endQuote == StringRef::npos ||
      endQuote <= quote)
    return std::nullopt;

  std::string rewritten = source.slice(0, quote).str();
  rewritten += stringutils::quoteCStringLiteral(solved);
  rewritten += source.substr(endQuote + 1).str();
  return rewritten;
}

/// One root-argument occurrence that can explain part of a changed leaf.
///
/// Multi-leaf replay solves a generated value by treating root actuals as
/// editable leaves inside a literal skeleton.  Decoded string-literal matches
/// are recorded as probe evidence only: any solved replacement must be written
/// back as source text that preserves the original string-literal spelling
/// domain.
struct MultiLeafPatternArgOccurrence {
  uint32_t argIdx = 0;
  size_t begin = 0;
  size_t end = 0;
  std::string oldText;
  bool matchedDecodedSourceStringLiteral = false;
  std::string sourceText;
};

/// Candidate source text probe for matching one root actual inside a leaf.
///
/// Ordinary source spelling and decoded string-literal payloads are tried in
/// the same order as before.  The decoded flag is carried with the probe so the
/// rewrite step can re-quote solved payloads instead of emitting raw tokens.
struct MultiLeafPatternSourceProbe {
  StringRef text;
  bool decodedSourceStringLiteral = false;
};

/// Candidate produced by rewriting the selector actual of a generated leaf.
///
/// The candidate records the full replacement invocation text plus the source
/// root argument whose B-side materialized range certifies the selector edit.
/// Candidates are sorted by replacement text exactly as the former local lambda
/// did so ambiguous selector inversions continue to reject fail-closed.
struct GeneratedSelectorActualRewriteCandidate {
  std::string replacement;
  uint32_t rootArgIdx = 0;
  size_t newSelectorSize = 0;
};

/// Private generated-leaf replay resolver used by the public engine entry point.
///
/// The resolver owns only the candidate search for one generated-leaf replay
/// attempt.  It mutates no long-lived state: all candidate text, token, and
/// replacement state remains local to `TryBuild`, while dependency callbacks and
/// proof certification are invoked at the same points as the former engine
/// method. Unsupported leaf surfaces, ambiguous inversions, and unprovable
/// root-argument rewrites continue to fail closed by returning `std::nullopt`.
class GeneratedLeafReplayResolver {
public:
  explicit GeneratedLeafReplayResolver(
      const RefoldMacroGeneratedLeafReplayEngine::Dependencies &deps)
      : deps_(deps) {}

  /// Try to build the generated-leaf fallback candidate for one root
  /// invocation without changing candidate ranking or fallback policy.
  std::optional<MacroPatch>
  TryBuild(const GeneratedLeafReplayContext &generatedLeafCtx) const;

private:
  /// Try the narrow selector-actual inversion used by generated-leaf replay.
  ///
  /// This helper owns only the candidate search that changes the selector
  /// argument of the root invocation.  It preserves the former ambiguity policy:
  /// every distinct replacement text is collected, sorted deterministically,
  /// and rejected unless all viable selector candidates agree byte-for-byte.
  std::optional<MacroPatch> TryGeneratedSelectorActualRewrite(
      const GeneratedLeafReplayContext &generatedLeafCtx,
      ArrayRef<LeafTok> oldToks, ArrayRef<LeafTok> newToks) const;

  /// Try the narrow trailing-variadic deactivation rewrite.
  ///
  /// This helper owns only the proof that a generated leaf lost the final
  /// `__VA_OPT__` data token while preserving the earlier leaf prefix.  It
  /// removes the corresponding trailing root actual by using the invocation
  /// argument ranges exactly as the former local lambda did: the edit starts
  /// at the previous argument end so the separating comma and whitespace are
  /// removed together with the old variadic payload.
  std::optional<MacroPatch> TryRemoveTrailingVariadicActual(
      const GeneratedLeafReplayContext &generatedLeafCtx,
      ArrayRef<LeafTok> oldToks, ArrayRef<LeafTok> newToks,
      bool rootHasTrailingVariadic, bool hasLeafRewrite) const;

  /// Try the multi-leaf pattern inversion used when several generated leaves
  /// changed under the same root invocation.
  ///
  /// This helper owns only the skeleton-matching rewrite: it finds root actual
  /// occurrences inside each changed old leaf value, proves that the unchanged
  /// literal gaps line up in the new leaf value, and composes per-argument
  /// replacements.  It preserves the former fail-closed policy for ambiguous
  /// occurrences, overlapping leaves, top-level comma introduction, and
  /// conflicting repeated uses of the same root argument.
  std::optional<MacroPatch> TryMultiLeafPatternRewrite(
      const GeneratedLeafReplayContext &generatedLeafCtx,
      ArrayRef<LeafTok> oldToks, ArrayRef<LeafTok> newToks,
      const InvocationActualRecoveryContext &actualRecoveryCtx) const;

  /// Try the final scalar generated-leaf fallback rewrite.
  ///
  /// This helper owns only the stable-root occurrence validation and final
  /// one-argument source rewrite.  It preserves the former fail-closed checks:
  /// the solved old leaf must occur uniquely in exactly one root actual, must
  /// not introduce a top-level comma for a non-variadic formal, and must not
  /// still be observed by another current-level standard or stringify span.
  std::optional<MacroPatch> TryStableRootScalarLeafRewrite(
      const GeneratedLeafReplayContext &generatedLeafCtx,
      const InvocationActualRecoveryContext &actualRecoveryCtx,
      StringRef oldLeaf, StringRef newLeaf) const;

  /// Returns whether a stable root occurrence still observes the old leaf.
  ///
  /// Missing or inverted B-side source mapping is treated as proof failure,
  /// matching the previous local-lambda policy: an unprovable stable
  /// occurrence rejects the final source rewrite rather than weakening to a
  /// fallback that cannot account for all owner observations.
  bool StableRootOccurrenceStillObservesOldLeaf(
      const RefoldModel::PPArgSpan &span, uint32_t targetArgIdx,
      StringRef oldLeaf) const;

  const RefoldMacroGeneratedLeafReplayEngine::Dependencies &deps_;
};

std::optional<MacroPatch>
GeneratedLeafReplayResolver::TryGeneratedSelectorActualRewrite(
    const GeneratedLeafReplayContext &generatedLeafCtx,
    ArrayRef<LeafTok> oldToks, ArrayRef<LeafTok> newToks) const {
  const RefoldModel::MacroInvocation &m = generatedLeafCtx.invocation;
  StringRef baseInvText = generatedLeafCtx.baseInvocationText;
  ArrayRef<std::pair<size_t, size_t>> invArgRanges =
      generatedLeafCtx.invocationArgRanges;

  if (oldToks.empty() || newToks.empty())
    return std::nullopt;

  SmallVector<GeneratedSelectorActualRewriteCandidate, 4> candidates;

  for (uint32_t selectorIdx = 0; selectorIdx < invArgRanges.size();
       ++selectorIdx) {
    auto selectorRange = invArgRanges[selectorIdx];
    if (selectorRange.second < selectorRange.first ||
        selectorRange.second > baseInvText.size())
      return std::nullopt;
    StringRef selectorText =
        baseInvText.slice(selectorRange.first, selectorRange.second).trim();
    const RefoldModel::MacroDirective *oldSelectorDef =
        deps_.resolveFunctionLikeMacroForReplay(selectorText);
    if (!oldSelectorDef || oldSelectorDef->defParams.empty())
      continue;
    // The selector consumes its own root actual, and each generated-callee
    // formal would have to consume one following root actual in this narrow
    // selector-inversion theorem. A generated call may also provide fixed
    // replacement-list actuals, so an insufficient root-actual tail is a miss
    // here, not an ArrayRef index past the invocation's recovered argument
    // ranges.
    if (selectorIdx + oldSelectorDef->defParams.size() >= invArgRanges.size())
      continue;

    SmallVector<StringRef, 8> actuals;
    for (size_t i = 0; i < oldSelectorDef->defParams.size(); ++i) {
      const auto r = invArgRanges[selectorIdx + 1 + i];
      if (r.second < r.first || r.second > baseInvText.size())
        return std::nullopt;
      actuals.push_back(baseInvText.slice(r.first, r.second).trim());
    }
    std::optional<SmallVector<std::string, 16>> oldReplay =
        replayFunctionLikeToLeafSpellings(*oldSelectorDef, actuals,
                                          deps_.lexLang);
    if (!oldReplay || !leafTokenSpellingsEqual(oldToks, *oldReplay))
      continue;

    const bool oldSelectorWasAlias =
        deps_.isObjectLikeSingleTokenAlias(selectorText);
    for (const RefoldModel::MacroDirective &sourceDirective :
         deps_.model.GetMacroDirectives()) {
      if (sourceDirective.subkind != "#define" ||
          sourceDirective.name.empty() ||
          sourceDirective.name == selectorText)
        continue;

      const bool candidateIsAlias =
          !sourceDirective.functionLike &&
          sourceDirective.replacementTokens.size() == 1 &&
          sourceDirective.replacementTokens[0].kind ==
              RefoldModel::MacroReplacementTokenKind::Literal;
      if (oldSelectorWasAlias != candidateIsAlias)
        continue;
      if (!oldSelectorWasAlias && !sourceDirective.functionLike)
        continue;

      const RefoldModel::MacroDirective *candidateDef =
          deps_.resolveFunctionLikeMacroForReplay(sourceDirective.name);
      if (!candidateDef || candidateDef == oldSelectorDef ||
          candidateDef->defParams.size() != actuals.size())
        continue;
      std::optional<SmallVector<std::string, 16>> candidateReplay =
          replayFunctionLikeToLeafSpellings(*candidateDef, actuals,
                                            deps_.lexLang);
      if (!candidateReplay ||
          !leafTokenSpellingsEqual(newToks, *candidateReplay))
        continue;

      std::string replacement = baseInvText.str();
      replacement.replace(selectorRange.first,
                          selectorRange.second - selectorRange.first,
                          sourceDirective.name.str());
      candidates.push_back(GeneratedSelectorActualRewriteCandidate{
          std::move(replacement), selectorIdx, sourceDirective.name.size()});
    }
  }

  if (candidates.empty())
    return std::nullopt;
  llvm::sort(candidates,
             [](const GeneratedSelectorActualRewriteCandidate &lhs,
                const GeneratedSelectorActualRewriteCandidate &rhs) {
               return lhs.replacement < rhs.replacement;
             });
  for (const GeneratedSelectorActualRewriteCandidate &candidate : candidates)
    if (candidate.replacement != candidates.front().replacement)
      return std::nullopt;

  MacroPatch patch{*m.invB, *m.invE, candidates.front().replacement, m.id};
  patch.materialized.hasOutputByteRange = true;
  patch.materialized.outputByteStart =
      invArgRanges[candidates.front().rootArgIdx].first;
  patch.materialized.outputByteEnd =
      patch.materialized.outputByteStart + candidates.front().newSelectorSize;
  certifyMacroPatchMaterializedBTokenRange(
      patch, static_cast<uint64_t>(generatedLeafCtx.bTokenEnvelope.first),
      static_cast<uint64_t>(generatedLeafCtx.bTokenEnvelope.second));
  deps_.proofCertifier.SetArgsOnlyStandardProof(
      patch, m,
      /*wholeEnvelopeReplayValidated=*/true);
  return patch;
}

std::optional<MacroPatch>
GeneratedLeafReplayResolver::TryRemoveTrailingVariadicActual(
    const GeneratedLeafReplayContext &generatedLeafCtx,
    ArrayRef<LeafTok> oldToks, ArrayRef<LeafTok> newToks,
    bool rootHasTrailingVariadic, bool hasLeafRewrite) const {
  const RefoldModel::MacroInvocation &m = generatedLeafCtx.invocation;
  StringRef baseInvText = generatedLeafCtx.baseInvocationText;
  ArrayRef<std::pair<size_t, size_t>> invArgRanges =
      generatedLeafCtx.invocationArgRanges;

  if (hasLeafRewrite || oldToks.size() <= newToks.size() ||
      !rootHasTrailingVariadic)
    return std::nullopt;

  bool newPrefixMatches = true;
  for (size_t i = 0; i < newToks.size(); ++i) {
    if (oldToks[i].spelling != newToks[i].spelling) {
      newPrefixMatches = false;
      break;
    }
  }
  if (!newPrefixMatches)
    return std::nullopt;

  bool onlyVaOptTailRemoved = true;
  for (size_t i = newToks.size(); i < oldToks.size(); ++i) {
    StringRef spelling(oldToks[i].spelling);
    const std::string decoded = leafValueForToken(spelling);
    if (spelling == ":" || spelling == "," || StringRef(decoded) == ":" ||
        StringRef(decoded) == ",")
      continue;
    // The remaining removed token is the old variadic data token.  There must
    // be exactly one such token for this narrow deactivation proof; richer
    // pack edits belong in the full generated-argument DAG proof.
    if (i + 1 != oldToks.size())
      onlyVaOptTailRemoved = false;
  }
  if (!onlyVaOptTailRemoved)
    return std::nullopt;

  if (!rootHasTrailingVariadic || invArgRanges.size() < 2)
    return std::nullopt;
  const uint32_t lastIdx = static_cast<uint32_t>(invArgRanges.size() - 1);
  const auto prev = invArgRanges[lastIdx - 1];
  const auto last = invArgRanges[lastIdx];
  if (last.second < last.first || last.second > baseInvText.size() ||
      last.first == last.second)
    return std::nullopt;

  // Deactivating a trailing `__VA_OPT__` tail removes the variadic root
  // actual, not the callee selector or the fixed argument before it.  The
  // formal range list gives us the exact separator-to-end span for the last
  // argument: start at the previous argument's end so the separating comma and
  // whitespace disappear together with the old variadic payload.
  std::string rewritten = baseInvText.slice(0, prev.second).str();
  rewritten += baseInvText.substr(last.second).str();
  MacroPatch patch{*m.invB, *m.invE, std::move(rewritten), m.id};
  patch.materialized.hasOutputByteRange = true;
  patch.materialized.outputByteStart = prev.second;
  patch.materialized.outputByteEnd = prev.second;
  certifyMacroPatchMaterializedBTokenRange(
      patch, static_cast<uint64_t>(generatedLeafCtx.bTokenEnvelope.first),
      static_cast<uint64_t>(generatedLeafCtx.bTokenEnvelope.second));
  deps_.proofCertifier.SetArgsOnlyStandardProof(
      patch, m,
      /*wholeEnvelopeReplayValidated=*/true);
  return patch;
}

std::optional<MacroPatch>
GeneratedLeafReplayResolver::TryMultiLeafPatternRewrite(
    const GeneratedLeafReplayContext &generatedLeafCtx,
    ArrayRef<LeafTok> oldToks, ArrayRef<LeafTok> newToks,
    const InvocationActualRecoveryContext &actualRecoveryCtx) const {
  const RefoldModel::MacroInvocation &m = generatedLeafCtx.invocation;
  StringRef baseInvText = generatedLeafCtx.baseInvocationText;
  ArrayRef<std::pair<size_t, size_t>> invArgRanges =
      generatedLeafCtx.invocationArgRanges;
  if (oldToks.size() != newToks.size())
    return std::nullopt;

  DenseMap<uint32_t, std::string> replByArgIdx;
  for (size_t tokIdx = 0; tokIdx < oldToks.size(); ++tokIdx) {
    if (oldToks[tokIdx].spelling == newToks[tokIdx].spelling)
      continue;

    std::string oldValueStorage = leafValueForToken(oldToks[tokIdx].spelling);
    std::string newValueStorage = leafValueForToken(newToks[tokIdx].spelling);
    StringRef oldValue(oldValueStorage);
    StringRef newValue(newValueStorage);
    SmallVector<MultiLeafPatternArgOccurrence, 8> occs;
    std::optional<uint32_t> emptyArgIdx;
    for (uint32_t argIdx = 0; argIdx < invArgRanges.size(); ++argIdx) {
      auto r = invArgRanges[argIdx];
      if (r.second < r.first || r.second > baseInvText.size())
        return std::nullopt;
      StringRef argText = baseInvText.slice(r.first, r.second).trim();
      if (deps_.resolveFunctionLikeMacroForReplay(argText))
        continue;
      if (argText.empty()) {
        if (emptyArgIdx)
          return std::nullopt;
        emptyArgIdx = argIdx;
        continue;
      }
      SmallVector<MultiLeafPatternSourceProbe, 2> probes;
      probes.push_back(MultiLeafPatternSourceProbe{argText, false});

      // Keep the decoded spelling alive for the full probe loop.  More
      // importantly, tag it as decoded evidence so a later source rewrite
      // re-quotes the solved payload.  Decoding makes `"beta"` comparable
      // to the generated leaf value `beta`, but it does not prove that the
      // source actual may be rewritten to the raw identifier `gamma`.
      std::optional<std::string> decodedArgStorage =
          decodeSimpleStringLiteralToken(argText);
      if (decodedArgStorage)
        probes.push_back(MultiLeafPatternSourceProbe{
            StringRef(*decodedArgStorage), true});

      for (MultiLeafPatternSourceProbe probe : probes) {
        if (probe.text.empty())
          continue;
        size_t pos = oldValue.find(probe.text);
        if (pos == StringRef::npos)
          continue;
        if (oldValue.find(probe.text, pos + probe.text.size()) !=
            StringRef::npos)
          return std::nullopt;
        MultiLeafPatternArgOccurrence occ;
        occ.argIdx = argIdx;
        occ.begin = pos;
        occ.end = pos + probe.text.size();
        occ.oldText = probe.text.str();
        occ.matchedDecodedSourceStringLiteral =
            probe.decodedSourceStringLiteral;
        occ.sourceText = argText.str();
        occs.push_back(std::move(occ));
        break;
      }
    }
    if (emptyArgIdx) {
      // Empty actuals are real macro arguments even though they have no
      // spelling to find in the old expansion.  Model the single empty leaf
      // as a zero-width occurrence at the end of the generated value; the
      // skeleton matcher below then proves whether B supplies a unique
      // suffix for that empty source slot or erases an old non-empty slot.
      MultiLeafPatternArgOccurrence occ;
      occ.argIdx = *emptyArgIdx;
      occ.begin = oldValue.size();
      occ.end = oldValue.size();
      occ.oldText = "";
      occs.push_back(std::move(occ));
    }
    if (occs.empty())
      return std::nullopt;
    llvm::sort(occs, [](const MultiLeafPatternArgOccurrence &lhs,
                         const MultiLeafPatternArgOccurrence &rhs) {
      if (lhs.begin != rhs.begin)
        return lhs.begin < rhs.begin;
      return lhs.argIdx < rhs.argIdx;
    });
    for (size_t i = 1; i < occs.size(); ++i)
      if (occs[i].begin < occs[i - 1].end)
        return std::nullopt;

    // Match the literal skeleton around the old root-argument leaves
    // against the new expansion value.  The leaves themselves may change
    // length, so only the fixed literal gaps are used as anchors.
    size_t newCursor = 0;
    for (size_t i = 0; i < occs.size(); ++i) {
      StringRef prefix =
          oldValue.slice(i == 0 ? 0 : occs[i - 1].end, occs[i].begin);
      if (!newValue.substr(newCursor).starts_with(prefix))
        return std::nullopt;
      newCursor += prefix.size();
      StringRef nextLiteral =
          oldValue.slice(occs[i].end, i + 1 < occs.size() ? occs[i + 1].begin
                                                          : oldValue.size());
      size_t nextPos = StringRef::npos;
      if (nextLiteral.empty()) {
        if (i + 1 < occs.size() && occs[i + 1].begin == occs[i].end) {
          // Adjacent generated leaves come from a paste-like expression
          // with no literal separator (`A##B`, or `A##<empty>`).  With no
          // anchor between leaves, use the same deterministic contribution
          // rule as unanchored paste inversion: every leaf before the next
          // literal keeps its old contribution width, and the final leaf in
          // the run consumes the remaining segment.  This prevents greedy
          // scalar rewrites such as `alpha,beta -> gammabeta,<empty>` while
          // still failing closed if the old-width cut is impossible.
          nextPos = newCursor + occs[i].oldText.size();
          if (nextPos > newValue.size())
            return std::nullopt;
        } else {
          nextPos = newValue.size();
        }
      } else {
        nextPos = newValue.find(nextLiteral, newCursor);
        if (nextPos == StringRef::npos)
          return std::nullopt;
        if (newValue.find(nextLiteral, nextPos + 1) != StringRef::npos)
          return std::nullopt;
      }
      StringRef solved = newValue.slice(newCursor, nextPos).trim();
      std::optional<std::string> sourceSolved = rewriteSolvedLeafAsSource(
          occs[i].matchedDecodedSourceStringLiteral, occs[i].sourceText,
          solved);
      if (!sourceSolved)
        return std::nullopt;
      StringRef sourceSolvedRef(*sourceSolved);
      auto existing = replByArgIdx.find(occs[i].argIdx);
      if (existing != replByArgIdx.end()) {
        if (!leafTextsTokenEquivalent(StringRef(existing->second),
                                      sourceSolvedRef, deps_.lexLang))
          return std::nullopt;
      } else {
        if (!isMacroInvocationVariadicFormal(m, occs[i].argIdx) &&
            replacementIntroducesTopLevelComma(sourceSolvedRef,
                                               deps_.lexLang))
          return std::nullopt;
        replByArgIdx[occs[i].argIdx] = std::move(*sourceSolved);
      }
      newCursor = nextPos;
    }

    // The loop above leaves `newCursor` at the start of the literal
    // suffix following the last editable leaf.  Intermediate separators are
    // consumed as the next leaf's prefix, but the final suffix has no next
    // iteration to consume it.  Consume that trailing literal context here
    // so decorated multi-leaf expressions such as
    // `pre_##A##_mid_##B##_suf` are matched as one skeleton rather than
    // rejected after solving the last source slot.
    if (!occs.empty()) {
      StringRef trailingLiteral = oldValue.drop_front(occs.back().end);
      if (!newValue.substr(newCursor).starts_with(trailingLiteral))
        return std::nullopt;
      newCursor += trailingLiteral.size();
    }
    if (newCursor != newValue.size())
      return std::nullopt;
  }

  if (replByArgIdx.empty())
    return std::nullopt;
  std::optional<InvocationRewriteWithRange> rewrite =
      deps_.buildInvocationRewriteWithRange(
          actualRecoveryCtx, replByArgIdx,
          /*materializedRangeByArgIdx=*/nullptr);
  if (!rewrite)
    return std::nullopt;
  MacroPatch patch{*generatedLeafCtx.invocation.invB,
                   *generatedLeafCtx.invocation.invE,
                   std::move(rewrite->text), generatedLeafCtx.invocation.id};
  deps_.proofCertifier.CertifyInvocationRewriteMaterializedOutputRange(
      patch, rewrite->materializedOutputByteStart,
      rewrite->materializedOutputByteEnd);
  certifyMacroPatchMaterializedBTokenRange(
      patch, static_cast<uint64_t>(generatedLeafCtx.bTokenEnvelope.first),
      static_cast<uint64_t>(generatedLeafCtx.bTokenEnvelope.second));
  deps_.proofCertifier.SetArgsOnlyStandardProof(
      patch, m,
      /*wholeEnvelopeReplayValidated=*/true);
  return patch;
}

bool GeneratedLeafReplayResolver::StableRootOccurrenceStillObservesOldLeaf(
    const RefoldModel::PPArgSpan &span, uint32_t targetArgIdx,
    StringRef oldLeaf) const {
  if (span.argIdx != targetArgIdx || span.begin >= span.end)
    return false;

  StringRef aText = deps_.sourceMapper.SliceASource(span.begin, span.end).trim();
  if (aText.find(oldLeaf) == StringRef::npos)
    return false;

  std::optional<std::pair<size_t, size_t>> bEnv =
      deps_.sourceMapper.MapAToBTokenEnvelopeByPPArgSpan(span);
  if (!bEnv || bEnv->second < bEnv->first)
    return true;

  StringRef bText =
      deps_.sourceMapper.SliceBSource(bEnv->first, bEnv->second).trim();
  if (bText.find(oldLeaf) == StringRef::npos)
    return false;

  return true;
}

std::optional<MacroPatch>
GeneratedLeafReplayResolver::TryStableRootScalarLeafRewrite(
    const GeneratedLeafReplayContext &generatedLeafCtx,
    const InvocationActualRecoveryContext &actualRecoveryCtx,
    StringRef oldLeaf, StringRef newLeaf) const {
  const RefoldModel::MacroInvocation &m = generatedLeafCtx.invocation;

  std::optional<uint32_t> targetArgIdx;
  std::optional<std::string> targetReplacement;
  for (uint32_t argIdx = 0;
       argIdx < generatedLeafCtx.invocationArgRanges.size(); ++argIdx) {
    auto r = generatedLeafCtx.invocationArgRanges[argIdx];
    if (r.second < r.first ||
        r.second > generatedLeafCtx.baseInvocationText.size())
      return std::nullopt;
    StringRef argText =
        generatedLeafCtx.baseInvocationText.slice(r.first, r.second);
    size_t pos = argText.find(oldLeaf);
    if (pos == StringRef::npos)
      continue;
    if (argText.find(oldLeaf, pos + oldLeaf.size()) != StringRef::npos)
      return std::nullopt;
    if (targetArgIdx)
      return std::nullopt;
    std::string rewrittenArg = stringutils::replaceRange(
        argText.str(), pos, pos + oldLeaf.size(), newLeaf);
    if (!isMacroInvocationVariadicFormal(m, argIdx) &&
        replacementIntroducesTopLevelComma(rewrittenArg, deps_.lexLang))
      return std::nullopt;
    targetArgIdx = argIdx;
    targetReplacement = std::move(rewrittenArg);
  }
  if (!targetArgIdx || !targetReplacement)
    return std::nullopt;

  // A generated-leaf rewrite is a source edit to the root invocation argument.
  // Therefore every current-level occurrence of that root formal that still
  // observes the solved old leaf in B would also be rewritten by the proposed
  // source change.  Reject that shape instead of preserving the parent
  // callsite: the edited leaf must be materialized or proved by the DAG path
  // that can account for all child/parent observations together.
  for (const RefoldModel::PPArgSpan &span :
       generatedLeafCtx.invocation.argSpans) {
    if (StableRootOccurrenceStillObservesOldLeaf(span, *targetArgIdx,
                                                 oldLeaf))
      return std::nullopt;
  }
  for (const RefoldModel::PPArgSpan &span :
       generatedLeafCtx.invocation.stringifySpans) {
    if (StableRootOccurrenceStillObservesOldLeaf(span, *targetArgIdx,
                                                 oldLeaf))
      return std::nullopt;
  }

  DenseMap<uint32_t, std::string> replByArgIdx;
  replByArgIdx[*targetArgIdx] = *targetReplacement;
  std::optional<InvocationRewriteWithRange> rewrite =
      deps_.buildInvocationRewriteWithRange(
          actualRecoveryCtx, replByArgIdx,
          /*materializedRangeByArgIdx=*/nullptr);
  if (!rewrite)
    return std::nullopt;

  MacroPatch patch{*generatedLeafCtx.invocation.invB,
                   *generatedLeafCtx.invocation.invE, std::move(rewrite->text),
                   generatedLeafCtx.invocation.id};
  deps_.proofCertifier.CertifyInvocationRewriteMaterializedOutputRange(
      patch, rewrite->materializedOutputByteStart,
      rewrite->materializedOutputByteEnd);
  certifyMacroPatchMaterializedBTokenRange(
      patch, static_cast<uint64_t>(generatedLeafCtx.bTokenEnvelope.first),
      static_cast<uint64_t>(generatedLeafCtx.bTokenEnvelope.second));
  deps_.proofCertifier.SetArgsOnlyStandardProof(
      patch, m,
      /*wholeEnvelopeReplayValidated=*/true);
  return patch;
}

std::optional<MacroPatch> GeneratedLeafReplayResolver::TryBuild(
    const GeneratedLeafReplayContext &generatedLeafCtx) const {
  const RefoldModel::MacroInvocation &m = generatedLeafCtx.invocation;
  StringRef baseInvText = generatedLeafCtx.baseInvocationText;
  ArrayRef<std::pair<size_t, size_t>> invArgRanges =
      generatedLeafCtx.invocationArgRanges;
  const std::pair<size_t, size_t> *bEnv = &generatedLeafCtx.bTokenEnvelope;
  const RefoldModel::MacroDirective *rootDefinition =
      &generatedLeafCtx.rootDefinition;
  InvocationActualRecoveryContext actualRecoveryCtx{m, baseInvText,
                                                    invArgRanges};

  SmallVector<LeafTok, 32> oldToks;
  SmallVector<LeafTok, 32> newToks;
  lexLeafTokens(generatedLeafCtx.oldExpansion, deps_.lexLang, oldToks);
  lexLeafTokens(generatedLeafCtx.newExpansion, deps_.lexLang, newToks);
  if (oldToks.empty() || newToks.empty())
    return std::nullopt;

  std::optional<std::pair<std::string, std::string>> leafRewrite;
  bool scalarLeafRewriteConflict = false;
  if (oldToks.size() == newToks.size()) {
    for (size_t i = 0; i < oldToks.size(); ++i) {
      if (oldToks[i].spelling == newToks[i].spelling)
        continue;
      std::optional<std::pair<std::string, std::string>> changed =
          changedMiddleSubstrings(
              StringRef(leafValueForToken(oldToks[i].spelling)),
              StringRef(leafValueForToken(newToks[i].spelling)));
      if (!changed) {
        scalarLeafRewriteConflict = true;
        leafRewrite.reset();
        break;
      }
      if (leafRewrite) {
        if (leafRewrite->first != changed->first ||
            leafRewrite->second != changed->second) {
          scalarLeafRewriteConflict = true;
          leafRewrite.reset();
          break;
        }
        continue;
      }
      leafRewrite = std::move(changed);
    }

    if (leafRewrite && !scalarLeafRewriteConflict) {
      // A generated-leaf rewrite edits one root invocation argument.  Such an
      // edit is admissible only if the whole owner expansion is consistent
      // with that single source change.  If the same solved old leaf remains
      // unchanged somewhere else in the owner cover, then rewriting the root
      // argument would also rewrite that occurrence.  Rejecting here prevents
      // cases such as REGISTER_COMMAND(open), where paste/stringify/call uses
      // become OPEN but one raw `open` occurrence intentionally stays open.
      for (size_t i = 0; i < oldToks.size(); ++i) {
        if (oldToks[i].spelling != newToks[i].spelling)
          continue;
        std::string stableValue = leafValueForToken(oldToks[i].spelling);
        if (StringRef(stableValue) == StringRef(leafRewrite->first))
          return std::nullopt;
      }
    }
  }

  if (auto selectorPatch =
          TryGeneratedSelectorActualRewrite(generatedLeafCtx, oldToks, newToks))
    return selectorPatch;

  auto trailingVariadicIsStringifiedByAnyRootCallee = [&]() {
    for (uint32_t argIdx = 0; argIdx < invArgRanges.size(); ++argIdx) {
      auto r = invArgRanges[argIdx];
      if (r.second < r.first || r.second > baseInvText.size())
        return false;
      const RefoldModel::MacroDirective *callee =
          deps_.resolveFunctionLikeMacroForReplay(
              baseInvText.slice(r.first, r.second).trim());
      if (!callee || callee->defParams.empty() ||
          !callee->defParams.back().variadic)
        continue;
      const uint32_t variadicIdx =
          static_cast<uint32_t>(callee->defParams.size() - 1);
      for (size_t i = 0; i + 1 < callee->replacementTokens.size(); ++i) {
        if (callee->replacementTokens[i].spelling == "#" &&
            callee->replacementTokens[i + 1].kind ==
                RefoldModel::MacroReplacementTokenKind::ParamRef &&
            callee->replacementTokens[i + 1].paramIndex &&
            *callee->replacementTokens[i + 1].paramIndex == variadicIdx)
          return true;
      }
    }
    return false;
  };

  // `__VA_OPT__` activation can add a new variadic root argument even though
  // no old root argument text exists to replace.  Accept only the canonical
  // case where the old expansion is a prefix of the new expansion and the
  // inserted tail contains exactly one data token after fixed punctuation;
  // the new token is inserted as the missing variadic actual.
  const bool rootHasTrailingVariadic =
      !rootDefinition->defParams.empty() &&
      rootDefinition->defParams.back().variadic;
  const bool omittedVariadicAbsent =
      rootHasTrailingVariadic &&
      invArgRanges.size() + 1 == rootDefinition->defParams.size();
  const bool omittedVariadicEmptyFormal =
      rootHasTrailingVariadic &&
      invArgRanges.size() == rootDefinition->defParams.size() &&
      !invArgRanges.empty() &&
      invArgRanges.back().first == invArgRanges.back().second;

  if (!leafRewrite && oldToks.size() < newToks.size() &&
      (omittedVariadicAbsent || omittedVariadicEmptyFormal)) {
    bool oldPrefixMatches = true;
    for (size_t i = 0; i < oldToks.size(); ++i) {
      if (oldToks[i].spelling != newToks[i].spelling) {
        oldPrefixMatches = false;
        break;
      }
    }
    if (oldPrefixMatches) {
      std::string insertedActual;
      bool sawInsertedData = false;
      const bool stringifiedTail =
          trailingVariadicIsStringifiedByAnyRootCallee();
      for (size_t i = oldToks.size(); i < newToks.size(); ++i) {
        StringRef spelling(newToks[i].spelling);
        const std::string decoded = leafValueForToken(spelling);
        // Leading `__VA_OPT__` separators are replacement-list context, not
        // source text to insert.  Once actual data has started, commas are
        // ordinary variadic-pack separators and must be preserved so multi-
        // actual activations become `, 2, 3` at the root callsite rather than
        // a single collapsed token.
        if (!sawInsertedData &&
            (spelling == ":" || spelling == "," || StringRef(decoded) == ":" ||
             StringRef(decoded) == ","))
          continue;
        if (stringifiedTail) {
          if (sawInsertedData)
            return std::nullopt;
          std::optional<std::string> decodedInserted =
              decodeSimpleStringLiteralToken(spelling);
          if (!decodedInserted)
            return std::nullopt;
          insertedActual = std::move(*decodedInserted);
          sawInsertedData = true;
          continue;
        }
        if (spelling == "," || StringRef(decoded) == ",") {
          if (insertedActual.empty() || insertedActual.back() == ' ')
            insertedActual += ",";
          else
            insertedActual += ",";
          insertedActual += " ";
        } else {
          if (!insertedActual.empty() && insertedActual.back() != ' ' &&
              insertedActual.back() != ',')
            insertedActual += " ";
          insertedActual += spelling.str();
        }
        sawInsertedData = true;
      }
      if (sawInsertedData && !StringRef(insertedActual).trim().empty()) {
        size_t close = baseInvText.rfind(')');
        if (close == StringRef::npos)
          return std::nullopt;
        std::string rewritten = baseInvText.slice(0, close).str();
        rewritten += ", ";
        rewritten += StringRef(insertedActual).trim();
        rewritten += baseInvText.substr(close).str();

        MacroPatch patch{*m.invB, *m.invE, std::move(rewritten), m.id};
        patch.materialized.hasOutputByteRange = true;
        patch.materialized.outputByteStart = close;
        patch.materialized.outputByteEnd =
            close + 2 + StringRef(insertedActual).trim().size();
        certifyMacroPatchMaterializedBTokenRange(
            patch, static_cast<uint64_t>(bEnv->first),
            static_cast<uint64_t>(bEnv->second));
        deps_.proofCertifier.SetArgsOnlyStandardProof(
            patch, m,
            /*wholeEnvelopeReplayValidated=*/true);
        return patch;
      }
    }
  }

  if (auto patch = TryRemoveTrailingVariadicActual(
          generatedLeafCtx, oldToks, newToks, rootHasTrailingVariadic,
          leafRewrite.has_value()))
    return patch;

  if (auto patch = TryMultiLeafPatternRewrite(
          generatedLeafCtx, oldToks, newToks, actualRecoveryCtx))
    return patch;

  if (!leafRewrite)
    return std::nullopt;

  return TryStableRootScalarLeafRewrite(generatedLeafCtx, actualRecoveryCtx,
                                        leafRewrite->first,
                                        leafRewrite->second);
}

} // namespace

std::optional<MacroPatch>
RefoldMacroGeneratedLeafReplayEngine::BuildGeneratedLeafReplayCandidate(
    const GeneratedLeafReplayContext &generatedLeafCtx) const {
  return GeneratedLeafReplayResolver(deps_).TryBuild(generatedLeafCtx);
}

} // namespace refold
} // namespace clang
