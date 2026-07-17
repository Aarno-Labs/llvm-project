//===--- RefoldMacroStandardArgsOnlyPatchBuilder.cpp ------------*- C++ -*-===//
//
// Implementation of the standard args-only patch builder.
//
// The builder owns the standard args-only ranking slot, including the pure
// paste-only fallback path that remains part of that slot after specialized
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

#include "core/RefoldLog.h"
#include "core/RefoldModel.h"
#include "edit/RefoldBInsertionLedger.h"
#include "macro/RefoldArgTextRecovery.h"
#include "macro/RefoldMacroGeneratedCalleeReplayEngine.h"
#include "macro/RefoldMacroGeneratedLeafReplayEngine.h"
#include "macro/RefoldMacroPatchProofCertifier.h"
#include "macro/RefoldMacroPlannerHelpers.h"
#include "macro/RefoldMacroReplay.h"
#include "macro/RefoldMacroTopology.h"
#include "macro/RefoldMacroTupleHelpers.h"
#include "proof/RefoldProofLattice.h"
#include "source/RefoldSourceMapper.h"
#include "source/RefoldToken.h"
#include "source/TokenTextHelpers.h"
#include "util/StringUtils.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringRef.h"

#include <algorithm>
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

RefoldMacroArgsOnlyTemplateSolver
RefoldMacroStandardArgsOnlyPatchBuilder::TemplateSolver() const {
  return RefoldMacroArgsOnlyTemplateSolver(
      {&deps_.model, deps_.aToks, deps_.bToks, deps_.bTokOff,
       &deps_.sourceMapper, &deps_.macroTopology, &deps_.proofLattice,
       &deps_.lexLang, deps_.strict});
}

RefoldMacroOccurrenceReplay
RefoldMacroStandardArgsOnlyPatchBuilder::OccurrenceReplay() const {
  return RefoldMacroOccurrenceReplay({deps_.aToks, deps_.bTokOff,
                                      &deps_.macroTopology, &deps_.sourceMapper,
                                      deps_.strict});
}

