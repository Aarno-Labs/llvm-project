//===--- RefoldMacroPatchPlanner.cpp ----------------------------*- C++ -*-===//
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
#include "macro/RefoldMacroWholeCoverPlanBuilder.h"
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
                                      deps_.strict, deps_.lexLang});
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
      {deps_.model, deps_.aToks, deps_.bToks, deps_.sourceMapper,
       deps_.proofLattice, deps_.lexLang});
}


namespace {

/// One proven replacement for an exact byte slice inside a root tuple actual.
///
/// The byte coordinates are relative to the complete source-spelled root actual
/// including its outer parentheses.  Multiple terminal children may prove the
/// same slice; they must agree byte-for-byte, and overlapping distinct slices
/// are rejected so tuple reconstruction never chooses an arbitrary owner.
struct TupleSiblingTerminalConstraint {
  uint32_t rootArgIdx = 0;
  uint32_t byteBegin = 0;
  uint32_t byteEnd = 0;
  std::string replacement;
};

/// Cached replay facts for one literal terminal child of a tuple-forwarding
/// root.  The sibling-terminal theorem needs to compare root-level forwarded
/// argument spans with child tuple references; keeping the recovered child
/// actuals next to the child definition avoids recomputing them and lets root
/// standard spans be resolved through the same producer tuple facts as paste
/// and stringification spans.
struct TupleSiblingTerminalChildInfo {
  const RefoldModel::MacroInvocation *child = nullptr;
  const RefoldModel::MacroDirective *definition = nullptr;
  SmallVector<std::string, 8> oldActuals;
};

bool spanCoversHunk(const RefoldModel::PPSpan &span,
                    const diffutils::Hunk &hunk) {
  return span.begin <= hunk.aStart && hunk.aEnd <= span.end;
}

bool addTupleSiblingTerminalConstraint(
    llvm::SmallVectorImpl<TupleSiblingTerminalConstraint> &constraints,
    uint32_t rootArgIdx, uint32_t byteBegin, uint32_t byteEnd,
    llvm::StringRef replacement) {
  if (byteEnd < byteBegin)
    return false;

  for (const TupleSiblingTerminalConstraint &existing : constraints) {
    if (existing.rootArgIdx != rootArgIdx)
      continue;
    const bool overlaps = byteBegin < existing.byteEnd &&
                          existing.byteBegin < byteEnd;
    if (!overlaps)
      continue;

    // Repeated terminals are allowed to prove the exact same tuple element,
    // but they must agree on the replacement spelling.  Partial overlaps would
    // require a nested edit composition theorem and therefore fail closed here.
    if (existing.byteBegin != byteBegin || existing.byteEnd != byteEnd ||
        existing.replacement != replacement)
      return false;
    return true;
  }

  constraints.push_back(TupleSiblingTerminalConstraint{
      rootArgIdx, byteBegin, byteEnd, replacement.trim().str()});
  return true;
}

const RefoldModel::MacroDirective *findDefinitionDirectiveById(
    const RefoldModel &model, uint64_t directiveId) {
  const RefoldModel::MacroDirective *directive =
      model.GetMacroDirectiveById(directiveId);
  if (!directive || !directive->IsDefine())
    return nullptr;
  return directive;
}

std::optional<std::string> recoverNormalizedChildActual(
    const RefoldModel::MacroInvocation &child, uint32_t argIdx) {
  if (!child.normalizedInvText || argIdx >= child.normalizedInvArgTextRanges.size())
    return std::nullopt;

  const auto &range = child.normalizedInvArgTextRanges[argIdx];
  if (!range.first || !range.second || *range.second < *range.first ||
      *range.second > child.normalizedInvText->size())
    return std::nullopt;

  return child.normalizedInvText
      ->slice(static_cast<size_t>(*range.first),
              static_cast<size_t>(*range.second))
      .trim()
      .str();
}

std::optional<RefoldModel::TupleArgRef> recoverUniqueTupleRefForChildFormal(
    const RefoldModel::MacroInvocation &child, uint32_t argIdx) {
  if (argIdx >= child.argTupleRefs.size() ||
      child.argTupleRefs[argIdx].size() != 1)
    return std::nullopt;
  return child.argTupleRefs[argIdx].front();
}

bool tupleRefMatchesOldActual(llvm::StringRef rootActualText,
                              const RefoldModel::TupleArgRef &tupleRef,
                              llvm::StringRef oldActual) {
  if (tupleRef.callerByteEnd < tupleRef.callerByteBegin ||
      tupleRef.callerByteEnd > rootActualText.size())
    return false;
  return rootActualText
             .slice(tupleRef.callerByteBegin, tupleRef.callerByteEnd)
             .trim() == oldActual.trim();
}

bool tupleSiblingTerminalConstraintReverseLess(
    const TupleSiblingTerminalConstraint &lhs,
    const TupleSiblingTerminalConstraint &rhs) {
  if (lhs.byteBegin != rhs.byteBegin)
    return lhs.byteBegin > rhs.byteBegin;
  return lhs.byteEnd > rhs.byteEnd;
}

bool addChildTupleSiblingTerminalConstraint(
    llvm::SmallVectorImpl<TupleSiblingTerminalConstraint> &constraints,
    const RefoldModel::MacroInvocation &child,
    const RefoldModel::MacroDirective &childDefinition,
    llvm::ArrayRef<std::string> oldChildActuals,
    llvm::ArrayRef<std::pair<size_t, size_t>> rootArgRanges,
    llvm::StringRef baseInvocationText, uint32_t argIdx,
    llvm::StringRef replacement, const clang::LangOptions &lexLang) {
  if (argIdx >= oldChildActuals.size() ||
      argIdx >= childDefinition.defParams.size())
    return false;

  std::optional<RefoldModel::TupleArgRef> tupleRef =
      recoverUniqueTupleRefForChildFormal(child, argIdx);
  if (!tupleRef || tupleRef->callerParamIndex >= rootArgRanges.size())
    return false;

  const auto rootRange = rootArgRanges[tupleRef->callerParamIndex];
  if (rootRange.second < rootRange.first ||
      rootRange.second > baseInvocationText.size())
    return false;
  llvm::StringRef rootActualText =
      baseInvocationText.slice(rootRange.first, rootRange.second);
  if (!tupleRefMatchesOldActual(rootActualText, *tupleRef,
                                oldChildActuals[argIdx]))
    return false;

  if (!childDefinition.defParams[argIdx].variadic &&
      replacementIntroducesTopLevelComma(replacement, lexLang))
    return false;

  return addTupleSiblingTerminalConstraint(
      constraints, tupleRef->callerParamIndex, tupleRef->callerByteBegin,
      tupleRef->callerByteEnd, replacement);
}


bool tupleSiblingConstraintSameSlice(
    const TupleSiblingTerminalConstraint &lhs,
    const TupleSiblingTerminalConstraint &rhs) {
  return lhs.rootArgIdx == rhs.rootArgIdx && lhs.byteBegin == rhs.byteBegin &&
         lhs.byteEnd == rhs.byteEnd;
}

bool addRootForwardedTupleSiblingTerminalConstraint(
    llvm::SmallVectorImpl<TupleSiblingTerminalConstraint> &constraints,
    llvm::ArrayRef<TupleSiblingTerminalChildInfo> childInfos,
    llvm::ArrayRef<std::pair<size_t, size_t>> rootArgRanges,
    llvm::StringRef baseInvocationText,
    const RefoldModel::PPArgSpan &rootSpan, llvm::StringRef oldContribution,
    llvm::StringRef replacement, const clang::LangOptions &lexLang) {
  if (rootSpan.argIdx >= rootArgRanges.size())
    return false;

  SmallVector<TupleSiblingTerminalConstraint, 4> candidates;
  for (const TupleSiblingTerminalChildInfo &childInfo : childInfos) {
    if (!childInfo.child || !childInfo.definition)
      return false;
    for (uint32_t argIdx = 0; argIdx < childInfo.oldActuals.size(); ++argIdx) {
      std::optional<RefoldModel::TupleArgRef> tupleRef =
          recoverUniqueTupleRefForChildFormal(*childInfo.child, argIdx);
      if (!tupleRef || tupleRef->callerParamIndex != rootSpan.argIdx ||
          tupleRef->callerParamIndex >= rootArgRanges.size())
        continue;

      const auto rootRange = rootArgRanges[tupleRef->callerParamIndex];
      if (rootRange.second < rootRange.first ||
          rootRange.second > baseInvocationText.size())
        return false;
      llvm::StringRef rootActualText =
          baseInvocationText.slice(rootRange.first, rootRange.second);
      if (!tupleRefMatchesOldActual(rootActualText, *tupleRef,
                                    childInfo.oldActuals[argIdx]))
        continue;
      if (oldContribution.trim() !=
          llvm::StringRef(childInfo.oldActuals[argIdx]).trim())
        continue;

      if (argIdx >= childInfo.definition->defParams.size())
        return false;
      if (!childInfo.definition->defParams[argIdx].variadic &&
          replacementIntroducesTopLevelComma(replacement, lexLang))
        return false;

      TupleSiblingTerminalConstraint candidate;
      candidate.rootArgIdx = tupleRef->callerParamIndex;
      candidate.byteBegin = tupleRef->callerByteBegin;
      candidate.byteEnd = tupleRef->callerByteEnd;
      candidate.replacement = replacement.trim().str();

      bool duplicate = false;
      for (const TupleSiblingTerminalConstraint &existing : candidates) {
        if (!tupleSiblingConstraintSameSlice(existing, candidate))
          continue;
        if (existing.replacement != candidate.replacement)
          return false;
        duplicate = true;
        break;
      }
      if (!duplicate)
        candidates.push_back(std::move(candidate));
    }
  }

  if (candidates.empty())
    return false;

  // A root-level standard span on a forwarded tuple formal does not identify a
  // tuple element by itself.  It becomes safe only when the child tuple refs
  // identify exactly one distinct source slice.  Repeated terminal children may
  // prove the same slice; different matching slices would be ambiguous and must
  // not be guessed from token spelling.
  if (candidates.size() != 1)
    return false;

  const TupleSiblingTerminalConstraint &candidate = candidates.front();
  return addTupleSiblingTerminalConstraint(
      constraints, candidate.rootArgIdx, candidate.byteBegin,
      candidate.byteEnd, candidate.replacement);
}

} // namespace

