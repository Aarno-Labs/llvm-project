//===--- RefoldMacroDAGLiftingPhase.cpp ------------------------*- C++ -*-===//
//
// DAG lifting phase.  Given a whole-cover planning context and the
// leaf-discovery result, lift discovered leaf candidates back up to the
// root invocation and, when a unique root-level patch can be certified,
// return it.  Text and lexical mechanics live in five sub-services
// (text primitives, invertibility solver, structured lifter, subtree
// certifier, candidate validator) constructed in the ctor.
//
//===----------------------------------------------------------------------===//

#include "macro/RefoldMacroDAGLiftingPhase.h"

#include "core/RefoldLog.h"
#include "core/RefoldModel.h"
#include "line-control/LineDirectiveInserter.h"
#include "line-control/RefoldLineObserverLayout.h"
#include "macro/RefoldArgTextRecovery.h"
#include "macro/RefoldMacroDAGLeafDiscoveryPhase.h"
#include "macro/RefoldMacroDAGSharedHelpers.h"
#include "macro/RefoldMacroOccurrenceProofValidator.h"
#include "macro/RefoldMacroPasteArgumentBuilder.h"
#include "macro/RefoldMacroPlannerHelpers.h"
#include "macro/RefoldMacroReplay.h"
#include "macro/RefoldMacroTopology.h"
#include "macro/RefoldMacroTupleHelpers.h"
#include "macro/RefoldMacroWholeCoverPlanningContext.h"
#include "proof/RefoldProofLattice.h"
#include "source/RefoldSourceMapper.h"
#include "source/TokenTextHelpers.h"
#include "util/StringUtils.h"

#include "clang/Basic/SourceLocation.h"
#include "clang/Basic/TokenKinds.h"
#include "clang/Lex/Lexer.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/Support/FormatVariadic.h"
#include "llvm/Support/MemoryBuffer.h"

#include <algorithm>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

using namespace llvm;

