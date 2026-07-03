//===--- RefoldMacroGeneratedLeafReplayEngine.cpp ------------*- C++ -*-===//
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

} // namespace

std::optional<MacroPatch>
RefoldMacroGeneratedLeafReplayEngine::BuildGeneratedLeafReplayCandidate(
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

  // Use the file-scope leaf-token carrier so leaf token-equivalence helpers
  // share one stable token representation.
  auto lexLeafTokens = [&](StringRef text, SmallVectorImpl<LeafTok> &out) {
    out.clear();
    SmallVector<RefoldLexBoundaryToken, 32> toks;
    refoldLexBoundaryTokens(text, deps_.lexLang, toks);
    for (const RefoldLexBoundaryToken &tok : toks)
      out.push_back(LeafTok{tok.spelling});
  };
  auto leafValueForToken = [&](StringRef spelling) -> std::string {
    if (std::optional<std::string> decoded =
            decodeSimpleStringLiteralToken(spelling))
      return *decoded;
    return spelling.str();
  };

  auto leafTextsTokenEquivalent = [&](StringRef lhs, StringRef rhs) {
    SmallVector<LeafTok, 16> lhsToks;
    SmallVector<LeafTok, 16> rhsToks;
    lexLeafTokens(lhs, lhsToks);
    lexLeafTokens(rhs, rhsToks);
    if (lhsToks.size() != rhsToks.size())
      return false;
    for (size_t i = 0; i < lhsToks.size(); ++i)
      if (lhsToks[i].spelling != rhsToks[i].spelling)
        return false;
    return true;
  };

  SmallVector<LeafTok, 32> oldToks;
  SmallVector<LeafTok, 32> newToks;
  lexLeafTokens(generatedLeafCtx.oldExpansion, oldToks);
  lexLeafTokens(generatedLeafCtx.newExpansion, newToks);
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

  auto tryGeneratedSelectorActualRewrite = [&]() -> std::optional<MacroPatch> {
    if (oldToks.empty() || newToks.empty())
      return std::nullopt;

    auto appendLexedArgumentTokens = [&](StringRef text,
                                         SmallVectorImpl<std::string> &out) {
      SmallVector<RefoldLexBoundaryToken, 16> toks;
      refoldLexBoundaryTokens(text, deps_.lexLang, toks);
      for (const RefoldLexBoundaryToken &tok : toks)
        out.push_back(tok.spelling);
    };

    auto stringifyArgumentForReplay = [&](StringRef text) -> std::string {
      SmallVector<RefoldLexBoundaryToken, 16> toks;
      refoldLexBoundaryTokens(text, deps_.lexLang, toks);
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
    };

    auto replayFunctionLikeToSpellings =
        [&](const RefoldModel::MacroDirective &definition,
            ArrayRef<StringRef> actuals)
        -> std::optional<SmallVector<std::string, 16>> {
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
          out.push_back(
              stringifyArgumentForReplay(actuals[*toks[i + 1].paramIndex]));
          i += 2;
          continue;
        }
        if (i + 1 < toks.size() && toks[i + 1].spelling == "##") {
          std::string pasted;
          auto appendPastePiece =
              [&](const RefoldModel::MacroReplacementToken &piece) {
                if (piece.kind ==
                    RefoldModel::MacroReplacementTokenKind::ParamRef) {
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
              };
          if (!appendPastePiece(tok))
            return std::nullopt;
          i += 2;
          while (true) {
            if (i >= toks.size() || !appendPastePiece(toks[i]))
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
          appendLexedArgumentTokens(actuals[*tok.paramIndex], out);
          ++i;
          continue;
        }
        if (tok.spelling == "##" || tok.spelling == "__VA_OPT__")
          return std::nullopt;
        out.push_back(tok.spelling.str());
        ++i;
      }
      return out;
    };

    struct SelectorCandidate {
      std::string replacement;
      uint32_t rootArgIdx = 0;
      size_t newSelectorSize = 0;
    };
    SmallVector<SelectorCandidate, 4> candidates;

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
      if (selectorIdx + oldSelectorDef->defParams.size() >=
          invArgRanges.size() + 1)
        continue;

      SmallVector<StringRef, 8> actuals;
      for (size_t i = 0; i < oldSelectorDef->defParams.size(); ++i) {
        const auto r = invArgRanges[selectorIdx + 1 + i];
        if (r.second < r.first || r.second > baseInvText.size())
          return std::nullopt;
        actuals.push_back(baseInvText.slice(r.first, r.second).trim());
      }
      std::optional<SmallVector<std::string, 16>> oldReplay =
          replayFunctionLikeToSpellings(*oldSelectorDef, actuals);
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
            replayFunctionLikeToSpellings(*candidateDef, actuals);
        if (!candidateReplay ||
            !leafTokenSpellingsEqual(newToks, *candidateReplay))
          continue;

        std::string replacement = baseInvText.str();
        replacement.replace(selectorRange.first,
                            selectorRange.second - selectorRange.first,
                            sourceDirective.name.str());
        candidates.push_back(SelectorCandidate{
            std::move(replacement), selectorIdx, sourceDirective.name.size()});
      }
    }

    if (candidates.empty())
      return std::nullopt;
    llvm::sort(candidates,
               [](const SelectorCandidate &lhs, const SelectorCandidate &rhs) {
                 return lhs.replacement < rhs.replacement;
               });
    for (const SelectorCandidate &candidate : candidates)
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
  };

  if (auto selectorPatch = tryGeneratedSelectorActualRewrite())
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

  auto removeTrailingVariadicActual = [&]() -> std::optional<MacroPatch> {
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
    // argument: start at the previous argument's end so the separating comma
    // and whitespace disappear together with the old variadic payload.
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
  };

  if (!leafRewrite && oldToks.size() > newToks.size() &&
      rootHasTrailingVariadic) {
    bool newPrefixMatches = true;
    for (size_t i = 0; i < newToks.size(); ++i) {
      if (oldToks[i].spelling != newToks[i].spelling) {
        newPrefixMatches = false;
        break;
      }
    }
    if (newPrefixMatches) {
      bool onlyVaOptTailRemoved = true;
      for (size_t i = newToks.size(); i < oldToks.size(); ++i) {
        StringRef spelling(oldToks[i].spelling);
        const std::string decoded = leafValueForToken(spelling);
        if (spelling == ":" || spelling == "," || StringRef(decoded) == ":" ||
            StringRef(decoded) == ",")
          continue;
        // The remaining removed token is the old variadic data token.  There
        // must be exactly one such token for this narrow deactivation proof;
        // richer pack edits belong in the full generated-argument DAG proof.
        if (i + 1 != oldToks.size())
          onlyVaOptTailRemoved = false;
      }
      if (onlyVaOptTailRemoved) {
        if (auto patch = removeTrailingVariadicActual())
          return patch;
      }
    }
  }

  auto tryMultiLeafPatternRewrite = [&]() -> std::optional<MacroPatch> {
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
      struct ArgOccurrence {
        uint32_t argIdx = 0;
        size_t begin = 0;
        size_t end = 0;
        std::string oldText;

        // The multi-leaf matcher can compare in two different domains:
        // ordinary source spelling, or the decoded payload of a source string
        // literal.  The latter is probe evidence only.  If it produces a new
        // source actual, the solved payload must be re-encoded as a string
        // literal instead of emitted as a raw identifier/token sequence.
        bool matchedDecodedSourceStringLiteral = false;
        std::string sourceText;
      };
      SmallVector<ArgOccurrence, 8> occs;
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
        struct Probe {
          StringRef text;
          bool decodedSourceStringLiteral = false;
        };
        SmallVector<Probe, 2> probes;
        probes.push_back(Probe{argText, false});

        // Keep the decoded spelling alive for the full probe loop.  More
        // importantly, tag it as decoded evidence so a later source rewrite
        // re-quotes the solved payload.  Decoding makes `"beta"` comparable
        // to the generated leaf value `beta`, but it does not prove that the
        // source actual may be rewritten to the raw identifier `gamma`.
        std::optional<std::string> decodedArgStorage =
            decodeSimpleStringLiteralToken(argText);
        if (decodedArgStorage)
          probes.push_back(Probe{StringRef(*decodedArgStorage), true});

        for (Probe probe : probes) {
          if (probe.text.empty())
            continue;
          size_t pos = oldValue.find(probe.text);
          if (pos == StringRef::npos)
            continue;
          if (oldValue.find(probe.text, pos + probe.text.size()) !=
              StringRef::npos)
            return std::nullopt;
          ArgOccurrence occ;
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
        ArgOccurrence occ;
        occ.argIdx = *emptyArgIdx;
        occ.begin = oldValue.size();
        occ.end = oldValue.size();
        occ.oldText = "";
        occs.push_back(std::move(occ));
      }
      if (occs.empty())
        return std::nullopt;
      llvm::sort(occs, [](const ArgOccurrence &lhs, const ArgOccurrence &rhs) {
        if (lhs.begin != rhs.begin)
          return lhs.begin < rhs.begin;
        return lhs.argIdx < rhs.argIdx;
      });
      for (size_t i = 1; i < occs.size(); ++i)
        if (occs[i].begin < occs[i - 1].end)
          return std::nullopt;

      auto rewriteSolvedLeafAsSource =
          [&](const ArgOccurrence &occ,
              StringRef solved) -> std::optional<std::string> {
        if (!occ.matchedDecodedSourceStringLiteral)
          return solved.str();

        StringRef source = StringRef(occ.sourceText).trim();
        const size_t quote = source.find('"');
        const size_t endQuote = source.rfind('"');
        if (quote == StringRef::npos || endQuote == StringRef::npos ||
            endQuote <= quote)
          return std::nullopt;

        // Preserve the source literal prefix (`L`, `u8`, etc.) and any suffix
        // spelling, but replace the decoded payload with a freshly quoted C
        // string literal.  This keeps decoded-payload matching from silently
        // changing an ordinary forwarded string literal into an identifier.
        std::string rewritten = source.slice(0, quote).str();
        rewritten += stringutils::quoteCStringLiteral(solved);
        rewritten += source.substr(endQuote + 1).str();
        return rewritten;
      };

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
        std::optional<std::string> sourceSolved =
            rewriteSolvedLeafAsSource(occs[i], solved);
        if (!sourceSolved)
          return std::nullopt;
        StringRef sourceSolvedRef(*sourceSolved);
        auto existing = replByArgIdx.find(occs[i].argIdx);
        if (existing != replByArgIdx.end()) {
          if (!leafTextsTokenEquivalent(StringRef(existing->second),
                                        sourceSolvedRef))
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
  };

  if (auto patch = tryMultiLeafPatternRewrite())
    return patch;

  if (!leafRewrite)
    return std::nullopt;

  const std::string &oldLeaf = leafRewrite->first;
  const std::string &newLeaf = leafRewrite->second;
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
        argText.str(), pos, pos + oldLeaf.size(), StringRef(newLeaf));
    if (!isMacroInvocationVariadicFormal(m, argIdx) &&
        replacementIntroducesTopLevelComma(rewrittenArg, deps_.lexLang))
      return std::nullopt;
    targetArgIdx = argIdx;
    targetReplacement = std::move(rewrittenArg);
  }
  if (!targetArgIdx || !targetReplacement)
    return std::nullopt;

  // A generated-leaf rewrite is a source edit to the root invocation
  // argument.  Therefore every current-level occurrence of that root formal
  // that still observes the solved old leaf in B would also be rewritten by
  // the proposed source change.  Reject that shape instead of preserving the
  // parent callsite: the edited leaf must be materialized or proved by the
  // DAG path that can account for all child/parent observations together.
  auto stableRootOccurrenceStillObservesOldLeaf =
      [&](const RefoldModel::PPArgSpan &span, StringRef observerKind) {
        if (span.argIdx != *targetArgIdx || span.begin >= span.end)
          return false;

        StringRef aText =
            deps_.sourceMapper.SliceASource(span.begin, span.end).trim();
        if (aText.find(StringRef(oldLeaf)) == StringRef::npos)
          return false;

        std::optional<std::pair<size_t, size_t>> bEnv =
            deps_.sourceMapper.MapAToBTokenEnvelopeByPPArgSpan(span);
        if (!bEnv || bEnv->second < bEnv->first) {
          return true;
        }

        StringRef bText =
            deps_.sourceMapper.SliceBSource(bEnv->first, bEnv->second).trim();
        if (bText.find(StringRef(oldLeaf)) == StringRef::npos)
          return false;

        return true;
      };

  for (const RefoldModel::PPArgSpan &span :
       generatedLeafCtx.invocation.argSpans) {
    if (stableRootOccurrenceStillObservesOldLeaf(span, "standard"))
      return std::nullopt;
  }
  for (const RefoldModel::PPArgSpan &span :
       generatedLeafCtx.invocation.stringifySpans) {
    if (stableRootOccurrenceStillObservesOldLeaf(span, "stringify"))
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

} // namespace refold
} // namespace clang
