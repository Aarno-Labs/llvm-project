//===--- RefoldMacroSubtreeReplayValidator.cpp ------------------*- C++ -*-===//
//
// Implementation of the heavy macro-subtree replay-stability validator.  See
// the header for the architectural contract.  The two long lambdas that used
// to live inside `SubtreeReplayDoesNotContradictSiblingSurface`
// (`hasNonForwardedSyntaxOutsideRefs` and `replayStringifyArgumentThroughRoot`)
// are named anon-namespace/private helpers so the public predicate reads as a
// flat sequence of named checks.
//
//===----------------------------------------------------------------------===//

#include "macro/RefoldMacroSubtreeReplayValidator.h"

#include "core/RefoldLog.h"
#include "core/RefoldModel.h"
#include "line-control/RefoldLineObserverLayout.h"
#include "macro/RefoldArgTextRecovery.h"
#include "macro/RefoldMacroPlannerHelpers.h"
#include "macro/RefoldMacroReplay.h"
#include "macro/RefoldMacroTopology.h"
#include "source/RefoldSourceMapper.h"
#include "util/StringUtils.h"

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

/// Sort invocation argument references from right to left for text rewriting.
bool invArgRefGreaterByByteRange(const RefoldModel::InvArgRef &lhs,
                                 const RefoldModel::InvArgRef &rhs) {
  if (lhs.byteBegin != rhs.byteBegin)
    return lhs.byteBegin > rhs.byteBegin;
  return lhs.byteEnd > rhs.byteEnd;
}

/// Recover the spelled text of one invocation argument from the producer-side
/// callsite surface recorded on a macro invocation.
///
/// The producer stores invocation argument byte ranges in TU-relative byte
/// coordinates. The consumer-side `invText` string, however, is a local slice
/// covering only the invocation text itself. This helper therefore rebases the
/// recorded argument byte range through `invB` before slicing `invText`.
///
/// Returns `std::nullopt` when the producer did not record a usable invocation
/// text/range pair for `argIdx`, or when the recorded range does not rebase
/// cleanly into the local invocation surface. Callers must treat failure as a
/// proof failure and remain fail-closed.
std::optional<StringRef>
tryGetInvocationArgText(const RefoldModel::MacroInvocation &mi,
                        unsigned argIdx) {
  if (!mi.invText)
    return std::nullopt;
  if (argIdx >= mi.invArgRanges.size())
    return std::nullopt;

  const auto &range = mi.invArgRanges[argIdx];
  if (!range.first || !range.second)
    return std::nullopt;

  uint64_t byteBegin = *range.first;
  uint64_t byteEnd = *range.second;
  if (byteEnd < byteBegin)
    return std::nullopt;

  if (mi.invB) {
    if (byteBegin < *mi.invB || byteEnd < *mi.invB)
      return std::nullopt;
    byteBegin -= *mi.invB;
    byteEnd -= *mi.invB;
  }

  if (byteEnd > mi.invText->size() || byteBegin > byteEnd)
    return std::nullopt;

  return StringRef(*mi.invText)
      .slice(static_cast<size_t>(byteBegin), static_cast<size_t>(byteEnd));
}

/// Return true only for the narrowly proved higher-order callee-closure case:
///
///   * the callee of a descendant invocation comes from exactly one caller
///     formal slot in `parent`
///   * the spelled invocation text for that slot is exactly the parent formal
///     name itself (for example `F` in `APPLY(F, X)`)
///   * the slot forwards through exactly one `argRef`
///   * the slot has no tuple forwarding witnesses
///   * `argDeps` agrees with that single forwarded caller slot
///
/// This is intentionally narrower than "general higher-order callee closure".
/// We only admit the whole-formal forwarding shape that is explicitly proven
/// by the current producer contract. Any richer shape must continue to fail
/// closed until the producer emits stronger callee-slice provenance.
bool isWholeFormalCallerForwardSlot(const RefoldModel::MacroInvocation &parent,
                                    uint32_t slot) {
  if (slot >= parent.defParams.size())
    return false;

  auto templateText = tryGetInvocationArgText(parent, slot);
  if (!templateText)
    return false;
  if (templateText->trim() != parent.defParams[slot].name)
    return false;

  if (slot >= parent.argRefs.size())
    return false;
  ArrayRef<RefoldModel::InvArgRef> refs(parent.argRefs[slot]);
  if (refs.size() != 1)
    return false;

  if (slot < parent.argTupleRefs.size() && !parent.argTupleRefs[slot].empty())
    return false;

  if (slot >= parent.argDeps.size())
    return false;
  ArrayRef<uint32_t> deps(parent.argDeps[slot]);
  if (deps.size() != 1 || deps.front() != refs.front().callerParamIndex)
    return false;

  return true;
}

