//===--- RefoldMacroStandardArgsOnlyPatchBuilder.cpp ------------*- C++ -*-===//
//
// Implementation of the standard args-only patch builder.
//
// Planner-side helpers still owned by `RefoldMacroPatchPlanner` are reached
// through the std::function callbacks supplied in `Dependencies`.  The
// method deliberately retains its ~15 top-level lambdas because they hold
// deep capture state; extracting them would obscure the control flow
// without meaningfully reducing the top-level method's size.
//
//===----------------------------------------------------------------------===//

#include "macro/RefoldMacroStandardArgsOnlyPatchBuilder.h"

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
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/FormatVariadic.h"

#include <algorithm>
#include <functional>
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

/// Equality for token diff hunks.  Kept as a named helper so synthetic hunk
/// deduplication uses the same relation everywhere in the standard args-only
/// builder.
bool sameTokenHunk(const diffutils::Hunk &lhs, const diffutils::Hunk &rhs) {
  return lhs.aStart == rhs.aStart && lhs.aEnd == rhs.aEnd &&
         lhs.bStart == rhs.bStart && lhs.bEnd == rhs.bEnd;
}

/// Canonical ordering for token hunks before deterministic deduplication.
bool tokenHunkLess(const diffutils::Hunk &lhs, const diffutils::Hunk &rhs) {
  if (lhs.aStart != rhs.aStart)
    return lhs.aStart < rhs.aStart;
  if (lhs.aEnd != rhs.aEnd)
    return lhs.aEnd < rhs.aEnd;
  if (lhs.bStart != rhs.bStart)
    return lhs.bStart < rhs.bStart;
  return lhs.bEnd < rhs.bEnd;
}

/// Raw delimiter-balance summary used only by local recovery checks that have
/// already established that approximate byte-level balancing is sufficient.
struct DelimiterBalance {
  int paren = 0;
  int bracket = 0;
  int brace = 0;
};

DelimiterBalance computeDelimiterBalance(llvm::StringRef text) {
  DelimiterBalance balance;
  for (char c : text) {
    switch (c) {
    case '(':
      ++balance.paren;
      break;
    case ')':
      --balance.paren;
      break;
    case '[':
      ++balance.bracket;
      break;
    case ']':
      --balance.bracket;
      break;
    case '{':
      ++balance.brace;
      break;
    case '}':
      --balance.brace;
      break;
    default:
      break;
    }
  }
  return balance;
}

/// Boundary-lexed token carrier for parent-tuple generated-callee replay.
///
struct ParentTupleCalleeReplayTok {
  std::string spelling;
  size_t begin = 0;
  size_t end = 0;
};

/// Boundary-lexed token with byte offsets inside a replay text slice.
///
/// The standard args-only builder uses this local carrier for replay helpers
/// that need only a token spelling and its half-open byte range inside the
/// replay text being analyzed.
struct ReplayTok {
  std::string spelling;
  size_t begin = 0;
  size_t end = 0;
};

