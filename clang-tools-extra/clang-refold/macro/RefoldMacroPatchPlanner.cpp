//===--- RefoldMacroPatchPlanner.cpp ---------------------------*- C++ -*-===//
//
// Macro invocation patch planning service for clang-refold.
//
// This translation unit owns macro patch planning and the planner-local
// helpers that still belong to macro orchestration.  Its state enters through
// explicit planner dependencies; specialized replay, proof, and selection
// sub-services own their narrower responsibilities.
//
//===----------------------------------------------------------------------===//

#include "macro/RefoldMacroPatchPlanner.h"
#include "core/RefoldLog.h"
#include "core/RefoldOwnerClassifier.h"
#include "edit/RefoldBInsertionLedger.h"
#include "line-control/RefoldLineObserverLayout.h"
#include "macro/RefoldArgTextRecovery.h"
#include "macro/RefoldMacroPlannerHelpers.h"
#include "macro/RefoldMacroReplay.h"
#include "macro/RefoldMacroStateProof.h"
#include "macro/RefoldMacroTupleHelpers.h"
#include "proof/RefoldOwnerStateProof.h"
#include "proof/RefoldProofLattice.h"
#include "source/TokenTextHelpers.h"
#include "util/StringUtils.h"

#include "clang/Basic/SourceLocation.h"
#include "clang/Basic/TokenKinds.h"
#include "clang/Lex/Lexer.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/STLFunctionalExtras.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/Support/FormatVariadic.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <functional>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

using namespace llvm;

