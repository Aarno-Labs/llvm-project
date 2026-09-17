//===--- RefoldMacroStandardArgsOnlyPatchBuilder.cpp ------------*- C++ -*-===//
//
// Implementation of the standard args-only patch builder.
//
// The builder owns the args-only entry point and the paste-aware replay it
// tries before the standard args-only ranking slot.  That slot includes the
// pure paste-only fallback path that remains part of it after specialized
// paste-aware proofs decline.  Planner-side helpers still owned by
// `RefoldMacroPatchPlanner` are reached through the std::function callbacks
// supplied in `Dependencies`.  The public service boundary intentionally
// remains this one builder; local replay policy is split into private helpers
// only when the helper can state its proof obligation, mutation boundary,
// ordering constraint, and fail-closed behavior explicitly.
//
//===----------------------------------------------------------------------===//

#include "macro/RefoldMacroStandardArgsOnlyPatchBuilder.h"
#include "macro/RefoldMacroStandardArgsOnlyInternals.h"

#include "edit/RefoldBInsertionLedger.h"
#include "macro/RefoldArgTextRecovery.h"
#include "macro/RefoldMacroDefinitionTapeSolver.h"
#include "macro/RefoldMacroGeneratedCalleeReplayEngine.h"
#include "macro/RefoldMacroGeneratedLeafReplayEngine.h"
#include "macro/RefoldMacroPatchProofCertifier.h"
#include "macro/RefoldMacroPlannerHelpers.h"
#include "macro/RefoldMacroReplay.h"
#include "macro/RefoldMacroTopology.h"
#include "macro/RefoldMacroTupleHelpers.h"
#include "model/RefoldModel.h"
#include "model/RefoldToken.h"
#include "proof/RefoldMacroPatchProofClassifier.h"
#include "source/RefoldSourceMapper.h"
#include "source/TokenTextHelpers.h"
#include "support/RefoldLog.h"
#include "support/StringUtils.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringRef.h"

#include <algorithm>
#include <cassert>
#include <optional>
#include <string>
#include <utility>
#include <vector>

using namespace llvm;

namespace clang {
namespace refold {

RefoldMacroStandardArgsOnlyPatchBuilder::
    RefoldMacroStandardArgsOnlyPatchBuilder(Dependencies deps)
    : deps_(std::move(deps)) {}

RefoldMacroStandardArgsOnlyPatchBuilder::
    ~RefoldMacroStandardArgsOnlyPatchBuilder() = default;

RefoldMacroStandardArgsOnlyPatchBuilder::EnvelopeCacheScope::EnvelopeCacheScope(
    const RefoldMacroStandardArgsOnlyPatchBuilder &builder)
    : builder_(builder) {
  assert(!builder_.envelopeCache_ && "envelope cache scopes do not nest");
  builder_.envelopeCache_ = std::make_unique<SyntheticEnvelopeCache>();
}

RefoldMacroStandardArgsOnlyPatchBuilder::EnvelopeCacheScope::
    ~EnvelopeCacheScope() {
  builder_.envelopeCache_.reset();
}

RefoldMacroArgsOnlyTemplateSolver
RefoldMacroStandardArgsOnlyPatchBuilder::TemplateSolver() const {
  return RefoldMacroArgsOnlyTemplateSolver(
      {&deps_.model, deps_.aToks, deps_.bToks, deps_.bTokOff,
       &deps_.sourceMapper, &deps_.macroTopology,
       &deps_.macroPatchProofClassifier, &deps_.lexLang});
}

RefoldMacroOccurrenceReplay
RefoldMacroStandardArgsOnlyPatchBuilder::OccurrenceReplay() const {
  return RefoldMacroOccurrenceReplay({deps_.aToks, deps_.bTokOff,
                                      &deps_.macroTopology, &deps_.sourceMapper,
                                      deps_.strict, &deps_.lexLang});
}

RefoldMacroPasteArgumentBuilder
RefoldMacroStandardArgsOnlyPatchBuilder::PasteArgumentBuilder() const {
  return RefoldMacroPasteArgumentBuilder(
      {&deps_.sourceMapper, deps_.aToks, deps_.bToks, &deps_.lexLang});
}

RefoldMacroDefinitionTapeSolver
RefoldMacroStandardArgsOnlyPatchBuilder::DefinitionTapeSolver() const {
  return RefoldMacroDefinitionTapeSolver(
      {&deps_.model, deps_.aToks, deps_.bToks, &deps_.sourceMapper,
       &deps_.macroPatchProofClassifier, &deps_.witnessTrace, &deps_.lexLang});
}

namespace {

/// Result of replaying a producer definition tape to repair standard argument
/// span formal indices.
struct DefinitionReplayedStandardArgSpanRepair {
  /// Standard argument spans to use for the standard args-only ranking slot.
  std::vector<RefoldModel::PPArgSpan> argSpans;

