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

#include "edit/RefoldBInsertionLedger.h"
#include "macro/RefoldArgTextRecovery.h"
#include "macro/RefoldMacroPlannerHelpers.h"
#include "macro/RefoldMacroReplay.h"
#include "macro/RefoldMacroStateProof.h"
#include "macro/RefoldMacroTupleHelpers.h"
#include "macro/RefoldMacroWholeCoverPlanBuilder.h"
#include "proof/RefoldMacroPatchProofClassifier.h"
#include "proof/RefoldOwnerStateProof.h"
#include "source/RefoldOwnerClassifier.h"
#include "source/TokenTextHelpers.h"
#include "support/RefoldLog.h"
#include "support/StringUtils.h"

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
      proofCertifier_(RefoldMacroPatchProofCertifier::Dependencies{
          *deps_.macroPatchProofClassifier, *deps_.acceptedCandidateBuilder}),
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
              *deps_.model,
              *deps_.sourceMapper,
              *deps_.lexLang,
              *deps_.macroTopology,
              *deps_.argTextRecovery,
              *deps_.bInsertionLedger,
              deps_.aToks,
              deps_.bToks,
              deps_.bSource,
              deps_.bTokOff,
              *deps_.abTokHunks,
              proofCertifier_,
              *deps_.macroPatchProofClassifier,
              *deps_.witnessTrace,
              generatedCalleeReplayEngine_,
              generatedLeafReplayEngine_,
              deps_.strict,
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
  assert(deps_.macroPatchProofClassifier &&
         "macro planner requires the macro-patch proof classifier");
  assert(deps_.acceptedCandidateBuilder &&
         "macro planner requires the accepted-candidate builder");
  assert(deps_.acceptedResultRanker &&
         "macro planner requires the accepted-result ranker");
  assert(deps_.witnessTrace && "macro planner requires the witness trace");
}

//===----------------------------------------------------------------------===//
// Borrowed proof service accessors
//===----------------------------------------------------------------------===//

RefoldOwnerStateProof &RefoldMacroPatchPlanner::GetOwnerStateProof() const {
  assert(deps_.ownerStateProof && "macro planner requires owner-state proof");
  return *deps_.ownerStateProof;
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
         invocationSpanMatchesCallsitePrefix(patch.replacement, invocation);
}

/// An argument replacement re-spelled in the original call-site bytes.
///
/// `text` carries exactly the token sequence the caller certified.  The
/// half-open byte range `[newBegin, newEnd)` of the certified replacement now
/// occupies `[resultBegin, resultEnd)` in `text`; every byte outside it came
/// from the original argument spelling.  The range was copied verbatim unless
/// `interiorRespaced` is set, in which case some of its inter-token whitespace
/// was replaced and only the whole range has a known image.
struct RespelledArgumentReplacement {
  std::string text;
  uint64_t newBegin = 0;
  uint64_t newEnd = 0;
  uint64_t resultBegin = 0;
  uint64_t resultEnd = 0;
  bool interiorRespaced = false;
};