RefoldMacroPasteArgumentBuilder
RefoldMacroStandardArgsOnlyPatchBuilder::PasteArgumentBuilder() const {
  return RefoldMacroPasteArgumentBuilder(
      {&deps_.sourceMapper, deps_.aToks, deps_.bToks, &deps_.lexLang});
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


/// Return true when `definition` is the direct selector/tuple forwarder shape
/// whose replacement list is exactly two distinct formals: a generated-callee
/// selector followed by one tuple actual.  This helper is deliberately limited
/// to the same source-level shape as object-selector tuple replay, so ordinary
/// standard args-only replay is not restricted for unrelated macros.
bool isDirectSelectorTupleForwarder(
    const RefoldModel::MacroDirective &definition, uint32_t &selectorArgIdx,
    uint32_t &tupleArgIdx) {
  if (definition.subkind != "#define" || !definition.functionLike ||
      definition.replacementTokens.size() != 2)
    return false;

  const RefoldModel::MacroReplacementToken &selectorToken =
      definition.replacementTokens.front();
  const RefoldModel::MacroReplacementToken &tupleToken =
      definition.replacementTokens.back();
  if (selectorToken.kind != RefoldModel::MacroReplacementTokenKind::ParamRef ||
      tupleToken.kind != RefoldModel::MacroReplacementTokenKind::ParamRef ||
      !selectorToken.paramIndex || !tupleToken.paramIndex ||
      *selectorToken.paramIndex == *tupleToken.paramIndex)
    return false;

  selectorArgIdx = *selectorToken.paramIndex;
  tupleArgIdx = *tupleToken.paramIndex;
  return true;
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
  if (definition.subkind != "#define" || !definition.functionLike ||
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
/// variadic contribution while preserving the fixed contribution.  The new
/// variadic contribution must itself be exactly one lexer token so the rewrite
/// does not invent a multi-token paste payload or change call-site arity.
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

  if (newVariadicActual.empty() ||
      StringRef(newVariadicActual) == *oldVariadicActual ||
      !textIsSingleTokenSpelling(newVariadicActual, deps.lexLang) ||
      replacementIntroducesTopLevelComma(newVariadicActual, deps.lexLang))
    return std::nullopt;

  if (!invocation.invB || !invocation.invE)
    return std::nullopt;

  DenseMap<uint32_t, std::string> replacementByArgIdx;
  replacementByArgIdx[shape->variadicArgIdx] = std::move(newVariadicActual);

  std::optional<InvocationRewriteWithRange> rewrite =
      deps.buildInvocationRewriteWithRange(
          actualCtx, replacementByArgIdx,
          /*materializedRangeByArgIdx=*/nullptr);
  if (!rewrite || StringRef(rewrite->text).trim() == baseInvocationText.trim())
    return std::nullopt;

  MacroPatch patch{*invocation.invB, *invocation.invE, std::move(rewrite->text),
                   invocation.id};
  deps.proofCertifier.CertifyInvocationRewriteMaterializedOutputRange(
      patch, rewrite->materializedOutputByteStart,
      rewrite->materializedOutputByteEnd);
  deps.certifyMacroPatchWholeExpansionBRange(invocation, patch);

  MacroPatchProof proof = deps.proofLattice.MakeMacroPatchProof(
      MacroPatchProofKind::ArgsOnlyStandard,
      /*preservesInvocationStructure=*/true, invocation.id);
  WholeEnvelopeReplayWitness wholeEnvelopeWitness;
  wholeEnvelopeWitness.rootMacroId = invocation.id;
  wholeEnvelopeWitness.replayValidated = true;
  wholeEnvelopeWitness.definitionTapeReplayValidated = true;
  proof.wholeEnvelopeReplay = wholeEnvelopeWitness;
  deps.proofLattice.SetMacroPatchProof(patch, std::move(proof));

  // The producer did not provide a paste-span interval for the VA_OPT paste
  // token, so this path carries its own replay proof.  Mark the patch after the
  // primary proof is installed and resync so the canonical paste witness is
  // derived from this completed carrier.
  patch.pasteReplayValidated = true;
  deps.proofLattice.MacroPatchProofClassifier().SyncMacroPatchProofSummary(
      patch);
  return patch;
}

/// Return true when `child` is the generated function-like callee that consumes
/// the root selector formal and obtains its terminal actuals from the root tuple
/// formal.  Object-selector replay has already had first right of refusal; this
/// predicate only identifies the situation where ordinary args-only replay would
/// otherwise preserve a stale selector after an unhandled generated-callee body
/// edit.
bool isSelectorTupleGeneratedCalleeChild(
    const RefoldMacroStandardArgsOnlyPatchBuilder::Dependencies &deps,
    const RefoldModel::MacroInvocation &child, uint32_t selectorArgIdx,
    uint32_t tupleArgIdx) {
  if (child.subkind != "func" ||
      child.calleeOrigin.kind != MacroCalleeOriginKind::CallerParam ||
      !llvm::is_contained(child.calleeOrigin.callerParamIndices,
                          selectorArgIdx) ||
      child.argTupleRefs.empty())
    return false;

  const RefoldModel::MacroDirective *definition =
      getDefinitionDirectiveForInvocation(deps.model, child);
  if (!definition || !definition->functionLike)
    return false;

  bool sawTupleActualRef = false;
  for (ArrayRef<RefoldModel::TupleArgRef> refs : child.argTupleRefs) {
    if (refs.empty())
      return false;
    for (const RefoldModel::TupleArgRef &ref : refs) {
      if (ref.callerParamIndex != tupleArgIdx)
        return false;
      sawTupleActualRef = true;
    }
  }
  return sawTupleActualRef;
}

/// Return true if a token hunk inside the root whole-cover is outside the
/// tuple-formal occurrences that ordinary args-only replay can update.
///
/// For `f t` generated-callee roots, such a hunk is a callee-body or callee
/// identity effect, not a tuple payload edit.  If object-selector tuple replay
/// has already failed to prove a replacement selector for that effect, allowing
/// ordinary args-only replay to continue would preserve the old selector and
/// silently emit a source spelling that preprocesses to the wrong stream.
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

/// After higher-order selector/tuple replay declines, reject ordinary
/// args-only replay for selector-generated callees when the edited whole-cover
/// still contains a non-tuple hunk.
///
/// This is a narrow fail-closed soundness gate, not the broad hunk-coverage
/// guard that previously regressed unrelated tests.  It applies only to roots
/// shaped like `f t`, where `f` resolves to a generated function-like callee and
/// the child callee records tuple-element provenance from `t`.  In that shape,
/// an unhandled non-tuple hunk means no visible selector macro could reproduce
/// the edited generated-callee body.  The only sound result is therefore to let
/// the existing whole-cover fallback materialize the expansion.
bool shouldRejectOrdinaryArgsOnlyAfterSelectorTupleReplayMiss(
    const RefoldMacroStandardArgsOnlyPatchBuilder::Dependencies &deps,
    const RefoldModel::MacroInvocation &invocation,
    const RefoldModel::MacroDirective &rootDefinition,
    StringRef baseInvocationText,
    ArrayRef<std::pair<size_t, size_t>> invocationArgRanges,
    ArrayRef<RefoldModel::PPArgSpan> standardArgSpans) {
  uint32_t selectorArgIdx = 0;
  uint32_t tupleArgIdx = 0;
  if (!isDirectSelectorTupleForwarder(rootDefinition, selectorArgIdx,
                                      tupleArgIdx) ||
      selectorArgIdx >= invocationArgRanges.size() ||
      tupleArgIdx >= invocationArgRanges.size())
    return false;

  std::optional<StringRef> selectorText = sliceInvocationArgumentText(
      baseInvocationText, invocationArgRanges, selectorArgIdx);
  std::optional<StringRef> tupleText = sliceInvocationArgumentText(
      baseInvocationText, invocationArgRanges, tupleArgIdx);
  if (!selectorText || selectorText->empty() || !tupleText ||
      !tupleText->starts_with("(") || !tupleText->ends_with(")"))
    return false;

  uint32_t selectorAliasHops = 0;
  const RefoldModel::MacroDirective *oldCallee =
      deps.resolveFunctionLikeMacroThroughAliasesWithHops(*selectorText,
                                                          &selectorAliasHops);
  if (!oldCallee || !oldCallee->functionLike)
    return false;

  bool sawSelectorTupleGeneratedCallee = false;
  for (const RefoldModel::MacroInvocation *child :
       deps.macroTopology.MacroChildrenOf(invocation.id)) {
    if (child && isSelectorTupleGeneratedCalleeChild(
                     deps, *child, selectorArgIdx, tupleArgIdx)) {
      sawSelectorTupleGeneratedCallee = true;
      break;
    }
  }
  if (!sawSelectorTupleGeneratedCallee)
    return false;

  return hasWholeCoverHunkOutsideTupleArgument(deps, invocation, tupleArgIdx,
                                               standardArgSpans);
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
  if (!definition || definition->subkind != "#define" ||
      !definition->functionLike || definition->name != m.name ||
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

  // Higher-order generated replay remains in its historical ranking position
  // before ordinary occurrence collection.  The private probe owns only the
  // generated-callee / generated-leaf / tuple-generated-callee discovery
  // sequence and returns nullopt for a non-terminal miss.
  if (auto higherOrderGeneratedPatch =
          HigherOrderGeneratedReplayProbe(deps_).TryBuild(m, h, baseInvText,
                                                          invArgRanges))
    return higherOrderGeneratedPatch;

  if (const RefoldModel::MacroDirective *rootDefinition =
          getDefinitionDirectiveForInvocation(deps_.model, m)) {
    if (shouldRejectOrdinaryArgsOnlyAfterSelectorTupleReplayMiss(
            deps_, m, *rootDefinition, baseInvText, invArgRanges,
            standardArgSpans))
      return std::nullopt;
  }

  RefoldMacroOccurrenceReplay occurrenceReplay = OccurrenceReplay();
  std::optional<TouchedFormalHunkCollection> collectedTouchedFormalHunks =
      TouchedFormalHunkCollector(deps_, occurrenceReplay)
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
} // namespace refold
} // namespace clang