RefoldMacroArgsOnlyTemplateSolver
RefoldMacroPatchPlanner::TemplateSolver() const {
  return RefoldMacroArgsOnlyTemplateSolver(
      {deps_.model, deps_.aToks, deps_.bToks, deps_.bTokOff, deps_.sourceMapper,
       deps_.macroTopology, deps_.proofLattice, deps_.lexLang});
}

RefoldMacroOccurrenceProofValidator
RefoldMacroPatchPlanner::OccurrenceProofValidator() const {
  return RefoldMacroOccurrenceProofValidator(
      {deps_.model, deps_.macroTopology});
}

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

bool RefoldMacroPatchPlanner::DerivedReplacementsReproduceStringifiedOperands(
    const RefoldModel::MacroInvocation &m,
    const DenseMap<uint32_t, std::string> &replacementsByArgIdx) const {
  if (m.stringifySpans.empty())
    return true;
  if (!deps_.sourceMapper || !deps_.argTextRecovery) {
    // Without both services the stringified operand cannot be recovered from B,
    // so agreement is unproven rather than true.
    return false;
  }

  for (const RefoldModel::PPArgSpan &stringifySpan : m.stringifySpans) {
    auto replacement = replacementsByArgIdx.find(stringifySpan.argIdx);
    if (replacement == replacementsByArgIdx.end())
      continue;

    // The stringified operand is one B token: the literal the edited stream
    // actually spells at this position.
    auto bEnv =
        deps_.sourceMapper->MapAToBTokenEnvelopeByPPArgSpan(stringifySpan);
    if (!bEnv || bEnv->second <= bEnv->first ||
        bEnv->second - bEnv->first != 1) {
      return false;
    }

    StringRef bStringifiedToken =
        deps_.sourceMapper->SliceBSource(bEnv->first, bEnv->second).trim();
    std::optional<std::string> decoded =
        deps_.argTextRecovery->UnstringifyLiteralToArgText(
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

/// An argument replacement re-spelled in the original call-site bytes.
///
/// `text` carries exactly the token sequence the caller certified.  The
/// half-open byte range `[newBegin, newEnd)` of the certified replacement was
/// copied verbatim and now starts at `resultBegin` in `text`; every byte
/// outside it came from the original argument spelling.
struct RespelledArgumentReplacement {
  std::string text;
  uint64_t newBegin = 0;
  uint64_t newEnd = 0;
  uint64_t resultBegin = 0;
};

/// Re-spell a certified argument replacement using the original call-site
/// argument spelling wherever the two agree token-for-token.
///
/// `newArgText` is derived from the modified preprocessed stream B, where an
/// expansion occupies a single physical line.  Splicing it verbatim over an
/// argument whose source spelling spanned several lines collapses the
/// invocation onto one line.  That collapse is observable: `__LINE__` in the
/// callee's replacement list takes the line of the invocation's *closing
/// paren*, so a lost newline moves the observer and the closing check rejects
/// the assembly.  Interior indentation and comments are lost the same way.
///
/// The repair aligns the two token sequences from both ends and copies only
/// the differing interior out of `newArgText`, keeping the original bytes
/// around it.
///
/// Proof obligation: the result must carry exactly the token sequence that was
/// certified, namely `newArgText`'s.  That is discharged by re-lexing the
/// spliced text and requiring kind-and-spelling equality token for token, so
/// every way the splice could change meaning -- a retained comment that
/// swallows it, tokens gluing at a seam, an original spelling that is not
/// token-identical outside the interior -- returns `std::nullopt` and leaves
/// the caller with the uncollapsed B text.  This is a spelling choice only; it
/// admits nothing and rejects nothing.
std::optional<RespelledArgumentReplacement>
respellArgumentReplacementInBaseSpelling(StringRef baseArgText,
                                         StringRef newArgText,
                                         const LangOptions &lexLang) {
  SmallVector<RefoldLexBoundaryToken, 16> baseToks;
  SmallVector<RefoldLexBoundaryToken, 16> newToks;
  refoldLexBoundaryTokens(baseArgText, lexLang, baseToks);
  refoldLexBoundaryTokens(newArgText, lexLang, newToks);

  const size_t baseCount = baseToks.size();
  const size_t newCount = newToks.size();
  if (baseCount == 0 || newCount == 0)
    return std::nullopt;

  auto sameToken = [](const RefoldLexBoundaryToken &lhs,
                      const RefoldLexBoundaryToken &rhs) -> bool {
    return lhs.kind == rhs.kind && lhs.spelling == rhs.spelling;
  };

  size_t prefix = 0;
  while (prefix < baseCount && prefix < newCount &&
         sameToken(baseToks[prefix], newToks[prefix]))
    ++prefix;

  // The original spelling already carries the certified tokens; keep its bytes
  // whole rather than re-deriving them from B.
  if (prefix == baseCount && prefix == newCount) {
    RespelledArgumentReplacement identical;
    identical.text = baseArgText.str();
    return identical;
  }

  size_t suffix = 0;
  while (suffix < baseCount - prefix && suffix < newCount - prefix &&
         sameToken(baseToks[baseCount - 1 - suffix],
                   newToks[newCount - 1 - suffix]))
    ++suffix;

  // A pure insertion or deletion leaves one side's differing interval empty,
  // which has no byte range to splice against.  Give up one matched token at a
  // time -- left edge first, so the choice stays deterministic -- until both
  // intervals name real tokens.
  size_t baseMidEndTok = baseCount - suffix;
  size_t newMidEndTok = newCount - suffix;
  while (prefix >= baseMidEndTok || prefix >= newMidEndTok) {
    if (prefix > 0) {
      --prefix;
      continue;
    }
    if (suffix > 0) {
      --suffix;
      baseMidEndTok = baseCount - suffix;
      newMidEndTok = newCount - suffix;
      continue;
    }
    return std::nullopt;
  }

  RespelledArgumentReplacement out;
  const uint64_t baseMidBegin = baseToks[prefix].begin;
  const uint64_t baseMidEnd = baseToks[baseMidEndTok - 1].end;
  out.newBegin = newToks[prefix].begin;
  out.newEnd = newToks[newMidEndTok - 1].end;
  out.resultBegin = baseMidBegin;

  out.text = baseArgText.substr(0, baseMidBegin).str();
  out.text += newArgText.substr(out.newBegin, out.newEnd - out.newBegin);
  out.text += baseArgText.substr(baseMidEnd);

  SmallVector<RefoldLexBoundaryToken, 16> resultToks;
  refoldLexBoundaryTokens(out.text, lexLang, resultToks);
  if (resultToks.size() != newCount)
    return std::nullopt;
  for (size_t i = 0; i < newCount; ++i)
    if (!sameToken(resultToks[i], newToks[i]))
      return std::nullopt;

  return out;
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

    out.text += baseInvocationText.slice(cursor, r.begin);

    auto replIt = replByArgIdx.find(argIdx);
    if (replIt == replByArgIdx.end())
      return std::nullopt;

    StringRef replacementText = replIt->second;

    std::optional<std::pair<uint64_t, uint64_t>> materializedRel;
    if (materializedRangeByArgIdx) {
      auto matIt = materializedRangeByArgIdx->find(argIdx);
      if (matIt != materializedRangeByArgIdx->end()) {
        if (matIt->second.second < matIt->second.first ||
            matIt->second.second > replacementText.size())
          return std::nullopt;
        materializedRel = matIt->second;
      }
    }

    // The replacement is derived from B, where the whole expansion sits on one
    // physical line.  An argument whose original spelling spanned several lines
    // must not be collapsed onto one: `__LINE__` in the callee's replacement
    // list observes the line of the invocation's closing paren, so the collapse
    // would move a preserved line observer.  Re-spell the certified replacement
    // in the original argument's bytes when the two agree token for token
    // outside the edit; the helper fails closed to the B text otherwise.
    const StringRef baseArgText = baseInvocationText.slice(r.begin, r.end);
    std::string respelledStorage;
    if (baseArgText.contains('\n')) {
      if (auto respelled = respellArgumentReplacementInBaseSpelling(
              baseArgText, replacementText, *deps_.lexLang)) {
        std::optional<std::pair<uint64_t, uint64_t>> remapped;
        if (materializedRel && materializedRel->first >= respelled->newBegin &&
            materializedRel->second <= respelled->newEnd) {
          const uint64_t remappedBegin =
              respelled->resultBegin +
              (materializedRel->first - respelled->newBegin);
          const uint64_t remappedEnd =
              remappedBegin +
              (materializedRel->second - materializedRel->first);
          remapped = std::make_pair(remappedBegin, remappedEnd);
        }

        // A materialized output interval that does not land inside the spliced
        // region has no proven image in the re-spelled bytes, so keep the
        // uncollapsed replacement for that argument rather than guess one.
        if (!materializedRel || remapped) {
          respelledStorage = std::move(respelled->text);
          replacementText = respelledStorage;
          materializedRel = remapped;
        }
      }
    }

    const uint64_t replBegin = static_cast<uint64_t>(out.text.size());
    out.text.append(replacementText.begin(), replacementText.end());
    const uint64_t replEnd = static_cast<uint64_t>(out.text.size());

    uint64_t materializedBegin = replBegin;
    uint64_t materializedEnd = replEnd;
    if (materializedRel) {
      materializedBegin = replBegin + materializedRel->first;
      materializedEnd = replBegin + materializedRel->second;
    }

    mappedBegin = mappedBegin ? std::min(*mappedBegin, materializedBegin)
                              : materializedBegin;
    mappedEnd =
        mappedEnd ? std::max(*mappedEnd, materializedEnd) : materializedEnd;

    cursor = r.end;
  }

  out.text += baseInvocationText.substr(cursor);
  if (!mappedBegin || !mappedEnd)
    return std::nullopt;

  if (!InvocationRewritePreservesLineObservers(ctx, out.text))
    return std::nullopt;

  out.materializedOutputByteStart = *mappedBegin;
  out.materializedOutputByteEnd = *mappedEnd;
  return out;
}

bool RefoldMacroPatchPlanner::InvocationRewritePreservesLineObservers(
    const InvocationActualRecoveryContext &ctx,
    StringRef rewrittenInvocationText) const {
  if (!deps_.macroTopology->ExpansionContainsLineObserver(ctx.invocation.id))
    return true;

  const size_t baseLineBreaks = ctx.baseInvocationText.count('\n');
  const size_t rewrittenLineBreaks = rewrittenInvocationText.count('\n');
  if (baseLineBreaks == rewrittenLineBreaks)
    return true;

  REFOLD_LOG_DEBUG(
      "macro/line-observer",
      "refusing callsite rewrite of inv id={0} name={1}: a __LINE__ expands "
      "inside it and the rewrite spans {2} line break(s) where the recorded "
      "spelling spans {3}",
      ctx.invocation.id, ctx.invocation.name,
      static_cast<uint64_t>(rewrittenLineBreaks),
      static_cast<uint64_t>(baseLineBreaks));
  return false;
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
  // The leading name may carry callsite whitespace; every later hop is already
  // an exact producer replacement-token spelling.
  return resolveFunctionLikeMacroThroughObjectAliases(
      *deps_.model, startName.trim(), aliasHops);
}

const RefoldModel::MacroDirective *
RefoldMacroPatchPlanner::ResolveFunctionLikeMacroForReplay(
    llvm::StringRef startName) const {
  return ResolveFunctionLikeMacroThroughAliasesWithHops(startName, nullptr);
}

bool RefoldMacroPatchPlanner::IsObjectLikeSingleTokenAlias(
    llvm::StringRef name) const {
  return isObjectLikeSingleTokenAliasName(*deps_.model, name);
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
  SmallVector<diffutils::Hunk, 4> tokenHunksInInvocationCover;
  if (deps_.abTokHunks) {
    for (const diffutils::Hunk &candidateHunk : *deps_.abTokHunks) {
      if (m.Covers(candidateHunk.aStart, candidateHunk.aEnd))
        tokenHunksInInvocationCover.push_back(candidateHunk);
    }
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
  if (deps_.argTextRecovery) {
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

      auto bEnv = deps_.sourceMapper->MapAToBTokenEnvelopeByPPArgSpan(
          stringifySpan);
      if (!bEnv || bEnv->second <= bEnv->first ||
          bEnv->second - bEnv->first != 1)
        continue;

      StringRef bStringifiedToken = deps_.sourceMapper
                                        ->SliceBSource(bEnv->first,
                                                       bEnv->second)
                                        .trim();
      std::optional<std::string> decoded =
          deps_.argTextRecovery->UnstringifyLiteralToArgText(
              bStringifiedToken, /*allowTopLevelComma=*/true);
      if (!decoded)
        continue;
      std::optional<std::string> canonical =
          stringutils::canonicalizeStringifyInversePayload(*decoded);
      if (!canonical)
        continue;

      std::string newArg = StringRef(*canonical).trim().str();
      if (!isMacroInvocationVariadicFormal(m, argIdx) &&
          replacementIntroducesTopLevelComma(newArg, (*deps_.lexLang)))
        continue;

      auto range = invArgRanges[argIdx];
      StringRef baseArgText =
          baseInvText.substr(range.first, range.second - range.first);
      if (!MacroArgReplacementMatchesAllOccurrencesInBIgnorePaste(
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
  }

  if (!stringifyConstrainedReplacements.empty() &&
      PasteArgumentBuilder().PasteArgReplacementsMatchAllPasteTokensInB(
          m, baseInvText, invArgRanges, stringifyConstrainedReplacements)) {
    std::optional<InvocationRewriteWithRange> rewrite =
        BuildInvocationRewriteWithRange(actualRecoveryCtx,
                                        stringifyConstrainedReplacements);
    if (!rewrite)
      return ArgsOnlyPatchAttempt::RejectResult();

    MacroPatch patch{*m.invB, *m.invE, std::move(rewrite->text), m.id};
    proofCertifier_.CertifyInvocationRewriteMaterializedOutputRange(
        patch, rewrite->materializedOutputByteStart,
        rewrite->materializedOutputByteEnd);
    CertifyMacroPatchWholeExpansionBRange(m, patch);
    MacroPatchProof proof = GetProofLattice().MakeMacroPatchProof(
        MacroPatchProofKind::ArgsOnlyPasteMulti,
        /*preservesInvocationStructure=*/true, m.id);
    proof.paste->replayValidated = true;
    GetProofLattice().SetMacroPatchProof(patch, std::move(proof));
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
      // Validate the locally derived paste replacement only against the hunk
      // that produced it.  Repeated paste macros may contain many independent
      // pasted products for the same formal; widening this per-segment check
      // to the whole invocation cover can make one local derivation reject
      // because other pasted products have not yet contributed their own local
      // splice.  The complete replay obligation is still discharged below by
      // `PasteArgReplacementsMatchAllPasteTokensInB`, after all derived
      // replacements for this hunk have been collected.
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
        MacroPatchProof proof = GetProofLattice().MakeMacroPatchProof(
            MacroPatchProofKind::ArgsOnlyPasteMulti,
            /*preservesInvocationStructure=*/true, m.id);
        // The builder already proved this rewrite by replaying the rewritten
        // invocation arguments against every pasted token occurrence in B.
        // Carry that proof source onto the accepted patch for converted
        // selector-site discharge.
        proof.paste->replayValidated = true;
        GetProofLattice().SetMacroPatchProof(patch, std::move(proof));
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
      MacroPatchProof proof = GetProofLattice().MakeMacroPatchProof(
          MacroPatchProofKind::ArgsOnlyPasteSingle,
          /*preservesInvocationStructure=*/true, m.id);
      // Single-segment paste rewrites are admitted only after direct replay
      // validation against all touched occurrences in B. Record that proof
      // source explicitly for converted selector-site discharge.
      proof.paste->replayValidated = true;
      GetProofLattice().SetMacroPatchProof(patch, std::move(proof));
      return ArgsOnlyPatchAttempt::AcceptedResult(std::move(patch));
    }
  }

  // If we touched paste but could not safely derive a paste splice patch,
  // fall through to the standard (non-paste) args-only policy below.

  return ArgsOnlyPatchAttempt::ContinueSearchResult();
}


std::optional<MacroPatch>
RefoldMacroPatchPlanner::TryBuildTupleSiblingTerminalReplayPatch(
    const RefoldModel::MacroInvocation &m, StringRef baseInvText) const {
  if (m.subkind != "func" || !m.invB || !m.invE || !deps_.abTokHunks)
    return std::nullopt;

  std::optional<InvocationActualLayout> actualLayout =
      RecoverInvocationActuals(m, baseInvText);
  if (!actualLayout)
    return std::nullopt;
  ArrayRef<std::pair<size_t, size_t>> invArgRanges =
      actualLayout->rangePairs();
  if (invArgRanges.empty())
    return std::nullopt;

  SmallVector<diffutils::Hunk, 8> coverHunks;
  for (const diffutils::Hunk &candidateHunk : *deps_.abTokHunks) {
    if (m.Covers(candidateHunk.aStart, candidateHunk.aEnd))
      coverHunks.push_back(candidateHunk);
  }
  if (coverHunks.empty())
    return std::nullopt;

  SmallVector<TupleSiblingTerminalChildInfo, 8> childInfos;
  for (const RefoldModel::MacroInvocation &candidateChild :
       deps_.model->GetMacroInvocations()) {
    if (!candidateChild.callerMacroId || *candidateChild.callerMacroId != m.id)
      continue;
    if (candidateChild.subkind != "func" ||
        !candidateChild.definitionDirectiveId ||
        candidateChild.calleeOrigin.kind != MacroCalleeOriginKind::LiteralMacroName)
      return std::nullopt;

    const RefoldModel::MacroDirective *childDefinition =
        findDefinitionDirectiveById(*deps_.model,
                                    *candidateChild.definitionDirectiveId);
    if (!childDefinition || !childDefinition->functionLike ||
        childDefinition->defParams.size() != candidateChild.defParams.size())
      return std::nullopt;

    TupleSiblingTerminalChildInfo childInfo;
    childInfo.child = &candidateChild;
    childInfo.definition = childDefinition;
    childInfo.oldActuals.reserve(childDefinition->defParams.size());
    for (uint32_t argIdx = 0; argIdx < childDefinition->defParams.size();
         ++argIdx) {
      std::optional<std::string> oldActual =
          recoverNormalizedChildActual(candidateChild, argIdx);
      if (!oldActual)
        return std::nullopt;
      childInfo.oldActuals.push_back(std::move(*oldActual));
    }
    childInfos.push_back(std::move(childInfo));
  }
  if (childInfos.size() < 2)
    return std::nullopt;

  SmallVector<TupleSiblingTerminalConstraint, 8> constraints;
  SmallVector<bool, 8> explainedHunks(coverHunks.size(), false);
  bool usedPasteOrStringifyEvidence = false;

  // A root argument span on a tuple-forwarding macro can represent a standard
  // occurrence inside one of the literal sibling terminals.  Producers may not
  // duplicate that standard span on the child invocation when the child was
  // materialized from a tuple formal (`DECL t`).  Resolve such spans through the
  // child arg_tuple_refs, and accept only if those refs identify one exact tuple
  // slice; token spelling alone is never enough to choose among repeated tuple
  // elements.
  for (const RefoldModel::PPArgSpan &span : m.argSpans) {
    for (size_t hunkIdx = 0; hunkIdx < coverHunks.size(); ++hunkIdx) {
      const diffutils::Hunk &coverHunk = coverHunks[hunkIdx];
      if (!spanCoversHunk(span, coverHunk))
        continue;
      std::optional<std::pair<size_t, size_t>> bEnv =
          deps_.sourceMapper->MapAToBTokenEnvelopeByPPArgSpan(span);
      if (!bEnv || bEnv->second <= bEnv->first)
        return std::nullopt;
      StringRef oldContribution =
          deps_.sourceMapper->SliceASource(span.begin, span.end).trim();
      StringRef newContribution =
          deps_.sourceMapper->SliceBSource(bEnv->first, bEnv->second).trim();
      if (!addRootForwardedTupleSiblingTerminalConstraint(
              constraints, childInfos, invArgRanges, baseInvText, span,
              oldContribution, newContribution, *deps_.lexLang))
        return std::nullopt;
      explainedHunks[hunkIdx] = true;
    }
  }

  for (const TupleSiblingTerminalChildInfo &childInfo : childInfos) {
    const RefoldModel::MacroInvocation &child = *childInfo.child;
    const RefoldModel::MacroDirective &childDefinition = *childInfo.definition;
    ArrayRef<std::string> oldChildActuals(childInfo.oldActuals);

    for (const RefoldModel::PPArgSpan &span : child.argSpans) {
      for (size_t hunkIdx = 0; hunkIdx < coverHunks.size(); ++hunkIdx) {
        const diffutils::Hunk &coverHunk = coverHunks[hunkIdx];
        if (!spanCoversHunk(span, coverHunk))
          continue;
        if (span.argIdx >= oldChildActuals.size())
          return std::nullopt;
        std::optional<std::pair<size_t, size_t>> bEnv =
            deps_.sourceMapper->MapAToBTokenEnvelopeByPPArgSpan(span);
        if (!bEnv || bEnv->second <= bEnv->first)
          return std::nullopt;
        StringRef oldContribution =
            deps_.sourceMapper->SliceASource(span.begin, span.end).trim();
        if (oldContribution != StringRef(oldChildActuals[span.argIdx]).trim())
          return std::nullopt;
        StringRef newContribution =
            deps_.sourceMapper->SliceBSource(bEnv->first, bEnv->second).trim();
        if (!addChildTupleSiblingTerminalConstraint(
                constraints, child, childDefinition, oldChildActuals,
                invArgRanges, baseInvText, span.argIdx, newContribution,
                *deps_.lexLang))
          return std::nullopt;
        explainedHunks[hunkIdx] = true;
      }
    }

    for (const RefoldModel::PPArgSpan &span : child.stringifySpans) {
      for (size_t hunkIdx = 0; hunkIdx < coverHunks.size(); ++hunkIdx) {
        const diffutils::Hunk &coverHunk = coverHunks[hunkIdx];
        if (!spanCoversHunk(span, coverHunk))
          continue;
        std::optional<std::pair<size_t, size_t>> bEnv =
            deps_.sourceMapper->MapAToBTokenEnvelopeByPPArgSpan(span);
        if (!bEnv || bEnv->second <= bEnv->first ||
            bEnv->second - bEnv->first != 1)
          return std::nullopt;
        StringRef bString =
            deps_.sourceMapper->SliceBSource(bEnv->first, bEnv->second).trim();
        std::optional<std::string> decoded =
            deps_.argTextRecovery->UnstringifyLiteralToArgText(
                bString, /*allowTopLevelComma=*/true);
        if (!decoded)
          return std::nullopt;
        std::optional<std::string> canonical =
            stringutils::canonicalizeStringifyInversePayload(*decoded);
        if (!canonical || !addChildTupleSiblingTerminalConstraint(
                constraints, child, childDefinition, oldChildActuals,
                invArgRanges, baseInvText, span.argIdx, *canonical,
                *deps_.lexLang))
          return std::nullopt;
        usedPasteOrStringifyEvidence = true;
        explainedHunks[hunkIdx] = true;
      }
    }

    for (size_t hunkIdx = 0; hunkIdx < coverHunks.size(); ++hunkIdx) {
      const diffutils::Hunk &coverHunk = coverHunks[hunkIdx];
      bool explainedByPaste = false;
      std::optional<std::vector<PasteArgEdit>> pasteEdits =
          PasteArgumentBuilder().DerivePasteArgEdits(child, coverHunk);
      if (pasteEdits) {
        for (const PasteArgEdit &edit : *pasteEdits) {
          if (edit.argIdx >= oldChildActuals.size())
            return std::nullopt;
          StringRef baseArg = oldChildActuals[edit.argIdx];
          std::string newArg =
              (edit.argByteBegin && edit.argByteEnd)
                  ? RefoldMacroPasteSpelling::
                        SplicePasteSegmentIntoSpellingArgExact(
                            baseArg, *edit.argByteBegin, *edit.argByteEnd,
                            edit.oldSeg, edit.newSeg)
                  : RefoldMacroPasteSpelling::SplicePasteSegmentIntoSpellingArg(
                        baseArg, edit.oldSeg, edit.newSeg);
          if (!addChildTupleSiblingTerminalConstraint(
                  constraints, child, childDefinition, oldChildActuals,
                  invArgRanges, baseInvText, edit.argIdx, newArg,
                  *deps_.lexLang))
            return std::nullopt;
          explainedByPaste = true;
        }
      }

      std::optional<PasteArgEdit> pasteEdit =
          PasteArgumentBuilder().DerivePasteArgEdit(child, coverHunk);
      if (pasteEdit) {
        if (pasteEdit->argIdx >= oldChildActuals.size())
          return std::nullopt;
        StringRef baseArg = oldChildActuals[pasteEdit->argIdx];
        std::string newArg =
            (pasteEdit->argByteBegin && pasteEdit->argByteEnd)
                ? RefoldMacroPasteSpelling::SplicePasteSegmentIntoSpellingArgExact(
                      baseArg, *pasteEdit->argByteBegin, *pasteEdit->argByteEnd,
                      pasteEdit->oldSeg, pasteEdit->newSeg)
                : RefoldMacroPasteSpelling::SplicePasteSegmentIntoSpellingArg(
                      baseArg, pasteEdit->oldSeg, pasteEdit->newSeg);
        if (!addChildTupleSiblingTerminalConstraint(
                constraints, child, childDefinition, oldChildActuals,
                invArgRanges, baseInvText, pasteEdit->argIdx, newArg,
                *deps_.lexLang))
          return std::nullopt;
        explainedByPaste = true;
      }

      if (explainedByPaste) {
        usedPasteOrStringifyEvidence = true;
        explainedHunks[hunkIdx] = true;
      }
    }
  }

  for (bool explained : explainedHunks)
    if (!explained)
      return std::nullopt;

  if (constraints.empty() || !usedPasteOrStringifyEvidence)
    return std::nullopt;

  DenseMap<uint32_t, SmallVector<TupleSiblingTerminalConstraint, 4>>
      constraintsByRootArg;
  for (const TupleSiblingTerminalConstraint &constraint : constraints)
    constraintsByRootArg[constraint.rootArgIdx].push_back(constraint);

  DenseMap<uint32_t, std::string> replacementByRootArg;
  for (auto &entry : constraintsByRootArg) {
    const uint32_t rootArgIdx = entry.first;
    if (rootArgIdx >= invArgRanges.size())
      return std::nullopt;
    const auto rootRange = invArgRanges[rootArgIdx];
    if (rootRange.second < rootRange.first ||
        rootRange.second > baseInvText.size())
      return std::nullopt;

    std::string rewrittenRootArg =
        baseInvText.slice(rootRange.first, rootRange.second).str();
    llvm::SmallVectorImpl<TupleSiblingTerminalConstraint> &rootConstraints =
        entry.second;
    llvm::sort(rootConstraints, tupleSiblingTerminalConstraintReverseLess);

    uint32_t previousBegin = std::numeric_limits<uint32_t>::max();
    for (const TupleSiblingTerminalConstraint &constraint : rootConstraints) {
      if (constraint.byteEnd < constraint.byteBegin ||
          constraint.byteEnd > rewrittenRootArg.size())
        return std::nullopt;
      if (previousBegin != std::numeric_limits<uint32_t>::max() &&
          constraint.byteEnd > previousBegin)
        return std::nullopt;
      previousBegin = constraint.byteBegin;
      rewrittenRootArg = stringutils::replaceRange(
          rewrittenRootArg, constraint.byteBegin, constraint.byteEnd,
          constraint.replacement);
    }

    if (StringRef(rewrittenRootArg).trim() !=
        baseInvText.slice(rootRange.first, rootRange.second).trim())
      replacementByRootArg[rootArgIdx] = std::move(rewrittenRootArg);
  }

  if (replacementByRootArg.empty())
    return std::nullopt;

  InvocationActualRecoveryContext actualRecoveryCtx{m, baseInvText,
                                                    invArgRanges};
  std::optional<InvocationRewriteWithRange> rewrite =
      BuildInvocationRewriteWithRange(actualRecoveryCtx, replacementByRootArg,
                                      /*materializedRangeByArgIdx=*/nullptr);
  if (!rewrite)
    return std::nullopt;

  MacroPatch patch{*m.invB, *m.invE, std::move(rewrite->text), m.id};
  proofCertifier_.CertifyInvocationRewriteMaterializedOutputRange(
      patch, rewrite->materializedOutputByteStart,
      rewrite->materializedOutputByteEnd);
  CertifyMacroPatchWholeExpansionBRange(m, patch);
  proofCertifier_.SetArgsOnlyStandardProof(
      patch, m, /*wholeEnvelopeReplayValidated=*/true,
      /*definitionTapeReplayValidated=*/false);
  GetProofLattice().MacroPatchProofClassifier().SyncMacroPatchProofSummary(
      patch);
  return patch;
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

// === Whole-cover delegation seam ===
//
// Whole-cover *orchestration* is implemented by
// RefoldMacroWholeCoverOrchestrator; whole-cover *plan computation* is
// implemented by RefoldMacroWholeCoverPlanBuilder, which the service graph
// constructs before the planner.  This adapter preserves the planner-facing
// API used by RefoldEngine and RefoldMacroStateRepairPlanner.

std::optional<WholeCoverPlan> RefoldMacroPatchPlanner::ComputeWholeCoverPlan(
    const RefoldModel::MacroInvocation &m) const {
  return deps_.wholeCoverPlanBuilder->ComputeWholeCoverPlan(m);
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