/// Re-spell the replaced interior of a multi-line argument so it keeps the
/// line breaks the original interior had.
///
/// The interior is tokens `[interiorBegin, baseInteriorEnd)` of the original
/// argument and `[interiorBegin, newInteriorEnd)` of the new one.  Each gap
/// between two original interior tokens that holds a line break is copied
/// verbatim over one gap of the new interior: the gap at the nearest token
/// index, the earlier one on a tie, each used at most once.
///
/// Only a new gap that already holds whitespace is used, so the copied bytes,
/// which are whitespace and comments, still stringify to a single space.  A gap
/// followed by `#` is never used, because the `#` would then begin a line
/// inside the invocation and be read as a directive.  Returns `std::nullopt`
/// when the original interior holds no line break, when a line break sits
/// inside a token (a line splice has no gap to move to), or when too few new
/// gaps qualify.  The caller re-lexes the result against the certified tokens,
/// including whether each token is preceded by white space, which also rejects
/// a copied gap that a line splice leaves without any.
std::optional<std::string>
carryInteriorLineBreaks(StringRef baseArgText,
                        ArrayRef<RefoldLexBoundaryToken> baseToks,
                        size_t baseInteriorEnd, StringRef newArgText,
                        ArrayRef<RefoldLexBoundaryToken> newToks,
                        size_t newInteriorEnd, size_t interiorBegin) {
  SmallVector<std::pair<size_t, StringRef>, 4> breakingGaps;
  size_t gapBreaks = 0;
  for (size_t k = interiorBegin + 1; k < baseInteriorEnd; ++k) {
    StringRef gap = baseArgText.slice(baseToks[k - 1].end, baseToks[k].begin);
    if (!gap.contains('\n'))
      continue;
    breakingGaps.push_back({k - interiorBegin, gap});
    gapBreaks += gap.count('\n');
  }
  if (breakingGaps.empty())
    return std::nullopt;
  const StringRef baseInterior = baseArgText.slice(
      baseToks[interiorBegin].begin, baseToks[baseInteriorEnd - 1].end);
  if (gapBreaks != baseInterior.count('\n'))
    return std::nullopt;

  const size_t newTokCount = newInteriorEnd - interiorBegin;
  SmallVector<std::optional<StringRef>, 8> assigned(newTokCount);
  for (const auto &[baseGap, gapText] : breakingGaps) {
    std::optional<size_t> best;
    for (size_t gap = 1; gap < newTokCount; ++gap) {
      const RefoldLexBoundaryToken &before = newToks[interiorBegin + gap - 1];
      const RefoldLexBoundaryToken &after = newToks[interiorBegin + gap];
      if (assigned[gap] || before.end == after.begin || after.kind == tok::hash)
        continue;
      const size_t distance = gap > baseGap ? gap - baseGap : baseGap - gap;
      const size_t bestDistance =
          best ? (*best > baseGap ? *best - baseGap : baseGap - *best) : 0;
      if (!best || distance < bestDistance)
        best = gap;
    }
    if (!best)
      return std::nullopt;
    assigned[*best] = gapText;
  }

  std::string text;
  for (size_t gap = 0; gap < newTokCount; ++gap) {
    const RefoldLexBoundaryToken &token = newToks[interiorBegin + gap];
    if (gap > 0)
      text += assigned[gap]
                  ? *assigned[gap]
                  : newArgText.slice(newToks[interiorBegin + gap - 1].end,
                                     token.begin);
    text += newArgText.slice(token.begin, token.end);
  }
  return text;
}