namespace clang {
namespace refold {

RefoldMacroDAGLiftingPhase::RefoldMacroDAGLiftingPhase(Dependencies deps)
    : deps_(std::move(deps)),
      textPrimitives_(RefoldMacroDAGTextPrimitives::Dependencies{
          deps_.model, deps_.sourceMapper, deps_.lexLang, deps_.argTextRecovery,
          deps_.proofLattice, deps_.getMacroInvocationFormalArgContentRanges}),
      invertibilitySolver_(RefoldMacroDAGInvertibilitySolver::Dependencies{
          textPrimitives_, deps_.sourceMapper, deps_.aToks, deps_.bToks,
          deps_.lexLang, deps_.argTextRecovery,
          deps_.getMacroInvocationFormalArgContentRanges,
          deps_
              .macroArgReplacementMatchesAllOccurrencesInBIgnorePasteSemanticProof}),
      structuredLifter_(RefoldMacroDAGStructuredLifter::Dependencies{
          deps_.model, textPrimitives_, invertibilitySolver_,
          deps_.sourceMapper, deps_.aToks, deps_.bToks, deps_.lexLang,
          deps_.argTextRecovery, deps_.macroTopology,
          deps_.getMacroInvocationFormalArgContentRanges}),
      subtreeCertifier_(RefoldMacroDAGSubtreeCertifier::Dependencies{
          textPrimitives_, structuredLifter_}),
      candidateValidator_(RefoldMacroDAGCandidateValidator::Dependencies{
          textPrimitives_, invertibilitySolver_, structuredLifter_,
          subtreeCertifier_, deps_.sourceMapper, deps_.bToks,
          deps_.argTextRecovery, deps_.macroTopology, deps_.proofLattice,
          deps_.lexLang, deps_.getMacroInvocationFormalArgContentRanges,
          deps_.computeWholeCoverPlan}) {}

RefoldMacroPasteArgumentBuilder
RefoldMacroDAGLiftingPhase::pasteArgumentBuilder() const {
  return RefoldMacroPasteArgumentBuilder(
      {&deps_.sourceMapper, deps_.aToks, deps_.bToks, &deps_.lexLang});
}

std::optional<MacroPatch> RefoldMacroDAGLiftingPhase::Run(
    const RefoldMacroWholeCoverPlanningContext &planningCtx,
    const DAGLeafDiscoveryResult &discoveryResult) const {
  // Unpack the planning context and discovery-phase outputs into the
  // named locals that the lifting body threads into the sub-services.
  // All references borrow from caller-owned storage.
  const RefoldModel::MacroInvocation &m = planningCtx.m;
  const diffutils::Hunk &h = planningCtx.h;
  const diffutils::Hunk &hEff = planningCtx.hEff;
  [[maybe_unused]] StringRef baseInvText = planningCtx.baseInvText;
  ArrayRef<RefoldModel::PPArgSpan> argLikeSpans = planningCtx.argLikeSpans;
  const bool rootHasDirectArgLikeSurface =
      planningCtx.rootHasDirectArgLikeSurface;
  const uint64_t invStart = planningCtx.invStart;
  const uint64_t invEnd = planningCtx.invEnd;

  StringRef invSpanText = discoveryResult.invSpanText;
  ArrayRef<std::pair<size_t, size_t>> invArgRanges =
      discoveryResult.invArgRanges;
  const size_t numArgs = invArgRanges.size();
  const auto &invById = discoveryResult.invById;
  const MacroSubtreeReplayValidationContext subtreeValidationCtx{
      m, invSpanText, invArgRanges, invById};
  const RefoldModel::MacroInvocation &rootInvocation =
      subtreeValidationCtx.rootInvocation;
  StringRef rootInvocationText = subtreeValidationCtx.rootInvocationText;
  ArrayRef<std::pair<size_t, size_t>> rootInvocationArgRanges =
      subtreeValidationCtx.rootInvocationArgRanges;
  const auto &leafCands = discoveryResult.leafCands;
  const auto &splitInsertionRootCandidates =
      discoveryResult.splitInsertionRootCandidates;
  [[maybe_unused]] const auto &rootArgText = discoveryResult.rootArgText;

  // Short local aliases used at the leaf/split candidate loops below.
  using LeafCandidate = DAGLeafCandidate;
  using SplitInsertionRootCandidate = DAGSplitInsertionRootCandidate;

  // Build the per-call lifting context that the sub-services take by
  // reference.  Each field borrows from caller-owned storage; nothing
  // here is owned.
  SmallVector<diffutils::Hunk, 1> tokenHunksForLiftingCtx;
  tokenHunksForLiftingCtx.push_back(h);
  const ArrayRef<diffutils::Hunk> tokenHunksARForCtx(tokenHunksForLiftingCtx);
  const RefoldMacroDAGLiftingContext liftingCtx{
      m,
      h,
      hEff,
      baseInvText,
      argLikeSpans,
      rootHasDirectArgLikeSurface,
      invStart,
      invEnd,
      invSpanText,
      invArgRanges,
      invById,
      subtreeValidationCtx,
      rootInvocation,
      rootInvocationText,
      rootInvocationArgRanges,
      llvm::ArrayRef<DAGLeafCandidate>(leafCands.data(), leafCands.size()),
      llvm::ArrayRef<DAGSplitInsertionRootCandidate>(
          splitInsertionRootCandidates.data(),
          splitInsertionRootCandidates.size()),
      rootArgText,
      tokenHunksARForCtx};

  // Wrap the current hunk in a single-element carrier because the
  // subtree replay-validation entry points take an `ArrayRef<Hunk>`
  // rather than a bare hunk reference.
  SmallVector<diffutils::Hunk, 1> tokenHunksForCheck;
  tokenHunksForCheck.push_back(h);
  ArrayRef<diffutils::Hunk> tokenHunksAR(tokenHunksForCheck);

  // --- Leaf lifting, validation, and uniqueness --------------------------
  //
  // We scan leaf candidates (deepest-first) and attempt to produce a root
  // invocation patch. We accept only if:
  //   * all lifted edits validate against B (occurrence matching), and
  //   * the resulting root patch is unique (no second distinct patch).
  std::optional<MacroPatch> uniquePatch;
  std::optional<std::string> uniquePatchBaseText;
  DagCandidateValidationMetadata uniquePatchValidation;
  unsigned distinctRootPatches = 0;

  DagCandidateAcceptanceContext dagCandidateAcceptanceCtx{
      subtreeValidationCtx, uniquePatch, uniquePatchBaseText,
      uniquePatchValidation, distinctRootPatches};

  // Replay any root-level split-insertion candidates that were proven while
  // scanning paired pure-insertion envelopes.
  //
  // These candidates already carry a concrete replacement for the full root
  // callsite text (for example, a reconstructed `WRAP(INC, (1) + 3)`), but
  // they still need to pass through the normal DAG candidate validation and
  // merge path so they compete consistently with any other root patches.
  for (const SplitInsertionRootCandidate &candidate :
       splitInsertionRootCandidates) {
    // Re-derive the expected root-formal rewrite map directly from the
    // candidate's replacement text. This gives the DAG validator the same
    // root-formal expectations it would have had if this candidate had been
    // produced through the ordinary replay path.
    auto replayRootFormals =
        structuredLifter_.BuildRootFormalRewriteMapFromCallsiteReplacement(
            liftingCtx, invSpanText, StringRef(candidate.patch.replacement));
    if (!replayRootFormals) {
      continue;
    }

    // Validate this split-root candidate against the exact set of root
    // formals implied by the reconstructed replacement. Also defer
    // occurrence-level consistency checks for the touched root formals so
    // the validator can discharge them using the final reconstructed root
    // callsite text rather than rejecting too early.
    DagCandidateValidationMetadata splitValidation;
    splitValidation.hasExpectedRootFormals = true;
    splitValidation.expectedRootFormals = *replayRootFormals;
    splitValidation.deferOccurrenceArgIdxs.assign(
        candidate.deferOccurrenceArgIdxs.begin(),
        candidate.deferOccurrenceArgIdxs.end());

    // Materialize a normal root patch from the queued split candidate.
    // Mark it as already proof-backed: the split-insertion logic has
    // already established that this is a structure-preserving args-only
    // root rewrite.
    MacroPatch splitRootPatch = candidate.patch;
    deps_.proofLattice.SetMacroPatchProof(
        splitRootPatch, deps_.proofLattice.MakeMacroPatchProof(
                            MacroPatchProofKind::ArgsOnlyPairedPureInsertion,
                            /*preservesInvocationStructure=*/true, m.id));

    // Feed the candidate through the shared DAG acceptance/merge logic so
    // it is deduplicated and checked for incompatibility exactly the same
    // way as other DAG-derived root patches.
    auto acceptCert = candidateValidator_.AcceptOrMergeDAGCandidatePatch(
        liftingCtx, dagCandidateAcceptanceCtx, std::move(splitRootPatch),
        invSpanText, "DAG split insertion root patch", &splitValidation);
    if (!acceptCert.accepted)
      return std::nullopt;
  }

  // Examine each candidate leaf invocation that may explain the edited
  // expansion rooted at `m`.
  //
  // For each leaf, this loop:
  //
  //   1. collects observed per-formal edits from reliable arg-like spans,
  //   2. reconstructs edits from unreliable pasted-token subranges when a
  //      unique segmentation can be proven,
  //   3. certifies those observed edits as leaf-formal rewrites,
  //   4. validates paste-sensitive leaf edits,
  //   5. handles the special chained-call suffix case, and otherwise
  //   6. builds a full bottom-up subtree certificate and tries to accept or
  //      merge the resulting root patch.
  //
  // Each leaf is fail-closed: if any local proof obligation fails, the loop
  // skips that leaf and continues looking for another certifiable witness.
  for (const LeafCandidate &cand : leafCands) {
    const RefoldModel::MacroInvocation &leaf = *cand.inv;

    DenseMap<uint32_t, SmallVector<ObservedFormalConstraint, 2>> leafObserved;
    DenseMap<uint32_t, OldNewText> leafEdits;
    DenseMap<uint64_t, SmallVector<const RefoldModel::PPArgSpan *, 4>>
        unreliPaste;
    bool invalid = false;

    // Record one normalized observed old/new constraint for a leaf formal.
    // Duplicate observations are harmless; divergent observations are
    // handled later by the formal certificate builder.
    auto recordLeafObserved = [&](uint32_t argIdx, StringRef oldText,
                                  StringRef newText) -> bool {
      auto &constraints = leafObserved[argIdx];
      for (const auto &existing : constraints) {
        if (existing.oldText == oldText && existing.newText == newText)
          return true;
      }
      constraints.push_back(
          ObservedFormalConstraint{oldText.str(), newText.str()});
      return true;
    };

    // --- Pass 1: collect reliable per-formal edits -----------------------
    //
    // For each touched arg-like span:
    //   * extract A and B text,
    //   * normalize it under the span's stringify/paste context, and
    //   * record the resulting old/new text as an observed formal
    //     constraint.
    //
    // Paste subranges whose B-side extraction is unreliable are deferred to
    // pass 2, where the whole pasted token can be split as one unit.
    for (const RefoldModel::PPArgSpan &sp : cand.argLike) {
      if (sp.argIdx >= cand.touched.size() || !cand.touched[sp.argIdx])
        continue;

      auto aTxt = extractDAGSpanText(deps_.sourceMapper, sp, /*fromB=*/false);
      auto bTxt = extractDAGSpanText(deps_.sourceMapper, sp, /*fromB=*/true);
      if (!aTxt || !bTxt)
        continue;

      if (sp.kind == PPArgSpanKind::Paste && sp.byteBegin && sp.byteEnd &&
          !bTxt->reliable) {
        uint64_t key = (uint64_t(sp.begin) << 32) | uint64_t(sp.end);
        unreliPaste[key].push_back(&sp);
        continue;
      }

      if (!bTxt->reliable) {
        invalid = true;
        break;
      }

      auto oldLift =
          normalizeDAGLiftText(deps_.argTextRecovery, cand.inv, sp, aTxt->text,
                               /*allowTopLevelComma=*/true);
      bool allowComma = sp.argIdx < cand.inv->defParams.size() &&
                        cand.inv->defParams[sp.argIdx].variadic;
      auto newLift =
          normalizeDAGLiftText(deps_.argTextRecovery, cand.inv, sp, bTxt->text,
                               /*allowTopLevelComma=*/allowComma);
      if (!oldLift || !newLift) {
        invalid = true;
        break;
      }

      if (*oldLift == *newLift)
        continue;

      if (!recordLeafObserved(sp.argIdx, *oldLift, *newLift)) {
        invalid = true;
        break;
      }
    }

    if (invalid)
      continue;

    // --- Pass 2: resolve unreliable paste subranges ----------------------
    //
    // When paste subrange extraction is unreliable on B, reconstruct the
    // per-operand B-side text from the full pasted-token envelope. This is
    // accepted only when the edited token has a unique split back into the
    // original operand sequence.
    for (auto &kv : unreliPaste) {
      auto &group = kv.second;
      if (group.size() < 2)
        continue;

      llvm::sort(group, pasteSpanPtrLessByByteRange);

      bool groupOk = true;
      for (const RefoldModel::PPArgSpan *sp : group) {
        if (!sp->byteBegin || !sp->byteEnd) {
          groupOk = false;
          break;
        }
      }
      if (!groupOk) {
        invalid = true;
        break;
      }

      // Extract the whole pasted token in A and B. The individual paste
      // operands may have unreliable B ranges, but the enclosing token must
      // still be extractable.
      RefoldModel::PPArgSpan whole = *group.front();
      whole.kind = PPArgSpanKind::Standard;
      whole.argIdx = 0;
      whole.byteBegin = std::nullopt;
      whole.byteEnd = std::nullopt;

      auto aTok =
          extractDAGSpanText(deps_.sourceMapper, whole, /*fromB=*/false);
      auto bTok = extractDAGSpanText(deps_.sourceMapper, whole, /*fromB=*/true);
      if (!aTok || !bTok || !bTok->reliable) {
        invalid = true;
        break;
      }

      StringRef oldTok = aTok->text;
      StringRef newTok = bTok->text;
      const uint64_t oldLen = oldTok.size();

      // Validate that all operand byte ranges are ordered, non-overlapping,
      // and contained in the original pasted-token text.
      for (size_t i = 0; i < group.size(); ++i) {
        const auto *sp = group[i];
        if (*sp->byteBegin > *sp->byteEnd || *sp->byteEnd > oldLen) {
          groupOk = false;
          break;
        }
        if (i > 0 && *group[i - 1]->byteEnd > *sp->byteBegin) {
          groupOk = false;
          break;
        }
      }
      if (!groupOk) {
        invalid = true;
        break;
      }

      // The text outside the paste operands must be preserved verbatim.
      // Otherwise the edited B token is not just a rewrite of the pasted
      // operand surfaces.
      StringRef leading = oldTok.take_front(*group.front()->byteBegin);
      StringRef trailing = oldTok.drop_front(*group.back()->byteEnd);
      if (!newTok.starts_with(leading) || !newTok.ends_with(trailing)) {
        invalid = true;
        break;
      }

      SmallVector<StringRef, 4> oldSegs;
      SmallVector<StringRef, 4> midBodies;
      oldSegs.reserve(group.size());
      midBodies.reserve(group.size() - 1);
      bool hasEmptyInternalSeparator = false;
      bool hasNonEmptyInternalSeparator = false;
      for (size_t i = 0; i < group.size(); ++i) {
        const auto *sp = group[i];
        oldSegs.push_back(oldTok.slice(*sp->byteBegin, *sp->byteEnd));
        if (i + 1 < group.size()) {
          StringRef mid = oldTok.slice(*sp->byteEnd, *group[i + 1]->byteBegin);
          if (mid.empty()) {
            hasEmptyInternalSeparator = true;
            continue;
          }
          hasNonEmptyInternalSeparator = true;
          midBodies.push_back(mid);
        }
      }
      if (hasEmptyInternalSeparator && hasNonEmptyInternalSeparator) {
        invalid = true;
        break;
      }

      StringRef core =
          newTok.slice(leading.size(), newTok.size() - trailing.size());

      SmallVector<StringRef, 4> curSegs;
      SmallVector<SmallVector<StringRef, 4>, 2> splitSolutions;
      auto addSplitSolution = [&](const SmallVectorImpl<StringRef> &parts) {
        SmallVector<StringRef, 4> copy(parts.begin(), parts.end());
        for (const auto &existing : splitSolutions)
          if (existing == copy)
            return;
        splitSolutions.push_back(std::move(copy));
      };

      if (hasEmptyInternalSeparator) {
        // No literal delimiter survives between adjacent pasted operands.
        // In that case the only sound split witness is an unchanged operand
        // that still appears verbatim in the edited pasted core. Accept the
        // group only when those unchanged anchors induce exactly one
        // segmentation of the rewritten core back into per-operand pieces.
        SmallVector<size_t, 4> anchoredIdxs;
        SmallVector<SmallVector<size_t, 4>, 4> anchorStartsByIdx;
        anchoredIdxs.reserve(group.size());
        anchorStartsByIdx.reserve(group.size());

        for (size_t i = 0; i < oldSegs.size(); ++i) {
          const StringRef anchor = oldSegs[i];
          if (anchor.empty())
            continue;

          SmallVector<size_t, 4> starts;
          for (size_t pos = 0;
               (pos = core.find(anchor, pos)) != StringRef::npos; ++pos)
            starts.push_back(pos);
          if (starts.empty())
            continue;

          anchoredIdxs.push_back(i);
          anchorStartsByIdx.push_back(std::move(starts));
        }

        if (anchoredIdxs.empty()) {
          invalid = true;
          break;
        }

        SmallVector<size_t, 4> curAnchorStarts;
        auto addZeroDelimiterAnchoredSolution =
            [&](ArrayRef<size_t> anchorStarts) {
              SmallVector<StringRef, 4> parts(group.size());
              size_t prevConsumed = 0;

              for (size_t anchorPos = 0; anchorPos < anchoredIdxs.size();
                   ++anchorPos) {
                const size_t anchorIdx = anchoredIdxs[anchorPos];
                const size_t anchorBegin = anchorStarts[anchorPos];
                const size_t anchorEnd =
                    anchorBegin + oldSegs[anchorIdx].size();
                if (anchorBegin < prevConsumed || anchorEnd > core.size())
                  return;

                if (anchorPos == 0) {
                  if (anchorIdx > 1)
                    return;
                  if (anchorIdx == 0) {
                    if (anchorBegin != 0)
                      return;
                  } else {
                    // The first anchor is operand 1, so operand 0 is the
                    // only possible prefix segment before that anchor.
                    parts[0] = core.slice(0, anchorBegin);
                  }
                } else {
                  const size_t prevAnchorIdx = anchoredIdxs[anchorPos - 1];
                  const size_t gapSegments = anchorIdx - prevAnchorIdx - 1;
                  if (gapSegments > 1)
                    return;
                  if (gapSegments == 1)
                    parts[prevAnchorIdx + 1] =
                        core.slice(prevConsumed, anchorBegin);
                  else if (anchorBegin != prevConsumed)
                    return;
                }

                parts[anchorIdx] = oldSegs[anchorIdx];
                prevConsumed = anchorEnd;
              }

              const size_t trailingGapSegments =
                  group.size() - anchoredIdxs.back() - 1;
              if (trailingGapSegments > 1)
                return;
              if (trailingGapSegments == 0) {
                if (prevConsumed != core.size())
                  return;
              } else {
                // There is exactly one unanchored operand after the last
                // anchor, so it must consume the remaining suffix.
                parts[anchoredIdxs.back() + 1] = core.drop_front(prevConsumed);
              }

              addSplitSolution(parts);
            };

        auto enumerateZeroDelimiterAnchors = [&](auto &&self, size_t anchorPos,
                                                 size_t minStart) -> void {
          if (splitSolutions.size() > 1)
            return;
          if (anchorPos == anchoredIdxs.size()) {
            addZeroDelimiterAnchoredSolution(curAnchorStarts);
            return;
          }

          const size_t anchorIdx = anchoredIdxs[anchorPos];
          const StringRef anchor = oldSegs[anchorIdx];
          for (size_t start : anchorStartsByIdx[anchorPos]) {
            if (start < minStart)
              continue;
            curAnchorStarts.push_back(start);
            self(self, anchorPos + 1, start + anchor.size());
            curAnchorStarts.pop_back();
          }
        };
        enumerateZeroDelimiterAnchors(enumerateZeroDelimiterAnchors, 0, 0);
      } else {
        // The normal case: split the rewritten core around the original
        // literal delimiters and require a unique segmentation.
        auto suffixDelimiterNeed = [&](size_t delimIdx) -> uint64_t {
          const StringRef delim = midBodies[delimIdx];
          uint64_t need = 0;
          for (size_t segIdx = delimIdx + 1; segIdx < oldSegs.size(); ++segIdx)
            need += countSubstringOccurrences(oldSegs[segIdx], delim);
          for (size_t later = delimIdx + 1; later < midBodies.size(); ++later)
            if (midBodies[later] == delim)
              ++need;
          return need;
        };

        auto splitCore = [&](auto &&self, size_t delimIdx,
                             StringRef rest) -> void {
          if (splitSolutions.size() > 1)
            return;
          if (delimIdx == midBodies.size()) {
            curSegs.push_back(rest);
            addSplitSolution(curSegs);
            curSegs.pop_back();
            return;
          }

          const StringRef delim = midBodies[delimIdx];
          const uint64_t needLeft =
              countSubstringOccurrences(oldSegs[delimIdx], delim);
          const uint64_t needRight = suffixDelimiterNeed(delimIdx);

          for (size_t pos = 0; (pos = rest.find(delim, pos)) != StringRef::npos;
               ++pos) {
            StringRef left = rest.slice(0, pos);
            StringRef tail = rest.drop_front(pos + delim.size());
            if (countSubstringOccurrences(left, delim) < needLeft)
              continue;
            if (countSubstringOccurrences(tail, delim) < needRight)
              continue;
            curSegs.push_back(left);
            self(self, delimIdx + 1, tail);
            curSegs.pop_back();
          }
        };
        splitCore(splitCore, 0, core);
      }

      if (splitSolutions.size() != 1 ||
          splitSolutions[0].size() != group.size()) {
        invalid = true;
        break;
      }

      auto recordLeafEdit = [&](uint32_t argIdx, StringRef oldText,
                                StringRef newText) -> bool {
        return recordLeafObserved(argIdx, oldText, newText);
      };

      // Normalize each recovered segment under its original paste-span
      // context and record it as a per-formal observed edit.
      for (size_t i = 0; i < group.size(); ++i) {
        const auto *sp = group[i];
        auto oldSeg =
            normalizeDAGLiftText(deps_.argTextRecovery, &leaf, *sp, oldSegs[i],
                                 /*allowTopLevelComma=*/true);
        auto newSeg = normalizeDAGLiftText(deps_.argTextRecovery, &leaf, *sp,
                                           splitSolutions[0][i],
                                           /*allowTopLevelComma=*/false);
        if (!oldSeg || !newSeg) {
          groupOk = false;
          break;
        }
        if (*oldSeg == *newSeg)
          continue;
        if (!recordLeafEdit(sp->argIdx, *oldSeg, *newSeg)) {
          groupOk = false;
          break;
        }
      }
      if (!groupOk) {
        invalid = true;
        break;
      }
    }

    if (invalid)
      continue;

    // Convert each touched leaf formal's observed old/new expansion text
    // into a certified leaf rewrite before lifting it toward the root.
    // Prefer the normal formal-rewrite certificate. If that fails solely
    // because the leaf lives in macro-body space and its observed text does
    // not match a unique raw structural template, allow one narrower seed:
    // all observed constraints for that formal must collapse to the same
    // normalized old/new text. That seed still must pass the structured
    // lift/root pipeline below.
    DenseSet<uint32_t> observedLeafSeedArgIdxs;
    for (const auto &kvLocal : leafObserved) {
      const uint32_t argIdx = kvLocal.first;
      auto formalCert =
          invertibilitySolver_.BuildObservedFormalRewriteCertificate(
              leaf, argIdx, kvLocal.second, /*preferredChildSyntax=*/nullptr,
              "DAG subtree leaf formal", tokenHunksAR);
      if (formalCert.kind == FormalRewriteCertificateKind::Invalid) {
        const bool uniformObservedSeedAllowed =
            formalCert.failure ==
            FormalRewriteFailure::MissingStructuralTemplate;
        if (uniformObservedSeedAllowed) {
          auto observedSeed =
              subtreeCertifier_.BuildUniformObservedLeafSeedCertificate(
                  liftingCtx, leaf, argIdx, kvLocal.second,
                  "DAG subtree leaf formal");
          if (observedSeed.kind ==
              UniformObservedLeafSeedCertificateKind::Unique) {
            observedLeafSeedArgIdxs.insert(argIdx);
            leafEdits[argIdx] = OldNewText{std::move(observedSeed.oldText),
                                           std::move(observedSeed.newText)};
            continue;
          }
        }
        invalid = true;
        break;
      }
      if (formalCert.kind == FormalRewriteCertificateKind::NoChange)
        continue;

      leafEdits[argIdx] = OldNewText{std::move(formalCert.oldText),
                                     std::move(formalCert.newText)};
    }

    if (invalid)
      continue;
    if (leafEdits.empty()) {
      continue;
    }

    SmallVector<uint32_t, 8> leafEditArgIdxs;
    leafEditArgIdxs.reserve(leafEdits.size());
    for (const auto &kvLocal : leafEdits)
      leafEditArgIdxs.push_back(kvLocal.first);

    const bool leafTouchesPaste =
        !leaf.pasteSpans.empty() &&
        anyInvocationArgTouchesPaste(leaf, leafEditArgIdxs);
    const bool allTouchedPasteArgsFromObservedLeafSeed =
        leafTouchesPaste && allTouchedPasteArgsAreContained(
                                leaf, leafEditArgIdxs, observedLeafSeedArgIdxs);

    // Paste-aware leaf certificate: when multiple leaf-formal rewrites
    // participate in the same pasted token, validate them as a group
    // against the B-side pasted-token spellings before attempting to lift
    // them up the caller chain.
    if (!leaf.pasteSpans.empty()) {
      DenseMap<uint32_t, std::string> leafReplByArgIdx;
      for (const auto &kvLocal : leafEdits)
        leafReplByArgIdx[kvLocal.first] = kvLocal.second.newText;

      if (leafTouchesPaste) {
        if (allTouchedPasteArgsFromObservedLeafSeed) {
          // Uniform observed seeds are already derived from the observed
          // pasted surface, so local paste validation is deferred and must
          // be discharged by the later subtree semantic certificate.
        } else if (!leaf.invText) {
          invalid = true;
        } else {
          auto leafRangesOpt = deps_.getMacroInvocationFormalArgContentRanges(
              leaf, StringRef(*leaf.invText));
          if (!leafRangesOpt) {
            invalid = true;
          } else if (!pasteArgumentBuilder()
                          .PasteArgReplacementsMatchAllPasteTokensInB(
                              leaf, StringRef(*leaf.invText), *leafRangesOpt,
                              leafReplByArgIdx)) {
            invalid = true;
          }
        }
      }
    }

    if (invalid)
      continue;

    bool deferLeafPasteValidation = false;
    if (!leaf.pasteSpans.empty())
      deferLeafPasteValidation = allTouchedPasteArgsFromObservedLeafSeed;

    // --- Special-case: chained call suffix arguments --------------------
    //
    // If the root invocation expands to an identifier that is immediately
    // called (e.g. PICK1()(10)), the callee's arguments are spelled in the
    // source as a chained call suffix following the root invocation. In
    // this situation, the leaf edit cannot be lifted to the root via
    // argDeps because the root has no formal parameters. Preserve the call
    // chain by patching the chained suffix argument ranges directly in the
    // invocation file text.
    if (numArgs == 0 && leaf.callerMacroId && *leaf.callerMacroId == m.id &&
        m.invFile && leaf.invFile && *leaf.invFile == *m.invFile) {
      const std::string absPath = deps_.lineDirs.ToAbsolutePath(*m.invFile);
      auto bufOrErr = llvm::MemoryBuffer::getFile(absPath);
      if (bufOrErr) {
        StringRef fileText = bufOrErr.get()->getBuffer();

        // Compute the chained call end in the same way as the application
        // Consume any trailing "(...)" groups after the root
        // invocation.
        const uint64_t chainEnd =
            stringutils::extendChainedCallEnd(fileText, invEnd, "((x)+1)");
        if (chainEnd > invEnd && chainEnd <= (uint64_t)fileText.size()) {
          // Apply call-chain local edits right-to-left so the byte ranges
          // remain relative to the original invocation spelling.
          struct LocalEdit {
            uint64_t begin; // relative to invStart
            uint64_t end;   // relative to invStart
            std::string repl;
          };

          SmallVector<LocalEdit, 4> localEdits;
          bool ok = true;

          for (auto &kv : leafEdits) {
            const uint32_t argIdx = kv.first;
            if (argIdx >= leaf.invArgRanges.size()) {
              ok = false;
              break;
            }

            const auto &rng = leaf.invArgRanges[argIdx];
            if (!rng.first || !rng.second) {
              ok = false;
              break;
            }

            const uint64_t bAbs = *rng.first;
            const uint64_t eAbs = *rng.second;
            if (bAbs > eAbs || eAbs > (uint64_t)fileText.size() ||
                bAbs < invStart || eAbs > chainEnd) {
              ok = false;
              break;
            }

            // Ensure the "old" text actually matches the invocation file at
            // the recorded byte range, so we don't patch unrelated text.
            StringRef oldInFile =
                fileText.slice((size_t)bAbs, (size_t)eAbs).trim();
            if (oldInFile != StringRef(kv.second.oldText).trim()) {
              ok = false;
              break;
            }

            localEdits.push_back(LocalEdit{
                bAbs - invStart,
                eAbs - invStart,
                StringRef(kv.second.newText).trim().str(),
            });
          }

          if (ok && !localEdits.empty()) {
            llvm::sort(localEdits, [](const LocalEdit &a, const LocalEdit &b) {
              return a.begin < b.begin;
            });

            uint64_t curB = 0;
            for (const auto &e : localEdits) {
              if (e.begin < curB || e.end < e.begin) {
                ok = false;
                break;
              }
              curB = e.end;
            }
          }

          if (ok && !localEdits.empty()) {
            std::string replText =
                fileText.slice((size_t)invStart, (size_t)chainEnd).str();

            // Apply edits back-to-front to keep byte indices stable.
            for (auto it = localEdits.rbegin(); it != localEdits.rend(); ++it) {
              replText.replace((size_t)it->begin, (size_t)(it->end - it->begin),
                               it->repl);
            }

            MacroPatch candPatch{invStart, chainEnd, replText, m.id};
            deps_.proofLattice.SetMacroPatchProof(
                candPatch, deps_.proofLattice.MakeMacroPatchProof(
                               MacroPatchProofKind::CallChainSuffix,
                               /*preservesInvocationStructure=*/true, m.id));

            auto acceptCert =
                candidateValidator_.AcceptOrMergeDAGCandidatePatch(
                    liftingCtx, dagCandidateAcceptanceCtx, std::move(candPatch),
                    fileText.slice((size_t)invStart, (size_t)chainEnd),
                    "DAG chained-call suffix patch");
            if (!acceptCert.accepted)
              return std::nullopt;
            continue;
          }
        }
      }
    }

    // --- Build one explicit subtree certificate --------------------------
    //
    // The leaf rewrite, caller-chain lifting, root-formal merge, and final
    // root validation are now treated as one bottom-up subtree certificate
    // instead of several ad hoc stages.
    auto subtreeCert = subtreeCertifier_.BuildSubtreeRewriteCertificate(
        liftingCtx, leaf, leafEdits, deferLeafPasteValidation);
    if (subtreeCert.kind == SubtreeRewriteCertificateKind::Invalid ||
        subtreeCert.kind == SubtreeRewriteCertificateKind::NoChange) {
      continue;
    }

    auto rootPatchCert =
        candidateValidator_.BuildRootPatchConstructionCertificate(
            liftingCtx, subtreeCert.rootCert, "DAG subtree root patch");
    if (rootPatchCert.kind == RootPatchConstructionCertificateKind::Invalid ||
        rootPatchCert.kind == RootPatchConstructionCertificateKind::NoChange) {
      continue;
    }

    // Replay-validate the constructed root patch against the subtree's
    // expected root-formal metadata before certifying or merging it.
    DagCandidateValidationMetadata subtreeValidation =
        candidateValidator_.BuildDagCandidateValidationMetadataFromSubtree(
            liftingCtx, subtreeCert);
    if (!candidateValidator_.ValidateDagCandidateProof(
            liftingCtx, dagCandidateAcceptanceCtx, subtreeValidation,
            invSpanText, StringRef(rootPatchCert.patch->replacement),
            "DAG subtree root patch")) {
      continue;
    }

    // Certify the patch with the subtree certificate summary. These fields
    // are audit metadata for the accepted result; the proof itself has
    // already been checked by the subtree/root validation certificates.
    rootPatchCert.patch->macroId = m.id;
    deps_.proofLattice.SetMacroPatchProof(
        *rootPatchCert.patch, deps_.proofLattice.MakeMacroPatchProof(
                                  MacroPatchProofKind::DagSubtreeRoot,
                                  /*preservesInvocationStructure=*/true, m.id));
    SubtreeCertificate &cert = rootPatchCert.patch->subtree;
    cert.backed = true;
    cert.leafMacroId = leaf.id;
    cert.witnessCount = 1;
    cert.invocationCertCount = static_cast<uint32_t>(
        subtreeCert.semantic.invocationCertificates.size());
    cert.formalCertCount =
        static_cast<uint32_t>(subtreeCert.semantic.formalCertificates.size());
    cert.argCertCount =
        static_cast<uint32_t>(subtreeCert.semantic.argCertificates.size());
    cert.liftChainCount =
        static_cast<uint32_t>(subtreeCert.semantic.liftChains.size());
    cert.liftStepCount = static_cast<uint32_t>(
        subtreeCert.semantic.structuredLiftCertificates.size());
    cert.rootMergeCount = static_cast<uint32_t>(
        subtreeCert.semantic.rootMergeCertificates.size());
    cert.usesLexicalBridge = subtreeCert.semantic.usesLexicalBridge;
    cert.touchesPaste = subtreeCert.semantic.touchesPaste;
    cert.hasWrapperSemantics = subtreeCert.semantic.hasWrapperSemantics;
    cert.hasStringifySemantics = subtreeCert.semantic.hasStringifySemantics;
    cert.hasWideStringifySemantics =
        subtreeCert.semantic.hasWideStringifySemantics;
    cert.hasPreferredChildSyntax = subtreeCert.semantic.hasPreferredChildSyntax;
    cert.hasRawInvocationPreservation =
        subtreeCert.semantic.hasRawInvocationPreservation;
    cert.hasPassthroughFlatten = subtreeCert.semantic.hasPassthroughFlatten;
    cert.hasBridgeSensitiveStructuredSemantics =
        subtreeCert.semantic.hasBridgeSensitiveStructuredSemantics;
    cert.deferredPasteDischarged =
        subtreeCert.semantic.deferredPasteDischarge.valid;
    cert.admissible = subtreeCert.semantic.admissibility.valid;
    cert.expectedRootFormalCount =
        static_cast<uint32_t>(subtreeValidation.expectedRootFormals.size());
    cert.deferredRootArgCount =
        static_cast<uint32_t>(subtreeValidation.deferOccurrenceArgIdxs.size());
    cert.bridgeSensitiveFormalCount = static_cast<uint32_t>(
        subtreeValidation.bridgeSensitiveFormalSignatures.size());
    cert.expectedRootFormalSummary =
        invertibilitySolver_.FormatFormalTextPairMap(
            subtreeValidation.expectedRootFormals);
    cert.deferredRootArgSummary =
        formatUInt32List(subtreeValidation.deferOccurrenceArgIdxs);
    cert.bridgeSensitiveFormalSummary =
        candidateValidator_.FormatBridgeSensitiveFormalSignatureMap(
            subtreeValidation.bridgeSensitiveFormalSignatures);

    // The subtree certificate is filled after the primary proof certify so
    // the classifier must see a refreshed MacroPatchProof carrier
    // before this candidate is merged, selected, or emitted.
    deps_.proofLattice.MacroPatchProofClassifier().SyncMacroPatchProofSummary(
        *rootPatchCert.patch);

    // Finally, merge this subtree-backed root patch with any previously
    // accepted DAG candidate for the same root invocation.
    auto acceptCert = candidateValidator_.AcceptOrMergeDAGCandidatePatch(
        liftingCtx, dagCandidateAcceptanceCtx, std::move(*rootPatchCert.patch),
        invSpanText, "DAG subtree root patch", &subtreeValidation);
    if (!acceptCert.accepted)
      return std::nullopt;
  }

  return uniquePatch;
}

} // namespace refold
} // namespace clang
