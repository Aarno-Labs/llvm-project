//===--- RefoldMacroGeneratedCalleeReplayEngine.cpp -------------*- C++ -*-===//
//
// Implementation of the higher-order generated-callee replay engine.
//
// The engine has no back-reference to `RefoldMacroPatchPlanner`.
// Planner-side helpers still owned by the planner
// (`BuildInvocationRewriteWithRange`, `ResolveFunctionLikeMacro*`) are
// reached through the std::function callbacks supplied in the engine's
// Dependencies bundle.
//
//===----------------------------------------------------------------------===//

#include "macro/RefoldMacroGeneratedCalleeReplayEngine.h"

#include "core/RefoldLog.h"
#include "core/RefoldModel.h"
#include "macro/RefoldArgTextRecovery.h"
#include "macro/RefoldMacroPatchProofCertifier.h"
#include "macro/RefoldMacroPlannerHelpers.h"
#include "macro/RefoldMacroTupleHelpers.h"
#include "source/RefoldSourceMapper.h"
#include "source/RefoldToken.h"
#include "source/TokenTextHelpers.h"
#include "util/StringUtils.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/FormatVariadic.h"

#include <algorithm>
#include <optional>
#include <string>
#include <utility>

using namespace llvm;

namespace clang {
namespace refold {

namespace {

/// Local token carrier used by generated-callee replay-token comparisons.
/// Local token spelling/range record used while analyzing a generated-callee
/// call surface.
struct ReplayTok {
  std::string spelling;
  size_t begin = 0;
  size_t end = 0;
};

/// Replacement-list shape for one deterministic generated call step.
///
/// The generated-callee engine constructs these shapes while replaying a callee
/// call that was produced by macro expansion rather than source spelling.
struct GeneratedCalleeCallShape {
  uint32_t calleeParamIdx = 0;
  llvm::SmallVector<std::pair<size_t, size_t>, 8> argTokenRanges;
  llvm::SmallVector<std::string, 8> prefixLiterals;
  llvm::SmallVector<std::string, 8> suffixLiterals;
};

} // namespace

RefoldMacroGeneratedCalleeReplayEngine::RefoldMacroGeneratedCalleeReplayEngine(
    Dependencies deps)
    : deps_(std::move(deps)) {}

bool RefoldMacroGeneratedCalleeReplayEngine::
    GeneratedCalleeReplayPreservesEnvelope(
        const GeneratedCalleeReplayContext &ctx) const {
  return ctx.wholeCoverATokens.first < ctx.wholeCoverATokens.second &&
         ctx.bTokenEnvelope.first < ctx.bTokenEnvelope.second;
}

bool RefoldMacroGeneratedCalleeReplayEngine::GeneratedCalleeReplayIsAdmissible(
    const GeneratedCalleeReplayContext &ctx) const {
  return ctx.followedGeneratedCall && ctx.currentDefinition &&
         !ctx.currentActuals.empty() &&
         macroDefinitionAcceptsActualCount(*ctx.currentDefinition,
                                           ctx.currentActuals.size());
}

std::optional<MacroPatch>
RefoldMacroGeneratedCalleeReplayEngine::BuildGeneratedCalleeReplayCandidate(
    const GeneratedCalleeReplayContext &generatedCalleeCtx) const {
  // Keep the same fail-closed entry gates after the caller has computed the
  // owner cover and B-token envelope.
  if (!GeneratedCalleeReplayPreservesEnvelope(generatedCalleeCtx))
    return std::nullopt;

  const RefoldModel::MacroInvocation &m = generatedCalleeCtx.invocation;
  StringRef baseInvText = generatedCalleeCtx.baseInvocationText;
  ArrayRef<std::pair<size_t, size_t>> invArgRanges =
      generatedCalleeCtx.invocationArgRanges;
  const std::pair<uint64_t, uint64_t> *cover =
      &generatedCalleeCtx.wholeCoverATokens;
  const std::pair<size_t, size_t> *bEnv = &generatedCalleeCtx.bTokenEnvelope;
  InvocationActualRecoveryContext actualRecoveryCtx{m, baseInvText,
                                                    invArgRanges};

  auto findGeneratedCallShape =
      [&](const RefoldModel::MacroDirective &definition)
      -> std::optional<GeneratedCalleeCallShape> {
    const auto &toks = definition.replacementTokens;
    if (toks.empty())
      return std::nullopt;

    auto isLiteralToken = [&](size_t idx, StringRef spelling) {
      return idx < toks.size() &&
             toks[idx].kind ==
                 RefoldModel::MacroReplacementTokenKind::Literal &&
             toks[idx].spelling == spelling;
    };

    auto findMatchingParen = [&](size_t openIdx,
                                 size_t limit) -> std::optional<size_t> {
      if (!isLiteralToken(openIdx, "("))
        return std::nullopt;
      unsigned depth = 1;
      for (size_t i = openIdx + 1; i < limit; ++i) {
        if (toks[i].kind != RefoldModel::MacroReplacementTokenKind::Literal)
          continue;
        if (toks[i].spelling == "(") {
          ++depth;
          continue;
        }
        if (toks[i].spelling != ")")
          continue;
        if (--depth == 0)
          return i;
      }
      return std::nullopt;
    };

    auto collectTopLevelArgRanges =
        [&](size_t openIdx, size_t closeIdx,
            SmallVectorImpl<std::pair<size_t, size_t>> &out) {
          if (openIdx >= closeIdx)
            return false;
          size_t argBegin = openIdx + 1;
          unsigned argDepth = 0;
          for (size_t i = openIdx + 1; i <= closeIdx; ++i) {
            const bool atEnd = i == closeIdx;
            if (!atEnd) {
              const auto &tok = toks[i];
              if (tok.kind == RefoldModel::MacroReplacementTokenKind::Literal) {
                if (tok.spelling == "(") {
                  ++argDepth;
                } else if (tok.spelling == ")") {
                  if (argDepth == 0)
                    return false;
                  --argDepth;
                }
              }
            }

            if (atEnd || (argDepth == 0 &&
                          toks[i].kind ==
                              RefoldModel::MacroReplacementTokenKind::Literal &&
                          toks[i].spelling == ",")) {
              if (argBegin == i)
                return false;
              out.push_back({argBegin, i});
              argBegin = i + 1;
            }
          }
          return !out.empty();
        };

    // Resolve the replacement-list expression that supplies the generated
    // callee to the current macro formal that owns the callee spelling.  The
    // direct case is the usual `F(...)` shape.  The selector case covers a
    // producer-proven, deterministic function-like selector such as
    // `SELECT(F)(x)`, where `SELECT(f)` replays to exactly one of its formals.
    // This is still a structural proof over replacement-list tokens: the
    // selector macro must be function-like, arity-compatible, and its whole
    // replacement list must be the selected formal.  No text-keyed guessing is
    // used to decide which root argument owns the generated callee.
    std::function<std::optional<uint32_t>(size_t, size_t, unsigned)>
        resolveCalleeParamInRange =
            [&](size_t begin, size_t end,
                unsigned depth) -> std::optional<uint32_t> {
      if (begin >= end || end > toks.size() ||
          depth > deps_.model.GetMacroDirectives().size())
        return std::nullopt;

      if (end == begin + 1 &&
          toks[begin].kind ==
              RefoldModel::MacroReplacementTokenKind::ParamRef &&
          toks[begin].paramIndex &&
          *toks[begin].paramIndex < definition.defParams.size())
        return *toks[begin].paramIndex;

      if (begin + 3 > end ||
          toks[begin].kind != RefoldModel::MacroReplacementTokenKind::Literal ||
          !isLiteralToken(begin + 1, "("))
        return std::nullopt;

      auto selectorClose = findMatchingParen(begin + 1, end);
      if (!selectorClose || *selectorClose + 1 != end)
        return std::nullopt;

      const RefoldModel::MacroDirective *selectorDefinition =
          deps_.resolveFunctionLikeMacroForReplay(
              StringRef(toks[begin].spelling));
      if (!selectorDefinition || selectorDefinition->defParams.empty())
        return std::nullopt;

      SmallVector<std::pair<size_t, size_t>, 8> selectorArgs;
      if (!collectTopLevelArgRanges(begin + 1, *selectorClose, selectorArgs) ||
          !macroDefinitionAcceptsActualCount(*selectorDefinition,
                                             selectorArgs.size()))
        return std::nullopt;

      // A generated-callee selector is deterministic only when the selector's
      // replacement tape is exactly one non-variadic formal.  Richer selector
      // expressions can be added later as their own replay proof, but they do
      // not belong in this single-owner callee-slot inversion.
      if (selectorDefinition->replacementTokens.size() != 1)
        return std::nullopt;
      const RefoldModel::MacroReplacementToken &selected =
          selectorDefinition->replacementTokens.front();
      if (selected.kind != RefoldModel::MacroReplacementTokenKind::ParamRef ||
          !selected.paramIndex ||
          *selected.paramIndex >= selectorDefinition->defParams.size() ||
          selectorDefinition->defParams[*selected.paramIndex].variadic ||
          *selected.paramIndex >= selectorArgs.size())
        return std::nullopt;

      const auto selectedArg = selectorArgs[*selected.paramIndex];
      return resolveCalleeParamInRange(selectedArg.first, selectedArg.second,
                                       depth + 1);
    };

    bool found = false;
    size_t callBegin = toks.size();
    size_t callOpen = toks.size();
    size_t callClose = toks.size();
    uint32_t calleeParamIdx = 0;

    for (size_t open = 1; open < toks.size(); ++open) {
      if (!isLiteralToken(open, "("))
        continue;

      auto close = findMatchingParen(open, toks.size());
      if (!close)
        return std::nullopt;

      std::optional<size_t> matchedBegin;
      std::optional<uint32_t> matchedParam;
      for (size_t begin = 0; begin < open; ++begin) {
        auto resolved = resolveCalleeParamInRange(begin, open, 0);
        if (!resolved)
          continue;
        if (matchedBegin)
          return std::nullopt;
        matchedBegin = begin;
        matchedParam = *resolved;
      }
      if (!matchedBegin)
        continue;

      if (found)
        return std::nullopt;
      found = true;
      callBegin = *matchedBegin;
      callOpen = open;
      callClose = *close;
      calleeParamIdx = *matchedParam;
      // Nested generated calls inside this call's arguments are argument
      // expressions, not competing owner calls.  Skip the body after
      // recording the outer call so shapes such as `H(G(X))` remain one
      // generated call whose first actual is the expression `G(X)`.
      open = *close;
    }

    if (!found || calleeParamIdx >= definition.defParams.size())
      return std::nullopt;

    auto collectLiteralContext = [&](size_t begin, size_t end,
                                     SmallVectorImpl<std::string> &out) {
      for (size_t i = begin; i < end; ++i) {
        const auto &tok = toks[i];
        if (tok.kind != RefoldModel::MacroReplacementTokenKind::Literal ||
            tok.spelling == "#" || tok.spelling == "##" ||
            tok.spelling == "__VA_OPT__")
          return false;
        out.push_back(tok.spelling.str());
      }
      return true;
    };

    GeneratedCalleeCallShape shape;
    shape.calleeParamIdx = calleeParamIdx;
    if (!collectLiteralContext(0, callBegin, shape.prefixLiterals) ||
        !collectLiteralContext(callClose + 1, toks.size(),
                               shape.suffixLiterals)) {
      return std::nullopt;
    }

    if (!collectTopLevelArgRanges(callOpen, callClose, shape.argTokenRanges))
      return std::nullopt;
    return shape;
  };

  auto splitVariadicPackSourceSlot =
      [&](const GeneratedCalleeSourceSlot &slot,
          SmallVectorImpl<GeneratedCalleeSourceSlot> &out) {
        SmallVector<RefoldLexBoundaryToken, 32> toks;
        refoldLexBoundaryTokens(StringRef(slot.text), deps_.lexLang, toks);

        size_t elemBegin = 0;
        int parenDepth = 0;
        bool sawComma = false;
        SmallVector<std::pair<size_t, size_t>, 8> pieces;

        for (const RefoldLexBoundaryToken &tok : toks) {
          if (tok.spelling == "(") {
            ++parenDepth;
            continue;
          }
          if (tok.spelling == ")") {
            if (parenDepth > 0)
              --parenDepth;
            continue;
          }
          if (tok.spelling != "," || parenDepth != 0)
            continue;

          sawComma = true;
          StringRef elem =
              StringRef(slot.text).slice(elemBegin, tok.begin).trim();
          if (elem.empty())
            return false;
          pieces.push_back(
              {static_cast<size_t>(elem.data() - slot.text.data()),
               static_cast<size_t>(elem.data() - slot.text.data()) +
                   elem.size()});
          elemBegin = tok.end;
        }

        if (!sawComma) {
          out.push_back(slot);
          return true;
        }

        StringRef finalElem = StringRef(slot.text).drop_front(elemBegin).trim();
        if (finalElem.empty())
          return false;
        pieces.push_back(
            {static_cast<size_t>(finalElem.data() - slot.text.data()),
             static_cast<size_t>(finalElem.data() - slot.text.data()) +
                 finalElem.size()});

        // A variadic formal can be forwarded into a fixed-arity generated
        // callee. The producer records the root variadic tail as one invocation
        // argument range (`foo, bar, baz`), but substituting `__VA_ARGS__` into
        // a generated call exposes those comma-separated elements as positional
        // actuals. Split only at commas that macro argument collection would
        // see: nested parentheses protect commas, while brackets/braces
        // intentionally do not. Each piece keeps the whole root variadic source
        // as its rewrite owner so multiple solved final parameters can be
        // composed back into one root argument replacement.
        for (const auto &piece : pieces) {
          GeneratedCalleeSourceSlot split = slot;
          split.text =
              StringRef(slot.text).slice(piece.first, piece.second).str();
          split.rootSourceText = slot.rootSourceText;
          split.rootArgIdx = slot.rootArgIdx;
          out.push_back(std::move(split));
        }
        return true;
      };

  auto appendActualsForParam =
      [&](const RefoldModel::MacroDirective &definition,
          ArrayRef<GeneratedCalleeSourceSlot> actuals, uint32_t paramIdx,
          SmallVectorImpl<GeneratedCalleeSourceSlot> &out) {
        if (paramIdx >= definition.defParams.size())
          return false;
        if (!isMacroDirectiveVariadicParam(definition, paramIdx)) {
          if (paramIdx >= actuals.size())
            return false;
          out.push_back(actuals[paramIdx]);
          return true;
        }
        generatedCalleeCtx.usesVariadicForwarding = true;
        if (actuals.size() < paramIdx)
          return false;

        // If this variadic parameter is still represented by one root
        // invocation range, distribute the pack now.  If it has already been
        // distributed by an earlier generated-call step, preserve the
        // existing positional slots.
        if (actuals.size() == paramIdx + 1)
          return splitVariadicPackSourceSlot(actuals[paramIdx], out);

        for (size_t i = paramIdx; i < actuals.size(); ++i)
          out.push_back(actuals[i]);
        return true;
      };

  auto findEditableParamInGeneratedArgument =
      [&](const RefoldModel::MacroDirective &definition,
          ArrayRef<GeneratedCalleeSourceSlot> actuals, size_t begin,
          size_t end) -> std::optional<GeneratedCalleeSourceSlot> {
    std::optional<GeneratedCalleeSourceSlot> editable;
    const auto &toks = definition.replacementTokens;
    for (size_t i = begin; i < end; ++i) {
      const auto &tok = toks[i];
      if (tok.kind != RefoldModel::MacroReplacementTokenKind::ParamRef)
        continue;
      if (!tok.paramIndex || *tok.paramIndex >= definition.defParams.size())
        return std::nullopt;
      if (isMacroDirectiveVariadicParam(definition, *tok.paramIndex))
        return std::nullopt;
      if (*tok.paramIndex >= actuals.size())
        return std::nullopt;

      // A function-like macro actual immediately followed by `(` is a
      // generated callee selector inside this argument expression, not the
      // source slot whose value should be edited.  Treat it as fixed proof
      // context and keep looking for the ordinary data argument.
      const bool selectorPosition =
          i + 1 < end &&
          toks[i + 1].kind == RefoldModel::MacroReplacementTokenKind::Literal &&
          toks[i + 1].spelling == "(" &&
          deps_.resolveFunctionLikeMacroForReplay(
              StringRef(actuals[*tok.paramIndex].text));
      if (selectorPosition)
        continue;

      const GeneratedCalleeSourceSlot &slot = actuals[*tok.paramIndex];
      if (editable) {
        // A generated actual expression with multiple distinct data leaves
        // (for example `A ## B` or `G(A, B)`) is not a single final-callee
        // source slot.  Let the generated-leaf pattern solver below invert
        // the whole literal skeleton and compose all root leaves together;
        // accepting it here would greedily rewrite the first leaf to the
        // complete joined token, e.g. `foo, <empty>` -> `foobar, <empty>`.
        return std::nullopt;
      }
      editable = slot;
    }
    return editable;
  };

  auto instantiateGeneratedArgument =
      [&](const RefoldModel::MacroDirective &definition,
          ArrayRef<GeneratedCalleeSourceSlot> actuals, size_t begin, size_t end,
          SmallVectorImpl<GeneratedCalleeSourceSlot> &out) {
        const auto &toks = definition.replacementTokens;
        if (begin >= end)
          return false;

        // `__VA_ARGS__` as a complete generated actual distributes the
        // variadic pack positionally into the next generated callee.  This is
        // the same owner proof as ordinary argument replay; the only extra
        // obligation is that each pack element keeps its own root source
        // slot.
        if (end == begin + 1 &&
            toks[begin].kind ==
                RefoldModel::MacroReplacementTokenKind::ParamRef &&
            toks[begin].paramIndex &&
            isMacroDirectiveVariadicParam(definition, *toks[begin].paramIndex))
          return appendActualsForParam(definition, actuals,
                                       *toks[begin].paramIndex, out);

        // Active `__VA_OPT__(, __VA_ARGS__)` in a generated-call argument
        // list contributes additional positional arguments rather than bytes
        // inside the preceding argument.  Accept only the canonical
        // comma-plus-variadic form here; anything more complex remains
        // outside this proof.
        for (size_t i = begin; i < end; ++i) {
          if (toks[i].spelling != "__VA_OPT__")
            continue;
          if (i != begin + 1 || begin >= end ||
              toks[begin].kind !=
                  RefoldModel::MacroReplacementTokenKind::ParamRef ||
              !toks[begin].paramIndex || i + 5 != end ||
              toks[i + 1].spelling != "(" || toks[i + 2].spelling != "," ||
              toks[i + 3].kind !=
                  RefoldModel::MacroReplacementTokenKind::ParamRef ||
              !toks[i + 3].paramIndex ||
              !isMacroDirectiveVariadicParam(definition,
                                             *toks[i + 3].paramIndex) ||
              toks[i + 4].spelling != ")")
            return false;
          if (!appendActualsForParam(definition, actuals,
                                     *toks[begin].paramIndex, out))
            return false;
          return appendActualsForParam(definition, actuals,
                                       *toks[i + 3].paramIndex, out);
        }

        std::optional<GeneratedCalleeSourceSlot> editable =
            findEditableParamInGeneratedArgument(definition, actuals, begin,
                                                 end);
        if (!editable)
          return false;

        std::string text;
        for (size_t i = begin; i < end; ++i) {
          const auto &tok = toks[i];
          if (tok.spelling == "##") {
            generatedCalleeCtx.usesPaste = true;
            continue;
          }
          if (tok.spelling == "#") {
            generatedCalleeCtx.usesStringification = true;
            if (i + 1 >= end ||
                toks[i + 1].kind !=
                    RefoldModel::MacroReplacementTokenKind::ParamRef ||
                !toks[i + 1].paramIndex ||
                *toks[i + 1].paramIndex >= definition.defParams.size() ||
                isMacroDirectiveVariadicParam(definition,
                                              *toks[i + 1].paramIndex) ||
                *toks[i + 1].paramIndex >= actuals.size())
              return false;
            // A stringified generated argument is still an expression over
            // the same root source slot.  Materialize the old string-literal
            // spelling for replay, but keep the editable owner as the
            // unstringified source argument so the solved value rewrites `X`,
            // not `#X` or `"X"`.
            text.push_back('"');
            text += actuals[*toks[i + 1].paramIndex].text;
            text.push_back('"');
            ++i;
            continue;
          }
          if (tok.spelling == "__VA_OPT__")
            return false;
          if (tok.kind == RefoldModel::MacroReplacementTokenKind::ParamRef) {
            if (!tok.paramIndex ||
                *tok.paramIndex >= definition.defParams.size() ||
                isMacroDirectiveVariadicParam(definition, *tok.paramIndex) ||
                *tok.paramIndex >= actuals.size())
              return false;
            text += actuals[*tok.paramIndex].text;
            continue;
          }
          text += tok.spelling.str();
        }

        GeneratedCalleeSourceSlot slot;
        slot.text = std::move(text);
        slot.rootSourceText = editable->rootSourceText;
        slot.rootArgIdx = editable->rootArgIdx;
        out.push_back(std::move(slot));
        return true;
      };

  for (size_t depth = 0; depth <= deps_.model.GetMacroDirectives().size();
       ++depth) {
    auto shape = findGeneratedCallShape(*generatedCalleeCtx.currentDefinition);
    if (!shape)
      break;
    if (shape->calleeParamIdx >= generatedCalleeCtx.currentActuals.size())
      return std::nullopt;

    uint32_t nextAliasHops = 0;
    const RefoldModel::MacroDirective *nextDefinition =
        deps_.resolveFunctionLikeMacroThroughAliasesWithHops(
            StringRef(
                generatedCalleeCtx.currentActuals[shape->calleeParamIdx].text),
            &nextAliasHops);
    generatedCalleeCtx.objectAliasHopCount += nextAliasHops;
    if (!nextDefinition || nextDefinition->defParams.empty())
      return std::nullopt;

    SmallVector<GeneratedCalleeSourceSlot, 8> nextActuals;
    for (const auto &argRange : shape->argTokenRanges) {
      if (!instantiateGeneratedArgument(*generatedCalleeCtx.currentDefinition,
                                        generatedCalleeCtx.currentActuals,
                                        argRange.first, argRange.second,
                                        nextActuals))
        return std::nullopt;
    }

    if (!macroDefinitionAcceptsActualCount(*nextDefinition, nextActuals.size()))
      return std::nullopt;

    generatedCalleeCtx.replayPrefixLiterals.append(
        shape->prefixLiterals.begin(), shape->prefixLiterals.end());
    generatedCalleeCtx.replaySuffixStack.push_back(shape->suffixLiterals);
    generatedCalleeCtx.currentDefinition = nextDefinition;
    generatedCalleeCtx.currentActuals = std::move(nextActuals);
    generatedCalleeCtx.followedGeneratedCall = true;
    ++generatedCalleeCtx.generatedCallDepth;
  }

  if (!GeneratedCalleeReplayIsAdmissible(generatedCalleeCtx))
    return std::nullopt;

  const bool finalHasVariadic =
      !generatedCalleeCtx.currentDefinition->defParams.empty() &&
      generatedCalleeCtx.currentDefinition->defParams.back().variadic;
  const size_t finalFixed =
      finalHasVariadic
          ? generatedCalleeCtx.currentDefinition->defParams.size() - 1
          : generatedCalleeCtx.currentDefinition->defParams.size();

  SmallVector<std::string, 8> oldActuals;
  SmallVector<uint32_t, 8> rootSlotByFinalParam;
  SmallVector<std::string, 8> rootSourceByFinalParam;
  for (size_t i = 0; i < finalFixed; ++i) {
    oldActuals.push_back(generatedCalleeCtx.currentActuals[i].text);
    rootSlotByFinalParam.push_back(
        generatedCalleeCtx.currentActuals[i].rootArgIdx);
    rootSourceByFinalParam.push_back(
        generatedCalleeCtx.currentActuals[i].rootSourceText);
  }
  if (finalHasVariadic) {
    std::string variadicText;
    raw_string_ostream os(variadicText);
    for (size_t i = finalFixed; i < generatedCalleeCtx.currentActuals.size();
         ++i) {
      if (i != finalFixed)
        os << ", ";
      os << StringRef(generatedCalleeCtx.currentActuals[i].text).trim();
    }
    os.flush();
    oldActuals.push_back(std::move(variadicText));
    rootSlotByFinalParam.push_back(
        generatedCalleeCtx.currentActuals[finalFixed].rootArgIdx);
    rootSourceByFinalParam.push_back(
        generatedCalleeCtx.currentActuals[finalFixed].rootSourceText);
  }
  if (oldActuals.size() !=
          generatedCalleeCtx.currentDefinition->defParams.size() ||
      rootSlotByFinalParam.size() != oldActuals.size() ||
      rootSourceByFinalParam.size() != oldActuals.size())
    return std::nullopt;

  auto lexReplayTokens = [&](StringRef text, SmallVectorImpl<ReplayTok> &out) {
    out.clear();
    SmallVector<RefoldLexBoundaryToken, 32> toks;
    refoldLexBoundaryTokens(text, deps_.lexLang, toks);
    for (const RefoldLexBoundaryToken &tok : toks)
      out.push_back(ReplayTok{tok.spelling, tok.begin, tok.end});
  };

  auto tokenSpellingsForText = [&](StringRef text) {
    SmallVector<ReplayTok, 16> toks;
    lexReplayTokens(text, toks);
    SmallVector<std::string, 16> out;
    for (const ReplayTok &tok : toks)
      out.push_back(tok.spelling);
    return out;
  };

  auto textsTokenEquivalent = [&](StringRef lhs, StringRef rhs) {
    SmallVector<std::string, 16> lhsToks = tokenSpellingsForText(lhs);
    SmallVector<std::string, 16> rhsToks = tokenSpellingsForText(rhs);
    if (lhsToks.size() != rhsToks.size())
      return false;
    for (size_t i = 0; i < lhsToks.size(); ++i)
      if (lhsToks[i] != rhsToks[i])
        return false;
    return true;
  };
  enum class ReplayKind { Literal, Param, Stringify, Paste };
  struct PastePiece {
    bool isParam = false;
    uint32_t paramIdx = 0;
    std::string literal;
  };
  struct ReplayElem {
    ReplayKind kind = ReplayKind::Literal;
    std::string literal;
    uint32_t paramIdx = 0;
    std::vector<PastePiece> pastePieces;
  };

  auto pastePieceFromReplacementToken =
      [&](const RefoldModel::MacroReplacementToken &tok,
          PastePiece &piece) -> bool {
    if (tok.kind == RefoldModel::MacroReplacementTokenKind::ParamRef) {
      if (!tok.paramIndex || *tok.paramIndex >= oldActuals.size())
        return false;
      piece.isParam = true;
      piece.paramIdx = *tok.paramIndex;
      return true;
    }
    if (tok.spelling == "#" || tok.spelling == "##" ||
        tok.spelling == "__VA_OPT__")
      return false;
    piece.isParam = false;
    piece.literal = tok.spelling.str();
    return true;
  };

  std::function<bool(size_t, size_t, std::vector<ReplayElem> &)>
      parseReplayRange;
  parseReplayRange = [&](size_t begin, size_t end,
                         std::vector<ReplayElem> &out) {
    const auto &tokens =
        generatedCalleeCtx.currentDefinition->replacementTokens;
    for (size_t i = begin; i < end;) {
      const auto &tok = tokens[i];
      if (tok.spelling == "#") {
        generatedCalleeCtx.usesStringification = true;
        if (i + 1 >= end ||
            tokens[i + 1].kind !=
                RefoldModel::MacroReplacementTokenKind::ParamRef ||
            !tokens[i + 1].paramIndex)
          return false;
        const uint32_t paramIdx = *tokens[i + 1].paramIndex;
        if (paramIdx >= oldActuals.size())
          return false;
        ReplayElem elem;
        elem.kind = ReplayKind::Stringify;
        elem.paramIdx = paramIdx;
        out.push_back(std::move(elem));
        i += 2;
        continue;
      }

      if (i + 1 < end && tokens[i + 1].spelling == "##") {
        generatedCalleeCtx.usesPaste = true;
        ReplayElem elem;
        elem.kind = ReplayKind::Paste;
        PastePiece first;
        if (!pastePieceFromReplacementToken(tok, first))
          return false;
        elem.pastePieces.push_back(std::move(first));
        i += 2;
        while (true) {
          if (i >= end)
            return false;
          PastePiece next;
          if (!pastePieceFromReplacementToken(tokens[i], next))
            return false;
          elem.pastePieces.push_back(std::move(next));
          ++i;
          if (i >= end || tokens[i].spelling != "##")
            break;
          ++i;
        }
        out.push_back(std::move(elem));
        continue;
      }

      if (tok.kind == RefoldModel::MacroReplacementTokenKind::ParamRef) {
        if (!tok.paramIndex || *tok.paramIndex >= oldActuals.size())
          return false;
        ReplayElem elem;
        elem.kind = ReplayKind::Param;
        elem.paramIdx = *tok.paramIndex;
        out.push_back(std::move(elem));
        ++i;
        continue;
      }
      if (tok.spelling == "##" || tok.spelling == "__VA_OPT__")
        return false;
      ReplayElem elem;
      elem.kind = ReplayKind::Literal;
      elem.literal = tok.spelling.str();
      out.push_back(std::move(elem));
      ++i;
    }
    return true;
  };

  std::vector<ReplayElem> finalPattern;
  if (!parseReplayRange(
          0, generatedCalleeCtx.currentDefinition->replacementTokens.size(),
          finalPattern) ||
      finalPattern.empty())
    return std::nullopt;

  std::vector<ReplayElem> replayPattern;
  for (const std::string &literal : generatedCalleeCtx.replayPrefixLiterals) {
    ReplayElem elem;
    elem.kind = ReplayKind::Literal;
    elem.literal = literal;
    replayPattern.push_back(std::move(elem));
  }
  replayPattern.insert(replayPattern.end(), finalPattern.begin(),
                       finalPattern.end());
  for (auto it = generatedCalleeCtx.replaySuffixStack.rbegin();
       it != generatedCalleeCtx.replaySuffixStack.rend(); ++it) {
    for (const std::string &literal : *it) {
      ReplayElem elem;
      elem.kind = ReplayKind::Literal;
      elem.literal = literal;
      replayPattern.push_back(std::move(elem));
    }
  }

  using SolvedActuals = SmallVector<std::string, 8>;
  auto assignSolvedActual = [&](SolvedActuals &actuals, uint32_t paramIdx,
                                StringRef value) -> bool {
    if (paramIdx >= actuals.size())
      return false;
    if (!textsTokenEquivalent(actuals[paramIdx], oldActuals[paramIdx]) &&
        !textsTokenEquivalent(actuals[paramIdx], value))
      return false;
    if (textsTokenEquivalent(actuals[paramIdx], oldActuals[paramIdx])) {
      actuals[paramIdx] = value.trim().str();
      return true;
    }
    return textsTokenEquivalent(actuals[paramIdx], value);
  };

  auto solvePasteToken =
      [&](ArrayRef<PastePiece> pieces, StringRef spelling,
          const SolvedActuals &seed) -> SmallVector<SolvedActuals, 4> {
    SmallVector<SolvedActuals, 4> solutions;
    const bool hasLiteralAnchor =
        llvm::any_of(pieces, [](const PastePiece &piece) {
          return !piece.isParam && !piece.literal.empty();
        });
    if (!hasLiteralAnchor) {
      SolvedActuals cur = seed;
      size_t cursor = 0;
      for (const PastePiece &piece : pieces) {
        if (!piece.isParam) {
          if (!spelling.substr(cursor).starts_with(piece.literal))
            return solutions;
          cursor += piece.literal.size();
          continue;
        }
        if (piece.paramIdx >= oldActuals.size())
          return solutions;
        const size_t width =
            StringRef(oldActuals[piece.paramIdx]).trim().size();
        if (cursor + width > spelling.size())
          return solutions;
        if (!assignSolvedActual(cur, piece.paramIdx,
                                spelling.slice(cursor, cursor + width))) {
          return solutions;
        }
        cursor += width;
      }
      if (cursor == spelling.size())
        solutions.push_back(std::move(cur));
      return solutions;
    }

    std::function<void(size_t, size_t, SolvedActuals &)> dfsPaste;
    dfsPaste = [&](size_t pieceIdx, size_t cursor, SolvedActuals &cur) {
      if (solutions.size() > 1)
        return;
      if (pieceIdx == pieces.size()) {
        if (cursor == spelling.size())
          solutions.push_back(cur);
        return;
      }
      const PastePiece &piece = pieces[pieceIdx];
      if (!piece.isParam) {
        if (spelling.substr(cursor).starts_with(piece.literal))
          dfsPaste(pieceIdx + 1, cursor + piece.literal.size(), cur);
        return;
      }
      for (size_t end = cursor; end <= spelling.size(); ++end) {
        SolvedActuals next = cur;
        if (!assignSolvedActual(next, piece.paramIdx,
                                spelling.slice(cursor, end))) {
          continue;
        }
        dfsPaste(pieceIdx + 1, end, next);
        if (solutions.size() > 1)
          return;
      }
    };
    SolvedActuals start = seed;
    dfsPaste(0, 0, start);
    return solutions;
  };

  auto solveExpansion =
      [&](StringRef expansion) -> std::optional<SolvedActuals> {
    SmallVector<ReplayTok, 32> toks;
    lexReplayTokens(expansion, toks);
    SmallVector<SolvedActuals, 4> solutions;
    SolvedActuals seed;
    for (const std::string &actual : oldActuals)
      seed.push_back(actual);

    std::function<void(ArrayRef<ReplayElem>, size_t, SolvedActuals &)> dfs;
    dfs = [&](ArrayRef<ReplayElem> elems, size_t tokPos, SolvedActuals &cur) {
      if (solutions.size() > 1)
        return;
      if (elems.empty()) {
        if (tokPos == toks.size())
          solutions.push_back(cur);
        return;
      }
      const ReplayElem &elem = elems.front();
      ArrayRef<ReplayElem> rest = elems.drop_front();
      switch (elem.kind) {
      case ReplayKind::Literal:
        if (tokPos < toks.size() && toks[tokPos].spelling == elem.literal)
          dfs(rest, tokPos + 1, cur);
        return;
      case ReplayKind::Param: {
        for (size_t end = tokPos; end <= toks.size(); ++end) {
          StringRef value;
          if (end > tokPos) {
            const size_t byteBegin = toks[tokPos].begin;
            const size_t byteEnd = toks[end - 1].end;
            value = expansion.slice(byteBegin, byteEnd);
          }
          SolvedActuals next = cur;
          if (!assignSolvedActual(next, elem.paramIdx, value))
            continue;
          dfs(rest, end, next);
          if (solutions.size() > 1)
            return;
        }
        return;
      }
      case ReplayKind::Stringify: {
        if (tokPos >= toks.size())
          return;
        std::optional<std::string> content =
            decodeSimpleStringLiteralToken(toks[tokPos].spelling);
        if (!content)
          return;
        SolvedActuals next = cur;
        if (!assignSolvedActual(next, elem.paramIdx, StringRef(*content)))
          return;
        dfs(rest, tokPos + 1, next);
        return;
      }
      case ReplayKind::Paste: {
        if (tokPos >= toks.size())
          return;
        SmallVector<SolvedActuals, 4> pasteSolutions =
            solvePasteToken(elem.pastePieces, toks[tokPos].spelling, cur);
        for (SolvedActuals &pasteSol : pasteSolutions) {
          dfs(rest, tokPos + 1, pasteSol);
          if (solutions.size() > 1)
            return;
        }
        return;
      }
      }
    };

    dfs(replayPattern, 0, seed);
    if (solutions.size() != 1)
      return std::nullopt;
    return solutions.front();
  };

  StringRef oldExpansion =
      deps_.sourceMapper.SliceASource(cover->first, cover->second).trim();
  StringRef newExpansion =
      deps_.sourceMapper.SliceBSource(bEnv->first, bEnv->second).trim();
  std::optional<SolvedActuals> oldSolved = solveExpansion(oldExpansion);
  std::optional<SolvedActuals> newSolved = solveExpansion(newExpansion);
  if (!oldSolved || !newSolved || oldSolved->size() != oldActuals.size() ||
      newSolved->size() != oldActuals.size())
    return std::nullopt;

  auto rewriteSourceActualFromSolvedExpansion =
      [&](StringRef source, StringRef oldText,
          StringRef newText) -> std::optional<std::string> {
    oldText = oldText.trim();
    newText = newText.trim();
    SmallVector<RefoldLexBoundaryToken, 16> sourceToks;
    SmallVector<RefoldLexBoundaryToken, 16> oldToks;
    SmallVector<RefoldLexBoundaryToken, 16> newToks;
    refoldLexBoundaryTokens(source, deps_.lexLang, sourceToks);
    refoldLexBoundaryTokens(oldText, deps_.lexLang, oldToks);
    refoldLexBoundaryTokens(newText, deps_.lexLang, newToks);
    if (!oldToks.empty() && oldToks.size() == newToks.size() &&
        sourceToks.size() >= oldToks.size()) {
      std::optional<size_t> matchBegin;
      bool ambiguous = false;
      for (size_t i = 0; i + oldToks.size() <= sourceToks.size(); ++i) {
        bool same = true;
        for (size_t j = 0; j < oldToks.size(); ++j) {
          if (sourceToks[i + j].spelling != oldToks[j].spelling) {
            same = false;
            break;
          }
        }
        if (!same)
          continue;
        if (matchBegin) {
          ambiguous = true;
          break;
        }
        matchBegin = i;
      }
      if (matchBegin && !ambiguous) {
        std::string rewritten = source.str();
        for (size_t j = oldToks.size(); j > 0; --j) {
          const size_t idx = *matchBegin + j - 1;
          rewritten = stringutils::replaceRange(
              rewritten, sourceToks[idx].begin, sourceToks[idx].end,
              newToks[j - 1].spelling);
        }
        return rewritten;
      }
    }
    if (source.trim() == oldText)
      return newText.str();
    size_t pos = source.find(oldText);
    if (pos != StringRef::npos) {
      if (source.find(oldText, pos + 1) != StringRef::npos)
        return std::nullopt;
      return stringutils::replaceRange(source.str(), pos, pos + oldText.size(),
                                       newText);
    }

    // The generated actual may wrap or paste the editable root contribution
    // before the final callee observes it: `(X)` stringified as `(beta)`,
    // `pre_##X` stringified as `pre_beta`, `G(X)` stringified as `ID(beta)`,
    // or `X##_tail` pasted before a later callee paste.  When old/new solved
    // values have a unique common context, invert only the changed middle
    // slice back into the original root source argument.
    size_t prefix = 0;
    while (prefix < oldText.size() && prefix < newText.size() &&
           oldText[prefix] == newText[prefix])
      ++prefix;
    size_t suffix = 0;
    while (suffix + prefix < oldText.size() &&
           suffix + prefix < newText.size() &&
           oldText[oldText.size() - suffix - 1] ==
               newText[newText.size() - suffix - 1])
      ++suffix;
    if (prefix + suffix < oldText.size()) {
      StringRef oldMiddle =
          oldText.slice(prefix, oldText.size() - suffix).trim();
      StringRef newMiddle =
          newText.slice(prefix, newText.size() - suffix).trim();
      if (!oldMiddle.empty()) {
        size_t middlePos = source.find(oldMiddle);
        if (middlePos != StringRef::npos &&
            source.find(oldMiddle, middlePos + 1) == StringRef::npos)
          return stringutils::replaceRange(
              source.str(), middlePos, middlePos + oldMiddle.size(), newMiddle);
      }
    }
    return std::nullopt;
  };

  DenseMap<uint32_t, std::string> replByRootArgIdx;
  DenseMap<uint32_t, std::string> workingRootTextByArgIdx;
  for (uint32_t i = 0; i < newSolved->size(); ++i) {
    if (i >= rootSlotByFinalParam.size())
      return std::nullopt;
    const uint32_t rootIdx = rootSlotByFinalParam[i];
    if (rootIdx >= invArgRanges.size())
      return std::nullopt;

    // Unchanged final-callee actuals impose no source rewrite obligation.
    // This is important after variadic-pack distribution: several final
    // parameters may all point back to one root `__VA_ARGS__` argument, and
    // unchanged pack elements must not compete with the changed element's
    // composed replacement for that same root range.
    if (textsTokenEquivalent(StringRef((*oldSolved)[i]),
                             StringRef((*newSolved)[i])))
      continue;

    if (!isMacroInvocationVariadicFormal(m, rootIdx) &&
        StringRef((*newSolved)[i]).trim().empty())
      return std::nullopt;

    StringRef originalRoot =
        baseInvText
            .slice(invArgRanges[rootIdx].first, invArgRanges[rootIdx].second)
            .trim();
    auto workingIt = workingRootTextByArgIdx.find(rootIdx);
    StringRef source = workingIt != workingRootTextByArgIdx.end()
                           ? StringRef(workingIt->second)
                           : (i < rootSourceByFinalParam.size()
                                  ? StringRef(rootSourceByFinalParam[i])
                                  : StringRef(oldActuals[i]));

    if (workingIt != workingRootTextByArgIdx.end()) {
      // Multiple final-callee parameters can impose the same edit on one
      // root actual.  For example, `FWD_DUP(G, X) -> G(X, X)` followed by
      // `JOIN(a, b) -> a ## b` solves both final parameters as `aa -> bb`,
      // but both obligations target the single root argument `X`.  After the
      // first obligation has rewritten that root text, replay the current
      // obligation against the original root spelling and accept it as
      // already discharged only when it yields the exact current root text.
      // This keeps duplicate-use composition deterministic while still
      // rejecting genuinely conflicting same-root obligations.
      auto alreadySatisfied = rewriteSourceActualFromSolvedExpansion(
          originalRoot, StringRef((*oldSolved)[i]), StringRef((*newSolved)[i]));
      if (alreadySatisfied &&
          textsTokenEquivalent(StringRef(*alreadySatisfied), source))
        continue;
    }

    auto rewritten = rewriteSourceActualFromSolvedExpansion(
        source, StringRef((*oldSolved)[i]), StringRef((*newSolved)[i]));
    if (!rewritten)
      return std::nullopt;
    if (!isMacroInvocationVariadicFormal(m, rootIdx) &&
        replacementIntroducesTopLevelComma(*rewritten, deps_.lexLang))
      return std::nullopt;

    // Compose multiple solved final parameters that originate from the same
    // root argument, especially a distributed variadic pack.  Each step is
    // still uniquely inverted against the current source text; if two solved
    // obligations cannot be composed into one deterministic root replacement,
    // the proof fails closed instead of choosing an arbitrary pack rewrite.
    workingRootTextByArgIdx[rootIdx] = std::move(*rewritten);
    StringRef finalRoot = StringRef(workingRootTextByArgIdx[rootIdx]).trim();
    if (finalRoot != originalRoot)
      replByRootArgIdx[rootIdx] = finalRoot.str();
    else
      replByRootArgIdx.erase(rootIdx);
  }

  if (replByRootArgIdx.empty())
    return std::nullopt;

  std::optional<InvocationRewriteWithRange> rewrite =
      deps_.buildInvocationRewriteWithRange(
          actualRecoveryCtx, replByRootArgIdx,
          /*materializedRangeByArgIdx=*/nullptr);
  if (!rewrite)
    return std::nullopt;

  MacroPatch patch{*m.invB, *m.invE, std::move(rewrite->text), m.id};
  deps_.proofCertifier.CertifyInvocationRewriteMaterializedOutputRange(
      patch, rewrite->materializedOutputByteStart,
      rewrite->materializedOutputByteEnd);
  certifyMacroPatchMaterializedBTokenRange(patch,
                                           static_cast<uint64_t>(bEnv->first),
                                           static_cast<uint64_t>(bEnv->second));
  deps_.proofCertifier.SetArgsOnlyStandardProof(
      patch, m, /*wholeEnvelopeReplayValidated=*/true);
  deps_.proofCertifier.CertifyGeneratedCalleeReplayProof(
      patch, m,
      generatedCalleeCtx.currentDefinition
          ? generatedCalleeCtx.currentDefinition->id
          : 0,
      generatedCalleeCtx.generatedCallDepth,
      generatedCalleeCtx.objectAliasHopCount,
      generatedCalleeCtx.usesStringification, generatedCalleeCtx.usesPaste,
      generatedCalleeCtx.usesVariadicForwarding,
      /*decodedStringLiteralEvidenceOnly=*/
      generatedCalleeCtx.usesStringification);
  return patch;
}

std::optional<MacroPatch> RefoldMacroGeneratedCalleeReplayEngine::
    BuildTupleGeneratedCalleeReplayCandidate(
        const TupleGeneratedCalleeReplayContext &tupleGeneratedCtx) const {
  const RefoldModel::MacroInvocation &m = tupleGeneratedCtx.invocation;
  StringRef baseInvText = tupleGeneratedCtx.baseInvocationText;
  const std::pair<uint64_t, uint64_t> *cover =
      &tupleGeneratedCtx.wholeCoverATokens;
  const std::pair<size_t, size_t> *bEnv = &tupleGeneratedCtx.bTokenEnvelope;

  const auto argRange =
      tupleGeneratedCtx.invocationArgRanges[tupleGeneratedCtx.callerArgIdx];
  if (argRange.second < argRange.first ||
      argRange.second > tupleGeneratedCtx.baseInvocationText.size())
    return std::nullopt;
  StringRef parentTrim = tupleGeneratedCtx.baseInvocationText
                             .slice(argRange.first, argRange.second)
                             .trim();
  if (!parentTrim.starts_with("(") || !parentTrim.ends_with(")") ||
      parentTrim.size() < 2)
    return std::nullopt;

  StringRef tuplePayload = parentTrim.drop_front().drop_back();
  SmallVector<TupleElementSlice, 8> tupleElems;
  if (!splitTopLevelTupleElementsWithLexer(tuplePayload, deps_.lexLang,
                                           tupleElems) ||
      tupleElems.size() < 2)
    return std::nullopt;

  auto tupleElementText = [&](size_t elemIdx) -> StringRef {
    const TupleElementSlice &elem = tupleElems[elemIdx];
    return tuplePayload.slice(elem.trimBegin, elem.trimEnd).trim();
  };

  struct GeneratedArgRef {
    uint32_t forwarderParamIdx = 0;
    bool variadicPack = false;
  };

  const auto &forwarderToks =
      tupleGeneratedCtx.forwarderDefinition.replacementTokens;

  // Recover the generated call inside the forwarding macro as a token-level
  // context rather than requiring the whole replacement list to be exactly
  // `F(X, ...)`.  This is the owner-level invariant for tuple-generated
  // callees: fixed literal tokens before/after the generated call belong to
  // the forwarding template, while the callee formal and generated actual
  // formals still map positionally back to tuple slots.  Examples accepted by
  // this proof include `F(X)`, `(F(X))`, and `F(X) "!"`; anything with
  // operators such as #/## in the forwarding layer remains outside this proof.
  size_t generatedCallBegin = forwarderToks.size();
  size_t generatedCallOpen = forwarderToks.size();
  size_t generatedCallClose = forwarderToks.size();
  uint32_t calleeForwarderParam = 0;
  bool foundGeneratedCall = false;
  for (size_t i = 0; i + 1 < forwarderToks.size(); ++i) {
    const auto &calleeTok = forwarderToks[i];
    const auto &openTok = forwarderToks[i + 1];
    if (calleeTok.kind != RefoldModel::MacroReplacementTokenKind::ParamRef ||
        !calleeTok.paramIndex ||
        openTok.kind != RefoldModel::MacroReplacementTokenKind::Literal ||
        openTok.spelling != "(") {
      continue;
    }

    unsigned depth = 1;
    size_t close = i + 2;
    for (; close < forwarderToks.size(); ++close) {
      const auto &tok = forwarderToks[close];
      if (tok.kind != RefoldModel::MacroReplacementTokenKind::Literal)
        continue;
      if (tok.spelling == "(") {
        ++depth;
        continue;
      }
      if (tok.spelling == ")") {
        if (--depth == 0)
          break;
      }
    }
    if (depth != 0 || close >= forwarderToks.size())
      return std::nullopt;
    if (foundGeneratedCall)
      return std::nullopt;
    foundGeneratedCall = true;
    generatedCallBegin = i;
    generatedCallOpen = i + 1;
    generatedCallClose = close;
    calleeForwarderParam = *calleeTok.paramIndex;
  }
  if (!foundGeneratedCall ||
      calleeForwarderParam >=
          tupleGeneratedCtx.forwarderDefinition.defParams.size() ||
      calleeForwarderParam >= tupleElems.size())
    return std::nullopt;

  SmallVector<std::string, 8> forwarderPrefixLiterals;
  SmallVector<std::string, 8> forwarderSuffixLiterals;
  auto collectForwarderLiteralContext = [&](size_t begin, size_t end,
                                            SmallVectorImpl<std::string> &out) {
    for (size_t i = begin; i < end; ++i) {
      const auto &tok = forwarderToks[i];
      if (tok.kind != RefoldModel::MacroReplacementTokenKind::Literal ||
          tok.spelling == "#" || tok.spelling == "##" ||
          tok.spelling == "__VA_OPT__")
        return false;
      out.push_back(tok.spelling.str());
    }
    return true;
  };
  if (!collectForwarderLiteralContext(0, generatedCallBegin,
                                      forwarderPrefixLiterals) ||
      !collectForwarderLiteralContext(generatedCallClose + 1,
                                      forwarderToks.size(),
                                      forwarderSuffixLiterals)) {
    return std::nullopt;
  }

  SmallVector<GeneratedArgRef, 8> generatedArgs;
  for (size_t i = generatedCallOpen + 1; i < generatedCallClose; ++i) {
    const auto &tok = forwarderToks[i];
    if (tok.spelling == "#" || tok.spelling == "##" ||
        tok.spelling == "__VA_OPT__")
      return std::nullopt;
    if (tok.kind != RefoldModel::MacroReplacementTokenKind::ParamRef)
      continue;
    if (!tok.paramIndex ||
        *tok.paramIndex >=
            tupleGeneratedCtx.forwarderDefinition.defParams.size())
      return std::nullopt;
    if (*tok.paramIndex == calleeForwarderParam)
      return std::nullopt;
    GeneratedArgRef ref;
    ref.forwarderParamIdx = *tok.paramIndex;
    ref.variadicPack =
        tupleGeneratedCtx.forwarderDefinition.defParams[*tok.paramIndex]
            .variadic;
    generatedArgs.push_back(ref);
  }
  if (generatedArgs.empty())
    return std::nullopt;

  uint32_t calleeAliasHops = 0;
  const RefoldModel::MacroDirective *calleeDefinition =
      deps_.resolveFunctionLikeMacroThroughAliasesWithHops(
          tupleElementText(static_cast<size_t>(calleeForwarderParam)),
          &calleeAliasHops);
  tupleGeneratedCtx.objectAliasHopCount += calleeAliasHops;
  if (!calleeDefinition || calleeDefinition->defParams.empty())
    return std::nullopt;

  SmallVector<StringRef, 8> oldGeneratedPieces;
  for (const GeneratedArgRef &ref : generatedArgs) {
    if (ref.variadicPack) {
      if (ref.forwarderParamIdx >= tupleElems.size())
        return std::nullopt;
      for (size_t i = ref.forwarderParamIdx; i < tupleElems.size(); ++i)
        oldGeneratedPieces.push_back(tupleElementText(i));
      continue;
    }
    if (ref.forwarderParamIdx >= tupleElems.size())
      return std::nullopt;
    oldGeneratedPieces.push_back(tupleElementText(ref.forwarderParamIdx));
  }

  const bool calleeHasVariadic = !calleeDefinition->defParams.empty() &&
                                 calleeDefinition->defParams.back().variadic;
  bool tupleReplayUsesStringification = false;
  bool tupleReplayUsesPaste = false;
  bool tupleReplayUsesVariadicForwarding = false;
  for (const GeneratedArgRef &ref : generatedArgs)
    tupleReplayUsesVariadicForwarding |= ref.variadicPack;
  const size_t fixedCalleeActuals = calleeHasVariadic
                                        ? calleeDefinition->defParams.size() - 1
                                        : calleeDefinition->defParams.size();
  if ((!calleeHasVariadic &&
       oldGeneratedPieces.size() != calleeDefinition->defParams.size()) ||
      (calleeHasVariadic && oldGeneratedPieces.size() < fixedCalleeActuals))
    return std::nullopt;

  SmallVector<std::string, 8> oldActuals;
  oldActuals.reserve(calleeDefinition->defParams.size());
  for (size_t i = 0; i < fixedCalleeActuals; ++i)
    oldActuals.push_back(oldGeneratedPieces[i].str());
  if (calleeHasVariadic) {
    std::string variadicText;
    raw_string_ostream os(variadicText);
    for (size_t i = fixedCalleeActuals; i < oldGeneratedPieces.size(); ++i) {
      if (i != fixedCalleeActuals)
        os << ", ";
      os << oldGeneratedPieces[i].trim();
    }
    os.flush();
    oldActuals.push_back(std::move(variadicText));
  }
  if (oldActuals.size() != calleeDefinition->defParams.size())
    return std::nullopt;

  StringRef oldExpansion =
      deps_.sourceMapper.SliceASource(cover->first, cover->second).trim();
  StringRef newExpansion =
      deps_.sourceMapper.SliceBSource(bEnv->first, bEnv->second).trim();

  // Replay the generated callee as a small replacement-list transducer over
  // tuple slots.  A generated callee contributes a sequence of literal
  // tokens, ordinary parameter projections, stringified projections, pasted-
  // token projections, and fixed forwarding-context tokens.  If that sequence
  // explains both the old whole-cover expansion and the new B-side expansion
  // uniquely, then the solved parameter values can be translated back to
  // positional tuple edits.
  auto lexReplayTokens = [&](StringRef text, SmallVectorImpl<ReplayTok> &out) {
    out.clear();
    SmallVector<RefoldLexBoundaryToken, 32> toks;
    refoldLexBoundaryTokens(text, deps_.lexLang, toks);
    for (const RefoldLexBoundaryToken &tok : toks)
      out.push_back(ReplayTok{tok.spelling, tok.begin, tok.end});
  };

  auto tokenSpellingsForText = [&](StringRef text) {
    SmallVector<ReplayTok, 16> toks;
    lexReplayTokens(text, toks);
    SmallVector<std::string, 16> out;
    for (const ReplayTok &tok : toks)
      out.push_back(tok.spelling);
    return out;
  };

  auto textsTokenEquivalent = [&](StringRef lhs, StringRef rhs) {
    SmallVector<std::string, 16> lhsToks = tokenSpellingsForText(lhs);
    SmallVector<std::string, 16> rhsToks = tokenSpellingsForText(rhs);
    if (lhsToks.size() != rhsToks.size())
      return false;
    for (size_t i = 0; i < lhsToks.size(); ++i)
      if (lhsToks[i] != rhsToks[i])
        return false;
    return true;
  };

  auto rewriteTupleElementFromSolvedExpansion =
      [&](size_t elemIdx, StringRef oldText,
          StringRef newText) -> std::optional<std::string> {
    StringRef source = tupleElementText(elemIdx);
    oldText = oldText.trim();
    newText = newText.trim();

    // Prefer token-position replacement over whole-slot replacement.  This is
    // what preserves spelling trivia in stringification inversions: `#x`
    // observes normalized whitespace (`"alpha + beta"`), but the tuple slot
    // may be `alpha   +   beta`.  If the old solved token sequence appears
    // once in the source slot, replace just those token spellings and leave
    // the original inter-token trivia untouched.
    SmallVector<RefoldLexBoundaryToken, 16> sourceToks;
    SmallVector<RefoldLexBoundaryToken, 16> oldToks;
    SmallVector<RefoldLexBoundaryToken, 16> newToks;
    refoldLexBoundaryTokens(source, deps_.lexLang, sourceToks);
    refoldLexBoundaryTokens(oldText, deps_.lexLang, oldToks);
    refoldLexBoundaryTokens(newText, deps_.lexLang, newToks);
    if (!oldToks.empty() && oldToks.size() == newToks.size() &&
        sourceToks.size() >= oldToks.size()) {
      std::optional<size_t> matchBegin;
      bool ambiguous = false;
      for (size_t i = 0; i + oldToks.size() <= sourceToks.size(); ++i) {
        bool same = true;
        for (size_t j = 0; j < oldToks.size(); ++j) {
          if (sourceToks[i + j].spelling != oldToks[j].spelling) {
            same = false;
            break;
          }
        }
        if (!same)
          continue;
        if (matchBegin) {
          ambiguous = true;
          break;
        }
        matchBegin = i;
      }
      if (matchBegin && !ambiguous) {
        std::string rewritten = source.str();
        for (size_t j = oldToks.size(); j > 0; --j) {
          const size_t idx = *matchBegin + j - 1;
          rewritten = stringutils::replaceRange(
              rewritten, sourceToks[idx].begin, sourceToks[idx].end,
              newToks[j - 1].spelling);
        }
        return rewritten;
      }
    }

    if (source == oldText)
      return newText.str();
    if (auto loc = findUniqueTrimmedSubstring(source, oldText))
      return stringutils::replaceRange(source.str(), loc->first, loc->second,
                                       newText);
    return std::nullopt;
  };
  enum class CalleeReplayKind { Literal, Param, Stringify, Paste, VaOpt };
  struct CalleePastePiece {
    bool isParam = false;
    uint32_t paramIdx = 0;
    std::string literal;
  };
  struct CalleeReplayElem {
    CalleeReplayKind kind = CalleeReplayKind::Literal;
    std::string literal;
    uint32_t paramIdx = 0;
    std::vector<CalleeReplayElem> children;
    std::vector<CalleePastePiece> pastePieces;
  };

  auto pastePieceFromReplacementToken =
      [&](const RefoldModel::MacroReplacementToken &tok,
          CalleePastePiece &piece) -> bool {
    if (tok.kind == RefoldModel::MacroReplacementTokenKind::ParamRef) {
      if (!tok.paramIndex || *tok.paramIndex >= oldActuals.size())
        return false;
      piece.isParam = true;
      piece.paramIdx = *tok.paramIndex;
      return true;
    }
    if (tok.spelling == "#" || tok.spelling == "##" ||
        tok.spelling == "__VA_OPT__")
      return false;
    piece.isParam = false;
    piece.literal = tok.spelling.str();
    return true;
  };

  std::function<bool(size_t, size_t, std::vector<CalleeReplayElem> &)>
      parseCalleeReplayRange;
  parseCalleeReplayRange = [&](size_t begin, size_t end,
                               std::vector<CalleeReplayElem> &out) {
    for (size_t i = begin; i < end;) {
      const auto &tok = calleeDefinition->replacementTokens[i];
      if (tok.spelling == "#") {
        if (i + 1 >= end ||
            calleeDefinition->replacementTokens[i + 1].kind !=
                RefoldModel::MacroReplacementTokenKind::ParamRef ||
            !calleeDefinition->replacementTokens[i + 1].paramIndex)
          return false;
        const uint32_t paramIdx =
            *calleeDefinition->replacementTokens[i + 1].paramIndex;
        if (paramIdx >= oldActuals.size())
          return false;
        tupleReplayUsesStringification = true;
        CalleeReplayElem elem;
        elem.kind = CalleeReplayKind::Stringify;
        elem.paramIdx = paramIdx;
        out.push_back(std::move(elem));
        i += 2;
        continue;
      }

      if (i + 1 < end &&
          calleeDefinition->replacementTokens[i + 1].spelling == "##") {
        tupleReplayUsesPaste = true;
        CalleeReplayElem elem;
        elem.kind = CalleeReplayKind::Paste;
        CalleePastePiece first;
        if (!pastePieceFromReplacementToken(tok, first))
          return false;
        elem.pastePieces.push_back(std::move(first));
        i += 2;
        while (true) {
          if (i >= end)
            return false;
          CalleePastePiece next;
          if (!pastePieceFromReplacementToken(
                  calleeDefinition->replacementTokens[i], next))
            return false;
          elem.pastePieces.push_back(std::move(next));
          ++i;
          if (i >= end ||
              calleeDefinition->replacementTokens[i].spelling != "##")
            break;
          ++i;
        }
        out.push_back(std::move(elem));
        continue;
      }

      if (tok.kind == RefoldModel::MacroReplacementTokenKind::ParamRef) {
        if (!tok.paramIndex || *tok.paramIndex >= oldActuals.size())
          return false;
        CalleeReplayElem elem;
        elem.kind = CalleeReplayKind::Param;
        elem.paramIdx = *tok.paramIndex;
        out.push_back(std::move(elem));
        ++i;
        continue;
      }
      if (tok.spelling == "##")
        return false;
      if (tok.spelling == "__VA_OPT__")
        return false;
      CalleeReplayElem elem;
      elem.kind = CalleeReplayKind::Literal;
      elem.literal = tok.spelling.str();
      out.push_back(std::move(elem));
      ++i;
    }
    return true;
  };

  std::vector<CalleeReplayElem> calleePattern;
  if (!parseCalleeReplayRange(0, calleeDefinition->replacementTokens.size(),
                              calleePattern) ||
      calleePattern.empty())
    return std::nullopt;

  std::vector<CalleeReplayElem> replayPattern;
  replayPattern.reserve(forwarderPrefixLiterals.size() + calleePattern.size() +
                        forwarderSuffixLiterals.size());
  for (const std::string &literal : forwarderPrefixLiterals) {
    CalleeReplayElem elem;
    elem.kind = CalleeReplayKind::Literal;
    elem.literal = literal;
    replayPattern.push_back(std::move(elem));
  }
  replayPattern.insert(replayPattern.end(), calleePattern.begin(),
                       calleePattern.end());
  for (const std::string &literal : forwarderSuffixLiterals) {
    CalleeReplayElem elem;
    elem.kind = CalleeReplayKind::Literal;
    elem.literal = literal;
    replayPattern.push_back(std::move(elem));
  }

  using SolvedActuals = SmallVector<std::string, 8>;
  auto assignSolvedActual = [&](SolvedActuals &actuals, uint32_t paramIdx,
                                StringRef value) -> bool {
    if (paramIdx >= actuals.size())
      return false;
    if (!textsTokenEquivalent(actuals[paramIdx], oldActuals[paramIdx]) &&
        !textsTokenEquivalent(actuals[paramIdx], value))
      return false;
    if (textsTokenEquivalent(actuals[paramIdx], oldActuals[paramIdx])) {
      actuals[paramIdx] = value.trim().str();
      return true;
    }
    return textsTokenEquivalent(actuals[paramIdx], value);
  };

  auto solvePasteToken =
      [&](ArrayRef<CalleePastePiece> pieces, StringRef spelling,
          const SolvedActuals &seed) -> SmallVector<SolvedActuals, 4> {
    SmallVector<SolvedActuals, 4> solutions;

    const bool hasLiteralAnchor =
        llvm::any_of(pieces, [](const CalleePastePiece &piece) {
          return !piece.isParam && !piece.literal.empty();
        });

    // If a paste expression has no fixed literal anchor (`A ## B`), arbitrary
    // substring search would invent cuts for length-changing rewrites.  The
    // only deterministic inverse available in that shape is the original
    // contribution width recorded by the tuple slots.  Anchored paste forms
    // (`pre_ ## X ## _suf`, `A ## _ ## B`) use the generic DFS below instead.
    if (!hasLiteralAnchor) {
      SolvedActuals cur = seed;
      size_t cursor = 0;
      for (const CalleePastePiece &piece : pieces) {
        if (!piece.isParam) {
          if (!spelling.substr(cursor).starts_with(piece.literal))
            return solutions;
          cursor += piece.literal.size();
          continue;
        }
        if (piece.paramIdx >= oldActuals.size())
          return solutions;
        const size_t width =
            StringRef(oldActuals[piece.paramIdx]).trim().size();
        if (cursor + width > spelling.size())
          return solutions;
        if (!assignSolvedActual(cur, piece.paramIdx,
                                spelling.slice(cursor, cursor + width)))
          return solutions;
        cursor += width;
      }
      if (cursor == spelling.size())
        solutions.push_back(std::move(cur));
      return solutions;
    }

    std::function<void(size_t, size_t, SolvedActuals &)> dfsPaste;
    dfsPaste = [&](size_t pieceIdx, size_t cursor, SolvedActuals &cur) {
      if (solutions.size() > 1)
        return;
      if (pieceIdx == pieces.size()) {
        if (cursor == spelling.size())
          solutions.push_back(cur);
        return;
      }
      const CalleePastePiece &piece = pieces[pieceIdx];
      if (!piece.isParam) {
        if (spelling.substr(cursor).starts_with(piece.literal))
          dfsPaste(pieceIdx + 1, cursor + piece.literal.size(), cur);
        return;
      }

      for (size_t end = cursor; end <= spelling.size(); ++end) {
        StringRef slice = spelling.slice(cursor, end);
        SolvedActuals next = cur;
        if (!assignSolvedActual(next, piece.paramIdx, slice))
          continue;
        dfsPaste(pieceIdx + 1, end, next);
        if (solutions.size() > 1)
          return;
      }
    };
    SolvedActuals start = seed;
    dfsPaste(0, 0, start);
    return solutions;
  };

  auto solveExpansion =
      [&](StringRef expansion) -> std::optional<SolvedActuals> {
    SmallVector<ReplayTok, 32> toks;
    lexReplayTokens(expansion, toks);
    SmallVector<SolvedActuals, 4> solutions;
    SolvedActuals seed;
    seed.reserve(oldActuals.size());
    for (const std::string &actual : oldActuals)
      seed.push_back(actual);

    std::function<void(ArrayRef<CalleeReplayElem>, size_t, SolvedActuals &)>
        dfs;
    dfs = [&](ArrayRef<CalleeReplayElem> elems, size_t tokPos,
              SolvedActuals &cur) {
      if (solutions.size() > 1)
        return;
      if (elems.empty()) {
        if (tokPos == toks.size())
          solutions.push_back(cur);
        return;
      }
      const CalleeReplayElem &elem = elems.front();
      ArrayRef<CalleeReplayElem> rest = elems.drop_front();
      switch (elem.kind) {
      case CalleeReplayKind::Literal:
        if (tokPos < toks.size() && toks[tokPos].spelling == elem.literal)
          dfs(rest, tokPos + 1, cur);
        return;
      case CalleeReplayKind::Param: {
        for (size_t end = tokPos; end <= toks.size(); ++end) {
          StringRef value;
          if (end > tokPos) {
            const size_t byteBegin = toks[tokPos].begin;
            const size_t byteEnd = toks[end - 1].end;
            value = expansion.slice(byteBegin, byteEnd);
          }
          SolvedActuals next = cur;
          if (!assignSolvedActual(next, elem.paramIdx, value))
            continue;
          dfs(rest, end, next);
          if (solutions.size() > 1)
            return;
        }
        return;
      }
      case CalleeReplayKind::Stringify: {
        if (tokPos >= toks.size())
          return;
        std::optional<std::string> content =
            decodeSimpleStringLiteralToken(toks[tokPos].spelling);
        if (!content)
          return;
        SolvedActuals next = cur;
        if (!assignSolvedActual(next, elem.paramIdx, StringRef(*content)))
          return;
        dfs(rest, tokPos + 1, next);
        return;
      }
      case CalleeReplayKind::Paste: {
        if (tokPos >= toks.size())
          return;
        SmallVector<SolvedActuals, 4> pasteSolutions =
            solvePasteToken(elem.pastePieces, toks[tokPos].spelling, cur);
        for (SolvedActuals &pasteSol : pasteSolutions) {
          dfs(rest, tokPos + 1, pasteSol);
          if (solutions.size() > 1)
            return;
        }
        return;
      }
      case CalleeReplayKind::VaOpt:
        return;
      }
    };

    dfs(replayPattern, 0, seed);
    if (solutions.size() != 1)
      return std::nullopt;
    return solutions.front();
  };

  std::optional<SolvedActuals> oldSolved = solveExpansion(oldExpansion);
  if (!oldSolved || oldSolved->size() != oldActuals.size())
    return std::nullopt;

  // The replay value observed at the final callee is not always textually the
  // same as the tuple element that supplied it.  For example, `STR(ID(x))`
  // stringifies the prescanned value `x`, while the source slot we want to
  // preserve is still `ID(x)`.  Do not require old replay values to equal the
  // whole tuple slot here.  The tuple edit step below uses the solved old
  // value as the replacement key and accepts it only if that token sequence
  // is uniquely found inside the original tuple element; otherwise the proof
  // fails closed.

  std::optional<SolvedActuals> newSolved = solveExpansion(newExpansion);
  if (!newSolved || newSolved->size() != oldActuals.size())
    return std::nullopt;

  SmallVector<std::string, 8> newActuals;
  newActuals.reserve(newSolved->size());
  for (const std::string &actual : *newSolved)
    newActuals.push_back(StringRef(actual).trim().str());

  SmallVector<std::string, 8> newGeneratedPieces;
  for (size_t i = 0; i < fixedCalleeActuals; ++i)
    newGeneratedPieces.push_back(newActuals[i]);
  if (calleeHasVariadic) {
    StringRef tail = StringRef(newActuals.back()).trim();
    if (!tail.empty())
      newGeneratedPieces.push_back(tail.str());
  }

  struct TupleEdit {
    size_t begin = 0;
    size_t end = 0;
    std::string text;
  };
  SmallVector<TupleEdit, 8> edits;
  size_t pieceCursor = 0;
  for (const GeneratedArgRef &ref : generatedArgs) {
    if (ref.variadicPack) {
      if (ref.forwarderParamIdx >= tupleElems.size())
        return std::nullopt;
      std::string text;
      raw_string_ostream os(text);
      bool first = true;
      while (pieceCursor < newGeneratedPieces.size()) {
        if (!first)
          os << ", ";
        first = false;
        os << StringRef(newGeneratedPieces[pieceCursor]).trim();
        ++pieceCursor;
      }
      os.flush();
      const TupleElementSlice &firstElem = tupleElems[ref.forwarderParamIdx];
      const TupleElementSlice &lastElem = tupleElems.back();
      edits.push_back(
          TupleEdit{firstElem.trimBegin, lastElem.trimEnd, std::move(text)});
      continue;
    }
    if (pieceCursor >= newGeneratedPieces.size() ||
        ref.forwarderParamIdx >= tupleElems.size())
      return std::nullopt;
    const TupleElementSlice &elem = tupleElems[ref.forwarderParamIdx];
    StringRef oldText = pieceCursor < oldSolved->size()
                            ? StringRef((*oldSolved)[pieceCursor]).trim()
                            : (pieceCursor < oldActuals.size()
                                   ? StringRef(oldActuals[pieceCursor]).trim()
                                   : oldGeneratedPieces[pieceCursor].trim());
    StringRef newText = StringRef(newGeneratedPieces[pieceCursor]).trim();
    if (newText != tuplePayload.slice(elem.trimBegin, elem.trimEnd).trim()) {
      auto rewrittenElem = rewriteTupleElementFromSolvedExpansion(
          ref.forwarderParamIdx, oldText, newText);
      if (!rewrittenElem)
        return std::nullopt;
      edits.push_back(
          TupleEdit{elem.trimBegin, elem.trimEnd, std::move(*rewrittenElem)});
    }
    ++pieceCursor;
  }
  if (pieceCursor != newGeneratedPieces.size() || edits.empty())
    return std::nullopt;

  llvm::sort(edits, [](const TupleEdit &lhs, const TupleEdit &rhs) {
    if (lhs.begin != rhs.begin)
      return lhs.begin > rhs.begin;
    return lhs.end > rhs.end;
  });

  std::string rebuiltPayload = tuplePayload.str();
  size_t previousBegin = std::numeric_limits<size_t>::max();
  for (const TupleEdit &edit : edits) {
    if (edit.end < edit.begin || edit.end > rebuiltPayload.size())
      return std::nullopt;
    if (previousBegin != std::numeric_limits<size_t>::max() &&
        edit.end > previousBegin)
      return std::nullopt;
    previousBegin = edit.begin;
    rebuiltPayload = stringutils::replaceRange(rebuiltPayload, edit.begin,
                                               edit.end, edit.text);
  }

  std::string rewrittenArg =
      ("(" + StringRef(rebuiltPayload).trim().str() + ")");
  std::string rewrittenInv = stringutils::replaceRange(
      baseInvText.str(), argRange.first, argRange.second, rewrittenArg);
  if (StringRef(rewrittenInv).trim() == baseInvText.trim())
    return std::nullopt;

  MacroPatch patch{*m.invB, *m.invE, std::move(rewrittenInv), m.id};
  patch.materialized.outputByteStart = 0;
  patch.materialized.outputByteEnd = patch.replacement.size();
  patch.materialized.hasOutputByteRange = true;
  certifyMacroPatchMaterializedBTokenRange(patch,
                                           static_cast<uint64_t>(bEnv->first),
                                           static_cast<uint64_t>(bEnv->second));
  deps_.proofCertifier.SetArgsOnlyStandardProof(
      patch, m, /*wholeEnvelopeReplayValidated=*/true);
  deps_.proofCertifier.CertifyGeneratedCalleeReplayProof(
      patch, m, calleeDefinition ? calleeDefinition->id : 0,
      /*generatedCallDepth=*/1, tupleGeneratedCtx.objectAliasHopCount,
      tupleReplayUsesStringification, tupleReplayUsesPaste,
      tupleReplayUsesVariadicForwarding,
      /*decodedStringLiteralEvidenceOnly=*/tupleReplayUsesStringification);
  return patch;
}

} // namespace refold
} // namespace clang