/// Re-spell a certified argument replacement using the original call-site
/// argument spelling wherever the two agree token-for-token.
///
/// `newArgText` is derived from the modified preprocessed stream B, which has
/// no comments and puts an expansion on a single physical line.  Splicing it
/// verbatim over the argument drops every comment and the original spacing,
/// even around tokens the edit did not touch.  For an argument whose source
/// spelling spanned several lines it also collapses the invocation onto one
/// line, which is observable: `__LINE__` in the callee's replacement list
/// takes the line of the invocation's *closing paren*, so a lost newline moves
/// the observer and the closing check rejects the assembly.
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
///
/// With \p keepInteriorLineBreaks, an interior that lost line breaks is also
/// re-spaced by `carryInteriorLineBreaks`, so a whole-actual replacement keeps
/// the original line count.  The caller asks for it only when a line observer
/// expands inside the invocation, where that count is observable.
///
/// When the formal is \p stringified, the kept original gaps must also leave
/// `#` unchanged: equal tokens with different white space between them
/// stringify differently.  The result is then required to stringify exactly as
/// `newArgText` does, or `std::nullopt` is returned.
std::optional<RespelledArgumentReplacement>
respellArgumentReplacementInBaseSpelling(StringRef baseArgText,
                                         StringRef newArgText,
                                         const LangOptions &lexLang,
                                         bool keepInteriorLineBreaks,
                                         bool stringified) {
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
    if (stringified &&
        stringizeMacroArgumentLikeClang(baseArgText, &lexLang) !=
            stringizeMacroArgumentLikeClang(newArgText, &lexLang))
      return std::nullopt;
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

  std::string interior =
      newArgText.substr(out.newBegin, out.newEnd - out.newBegin).str();
  if (keepInteriorLineBreaks) {
    if (std::optional<std::string> respaced = carryInteriorLineBreaks(
            baseArgText, baseToks, baseMidEndTok, newArgText, newToks,
            newMidEndTok, prefix)) {
      interior = std::move(*respaced);
      out.interiorRespaced = true;
    }
  }

  out.text = baseArgText.substr(0, baseMidBegin).str();
  out.text += interior;
  out.resultEnd = out.text.size();
  out.text += baseArgText.substr(baseMidEnd);

  SmallVector<RefoldLexBoundaryToken, 16> resultToks;
  refoldLexBoundaryTokens(out.text, lexLang, resultToks);
  if (resultToks.size() != newCount)
    return std::nullopt;
  for (size_t i = 0; i < newCount; ++i) {
    if (!sameToken(resultToks[i], newToks[i]))
      return std::nullopt;
    // Re-spacing swaps the white space between interior tokens for original
    // gaps, so it must leave each of those tokens' leading-space fact alone:
    // `#` stringifies exactly that.
    if (out.interiorRespaced && i > prefix && i < newMidEndTok &&
        resultToks[i].leadingSpace != newToks[i].leadingSpace)
      return std::nullopt;
  }
  if (stringified && stringizeMacroArgumentLikeClang(out.text, &lexLang) !=
                         stringizeMacroArgumentLikeClang(newArgText, &lexLang))
    return std::nullopt;

  return out;
}
} // namespace

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

    // The replacement is derived from B, which has no comments and puts the
    // whole expansion on one physical line.  Re-spell the certified
    // replacement in the original argument's bytes wherever the two agree
    // token for token, so comments, spacing and line breaks outside the edit
    // survive.  Line breaks are observable: `__LINE__` in the callee's
    // replacement list takes the line of the invocation's closing paren.  The
    // helper fails closed to the B text.
    const StringRef baseArgText = baseInvocationText.slice(r.begin, r.end);
    std::string respelledStorage;
    const bool stringified = llvm::any_of(
        ctx.invocation.stringifySpans, [&](const RefoldModel::PPArgSpan &span) {
          return span.argIdx == argIdx;
        });
    if (auto respelled = respellArgumentReplacementInBaseSpelling(
            baseArgText, replacementText, *deps_.lexLang,
            deps_.macroTopology->ExpansionContainsLineObserver(
                ctx.invocation.id),
            stringified)) {
      std::optional<std::pair<uint64_t, uint64_t>> remapped;
      if (respelled->interiorRespaced) {
        // Re-spacing moved bytes inside the interior, so only an interval
        // covering exactly the whole interior keeps a known image.
        if (materializedRel &&
            *materializedRel ==
                std::make_pair(respelled->newBegin, respelled->newEnd))
          remapped =
              std::make_pair(respelled->resultBegin, respelled->resultEnd);
      } else if (materializedRel &&
                 materializedRel->first >= respelled->newBegin &&
                 materializedRel->second <= respelled->newEnd) {
        const uint64_t remappedBegin =
            respelled->resultBegin +
            (materializedRel->first - respelled->newBegin);
        const uint64_t remappedEnd =
            remappedBegin + (materializedRel->second - materializedRel->first);
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

std::optional<MacroPatch>
RefoldMacroPatchPlanner::TryBuildTupleSiblingTerminalReplayPatch(
    const RefoldModel::MacroInvocation &m, StringRef baseInvText) const {
  if (m.subkind != "func" || !m.invB || !m.invE || !deps_.abTokHunks)
    return std::nullopt;

  std::optional<InvocationActualLayout> actualLayout =
      standardArgsOnlyPatchBuilder_.RecoverInvocationActuals(m, baseInvText);
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
      PasteArgEditsResult pasteEdits =
          PasteArgumentBuilder().DerivePasteArgEdits(child, coverHunk);

      // A proved-ambiguous paste origin refuses the whole cover: the
      // single-segment derivation below would otherwise re-derive the same
      // argument from the raw token diff and add a constraint this derivation
      // declined to justify.
      if (pasteEdits.kind == PasteArgDerivation::AmbiguousOrigin)
        return std::nullopt;

      {
        for (const PasteArgEdit &edit : pasteEdits.edits) {
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
  deps_.macroPatchProofClassifier->SyncMacroPatchProofSummary(patch);
  return patch;
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