  /// Whether the definition-tape proof changed any recorded formal index.
  bool replayedFormalIndices = false;
};

/// Return the unmodified recorded span set for definition replay fallback.
///
/// The definition-tape repair helper calls this on every unproved branch so the
/// old producer metadata remains the only source of truth unless replay proves a
/// complete, non-stringify/non-paste formal-index repair.
DefinitionReplayedStandardArgSpanRepair
makeOriginalDefinitionReplayedStandardArgSpanRepair(
    const RefoldModel::MacroInvocation &m) {
  DefinitionReplayedStandardArgSpanRepair result;
  result.argSpans = m.argSpans;
  return result;
}

/// Return a trimmed invocation argument slice from an already parsed invocation
/// layout.  The byte ranges are relative to `baseInvocationText`.
std::optional<StringRef> sliceInvocationArgumentText(
    StringRef baseInvocationText,
    ArrayRef<std::pair<size_t, size_t>> invocationArgRanges,
    uint32_t argIdx) {
  if (argIdx >= invocationArgRanges.size())
    return std::nullopt;
  const auto range = invocationArgRanges[argIdx];
  if (range.second < range.first || range.second > baseInvocationText.size())
    return std::nullopt;
  return baseInvocationText.slice(range.first, range.second).trim();
}


/// Direct conditional-token-paste shape proved from a definition replacement
/// list.  This covers the restricted `base ## __VA_OPT__(__VA_ARGS__)` family
/// where the paste result is one PP token and the changed suffix/prefix belongs
/// to the variadic formal.
struct DirectVaOptPasteTokenShape {
  /// Formal whose source spelling is the stable non-variadic side of the paste.
  uint32_t fixedArgIdx = 0;
  /// Variadic formal contributed only when the `__VA_OPT__` branch is active.
  uint32_t variadicArgIdx = 0;
  /// True for `fixed ## __VA_OPT__(variadic)`, false for the reverse order.
  bool fixedBeforeVariadic = true;
};

/// Return true when `text` trims to exactly one raw lexer token with the same
/// spelling.  The conditional-paste bridge uses this to avoid inventing source
/// spellings for multi-token actuals or relying on maximal-munch-adjacent text
/// that would not be a stable macro actual segment.
bool textIsSingleTokenSpelling(StringRef text, const LangOptions &lexLang) {
  text = text.trim();
  if (text.empty())
    return false;

  SmallVector<RefoldLexBoundaryToken, 4> tokens;
  refoldLexBoundaryTokens(text, lexLang, tokens);
  return tokens.size() == 1 && tokens.front().begin == 0 &&
         tokens.front().end == text.size() && tokens.front().spelling == text;
}

/// Return whether an invocation delimiter slice contains exactly one comma and
/// otherwise only whitespace.
///
/// Variadic deactivation rewrites remove the final variadic actual rather than
/// replacing its content with an empty string.  This helper gives that deletion
/// a small fail-closed boundary: the bytes between the previous argument and the
/// variadic argument must be only the call-site delimiter we are allowed to
/// remove.
bool invocationDelimiterIsSingleComma(StringRef delimiterText) {
  bool sawComma = false;
  for (char c : delimiterText) {
    if (c == ',') {
      if (sawComma)
        return false;
      sawComma = true;
      continue;
    }
    if (c != ' ' && c != '\t' && c != '\n' && c != '\r' && c != '\f' &&
        c != '\v')
      return false;
  }
  return sawComma;
}

/// Rebuild an invocation after deleting its final variadic actual and the
/// delimiter that introduced it.
///
/// This is intentionally not folded into `BuildInvocationRewriteWithRange`:
/// that service replaces formal-content ranges and must not learn how to delete
/// call-site delimiters.  Conditional `__VA_OPT__` paste deactivation is the one
/// args-only proof here that needs delimiter deletion, because the source form
/// changes from `MAKE(foo, bar)` to `MAKE(foo)` when the variadic pack becomes
/// empty.
std::optional<InvocationRewriteWithRange> buildInvocationRewriteDroppingFinalArg(
    StringRef baseInvocationText,
    ArrayRef<std::pair<size_t, size_t>> invocationArgRanges,
    uint32_t argIdx, uint32_t materializedArgIdx) {
  if (argIdx == 0 || argIdx + 1 != invocationArgRanges.size() ||
      materializedArgIdx >= invocationArgRanges.size())
    return std::nullopt;

  const auto previousRange = invocationArgRanges[argIdx - 1];
  const auto droppedRange = invocationArgRanges[argIdx];
  const auto materializedRange = invocationArgRanges[materializedArgIdx];
  if (previousRange.second < previousRange.first ||
      droppedRange.second < droppedRange.first ||
      materializedRange.second < materializedRange.first ||
      previousRange.second > baseInvocationText.size() ||
      droppedRange.second > baseInvocationText.size() ||
      materializedRange.second > baseInvocationText.size() ||
      previousRange.second > droppedRange.first ||
      materializedRange.first >= droppedRange.second)
    return std::nullopt;

  StringRef delimiterText =
      baseInvocationText.slice(previousRange.second, droppedRange.first);
  if (!invocationDelimiterIsSingleComma(delimiterText))
    return std::nullopt;

  InvocationRewriteWithRange out;
  out.text = baseInvocationText.slice(0, previousRange.second).str();
  out.text += baseInvocationText.substr(droppedRange.second);
  out.materializedOutputByteStart = materializedRange.first;
  out.materializedOutputByteEnd = materializedRange.second;
  return out;
}

/// Recognize the direct conditional paste shape that the producer currently
/// records with `paste_tokens` but without `paste_spans`.
///
/// The accepted grammar is intentionally tiny:
///
///   fixed ## __VA_OPT__(variadic)
///   __VA_OPT__(variadic) ## fixed
///
/// where `fixed` is a non-variadic parameter reference and `variadic` is the
/// invocation's variadic formal.  General token-paste replay remains owned by
/// the existing paste-span path; this helper only fills the missing proof edge
/// for a conditional paste whose edited token can be derived uniquely from the
/// call-site actual text.
std::optional<DirectVaOptPasteTokenShape>
matchDirectVaOptPasteTokenShape(const RefoldModel::MacroDirective &definition) {
  if (!definition.IsFunctionLikeDefine() ||
      definition.replacementTokens.size() != 6)
    return std::nullopt;

  auto paramIndexAt = [&](size_t index) -> std::optional<uint32_t> {
    const RefoldModel::MacroReplacementToken &token =
        definition.replacementTokens[index];
    if (token.kind != RefoldModel::MacroReplacementTokenKind::ParamRef ||
        !token.paramIndex || *token.paramIndex >= definition.defParams.size())
      return std::nullopt;
    return *token.paramIndex;
  };

  auto literalAt = [&](size_t index, StringRef spelling) -> bool {
    const RefoldModel::MacroReplacementToken &token =
        definition.replacementTokens[index];
    return token.kind == RefoldModel::MacroReplacementTokenKind::Literal &&
           token.spelling == spelling;
  };

  auto buildShape = [&](uint32_t fixedArgIdx, uint32_t variadicArgIdx,
                        bool fixedBeforeVariadic)
      -> std::optional<DirectVaOptPasteTokenShape> {
    if (fixedArgIdx == variadicArgIdx ||
        fixedArgIdx >= definition.defParams.size() ||
        variadicArgIdx >= definition.defParams.size() ||
        definition.defParams[fixedArgIdx].variadic ||
        !definition.defParams[variadicArgIdx].variadic)
      return std::nullopt;

    DirectVaOptPasteTokenShape shape;
    shape.fixedArgIdx = fixedArgIdx;
    shape.variadicArgIdx = variadicArgIdx;
    shape.fixedBeforeVariadic = fixedBeforeVariadic;
    return shape;
  };

  if (auto fixedArgIdx = paramIndexAt(0)) {
    if (literalAt(1, "##") && literalAt(2, "__VA_OPT__") &&
        literalAt(3, "(") && literalAt(5, ")")) {
      if (auto variadicArgIdx = paramIndexAt(4))
        return buildShape(*fixedArgIdx, *variadicArgIdx,
                          /*fixedBeforeVariadic=*/true);
    }
  }

  if (literalAt(0, "__VA_OPT__") && literalAt(1, "(") &&
      literalAt(3, ")") && literalAt(4, "##")) {
    if (auto variadicArgIdx = paramIndexAt(2)) {
      if (auto fixedArgIdx = paramIndexAt(5))
        return buildShape(*fixedArgIdx, *variadicArgIdx,
                          /*fixedBeforeVariadic=*/false);
    }
  }

  return std::nullopt;
}

/// Build an invocation-preserving patch for the direct conditional paste case
/// where the producer has a paste-token witness but no paste-span interval.
///
/// Proof obligation: the old invocation actuals must concatenate to the single
/// A-side pasted token, and the edited B token must differ only in the
/// variadic contribution while preserving the fixed contribution.  A non-empty
/// new variadic contribution must itself be exactly one lexer token so the
/// rewrite does not invent a multi-token paste payload.  An empty new
/// contribution is admitted only as `__VA_OPT__` deactivation and is emitted by
/// deleting the final variadic argument plus its comma delimiter.
std::optional<MacroPatch> tryBuildDirectVaOptPasteTokenPatch(
    const RefoldMacroStandardArgsOnlyPatchBuilder::Dependencies &deps,
    const RefoldModel::MacroInvocation &invocation, const diffutils::Hunk &hunk,
    StringRef baseInvocationText,
    ArrayRef<std::pair<size_t, size_t>> invocationArgRanges,
    const InvocationActualRecoveryContext &actualCtx) {
  if (!invocation.pasteSpans.empty() || invocation.pasteTokens.empty() ||
      !invocation.argSpans.empty() || !invocation.stringifySpans.empty())
    return std::nullopt;

  const RefoldModel::MacroDirective *definition =
      getDefinitionDirectiveForInvocation(deps.model, invocation);
  if (!definition || definition->name != invocation.name ||
      definition->defParams.size() != invocation.defParams.size())
    return std::nullopt;

  std::optional<DirectVaOptPasteTokenShape> shape =
      matchDirectVaOptPasteTokenShape(*definition);
  if (!shape)
    return std::nullopt;

  std::optional<std::pair<uint64_t, uint64_t>> cover =
      RefoldMacroWholeCoverProof::GetWholeCoverATokRange(invocation);
  if (!cover || cover->second != cover->first + 1 ||
      hunk.aStart != cover->first || hunk.aEnd != cover->second ||
      hunk.bEnd != hunk.bStart + 1 || hunk.aStart >= deps.aToks.size() ||
      hunk.bStart >= deps.bToks.size())
    return std::nullopt;

  std::optional<StringRef> fixedActual = sliceInvocationArgumentText(
      baseInvocationText, invocationArgRanges, shape->fixedArgIdx);
  std::optional<StringRef> oldVariadicActual = sliceInvocationArgumentText(
      baseInvocationText, invocationArgRanges, shape->variadicArgIdx);
  if (!fixedActual || !oldVariadicActual || fixedActual->empty() ||
      oldVariadicActual->empty() ||
      !textIsSingleTokenSpelling(*fixedActual, deps.lexLang) ||
      !textIsSingleTokenSpelling(*oldVariadicActual, deps.lexLang))
    return std::nullopt;

  StringRef aToken = deps.aToks[static_cast<size_t>(hunk.aStart)].spelling;
  StringRef bToken = deps.bToks[static_cast<size_t>(hunk.bStart)].spelling;
  std::string expectedA;
  if (shape->fixedBeforeVariadic) {
    expectedA = fixedActual->str();
    expectedA += oldVariadicActual->str();
  } else {
    expectedA = oldVariadicActual->str();
    expectedA += fixedActual->str();
  }
  if (aToken != expectedA)
    return std::nullopt;

  std::string newVariadicActual;
  if (shape->fixedBeforeVariadic) {
    if (!bToken.starts_with(*fixedActual))
      return std::nullopt;
    newVariadicActual = bToken.drop_front(fixedActual->size()).str();
  } else {
    if (!bToken.ends_with(*fixedActual))
      return std::nullopt;
    newVariadicActual = bToken.drop_back(fixedActual->size()).str();
  }

  const bool deactivatesVaOpt = newVariadicActual.empty();
  if ((!deactivatesVaOpt &&
       (!textIsSingleTokenSpelling(newVariadicActual, deps.lexLang) ||
        replacementIntroducesTopLevelComma(newVariadicActual, deps.lexLang))) ||
      StringRef(newVariadicActual) == *oldVariadicActual)
    return std::nullopt;

  if (!invocation.invB || !invocation.invE)
    return std::nullopt;

  std::optional<InvocationRewriteWithRange> rewrite;
  if (deactivatesVaOpt) {
    rewrite = buildInvocationRewriteDroppingFinalArg(
        baseInvocationText, invocationArgRanges, shape->variadicArgIdx,
        shape->fixedArgIdx);
  } else {
    DenseMap<uint32_t, std::string> replacementByArgIdx;
    replacementByArgIdx[shape->variadicArgIdx] = std::move(newVariadicActual);
    rewrite = deps.buildInvocationRewriteWithRange(
        actualCtx, replacementByArgIdx,
        /*materializedRangeByArgIdx=*/nullptr);
  }
  if (!rewrite || StringRef(rewrite->text).trim() == baseInvocationText.trim())
    return std::nullopt;

  MacroPatch patch{*invocation.invB, *invocation.invE, std::move(rewrite->text),
                   invocation.id};
  deps.proofCertifier.CertifyInvocationRewriteMaterializedOutputRange(
      patch, rewrite->materializedOutputByteStart,
      rewrite->materializedOutputByteEnd);
  deps.certifyMacroPatchWholeExpansionBRange(invocation, patch);

  MacroPatchProof proof =
      makeMacroPatchProof(MacroPatchProofKind::ArgsOnlyStandard,
                          /*preservesInvocationStructure=*/true, invocation.id);
  WholeEnvelopeReplayWitness wholeEnvelopeWitness;
  wholeEnvelopeWitness.rootMacroId = invocation.id;
  wholeEnvelopeWitness.replayValidated = true;
  wholeEnvelopeWitness.definitionTapeReplayValidated = true;
  proof.wholeEnvelopeReplay = wholeEnvelopeWitness;

  // The producer did not provide a paste-span interval for the VA_OPT paste
  // token, so this path carries its own replay proof rather than one the
  // producer's paste spans require.
  PasteWitness pasteWitness;
  pasteWitness.rootMacroId = invocation.id;
  pasteWitness.requiresProducerPasteSpans = false;
  pasteWitness.replayValidated = true;
  proof.paste = pasteWitness;
  deps.macroPatchProofClassifier.SetMacroPatchProof(patch, std::move(proof));
  return patch;
}


/// Direct `__VA_OPT__` stringify deactivation shape proved from a definition
/// replacement list.  This covers the restricted
/// `fixed __VA_OPT__(, #variadic)` family where the edited stream deletes the
/// comma and stringified variadic payload, leaving only the fixed formal.
struct DirectVaOptStringifyDeactivationShape {
  /// Formal that remains visible after `__VA_OPT__` deactivation.
  uint32_t fixedArgIdx = 0;
  /// Variadic formal stringified inside the active `__VA_OPT__` payload.
  uint32_t variadicArgIdx = 0;
};

/// Recognize the direct conditional stringify shape:
///
///   fixed __VA_OPT__(, #variadic)
///
/// This intentionally does not try to solve arbitrary `__VA_OPT__` payloads.
/// The caller still has to prove the observed A-side token layout and the
/// exact source-level deletion of the final variadic actual.
std::optional<DirectVaOptStringifyDeactivationShape>
matchDirectVaOptStringifyDeactivationShape(
    const RefoldModel::MacroDirective &definition) {
  if (!definition.IsFunctionLikeDefine() ||
      definition.replacementTokens.size() != 7)
    return std::nullopt;

  auto literalAt = [&](size_t idx, StringRef spelling) {
    return idx < definition.replacementTokens.size() &&
           definition.replacementTokens[idx].kind ==
               RefoldModel::MacroReplacementTokenKind::Literal &&
           definition.replacementTokens[idx].spelling == spelling;
  };
  auto paramIndexAt = [&](size_t idx) -> std::optional<uint32_t> {
    if (idx >= definition.replacementTokens.size())
      return std::nullopt;
    const RefoldModel::MacroReplacementToken &token =
        definition.replacementTokens[idx];
    if (token.kind != RefoldModel::MacroReplacementTokenKind::ParamRef ||
        !token.paramIndex || *token.paramIndex >= definition.defParams.size())
      return std::nullopt;
    return *token.paramIndex;
  };

  std::optional<uint32_t> fixedArgIdx = paramIndexAt(0);
  std::optional<uint32_t> variadicArgIdx = paramIndexAt(5);
  if (!fixedArgIdx || !variadicArgIdx || *fixedArgIdx == *variadicArgIdx ||
      !literalAt(1, "__VA_OPT__") || !literalAt(2, "(") ||
      !literalAt(3, ",") || !literalAt(4, "#") || !literalAt(6, ")"))
    return std::nullopt;

  if (!definition.defParams[*variadicArgIdx].variadic)
    return std::nullopt;

  DirectVaOptStringifyDeactivationShape shape;
  shape.fixedArgIdx = *fixedArgIdx;
  shape.variadicArgIdx = *variadicArgIdx;
  return shape;
}

/// Return the unique producer span for `argIdx` when it exists.
///
/// Direct VA_OPT-stringify deactivation is intentionally limited to one fixed
/// occurrence and one stringified variadic occurrence.  Multiple occurrences
/// would require cross-occurrence agreement and should remain on the ordinary
/// args-only/stringify path instead of this delimiter-deletion bridge.
std::optional<RefoldModel::PPArgSpan> findUniqueArgSpanByKind(
    ArrayRef<RefoldModel::PPArgSpan> spans, uint32_t argIdx,
    PPArgSpanKind kind) {
  std::optional<RefoldModel::PPArgSpan> result;
  for (const RefoldModel::PPArgSpan &span : spans) {
    if (span.argIdx != argIdx || span.kind != kind)
      continue;
    if (result)
      return std::nullopt;
    result = span;
  }
  return result;
}

/// Build an invocation-preserving patch for deactivating
/// `fixed __VA_OPT__(, #variadic)`.
///
/// Ordinary stringify replay owns only the string literal span.  In this shape,
/// however, the source edit deletes both that span and the fixed comma inside
/// the `__VA_OPT__` payload.  Since the comma is replacement-list syntax, the
/// normal occurrence collector correctly refuses the hunk.  This bridge accepts
/// only the direct deactivation theorem: the fixed formal remains unchanged,
/// the stringified variadic span is deleted with its immediately preceding
/// comma token, and the source rewrite removes the final variadic actual plus
/// its call-site comma delimiter.
std::optional<MacroPatch> tryBuildDirectVaOptStringifyDeactivationPatch(
    const RefoldMacroStandardArgsOnlyPatchBuilder::Dependencies &deps,
    const RefoldModel::MacroInvocation &invocation, const diffutils::Hunk &hunk,
    StringRef baseInvocationText,
    ArrayRef<std::pair<size_t, size_t>> invocationArgRanges) {
  if (!invocation.pasteSpans.empty() || !invocation.pasteTokens.empty() ||
      invocation.stringifySpans.empty() || invocation.argSpans.empty())
    return std::nullopt;

  const RefoldModel::MacroDirective *definition =
      getDefinitionDirectiveForInvocation(deps.model, invocation);
  if (!definition || definition->name != invocation.name ||
      definition->defParams.size() != invocation.defParams.size())
    return std::nullopt;

  std::optional<DirectVaOptStringifyDeactivationShape> shape =
      matchDirectVaOptStringifyDeactivationShape(*definition);
  if (!shape)
    return std::nullopt;

  if (shape->fixedArgIdx >= invocationArgRanges.size() ||
      shape->variadicArgIdx >= invocationArgRanges.size())
    return std::nullopt;

  std::optional<RefoldModel::PPArgSpan> fixedSpan = findUniqueArgSpanByKind(
      invocation.argSpans, shape->fixedArgIdx, PPArgSpanKind::Standard);
  std::optional<RefoldModel::PPArgSpan> stringifySpan = findUniqueArgSpanByKind(
      invocation.stringifySpans, shape->variadicArgIdx,
      PPArgSpanKind::Stringify);
  if (!fixedSpan || !stringifySpan || fixedSpan->begin >= fixedSpan->end ||
      stringifySpan->begin + 1 != stringifySpan->end ||
      fixedSpan->end > stringifySpan->begin ||
      stringifySpan->end > deps.aToks.size())
    return std::nullopt;

  std::optional<std::pair<uint64_t, uint64_t>> cover =
      RefoldMacroWholeCoverProof::GetWholeCoverATokRange(invocation);
  if (!cover || cover->first != fixedSpan->begin ||
      cover->second != stringifySpan->end || hunk.aStart != fixedSpan->end ||
      hunk.aEnd != stringifySpan->end || hunk.bStart != hunk.bEnd)
    return std::nullopt;

  // The token immediately before the stringified variadic payload must be the
  // comma literal inside the VA_OPT payload.  The definition shape proves that
  // the comma exists; this token-level check proves the observed A expansion
  // surface matches that shape exactly.
  if (stringifySpan->begin == 0 ||
      deps.aToks[static_cast<size_t>(stringifySpan->begin - 1)].spelling != ",")
    return std::nullopt;

  std::optional<StringRef> oldVariadicActual = sliceInvocationArgumentText(
      baseInvocationText, invocationArgRanges, shape->variadicArgIdx);
  if (!oldVariadicActual || oldVariadicActual->empty())
    return std::nullopt;

  StringRef oldStringifiedToken =
      deps.aToks[static_cast<size_t>(stringifySpan->begin)].spelling;
  std::optional<std::string> unstringifiedOld =
      deps.argTextRecovery.UnstringifyLiteralToArgText(oldStringifiedToken,
                                                       /*allowTopLevelComma=*/true);
  if (!unstringifiedOld)
    return std::nullopt;

  std::optional<std::string> canonicalOld =
      stringutils::canonicalizeStringifyInversePayload(*unstringifiedOld);
  if (!canonicalOld || StringRef(*canonicalOld).trim() !=
                           StringRef(*unstringifiedOld).trim() ||
      StringRef(*canonicalOld).trim() != oldVariadicActual->trim())
    return std::nullopt;

  if (!invocation.invB || !invocation.invE)
    return std::nullopt;

  std::optional<InvocationRewriteWithRange> rewrite =
      buildInvocationRewriteDroppingFinalArg(
          baseInvocationText, invocationArgRanges, shape->variadicArgIdx,
          shape->fixedArgIdx);
  if (!rewrite || StringRef(rewrite->text).trim() == baseInvocationText.trim())
    return std::nullopt;

  MacroPatch patch{*invocation.invB, *invocation.invE, std::move(rewrite->text),
                   invocation.id};
  deps.proofCertifier.CertifyInvocationRewriteMaterializedOutputRange(
      patch, rewrite->materializedOutputByteStart,
      rewrite->materializedOutputByteEnd);
  deps.certifyMacroPatchWholeExpansionBRange(invocation, patch);

  MacroPatchProof proof =
      makeMacroPatchProof(MacroPatchProofKind::ArgsOnlyStandard,
                          /*preservesInvocationStructure=*/true, invocation.id);
  WholeEnvelopeReplayWitness wholeEnvelopeWitness;
  wholeEnvelopeWitness.rootMacroId = invocation.id;
  wholeEnvelopeWitness.replayValidated = true;
  wholeEnvelopeWitness.definitionTapeReplayValidated = true;
  proof.wholeEnvelopeReplay = wholeEnvelopeWitness;
  deps.macroPatchProofClassifier.SetMacroPatchProof(patch, std::move(proof));
  deps.macroPatchProofClassifier.SyncMacroPatchProofSummary(patch);
  return patch;
}


/// Restricted generated-callee shape for activating a stringified `__VA_OPT__`
/// payload through a variadic forwarding root.
struct GeneratedVaOptStringifyPayloadActivationShape {
  uint32_t rootCalleeArgIdx = 0;
  uint32_t rootVariadicArgIdx = 0;
  uint32_t fixedFinalArgIdx = 0;
  uint32_t variadicFinalArgIdx = 0;
};

/// Recognize `callee(__VA_ARGS__)` at the root and
/// `fixed __VA_OPT__(# variadic)` at the reached callee.
///
/// The bridge intentionally accepts only the canonical two-level shape needed
/// to distinguish a stable fixed generated actual from an activated
/// stringified variadic payload.  More complex generated-call grammars remain
/// owned by the general generated-callee replay engine.
std::optional<GeneratedVaOptStringifyPayloadActivationShape>
matchGeneratedVaOptStringifyPayloadActivationShape(
    const RefoldModel::MacroDirective &rootDefinition,
    const RefoldModel::MacroDirective &calleeDefinition) {
  if (!rootDefinition.IsFunctionLikeDefine() ||
      !calleeDefinition.IsFunctionLikeDefine() ||
      rootDefinition.replacementTokens.size() != 4 ||
      calleeDefinition.replacementTokens.size() != 6 ||
      rootDefinition.defParams.size() < 2 ||
      calleeDefinition.defParams.size() != 2 ||
      !rootDefinition.defParams.back().variadic ||
      !calleeDefinition.defParams.back().variadic)
    return std::nullopt;

  auto rootParamAt = [&](size_t idx) -> std::optional<uint32_t> {
    const RefoldModel::MacroReplacementToken &tok =
        rootDefinition.replacementTokens[idx];
    if (tok.kind != RefoldModel::MacroReplacementTokenKind::ParamRef ||
        !tok.paramIndex || *tok.paramIndex >= rootDefinition.defParams.size())
      return std::nullopt;
    return *tok.paramIndex;
  };
  auto rootLiteralAt = [&](size_t idx, StringRef spelling) {
    const RefoldModel::MacroReplacementToken &tok =
        rootDefinition.replacementTokens[idx];
    return tok.kind == RefoldModel::MacroReplacementTokenKind::Literal &&
           tok.spelling == spelling;
  };

  std::optional<uint32_t> rootCalleeArgIdx = rootParamAt(0);
  std::optional<uint32_t> rootVariadicArgIdx = rootParamAt(2);
  if (!rootCalleeArgIdx || !rootVariadicArgIdx ||
      *rootCalleeArgIdx == *rootVariadicArgIdx ||
      !rootLiteralAt(1, "(") || !rootLiteralAt(3, ")") ||
      rootDefinition.defParams[*rootCalleeArgIdx].variadic ||
      !rootDefinition.defParams[*rootVariadicArgIdx].variadic)
    return std::nullopt;

  auto calleeParamAt = [&](size_t idx) -> std::optional<uint32_t> {
    const RefoldModel::MacroReplacementToken &tok =
        calleeDefinition.replacementTokens[idx];
    if (tok.kind != RefoldModel::MacroReplacementTokenKind::ParamRef ||
        !tok.paramIndex || *tok.paramIndex >= calleeDefinition.defParams.size())
      return std::nullopt;
    return *tok.paramIndex;
  };
  auto calleeLiteralAt = [&](size_t idx, StringRef spelling) {
    const RefoldModel::MacroReplacementToken &tok =
        calleeDefinition.replacementTokens[idx];
    return tok.kind == RefoldModel::MacroReplacementTokenKind::Literal &&
           tok.spelling == spelling;
  };

  std::optional<uint32_t> fixedFinalArgIdx = calleeParamAt(0);
  std::optional<uint32_t> variadicFinalArgIdx = calleeParamAt(4);
  if (!fixedFinalArgIdx || !variadicFinalArgIdx ||
      *fixedFinalArgIdx == *variadicFinalArgIdx ||
      calleeDefinition.defParams[*fixedFinalArgIdx].variadic ||
      !calleeDefinition.defParams[*variadicFinalArgIdx].variadic ||
      !calleeLiteralAt(1, "__VA_OPT__") || !calleeLiteralAt(2, "(") ||
      !calleeLiteralAt(3, "#") || !calleeLiteralAt(5, ")"))
    return std::nullopt;

  GeneratedVaOptStringifyPayloadActivationShape shape;
  shape.rootCalleeArgIdx = *rootCalleeArgIdx;
  shape.rootVariadicArgIdx = *rootVariadicArgIdx;
  shape.fixedFinalArgIdx = *fixedFinalArgIdx;
  shape.variadicFinalArgIdx = *variadicFinalArgIdx;
  return shape;
}

/// Build an invocation-preserving patch for generated `__VA_OPT__`
/// stringification activation through a forwarding root.
///
/// The accepted example is:
///
///   CALL(S, "p")
///   #define CALL(f, ...) f(__VA_ARGS__)
///   #define S(prefix, ...) prefix __VA_OPT__(# __VA_ARGS__)
///
/// When the B-side expansion is `"p" "alpha"`, the fixed generated actual
/// `prefix = "p"` is still present and is therefore anchored.  The inserted
/// string literal is then decoded as the activated variadic payload.  The proof
/// fails closed unless the old expansion is exactly that fixed actual and the
/// new expansion is that same token followed by one decodable string literal.
std::optional<MacroPatch> tryBuildGeneratedVaOptStringifyPayloadActivationPatch(
    const RefoldMacroStandardArgsOnlyPatchBuilder::Dependencies &deps,
    const RefoldModel::MacroInvocation &invocation, const diffutils::Hunk &hunk,
    StringRef baseInvocationText,
    ArrayRef<std::pair<size_t, size_t>> invocationArgRanges) {
  if (!invocation.invB || !invocation.invE)
    return std::nullopt;

  const RefoldModel::MacroDirective *rootDefinition =
      getDefinitionDirectiveForInvocation(deps.model, invocation);
  if (!rootDefinition || !rootDefinition->functionLike)
    return std::nullopt;

  std::optional<StringRef> calleeName =
      sliceInvocationArgumentText(baseInvocationText, invocationArgRanges, 0);
  if (!calleeName || calleeName->empty())
    return std::nullopt;

  uint32_t objectAliasHops = 0;
  const RefoldModel::MacroDirective *calleeDefinition =
      deps.resolveFunctionLikeMacroThroughAliasesWithHops(*calleeName,
                                                          &objectAliasHops);
  if (!calleeDefinition || !calleeDefinition->functionLike)
    return std::nullopt;

  std::optional<GeneratedVaOptStringifyPayloadActivationShape> shape =
      matchGeneratedVaOptStringifyPayloadActivationShape(*rootDefinition,
                                                         *calleeDefinition);
  if (!shape)
    return std::nullopt;
  if (shape->rootCalleeArgIdx >= invocationArgRanges.size() ||
      shape->rootVariadicArgIdx >= invocationArgRanges.size())
    return std::nullopt;

  // The root is a variadic forwarding call whose old pack supplies only the
  // fixed generated actual.  If the old root pack is already comma-separated,
  // this direct activation theorem would have to compose existing tail slots;
  // leave that to the general replay engine instead.
  std::optional<StringRef> fixedRootActual = sliceInvocationArgumentText(
      baseInvocationText, invocationArgRanges, shape->rootVariadicArgIdx);
  if (!fixedRootActual || fixedRootActual->empty() ||
      replacementIntroducesTopLevelComma(*fixedRootActual, deps.lexLang))
    return std::nullopt;

  std::optional<std::pair<uint64_t, uint64_t>> cover =
      RefoldMacroWholeCoverProof::GetWholeCoverATokRange(invocation);
  if (!cover || cover->second != cover->first + 1 ||
      hunk.aStart != cover->second || hunk.aEnd != cover->second ||
      hunk.bEnd != hunk.bStart + 1 || cover->first >= deps.aToks.size() ||
      hunk.bStart == 0 || hunk.bStart >= deps.bToks.size())
    return std::nullopt;

  const PPTok &oldPrefixTok = deps.aToks[static_cast<size_t>(cover->first)];
  const PPTok &bPrefixTok = deps.bToks[static_cast<size_t>(hunk.bStart - 1)];
  const PPTok &insertedTok = deps.bToks[static_cast<size_t>(hunk.bStart)];
  if (oldPrefixTok.spelling != bPrefixTok.spelling ||
      oldPrefixTok.spelling != fixedRootActual->trim() ||
      insertedTok.kind != "string_literal")
    return std::nullopt;

  std::optional<std::string> decodedPayload =
      decodeSimpleStringLiteralToken(insertedTok.spelling);
  if (!decodedPayload)
    return std::nullopt;
  std::optional<std::string> canonicalPayload =
      stringutils::canonicalizeStringifyInversePayload(*decodedPayload);
  if (!canonicalPayload || StringRef(*canonicalPayload).trim().empty())
    return std::nullopt;

  DenseMap<uint32_t, std::string> replacements;
  std::string rewrittenRootActual = fixedRootActual->trim().str();
  rewrittenRootActual += ", ";
  rewrittenRootActual += StringRef(*canonicalPayload).trim();
  replacements[shape->rootVariadicArgIdx] = std::move(rewrittenRootActual);

  InvocationActualRecoveryContext actualCtx{invocation, baseInvocationText,
                                            invocationArgRanges};
  std::optional<InvocationRewriteWithRange> rewrite =
      deps.buildInvocationRewriteWithRange(actualCtx, replacements,
                                           /*materializedRangeByArgIdx=*/nullptr);
  if (!rewrite || StringRef(rewrite->text).trim() == baseInvocationText.trim())
    return std::nullopt;

  MacroPatch patch{*invocation.invB, *invocation.invE, std::move(rewrite->text),
                   invocation.id};
  deps.proofCertifier.CertifyInvocationRewriteMaterializedOutputRange(
      patch, rewrite->materializedOutputByteStart,
      rewrite->materializedOutputByteEnd);
  deps.certifyMacroPatchWholeExpansionBRange(invocation, patch);
  deps.proofCertifier.SetArgsOnlyStandardProof(
      patch, invocation, /*wholeEnvelopeReplayValidated=*/true,
      /*definitionTapeReplayValidated=*/true);
  deps.proofCertifier.CertifyGeneratedCalleeReplayProof(
      patch, invocation, calleeDefinition->id, /*generatedCallDepth=*/1,
      objectAliasHops, /*usesStringification=*/true, /*usesPaste=*/false,
      /*usesVariadicForwarding=*/true,
      /*decodedStringLiteralEvidenceOnly=*/true);
  return patch;
}
/// Return true if a token hunk inside the root whole-cover is outside the
/// tuple-formal occurrences that ordinary args-only replay can update.
///
/// This is the residual *solving-side* question that survives the
/// admission/solving split, and it is deliberately not part of the theorem's
/// domain claim.  Once the object-selector/tuple theorem has admitted a root
/// and failed to prove a patch, ordinary args-only replay may still take the
/// case -- but only when every edit inside the whole cover lands in the tuple
/// formal it is able to rewrite.  A hunk outside that formal is a callee-body
/// or callee-identity effect, so args-only would preserve the old selector and
/// emit source that preprocesses to the wrong stream.
///
/// Removing this check does not merely refuse more: it refuses roots whose
/// edits are entirely attributable to the tuple actual, where args-only
/// produces a correct, selector-preserving fold.  See
/// selector_tuple_contained_tuple_edit_still_folds.c, which fails when this
/// condition is dropped.
bool hasWholeCoverHunkOutsideTupleArgument(
    const RefoldMacroStandardArgsOnlyPatchBuilder::Dependencies &deps,
    const RefoldModel::MacroInvocation &invocation, uint32_t tupleArgIdx,
    ArrayRef<RefoldModel::PPArgSpan> standardArgSpans) {
  auto cover = RefoldMacroWholeCoverProof::GetWholeCoverATokRange(invocation);
  if (!cover || cover->first >= cover->second)
    return false;

  SmallVector<RefoldModel::PPArgSpan, 8> tupleSpans;
  for (const RefoldModel::PPArgSpan &span : standardArgSpans) {
    if (span.kind == PPArgSpanKind::Standard && span.argIdx == tupleArgIdx)
      tupleSpans.push_back(span);
  }
  if (tupleSpans.empty())
    return false;

  for (const diffutils::Hunk &hunk : deps.abTokHunks) {
    if (hunk.aStart < cover->first || hunk.aEnd > cover->second)
      continue;

    SmallVector<char, 8> touched(tupleSpans.size(), 0);
    if (!deps.sourceMapper.HunkFullyWithinArgSpans(
            hunk, ArrayRef<RefoldModel::PPArgSpan>(tupleSpans.data(),
                                                   tupleSpans.size()),
            MutableArrayRef<char>(touched.data(), touched.size())))
      return true;
  }
  return false;
}

/// Repairs recorded standard arg-span formal indices through the immutable
/// producer definition tape when that tape proves an ordinary, non-stringify,
/// non-paste replay of the invocation's complete A-side cover.
///
/// This helper owns only the definition-tape proof/search obligation.  It does
/// not mutate planner state, candidate state, proof carriers, or occurrence
/// ordering; the caller receives the former local side effect as an explicit
/// result bit.  Any unsupported ambiguity -- mismatched definition identity,
/// stringify/paste syntax, invalid parameter references, incomplete cover
/// replay, empty parameter spans, or literal-token mismatch -- fails closed by
/// returning the originally recorded invocation spans with no repair flag.
/// Successful replay preserves the existing sorted standard-occurrence order
/// used by the old local lambda and only rewrites the formal indices proven by
/// the replacement-list parameter-reference sequence.
DefinitionReplayedStandardArgSpanRepair getDefinitionReplayedStandardArgSpans(
    const RefoldModel &model, llvm::ArrayRef<PPTok> aToks,
    const RefoldModel::MacroInvocation &m) {
  std::vector<RefoldModel::PPArgSpan> out = m.argSpans;

  // Producer arg indices can be ambiguous when an actual contains a comma
  // that is not protected by parentheses, e.g. `M(arr[1, 2], 3)`.  Clang's
  // source range for the first written argument may cover the bracketed text,
  // while macro replacement still substitutes the comma-separated pieces into
  // successive formals.  Repair only the fully provable case: the definition
  // replacement-list tape and the recorded standard spans must replay the
  // macro's complete A-side cover exactly, with one non-empty standard span
  // per replacement-list parameter reference.
  const RefoldModel::MacroDirective *definition =
      getDefinitionDirectiveForInvocation(model, m);
  if (!definition || !definition->IsFunctionLikeDefine() ||
      definition->name != m.name ||
      definition->defParams.size() != m.defParams.size() || out.empty() ||
      !m.stringifySpans.empty() || !m.pasteSpans.empty() ||
      !m.cover.IsValid() || m.cover.end > aToks.size())
    return makeOriginalDefinitionReplayedStandardArgSpanRepair(m);

  // Extract the formal-reference order from the macro replacement-list tape.
  // Stringify and paste are excluded because their spelling/segmentation rules
  // are not ordinary standard-argument substitution.
  SmallVector<uint32_t, 8> formalSeq;
  formalSeq.reserve(definition->replacementTokens.size());
  for (const RefoldModel::MacroReplacementToken &token :
       definition->replacementTokens) {
    if (token.spelling == "#" || token.spelling == "##")
      return makeOriginalDefinitionReplayedStandardArgSpanRepair(m);
    if (token.kind != RefoldModel::MacroReplacementTokenKind::ParamRef)
      continue;
    if (!token.paramIndex || *token.paramIndex >= m.defParams.size())
      return makeOriginalDefinitionReplayedStandardArgSpanRepair(m);
    formalSeq.push_back(*token.paramIndex);
  }

  if (formalSeq.size() != out.size())
    return makeOriginalDefinitionReplayedStandardArgSpanRepair(m);

  llvm::sort(out, ppArgSpanLessByTokenRangeAndArg);

  // Replay the definition replacement-list tape over the A-side macro cover.
  // Literals must match real expanded tokens; each parameter reference must
  // consume the next recorded standard span exactly at the current cursor.
  uint64_t tok = m.cover.begin;
  size_t argSpanIdx = 0;
  for (const RefoldModel::MacroReplacementToken &repTok :
       definition->replacementTokens) {
    switch (repTok.kind) {
    case RefoldModel::MacroReplacementTokenKind::Literal:
      if (tok >= m.cover.end || tok >= aToks.size() ||
          aToks[static_cast<size_t>(tok)].spelling != repTok.spelling)
        return makeOriginalDefinitionReplayedStandardArgSpanRepair(m);
      ++tok;
      break;
    case RefoldModel::MacroReplacementTokenKind::ParamRef: {
      if (argSpanIdx >= out.size())
        return makeOriginalDefinitionReplayedStandardArgSpanRepair(m);
      const RefoldModel::PPArgSpan &sp = out[argSpanIdx];
      if (sp.kind != PPArgSpanKind::Standard || sp.begin != tok ||
          sp.begin >= sp.end || sp.end > m.cover.end || sp.end > aToks.size())
        return makeOriginalDefinitionReplayedStandardArgSpanRepair(m);
      tok = sp.end;
      ++argSpanIdx;
      break;
    }
    }
  }

  if (tok != m.cover.end || argSpanIdx != out.size())
    return makeOriginalDefinitionReplayedStandardArgSpanRepair(m);

  bool changed = false;
  for (size_t i = 0; i < out.size(); ++i) {
    if (out[i].argIdx != formalSeq[i])
      changed = true;
    out[i].argIdx = formalSeq[i];
  }

  DefinitionReplayedStandardArgSpanRepair result;
  result.argSpans = std::move(out);
  result.replayedFormalIndices = changed;
  return result;
}



} // namespace

std::optional<MacroPatch>
RefoldMacroStandardArgsOnlyPatchBuilder::TryBuildHigherOrderGeneratedReplay(
    const RefoldModel::MacroInvocation &invocation,
    const diffutils::Hunk &hunk, StringRef baseInvocationText,
    ArrayRef<std::pair<size_t, size_t>> invocationArgRanges) const {
  // Reuse the exact theorem ordering and fail-closed behavior of the private
  // higher-order probe.  This wrapper adds no alternate solver or fallback; it
  // only exposes the already-existing proof sequence to whole-cover candidate
  // arbitration after an earlier args-only path returned first.
  return HigherOrderGeneratedReplayProbe(deps_).TryBuild(
      invocation, hunk, baseInvocationText, invocationArgRanges);
}

std::optional<MacroPatch>
RefoldMacroStandardArgsOnlyPatchBuilder::BuildStandardArgsOnlyPatch(
    const ArgsOnlyPlanningContext &ctx) const {
  const RefoldModel::MacroInvocation &m = ctx.invocation;
  const diffutils::Hunk &h = ctx.hunk;
  const diffutils::Hunk &hArgs = ctx.hunk;
  StringRef baseInvText = ctx.baseInvocationText;
  ArrayRef<std::pair<size_t, size_t>> invArgRanges =
      ctx.actualLayout.rangePairs();
  const InvocationActualRecoveryContext actualRecoveryCtx{m, baseInvText,
                                                          invArgRanges};
  const ArgsOnlyTemplateReplayContext argsOnlyTemplateCtx{m, baseInvText,
                                                          invArgRanges};

  // Standard args-only policy:
  // Collect arg-span occurrences (and stringify occurrences) and require the
  // entire hunk to be covered by those spans before deriving per-arg
  // replacements from the B slices.  Pure paste-only replay is handled as the
  // deterministic fallback for this same ranking slot when no standard or
  // stringify occurrences exist.  The immutable producer #define repair remains
  // private to this translation unit and reports its former side effect
  // explicitly.
  bool replayedStandardArgSpanFormalIndices = false;

  // Prefer the direct current-level invocation parse; fall back to definition
  // replay only for older producer shapes where the replacement-list tape
  // proves the same reindexing.
  std::vector<RefoldModel::PPArgSpan> standardArgSpans;
  if (auto currentLevelSpans = TemplateSolver().GetCurrentLevelStandardArgSpans(
          argsOnlyTemplateCtx)) {
    standardArgSpans = std::move(*currentLevelSpans);
  } else {
    DefinitionReplayedStandardArgSpanRepair repair =
        getDefinitionReplayedStandardArgSpans(deps_.model, deps_.aToks, m);
    replayedStandardArgSpanFormalIndices = repair.replayedFormalIndices;
    standardArgSpans = std::move(repair.argSpans);
  }

  TouchedFormalHunkCollection touchedFormalHunks;
  std::vector<RefoldModel::PPArgSpan> &occs =
      touchedFormalHunks.occurrences;
  std::vector<char> &occIsStringify =
      touchedFormalHunks.occurrenceIsStringify;

  append_range(occs, standardArgSpans);
  append_range(occs, m.stringifySpans);

  occIsStringify.resize(occs.size());
  std::fill_n(occIsStringify.begin(), standardArgSpans.size(), false);
  std::fill_n(occIsStringify.begin() + standardArgSpans.size(),
              m.stringifySpans.size(), true);

  // Pure paste-only invocations have no STANDARD or STRINGIFY evidence, so the
  // normal args-only path bottoms out at occs.empty(). Keep this ranking guard
  // in the main orchestration body, then delegate only the deterministic
  // paste-edit derivation and proof-candidate construction.
  if (occs.empty() && !m.pasteSpans.empty()) {
    return PurePasteOnlyArgsOnlyCandidateBuilder(deps_, PasteArgumentBuilder())
        .TryBuild(m, hArgs, baseInvText, invArgRanges, actualRecoveryCtx);
  }

  if (occs.empty() && m.pasteSpans.empty() && !m.pasteTokens.empty()) {
    if (auto directVaOptPasteTokenPatch = tryBuildDirectVaOptPasteTokenPatch(
            deps_, m, hArgs, baseInvText, invArgRanges, actualRecoveryCtx))
      return directVaOptPasteTokenPatch;
  }

  if (auto directVaOptStringifyDeactivationPatch =
          tryBuildDirectVaOptStringifyDeactivationPatch(
              deps_, m, hArgs, baseInvText, invArgRanges))
    return directVaOptStringifyDeactivationPatch;

  if (auto generatedVaOptStringifyPayloadPatch =
          tryBuildGeneratedVaOptStringifyPayloadActivationPatch(
              deps_, m, hArgs, baseInvText, invArgRanges))
    return generatedVaOptStringifyPayloadPatch;

  // Higher-order generated replay remains in its historical ranking position
  // before ordinary occurrence collection.  The private probe owns only the
  // generated-callee / generated-leaf / tuple-generated-callee discovery
  // sequence and returns nullopt for a non-terminal miss.
  if (auto higherOrderGeneratedPatch =
          HigherOrderGeneratedReplayProbe(deps_).TryBuild(m, h, baseInvText,
                                                          invArgRanges))
    return higherOrderGeneratedPatch;

  // A theorem that admits a root and then fails to prove a patch for it owns
  // that root, and ordinary args-only replay must not silently take it over.
  // For an object-selector/tuple forwarder `f t`, args-only would rewrite the
  // tuple actual while preserving the old selector spelling, emitting source
  // that preprocesses to a stream the edit never asked for.
  //
  // The claim consulted is the theorem's own, not a second coding of it: the
  // gate this replaced re-derived the shape independently -- a distinct root
  // parse, alias walk and topology scan -- and could disagree with the theorem
  // about which roots were in scope.  Admission is now stated once.
  //
  // Ownership alone is not the whole rule, though.  `hasWholeCoverHunkOutside-
  // TupleArgument` is the residual solving-side condition: when every edit in
  // the whole cover lands inside the tuple formal args-only can rewrite, the
  // old selector is still correct and args-only produces a sound, tight fold.
  // Refusing those too costs real refolds and buys no soundness -- see
  // selector_tuple_contained_tuple_edit_still_folds.c.
  if (const RefoldModel::MacroDirective *rootDefinition =
          getDefinitionDirectiveForInvocation(deps_.model, m)) {
    if (std::optional<ObjectSelectorTupleRootClaim> selectorTupleClaim =
            deps_.generatedCalleeReplayEngine.ClaimObjectSelectorTupleRoot(
                *rootDefinition, baseInvText, invArgRanges)) {
      if (hasWholeCoverHunkOutsideTupleArgument(
              deps_, m, selectorTupleClaim->tupleArgIdx, standardArgSpans))
        return std::nullopt;
    }
  }

  RefoldMacroOccurrenceReplay occurrenceReplay = OccurrenceReplay();
  std::optional<TouchedFormalHunkCollection> collectedTouchedFormalHunks =
      TouchedFormalHunkCollector(deps_, occurrenceReplay, envelopeCache_.get())
          .Collect(touchedFormalHunks, m, hArgs, invArgRanges.size());
  if (!collectedTouchedFormalHunks)
    return std::nullopt;
  touchedFormalHunks = std::move(*collectedTouchedFormalHunks);

  // `touched` is now indexed by formal argument, not occurrence. Later checks
  // use it to decide which invocation arguments need replacement and which must
  // remain unchanged.  `tokenHunks` is the normalized authoritative hunk set for
  // those touched formal arguments.
  std::vector<char> &touched = touchedFormalHunks.touchedFormals;
  ArrayRef<diffutils::Hunk> tokenHunks(touchedFormalHunks.tokenHunks);

  // Caller-tuple forwarding remains in the same ranking position as before;
  // only the proof/search body is isolated in a private resolver.
  CallerTupleForwardedRewriteResolver callerTupleForwardedRewriteResolver(
      deps_, m);
  // Definition-replayed span repair has its own all-occurrences proof because it
  // validates against the repaired `standardArgSpans` vector rather than the raw
  // producer occurrence indices.
  ReplayedFormalOccurrenceValidator replayedFormalOccurrenceValidator(
      deps_, occurrenceReplay);
  TupleSliceConsistencyValidator tupleSliceConsistencyValidator(deps_,
                                                               occurrenceReplay);

  // Compute argument replacements implied by each touched occurrence. Multiple
  // occurrences of the same argIdx must imply the exact same replacement,
  // otherwise the macro cannot be refolded args-only.  Finalized replacements
  // are recorded in a carrier so proof certification consumes explicit, completed
  // state rather than continuing to own occurrence-discovery locals.
  ArgsOnlyFinalArgumentRewriteSet finalArgumentRewrites;
  SmallVector<uint32_t, 8> touchedArgIdxs;
  for (const auto &sp : occs) {
    if (sp.argIdx >= touched.size() || !touched[sp.argIdx])
      continue;
    if (!llvm::is_contained(touchedArgIdxs, sp.argIdx))
      touchedArgIdxs.push_back(sp.argIdx);
  }

  for (uint32_t argIdx : touchedArgIdxs) {
    if (static_cast<size_t>(argIdx) >= invArgRanges.size())
      return std::nullopt;

    auto r0 = invArgRanges[argIdx];
    StringRef baseArgText = baseInvText.substr(r0.first, r0.second - r0.first);

    std::optional<InvocationOccurrenceObservationSet> collectedOccurrences =
        InvocationOccurrenceObservationCollector(deps_, occurrenceReplay)
            .Collect(m, argIdx, baseArgText, occs, occIsStringify, tokenHunks, h);
    if (!collectedOccurrences)
      return std::nullopt;

    InvocationOccurrenceObservationSet occurrenceObservation =
        std::move(*collectedOccurrences);
    SmallVector<OccObservation, 8> &occObservations =
        occurrenceObservation.observations;
    const std::optional<std::string> &unifiedNewArg =
        occurrenceObservation.unifiedNewArg;
    const std::optional<std::pair<uint64_t, uint64_t>>
        &unifiedMaterializedNewTextRange =
            occurrenceObservation.unifiedMaterializedNewTextRange;
    const bool sawUntrackedMaterializedNewTextRange =
        occurrenceObservation.sawUntrackedMaterializedNewTextRange;
    const bool needTupleForwarding = occurrenceObservation.needTupleForwarding;

    std::string finalNewArg;
    bool tupleForwarded = false;

    if (needTupleForwarding) {
      // Occurrence observations for this formal did not collapse to one uniform
      // replacement. Try the narrower tuple-forwarding proof before rejecting
      // the args-only rewrite outright.
      if (!callerTupleForwardedRewriteResolver.TryRewrite(
          argIdx, baseArgText, occObservations, finalNewArg)) {
        return std::nullopt;
      }
      tupleForwarded = true;
    } else if (unifiedNewArg) {
      // All observed occurrences of this formal agreed on one replacement
      // spelling.  Before accepting a whole-argument expansion replacement,
      // give direct tuple-ref forwarding a chance to prove a more structural
      // edit of a caller tuple element.  This covers generated-callee shapes
      // such as `WRAP((ADD_ONE, 10))`, where the root occurrence is the full
      // callee expansion but the actual source edit belongs to the tuple
      // element `10`.
      if (callerTupleForwardedRewriteResolver.TryRewrite(
          argIdx, baseArgText, occObservations, finalNewArg)) {
        tupleForwarded = true;
      } else {
        finalNewArg = *unifiedNewArg;
      }
    } else {
      // This formal had no usable observation from the touched hunk set.
      continue;
    }

    std::optional<std::pair<uint64_t, uint64_t>> finalMaterializedRange;
    if (!tupleForwarded && !sawUntrackedMaterializedNewTextRange &&
        unifiedMaterializedNewTextRange && unifiedNewArg &&
        finalNewArg == *unifiedNewArg) {
      finalMaterializedRange = *unifiedMaterializedNewTextRange;
    }

    // Replacing a non-variadic formal with a top-level comma would change macro
    // invocation arity, so reject it before validating occurrence consistency.
    if (!isMacroInvocationVariadicFormal(m, argIdx) &&
        replacementIntroducesTopLevelComma(finalNewArg, deps_.lexLang))
      return std::nullopt;

    const bool matchesAllOccurrences =
        tupleForwarded ? tupleSliceConsistencyValidator.Validate(
                             argIdx, standardArgSpans, occObservations, tokenHunks)
        : replayedStandardArgSpanFormalIndices
            ? replayedFormalOccurrenceValidator.Validate(
                  argIdx, finalNewArg, standardArgSpans, tokenHunks)
            : occurrenceReplay.MacroArgReplacementMatchesAllOccurrencesInB(
                  m, argIdx, baseArgText, finalNewArg, tokenHunks);

    if (!matchesAllOccurrences) {
      // The local observations were explainable, but the completed replacement
      // failed the global occurrence proof.  There is no recovery or diagnostic
      // side channel at this point: reject the args-only candidate fail-closed
      // rather than attempting to preserve unused hunk-effect trace strings.
      return std::nullopt;
    }

    if (isMacroInvocationVariadicFormal(m, argIdx)) {
      StringRef trimmedFinal(finalNewArg);
      trimmedFinal = trimmedFinal.trim();

      // The replay check above validates the edited expansion surface. For the
      // invocation spelling, however, a variadic formal does not own the fixed
      // separator before it. If deletion of the first tuple element left that
      // separator at the front of the reconstructed replacement, remove exactly
      // one leading comma so the callsite spells the shortened tuple rather
      // than an empty first variadic argument.
      if (!trimmedFinal.empty() && trimmedFinal.front() == ',') {
        trimmedFinal = trimmedFinal.drop_front();
        while (!trimmedFinal.empty() &&
               (trimmedFinal.front() == ' ' || trimmedFinal.front() == '\t'))
          trimmedFinal = trimmedFinal.drop_front();
        finalNewArg = trimmedFinal.str();
        finalMaterializedRange = std::nullopt;
      }
    }

    finalArgumentRewrites.Record(argIdx, std::move(finalNewArg),
                                  finalMaterializedRange, tupleForwarded);
  }

  return ArgsOnlyProofCertifier(deps_).BuildAcceptedCandidate(
      m, actualRecoveryCtx, finalArgumentRewrites);
}

bool RefoldMacroStandardArgsOnlyPatchBuilder::
    DerivedReplacementsReproduceStringifiedOperands(
        const RefoldModel::MacroInvocation &m,
        const DenseMap<uint32_t, std::string> &replacementsByArgIdx) const {
  if (m.stringifySpans.empty())
    return true;

  for (const RefoldModel::PPArgSpan &stringifySpan : m.stringifySpans) {
    auto replacement = replacementsByArgIdx.find(stringifySpan.argIdx);
    if (replacement == replacementsByArgIdx.end())
      continue;

    // The stringified operand is one B token: the literal the edited stream
    // actually spells at this position.
    auto bEnv =
        deps_.sourceMapper.MapAToBTokenEnvelopeByPPArgSpan(stringifySpan);
    if (!bEnv || bEnv->second <= bEnv->first ||
        bEnv->second - bEnv->first != 1) {
      return false;
    }

    StringRef bStringifiedToken =
        deps_.sourceMapper.SliceBSource(bEnv->first, bEnv->second).trim();
    std::optional<std::string> decoded =
        deps_.argTextRecovery.UnstringifyLiteralToArgText(
            bStringifiedToken, /*allowTopLevelComma=*/true);
    if (!decoded)
      return false;
    std::optional<std::string> canonical =
        stringutils::canonicalizeStringifyInversePayload(*decoded);
    if (!canonical)
      return false;

    // Clang stringification collapses internal whitespace, so compare the
    // canonicalized inverse payload rather than raw spellings.
    std::optional<std::string> canonicalReplacement =
        stringutils::canonicalizeStringifyInversePayload(replacement->second);
    if (!canonicalReplacement)
      return false;

    if (StringRef(*canonical).trim() !=
        StringRef(*canonicalReplacement).trim()) {
      REFOLD_LOG_TRACE(
          "macro/args-only",
          "reject inv id={0} name={1} reason=stringify-operand-not-reproduced "
          "argIdx={2} bStringified='{3}' derivedArg='{4}'",
          m.id, m.name, stringifySpan.argIdx, bStringifiedToken,
          replacement->second);
      return false;
    }
  }

  return true;
}

std::optional<InvocationActualLayout>
RefoldMacroStandardArgsOnlyPatchBuilder::RecoverInvocationActuals(
    const RefoldModel::MacroInvocation &invocation,
    StringRef baseInvocationText) const {
  // Args-only replay starts with data recovery, not candidate construction.
  // Keep recovery fail-closed and limited to the required availability checks:
  // a concrete invocation byte span, a literal callee origin, parsed
  // formal-content ranges, and the named actual-recovery precondition.
  if (!invocation.invB || !invocation.invE)
    return std::nullopt;

  if (!hasLiteralMacroCalleeOrigin(invocation))
    return std::nullopt;

  auto rangesOpt = RefoldMacroActualLayout({&deps_.lexLang})
                       .GetMacroInvocationFormalArgContentRanges(
                           invocation, baseInvocationText);
  if (!rangesOpt)
    return std::nullopt;

  InvocationActualLayout layout;
  layout.contentRanges = std::move(*rangesOpt);

  InvocationActualRecoveryContext actualRecoveryCtx{
      invocation, baseInvocationText, layout.rangePairs()};
  if (!InvocationActualsAreRecoverable(actualRecoveryCtx))
    return std::nullopt;

  return layout;
}

bool RefoldMacroStandardArgsOnlyPatchBuilder::InvocationActualsAreRecoverable(
    const InvocationActualRecoveryContext &ctx) const {
  // The source-spelling rewrite path is only meaningful for an invocation
  // whose physical callsite byte range exists.  The caller already obtained
  // the formal-content ranges from the recovery service; this method gives
  // that data-availability precondition a named fail-closed boundary.
  if (!ctx.invocation.invB || !ctx.invocation.invE)
    return false;

  // Do not validate every recovered argument range here: the rewrite builder
  // validates only the argument indices it is asked to replace.
  // Keeping that check in BuildInvocationRewriteWithRange preserves the exact
  // fail-closed timing for paths that never touch a particular actual.
  (void)ctx.baseInvocationText;
  (void)ctx.invocationArgRanges;
  return true;
}

RefoldMacroStandardArgsOnlyPatchBuilder::ArgsOnlyPatchAttempt
RefoldMacroStandardArgsOnlyPatchBuilder::BuildPasteAwareArgsOnlyPatch(
    const ArgsOnlyPlanningContext &ctx) const {
  const RefoldModel::MacroInvocation &m = ctx.invocation;
  const diffutils::Hunk &h = ctx.hunk;
  StringRef baseInvText = ctx.baseInvocationText;
  ArrayRef<std::pair<size_t, size_t>> invArgRanges =
      ctx.actualLayout.rangePairs();
  const diffutils::Hunk tokenHunksCurrent[] = {h};
  SmallVector<diffutils::Hunk, 4> tokenHunksInInvocationCover;
  for (const diffutils::Hunk &candidateHunk : deps_.abTokHunks) {
    if (m.Covers(candidateHunk.aStart, candidateHunk.aEnd))
      tokenHunksInInvocationCover.push_back(candidateHunk);
  }
  if (tokenHunksInInvocationCover.empty())
    tokenHunksInInvocationCover.push_back(h);

  const InvocationActualRecoveryContext actualRecoveryCtx{m, baseInvText,
                                                          invArgRanges};

  // Fast path for token-paste edits. A single pasted token can embed multiple
  // argument contributions (e.g., X##_##Y##_##Z), so a single edit hunk may
  // change multiple arg segments inside that token (e.g., a_b_c -> d_e_f). In
  // that case we attempt to derive per-arg segment replacements and splice them
  // into the invocation spelling.
  if (!PasteArgumentBuilder().HunkTouchesAnyPasteToken(m, h))
    return ArgsOnlyPatchAttempt::ContinueSearchResult();

  // A pasted token can be locally ambiguous while another occurrence of the
  // same formal disambiguates it.  For example, in `x##y` plus `#x`, the edit
  // `abc -> alphabc` alone could be read as either `x: a -> alpha` or
  // `y: bc -> lphabc`; the stringified occurrence `"a" -> "alpha"` proves the
  // former.  Before the legacy single-segment paste fallback assigns bytes at
  // an undelimited paste boundary, collect exact stringify-derived formal
  // replacements and ask the grouped paste replay validator whether those
  // replacements already explain every pasted token in B.  This is still
  // fail-closed: malformed stringification, conflicting constraints, top-level
  // comma introduction for non-variadic formals, or paste replay mismatch all
  // decline this constrained path instead of guessing a boundary.
  DenseMap<uint32_t, std::string> stringifyConstrainedReplacements;
  for (const RefoldModel::PPArgSpan &stringifySpan : m.stringifySpans) {
    uint32_t argIdx = stringifySpan.argIdx;
    if (static_cast<size_t>(argIdx) >= invArgRanges.size())
      continue;

    bool argParticipatesInPaste = false;
    for (const RefoldModel::PPArgSpan &pasteSpan : m.pasteSpans) {
      if (pasteSpan.argIdx == argIdx) {
        argParticipatesInPaste = true;
        break;
      }
    }
    if (!argParticipatesInPaste)
      continue;

    auto bEnv =
        deps_.sourceMapper.MapAToBTokenEnvelopeByPPArgSpan(stringifySpan);
    if (!bEnv || bEnv->second <= bEnv->first || bEnv->second - bEnv->first != 1)
      continue;

    StringRef bStringifiedToken =
        deps_.sourceMapper.SliceBSource(bEnv->first, bEnv->second).trim();
    std::optional<std::string> decoded =
        deps_.argTextRecovery.UnstringifyLiteralToArgText(
            bStringifiedToken, /*allowTopLevelComma=*/true);
    if (!decoded)
      continue;
    std::optional<std::string> canonical =
        stringutils::canonicalizeStringifyInversePayload(*decoded);
    if (!canonical)
      continue;

    std::string newArg = StringRef(*canonical).trim().str();
    if (!isMacroInvocationVariadicFormal(m, argIdx) &&
        replacementIntroducesTopLevelComma(newArg, deps_.lexLang))
      continue;

    auto range = invArgRanges[argIdx];
    StringRef baseArgText =
        baseInvText.substr(range.first, range.second - range.first);
    if (!OccurrenceReplay()
             .MacroArgReplacementMatchesAllOccurrencesInBIgnorePaste(
                 m, argIdx, baseArgText, newArg, tokenHunksInInvocationCover))
      continue;

    auto existing = stringifyConstrainedReplacements.find(argIdx);
    if (existing != stringifyConstrainedReplacements.end()) {
      if (existing->second != newArg)
        return ArgsOnlyPatchAttempt::RejectResult();
      continue;
    }
    stringifyConstrainedReplacements[argIdx] = std::move(newArg);
  }

  if (!stringifyConstrainedReplacements.empty() &&
      PasteArgumentBuilder().PasteArgReplacementsMatchAllPasteTokensInB(
          m, baseInvText, invArgRanges, stringifyConstrainedReplacements)) {
    std::optional<InvocationRewriteWithRange> rewrite =
        deps_.buildInvocationRewriteWithRange(
            actualRecoveryCtx, stringifyConstrainedReplacements,
            /*materializedRangeByArgIdx=*/nullptr);
    if (!rewrite)
      return ArgsOnlyPatchAttempt::RejectResult();

    MacroPatch patch{*m.invB, *m.invE, std::move(rewrite->text), m.id};
    deps_.proofCertifier.CertifyInvocationRewriteMaterializedOutputRange(
        patch, rewrite->materializedOutputByteStart,
        rewrite->materializedOutputByteEnd);
    deps_.certifyMacroPatchWholeExpansionBRange(m, patch);
    MacroPatchProof proof =
        makeMacroPatchProof(MacroPatchProofKind::ArgsOnlyPasteMulti,
                            /*preservesInvocationStructure=*/true, m.id);
    proof.paste->replayValidated = true;
    deps_.macroPatchProofClassifier.SetMacroPatchProof(patch, std::move(proof));
    return ArgsOnlyPatchAttempt::AcceptedResult(std::move(patch));
  }

  auto edits = PasteArgumentBuilder().DerivePasteArgEdits(m, h);
  if (edits && !edits->empty()) {
    DenseMap<uint32_t, std::string> replByArgIdx;
    for (const auto &pae : *edits) {
      uint32_t argIdx = pae.argIdx;
      if (static_cast<size_t>(argIdx) >= invArgRanges.size())
        return ArgsOnlyPatchAttempt::RejectResult();

      // A single argument may contribute multiple segments to the same
      // pasted token (e.g. X##_..._##X). We merge repeated argIdx
      // conservatively after deriving the candidate replacement below.
      auto range = invArgRanges[argIdx];
      StringRef baseArgText =
          baseInvText.substr(range.first, range.second - range.first);

      // Prefer an exact source-slice splice when the replay witness
      // provides one. Otherwise retain the existing conservative boundary
      // splice for legacy paste-span metadata.
      std::string newArg =
          (pae.argByteBegin && pae.argByteEnd)
              ? RefoldMacroPasteSpelling::
                    SplicePasteSegmentIntoSpellingArgExact(
                        baseArgText, *pae.argByteBegin, *pae.argByteEnd,
                        pae.oldSeg, pae.newSeg)
              : RefoldMacroPasteSpelling::SplicePasteSegmentIntoSpellingArg(
                    baseArgText, pae.oldSeg, pae.newSeg);
      if (newArg.empty()) {
        // Deleting an entire argument (making it empty) is legal. Accept this
        // only when the paste-span covered the whole argument spelling.
        if (!(StringRef(pae.newSeg).trim().empty() &&
              baseArgText.trim() == StringRef(pae.oldSeg).trim()))
          return ArgsOnlyPatchAttempt::RejectResult();
      }

      // If the argument is not the variadic formal, replacing it with a
      // text that introduces a top-level comma would change the macro
      // invocation's argument list.
      if (!isMacroInvocationVariadicFormal(m, argIdx) &&
          replacementIntroducesTopLevelComma(newArg, deps_.lexLang))
        return ArgsOnlyPatchAttempt::RejectResult();

      auto existing = replByArgIdx.find(argIdx);
      if (existing != replByArgIdx.end()) {
        if (existing->second != newArg)
          return ArgsOnlyPatchAttempt::RejectResult();
        continue;
      }

      // Per-arg safety gate: validate standard + stringify occurrences for
      // this arg.
      //
      // NOTE: For multi-span paste edits where the pasted token length may
      // change, per-arg paste-span validation cannot be done reliably in
      // isolation. We validate paste tokens as a *group* below via
      // pasteArgReplacementsMatchAllPasteTokensInB(...).
      // Validate the locally derived paste replacement only against the hunk
      // that produced it.  Repeated paste macros may contain many independent
      // pasted products for the same formal; widening this per-segment check
      // to the whole invocation cover can make one local derivation reject
      // because other pasted products have not yet contributed their own local
      // splice.  The complete replay obligation is still discharged below by
      // `PasteArgReplacementsMatchAllPasteTokensInB`, after all derived
      // replacements for this hunk have been collected.
      if (!OccurrenceReplay()
               .MacroArgReplacementMatchesAllOccurrencesInBIgnorePaste(
                   m, argIdx, baseArgText, newArg, tokenHunksCurrent)) {
        return ArgsOnlyPatchAttempt::RejectResult();
      }

      replByArgIdx[argIdx] = std::move(newArg);
    }

    if (!replByArgIdx.empty()) {
      // Combined safety gate: applying all derived replacements must
      // reconstruct every pasted token occurrence exactly as seen in B.
      if (!PasteArgumentBuilder().PasteArgReplacementsMatchAllPasteTokensInB(
              m, baseInvText, invArgRanges, replByArgIdx)) {
        return ArgsOnlyPatchAttempt::RejectResult();
      }

      // The paste gate above proves only the pasted operands.  A parameter that
      // is also stringified is constrained independently, and the per-argument
      // check earlier sees only the hunk that produced the replacement -- an
      // unchanged literal such as `"mime"` lies outside it and is never
      // examined.  Prove the stringified operands too, so an argument derived
      // purely from the paste cannot silently rewrite a string literal that B
      // left alone.
      if (!DerivedReplacementsReproduceStringifiedOperands(m, replByArgIdx))
        return ArgsOnlyPatchAttempt::RejectResult();

      std::optional<InvocationRewriteWithRange> rewrite =
          deps_.buildInvocationRewriteWithRange(
              actualRecoveryCtx, replByArgIdx,
              /*materializedRangeByArgIdx=*/nullptr);
      if (!rewrite)
        return ArgsOnlyPatchAttempt::RejectResult();

      {
        MacroPatch patch{*m.invB, *m.invE, std::move(rewrite->text), m.id};
        deps_.proofCertifier.CertifyInvocationRewriteMaterializedOutputRange(
            patch, rewrite->materializedOutputByteStart,
            rewrite->materializedOutputByteEnd);
        // Paste replay validates the rewritten callsite against every pasted
        // token occurrence in the expansion.  For the edit map, therefore,
        // the B-side materialization is the whole expansion cover that the
        // source argument rewrite regenerates, not merely the first changed
        // pasted-token hunk.
        deps_.certifyMacroPatchWholeExpansionBRange(m, patch);
        MacroPatchProof proof =
            makeMacroPatchProof(MacroPatchProofKind::ArgsOnlyPasteMulti,
                                /*preservesInvocationStructure=*/true, m.id);
        // The builder already proved this rewrite by replaying the rewritten
        // invocation arguments against every pasted token occurrence in B.
        // Carry that proof source onto the accepted patch for converted
        // selector-site discharge.
        proof.paste->replayValidated = true;
        deps_.macroPatchProofClassifier.SetMacroPatchProof(patch,
                                                           std::move(proof));
        return ArgsOnlyPatchAttempt::AcceptedResult(std::move(patch));
      }
    }
  }

  // Single-segment paste edit (existing behavior)
  //
  // This handles the common case where only one pasted segment changes (e.g.
  // X##_##Y, changing just X). The multi-span derivation above requires token
  // lengths to remain stable; when they do not, we fall back to deriving a
  // single segment edit from the token-level diff.
  auto pae = PasteArgumentBuilder().DerivePasteArgEdit(m, h);
  if (pae) {
    uint32_t argIdx = pae->argIdx;

    // HARD FAILURE: If we derived a paste edit but the index is invalid,
    // we must exit, not fall through.
    if (static_cast<size_t>(argIdx) >= invArgRanges.size())
      return ArgsOnlyPatchAttempt::RejectResult();

    auto r = invArgRanges[argIdx];
    StringRef baseArgText = baseInvText.substr(r.first, r.second - r.first);
    std::string newArg =
        (pae->argByteBegin && pae->argByteEnd)
            ? RefoldMacroPasteSpelling::SplicePasteSegmentIntoSpellingArgExact(
                  baseArgText, *pae->argByteBegin, *pae->argByteEnd,
                  pae->oldSeg, pae->newSeg)
            : RefoldMacroPasteSpelling::SplicePasteSegmentIntoSpellingArg(
                  baseArgText, pae->oldSeg, pae->newSeg);
    if (newArg.empty()) {
      // Deleting an entire argument (making it empty) is legal. Accept this
      // only when the paste-span covered the whole argument spelling.
      if (!(StringRef(pae->newSeg).trim().empty() &&
            baseArgText.trim() == StringRef(pae->oldSeg).trim()))
        return ArgsOnlyPatchAttempt::RejectResult();
    }

    if (!isMacroInvocationVariadicFormal(m, argIdx) &&
        replacementIntroducesTopLevelComma(newArg, deps_.lexLang))
      return ArgsOnlyPatchAttempt::RejectResult();

    // Safety gate: for single-segment paste edits we can directly validate
    // all occurrences, including paste-span occurrences, against the B
    // stream.
    if (!OccurrenceReplay().MacroArgReplacementMatchesAllOccurrencesInB(
            m, argIdx, baseArgText, newArg, tokenHunksCurrent)) {
      return ArgsOnlyPatchAttempt::RejectResult();
    }

    DenseMap<uint32_t, std::string> singleReplByArgIdx;
    singleReplByArgIdx[argIdx] = std::move(newArg);
    std::optional<InvocationRewriteWithRange> rewrite =
        deps_.buildInvocationRewriteWithRange(
            actualRecoveryCtx, singleReplByArgIdx,
            /*materializedRangeByArgIdx=*/nullptr);
    if (!rewrite)
      return ArgsOnlyPatchAttempt::RejectResult();

    {
      MacroPatch patch{*m.invB, *m.invE, std::move(rewrite->text), m.id};
      deps_.proofCertifier.CertifyInvocationRewriteMaterializedOutputRange(
          patch, rewrite->materializedOutputByteStart,
          rewrite->materializedOutputByteEnd);
      // As with the multi-paste path, the source argument rewrite is a
      // compact representation of the macro's replayed expansion surface.
      // Keep the B-side map anchored to that whole expansion envelope.
      deps_.certifyMacroPatchWholeExpansionBRange(m, patch);
      MacroPatchProof proof =
          makeMacroPatchProof(MacroPatchProofKind::ArgsOnlyPasteSingle,
                              /*preservesInvocationStructure=*/true, m.id);
      // Single-segment paste rewrites are admitted only after direct replay
      // validation against all touched occurrences in B. Record that proof
      // source explicitly for converted selector-site discharge.
      proof.paste->replayValidated = true;
      deps_.macroPatchProofClassifier.SetMacroPatchProof(patch,
                                                         std::move(proof));
      return ArgsOnlyPatchAttempt::AcceptedResult(std::move(patch));
    }
  }

  // If we touched paste but could not safely derive a paste splice patch,
  // fall through to the standard (non-paste) args-only policy below.

  return ArgsOnlyPatchAttempt::ContinueSearchResult();
}

std::optional<MacroPatch>
RefoldMacroStandardArgsOnlyPatchBuilder::BuildMacroInvocationPatchArgsOnly(
    const RefoldModel::MacroInvocation &m, const diffutils::Hunk &h,
    StringRef baseInvText) const {
  std::optional<InvocationActualLayout> recoveredActualLayout =
      RecoverInvocationActuals(m, baseInvText);
  if (!recoveredActualLayout)
    return std::nullopt;

  const ArgsOnlyPlanningContext planningCtx{m, h, baseInvText,
                                            *recoveredActualLayout};
  const ArrayRef<std::pair<size_t, size_t>> invArgRanges =
      recoveredActualLayout->rangePairs();
  const ArgsOnlyTemplateReplayContext argsOnlyTemplateCtx{m, baseInvText,
                                                          invArgRanges};

  // Definition-tape replay proves surfaces that ordinary non-empty occurrence
  // replay cannot see: empty actuals, zero-token formal slots, and __VA_OPT__
  // branch flips.  A miss is non-terminal and only declines this replay path.
  if (auto replayPatch =
          DefinitionTapeSolver().TryDefinitionTapeReplayArgsOnlyPatch(
              planningCtx.invocation, planningCtx.hunk,
              planningCtx.baseInvocationText,
              planningCtx.actualLayout.rangePairs()))
    return replayPatch;

  // The ordinary current-level template solver has higher priority than the
  // later paste/standard fallback phases, preserving the established admission
  // order for args-only candidates.
  if (auto templatePatch =
          TemplateSolver().TryTemplateSolvedArgsOnlyPatch(argsOnlyTemplateCtx))
    return templatePatch;

  // Paste-aware replay has two non-success outcomes: continue when the hunk was
  // not accepted by a paste-specialized proof, or reject when a touched paste
  // surface is proven invalid under the fail-closed paste contract.
  ArgsOnlyPatchAttempt pasteAttempt = BuildPasteAwareArgsOnlyPatch(planningCtx);
  if (pasteAttempt.disposition == ArgsOnlyPatchAttempt::Disposition::Accepted)
    return std::move(pasteAttempt.patch);
  if (pasteAttempt.disposition == ArgsOnlyPatchAttempt::Disposition::Reject)
    return std::nullopt;

  return BuildStandardArgsOnlyPatch(planningCtx);
}
} // namespace refold
} // namespace clang
