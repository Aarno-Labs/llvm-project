//===--- RefoldMacroSelectorSubstitutionPhase.cpp --------------*- C++ -*-===//
//
// Paste-derived callee-selector substitution phase.
//
// This translation unit owns the fallback candidate path that derives a callee
// selector rewrite from producer function-like macro definitions and validates
// the resulting candidate against the current whole-cover planning context.
// Local helper lambdas keep the definition lookup, token replay, depth, and gap
// collection rules close to the substitution algorithm they serve.
//
//===----------------------------------------------------------------------===//

#include "macro/RefoldMacroSelectorSubstitutionPhase.h"

#include "core/RefoldModel.h"
#include "line-control/RefoldLineObserverLayout.h"
#include "macro/RefoldMacroDAGSharedHelpers.h"
#include "macro/RefoldMacroPlannerHelpers.h"
#include "macro/RefoldMacroWholeCoverPlanningContext.h"
#include "proof/RefoldProofLattice.h"
#include "source/RefoldSourceMapper.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"

#include <algorithm>
#include <optional>
#include <utility>

using namespace llvm;

namespace clang {
namespace refold {

RefoldMacroSelectorSubstitutionPhase::RefoldMacroSelectorSubstitutionPhase(
    Dependencies deps)
    : deps_(std::move(deps)) {}

std::optional<MacroPatch> RefoldMacroSelectorSubstitutionPhase::Run(
    const RefoldMacroWholeCoverPlanningContext &planningCtx) const {
  const RefoldModel::MacroInvocation &m = planningCtx.m;
  const diffutils::Hunk &hEff = planningCtx.hEff;
  StringRef baseInvText = planningCtx.baseInvText;
  [[maybe_unused]] const uint64_t invStart = planningCtx.invStart;
  [[maybe_unused]] const uint64_t invEnd = planningCtx.invEnd;

  StringRef invSpanText =
      !baseInvText.empty()
          ? baseInvText
          : (m.invText ? StringRef(*m.invText) : StringRef(""));
  if (m.subkind != "func" ||
      !RefoldLineObserverLayout::InvocationSpanMatchesCallsitePrefix(
          invSpanText, m) ||
      !m.invB || !m.invE)
    return std::nullopt;

  auto rootArgRangesOpt =
      deps_.getMacroInvocationFormalArgContentRanges(m, invSpanText);
  if (!rootArgRangesOpt)
    return std::nullopt;
  const auto &rootArgRanges = *rootArgRangesOpt;

  using TokenSpellings = SmallVector<std::string, 16>;
  using ArgumentTokenSpellings = SmallVector<TokenSpellings, 4>;

  struct ProducerFunctionMacroDefinition {
    const RefoldModel::MacroDirective *directive = nullptr;
    StringRef name;
  };

  auto producerFunctionDefinitionFromDirective =
      [&](const RefoldModel::MacroDirective &directive)
      -> std::optional<ProducerFunctionMacroDefinition> {
    if (directive.subkind != "#define" || directive.name.empty())
      return std::nullopt;

    ProducerFunctionMacroDefinition definition;
    definition.directive = &directive;
    definition.name = directive.name;
    return definition;
  };

  // Resolve the macro state at a specific producer item id.  The selector
  // substitution may only reuse a definition that was active during the
  // original expansion.  It never edits a #define, revives an inactive
  // definition, or synthesizes a replacement-list model in the consumer.
  auto activeFunctionDefinitionBefore =
      [&](uint64_t beforeItemId,
          StringRef name) -> std::optional<ProducerFunctionMacroDefinition> {
    const RefoldModel::MacroDirective *active = nullptr;
    for (const auto &directive : deps_.model.GetMacroDirectives()) {
      if (directive.id >= beforeItemId)
        continue;
      if (directive.subkind != "#define" && directive.subkind != "#undef")
        continue;
      // MacroDirective::name is producer-owned macro-state proof data.
      // The selector proof must not rediscover #define/#undef names from
      // raw directive text; if the model name does not match, this
      // directive is not a state transition for the requested macro.
      if (directive.name != name)
        continue;
      if (!active || directive.id > active->id)
        active = &directive;
    }
    if (!active || active->subkind != "#define")
      return std::nullopt;
    return producerFunctionDefinitionFromDirective(*active);
  };

  // Replay a producer-recorded replacement-list tape. The producer has already
  // classified each replacement token as either fixed literal spelling or a
  // formal-parameter reference, so the consumer substitutes recovered
  // argument token spellings and compares the resulting token sequence
  // against A/B.  '#' and '##' are still rejected because this selector
  // proof does not model stringification or paste inside the alternate
  // callee body.
  auto replayProducerFunctionMacroTokens =
      [&](const ProducerFunctionMacroDefinition &definition,
          ArrayRef<TokenSpellings> actualArgs)
      -> std::optional<TokenSpellings> {
    const RefoldModel::MacroDirective &directive = *definition.directive;
    if (directive.defParams.size() != actualArgs.size())
      return std::nullopt;

    TokenSpellings replayed;
    for (const RefoldModel::MacroReplacementToken &token :
         directive.replacementTokens) {
      if (token.spelling == "#" || token.spelling == "##")
        return std::nullopt;
      switch (token.kind) {
      case RefoldModel::MacroReplacementTokenKind::Literal:
        replayed.push_back(token.spelling.str());
        break;
      case RefoldModel::MacroReplacementTokenKind::ParamRef:
        if (!token.paramIndex || *token.paramIndex >= actualArgs.size())
          return std::nullopt;
        replayed.append(actualArgs[*token.paramIndex].begin(),
                        actualArgs[*token.paramIndex].end());
        break;
      }
    }
    return replayed;
  };

  // Recover the selected callee's ordinary argument token sequence from the
  // A-side PP cover by subtracting producer-recorded body spans.  This
  // keeps the replay proof entirely token-based: every non-body occurrence
  // of the single formal must have the same A-token spelling sequence, or
  // the proof has no unique non-selector argument vector and rejects.
  auto coverMinusBodySingleArg = [&](const RefoldModel::MacroInvocation &leaf)
      -> std::optional<ArgumentTokenSpellings> {
    if (leaf.defParams.size() != 1 || !leaf.cover.IsValid() ||
        leaf.cover.begin >= leaf.cover.end ||
        leaf.cover.end > deps_.aToks.size())
      return std::nullopt;

    SmallVector<RefoldModel::PPSpan, 4> body;
    body.append(leaf.bodySpans.begin(), leaf.bodySpans.end());
    llvm::sort(body, ppSpanLessByTokenRange);

    auto collectGapTokens =
        [&](uint64_t beginTok,
            uint64_t endTok) -> std::optional<TokenSpellings> {
      if (endTok < beginTok || endTok > deps_.aToks.size())
        return std::nullopt;
      TokenSpellings out;
      for (uint64_t tok = beginTok; tok < endTok; ++tok)
        out.push_back(deps_.aToks[static_cast<size_t>(tok)].spelling);
      return out;
    };

    uint64_t cursor = leaf.cover.begin;
    std::optional<TokenSpellings> argTokens;
    for (const auto &sp : body) {
      if (sp.end <= leaf.cover.begin || sp.begin >= leaf.cover.end)
        continue;
      const uint64_t clippedBegin =
          std::max<uint64_t>(sp.begin, leaf.cover.begin);
      const uint64_t clippedEnd = std::min<uint64_t>(sp.end, leaf.cover.end);
      if (cursor < clippedBegin) {
        std::optional<TokenSpellings> gap =
            collectGapTokens(cursor, clippedBegin);
        if (!gap)
          return std::nullopt;
        if (!gap->empty()) {
          if (argTokens && !tokenSpellingVectorsEqual(*argTokens, *gap))
            return std::nullopt;
          argTokens = std::move(*gap);
        }
      }
      cursor = std::max<uint64_t>(cursor, clippedEnd);
    }
    if (cursor < leaf.cover.end) {
      std::optional<TokenSpellings> gap =
          collectGapTokens(cursor, leaf.cover.end);
      if (!gap)
        return std::nullopt;
      if (!gap->empty()) {
        if (argTokens && !tokenSpellingVectorsEqual(*argTokens, *gap))
          return std::nullopt;
        argTokens = std::move(*gap);
      }
    }
    if (!argTokens)
      return std::nullopt;

    ArgumentTokenSpellings out;
    out.push_back(std::move(*argTokens));
    return out;
  };

  // Build an invocation index so provenance walks can climb from the
  // descendant callee back to the root invocation deterministically by
  // producer-recorded caller ids.
  DenseMap<uint64_t, const RefoldModel::MacroInvocation *> invById;
  for (const auto &mi : deps_.model.GetMacroInvocations())
    invById[mi.id] = &mi;

  auto depthToRoot =
      [&](const RefoldModel::MacroInvocation &cand) -> std::optional<unsigned> {
    unsigned d = 0;
    std::optional<uint64_t> p = cand.callerMacroId;
    while (p) {
      ++d;
      if (*p == m.id)
        return d;
      auto it = invById.find(*p);
      if (it == invById.end())
        break;
      p = it->second->callerMacroId;
    }
    return std::nullopt;
  };

  // A selector witness identifies the exact root invocation byte slice that
  // supplied the paste-derived callee selector, plus the replacement
  // spelling that would select a candidate active macro definition.  Byte
  // offsets are relative to the root invocation text used for the emitted
  // MacroPatch.
  struct SelectorRewriteWitness {
    uint32_t rootArgIdx = 0;
    uint64_t selectorByteBegin = 0;
    uint64_t selectorByteEnd = 0;
    std::string oldSelector;
    std::string newSelector;
  };

  // Invert the producer-recorded callee-origin tape against a candidate
  // macro name.  Literal parts must match exactly.  Exactly one
  // caller_arg_slice part may supply the selector, and it must name this
  // root invocation and a concrete byte slice of one root argument.  If the
  // split against the next literal anchor is ambiguous, the proof rejects.
  auto deriveSelectorRewriteForCandidateName =
      [&](const RefoldModel::MacroCalleeOrigin &origin,
          StringRef candidateName) -> std::optional<SelectorRewriteWitness> {
    if (origin.kind != MacroCalleeOriginKind::Paste || !origin.spelling ||
        *origin.spelling == candidateName || origin.parts.empty() ||
        candidateName.empty())
      return std::nullopt;

    size_t candidatePos = 0;
    std::optional<SelectorRewriteWitness> selector;
    for (size_t i = 0; i < origin.parts.size(); ++i) {
      const RefoldModel::CalleeOriginPart &part = origin.parts[i];
      if (part.kind == RefoldModel::CalleeOriginPartKind::Literal) {
        if (!candidateName.substr(candidatePos).starts_with(part.spelling))
          return std::nullopt;
        candidatePos += part.spelling.size();
        continue;
      }

      if (part.kind != RefoldModel::CalleeOriginPartKind::CallerArgSlice ||
          !part.rootMacroId || *part.rootMacroId != m.id ||
          !part.rootParamIndex || !part.byteBegin || !part.byteEnd ||
          *part.rootParamIndex >= rootArgRanges.size())
        return std::nullopt;
      if (selector)
        return std::nullopt;

      const auto &argRange = rootArgRanges[*part.rootParamIndex];
      const uint64_t argLen = argRange.second - argRange.first;
      if (*part.byteEnd < *part.byteBegin || *part.byteEnd > argLen)
        return std::nullopt;

      StringRef rootSlice = invSpanText.slice(argRange.first + *part.byteBegin,
                                              argRange.first + *part.byteEnd);
      if (rootSlice != part.spelling)
        return std::nullopt;

      StringRef nextLiteral;
      for (size_t j = i + 1; j < origin.parts.size(); ++j) {
        if (origin.parts[j].kind ==
            RefoldModel::CalleeOriginPartKind::Literal) {
          nextLiteral = origin.parts[j].spelling;
          break;
        }
        if (origin.parts[j].kind ==
            RefoldModel::CalleeOriginPartKind::CallerArgSlice)
          return std::nullopt;
      }

      StringRef replacementPart;
      if (nextLiteral.empty()) {
        replacementPart = candidateName.drop_front(candidatePos);
        candidatePos = candidateName.size();
      } else {
        size_t found = candidateName.find(nextLiteral, candidatePos);
        if (found == StringRef::npos)
          return std::nullopt;
        if (candidateName.find(nextLiteral, found + 1) != StringRef::npos)
          return std::nullopt;
        replacementPart = candidateName.slice(candidatePos, found);
        candidatePos = found;
      }

      if (replacementPart.empty() || replacementPart == part.spelling)
        return std::nullopt;

      SelectorRewriteWitness out;
      out.rootArgIdx = *part.rootParamIndex;
      out.selectorByteBegin = argRange.first + *part.byteBegin;
      out.selectorByteEnd = argRange.first + *part.byteEnd;
      out.oldSelector = part.spelling.str();
      out.newSelector = replacementPart.str();
      selector = std::move(out);
    }

    if (candidatePos != candidateName.size() || !selector)
      return std::nullopt;
    return selector;
  };

  // Each candidate records one complete explanation of B: which descendant
  // callee was edited, which active alternate macro definition explains the
  // B-side cover, and what root invocation text would select that macro.
  struct SelectorCandidate {
    uint64_t leafId = 0;
    uint64_t candidateDirectiveId = 0;
    uint32_t rootArgIdx = 0;
    uint64_t selectorByteBegin = 0;
    uint64_t selectorByteEnd = 0;
    uint64_t bTokStart = 0;
    uint64_t bTokEnd = 0;
    uint64_t newSelectorSize = 0;
    std::string replacement;
  };
  SmallVector<SelectorCandidate, 4> candidates;

  // Search descendants of the current root for the selected callee whose
  // body-owned tokens contain the edited A hunk.  This prevents selector
  // substitution from firing on unrelated paste tokens in the same root
  // DAG.
  for (const auto &leaf : deps_.model.GetMacroInvocations()) {
    if (leaf.id == m.id || leaf.subkind != "func")
      continue;
    std::optional<unsigned> depth = depthToRoot(leaf);
    if (!depth || *depth == 0)
      continue;
    if (!leaf.cover.IsValid() ||
        !(leaf.cover.begin <= hEff.aStart && hEff.aEnd <= leaf.cover.end) ||
        !hunkWithinPPSpans(hEff, leaf.bodySpans))
      continue;

    // First prove that the original selected callee definition explains
    // the A-side cover under the recovered non-selector arguments.  Without
    // this baseline equality, replacing the selector would be relating B to
    // a model that did not actually produce A.
    std::optional<ProducerFunctionMacroDefinition> currentDef =
        activeFunctionDefinitionBefore(leaf.id, leaf.name);
    if (!currentDef ||
        (leaf.definitionDirectiveId &&
         currentDef->directive->id != *leaf.definitionDirectiveId))
      continue;
    std::optional<ArgumentTokenSpellings> actualArgs =
        coverMinusBodySingleArg(leaf);
    if (!actualArgs)
      continue;

    std::optional<TokenSpellings> currentExpansion =
        replayProducerFunctionMacroTokens(*currentDef, *actualArgs);
    if (!currentExpansion ||
        !deps_.tokenSpellingsEqualToA(*currentExpansion, leaf.cover.begin,
                                      leaf.cover.end))
      continue;

    // The alternate macro must explain exactly the B token envelope mapped
    // from the selected callee's original PP cover.  The selector proof
    // does not widen the edit or borrow neighboring B tokens.
    std::optional<std::pair<size_t, size_t>> bEnv =
        deps_.sourceMapper.MapATokRangeAToBTokenEnvelope(leaf.cover.begin,
                                                         leaf.cover.end);
    if (!bEnv || bEnv->second <= bEnv->first)
      continue;

    if (leaf.calleeOrigin.kind != MacroCalleeOriginKind::Paste ||
        !leaf.calleeOrigin.spelling ||
        *leaf.calleeOrigin.spelling != leaf.name ||
        leaf.calleeOrigin.parts.empty())
      continue;

    // Enumerate existing macro definitions as possible selector targets.
    // This is a finite namespace proof over definitions already present in
    // the source.  Each candidate must be active at the root expansion
    // point and must replay through producer-recorded replacement tokens.
    for (const auto &directive : deps_.model.GetMacroDirectives()) {
      std::optional<ProducerFunctionMacroDefinition> candidateDef =
          producerFunctionDefinitionFromDirective(directive);
      if (!candidateDef || candidateDef->name == leaf.name)
        continue;
      std::optional<ProducerFunctionMacroDefinition> activeCandidate =
          activeFunctionDefinitionBefore(m.id, candidateDef->name);
      if (!activeCandidate ||
          activeCandidate->directive->id != candidateDef->directive->id)
        continue;
      if (candidateDef->directive->defParams.size() !=
          currentDef->directive->defParams.size())
        continue;

      std::optional<SelectorRewriteWitness> selector =
          deriveSelectorRewriteForCandidateName(leaf.calleeOrigin,
                                                candidateDef->name);
      if (!selector)
        continue;

      std::optional<TokenSpellings> candidateExpansion =
          replayProducerFunctionMacroTokens(*candidateDef, *actualArgs);
      if (!candidateExpansion ||
          !deps_.tokenSpellingsEqualToB(*candidateExpansion,
                                        static_cast<uint64_t>(bEnv->first),
                                        static_cast<uint64_t>(bEnv->second)))
        continue;

      // Build the only source edit admitted by this proof: replace the
      // producer-proven selector slice inside the root invocation and leave
      // the rest of the callsite unchanged.  The normal root replay
      // validator still checks that the resulting invocation text is
      // well-formed.
      std::string replacement = invSpanText.str();
      replacement.replace(selector->selectorByteBegin,
                          selector->selectorByteEnd -
                              selector->selectorByteBegin,
                          selector->newSelector);
      if (!deps_.validateMergedDirectAndDagRootReplacement(m, invSpanText,
                                                           replacement))
        continue;

      candidates.push_back(SelectorCandidate{
          leaf.id, candidateDef->directive->id, selector->rootArgIdx,
          selector->selectorByteBegin, selector->selectorByteEnd,
          static_cast<uint64_t>(bEnv->first),
          static_cast<uint64_t>(bEnv->second),
          static_cast<uint64_t>(selector->newSelector.size()),
          std::move(replacement)});
    }
  }

  if (candidates.empty())
    return std::nullopt;

  // Sort before uniqueness checking so diagnostics and tie handling are
  // deterministic.  The proof accepts multiple witnesses only when they all
  // lead to the exact same root replacement text; distinct selector
  // rewrites are treated as ambiguous and rejected.
  llvm::sort(candidates,
             [](const SelectorCandidate &lhs, const SelectorCandidate &rhs) {
               if (lhs.replacement != rhs.replacement)
                 return lhs.replacement < rhs.replacement;
               if (lhs.leafId != rhs.leafId)
                 return lhs.leafId < rhs.leafId;
               if (lhs.candidateDirectiveId != rhs.candidateDirectiveId)
                 return lhs.candidateDirectiveId < rhs.candidateDirectiveId;
               if (lhs.rootArgIdx != rhs.rootArgIdx)
                 return lhs.rootArgIdx < rhs.rootArgIdx;
               if (lhs.selectorByteBegin != rhs.selectorByteBegin)
                 return lhs.selectorByteBegin < rhs.selectorByteBegin;
               if (lhs.selectorByteEnd != rhs.selectorByteEnd)
                 return lhs.selectorByteEnd < rhs.selectorByteEnd;
               if (lhs.bTokStart != rhs.bTokStart)
                 return lhs.bTokStart < rhs.bTokStart;
               return lhs.bTokEnd < rhs.bTokEnd;
             });

  const std::string &chosenReplacement = candidates.front().replacement;
  for (const SelectorCandidate &candidate : candidates)
    if (candidate.replacement != chosenReplacement) {
      return std::nullopt;
    }

  // Certify the accepted selector substitution as a structure-preserving root
  // macro patch.  The materialized B token range records the descendant
  // expansion that this selector explains, while the output byte range
  // points at the rewritten root argument inside the replacement callsite
  // text.
  MacroPatch patch{*m.invB, *m.invE, chosenReplacement, m.id};
  patch.materialized.hasBTokenRange = true;
  patch.materialized.bTokStart = candidates.front().bTokStart;
  patch.materialized.bTokEnd = candidates.front().bTokEnd;
  patch.materialized.hasOutputByteRange = true;
  patch.materialized.outputByteStart = candidates.front().selectorByteBegin;
  patch.materialized.outputByteEnd =
      candidates.front().selectorByteBegin + candidates.front().newSelectorSize;
  deps_.proofLattice.SetMacroPatchProof(
      patch, deps_.proofLattice.MakeMacroPatchProof(
                 MacroPatchProofKind::PasteDerivedCalleeSelector,
                 /*preservesInvocationStructure=*/true, m.id));
  return patch;
}

} // namespace refold
} // namespace clang