namespace clang {
namespace refold {

// Service members are constructed in declaration order from `deps_` and,
// where relevant, earlier-constructed members.  The two generated-callee
// engines take std::function callbacks for
// `ResolveFunctionLikeMacroForReplay`, `IsObjectLikeSingleTokenAlias`, and
// `BuildInvocationRewriteWithRange`: those helpers stay on the planner
// because several replay paths share the same policy, and the engines must not
// back-reference planner internals.
RefoldMacroPatchPlanner::RefoldMacroPatchPlanner(Dependencies deps)
    : deps_(std::move(deps)),
      proofCertifier_(
          RefoldMacroPatchProofCertifier::Dependencies{*deps_.proofLattice}),
      subtreeReplayValidator_(RefoldMacroSubtreeReplayValidator::Dependencies{
          *deps_.model, *deps_.macroTopology, *deps_.sourceMapper,
          *deps_.argTextRecovery, *deps_.lexLang, *deps_.abTokHunks}),
      replayStabilityValidator_(
          RefoldMacroReplayStabilityValidator::Dependencies{
              *deps_.model, *deps_.macroTopology, *deps_.sourceMapper,
              deps_.aToks, deps_.bToks, *deps_.lexLang, *deps_.pathIdentity,
              *deps_.lineDirs, *deps_.macroStateProof,
              subtreeReplayValidator_}),
      generatedCalleeReplayEngine_(
          RefoldMacroGeneratedCalleeReplayEngine::Dependencies{
              *deps_.model, *deps_.sourceMapper, *deps_.lexLang,
              proofCertifier_,
              [this](
                  llvm::StringRef name) -> const RefoldModel::MacroDirective * {
                return ResolveFunctionLikeMacroForReplay(name);
              },
              [this](llvm::StringRef name, uint32_t *aliasHops)
                  -> const RefoldModel::MacroDirective * {
                return ResolveFunctionLikeMacroThroughAliasesWithHops(
                    name, aliasHops);
              },
              [this](
                  const InvocationActualRecoveryContext &ctx,
                  const llvm::DenseMap<uint32_t, std::string> &repl,
                  const llvm::DenseMap<uint32_t, std::pair<uint64_t, uint64_t>>
                      *materializedRangeByArgIdx) {
                return BuildInvocationRewriteWithRange(
                    ctx, repl, materializedRangeByArgIdx);
              }}),
      generatedLeafReplayEngine_(
          RefoldMacroGeneratedLeafReplayEngine::Dependencies{
              *deps_.model, *deps_.sourceMapper, *deps_.lexLang,
              proofCertifier_,
              [this](
                  llvm::StringRef name) -> const RefoldModel::MacroDirective * {
                return ResolveFunctionLikeMacroForReplay(name);
              },
              [this](llvm::StringRef name) {
                return IsObjectLikeSingleTokenAlias(name);
              },
              [this](
                  const InvocationActualRecoveryContext &ctx,
                  const llvm::DenseMap<uint32_t, std::string> &repl,
                  const llvm::DenseMap<uint32_t, std::pair<uint64_t, uint64_t>>
                      *materializedRangeByArgIdx) {
                return BuildInvocationRewriteWithRange(
                    ctx, repl, materializedRangeByArgIdx);
              }}),
      standardArgsOnlyPatchBuilder_(
          RefoldMacroStandardArgsOnlyPatchBuilder::Dependencies{
              *deps_.model, *deps_.sourceMapper, *deps_.lexLang,
              *deps_.macroTopology, *deps_.argTextRecovery,
              *deps_.bInsertionLedger, deps_.aToks, deps_.bToks, deps_.bSource,
              deps_.bTokOff, *deps_.abTokHunks, proofCertifier_,
              *deps_.proofLattice, generatedCalleeReplayEngine_,
              generatedLeafReplayEngine_, deps_.strict,
              [this](llvm::StringRef name, uint32_t *aliasHops)
                  -> const RefoldModel::MacroDirective * {
                return ResolveFunctionLikeMacroThroughAliasesWithHops(
                    name, aliasHops);
              },
              [this](
                  const InvocationActualRecoveryContext &ctx,
                  const llvm::DenseMap<uint32_t, std::string> &repl,
                  const llvm::DenseMap<uint32_t, std::pair<uint64_t, uint64_t>>
                      *materializedRangeByArgIdx) {
                return BuildInvocationRewriteWithRange(
                    ctx, repl, materializedRangeByArgIdx);
              },
              [this](const RefoldModel::MacroInvocation &m, MacroPatch &patch) {
                return CertifyMacroPatchWholeExpansionBRange(m, patch);
              }}) {
  assert(deps_.model && "macro planner requires a model");
  assert(deps_.abTokHunks && "macro planner requires token hunks");
  assert(deps_.bInsertionLedger &&
         "macro planner requires B insertion claim ledger");
  assert(deps_.argTextRecovery && "macro planner requires arg recovery");
  assert(deps_.lexLang && "macro planner requires lexer options");
  assert(deps_.lineDirs && "macro planner requires line directives");
  assert(deps_.macroTopology && "macro planner requires macro topology");
  assert(deps_.pathIdentity && "macro planner requires path identity");
  assert(deps_.sourceMapper && "macro planner requires source mapper");
  assert(deps_.ownerClassifier && "macro planner requires owner/TU classifier");
  assert(deps_.macroStateProof && "macro planner requires macro-state proof");
  assert(deps_.ownerStateProof && "macro planner requires owner-state proof");
  assert(deps_.proofLattice && "macro planner requires proof lattice");
}

//===----------------------------------------------------------------------===//
// Borrowed proof service accessors
//===----------------------------------------------------------------------===//

RefoldMacroStateProof &RefoldMacroPatchPlanner::GetMacroStateProof() const {
  assert(deps_.macroStateProof && "macro planner requires macro-state proof");
  return *deps_.macroStateProof;
}

RefoldOwnerStateProof &RefoldMacroPatchPlanner::GetOwnerStateProof() const {
  assert(deps_.ownerStateProof && "macro planner requires owner-state proof");
  return *deps_.ownerStateProof;
}

RefoldProofLattice &RefoldMacroPatchPlanner::GetProofLattice() const {
  assert(deps_.proofLattice && "macro planner requires proof lattice");
  return *deps_.proofLattice;
}

//===----------------------------------------------------------------------===//
// Macro proof helper façades
//===----------------------------------------------------------------------===//
//
// The planner exposes a small façade surface for call sites that ask it for
// argument-layout and whole-cover information.  The actual proof work below is
// delegated to named macro-domain services, which keeps occurrence replay,
// actual-layout recovery, paste spelling, and whole-cover proof code out of the
// engine and out of the planner's main algorithm body.

RefoldMacroOccurrenceReplay RefoldMacroPatchPlanner::OccurrenceReplay() const {
  return RefoldMacroOccurrenceReplay({deps_.aToks, deps_.bTokOff,
                                      deps_.macroTopology, deps_.sourceMapper,
                                      deps_.strict});
}

RefoldMacroActualLayout RefoldMacroPatchPlanner::ActualLayout() const {
  return RefoldMacroActualLayout({deps_.lexLang});
}

RefoldMacroPasteArgumentBuilder
RefoldMacroPatchPlanner::PasteArgumentBuilder() const {
  return RefoldMacroPasteArgumentBuilder(
      {deps_.sourceMapper, deps_.aToks, deps_.bToks, deps_.lexLang});
}

RefoldMacroDefinitionTapeSolver
RefoldMacroPatchPlanner::DefinitionTapeSolver() const {
  return RefoldMacroDefinitionTapeSolver(
      {deps_.model, deps_.aToks, deps_.bToks, deps_.bTokOff, deps_.sourceMapper,
       deps_.proofLattice, deps_.lexLang, deps_.strict});
}

RefoldMacroArgsOnlyTemplateSolver
RefoldMacroPatchPlanner::TemplateSolver() const {
  return RefoldMacroArgsOnlyTemplateSolver(
      {deps_.model, deps_.aToks, deps_.bToks, deps_.bTokOff, deps_.sourceMapper,
       deps_.macroTopology, deps_.proofLattice, deps_.lexLang, deps_.strict});
}

RefoldMacroOccurrenceProofValidator
RefoldMacroPatchPlanner::OccurrenceProofValidator() const {
  return RefoldMacroOccurrenceProofValidator(
      {deps_.model, deps_.macroTopology});
}

// `BuildGeneratedLeafReplayCandidate` no longer has a planner-side
// forwarder; callers go through `GeneratedLeafReplayEngine()`.

bool RefoldMacroPatchPlanner::MacroArgReplacementMatchesAllOccurrencesInB(
    const RefoldModel::MacroInvocation &m, uint32_t argIdx, StringRef baseArg,
    StringRef newArg, ArrayRef<diffutils::Hunk> tokenHunks) const {
  return OccurrenceReplay().MacroArgReplacementMatchesAllOccurrencesInB(
      m, argIdx, baseArg, newArg, tokenHunks);
}

bool RefoldMacroPatchPlanner::
    MacroArgReplacementMatchesAllOccurrencesInBIgnorePaste(
        const RefoldModel::MacroInvocation &m, uint32_t argIdx,
        StringRef baseArg, StringRef newArg,
        ArrayRef<diffutils::Hunk> tokenHunks) const {
  return OccurrenceReplay()
      .MacroArgReplacementMatchesAllOccurrencesInBIgnorePaste(
          m, argIdx, baseArg, newArg, tokenHunks);
}

bool RefoldMacroPatchPlanner::
    MacroArgReplacementMatchesAllOccurrencesInBIgnorePasteSemanticProof(
        const RefoldModel::MacroInvocation &m, uint32_t argIdx,
        StringRef baseArg, StringRef newArg,
        ArrayRef<diffutils::Hunk> tokenHunks) const {
  return OccurrenceReplay()
      .MacroArgReplacementMatchesAllOccurrencesInBIgnorePasteSemanticProof(
          m, argIdx, baseArg, newArg, tokenHunks);
}

std::optional<std::vector<std::pair<size_t, size_t>>>
RefoldMacroPatchPlanner::GetMacroInvocationFormalArgContentRanges(
    const RefoldModel::MacroInvocation &m, StringRef invText) const {
  return ActualLayout().GetMacroInvocationFormalArgContentRanges(m, invText);
}

std::optional<std::pair<uint64_t, uint64_t>>
RefoldMacroPatchPlanner::GetWholeCoverATokRange(
    const RefoldModel::MacroInvocation &m) const {
  return RefoldMacroWholeCoverProof::GetWholeCoverATokRange(m);
}

namespace {
/// Return whether a patch is a structure-preserving callsite rewrite for the
/// current invocation root.  Expanded/materialized replacements intentionally
/// fail this pure predicate and are tracked separately by the reuse context.
static bool macroPatchIsCallsiteForInvocation(
    const MacroPatch &patch, const RefoldModel::MacroInvocation &invocation) {
  return patch.proof.preservesInvocationStructure &&
         patch.proof.proofRootMacroId == invocation.id &&
         RefoldLineObserverLayout::InvocationSpanMatchesCallsitePrefix(
             patch.replacement, invocation);
}
} // namespace

std::optional<RefoldMacroPatchPlanner::InvocationActualLayout>
RefoldMacroPatchPlanner::RecoverInvocationActuals(
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

  auto rangesOpt =
      GetMacroInvocationFormalArgContentRanges(invocation, baseInvocationText);
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

bool RefoldMacroPatchPlanner::InvocationActualsAreRecoverable(
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

std::optional<RefoldMacroPatchPlanner::InvocationRewriteWithRange>
RefoldMacroPatchPlanner::BuildInvocationRewriteWithRange(
    const InvocationActualRecoveryContext &ctx,
    const DenseMap<uint32_t, std::string> &replByArgIdx,
    const DenseMap<uint32_t, std::pair<uint64_t, uint64_t>>
        *materializedRangeByArgIdx) const {
  // Rebuild the invocation by replacing formal-content ranges right-to-left in
  // source order while carrying the exact replacement-relative materialized
  // output interval.  All state required for recovery is supplied explicitly
  // through InvocationActualRecoveryContext.
  if (replByArgIdx.empty())
    return std::nullopt;

  const ArrayRef<std::pair<size_t, size_t>> invocationArgRanges =
      ctx.invocationArgRanges;
  StringRef baseInvocationText = ctx.baseInvocationText;

  SmallVector<uint32_t, 8> keys;
  keys.reserve(replByArgIdx.size());
  for (const auto &entry : replByArgIdx) {
    if (static_cast<size_t>(entry.first) >= invocationArgRanges.size())
      return std::nullopt;
    keys.push_back(entry.first);
  }

  llvm::sort(
      keys,
      [&invocationArgRanges](const uint32_t lhs, const uint32_t rhs) -> bool {
        const auto lhsRange = invocationArgRanges[lhs];
        const auto rhsRange = invocationArgRanges[rhs];
        if (lhsRange.first != rhsRange.first)
          return lhsRange.first < rhsRange.first;
        return lhs < rhs;
      });

  InvocationRewriteWithRange out;
  out.text.reserve(baseInvocationText.size());

  uint64_t cursor = 0;
  std::optional<uint64_t> mappedBegin;
  std::optional<uint64_t> mappedEnd;
  for (uint32_t argIdx : keys) {
    const auto rawRange = invocationArgRanges[argIdx];
    const ActualContentRange r{rawRange.first, rawRange.second};
    if (r.end < r.begin || r.end > baseInvocationText.size() ||
        r.begin < cursor)
      return std::nullopt;

    out.text += baseInvocationText.slice(cursor, r.begin).str();

    auto replIt = replByArgIdx.find(argIdx);
    if (replIt == replByArgIdx.end())
      return std::nullopt;

    const uint64_t replBegin = static_cast<uint64_t>(out.text.size());
    out.text.append(replIt->second);
    const uint64_t replEnd = static_cast<uint64_t>(out.text.size());

    uint64_t materializedBegin = replBegin;
    uint64_t materializedEnd = replEnd;
    if (materializedRangeByArgIdx) {
      auto matIt = materializedRangeByArgIdx->find(argIdx);
      if (matIt != materializedRangeByArgIdx->end()) {
        const uint64_t relBegin = matIt->second.first;
        const uint64_t relEnd = matIt->second.second;
        if (relEnd < relBegin || relEnd > replIt->second.size())
          return std::nullopt;
        materializedBegin = replBegin + relBegin;
        materializedEnd = replBegin + relEnd;
      }
    }

    mappedBegin = mappedBegin ? std::min(*mappedBegin, materializedBegin)
                              : materializedBegin;
    mappedEnd =
        mappedEnd ? std::max(*mappedEnd, materializedEnd) : materializedEnd;

    cursor = r.end;
  }

  out.text += baseInvocationText.substr(cursor).str();
  if (!mappedBegin || !mappedEnd)
    return std::nullopt;

  out.materializedOutputByteStart = *mappedBegin;
  out.materializedOutputByteEnd = *mappedEnd;
  return out;
}

RefoldMacroPatchPlanner::MacroPatchReuseAdmissionContext
RefoldMacroPatchPlanner::RecoverWholeCoverReuseContext(
    const RefoldModel::MacroInvocation &invocation,
    const Owner &currentPatchOwner, uint64_t invocationStart,
    uint64_t invocationEnd,
    const DenseMap<std::optional<uint64_t>, DenseMap<uint64_t, MacroPatch>>
        &patchMap,
    ExistingMacroPatchContext existingContext) const {
  // Reuse discovery runs before whole-cover candidate assembly because
  // same-span reuse must see both the caller-owned patch map and the already
  // coalesced same-pass context. The resulting carrier borrows those accepted
  // patches; it does not mutate map ownership or change the deterministic
  // representative choice.
  MacroPatchReuseAdmissionContext ctx{invocation, currentPatchOwner,
                                      invocation.ownerIncludeId,
                                      invocationStart, invocationEnd};
  CollectExistingMacroPatchReuseFromMap(ctx, patchMap);
  AdmitCallerExistingMacroPatchContext(ctx, existingContext);
  return ctx;
}

bool RefoldMacroPatchPlanner::ExistingPatchMatchesReuseSite(
    const MacroPatchReuseAdmissionContext &ctx, const MacroPatch &patch) const {
  return patch.invRange.begin == ctx.invocationStart &&
         patch.invRange.end == ctx.invocationEnd &&
         MacroPatchOwnerMatches(patch, ctx.currentPatchOwner);
}

bool RefoldMacroPatchPlanner::ExistingPatchPreservesCurrentInvocation(
    const MacroPatchReuseAdmissionContext &ctx, const MacroPatch &patch) const {
  return macroPatchIsCallsiteForInvocation(patch, ctx.invocation);
}

void RefoldMacroPatchPlanner::CollectExistingMacroPatchReuseFromMap(
    MacroPatchReuseAdmissionContext &ctx,
    const DenseMap<std::optional<uint64_t>, DenseMap<uint64_t, MacroPatch>>
        &patchMap) const {
  auto ownerIt = patchMap.find(ctx.ownerIncludeId);
  if (ownerIt == patchMap.end())
    return;

  std::optional<uint64_t> bestNonCallsiteId;
  std::optional<uint64_t> bestCallsiteId;

  for (const auto &kv : ownerIt->second) {
    const uint64_t id = kv.first;
    const MacroPatch &patch = kv.second;

    // Only same physical bytes under the same owner certificate are reusable.
    if (!ExistingPatchMatchesReuseSite(ctx, patch))
      continue;

    if (!ExistingPatchPreservesCurrentInvocation(ctx, patch)) {
      if (!bestNonCallsiteId || id < *bestNonCallsiteId)
        bestNonCallsiteId = id;
    } else {
      if (!bestCallsiteId || id < *bestCallsiteId)
        bestCallsiteId = id;
    }
  }

  if (bestNonCallsiteId) {
    auto it = ownerIt->second.find(*bestNonCallsiteId);
    if (it != ownerIt->second.end())
      ctx.existingExpandedPatch = &it->second;
  }

  if (bestCallsiteId) {
    auto it = ownerIt->second.find(*bestCallsiteId);
    if (it != ownerIt->second.end()) {
      ctx.existingPatch = &it->second;
      ctx.existingIsCallsite =
          ExistingPatchPreservesCurrentInvocation(ctx, it->second);
    }
  }
}

void RefoldMacroPatchPlanner::AdmitCallerExistingMacroPatchContext(
    MacroPatchReuseAdmissionContext &ctx,
    RefoldMacroPatchPlanner::ExistingMacroPatchContext existingContext) const {
  if (ctx.existingPatch || ctx.existingExpandedPatch || !existingContext.patch)
    return;

  const MacroPatch &patch = *existingContext.patch;
  if (!ExistingPatchMatchesReuseSite(ctx, patch))
    return;

  const bool contextIsCallsite =
      existingContext.isCallsite &&
      ExistingPatchPreservesCurrentInvocation(ctx, patch);
  if (contextIsCallsite) {
    ctx.existingPatch = &patch;
    ctx.existingIsCallsite = true;
    ctx.existingCameFromCallerContext = true;
  } else {
    ctx.existingExpandedPatch = &patch;
    ctx.existingExpandedCameFromCallerContext = true;
  }
}

// Certify the B-token envelope corresponding to an invocation's whole A-side
// macro cover.  The helper depends only on planner services plus its explicit
// invocation/patch arguments.
bool RefoldMacroPatchPlanner::CertifyMacroPatchWholeExpansionBRange(
    const RefoldModel::MacroInvocation &m, MacroPatch &patch) const {
  std::optional<std::pair<uint64_t, uint64_t>> cover =
      GetWholeCoverATokRange(m);
  if (!cover)
    return false;

  std::optional<std::pair<size_t, size_t>> bEnv =
      (*deps_.sourceMapper)
          .MapATokRangeAToBTokenEnvelopePreserveBoundaryInsertions(
              cover->first, cover->second);
  if (!bEnv || bEnv->first >= bEnv->second)
    return false;

  patch.materialized.hasBTokenRange = true;
  patch.materialized.bTokStart = static_cast<uint64_t>(bEnv->first);
  patch.materialized.bTokEnd = static_cast<uint64_t>(bEnv->second);
  return true;
}

bool RefoldMacroPatchPlanner::MatchLiteralAToken(
    uint64_t tok, llvm::StringRef spelling) const {
  return tok < deps_.aToks.size() &&
         deps_.aToks[static_cast<size_t>(tok)].spelling == spelling;
}

const RefoldModel::MacroDirective *
RefoldMacroPatchPlanner::ResolveFunctionLikeMacroThroughAliasesWithHops(
    llvm::StringRef startName, uint32_t *aliasHops) const {
  if (aliasHops)
    *aliasHops = 0;
  if (startName.empty())
    return nullptr;
  SmallVector<std::string, 8> seen;
  std::string current = startName.trim().str();
  for (size_t depth = 0; depth <= (*deps_.model).GetMacroDirectives().size();
       ++depth) {
    if (llvm::is_contained(seen, current))
      return nullptr;
    seen.push_back(current);

    const RefoldModel::MacroDirective *functionLike = nullptr;
    const RefoldModel::MacroDirective *alias = nullptr;
    for (const RefoldModel::MacroDirective &directive :
         (*deps_.model).GetMacroDirectives()) {
      if (directive.subkind != "#define" ||
          directive.name != llvm::StringRef(current))
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
    if (aliasHops)
      ++*aliasHops;
    current = alias->replacementTokens[0].spelling.str();
  }
  return nullptr;
}

const RefoldModel::MacroDirective *
RefoldMacroPatchPlanner::ResolveFunctionLikeMacroForReplay(
    llvm::StringRef startName) const {
  return ResolveFunctionLikeMacroThroughAliasesWithHops(startName, nullptr);
}

bool RefoldMacroPatchPlanner::IsObjectLikeSingleTokenAlias(
    llvm::StringRef name) const {
  for (const RefoldModel::MacroDirective &directive :
       (*deps_.model).GetMacroDirectives()) {
    if (directive.subkind != "#define" || directive.name != name ||
        directive.functionLike)
      continue;
    if (directive.replacementTokens.size() == 1 &&
        directive.replacementTokens[0].kind ==
            RefoldModel::MacroReplacementTokenKind::Literal)
      return true;
  }
  return false;
}

bool RefoldMacroPatchPlanner::TokenSpellingsEqualToA(
    llvm::ArrayRef<std::string> expected, uint64_t beginTok,
    uint64_t endTok) const {
  if (endTok < beginTok || endTok > deps_.aToks.size() ||
      endTok - beginTok != expected.size())
    return false;
  for (size_t i = 0; i < expected.size(); ++i) {
    if (deps_.aToks[static_cast<size_t>(beginTok) + i].spelling !=
        llvm::StringRef(expected[i]))
      return false;
  }
  return true;
}

bool RefoldMacroPatchPlanner::TokenSpellingsEqualToB(
    llvm::ArrayRef<std::string> expected, uint64_t beginTok,
    uint64_t endTok) const {
  if (endTok < beginTok || endTok > deps_.bToks.size() ||
      endTok - beginTok != expected.size())
    return false;
  for (size_t i = 0; i < expected.size(); ++i) {
    if (deps_.bToks[static_cast<size_t>(beginTok) + i].spelling !=
        llvm::StringRef(expected[i]))
      return false;
  }
  return true;
}

bool RefoldMacroPatchPlanner::IsParenthesizedTuple(llvm::StringRef arg) const {
  arg = arg.trim();
  if (!arg.starts_with("(") || !arg.ends_with(")") || arg.size() < 2)
    return false;
  SmallVector<TupleElementSlice, 8> elems;
  return splitTopLevelTupleElementsWithLexer(arg.drop_front().drop_back(),
                                             (*deps_.lexLang), elems) &&
         elems.size() >= 2;
}

RefoldMacroPatchPlanner::ArgsOnlyPatchAttempt
RefoldMacroPatchPlanner::BuildPasteAwareArgsOnlyPatch(
    const ArgsOnlyPlanningContext &ctx) const {
  const RefoldModel::MacroInvocation &m = ctx.invocation;
  const diffutils::Hunk &h = ctx.hunk;
  StringRef baseInvText = ctx.baseInvocationText;
  ArrayRef<std::pair<size_t, size_t>> invArgRanges =
      ctx.actualLayout.rangePairs();
  const diffutils::Hunk tokenHunksCurrent[] = {h};
  const InvocationActualRecoveryContext actualRecoveryCtx{m, baseInvText,
                                                          invArgRanges};

  // Fast path for token-paste edits. A single pasted token can embed multiple
  // argument contributions (e.g., X##_##Y##_##Z), so a single edit hunk may
  // change multiple arg segments inside that token (e.g., a_b_c -> d_e_f). In
  // that case we attempt to derive per-arg segment replacements and splice them
  // into the invocation spelling.
  if (!PasteArgumentBuilder().HunkTouchesAnyPasteToken(m, h))
    return ArgsOnlyPatchAttempt::ContinueSearchResult();

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
          replacementIntroducesTopLevelComma(newArg, (*deps_.lexLang)))
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
      if (!MacroArgReplacementMatchesAllOccurrencesInBIgnorePaste(
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

      std::optional<InvocationRewriteWithRange> rewrite =
          BuildInvocationRewriteWithRange(actualRecoveryCtx, replByArgIdx);
      if (!rewrite)
        return ArgsOnlyPatchAttempt::RejectResult();

      {
        MacroPatch patch{*m.invB, *m.invE, std::move(rewrite->text), m.id};
        proofCertifier_.CertifyInvocationRewriteMaterializedOutputRange(
            patch, rewrite->materializedOutputByteStart,
            rewrite->materializedOutputByteEnd);
        // Paste replay validates the rewritten callsite against every pasted
        // token occurrence in the expansion.  For the edit map, therefore,
        // the B-side materialization is the whole expansion cover that the
        // source argument rewrite regenerates, not merely the first changed
        // pasted-token hunk.
        CertifyMacroPatchWholeExpansionBRange(m, patch);
        GetProofLattice().SetMacroPatchProof(
            patch, GetProofLattice().MakeMacroPatchProof(
                       MacroPatchProofKind::ArgsOnlyPasteMulti,
                       /*preservesInvocationStructure=*/true, m.id));
        // The builder already proved this rewrite by replaying the rewritten
        // invocation arguments against every pasted token occurrence in B.
        // Carry that proof source onto the accepted patch for converted
        // selector-site discharge.
        patch.pasteReplayValidated = true;
        GetProofLattice()
            .MacroPatchProofClassifier()
            .SyncMacroPatchProofSummary(patch);
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
        replacementIntroducesTopLevelComma(newArg, (*deps_.lexLang)))
      return ArgsOnlyPatchAttempt::RejectResult();

    // Safety gate: for single-segment paste edits we can directly validate
    // all occurrences, including paste-span occurrences, against the B
    // stream.
    if (!MacroArgReplacementMatchesAllOccurrencesInB(
            m, argIdx, baseArgText, newArg, tokenHunksCurrent)) {
      return ArgsOnlyPatchAttempt::RejectResult();
    }

    DenseMap<uint32_t, std::string> singleReplByArgIdx;
    singleReplByArgIdx[argIdx] = std::move(newArg);
    std::optional<InvocationRewriteWithRange> rewrite =
        BuildInvocationRewriteWithRange(actualRecoveryCtx, singleReplByArgIdx);
    if (!rewrite)
      return ArgsOnlyPatchAttempt::RejectResult();

    {
      MacroPatch patch{*m.invB, *m.invE, std::move(rewrite->text), m.id};
      proofCertifier_.CertifyInvocationRewriteMaterializedOutputRange(
          patch, rewrite->materializedOutputByteStart,
          rewrite->materializedOutputByteEnd);
      // As with the multi-paste path, the source argument rewrite is a
      // compact representation of the macro's replayed expansion surface.
      // Keep the B-side map anchored to that whole expansion envelope.
      CertifyMacroPatchWholeExpansionBRange(m, patch);
      GetProofLattice().SetMacroPatchProof(
          patch, GetProofLattice().MakeMacroPatchProof(
                     MacroPatchProofKind::ArgsOnlyPasteSingle,
                     /*preservesInvocationStructure=*/true, m.id));
      // Single-segment paste rewrites are admitted only after direct replay
      // validation against all touched occurrences in B. Record that proof
      // source explicitly for converted selector-site discharge.
      patch.pasteReplayValidated = true;
      GetProofLattice().MacroPatchProofClassifier().SyncMacroPatchProofSummary(
          patch);
      return ArgsOnlyPatchAttempt::AcceptedResult(std::move(patch));
    }
  }

  // If we touched paste but could not safely derive a paste splice patch,
  // fall through to the standard (non-paste) args-only policy below.

  return ArgsOnlyPatchAttempt::ContinueSearchResult();
}

std::optional<MacroPatch>
RefoldMacroPatchPlanner::BuildMacroInvocationPatchArgsOnly(
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

  return standardArgsOnlyPatchBuilder_.BuildStandardArgsOnlyPatch(planningCtx);
}

Owner RefoldMacroPatchPlanner::NormalizeHunkOwnerForPatch(
    StringRef tuPath, const diffutils::Hunk &h) const {
  Owner owner = deps_.ownerClassifier->ClassifyOwnerWithSegments(tuPath, h);
  const bool mapsToTU =
      deps_.ownerClassifier->HunkMapsToTU(h.aStart, h.aEnd, tuPath);
  if (mapsToTU)
    return Owner::TU(owner.condArmId);
  if (owner.kind == OwnerKind::Include && owner.includeId)
    return Owner::Include(*owner.includeId, owner.condArmId);
  return Owner::Unknown();
}

bool RefoldMacroPatchPlanner::MacroPatchOwnerMatches(const MacroPatch &patch,
                                                     const Owner &owner) const {
  // Only compare against a concrete, single-owner certificate. Mixed-owner
  // patches cannot be treated as belonging to one TU/include/conditional owner.
  if (!patch.ownerCert.present || patch.ownerCert.mixedWitness)
    return false;
  if (!owner.IsTU() && !owner.IsInclude())
    return false;

  const uint8_t wantKind = owner.IsTU() ? 1 : 2;
  if (patch.ownerCert.kindCode != wantKind)
    return false;

  // Match the serialized owner certificate exactly: owner kind, include ID, and
  // optional conditional-arm identity must all agree.
  const uint64_t wantInclude = owner.includeId.value_or(0);
  if (patch.ownerCert.includeId != wantInclude)
    return false;

  if (patch.ownerCert.condArm.present != owner.condArmId.has_value())
    return false;
  if (patch.ownerCert.condArm.present &&
      patch.ownerCert.condArm.armId != owner.condArmId.value())
    return false;

  return true;
}

void RefoldMacroPatchPlanner::CarryMacroPatchOwnerCertificate(
    MacroPatch &dst, const MacroPatch &src) const {
  dst.ownerCert = src.ownerCert;
}

void RefoldMacroPatchPlanner::CertifyMacroPatchOwnerWitness(
    MacroPatch &patch, const Owner &owner) const {
  // Only TU/include owners are representable in the compact legacy patch
  // certificate. More specific owners (macro directives, pragma islands,
  // line-control islands) must be discharged by their own proof class rather
  // than being lossy-serialized as kind code 0.
  if (!owner.IsTU() && !owner.IsInclude())
    return;

  // Serialize the owner into the compact certificate fields stored on
  // MacroPatch. This lets later merge/selection checks compare owner witnesses
  // without retaining the full Owner object.
  const uint8_t kindCode = owner.IsTU() ? 1 : 2;
  const uint64_t includeId = owner.includeId.value_or(0);
  const bool hasCondArm = owner.condArmId.has_value();
  const uint64_t condArmId = hasCondArm ? *owner.condArmId : 0;

  // First concrete witness initializes the certificate.
  if (!patch.ownerCert.present) {
    patch.ownerCert.present = true;
    patch.ownerCert.mixedWitness = false;
    patch.ownerCert.kindCode = kindCode;
    patch.ownerCert.includeId = includeId;
    patch.ownerCert.condArm.present = hasCondArm;
    patch.ownerCert.condArm.armId = condArmId;
    patch.ownerCert.witnessCount = 1;
    return;
  }

  // Additional witnesses must match the original certificate exactly. If any
  // differ, preserve the witness count but mark the patch as mixed-owner so it
  // cannot later be treated as a single-owner rewrite.
  ++patch.ownerCert.witnessCount;
  if (patch.ownerCert.kindCode != kindCode ||
      patch.ownerCert.includeId != includeId ||
      patch.ownerCert.condArm.present != hasCondArm ||
      (hasCondArm && patch.ownerCert.condArm.armId != condArmId)) {
    patch.ownerCert.mixedWitness = true;
  }
}

// === Whole-cover orchestrator delegation seam ===
//
// Whole-cover planning is implemented by RefoldMacroWholeCoverOrchestrator.
// These adapter methods preserve the planner-facing API used by RefoldEngine,
// RefoldServiceGraphBuilder, and RefoldMacroStateRepairPlanner while keeping
// whole-cover policy inside the macro orchestration service.

std::optional<WholeCoverPlan> RefoldMacroPatchPlanner::ComputeWholeCoverPlan(
    const RefoldModel::MacroInvocation &m) const {
  return RefoldMacroWholeCoverOrchestrator(this).ComputeWholeCoverPlan(m);
}

bool RefoldMacroPatchPlanner::WholeCoverPatchMatchesPlan(
    const MacroPatch &patch, const WholeCoverPlan &plan,
    uint64_t rootMacroId) const {
  return RefoldMacroWholeCoverOrchestrator(this).WholeCoverPatchMatchesPlan(
      patch, plan, rootMacroId);
}

std::optional<MacroPatch>
RefoldMacroPatchPlanner::TryCounterLiteralWholeCoverPatch(
    const RefoldModel::MacroInvocation &invocation, const diffutils::Hunk &hunk,
    uint64_t invocationStart, uint64_t invocationEnd) const {
  return RefoldMacroWholeCoverOrchestrator(this)
      .TryCounterLiteralWholeCoverPatch(invocation, hunk, invocationStart,
                                        invocationEnd);
}

std::optional<MacroPatch>
RefoldMacroPatchPlanner::BuildMacroInvocationPatchWholeCover(
    const RefoldModel::MacroInvocation &m, const diffutils::Hunk &h,
    StringRef baseInvText,
    const DenseMap<std::optional<uint64_t>, DenseMap<uint64_t, MacroPatch>>
        &patchMap) const {
  return RefoldMacroWholeCoverOrchestrator(this)
      .BuildMacroInvocationPatchWholeCover(m, h, baseInvText, patchMap);
}

std::optional<MacroPatch>
RefoldMacroPatchPlanner::BuildMacroInvocationPatchWholeCover(
    const RefoldModel::MacroInvocation &m, const diffutils::Hunk &h,
    StringRef baseInvText,
    const DenseMap<std::optional<uint64_t>, DenseMap<uint64_t, MacroPatch>>
        &patchMap,
    RefoldMacroPatchPlanner::ExistingMacroPatchContext existingContext) const {
  return RefoldMacroWholeCoverOrchestrator(this)
      .BuildMacroInvocationPatchWholeCover(m, h, baseInvText, patchMap,
                                           existingContext);
}

} // namespace refold
} // namespace clang