/// True iff `argText` carries any non-whitespace syntax that lies outside the
/// argument-reference ranges given by `refs`.  Used during sibling-surface
/// replay to detect arguments whose non-forwarded prefix/suffix would change
/// when re-spelled through the candidate root.
bool hasNonForwardedSyntaxOutsideRefs(StringRef argText,
                                      ArrayRef<RefoldModel::InvArgRef> refs,
                                      size_t argAbsBegin) {
  SmallVector<std::pair<size_t, size_t>, 4> ranges;
  for (const RefoldModel::InvArgRef &ref : refs) {
    if (ref.byteBegin < argAbsBegin || ref.byteEnd < ref.byteBegin)
      continue;
    const size_t relBegin = static_cast<size_t>(ref.byteBegin - argAbsBegin);
    const size_t relEnd = static_cast<size_t>(ref.byteEnd - argAbsBegin);
    if (relEnd > argText.size())
      continue;
    ranges.push_back({relBegin, relEnd});
  }
  llvm::sort(ranges);

  size_t cursor = 0;
  for (const auto &range : ranges) {
    if (range.first > cursor &&
        !argText.slice(cursor, range.first).trim().empty())
      return true;
    cursor = std::max(cursor, range.second);
  }
  return cursor < argText.size() && !argText.drop_front(cursor).trim().empty();
}

// Call sites use the shared `getDefinitionDirectiveForInvocation` inline
// helper from RefoldMacroPlannerHelpers.h.

} // namespace

RefoldMacroSubtreeReplayValidator::RefoldMacroSubtreeReplayValidator(
    Dependencies deps)
    : deps_(std::move(deps)) {}

bool RefoldMacroSubtreeReplayValidator::DirectlyStringifiesFormal(
    const RefoldModel::MacroInvocation &inv, uint32_t argIdx) const {
  const RefoldModel::MacroDirective *definition =
      getDefinitionDirectiveForInvocation(deps_.model, inv);
  if (!definition || argIdx >= definition->defParams.size())
    return false;

  for (size_t i = 0; i + 1 < definition->replacementTokens.size(); ++i) {
    const auto &hash = definition->replacementTokens[i];
    const auto &formal = definition->replacementTokens[i + 1];
    if (hash.spelling == "#" &&
        formal.kind == RefoldModel::MacroReplacementTokenKind::ParamRef &&
        formal.paramIndex && *formal.paramIndex == argIdx)
      return true;
  }
  return false;
}

bool RefoldMacroSubtreeReplayValidator::HunkTouchesASpan(uint64_t begin,
                                                         uint64_t end) const {
  for (const diffutils::Hunk &hunk : deps_.abTokHunks) {
    if (hunk.aStart < end && begin < hunk.aEnd)
      return true;
  }
  return false;
}

bool RefoldMacroSubtreeReplayValidator::SubtreePathHasProvableCalleeClosure(
    const MacroSubtreeReplayValidationContext &ctx,
    const RefoldModel::MacroInvocation &candidate) const {
  // A descendant is replayable through the validated root only when every
  // generated-callee hop is justified either by a literal callee spelling or
  // by the already-proven whole-formal caller-forwarding rule.  Unsupported
  // or broken ancestry remains a fail-closed rejection.
  const RefoldModel::MacroInvocation *cur = &candidate;
  for (;;) {
    if (!hasLiteralMacroCalleeOrigin(*cur)) {
      if (cur->calleeOrigin.kind != MacroCalleeOriginKind::CallerParam ||
          !cur->callerMacroId ||
          cur->calleeOrigin.callerParamIndices.size() != 1)
        return false;

      auto parentIt = ctx.invocationById.find(*cur->callerMacroId);
      if (parentIt == ctx.invocationById.end())
        return false;

      const uint32_t slot = cur->calleeOrigin.callerParamIndices.front();
      if (!isWholeFormalCallerForwardSlot(*parentIt->second, slot))
        return false;
    }

    if (cur->id == ctx.rootInvocation.id)
      return true;
    if (!cur->callerMacroId)
      return false;
    auto it = ctx.invocationById.find(*cur->callerMacroId);
    if (it == ctx.invocationById.end())
      return false;
    cur = it->second;
  }
}