/// Compare a parent-tuple callee replay token subrange against expected
/// spellings.  The parent-tuple solver uses its own replay-token carrier, so
/// this helper is deliberately typed to that carrier instead of the generic
/// generated-callee replay token used by other local solvers.
bool replayTokenRangeSpellingsEqual(
    llvm::ArrayRef<ParentTupleCalleeReplayTok> toks, size_t begin,
    llvm::ArrayRef<std::string> expected) {
  if (begin + expected.size() > toks.size())
    return false;
  for (size_t i = 0; i < expected.size(); ++i)
    if (toks[begin + i].spelling != expected[i])
      return false;
  return true;
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

  // Standard (non-paste) args-only policy:
  // Collect arg-span occurrences (and stringify occurrences) and require the
  // entire hunk to be covered by those spans. Then derive per-arg replacements
  // from the B slices.
  // The immutable producer #define is resolved through a named planner helper.
  bool replayedStandardArgSpanFormalIndices = false;
  auto getDefinitionReplayedStandardArgSpans = [&]() {
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
        getDefinitionDirectiveForInvocation(deps_.model, m);
    if (!definition || definition->subkind != "#define" ||
        !definition->functionLike || definition->name != m.name ||
        definition->defParams.size() != m.defParams.size() || out.empty() ||
        !m.stringifySpans.empty() || !m.pasteSpans.empty() ||
        !m.cover.IsValid() || m.cover.end > deps_.aToks.size())
      return out;

    // Extract the formal-reference order from the macro replacement-list tape.
    // Stringify and paste are excluded because their spelling/segmentation
    // rules are not ordinary standard-argument substitution.
    SmallVector<uint32_t, 8> formalSeq;
    formalSeq.reserve(definition->replacementTokens.size());
    for (const RefoldModel::MacroReplacementToken &token :
         definition->replacementTokens) {
      if (token.spelling == "#" || token.spelling == "##")
        return out;
      if (token.kind != RefoldModel::MacroReplacementTokenKind::ParamRef)
        continue;
      if (!token.paramIndex || *token.paramIndex >= m.defParams.size())
        return out;
      formalSeq.push_back(*token.paramIndex);
    }

    if (formalSeq.size() != out.size())
      return out;

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
        if (tok >= m.cover.end || tok >= deps_.aToks.size() ||
            deps_.aToks[static_cast<size_t>(tok)].spelling != repTok.spelling)
          return m.argSpans;
        ++tok;
        break;
      case RefoldModel::MacroReplacementTokenKind::ParamRef: {
        if (argSpanIdx >= out.size())
          return m.argSpans;
        const RefoldModel::PPArgSpan &sp = out[argSpanIdx];
        if (sp.kind != PPArgSpanKind::Standard || sp.begin != tok ||
            sp.begin >= sp.end || sp.end > m.cover.end ||
            sp.end > deps_.aToks.size())
          return m.argSpans;
        tok = sp.end;
        ++argSpanIdx;
        break;
      }
      }
    }

    if (tok != m.cover.end || argSpanIdx != out.size())
      return m.argSpans;

    bool changed = false;
    for (size_t i = 0; i < out.size(); ++i) {
      if (out[i].argIdx != formalSeq[i])
        changed = true;
      out[i].argIdx = formalSeq[i];
    }
    if (changed) {
      replayedStandardArgSpanFormalIndices = true;
    }
    return out;
  };

  // Prefer the direct current-level invocation parse; fall back to definition
  // replay only for older producer shapes where the replacement-list tape
  // proves the same reindexing.
  std::vector<RefoldModel::PPArgSpan> standardArgSpans;
  if (auto currentLevelSpans = TemplateSolver().GetCurrentLevelStandardArgSpans(
          argsOnlyTemplateCtx)) {
    standardArgSpans = std::move(*currentLevelSpans);
  } else {
    standardArgSpans = getDefinitionReplayedStandardArgSpans();
  }

  std::vector<RefoldModel::PPArgSpan> occs;
  append_range(occs, standardArgSpans);
  append_range(occs, m.stringifySpans);

  std::vector<char> occIsStringify;
  occIsStringify.resize(occs.size());
  std::fill_n(occIsStringify.begin(), standardArgSpans.size(), false);
  std::fill_n(occIsStringify.begin() + standardArgSpans.size(),
              m.stringifySpans.size(), true);

  // Pure paste-only invocations have no STANDARD or STRINGIFY evidence, so the
  // normal args-only path bottoms out at occs.empty(). They are still
  // invertible when the touched pasted token can be segmented back into unique
  // per-argument replacements and those replacements reconstruct every pasted
  // token occurrence in B.
  if (occs.empty() && !m.pasteSpans.empty()) {

    // First derive per-argument paste edits from the current hunk. The
    // derivation proves that the edited pasted-token spelling can be
    // mapped back to argument segments rather than arbitrary token substrings.
    auto edits = PasteArgumentBuilder().DerivePasteArgEdits(m, hArgs);
    if (!edits || edits->empty()) {
      return std::nullopt;
    }

    // Merge all derived paste edits into one replacement spelling per
    // invocation argument. Multiple pasted-token occurrences may refer to the
    // same formal, but they must all demand the same final argument spelling.
    DenseMap<uint32_t, std::string> replByArgIdx;
    for (const auto &pae : *edits) {
      const uint32_t argIdx = pae.argIdx;
      if (static_cast<size_t>(argIdx) >= invArgRanges.size()) {
        return std::nullopt;
      }

      // Reconstruct the full invocation-argument spelling by replacing the
      // derived old paste segment with the derived new paste segment. Prefer
      // the exact byte-window splice when the paste witness identifies the
      // segment boundaries inside the argument.
      auto range = invArgRanges[argIdx];
      StringRef baseArgText =
          baseInvText.substr(range.first, range.second - range.first);
      std::string newArg =
          (pae.argByteBegin && pae.argByteEnd)
              ? RefoldMacroPasteSpelling::
                    SplicePasteSegmentIntoSpellingArgExact(
                        baseArgText, *pae.argByteBegin, *pae.argByteEnd,
                        pae.oldSeg, pae.newSeg)
              : RefoldMacroPasteSpelling::SplicePasteSegmentIntoSpellingArg(
                    baseArgText, pae.oldSeg, pae.newSeg);

      // An empty splice result normally means the old segment could not be
      // found or replaced safely. The one accepted empty-result case is a true
      // no-op where the new segment is empty and the original argument was
      // exactly the old segment after trimming.
      if (newArg.empty()) {
        if (!(StringRef(pae.newSeg).trim().empty() &&
              baseArgText.trim() == StringRef(pae.oldSeg).trim())) {
          return std::nullopt;
        }
      }

      // A non-variadic macro formal cannot be rewritten to text containing a
      // top-level comma, because that would change call-site arity.
      if (!isMacroInvocationVariadicFormal(m, argIdx) &&
          replacementIntroducesTopLevelComma(newArg, deps_.lexLang)) {
        return std::nullopt;
      }

      // If the same formal was observed through multiple pasted tokens, require
      // every occurrence to reconstruct the exact same replacement argument.
      auto existing = replByArgIdx.find(argIdx);
      if (existing != replByArgIdx.end()) {
        if (existing->second != newArg) {
          return std::nullopt;
        }
        continue;
      }

      replByArgIdx[argIdx] = std::move(newArg);
    }

    // No argument changed after merging, so there is no invocation rewrite to
    // propose from this fallback.
    if (replByArgIdx.empty()) {
      return std::nullopt;
    }

    // Validate the merged argument replacements globally against every pasted
    // token occurrence in B. This prevents accepting a rewrite that explains
    // only the touched token while breaking another paste occurrence from the
    // same invocation.
    if (!PasteArgumentBuilder().PasteArgReplacementsMatchAllPasteTokensInB(
            m, baseInvText, invArgRanges, replByArgIdx)) {
      return std::nullopt;
    }

    std::optional<InvocationRewriteWithRange> rewrite =
        deps_.buildInvocationRewriteWithRange(
            actualRecoveryCtx, replByArgIdx,
            /*materializedRangeByArgIdx=*/nullptr);
    if (!rewrite)
      return std::nullopt;

    {
      MacroPatch patch{*m.invB, *m.invE, std::move(rewrite->text), m.id};
      deps_.proofCertifier.CertifyInvocationRewriteMaterializedOutputRange(
          patch, rewrite->materializedOutputByteStart,
          rewrite->materializedOutputByteEnd);
      // Pure-paste-only replay has no standard/stringify occurrence to define
      // a smaller B-side surface.  The proved replay unit is the full expansion
      // cover reconstructed from the rewritten invocation arguments.
      deps_.certifyMacroPatchWholeExpansionBRange(m, patch);
      deps_.proofLattice.SetMacroPatchProof(
          patch, deps_.proofLattice.MakeMacroPatchProof(
                     MacroPatchProofKind::ArgsOnlyPurePasteOnly,
                     /*preservesInvocationStructure=*/true, m.id));
      // Pure-paste-only rewrites have no standard or stringify occurrences to
      // lean on, so successful all-paste replay is the decisive proof source.
      patch.pasteReplayValidated = true;
      deps_.proofLattice.MacroPatchProofClassifier().SyncMacroPatchProofSummary(
          patch);
      return patch;
    }
  }

  // Higher-order generated-callee replay for ordinary macro actual slots.
  //
  // The tuple-specific proof below handles wrappers such as `WRAP((STR, x))`,
  // where the source slots to repair are tuple elements inside one formal.  The
  // same owner-level invariant also applies when those slots are ordinary
  // invocation actuals:
  //
  //   #define APPLY(F, G, X) F(G, X)
  //   #define FWD(G, X) G(X)
  //   #define STR(x) #x
  //   APPLY(FWD, STR, alpha)
  //
  // The root expansion surface belongs to APPLY, but the edited token may be
  // exposed only after following one or more generated calls (`APPLY -> FWD ->
  // STR`).  This proof follows that generated-call chain through replacement
  // lists, replays the final callee replacement-list as a token transducer, and
  // maps the solved final callee actuals back to the original APPLY actual
  // ranges.  It is owner-polymorphic with respect to the source slots: no tuple
  // syntax is assumed, and every accepted edit is positional rather than
  // text-keyed.
  {
    // Higher-order generated-callee replay keeps the fail-closed discovery
    // order, then hands the explicit 6B context to the named builder.
    if (m.definitionDirectiveId && m.invB && m.invE &&
        m.stringifySpans.empty() && m.pasteSpans.empty()) {
      auto cover = RefoldMacroWholeCoverProof::GetWholeCoverATokRange(m);
      if (cover && cover->first < cover->second) {
        auto bEnv = deps_.sourceMapper.MapATokRangeAToBTokenEnvelope(
            cover->first, cover->second);
        if (!bEnv || bEnv->first >= bEnv->second)
          bEnv = deps_.sourceMapper
                     .MapATokRangeAToBTokenEnvelopePreserveBoundaryInsertions(
                         cover->first, cover->second);
        if (bEnv && bEnv->first < bEnv->second) {
          const RefoldModel::MacroDirective *rootDefinition = nullptr;
          for (const RefoldModel::MacroDirective &directive :
               deps_.model.GetMacroDirectives()) {
            if (directive.id == *m.definitionDirectiveId) {
              rootDefinition = &directive;
              break;
            }
          }
          if (rootDefinition && rootDefinition->subkind == "#define" &&
              rootDefinition->functionLike &&
              !rootDefinition->defParams.empty()) {
            SmallVector<GeneratedCalleeSourceSlot, 8> currentActuals;
            currentActuals.reserve(invArgRanges.size());
            bool actualsRecoverable = true;
            for (uint32_t i = 0; i < invArgRanges.size(); ++i) {
              const auto r = invArgRanges[i];
              if (r.second < r.first || r.second > baseInvText.size()) {
                actualsRecoverable = false;
                break;
              }
              GeneratedCalleeSourceSlot slot;
              slot.text = baseInvText.slice(r.first, r.second).trim().str();
              slot.rootSourceText = slot.text;
              slot.rootArgIdx = i;
              currentActuals.push_back(std::move(slot));
            }

            if (actualsRecoverable &&
                macroDefinitionAcceptsActualCount(*rootDefinition,
                                                  currentActuals.size())) {
              SmallVector<std::string, 8> replayPrefixLiterals;
              SmallVector<SmallVector<std::string, 8>, 8> replaySuffixStack;
              const RefoldModel::MacroDirective *currentDefinition =
                  rootDefinition;
              bool followedGeneratedCall = false;
              uint32_t generatedCallDepth = 0;
              uint32_t objectAliasHopCount = 0;
              bool generatedReplayUsesStringification = false;
              bool generatedReplayUsesPaste = false;
              bool generatedReplayUsesVariadicForwarding = false;

              GeneratedCalleeReplayContext generatedCalleeCtx{
                  m,
                  baseInvText,
                  invArgRanges,
                  *rootDefinition,
                  *cover,
                  *bEnv,
                  replayPrefixLiterals,
                  replaySuffixStack,
                  currentDefinition,
                  currentActuals,
                  followedGeneratedCall,
                  generatedCallDepth,
                  objectAliasHopCount,
                  generatedReplayUsesStringification,
                  generatedReplayUsesPaste,
                  generatedReplayUsesVariadicForwarding};

              if (auto higherOrderGeneratedPatch =
                      deps_.generatedCalleeReplayEngine
                          .BuildGeneratedCalleeReplayCandidate(
                              generatedCalleeCtx))
                return higherOrderGeneratedPatch;
            }
          }
        }
      }
    }
  }

  // Last-resort higher-order leaf replay for generated-call owners whose
  // argument expressions have not yet been lowered into the full transducer
  // graph above.  This is still a proof, not a preference: it is considered
  // only for a root macro whose replacement list generates a call through a
  // formal callee, and it accepts only a unique old->new leaf substitution that
  // can be mapped back into exactly one root invocation argument.  This covers
  // owner surfaces such as `F(G, A, B) -> G(A ## B)`, `G(#X)`, and repeated
  // generated calls `G(X) G(X)` without choosing among ambiguous edits.
  {
    // Generated-leaf replay keeps the definition/invocation discovery gates in
    // place and delegates only the solved replay/admission body to the named
    // helper.
    if (m.definitionDirectiveId && m.invB && m.invE) {
      const RefoldModel::MacroDirective *rootDefinition = nullptr;
      for (const RefoldModel::MacroDirective &directive :
           deps_.model.GetMacroDirectives()) {
        if (directive.id == *m.definitionDirectiveId) {
          rootDefinition = &directive;
          break;
        }
      }
      if (rootDefinition && rootDefinition->subkind == "#define" &&
          rootDefinition->functionLike && !rootDefinition->defParams.empty()) {
        bool hasGeneratedCall = false;
        const auto &rootToks = rootDefinition->replacementTokens;
        for (size_t i = 0; i + 1 < rootToks.size(); ++i) {
          if (rootToks[i].kind ==
                  RefoldModel::MacroReplacementTokenKind::ParamRef &&
              rootToks[i].paramIndex &&
              rootToks[i + 1].kind ==
                  RefoldModel::MacroReplacementTokenKind::Literal &&
              rootToks[i + 1].spelling == "(") {
            hasGeneratedCall = true;
            break;
          }
        }

        auto cover = RefoldMacroWholeCoverProof::GetWholeCoverATokRange(m);
        if (hasGeneratedCall && cover && cover->first < cover->second) {
          auto bEnv = deps_.sourceMapper.MapATokRangeAToBTokenEnvelope(
              cover->first, cover->second);
          if (!bEnv || bEnv->first >= bEnv->second)
            bEnv = deps_.sourceMapper
                       .MapATokRangeAToBTokenEnvelopePreserveBoundaryInsertions(
                           cover->first, cover->second);
          if (bEnv && bEnv->first < bEnv->second) {
            StringRef oldExpansion =
                deps_.sourceMapper.SliceASource(cover->first, cover->second)
                    .trim();
            StringRef newExpansion =
                deps_.sourceMapper.SliceBSource(bEnv->first, bEnv->second)
                    .trim();

            if (oldExpansion == newExpansion && h.isInsertOnly() &&
                h.aStart == cover->second && h.bStart == bEnv->second &&
                h.bStart < h.bEnd) {
              bEnv->second = static_cast<size_t>(h.bEnd);
              newExpansion =
                  deps_.sourceMapper.SliceBSource(bEnv->first, bEnv->second)
                      .trim();
            }

            bool rootHasGeneratedSelectorDescendant = false;
            for (const RefoldModel::MacroInvocation &candidate :
                 deps_.model.GetMacroInvocations()) {
              const RefoldModel::MacroInvocation *cur = &candidate;
              bool isDescendant = false;
              for (size_t depth = 0;
                   cur && depth <= deps_.model.GetMacroInvocations().size();
                   ++depth) {
                if (cur->id == m.id) {
                  isDescendant = true;
                  break;
                }
                if (!cur->callerMacroId)
                  break;
                cur = deps_.macroTopology.FindMacroInvocationById(
                    *cur->callerMacroId);
              }
              if (!isDescendant)
                continue;
              if (candidate.calleeOrigin.kind ==
                      MacroCalleeOriginKind::CallerParam &&
                  !candidate.calleeOrigin.callerParamIndices.empty()) {
                rootHasGeneratedSelectorDescendant = true;
                break;
              }
            }

            if (h.isInsertOnly() && rootHasGeneratedSelectorDescendant &&
                (h.aStart == cover->first || h.aStart == cover->second)) {
              while (bEnv->first > 0 &&
                     bEnv->first - 1 <
                         deps_.bInsertionLedger.BTokToInsertionId().size()) {
                int32_t insId =
                    deps_.bInsertionLedger.BTokToInsertionId()[bEnv->first - 1];
                if (insId < 0)
                  break;
                const BInsertionProv &ins =
                    deps_.bInsertionLedger
                        .Insertions()[static_cast<size_t>(insId)];
                if (ins.claim == BInsertionClaim::Standalone ||
                    ins.aGap != cover->first || ins.b1 != bEnv->first)
                  break;
                bEnv->first = ins.b0;
              }
              while (bEnv->second <
                     deps_.bInsertionLedger.BTokToInsertionId().size()) {
                int32_t insId =
                    deps_.bInsertionLedger.BTokToInsertionId()[bEnv->second];
                if (insId < 0)
                  break;
                const BInsertionProv &ins =
                    deps_.bInsertionLedger
                        .Insertions()[static_cast<size_t>(insId)];
                if (ins.claim == BInsertionClaim::Standalone ||
                    ins.aGap != cover->second || ins.b0 != bEnv->second)
                  break;
                bEnv->second = ins.b1;
              }
              newExpansion =
                  deps_.sourceMapper.SliceBSource(bEnv->first, bEnv->second)
                      .trim();
            }

            if (!oldExpansion.empty() && !newExpansion.empty() &&
                oldExpansion != newExpansion) {
              GeneratedLeafReplayContext generatedLeafCtx{
                  m,           h,     baseInvText,     invArgRanges,
                  *cover,      *bEnv, *rootDefinition, oldExpansion,
                  newExpansion};
              if (auto higherOrderLeafPatch =
                      deps_.generatedLeafReplayEngine
                          .BuildGeneratedLeafReplayCandidate(generatedLeafCtx))
                return higherOrderLeafPatch;
            }
          }
        }
      }
    }
  }

  // Some tuple-generated callees expose the edited token only through a
  // nested child macro surface, so the root wrapper can have no standard
  // PPArgSpan/stringify occurrence at all.  That happens for shapes such as
  //
  //   #define WRAP(PAIR) CALL PAIR
  //   #define CALL(F, X) F(X)
  //   #define STR(x) #x
  //   WRAP((STR, alpha))
  //
  // where the root WRAP cover is the string literal produced by STR, not a
  // direct argument span of WRAP.  Try a narrow positional tuple proof before
  // giving up on an empty occurrence set: the wrapper must forward exactly one
  // tuple formal into a generated callee call, and the callee replacement list
  // must be invertible for the observed whole-cover A/B expansion.
  {
    // Tuple-generated callee replay remains after ordinary generated-leaf
    // replay in the same ranking position; only the body is now a named helper.
    if (m.definitionDirectiveId && m.invB && m.invE &&
        m.stringifySpans.empty() && m.pasteSpans.empty()) {
      auto cover = RefoldMacroWholeCoverProof::GetWholeCoverATokRange(m);
      if (cover && cover->first < cover->second) {
        auto bEnv = deps_.sourceMapper.MapATokRangeAToBTokenEnvelope(
            cover->first, cover->second);
        if (!bEnv || bEnv->first >= bEnv->second)
          bEnv = deps_.sourceMapper
                     .MapATokRangeAToBTokenEnvelopePreserveBoundaryInsertions(
                         cover->first, cover->second);
        if (bEnv && bEnv->first < bEnv->second) {
          const RefoldModel::MacroDirective *rootDefinition = nullptr;
          for (const RefoldModel::MacroDirective &directive :
               deps_.model.GetMacroDirectives()) {
            if (directive.id == *m.definitionDirectiveId) {
              rootDefinition = &directive;
              break;
            }
          }
          if (rootDefinition && rootDefinition->subkind == "#define" &&
              rootDefinition->functionLike &&
              rootDefinition->replacementTokens.size() == 2) {
            const auto &rootTok0 = rootDefinition->replacementTokens[0];
            const auto &rootTok1 = rootDefinition->replacementTokens[1];
            if (rootTok0.kind ==
                    RefoldModel::MacroReplacementTokenKind::Literal &&
                rootTok1.kind ==
                    RefoldModel::MacroReplacementTokenKind::ParamRef &&
                rootTok1.paramIndex &&
                *rootTok1.paramIndex < invArgRanges.size()) {
              const uint32_t callerArgIdx = *rootTok1.paramIndex;
              uint32_t tupleObjectAliasHopCount = 0;
              uint32_t forwarderAliasHops = 0;
              const RefoldModel::MacroDirective *forwarderDefinition =
                  deps_.resolveFunctionLikeMacroThroughAliasesWithHops(
                      rootTok0.spelling, &forwarderAliasHops);
              tupleObjectAliasHopCount += forwarderAliasHops;
              if (forwarderDefinition &&
                  !forwarderDefinition->defParams.empty()) {
                TupleGeneratedCalleeReplayContext tupleGeneratedCtx{
                    m,
                    baseInvText,
                    invArgRanges,
                    *cover,
                    *bEnv,
                    *rootDefinition,
                    *forwarderDefinition,
                    callerArgIdx,
                    tupleObjectAliasHopCount};
                if (auto tupleGeneratedPatch =
                        deps_.generatedCalleeReplayEngine
                            .BuildTupleGeneratedCalleeReplayCandidate(
                                tupleGeneratedCtx))
                  return tupleGeneratedPatch;
              }
            }
          }
        }
      }
    }
  }

  if (occs.empty())
    return std::nullopt;

  std::vector<char> touchedOcc(occs.size(), 0);
  if (!deps_.sourceMapper.HunkFullyWithinArgSpans(hArgs, occs, touchedOcc)) {
    return std::nullopt;
  }

  // Convert touched occurrence spans into touched formal arguments. The
  // args-only path is valid only for edits fully contained inside recorded
  // argument occurrences; anything outside those spans must fail closed.
  unsigned occFormalCount = static_cast<unsigned>(invArgRanges.size());
  for (const auto &sp : occs)
    occFormalCount = std::max(occFormalCount, (unsigned)sp.argIdx + 1);

  std::vector<char> touched(occFormalCount, 0);
  for (size_t i = 0; i < occs.size(); ++i) {
    if (!touchedOcc[i])
      continue;
    const auto &sp = occs[i];
    if (sp.argIdx >= touched.size())
      return std::nullopt;
    touched[sp.argIdx] = 1;
  }

  // `touched` is now indexed by formal argument, not occurrence. Later checks
  // use it to decide which invocation arguments need replacement and which must
  // remain unchanged.

  auto hunkTouchesFormalOccurrence =
      [&](const diffutils::Hunk &cand,
          const RefoldModel::PPArgSpan &sp) -> bool {
    // For replacements/deletions, touching is ordinary A-range overlap. For
    // pure insertions, the hunk has no A width, so require the existing
    // argument-span ownership helper to prove that the B insertion belongs to
    // this occurrence.
    if (cand.aStart == cand.aEnd) {
      auto bEnv = deps_.sourceMapper.MapAToBTokenEnvelopeByPPArgSpan(sp);
      if (!bEnv)
        return false;
      return OccurrenceReplay()
          .GetOwnedPureInsertionBRangeForArgSpan(sp, occs, *bEnv, cand)
          .has_value();
    }

    return cand.aStart < sp.end && cand.aEnd > sp.begin;
  };

  // A provenance-only LCS can split one logical macro-argument rewrite into
  // several pure-insertion islands around repeated punctuation. When the
  // current seed is a pure insertion, widen the set of touched formals to every
  // occurrence in this same invocation cover that is independently touched by
  // another token hunk. This does not move hunk boundaries and does not inspect
  // neighboring token spellings; it only lets the existing replay validator see
  // the complete split edit before deciding whether an args-only rewrite is
  // actually proven.
  if (hArgs.aStart == hArgs.aEnd && hArgs.bStart < hArgs.bEnd) {
    for (const diffutils::Hunk &cand : deps_.abTokHunks) {
      if (cand.aStart < m.cover.begin || cand.aEnd > m.cover.end)
        continue;
      if (cand.bStart >= cand.bEnd)
        continue;

      for (const auto &sp : occs) {
        if (sp.argIdx >= touched.size())
          return std::nullopt;
        if (hunkTouchesFormalOccurrence(cand, sp))
          touched[sp.argIdx] = 1;
      }
    }
  }

  auto hunkTouchesTouchedFormal = [&](const diffutils::Hunk &cand) -> bool {
    // Keep only hunks that intersect an occurrence of a formal already touched
    // by the primary args-only hunk. This lets the rewrite validate all edits
    // to the same formal argument, not just the hunk that triggered this path.
    for (const auto &sp : occs) {
      if (sp.argIdx >= touched.size() || !touched[sp.argIdx])
        continue;
      if (hunkTouchesFormalOccurrence(cand, sp))
        return true;
    }
    return false;
  };

  SmallVector<diffutils::Hunk, 8> tokenHunksForTouchedFormals;
  tokenHunksForTouchedFormals.push_back(hArgs);

  // Add sibling token hunks that also touch the same formal arguments. The
  // eventual argument replacement must explain the complete set of token edits
  // for those formals, otherwise we could accept a partial rewrite.
  for (const auto &cand : deps_.abTokHunks) {
    if (cand.aStart == hArgs.aStart && cand.aEnd == hArgs.aEnd &&
        cand.bStart == hArgs.bStart && cand.bEnd == hArgs.bEnd)
      continue;
    if (!hunkTouchesTouchedFormal(cand))
      continue;
    tokenHunksForTouchedFormals.push_back(cand);
  }

  auto maybeAddSyntheticTouchedFormalEnvelope =
      [&](const RefoldModel::PPArgSpan &sp, const diffutils::Hunk &anchor) {
        // This synthesis is only for insertion-frontier hunks. Non-insertion
        // hunks already carry an A-side interval and do not need
        // reconstruction.
        if (anchor.aStart != anchor.aEnd)
          return;
        if (anchor.aStart < m.cover.begin || anchor.aEnd > m.cover.end)
          return;

        for (const auto &partner : deps_.abTokHunks) {
          if (sameTokenHunk(anchor, partner))
            continue;

          // Pair the anchor only with another insertion frontier from the same
          // macro cover. Their combined envelope may reveal the true touched
          // argument span after common edge tokens are trimmed.
          if (partner.aStart != partner.aEnd)
            continue;
          if (partner.aStart < m.cover.begin || partner.aEnd > m.cover.end)
            continue;

          const diffutils::Hunk env =
              buildCombinedInsertionEnvelope(anchor, partner);
          const diffutils::Hunk envTrim =
              trimCommonEdgeTokens(env, deps_.aToks, deps_.bToks);

          // The trimmed synthetic envelope must expose a real A-side token
          // range.
          if (envTrim.aStart >= envTrim.aEnd)
            continue;

          // The exposed range must be fully contained in the exact occurrence
          // currently being considered.
          if (!(sp.begin <= envTrim.aStart && envTrim.aEnd <= sp.end))
            continue;

          SmallVector<char, 8> envTouched(occs.size(), 0);
          if (!deps_.sourceMapper.HunkFullyWithinArgSpans(envTrim, occs,
                                                          envTouched))
            continue;

          bool touchesThisExactOccurrence = false;
          for (size_t occIdx = 0; occIdx < occs.size(); ++occIdx) {
            if (!envTouched[occIdx])
              continue;

            // Fail closed if the synthetic envelope touches any other formal or
            // any other occurrence. It is valid only as an explanation for
            // this exact argument occurrence.
            if (occs[occIdx].argIdx != sp.argIdx)
              return;
            if (occs[occIdx].begin != sp.begin || occs[occIdx].end != sp.end)
              return;

            touchesThisExactOccurrence = true;
          }
          if (!touchesThisExactOccurrence)
            continue;

          // Add the proof-compatible synthetic hunk so downstream args-only
          // replacement logic validates the complete touched formal edit.
          tokenHunksForTouchedFormals.push_back(envTrim);
        }
      };

  // Iterate over a snapshot of the currently known hunks. Synthetic-envelope
  // discovery may append to `tokenHunksForTouchedFormals`, so the seed copy
  // avoids recursively pairing newly synthesized hunks in the same pass.
  SmallVector<diffutils::Hunk, 8> seedTokenHunks(
      tokenHunksForTouchedFormals.begin(), tokenHunksForTouchedFormals.end());

  for (size_t occIdx = 0; occIdx < occs.size(); ++occIdx) {
    const auto &sp = occs[occIdx];
    if (sp.argIdx >= touched.size() || !touched[sp.argIdx])
      continue;

    for (const auto &cand : seedTokenHunks) {
      // Only zero-width A-side insertion frontiers can participate in synthetic
      // envelope construction. Non-insertion hunks already expose their A
      // range.
      if (cand.aStart != cand.aEnd)
        continue;
      maybeAddSyntheticTouchedFormalEnvelope(sp, cand);
    }
  }

  // Normalize the hunk set after adding synthetic envelopes. This prevents the
  // same physical edit from being observed multiple times through equivalent
  // primary/synthetic paths.
  llvm::sort(tokenHunksForTouchedFormals, tokenHunkLess);
  tokenHunksForTouchedFormals.erase(
      std::unique(tokenHunksForTouchedFormals.begin(),
                  tokenHunksForTouchedFormals.end(), sameTokenHunk),
      tokenHunksForTouchedFormals.end());

  // From this point on, treat the normalized vector as the authoritative token
  // hunk set for the touched formal arguments.
  ArrayRef<diffutils::Hunk> tokenHunks(tokenHunksForTouchedFormals);

  struct OccObservation {
    // The original spelling contributed by one occurrence of a formal argument.
    StringRef oldText;

    // The rewritten spelling inferred for that same occurrence from the token
    // hunk set.
    std::string newText;

    // Optional byte range inside newText that corresponds exactly to B-only
    // insertion payload owned by this occurrence. When absent, the whole
    // rewritten argument remains the conservative materialized output range.
    std::optional<std::pair<uint64_t, uint64_t>> materializedNewTextRange;
  };

  // Try to rebuild a caller tuple argument from occurrence observations that
  // were seen through a child macro invocation. This accepts only two certified
  // forwarding shapes: direct tuple-ref metadata, or a variadic identity-
  // forward wrapper where the child preserves the caller tuple as one full-
  // width argument.
  auto tryTupleForwardedCallerTupleRewrite =
      [&](uint32_t callerArgIdx, StringRef baseArgText,
          ArrayRef<OccObservation> occObservations,
          std::string &outNewArg) -> bool {
    /// Tuple rewrite modes are ordered from strongest proof to weakest.
    ///
    /// DirectTupleRefs uses producer-supplied tuple element byte ranges in
    /// the normalized child invocation. VariadicIdentityForward is the
    /// fallback for variadic forwarding wrappers whose immediate child
    /// keeps the caller's variadic tuple intact as a single full-width
    /// forwarded argument (for example `__VA_ARGS__`).
    enum class TupleRewriteMode {
      None,
      DirectTupleRefs,
      VariadicIdentityForward,
    };

    StringRef parentTrim = baseArgText.trim();

    // There is no caller tuple to rewrite if the parent argument is empty.
    if (parentTrim.empty())
      return false;

    auto getNormalizedArgText =
        [&](const RefoldModel::MacroInvocation &inv,
            uint32_t argIdx) -> std::optional<StringRef> {
      // Return a child argument slice from normalized invocation text. This is
      // used by tuple-ref mode because tuple refs are expressed over normalized
      // child argument text/ranges.
      if (!inv.normalizedInvText)
        return std::nullopt;
      if (argIdx >= inv.normalizedInvArgTextRanges.size())
        return std::nullopt;
      const auto &rng = inv.normalizedInvArgTextRanges[argIdx];
      if (!rng.first || !rng.second || *rng.second < *rng.first)
        return std::nullopt;
      if (*rng.second > inv.normalizedInvText->size())
        return std::nullopt;
      return StringRef(*inv.normalizedInvText)
          .slice((size_t)*rng.first, (size_t)*rng.second)
          .trim();
    };

    auto getInvocationArgText =
        [&](const RefoldModel::MacroInvocation &inv,
            uint32_t argIdx) -> std::optional<StringRef> {
      // Return a child argument slice from the raw invocation spelling. This
      // is used by identity-forward mode, where the proof comes from raw
      // arg-ref byte coverage rather than tuple-ref metadata.
      if (!inv.invText || !inv.invB)
        return std::nullopt;
      if (argIdx >= inv.invArgRanges.size())
        return std::nullopt;
      const auto &rng = inv.invArgRanges[argIdx];
      if (!rng.first || !rng.second || *rng.second < *rng.first ||
          *rng.first < *inv.invB)
        return std::nullopt;
      const uint64_t relB = *rng.first - *inv.invB;
      const uint64_t relE = *rng.second - *inv.invB;
      if (relE < relB || relE > inv.invText->size())
        return std::nullopt;
      return StringRef(*inv.invText).slice((size_t)relB, (size_t)relE).trim();
    };

    /// Resolve a macro name through a deterministic object-like alias chain to
    /// a unique function-like definition.
    ///
    /// Tuple-generated-callee proofs use this for two different
    /// source-preserving cases: the root wrapper can name the forwarding macro
    /// through an alias
    /// (`CALL_ALIAS PAIR`), and the tuple's callee element can itself be an
    /// alias chain (`FSEL1 -> FSEL2 -> ADD_ONE`).  The alias is used only as
    /// proof evidence; the tuple source spelling is never replaced by the
    /// resolved name.  Each hop must be a unique object-like #define with
    /// exactly one literal replacement token, and the walk is bounded by the
    /// directive table so cycles or ambiguous macro-state histories fail
    /// closed.
    auto resolveFunctionLikeMacroThroughObjectAliases =
        [&](StringRef startName) -> const RefoldModel::MacroDirective * {
      if (startName.empty())
        return nullptr;

      SmallVector<std::string, 8> seen;
      std::string current = startName.str();
      for (size_t depth = 0; depth <= deps_.model.GetMacroDirectives().size();
           ++depth) {
        if (llvm::is_contained(seen, current))
          return nullptr;
        seen.push_back(current);

        const RefoldModel::MacroDirective *functionLike = nullptr;
        const RefoldModel::MacroDirective *alias = nullptr;
        for (const RefoldModel::MacroDirective &directive :
             deps_.model.GetMacroDirectives()) {
          if (directive.subkind != "#define" ||
              directive.name != StringRef(current))
            continue;
          if (directive.functionLike) {
            if (functionLike)
              return nullptr;
            functionLike = &directive;
            continue;
          }
          if (directive.replacementTokens.size() == 1 &&
              directive.replacementTokens[0].kind ==
                  RefoldModel::MacroReplacementTokenKind::Literal) {
            if (alias)
              return nullptr;
            alias = &directive;
          }
        }

        if (functionLike)
          return functionLike;
        if (!alias)
          return nullptr;
        current = alias->replacementTokens[0].spelling.str();
      }
      return nullptr;
    };

    auto tryParentTupleGeneratedCalleeRewrite = [&]() -> bool {
      // Handle the tuple-generated-callee case before the generic text-keyed
      // tuple rewrite.  In shapes such as
      //
      //   #define WRAP(PAIR) CALL PAIR
      //   #define CALL(F, X) F(X)
      //   WRAP((ADD_ONE, 10))
      //
      // the parent argument occurrence in PP output is the callee expansion
      // `((10) + 1)`, not the tuple element `10` by itself.  A whole-argument
      // replacement would therefore validate but collapse the tuple to
      // `WRAP(((20) + 1))`.  This proof reconstructs the generated call
      // positionally: tuple element 0 supplies the callee, later tuple elements
      // supply the generated actuals, and the callee replacement list is
      // replayed to determine which tuple slot changed.
      if (!m.definitionDirectiveId)
        return false;

      const RefoldModel::MacroDirective *rootDefinition = nullptr;
      for (const RefoldModel::MacroDirective &directive :
           deps_.model.GetMacroDirectives()) {
        if (directive.id == *m.definitionDirectiveId) {
          rootDefinition = &directive;
          break;
        }
      }
      if (!rootDefinition || rootDefinition->subkind != "#define" ||
          !rootDefinition->functionLike)
        return false;

      // This fallback proves the common tuple-wrapper shape directly from the
      // parent definition: the wrapper replacement is a literal forwarding
      // macro followed by exactly the caller tuple formal, e.g. `CALL PAIR`.
      // Keeping the shape this narrow prevents the proof from guessing about
      // arbitrary wrapper bodies.
      if (rootDefinition->replacementTokens.size() != 2)
        return false;
      const auto &rootTok0 = rootDefinition->replacementTokens[0];
      const auto &rootTok1 = rootDefinition->replacementTokens[1];
      if (rootTok0.kind != RefoldModel::MacroReplacementTokenKind::Literal ||
          rootTok1.kind != RefoldModel::MacroReplacementTokenKind::ParamRef ||
          !rootTok1.paramIndex || *rootTok1.paramIndex != callerArgIdx)
        return false;
      const StringRef forwarderName = rootTok0.spelling;

      const RefoldModel::MacroDirective *forwarderDefinition =
          resolveFunctionLikeMacroThroughObjectAliases(forwarderName);
      if (!forwarderDefinition || forwarderDefinition->defParams.empty())
        return false;

      // The forwarded caller argument must be a real parenthesized tuple.  The
      // byte ranges returned by the splitter are relative to the tuple payload
      // between the outer parentheses; edits are later applied to that payload
      // and wrapped back in the original tuple parens.
      if (!parentTrim.starts_with("(") || !parentTrim.ends_with(")") ||
          parentTrim.size() < 2)
        return false;
      StringRef tuplePayload = parentTrim.drop_front().drop_back();
      SmallVector<TupleElementSlice, 8> tupleElems;
      if (!splitTopLevelTupleElementsWithLexer(tuplePayload, deps_.lexLang,
                                               tupleElems))
        return false;
      if (tupleElems.size() < 2)
        return false;

      // A generated-argument reference maps one forwarder formal to the callee
      // actual list.  For a fixed formal this is a one-tuple-element mapping;
      // for a variadic forwarder formal it represents the entire remaining
      // tuple tail, which is why later edits must be positional rather than
      // keyed by old text.
      struct GeneratedArgRef {
        uint32_t forwarderParamIdx = 0;
        bool variadicPack = false;
      };

      const auto &forwarderToks = forwarderDefinition->replacementTokens;
      if (forwarderToks.size() < 4)
        return false;
      if (forwarderToks[0].kind !=
              RefoldModel::MacroReplacementTokenKind::ParamRef ||
          !forwarderToks[0].paramIndex)
        return false;
      const uint32_t calleeForwarderParam = *forwarderToks[0].paramIndex;
      if (calleeForwarderParam >= forwarderDefinition->defParams.size() ||
          calleeForwarderParam >= tupleElems.size())
        return false;
      if (forwarderToks[1].kind !=
              RefoldModel::MacroReplacementTokenKind::Literal ||
          forwarderToks[1].spelling != "(" ||
          forwarderToks.back().kind !=
              RefoldModel::MacroReplacementTokenKind::Literal ||
          forwarderToks.back().spelling != ")")
        return false;

      // Accept only a direct generated-call replacement list:
      //   calleeFormal '(' generatedActualFormals... ')'
      // The callee formal itself is not rewritten here; it is resolved to a
      // function-like macro definition below, while the remaining formals
      // become positional tuple-edit targets.
      SmallVector<GeneratedArgRef, 8> generatedArgs;
      for (size_t i = 2, e = forwarderToks.size() - 1; i < e; ++i) {
        const auto &tok = forwarderToks[i];
        if (tok.spelling == "#" || tok.spelling == "##" ||
            tok.spelling == "__VA_OPT__")
          return false;
        if (tok.kind != RefoldModel::MacroReplacementTokenKind::ParamRef)
          continue;
        if (!tok.paramIndex ||
            *tok.paramIndex >= forwarderDefinition->defParams.size())
          return false;
        if (*tok.paramIndex == calleeForwarderParam)
          return false;
        GeneratedArgRef ref;
        ref.forwarderParamIdx = *tok.paramIndex;
        ref.variadicPack =
            forwarderDefinition->defParams[*tok.paramIndex].variadic;
        generatedArgs.push_back(ref);
      }
      if (generatedArgs.empty())
        return false;

      auto tupleElementText = [&](size_t elemIdx) -> StringRef {
        const TupleElementSlice &elem = tupleElems[elemIdx];
        return tuplePayload.slice(elem.trimBegin, elem.trimEnd).trim();
      };

      auto resolveFunctionLikeCallee =
          [&](StringRef calleeSpelling) -> const RefoldModel::MacroDirective * {
        // Resolve the tuple's callee element through the same exact alias proof
        // used for the forwarding macro.  This admits chains such as
        // `FSEL1 -> FSEL2 -> ADD_ONE` while preserving the original tuple
        // spelling (`FSEL1`) in the reconstructed source.
        return resolveFunctionLikeMacroThroughObjectAliases(calleeSpelling);
      };

      const StringRef calleeSourceText =
          tupleElementText(static_cast<size_t>(calleeForwarderParam));
      const RefoldModel::MacroDirective *calleeDefinition =
          resolveFunctionLikeCallee(calleeSourceText);
      if (!calleeDefinition || calleeDefinition->defParams.empty())
        return false;

      // Read the generated actuals from the original tuple spelling.  This is
      // positional, so duplicate spellings like `(ADD2, 10, 10)` remain
      // distinguishable by tuple slot even though their text is identical.
      SmallVector<StringRef, 8> oldGeneratedActualPieces;
      for (const GeneratedArgRef &ref : generatedArgs) {
        if (ref.variadicPack) {
          if (ref.forwarderParamIdx >= tupleElems.size())
            return false;
          for (size_t i = ref.forwarderParamIdx; i < tupleElems.size(); ++i)
            oldGeneratedActualPieces.push_back(tupleElementText(i));
          continue;
        }
        if (ref.forwarderParamIdx >= tupleElems.size())
          return false;
        oldGeneratedActualPieces.push_back(
            tupleElementText(ref.forwarderParamIdx));
      }

      const bool calleeHasVariadic =
          !calleeDefinition->defParams.empty() &&
          calleeDefinition->defParams.back().variadic;
      const size_t fixedCalleeActuals =
          calleeHasVariadic ? calleeDefinition->defParams.size() - 1
                            : calleeDefinition->defParams.size();
      if ((!calleeHasVariadic && oldGeneratedActualPieces.size() !=
                                     calleeDefinition->defParams.size()) ||
          (calleeHasVariadic &&
           oldGeneratedActualPieces.size() < fixedCalleeActuals))
        return false;

      // Repackage the tuple pieces as the actual list of the generated callee.
      // For a variadic callee, all generated tail pieces are joined into the
      // one variadic formal spelling; the inverse mapping back to tuple
      // elements is performed after solving the new expansion.
      SmallVector<std::string, 8> oldActuals;
      oldActuals.reserve(calleeDefinition->defParams.size());
      for (size_t i = 0; i < fixedCalleeActuals; ++i)
        oldActuals.push_back(oldGeneratedActualPieces[i].str());
      if (calleeHasVariadic) {
        std::string variadicText;
        raw_string_ostream os(variadicText);
        for (size_t i = fixedCalleeActuals; i < oldGeneratedActualPieces.size();
             ++i) {
          if (i != fixedCalleeActuals)
            os << ", ";
          os << oldGeneratedActualPieces[i].trim();
        }
        os.flush();
        oldActuals.push_back(std::move(variadicText));
      }
      if (oldActuals.size() != calleeDefinition->defParams.size())
        return false;

      // Token view used for callee replay.  The spelling sequence proves the
      // replacement-list grammar; the byte offsets let a solved token interval
      // be converted back to exact text for a tuple slot.
      auto lexReplayTokens = [&](StringRef text,
                                 SmallVectorImpl<ReplayTok> &out) {
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

      // Normalized callee replacement-list tree.  This mirrors ReplayElem above
      // but is local to the generated callee: literals must match exactly,
      // params become solved callee actual ranges, and VA_OPT recursively
      // models the erased/exposed payload.
      enum class CalleeReplayKind { Literal, Param, VaOpt, Stringify, Paste };
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

      // Parse the callee replacement list into a replay tree.  Ordinary
      // literal/param/VA_OPT replay handles generated callees like
      // `ADD_ONE(x)`, while the Stringify/Paste nodes below cover the two
      // non-injective macro operators only when the observed old and new
      // expansion text make the inverse mapping unique.  The operators remain
      // local to this generated callee; the final tuple edit is still
      // positional over the caller tuple.
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
            CalleeReplayElem elem;
            elem.kind = CalleeReplayKind::Stringify;
            elem.paramIdx = paramIdx;
            out.push_back(std::move(elem));
            i += 2;
            continue;
          }

          if (i + 1 < end &&
              calleeDefinition->replacementTokens[i + 1].spelling == "##") {
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
          if (tok.spelling == "__VA_OPT__") {
            if (i + 1 >= end ||
                calleeDefinition->replacementTokens[i + 1].kind !=
                    RefoldModel::MacroReplacementTokenKind::Literal ||
                calleeDefinition->replacementTokens[i + 1].spelling != "(")
              return false;
            unsigned depth = 1;
            size_t j = i + 2;
            for (; j < end; ++j) {
              const auto &inner = calleeDefinition->replacementTokens[j];
              if (inner.kind != RefoldModel::MacroReplacementTokenKind::Literal)
                continue;
              if (inner.spelling == "(") {
                ++depth;
                continue;
              }
              if (inner.spelling == ")") {
                if (--depth == 0)
                  break;
              }
            }
            if (depth != 0 || j >= end)
              return false;
            CalleeReplayElem elem;
            elem.kind = CalleeReplayKind::VaOpt;
            if (!parseCalleeReplayRange(i + 2, j, elem.children))
              return false;
            out.push_back(std::move(elem));
            i = j + 1;
            continue;
          }
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
        return false;

      SmallVector<SmallVector<std::string, 16>, 8> oldActualTokSpellings;
      for (const std::string &actual : oldActuals)
        oldActualTokSpellings.push_back(tokenSpellingsForText(actual));
      auto singleTokenSpelling =
          [&](StringRef text) -> std::optional<std::string> {
        SmallVector<ReplayTok, 4> toks;
        lexReplayTokens(text, toks);
        if (toks.size() != 1)
          return std::nullopt;
        return toks.front().spelling;
      };

      auto rewriteTupleElementFromSolvedExpansion =
          [&](size_t elemIdx, StringRef oldExpansion,
              StringRef newExpansion) -> std::optional<std::string> {
        StringRef source = tupleElementText(elemIdx);
        oldExpansion = oldExpansion.trim();
        newExpansion = newExpansion.trim();
        if (source == oldExpansion)
          return newExpansion.str();
        if (auto loc = findUniqueTrimmedSubstring(source, oldExpansion))
          return stringutils::replaceRange(source.str(), loc->first,
                                           loc->second, newExpansion);
        return std::nullopt;
      };

      // Verify that the old generated expansion is actually explained by the
      // tuple-derived callee actuals.  This is the A-side proof for the tuple
      // bridge; without it, solving the new text alone could rewrite an
      // unrelated whole-argument expansion that merely happens to look similar.
      std::function<void(ArrayRef<CalleeReplayElem>, ArrayRef<ReplayTok>,
                         size_t, SmallVectorImpl<size_t> &)>
          matchOldEnds;
      matchOldEnds = [&](ArrayRef<CalleeReplayElem> elems,
                         ArrayRef<ReplayTok> toks, size_t pos,
                         SmallVectorImpl<size_t> &ends) {
        if (elems.empty()) {
          ends.push_back(pos);
          return;
        }
        const CalleeReplayElem &elem = elems.front();
        ArrayRef<CalleeReplayElem> rest = elems.drop_front();
        switch (elem.kind) {
        case CalleeReplayKind::Literal:
          if (pos < toks.size() && toks[pos].spelling == elem.literal)
            matchOldEnds(rest, toks, pos + 1, ends);
          return;
        case CalleeReplayKind::Param: {
          const auto &expected = oldActualTokSpellings[elem.paramIdx];
          if (pos + expected.size() > toks.size())
            return;
          for (size_t i = 0; i < expected.size(); ++i)
            if (toks[pos + i].spelling != expected[i])
              return;
          matchOldEnds(rest, toks, pos + expected.size(), ends);
          return;
        }
        case CalleeReplayKind::Stringify: {
          if (pos >= toks.size())
            return;
          std::optional<std::string> content =
              decodeSimpleStringLiteralToken(toks[pos].spelling);
          if (!content)
            return;
          StringRef oldActual = StringRef(oldActuals[elem.paramIdx]).trim();
          if (StringRef(*content).trim() != oldActual) {
            auto loc =
                findUniqueTrimmedSubstring(oldActual, StringRef(*content));
            if (!loc)
              return;
          }
          matchOldEnds(rest, toks, pos + 1, ends);
          return;
        }
        case CalleeReplayKind::Paste: {
          if (pos >= toks.size())
            return;
          std::string expected;
          for (const CalleePastePiece &piece : elem.pastePieces) {
            if (piece.isParam)
              expected += StringRef(oldActuals[piece.paramIdx]).trim().str();
            else
              expected += piece.literal;
          }
          if (toks[pos].spelling != expected)
            return;
          matchOldEnds(rest, toks, pos + 1, ends);
          return;
        }
        case CalleeReplayKind::VaOpt: {
          matchOldEnds(rest, toks, pos, ends);
          SmallVector<size_t, 4> childEnds;
          matchOldEnds(elem.children, toks, pos, childEnds);
          for (size_t childEnd : childEnds)
            if (childEnd != pos)
              matchOldEnds(rest, toks, childEnd, ends);
          return;
        }
        }
      };

      auto matchOldExpansion = [&](StringRef oldExpansion) {
        SmallVector<ReplayTok, 32> toks;
        lexReplayTokens(oldExpansion, toks);
        SmallVector<size_t, 4> ends;
        matchOldEnds(calleePattern, toks, 0, ends);
        return llvm::is_contained(ends, toks.size());
      };

      auto solveStringifyOrPasteNewExpansion = [&](StringRef newExpansion)
          -> std::optional<SmallVector<std::string, 8>> {
        if (calleePattern.size() != 1)
          return std::nullopt;
        const CalleeReplayElem &elem = calleePattern.front();
        SmallVector<std::string, 8> actuals;
        actuals.resize(calleeDefinition->defParams.size());
        for (size_t i = 0; i < oldActuals.size(); ++i)
          actuals[i] = oldActuals[i];

        if (elem.kind == CalleeReplayKind::Stringify) {
          std::optional<std::string> token = singleTokenSpelling(newExpansion);
          if (!token)
            return std::nullopt;
          std::optional<std::string> content =
              decodeSimpleStringLiteralToken(*token);
          if (!content)
            return std::nullopt;
          actuals[elem.paramIdx] = std::move(*content);
          return actuals;
        }

        if (elem.kind != CalleeReplayKind::Paste)
          return std::nullopt;
        std::optional<std::string> pasted = singleTokenSpelling(newExpansion);
        if (!pasted)
          return std::nullopt;

        size_t cursor = 0;
        SmallVector<std::optional<std::string>, 8> assigned;
        assigned.resize(calleeDefinition->defParams.size());
        for (const CalleePastePiece &piece : elem.pastePieces) {
          if (piece.isParam) {
            const size_t width =
                StringRef(oldActuals[piece.paramIdx]).trim().size();
            if (cursor + width > pasted->size())
              return std::nullopt;
            std::string slice =
                StringRef(*pasted).slice(cursor, cursor + width).str();
            cursor += width;
            if (assigned[piece.paramIdx] && *assigned[piece.paramIdx] != slice)
              return std::nullopt;
            assigned[piece.paramIdx] = std::move(slice);
            continue;
          }
          if (!StringRef(*pasted).substr(cursor).starts_with(piece.literal))
            return std::nullopt;
          cursor += piece.literal.size();
        }
        if (cursor != pasted->size())
          return std::nullopt;
        for (size_t i = 0; i < assigned.size(); ++i)
          if (assigned[i])
            actuals[i] = std::move(*assigned[i]);
        return actuals;
      };

      auto solveNewExpansion = [&](StringRef newExpansion)
          -> std::optional<SmallVector<std::string, 8>> {
        // Invert the callee replacement-list grammar over the B-side expansion
        // and recover the new callee actual text.  Repeated formal references
        // must bind to identical token spellings, and the result is accepted
        // only when there is exactly one solution.  This is what makes
        // duplicate tuple values safe: slots are solved by replay position, not
        // by text.
        SmallVector<ReplayTok, 32> toks;
        lexReplayTokens(newExpansion, toks);
        SmallVector<std::optional<std::pair<size_t, size_t>>, 8> assigned;
        assigned.resize(calleeDefinition->defParams.size());
        SmallVector<SmallVector<std::string, 8>, 4> solutions;

        std::function<void(
            ArrayRef<CalleeReplayElem>, size_t,
            SmallVector<std::optional<std::pair<size_t, size_t>>, 8> &)>
            dfs;
        dfs = [&](ArrayRef<CalleeReplayElem> elems, size_t tokPos,
                  SmallVector<std::optional<std::pair<size_t, size_t>>, 8>
                      &curAssigned) {
          if (solutions.size() > 1)
            return;
          if (elems.empty()) {
            if (tokPos != toks.size())
              return;
            SmallVector<std::string, 8> actuals;
            for (const auto &range : curAssigned) {
              if (!range)
                return;
              if (range->first == range->second) {
                actuals.push_back(std::string());
                continue;
              }
              const size_t byteBegin = toks[range->first].begin;
              const size_t byteEnd = toks[range->second - 1].end;
              actuals.push_back(newExpansion.slice(byteBegin, byteEnd).str());
            }
            solutions.push_back(std::move(actuals));
            return;
          }

          const CalleeReplayElem &elem = elems.front();
          ArrayRef<CalleeReplayElem> rest = elems.drop_front();
          switch (elem.kind) {
          case CalleeReplayKind::Literal:
            if (tokPos < toks.size() && toks[tokPos].spelling == elem.literal)
              dfs(rest, tokPos + 1, curAssigned);
            return;
          case CalleeReplayKind::Param: {
            if (elem.paramIdx >= curAssigned.size())
              return;
            if (curAssigned[elem.paramIdx]) {
              const auto range = *curAssigned[elem.paramIdx];
              const size_t width = range.second - range.first;
              if (tokPos + width <= toks.size()) {
                bool same = true;
                for (size_t i = 0; i < width; ++i) {
                  if (toks[range.first + i].spelling !=
                      toks[tokPos + i].spelling) {
                    same = false;
                    break;
                  }
                }
                if (same)
                  dfs(rest, tokPos + width, curAssigned);
              }
              return;
            }
            for (size_t end = tokPos; end <= toks.size(); ++end) {
              curAssigned[elem.paramIdx] = std::make_pair(tokPos, end);
              dfs(rest, end, curAssigned);
              curAssigned[elem.paramIdx].reset();
              if (solutions.size() > 1)
                return;
            }
            return;
          }
          case CalleeReplayKind::Stringify:
          case CalleeReplayKind::Paste:
            return;
          case CalleeReplayKind::VaOpt: {
            dfs(rest, tokPos, curAssigned);
            SmallVector<std::optional<std::pair<size_t, size_t>>, 8>
                withPayload = curAssigned;
            std::function<void(
                ArrayRef<CalleeReplayElem>, size_t,
                SmallVector<std::optional<std::pair<size_t, size_t>>, 8> &)>
                dfsChild;
            dfsChild = [&](ArrayRef<CalleeReplayElem> childElems,
                           size_t childTokPos,
                           SmallVector<std::optional<std::pair<size_t, size_t>>,
                                       8> &childAssigned) {
              if (childElems.empty()) {
                if (childTokPos != tokPos)
                  dfs(rest, childTokPos, childAssigned);
                return;
              }
              const CalleeReplayElem &child = childElems.front();
              ArrayRef<CalleeReplayElem> childRest = childElems.drop_front();
              switch (child.kind) {
              case CalleeReplayKind::Literal:
                if (childTokPos < toks.size() &&
                    toks[childTokPos].spelling == child.literal)
                  dfsChild(childRest, childTokPos + 1, childAssigned);
                return;
              case CalleeReplayKind::Param: {
                if (child.paramIdx >= childAssigned.size())
                  return;
                if (childAssigned[child.paramIdx]) {
                  const auto range = *childAssigned[child.paramIdx];
                  const size_t width = range.second - range.first;
                  if (childTokPos + width <= toks.size()) {
                    bool same = true;
                    for (size_t i = 0; i < width; ++i) {
                      if (toks[range.first + i].spelling !=
                          toks[childTokPos + i].spelling) {
                        same = false;
                        break;
                      }
                    }
                    if (same)
                      dfsChild(childRest, childTokPos + width, childAssigned);
                  }
                  return;
                }
                for (size_t end = childTokPos; end <= toks.size(); ++end) {
                  childAssigned[child.paramIdx] =
                      std::make_pair(childTokPos, end);
                  dfsChild(childRest, end, childAssigned);
                  childAssigned[child.paramIdx].reset();
                  if (solutions.size() > 1)
                    return;
                }
                return;
              }
              case CalleeReplayKind::Stringify:
              case CalleeReplayKind::Paste:
              case CalleeReplayKind::VaOpt:
                return;
              }
            };
            dfsChild(elem.children, tokPos, withPayload);
            return;
          }
          }
        };

        dfs(calleePattern, 0, assigned);
        if (solutions.size() != 1)
          return std::nullopt;
        return solutions.front();
      };

      // Every observed occurrence of the parent argument must agree on the same
      // solved generated-callee actuals.  If one occurrence cannot be explained
      // by the old tuple-derived call, or if two occurrences imply different B
      // actuals, the tuple bridge is unproved.
      std::optional<SmallVector<std::string, 8>> mergedSolvedActuals;
      for (const OccObservation &obs : occObservations) {
        if (!matchOldExpansion(obs.oldText))
          continue;
        auto solved = solveStringifyOrPasteNewExpansion(obs.newText);
        if (!solved)
          solved = solveNewExpansion(obs.newText);
        if (!solved || solved->size() != calleeDefinition->defParams.size())
          return false;
        if (!mergedSolvedActuals) {
          mergedSolvedActuals = std::move(*solved);
          continue;
        }
        if (mergedSolvedActuals->size() != solved->size())
          return false;
        for (size_t i = 0; i < solved->size(); ++i)
          if (StringRef((*mergedSolvedActuals)[i]).trim() !=
              StringRef((*solved)[i]).trim())
            return false;
      }
      if (!mergedSolvedActuals)
        return false;

      // For ordinary param replay, the source tuple element and the old
      // generated actual are the same spelling.  Stringification is different:
      // the B-side expansion is a string literal whose payload corresponds to
      // the generated actual after forwarding/prescan, while the tuple slot may
      // still contain a structural spelling such as `ID(alpha)`.  Keep a
      // separate old-expansion projection for tuple editing so `"alpha" ->
      // "beta"` can become `ID(alpha) -> ID(beta)` instead of replacing the
      // whole slot with `beta`.
      SmallVector<std::string, 8> oldGeneratedPiecesForRewrite;
      for (StringRef piece : oldGeneratedActualPieces)
        oldGeneratedPiecesForRewrite.push_back(piece.trim().str());
      if (calleePattern.size() == 1 &&
          calleePattern.front().kind == CalleeReplayKind::Stringify) {
        const uint32_t paramIdx = calleePattern.front().paramIdx;
        if (paramIdx < oldGeneratedPiecesForRewrite.size()) {
          for (const OccObservation &obs : occObservations) {
            std::optional<std::string> token = singleTokenSpelling(obs.oldText);
            if (!token)
              continue;
            std::optional<std::string> content =
                decodeSimpleStringLiteralToken(*token);
            if (!content)
              continue;
            StringRef sourcePiece =
                StringRef(oldGeneratedPiecesForRewrite[paramIdx]);
            if (sourcePiece == StringRef(*content) ||
                findUniqueTrimmedSubstring(sourcePiece, StringRef(*content))) {
              oldGeneratedPiecesForRewrite[paramIdx] = std::move(*content);
              break;
            }
          }
        }
      }

      // Split the solved callee actuals back into the generated tuple pieces.
      // A non-empty variadic tail appends more tuple elements; an empty tail
      // deletes the old variadic tail slice during the tuple-edit pass below.
      SmallVector<std::string, 8> newGeneratedPieces;
      for (size_t i = 0; i < fixedCalleeActuals; ++i)
        newGeneratedPieces.push_back((*mergedSolvedActuals)[i]);
      if (calleeHasVariadic) {
        StringRef tail = StringRef((*mergedSolvedActuals).back()).trim();
        if (!tail.empty())
          newGeneratedPieces.push_back(tail.str());
      }

      // Translate the solved callee actuals back to the tuple elements consumed
      // by the forwarding macro.  Non-variadic forwarding parameters map to one
      // tuple element each.  A variadic forwarding parameter maps to the whole
      // remaining tuple tail, so insertion/removal of callee variadic actuals
      // is represented by replacing that tail slice.
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
            return false;
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
          const TupleElementSlice &firstElem =
              tupleElems[ref.forwarderParamIdx];
          const TupleElementSlice &lastElem = tupleElems.back();
          edits.push_back(TupleEdit{firstElem.trimBegin, lastElem.trimEnd,
                                    std::move(text)});
          continue;
        }
        if (pieceCursor >= newGeneratedPieces.size() ||
            ref.forwarderParamIdx >= tupleElems.size())
          return false;
        const TupleElementSlice &elem = tupleElems[ref.forwarderParamIdx];
        StringRef oldText =
            pieceCursor < oldGeneratedPiecesForRewrite.size()
                ? StringRef(oldGeneratedPiecesForRewrite[pieceCursor]).trim()
                : StringRef(oldGeneratedActualPieces[pieceCursor]).trim();
        StringRef newText = StringRef(newGeneratedPieces[pieceCursor]).trim();
        if (newText !=
            tuplePayload.slice(elem.trimBegin, elem.trimEnd).trim()) {
          std::optional<std::string> rewrittenElem =
              rewriteTupleElementFromSolvedExpansion(ref.forwarderParamIdx,
                                                     oldText, newText);
          if (!rewrittenElem)
            return false;
          edits.push_back(TupleEdit{elem.trimBegin, elem.trimEnd,
                                    std::move(*rewrittenElem)});
        }
        ++pieceCursor;
      }
      if (pieceCursor != newGeneratedPieces.size())
        return false;
      if (edits.empty())
        return false;

      llvm::sort(edits, [](const TupleEdit &lhs, const TupleEdit &rhs) {
        if (lhs.begin != rhs.begin)
          return lhs.begin > rhs.begin;
        return lhs.end > rhs.end;
      });

      std::string rebuiltPayload = tuplePayload.str();
      size_t previousBegin = std::numeric_limits<size_t>::max();
      for (const TupleEdit &edit : edits) {
        if (edit.end < edit.begin || edit.end > rebuiltPayload.size())
          return false;
        if (previousBegin != std::numeric_limits<size_t>::max() &&
            edit.end > previousBegin)
          return false;
        previousBegin = edit.begin;
        rebuiltPayload = stringutils::replaceRange(rebuiltPayload, edit.begin,
                                                   edit.end, edit.text);
      }

      outNewArg = ("(" + StringRef(rebuiltPayload).trim().str() + ")");
      return true;
    };

    if (tryParentTupleGeneratedCalleeRewrite())
      return true;

    const RefoldModel::MacroInvocation *tupleChild = nullptr;
    TupleRewriteMode rewriteMode = TupleRewriteMode::None;
    SmallVector<std::pair<uint32_t, StringRef>, 8> childArgs;
    SmallVector<RefoldModel::TupleArgRef, 8> childTupleRefs;
    std::optional<uint32_t> identityForwardChildArgIdx;

    // Search direct children of the current macro invocation for exactly one
    // forwarding witness. Multiple usable children would make the caller tuple
    // rewrite ambiguous, so the code fails closed if more than one is found.
    for (const auto &cand : deps_.model.GetMacroInvocations()) {
      if (!cand.callerMacroId || *cand.callerMacroId != m.id)
        continue;

      if (cand.normalizedInvText && !cand.normalizedInvArgTextRanges.empty() &&
          !cand.argTupleRefs.empty() &&
          cand.normalizedInvArgTextRanges.size() == cand.argTupleRefs.size()) {
        SmallVector<std::pair<uint32_t, StringRef>, 8> localChildArgs;
        SmallVector<RefoldModel::TupleArgRef, 8> localTupleRefs;
        DenseSet<StringRef> seenOldTexts;
        bool ok = false;

        for (uint32_t childArgIdx = 0; childArgIdx < cand.argTupleRefs.size();
             ++childArgIdx) {
          const auto &refs = cand.argTupleRefs[childArgIdx];

          // This proof mode accepts only one direct tuple-ref per child
          // argument. Multi-ref composition is outside this local
          // reconstruction proof.
          if (refs.size() != 1)
            continue;

          const auto &ref = refs.front();
          if (ref.callerParamIndex != callerArgIdx)
            continue;

          auto oldArgText = getNormalizedArgText(cand, childArgIdx);
          if (!oldArgText)
            return false;

          // Duplicate old text would make the observation map ambiguous because
          // old occurrence text is used as the key when applying replacements.
          if (seenOldTexts.find(*oldArgText) != seenOldTexts.end())
            return false;
          seenOldTexts.insert(*oldArgText);

          // Validate that the tuple-ref byte range is a real slice of the
          // parent argument and that it textually agrees with the child
          // argument text.
          if (ref.callerByteEnd < ref.callerByteBegin ||
              ref.callerByteEnd > parentTrim.size())
            return false;
          StringRef slice =
              parentTrim.slice(ref.callerByteBegin, ref.callerByteEnd).trim();
          if (slice != oldArgText->trim())
            return false;

          localChildArgs.push_back({childArgIdx, *oldArgText});
          localTupleRefs.push_back(ref);
          ok = true;
        }

        if (ok) {
          // Accept exactly one child as the tuple-ref witness. A second
          // witness would give two possible reconstructions of the same caller
          // argument.
          if (tupleChild)
            return false;
          tupleChild = &cand;
          rewriteMode = TupleRewriteMode::DirectTupleRefs;
          childArgs = std::move(localChildArgs);
          childTupleRefs = std::move(localTupleRefs);
          continue;
        }
      }

      if (!isMacroInvocationVariadicFormal(m, callerArgIdx) || !cand.invText ||
          !cand.invB || cand.invArgRanges.empty() || cand.argRefs.empty())
        continue;

      // Variadic forwarding wrappers may not carry tuple-specific metadata.
      // Accept a second certified shape where one child argument is a
      // full-width identity forward of the caller variadic formal. That
      // proves the caller tuple survives unchanged at the child hop, so we
      // can safely rebuild it element-by-element from the occurrence
      // observations.
      std::optional<uint32_t> localIdentityArgIdx;
      for (uint32_t childArgIdx = 0; childArgIdx < cand.invArgRanges.size() &&
                                     childArgIdx < cand.argRefs.size();
           ++childArgIdx) {
        const auto &rng = cand.invArgRanges[childArgIdx];
        if (!rng.first || !rng.second || *rng.second < *rng.first ||
            *rng.first < *cand.invB)
          continue;

        const auto &refs = cand.argRefs[childArgIdx];
        if (refs.size() != 1)
          continue;

        const auto &ref = refs.front();
        if (ref.callerParamIndex != callerArgIdx)
          continue;

        const uint64_t relB = *rng.first - *cand.invB;
        const uint64_t relE = *rng.second - *cand.invB;
        if (relE < relB || relE > cand.invText->size())
          continue;

        StringRef rawArg =
            StringRef(*cand.invText).slice((size_t)relB, (size_t)relE);

        // Compare the arg-ref byte coverage against the trimmed child argument
        // bounds. Full-width trimmed coverage is the identity-forward proof:
        // the child argument is exactly the caller argument, modulo surrounding
        // space.
        size_t trimLead = 0;
        size_t trimEnd = rawArg.size();
        std::tie(trimLead, trimEnd) =
            stringutils::trimWsRange(rawArg, 0, rawArg.size());
        if (trimLead == trimEnd)
          continue;

        const uint64_t trimmedAbsBegin = relB + trimLead;
        const uint64_t trimmedAbsEnd = relB + trimEnd;
        if (ref.byteBegin != trimmedAbsBegin || ref.byteEnd != trimmedAbsEnd)
          continue;

        auto oldArgText = getInvocationArgText(cand, childArgIdx);
        if (!oldArgText || oldArgText->empty())
          continue;

        // More than one identity-forwarding child argument would not provide a
        // unique positional tuple reconstruction.
        if (localIdentityArgIdx)
          return false;
        localIdentityArgIdx = childArgIdx;
      }

      if (!localIdentityArgIdx)
        continue;

      // As above, the child witness must be unique across both proof modes.
      if (tupleChild)
        return false;

      tupleChild = &cand;
      rewriteMode = TupleRewriteMode::VariadicIdentityForward;
      identityForwardChildArgIdx = *localIdentityArgIdx;
    }

    // No direct child proved either tuple-ref forwarding or identity
    // forwarding.
    if (!tupleChild)
      return false;

    std::string rebuilt;
    bool changed = false;

    if (rewriteMode == TupleRewriteMode::DirectTupleRefs) {
      if (childArgs.empty() || childArgs.size() != childTupleRefs.size())
        return false;

      StringMap<std::string> newTextByOld;

      // Collapse occurrence observations by old text. Every occurrence of the
      // same old child text must request the same new text.
      for (const auto &obs : occObservations) {
        auto it = newTextByOld.find(obs.oldText);
        if (it == newTextByOld.end()) {
          newTextByOld[obs.oldText] = obs.newText;
          continue;
        }
        if (it->second != obs.newText)
          return false;
      }

      // A tuple-forwarding child can use one tuple element as a callee and a
      // different tuple element as that callee's argument, e.g.
      // `WRAP((ADD_ONE, 10))` -> `CALL(ADD_ONE, 10)` -> `ADD_ONE(10)`.
      // In that shape the only expansion occurrence visible at the WRAP level
      // is the full callee expansion `((10) + 1)`, so the direct tuple-ref map
      // above has no key for the tuple element `10`.  Recover that missing
      // element rewrite by proving the child replacement-list constructs a
      // function-like invocation from tuple-ref formals and then replaying the
      // callee's own replacement-token tape against the observed old/new
      // expansion text.
      auto deriveTupleElementRewritesThroughForwardedCallee = [&]() -> bool {
        if (!tupleChild || !tupleChild->definitionDirectiveId)
          return true;

        const RefoldModel::MacroDirective *childDefinition = nullptr;
        for (const RefoldModel::MacroDirective &directive :
             deps_.model.GetMacroDirectives()) {
          if (directive.id == *tupleChild->definitionDirectiveId) {
            childDefinition = &directive;
            break;
          }
        }
        if (!childDefinition || childDefinition->subkind != "#define" ||
            !childDefinition->functionLike)
          return true;

        // The accepted forwarding shape is a replacement list of the form
        //   <callee-param> '(' <argument-param/literal tape> ')'
        // with the callee and each argument coming from direct tuple refs. This
        // is a syntactic proof of a generated call, not a name-based heuristic.
        const auto &repToks = childDefinition->replacementTokens;
        if (repToks.size() < 4)
          return true;
        if (repToks[0].kind !=
                RefoldModel::MacroReplacementTokenKind::ParamRef ||
            !repToks[0].paramIndex ||
            *repToks[0].paramIndex >= childArgs.size())
          return true;
        if (repToks[1].kind !=
                RefoldModel::MacroReplacementTokenKind::Literal ||
            repToks[1].spelling != "(")
          return true;
        if (repToks.back().kind !=
                RefoldModel::MacroReplacementTokenKind::Literal ||
            repToks.back().spelling != ")")
          return true;

        const uint32_t calleeChildArgIdx = *repToks[0].paramIndex;
        std::optional<StringRef> calleeName;
        for (const auto &arg : childArgs) {
          if (arg.first == calleeChildArgIdx) {
            calleeName = arg.second.trim();
            break;
          }
        }
        if (!calleeName || calleeName->empty())
          return true;

        const RefoldModel::MacroDirective *calleeDefinition = nullptr;
        for (const RefoldModel::MacroDirective &directive :
             deps_.model.GetMacroDirectives()) {
          if (directive.subkind == "#define" && directive.functionLike &&
              directive.name == *calleeName) {
            if (calleeDefinition)
              return false;
            calleeDefinition = &directive;
          }
        }
        if (!calleeDefinition)
          return true;

        SmallVector<uint32_t, 4> forwardedChildArgs;
        for (size_t i = 2, e = repToks.size() - 1; i < e; ++i) {
          const auto &tok = repToks[i];
          if (tok.spelling == "#" || tok.spelling == "##" ||
              tok.spelling == "__VA_OPT__")
            return true;
          if (tok.kind != RefoldModel::MacroReplacementTokenKind::ParamRef)
            continue;
          if (!tok.paramIndex || *tok.paramIndex >= childArgs.size())
            return true;
          forwardedChildArgs.push_back(*tok.paramIndex);
        }
        if (forwardedChildArgs.empty() ||
            forwardedChildArgs.size() != calleeDefinition->defParams.size())
          return true;

        auto childArgText =
            [&](uint32_t childArgIdx) -> std::optional<StringRef> {
          for (const auto &arg : childArgs)
            if (arg.first == childArgIdx)
              return arg.second.trim();
          return std::nullopt;
        };

        SmallVector<std::string, 4> oldActuals;
        oldActuals.reserve(forwardedChildArgs.size());
        for (uint32_t childArgIdx : forwardedChildArgs) {
          auto text = childArgText(childArgIdx);
          if (!text)
            return true;
          oldActuals.push_back(text->str());
        }

        // Use the file-scope token carrier for this parent-tuple callee replay
        // proof so token-range helpers can later become named methods.
        using ReplayTok = ParentTupleCalleeReplayTok;
        auto lexReplayTokens = [&](StringRef text,
                                   SmallVectorImpl<ReplayTok> &out) {
          out.clear();
          SmallVector<RefoldLexBoundaryToken, 16> toks;
          refoldLexBoundaryTokens(text, deps_.lexLang, toks);
          for (const RefoldLexBoundaryToken &tok : toks)
            out.push_back(ReplayTok{tok.spelling, tok.begin, tok.end});
        };

        auto tokenSpellingsForText = [&](StringRef text) {
          SmallVector<ReplayTok, 8> toks;
          lexReplayTokens(text, toks);
          SmallVector<std::string, 8> out;
          for (const ReplayTok &tok : toks)
            out.push_back(tok.spelling);
          return out;
        };

        struct CalleeReplayElem {
          bool isParam = false;
          std::string literal;
          uint32_t paramIdx = 0;
        };
        SmallVector<CalleeReplayElem, 16> calleePattern;
        for (const RefoldModel::MacroReplacementToken &tok :
             calleeDefinition->replacementTokens) {
          if (tok.spelling == "#" || tok.spelling == "##" ||
              tok.spelling == "__VA_OPT__")
            return true;
          CalleeReplayElem elem;
          if (tok.kind == RefoldModel::MacroReplacementTokenKind::ParamRef) {
            if (!tok.paramIndex || *tok.paramIndex >= oldActuals.size())
              return true;
            elem.isParam = true;
            elem.paramIdx = *tok.paramIndex;
          } else {
            elem.literal = tok.spelling.str();
          }
          calleePattern.push_back(std::move(elem));
        }
        if (calleePattern.empty())
          return true;

        SmallVector<SmallVector<std::string, 8>, 4> oldActualTokSpellings;
        for (const std::string &actual : oldActuals)
          oldActualTokSpellings.push_back(tokenSpellingsForText(actual));

        auto matchOldExpansion = [&](StringRef oldExpansion) {
          SmallVector<ReplayTok, 16> toks;
          lexReplayTokens(oldExpansion, toks);
          size_t pos = 0;
          for (const CalleeReplayElem &elem : calleePattern) {
            if (!elem.isParam) {
              if (pos >= toks.size() || toks[pos].spelling != elem.literal)
                return false;
              ++pos;
              continue;
            }
            const auto &expected = oldActualTokSpellings[elem.paramIdx];
            if (!replayTokenRangeSpellingsEqual(toks, pos, expected))
              return false;
            pos += expected.size();
          }
          return pos == toks.size();
        };

        auto solveNewExpansion = [&](StringRef newExpansion)
            -> std::optional<SmallVector<std::string, 4>> {
          SmallVector<ReplayTok, 16> toks;
          lexReplayTokens(newExpansion, toks);
          SmallVector<std::optional<std::pair<size_t, size_t>>, 4> assigned;
          assigned.resize(calleeDefinition->defParams.size());
          SmallVector<SmallVector<std::string, 4>, 4> solutions;

          std::function<void(size_t, size_t)> dfs = [&](size_t elemIdx,
                                                        size_t tokPos) {
            if (solutions.size() > 1)
              return;
            if (elemIdx == calleePattern.size()) {
              if (tokPos != toks.size())
                return;
              SmallVector<std::string, 4> actuals;
              for (const auto &range : assigned) {
                if (!range)
                  return;
                if (range->first == range->second) {
                  actuals.push_back(std::string());
                  continue;
                }
                const size_t byteBegin = toks[range->first].begin;
                const size_t byteEnd = toks[range->second - 1].end;
                actuals.push_back(newExpansion.slice(byteBegin, byteEnd).str());
              }
              solutions.push_back(std::move(actuals));
              return;
            }

            const CalleeReplayElem &elem = calleePattern[elemIdx];
            if (!elem.isParam) {
              if (tokPos < toks.size() && toks[tokPos].spelling == elem.literal)
                dfs(elemIdx + 1, tokPos + 1);
              return;
            }

            if (elem.paramIdx >= assigned.size())
              return;
            if (assigned[elem.paramIdx]) {
              const auto range = *assigned[elem.paramIdx];
              const size_t width = range.second - range.first;
              if (tokPos + width <= toks.size()) {
                bool same = true;
                for (size_t i = 0; i < width; ++i) {
                  if (toks[range.first + i].spelling !=
                      toks[tokPos + i].spelling) {
                    same = false;
                    break;
                  }
                }
                if (same)
                  dfs(elemIdx + 1, tokPos + width);
              }
              return;
            }

            for (size_t end = tokPos; end <= toks.size(); ++end) {
              assigned[elem.paramIdx] = std::make_pair(tokPos, end);
              dfs(elemIdx + 1, end);
              assigned[elem.paramIdx].reset();
              if (solutions.size() > 1)
                return;
            }
          };

          dfs(0, 0);
          if (solutions.size() != 1)
            return std::nullopt;
          return solutions.front();
        };

        for (const OccObservation &obs : occObservations) {
          if (!matchOldExpansion(obs.oldText))
            continue;
          auto solvedActuals = solveNewExpansion(obs.newText);
          if (!solvedActuals)
            return false;
          if (solvedActuals->size() != forwardedChildArgs.size())
            return false;

          for (size_t i = 0; i < forwardedChildArgs.size(); ++i) {
            auto oldText = childArgText(forwardedChildArgs[i]);
            if (!oldText)
              return false;
            StringRef oldKey = oldText->trim();
            StringRef newValue = StringRef((*solvedActuals)[i]).trim();
            if (oldKey == newValue)
              continue;
            auto it = newTextByOld.find(oldKey);
            if (it == newTextByOld.end()) {
              newTextByOld[oldKey] = newValue.str();
              continue;
            }
            if (StringRef(it->second).trim() != newValue)
              return false;
          }
        }
        return true;
      };

      if (!deriveTupleElementRewritesThroughForwardedCallee())
        return false;

      rebuilt = parentTrim.str();

      // Replace parent tuple slices from right to left so tuple-ref byte
      // offsets remain valid while editing `rebuilt`.
      SmallVector<unsigned, 8> order(childTupleRefs.size());
      for (unsigned i = 0; i < childTupleRefs.size(); ++i)
        order[i] = i;
      llvm::sort(order, [&](unsigned a, unsigned b) {
        return childTupleRefs[a].callerByteBegin >
               childTupleRefs[b].callerByteBegin;
      });

      for (unsigned idx : order) {
        const auto &pair = childArgs[idx];
        StringRef oldChildText = pair.second.trim();
        auto it = newTextByOld.find(oldChildText);
        if (it == newTextByOld.end())
          continue;

        const auto &ref = childTupleRefs[idx];
        rebuilt = stringutils::replaceRange(rebuilt, ref.callerByteBegin,
                                            ref.callerByteEnd, it->second);
        if (it->second != oldChildText)
          changed = true;
      }

      if (!changed)
        return false;

    } else if (rewriteMode == TupleRewriteMode::VariadicIdentityForward) {
      SmallVector<TupleElementSlice, 8> tupleElems;

      // Split the caller variadic argument into top-level elements using the
      // lexer-backed splitter so nested commas do not create false elements.
      if (!splitTopLevelTupleElementsWithLexer(parentTrim, deps_.lexLang,
                                               tupleElems))
        return false;

      // Identity-forward rewrites are positional: the immediate child keeps
      // the caller variadic tuple intact, so each observed occurrence must
      // correspond to exactly one top-level tuple element in order.
      if (tupleElems.size() != occObservations.size())
        return false;

      rebuilt = parentTrim.str();
      for (size_t i = tupleElems.size(); i > 0; --i) {
        const auto &elem = tupleElems[i - 1];
        StringRef oldElemText =
            parentTrim.slice(elem.trimBegin, elem.trimEnd).trim();

        // Replacements apply from right to left so earlier byte offsets
        // stay valid while we splice into the rebuilt caller tuple.
        if (oldElemText != occObservations[i - 1].oldText.trim())
          return false;

        rebuilt =
            stringutils::replaceRange(rebuilt, elem.trimBegin, elem.trimEnd,
                                      occObservations[i - 1].newText);
        if (occObservations[i - 1].newText != oldElemText)
          changed = true;
      }

      if (!changed)
        return false;

    } else {
      return false;
    }

    // Return the rebuilt caller argument, normalized to the same trimmed
    // spelling convention used throughout this tuple-forwarding path.
    outNewArg = StringRef(rebuilt).trim().str();

    return true;
  };

  // If the rewritten B slice introduces extra unmatched opening delimiters
  // relative to the old occurrence text, extend the right edge over matching
  // unchanged closer tokens. This keeps the observation envelope balanced when
  // the diff split left the closers just outside the initial hunk.
  auto maybeExtendRightBoundaryClosers =
      [&](const RefoldModel::PPArgSpan &sp, std::pair<size_t, size_t> env,
          StringRef oldText) -> std::pair<size_t, size_t> {
    StringRef curText =
        deps_.sourceMapper.SliceBSource(env.first, env.second).trim();
    auto oldBal = computeDelimiterBalance(oldText);
    auto newBal = computeDelimiterBalance(curText);

    // Only extra opens introduced by the candidate B text need compensation.
    // Extra closers or unchanged balance do not require right-edge widening.
    int needParen = std::max(0, newBal.paren - oldBal.paren);
    int needBracket = std::max(0, newBal.bracket - oldBal.bracket);
    int needBrace = std::max(0, newBal.brace - oldBal.brace);
    if (needParen == 0 && needBracket == 0 && needBrace == 0)
      return env;

    uint64_t aPos = sp.end;
    size_t bPos = env.second;

    // Walk forward only through identical A/B closer tokens immediately after
    // the occurrence. This preserves semantics: widening is allowed only over
    // text that already matches on both sides and exactly satisfies the missing
    // delimiter balance.
    while ((needParen > 0 || needBracket > 0 || needBrace > 0) &&
           aPos < deps_.aToks.size() && bPos < deps_.bToks.size()) {
      StringRef aTok = deps_.aToks[static_cast<size_t>(aPos)].spelling;
      StringRef bTok = deps_.bToks[bPos].spelling;
      if (aTok != bTok)
        break;

      if (aTok == ")" && needParen > 0) {
        --needParen;
        ++aPos;
        ++bPos;
        env.second = bPos;
        continue;
      }
      if (aTok == "]" && needBracket > 0) {
        --needBracket;
        ++aPos;
        ++bPos;
        env.second = bPos;
        continue;
      }
      if (aTok == "}" && needBrace > 0) {
        --needBrace;
        ++aPos;
        ++bPos;
        env.second = bPos;
        continue;
      }

      // Stop at the first non-needed token; this helper is a narrow boundary
      // repair, not a general hunk-widening mechanism.
      break;
    }
    return env;
  };

  // Validate a repaired formal replacement against every occurrence whose
  // argIdx was obtained by current-level/declaration replay.
  //
  // This mirrors the normal all-occurrences check, but uses the repaired
  // `standardArgSpans` collection so stale producer indices cannot validate a
  // partial rewrite of only the first occurrence.
  auto standardArgReplacementMatchesAllReplayedOccurrencesInB =
      [&](uint32_t argIdx, StringRef newArg,
          ArrayRef<diffutils::Hunk> tokenHunks) -> bool {
    const uint64_t maxTok =
        deps_.bTokOff.empty() ? 0ULL
                              : static_cast<uint64_t>(deps_.bTokOff.size() - 1);
    const StringRef expected = newArg.trim();
    bool sawOccurrence = false;

    for (const RefoldModel::PPArgSpan &s : standardArgSpans) {
      if (s.argIdx != argIdx || s.kind != PPArgSpanKind::Standard)
        continue;
      sawOccurrence = true;

      auto bEnv = deps_.sourceMapper.MapAToBTokenEnvelopeByPPArgSpan(s);
      if (!bEnv || bEnv->second < bEnv->first)
        return false;

      // Grow the mapped B envelope with any owned insertions or overlapping
      // hunks for this occurrence before comparing the materialized text.
      size_t lo = bEnv->first;
      size_t hi = bEnv->second;
      for (const diffutils::Hunk &hk : tokenHunks) {
        if (auto owned =
                OccurrenceReplay().GetOwnedPureInsertionBRangeForArgSpan(
                    s, standardArgSpans, *bEnv, hk)) {
          lo = std::min(lo, owned->first);
          hi = std::max(hi, owned->second);
          continue;
        }
        if (hk.aStart == hk.aEnd)
          continue;
        if (hk.aStart < s.end && hk.aEnd > s.begin && hk.bStart < hk.bEnd) {
          lo = static_cast<size_t>(std::min<uint64_t>(lo, hk.bStart));
          hi = static_cast<size_t>(std::max<uint64_t>(hi, hk.bEnd));
        }
      }

      lo = static_cast<size_t>(std::clamp<uint64_t>(lo, 0ULL, maxTok));
      hi = static_cast<size_t>(std::clamp<uint64_t>(hi, lo, maxTok));

      StringRef oldText =
          deps_.sourceMapper.SliceASource(s.begin, s.end).trim();
      auto grownEnv = maybeExtendRightBoundaryClosers(s, {lo, hi}, oldText);
      StringRef actual =
          deps_.sourceMapper.SliceBSource(grownEnv.first, grownEnv.second)
              .trim();
      if (actual != expected)
        return false;
    }

    return sawOccurrence;
  };

  // Compute argument replacements implied by each touched occurrence. Multiple
  // occurrences of the same argIdx must imply the exact same replacement,
  // otherwise the macro cannot be refolded args-only.
  DenseMap<uint32_t, std::string> replByArgIdx;
  DenseMap<uint32_t, std::pair<uint64_t, uint64_t>> materializedRangeByArgIdx;
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

    SmallVector<OccObservation, 8> occObservations;
    std::optional<std::string> unifiedNewArg;
    std::optional<std::pair<uint64_t, uint64_t>>
        unifiedMaterializedNewTextRange;
    bool sawUntrackedMaterializedNewTextRange = false;
    bool needTupleForwarding = false;

    for (size_t i = 0; i < occs.size(); ++i) {
      const auto &sp = occs[i];
      if (sp.argIdx != argIdx)
        continue;

      // Start with the B-token envelope corresponding to this A-side argument
      // occurrence. If the direct PP-arg mapping is unavailable, fall back to
      // the triggering hunk's B range so the path can still fail/validate
      // locally.
      auto bEnv = deps_.sourceMapper.MapAToBTokenEnvelopeByPPArgSpan(sp);
      if (!bEnv) {
        if (h.bStart >= h.bEnd)
          return std::nullopt;
        bEnv = {static_cast<size_t>(h.bStart), static_cast<size_t>(h.bEnd)};
      }

      std::optional<std::pair<size_t, size_t>> ownedInsertionBRange;
      bool contributedNonInsertionHunk = false;

      if (bEnv) {
        size_t e0 = bEnv->first;
        size_t e1 = bEnv->second;

        // Widen the occurrence envelope to include every token hunk that
        // belongs to this same touched formal. This prevents deriving a
        // replacement from only one fragment of a multi-hunk argument edit.
        for (const auto &candH : tokenHunks) {
          // Pure insertions have no A-side interval, so accept them only when
          // the ownership helper proves the inserted B range belongs to this
          // exact argument occurrence. Record those inserted B tokens
          // separately: they are the precise output-side materialization
          // surface for insertion-only argument rewrites.
          if (auto owned =
                  OccurrenceReplay().GetOwnedPureInsertionBRangeForArgSpan(
                      sp, occs, *bEnv, candH)) {
            const size_t insB0 = owned->first;
            const size_t insB1 = owned->second;
            if (!(insB1 < e0 || e1 < insB0)) {
              e0 = std::min(e0, insB0);
              e1 = std::max(e1, insB1);
              if (ownedInsertionBRange) {
                ownedInsertionBRange->first =
                    std::min(ownedInsertionBRange->first, insB0);
                ownedInsertionBRange->second =
                    std::max(ownedInsertionBRange->second, insB1);
              } else {
                ownedInsertionBRange = std::make_pair(insB0, insB1);
              }
            }
            continue;
          }

          // Non-insertion hunks can widen the envelope when their A-side
          // interval overlaps this argument occurrence and they carry concrete
          // B text.
          if (candH.aStart == candH.aEnd)
            continue;
          if (candH.aStart < sp.end && candH.aEnd > sp.begin &&
              candH.bStart < candH.bEnd) {
            contributedNonInsertionHunk = true;
            e0 = std::min(e0, static_cast<size_t>(candH.bStart));
            e1 = std::max(e1, static_cast<size_t>(candH.bEnd));
          }
        }

        if (e0 != bEnv->first || e1 != bEnv->second) {
          bEnv = std::make_pair(e0, e1);
        }
      }

      // `oldText` is the original occurrence spelling. `bEnv` is the candidate
      // B spelling that should replace this occurrence after all relevant hunks
      // for the same formal have been incorporated.
      StringRef oldText =
          deps_.sourceMapper.SliceASource(sp.begin, sp.end).trim();

      // For ordinary argument occurrences, allow a narrow right-edge repair
      // over unchanged closer tokens when the diff split leaves balancing
      // delimiters just outside the initial B envelope.
      if (sp.kind == PPArgSpanKind::Standard) {
        auto grownEnv = maybeExtendRightBoundaryClosers(sp, *bEnv, oldText);
        if (grownEnv.second != bEnv->second) {
          bEnv = grownEnv;
        }
      }

      StringRef bSlice =
          deps_.sourceMapper.SliceBSource(bEnv->first, bEnv->second).trim();
      std::string newArg = bSlice.str();

      std::optional<std::pair<uint64_t, uint64_t>> materializedNewTextRange;
      if (!occIsStringify[i] && ownedInsertionBRange &&
          !contributedNonInsertionHunk) {
        if (auto insertedBytes = deps_.sourceMapper.BTokenRangeToByteRange(
                ownedInsertionBRange->first, ownedInsertionBRange->second)) {
          const char *sourceBegin = deps_.bSource.data();
          const char *sourceEnd = sourceBegin + deps_.bSource.size();
          const char *sliceBeginPtr = bSlice.data();
          const char *sliceEndPtr = sliceBeginPtr + bSlice.size();
          if (sourceBegin <= sliceBeginPtr && sliceBeginPtr <= sourceEnd &&
              sourceBegin <= sliceEndPtr && sliceEndPtr <= sourceEnd) {
            const uint64_t sliceBegin =
                static_cast<uint64_t>(sliceBeginPtr - sourceBegin);
            const uint64_t sliceEnd =
                static_cast<uint64_t>(sliceEndPtr - sourceBegin);
            if (insertedBytes->first >= sliceBegin &&
                insertedBytes->second <= sliceEnd) {
              materializedNewTextRange =
                  std::make_pair(insertedBytes->first - sliceBegin,
                                 insertedBytes->second - sliceBegin);
            }
          }
        }
      }

      if (occIsStringify[i]) {
        // Stringify occurrences expose a string literal in the expansion, not
        // the raw argument spelling. Invert the literal back to argument text,
        // then require canonicalization to be stable so ambiguous escapes fail
        // closed.
        auto un = deps_.argTextRecovery.UnstringifyLiteralToArgText(
            bSlice, isMacroInvocationVariadicFormal(m, argIdx));
        if (!un)
          return std::nullopt;

        auto canon = stringutils::canonicalizeStringifyInversePayload(*un);
        if (!canon || StringRef(*canon).trim() != StringRef(*un).trim()) {
          return std::nullopt;
        }

        newArg = std::move(*canon);

        // Normalize the old side into the same unstringified representation so
        // the later occurrence-consistency checks compare argument text to
        // argument text.
        auto oldUn =
            deps_.argTextRecovery.UnstringifyLiteralToArgText(oldText, true);
        if (oldUn)
          oldText = StringRef(*oldUn).trim();
      }

      if (!occIsStringify[i] && !m.pasteSpans.empty()) {
        bool argHasPaste = false;
        for (const auto &ps : m.pasteSpans) {
          if (ps.argIdx == argIdx) {
            argHasPaste = true;
            break;
          }
        }

        if (argHasPaste) {
          StringRef aSlice =
              deps_.sourceMapper.SliceASource(sp.begin, sp.end).trim();
          if (!aSlice.empty()) {
            size_t pos = baseArgText.find(aSlice);
            if (pos != StringRef::npos) {
              // Paste/lift case: the occurrence may represent only the pasted
              // segment inside a larger call-site argument. Replace that
              // original segment inside the full base argument rather than
              // replacing the whole argument with the pasted-token slice.
              std::string cand = baseArgText.substr(0, pos).str() +
                                 bSlice.str() +
                                 baseArgText.substr(pos + aSlice.size()).str();
              newArg = StringRef(cand).trim().str();
              materializedNewTextRange = std::nullopt;
            } else {
              // The pasted occurrence could not be located inside the original
              // call-site argument, so leave `newArg` as the direct B slice and
              // let the later consistency/validation checks decide whether it
              // is usable.
            }
          }
        }
      }

      // Record this occurrence-level old/new observation. If all observations
      // for this formal agree on one replacement, the args-only path can
      // rewrite the formal directly; disagreement triggers the tuple-forwarding
      // fallback.
      occObservations.push_back(
          OccObservation{oldText, newArg, materializedNewTextRange});
      if (!materializedNewTextRange) {
        sawUntrackedMaterializedNewTextRange = true;
      } else if (unifiedMaterializedNewTextRange) {
        unifiedMaterializedNewTextRange->first =
            std::min(unifiedMaterializedNewTextRange->first,
                     materializedNewTextRange->first);
        unifiedMaterializedNewTextRange->second =
            std::max(unifiedMaterializedNewTextRange->second,
                     materializedNewTextRange->second);
      } else {
        unifiedMaterializedNewTextRange = *materializedNewTextRange;
      }

      if (!unifiedNewArg)
        unifiedNewArg = newArg;
      else if (*unifiedNewArg != newArg)
        needTupleForwarding = true;
    }

    std::string finalNewArg;
    bool tupleForwarded = false;

    if (needTupleForwarding) {
      // Occurrence observations for this formal did not collapse to one uniform
      // replacement. Try the narrower tuple-forwarding proof before rejecting
      // the args-only rewrite outright.
      if (!tryTupleForwardedCallerTupleRewrite(argIdx, baseArgText,
                                               occObservations, finalNewArg)) {
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
      if (tryTupleForwardedCallerTupleRewrite(argIdx, baseArgText,
                                              occObservations, finalNewArg)) {
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

    auto tupleSliceConsistencyMatchesAllOccurrencesInB = [&]() -> bool {
      // Tuple-forwarded rewrites are slice-based: each old tuple element/slice
      // must consistently map to exactly one new spelling across all
      // observations.
      llvm::StringMap<std::string> newTextByOld;
      for (const auto &obs : occObservations) {
        StringRef oldKey = StringRef(obs.oldText).trim();
        StringRef newVal = StringRef(obs.newText).trim();
        auto it = newTextByOld.find(oldKey);
        if (it == newTextByOld.end()) {
          newTextByOld[oldKey] = newVal.str();
          continue;
        }
        if (StringRef(it->second).trim() != newVal)
          return false;
      }

      // Validate the slice map against every standard occurrence of this formal
      // in B, not only the occurrence that originally triggered the rewrite.
      for (const auto &s : standardArgSpans) {
        if (s.argIdx != argIdx || s.kind != PPArgSpanKind::Standard)
          continue;

        StringRef oldSlice =
            deps_.sourceMapper.SliceASource(s.begin, s.end).trim();
        auto expectedIt = newTextByOld.find(oldSlice);
        if (expectedIt == newTextByOld.end())
          return false;

        auto bEnv = deps_.sourceMapper.MapAToBTokenEnvelopeByPPArgSpan(s);
        if (!bEnv)
          return false;

        size_t lo = bEnv->first;
        size_t hi = bEnv->second;

        // Reconstruct the same widened B envelope used during observation
        // collection, incorporating owned insertions and overlapping token
        // hunks for this occurrence.
        for (const auto &hk : tokenHunks) {
          if (auto owned =
                  OccurrenceReplay().GetOwnedPureInsertionBRangeForArgSpan(
                      s, standardArgSpans, *bEnv, hk)) {
            lo = std::min(lo, owned->first);
            hi = std::max(hi, owned->second);
            continue;
          }
          if (hk.aStart == hk.aEnd)
            continue;
          if (hk.aStart < s.end && hk.aEnd > s.begin && hk.bStart < hk.bEnd) {
            lo = static_cast<size_t>(std::min<uint64_t>(lo, hk.bStart));
            hi = static_cast<size_t>(std::max<uint64_t>(hi, hk.bEnd));
          }
        }

        // Apply the same narrow delimiter-closer repair used for the primary
        // observation path so validation compares equivalent envelopes.
        if (s.kind == PPArgSpanKind::Standard) {
          auto grownEnv = maybeExtendRightBoundaryClosers(
              s, std::make_pair(lo, hi), oldSlice);
          lo = grownEnv.first;
          hi = grownEnv.second;
        }

        StringRef tokText = deps_.sourceMapper.SliceBSource(lo, hi).trim();
        if (tokText != StringRef(expectedIt->second).trim())
          return false;
      }

      return true;
    };

    const bool matchesAllOccurrences =
        tupleForwarded ? tupleSliceConsistencyMatchesAllOccurrencesInB()
        : replayedStandardArgSpanFormalIndices
            ? standardArgReplacementMatchesAllReplayedOccurrencesInB(
                  argIdx, finalNewArg, tokenHunks)
            : OccurrenceReplay().MacroArgReplacementMatchesAllOccurrencesInB(
                  m, argIdx, baseArgText, finalNewArg, tokenHunks);

    if (!matchesAllOccurrences) {
      // The candidate replacement explained the local observations but failed
      // the global occurrence check. Before returning, gather tuple-specific
      // diagnostics when child tuple metadata exists for this argument.
      bool hasTupleChildForArg = false;
      for (const auto &cand : deps_.model.GetMacroInvocations()) {
        if (!cand.callerMacroId || *cand.callerMacroId != m.id)
          continue;
        if (cand.argTupleRefs.empty())
          continue;

        for (const auto &refs : cand.argTupleRefs) {
          for (const auto &ref : refs) {
            if (ref.callerParamIndex == argIdx) {
              hasTupleChildForArg = true;
              break;
            }
          }
          if (hasTupleChildForArg)
            break;
        }
        if (hasTupleChildForArg)
          break;
      }

      if (hasTupleChildForArg) {

        for (const auto &s : standardArgSpans) {
          if (s.argIdx != argIdx || s.kind != PPArgSpanKind::Standard)
            continue;

          auto bEnv = deps_.sourceMapper.MapAToBTokenEnvelopeByPPArgSpan(s);
          if (!bEnv) {
            continue;
          }

          size_t lo = bEnv->first;
          size_t hi = bEnv->second;
          SmallVector<std::string, 8> hunkEffects;

          // Rebuild and log the exact envelope-extension reasoning for this
          // occurrence: owned insertion hunks, overlapping hunks, or ignored
          // hunks.
          for (const auto &hk : tokenHunks) {
            if (auto owned =
                    OccurrenceReplay().GetOwnedPureInsertionBRangeForArgSpan(
                        s, standardArgSpans, *bEnv, hk)) {
              hunkEffects.push_back(
                  formatv("owned {0} -> [{1},{2}) '{3}'", hk, owned->first,
                          owned->second,
                          stringutils::showWsWithClip(
                              deps_.sourceMapper.SliceBSource(owned->first,
                                                              owned->second),
                              80))
                      .str());
              lo = std::min(lo, owned->first);
              hi = std::max(hi, owned->second);
              continue;
            }

            bool touches = false;
            if (hk.aStart != hk.aEnd)
              touches = (hk.aStart < s.end && hk.aEnd > s.begin);

            if (touches && hk.bStart < hk.bEnd) {
              hunkEffects.push_back(
                  formatv(
                      "overlap {0} -> [{1},{2}) '{3}'", hk, (uint64_t)hk.bStart,
                      (uint64_t)hk.bEnd,
                      stringutils::showWsWithClip(
                          deps_.sourceMapper.SliceBSource(hk.bStart, hk.bEnd),
                          80))
                      .str());
              lo = static_cast<size_t>(std::min<uint64_t>(lo, hk.bStart));
              hi = static_cast<size_t>(std::max<uint64_t>(hi, hk.bEnd));
            } else {
              // This hunk did not contribute to the B envelope for this
              // occurrence; keep it in the diagnostic output so missing/extra
              // hunk effects are visible when debugging tuple-forward failures.
              hunkEffects.push_back(formatv("ignored {0}", hk).str());
            }
          }
        }
      }

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

    if (finalMaterializedRange &&
        finalMaterializedRange->second <= finalNewArg.size()) {
      materializedRangeByArgIdx[argIdx] = *finalMaterializedRange;
    }
    replByArgIdx[argIdx] = std::move(finalNewArg);
  }

  // If nothing required replacement, there is no meaningful args-only patch to
  // emit.
  if (replByArgIdx.empty()) {
    return std::nullopt;
  }

  std::optional<InvocationRewriteWithRange> rewrite =
      deps_.buildInvocationRewriteWithRange(actualRecoveryCtx, replByArgIdx,
                                            &materializedRangeByArgIdx);
  if (!rewrite)
    return std::nullopt;

  {
    MacroPatch patch{*m.invB, *m.invE, std::move(rewrite->text), m.id};
    deps_.proofCertifier.CertifyInvocationRewriteMaterializedOutputRange(
        patch, rewrite->materializedOutputByteStart,
        rewrite->materializedOutputByteEnd);
    // The materialized output byte range may remain narrowed to an inserted
    // payload inside one argument, but the target-PP proof for an
    // invocation-preserving macro repair is the B-side expansion envelope of
    // the whole macro owner.  Keeping the output byte range narrow is useful
    // for source-spelling edits; leaving the B-token range uncertified would
    // make append/pure-insertion repairs look theorem-incomplete even after the
    // occurrence replay above proved that the rewritten invocation regenerates
    // the edited expansion.
    deps_.certifyMacroPatchWholeExpansionBRange(m, patch);
    deps_.proofCertifier.SetArgsOnlyStandardProof(
        patch, m, /*wholeEnvelopeReplayValidated=*/false);
    return patch;
  }
}
} // namespace refold
} // namespace clang