std::optional<std::string>
RefoldMacroSubtreeReplayValidator::ReplayStringifyArgumentThroughRoot(
    const RefoldModel::MacroInvocation &inv, uint32_t argIdx,
    ArrayRef<std::string> rootActuals) const {
  if (!DirectlyStringifiesFormal(inv, argIdx))
    return std::nullopt;
  if (!inv.invText || argIdx >= inv.argRefs.size())
    return std::nullopt;
  auto rangesOpt =
      RefoldMacroActualLayout({&deps_.lexLang})
          .GetMacroInvocationFormalArgContentRanges(inv, *inv.invText);
  if (!rangesOpt || argIdx >= rangesOpt->size())
    return std::nullopt;

  const auto argRange = (*rangesOpt)[argIdx];
  StringRef invText = *inv.invText;
  if (argRange.second > invText.size() || argRange.second < argRange.first)
    return std::nullopt;

  StringRef oldArgText = invText.slice(argRange.first, argRange.second);
  const auto &refs = inv.argRefs[argIdx];
  if (!hasNonForwardedSyntaxOutsideRefs(oldArgText, refs, argRange.first))
    return std::nullopt;

  std::string replayed = oldArgText.str();
  SmallVector<RefoldModel::InvArgRef, 4> sortedRefs;
  sortedRefs.append(refs.begin(), refs.end());
  llvm::sort(sortedRefs, invArgRefGreaterByByteRange);

  for (const RefoldModel::InvArgRef &ref : sortedRefs) {
    if (ref.callerParamIndex >= rootActuals.size())
      return std::nullopt;
    if (ref.byteBegin < argRange.first || ref.byteEnd < ref.byteBegin)
      return std::nullopt;
    const size_t relBegin = static_cast<size_t>(ref.byteBegin - argRange.first);
    const size_t relEnd = static_cast<size_t>(ref.byteEnd - argRange.first);
    if (relEnd > replayed.size())
      return std::nullopt;
    replayed.replace(relBegin, relEnd - relBegin,
                     rootActuals[ref.callerParamIndex]);
  }

  return stringutils::canonicalizeStringifyInversePayload(replayed);
}

bool RefoldMacroSubtreeReplayValidator::
    SubtreeReplayDoesNotContradictSiblingSurface(
        const MacroSubtreeReplayValidationContext &ctx,
        const MacroPatch &patch) const {
  const RefoldModel::MacroInvocation &root = ctx.rootInvocation;

  // Only local DAG subtree-root proofs need this owner-closure check.  Other
  // proof kinds are either already owner-wide or are handled by their own
  // final replay-stability gates.
  if (patch.proof.kind != MacroPatchProofKind::DagSubtreeRoot ||
      !patch.subtree.backed || patch.proof.proofRootMacroId != root.id ||
      !patch.proof.preservesInvocationStructure)
    return true;
  if (!RefoldLineObserverLayout::InvocationSpanMatchesCallsitePrefix(
          patch.replacement, root))
    return true;

  auto rootRangesOpt =
      RefoldMacroActualLayout({&deps_.lexLang})
          .GetMacroInvocationFormalArgContentRanges(root, patch.replacement);
  if (!rootRangesOpt)
    return true;

  SmallVector<std::string, 8> rootActuals;
  rootActuals.reserve(rootRangesOpt->size());
  for (const auto &range : *rootRangesOpt)
    rootActuals.push_back(
        StringRef(patch.replacement).slice(range.first, range.second).str());

  for (const RefoldModel::MacroInvocation &inv :
       deps_.model.GetMacroInvocations()) {
    if (deps_.macroTopology.GetRootMacroId(inv.id) != root.id)
      continue;
    for (const RefoldModel::PPArgSpan &span : inv.stringifySpans) {
      if (!HunkTouchesASpan(span.begin, span.end))
        continue;

      std::optional<std::pair<size_t, size_t>> bEnv =
          deps_.sourceMapper
              .MapATokRangeAToBTokenEnvelopePreserveBoundaryInsertions(
                  span.begin, span.end);
      if (!bEnv || bEnv->second <= bEnv->first)
        continue;

      std::optional<std::string> bPayload =
          deps_.argTextRecovery.UnstringifyLiteralToArgText(
              deps_.sourceMapper.SliceBSource(bEnv->first, bEnv->second),
              /*allowTopLevelComma=*/true);
      if (!bPayload)
        continue;
      std::optional<std::string> canonicalB =
          stringutils::canonicalizeStringifyInversePayload(*bPayload);
      if (!canonicalB)
        continue;

      std::optional<std::string> replayed =
          ReplayStringifyArgumentThroughRoot(inv, span.argIdx, rootActuals);
      if (!replayed)
        continue;

      if (*replayed != *canonicalB) {
        REFOLD_LOG_TRACE(
            "macro/proof",
            "suppress DAG subtree root replay: sibling direct stringify "
            "surface would change under candidate root invocation inv id={0} "
            "name={1} stringifyMacro={2} stringifyName={3} span=[{4},{5}) "
            "replayed='{6}' bPayload='{7}' replacement='{8}'",
            root.id, root.name, inv.id, inv.name, span.begin, span.end,
            stringutils::showWsWithClip(*replayed, 160),
            stringutils::showWsWithClip(*canonicalB, 160),
            stringutils::showWsWithClip(patch.replacement, 220));
        return false;
      }
    }
  }

  return true;
}

bool RefoldMacroSubtreeReplayValidator::ClaimedWholeEnvelopeIsReplaySafe(
    const MacroSubtreeReplayValidationContext &ctx,
    const MacroPatch &patch) const {
  // A claimed whole-envelope replay backed only by a local DAG subtree proof is
  // admissible only if the replay remains closed over sibling observer
  // surfaces. Keep this wrapper separate so additional whole-envelope gates can
  // be added without changing final-candidate order.
  return SubtreeReplayDoesNotContradictSiblingSurface(ctx, patch);
}

} // namespace refold
} // namespace clang
