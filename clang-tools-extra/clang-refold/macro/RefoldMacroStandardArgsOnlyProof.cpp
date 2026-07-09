//===--- RefoldMacroStandardArgsOnlyProof.cpp -------------------*- C++ -*-===//
//
// Private proof and final-candidate construction helpers for the standard
// args-only macro patch builder.
//
// This file is part of the private implementation split behind
// `RefoldMacroStandardArgsOnlyPatchBuilder`.  It owns only finalized argument
// rewrite recording, the pure paste-only args-only fallback candidate, and the
// final standard-argument proof certification step.  It does not discover new
// occurrence evidence, probe generated-callee replay, choose candidate ranking,
// or introduce fallback behavior beyond the caller's already-selected
// args-only ranking slot.
//
//===----------------------------------------------------------------------===//

#include "macro/RefoldMacroStandardArgsOnlyInternals.h"

#include "macro/RefoldMacroPatchProofCertifier.h"
#include "macro/RefoldMacroPlannerHelpers.h"
#include "proof/RefoldProofLattice.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/StringRef.h"

#include <optional>
#include <string>
#include <utility>

using namespace llvm;

namespace clang {
namespace refold {

void ArgsOnlyFinalArgumentRewriteSet::Record(
    uint32_t argIdx, std::string finalNewArg,
    std::optional<std::pair<uint64_t, uint64_t>> materializedRange,
    bool tupleForwarded) {
  tupleForwardedByArgIdx[argIdx] = tupleForwarded;
  if (materializedRange && materializedRange->second <= finalNewArg.size())
    materializedRangeByArgIdx[argIdx] = *materializedRange;
  replacementsByArgIdx[argIdx] = std::move(finalNewArg);
}

bool ArgsOnlyFinalArgumentRewriteSet::Empty() const {
  return replacementsByArgIdx.empty();
}

PurePasteOnlyArgsOnlyCandidateBuilder::
    PurePasteOnlyArgsOnlyCandidateBuilder(
        const RefoldMacroStandardArgsOnlyPatchBuilder::Dependencies &deps,
        RefoldMacroPasteArgumentBuilder pasteBuilder)
    : deps_(deps), pasteBuilder_(std::move(pasteBuilder)) {}

std::optional<MacroPatch> PurePasteOnlyArgsOnlyCandidateBuilder::TryBuild(
    const RefoldModel::MacroInvocation &invocation, const diffutils::Hunk &hunk,
    StringRef baseInvocationText,
    ArrayRef<std::pair<size_t, size_t>> invocationArgRanges,
    const InvocationActualRecoveryContext &actualCtx) const {
  // First derive per-argument paste edits from the current hunk. The derivation
  // proves that the edited pasted-token spelling can be mapped back to argument
  // segments rather than arbitrary token substrings.
  auto edits = pasteBuilder_.DerivePasteArgEdits(invocation, hunk);
  if (!edits || edits->empty())
    return std::nullopt;

  // Merge all derived paste edits into one replacement spelling per invocation
  // argument. Multiple pasted-token occurrences may refer to the same formal,
  // but they must all demand the same final argument spelling.
  DenseMap<uint32_t, std::string> replByArgIdx;
  for (const auto &pae : *edits) {
    const uint32_t argIdx = pae.argIdx;
    if (static_cast<size_t>(argIdx) >= invocationArgRanges.size())
      return std::nullopt;

    // Reconstruct the full invocation-argument spelling by replacing the
    // derived old paste segment with the derived new paste segment. Prefer the
    // exact byte-window splice when the paste witness identifies the segment
    // boundaries inside the argument.
    auto range = invocationArgRanges[argIdx];
    StringRef baseArgText = baseInvocationText.substr(
        range.first, range.second - range.first);
    std::string newArg =
        (pae.argByteBegin && pae.argByteEnd)
            ? RefoldMacroPasteSpelling::SplicePasteSegmentIntoSpellingArgExact(
                  baseArgText, *pae.argByteBegin, *pae.argByteEnd, pae.oldSeg,
                  pae.newSeg)
            : RefoldMacroPasteSpelling::SplicePasteSegmentIntoSpellingArg(
                  baseArgText, pae.oldSeg, pae.newSeg);

    // An empty splice result normally means the old segment could not be found
    // or replaced safely. The one accepted empty-result case is a true no-op
    // where the new segment is empty and the original argument was exactly the
    // old segment after trimming.
    if (newArg.empty()) {
      if (!(StringRef(pae.newSeg).trim().empty() &&
            baseArgText.trim() == StringRef(pae.oldSeg).trim()))
        return std::nullopt;
    }

    // A non-variadic macro formal cannot be rewritten to text containing a
    // top-level comma, because that would change call-site arity.
    if (!isMacroInvocationVariadicFormal(invocation, argIdx) &&
        replacementIntroducesTopLevelComma(newArg, deps_.lexLang))
      return std::nullopt;

    // If the same formal was observed through multiple pasted tokens, require
    // every occurrence to reconstruct the exact same replacement argument.
    auto existing = replByArgIdx.find(argIdx);
    if (existing != replByArgIdx.end()) {
      if (existing->second != newArg)
        return std::nullopt;
      continue;
    }

    replByArgIdx[argIdx] = std::move(newArg);
  }

  // No argument changed after merging, so there is no invocation rewrite to
  // propose from this fallback.
  if (replByArgIdx.empty())
    return std::nullopt;

  // Validate the merged argument replacements globally against every pasted
  // token occurrence in B. This prevents accepting a rewrite that explains only
  // the touched token while breaking another paste occurrence from the same
  // invocation.
  if (!pasteBuilder_.PasteArgReplacementsMatchAllPasteTokensInB(
          invocation, baseInvocationText, invocationArgRanges, replByArgIdx))
    return std::nullopt;

  std::optional<InvocationRewriteWithRange> rewrite =
      deps_.buildInvocationRewriteWithRange(
          actualCtx, replByArgIdx,
          /*materializedRangeByArgIdx=*/nullptr);
  if (!rewrite)
    return std::nullopt;

  MacroPatch patch{*invocation.invB, *invocation.invE, std::move(rewrite->text),
                   invocation.id};
  deps_.proofCertifier.CertifyInvocationRewriteMaterializedOutputRange(
      patch, rewrite->materializedOutputByteStart,
      rewrite->materializedOutputByteEnd);
  // Pure-paste-only replay has no standard/stringify occurrence to define a
  // smaller B-side surface. The proved replay unit is the full expansion cover
  // reconstructed from the rewritten invocation arguments.
  deps_.certifyMacroPatchWholeExpansionBRange(invocation, patch);
  deps_.proofLattice.SetMacroPatchProof(
      patch, deps_.proofLattice.MakeMacroPatchProof(
                 MacroPatchProofKind::ArgsOnlyPurePasteOnly,
                 /*preservesInvocationStructure=*/true, invocation.id));
  // Pure-paste-only rewrites have no standard or stringify occurrences to lean
  // on, so successful all-paste replay is the decisive proof source.
  patch.pasteReplayValidated = true;
  deps_.proofLattice.MacroPatchProofClassifier().SyncMacroPatchProofSummary(
      patch);
  return patch;
}

ArgsOnlyProofCertifier::ArgsOnlyProofCertifier(
    const RefoldMacroStandardArgsOnlyPatchBuilder::Dependencies &deps)
    : deps_(deps) {}

std::optional<MacroPatch> ArgsOnlyProofCertifier::BuildAcceptedCandidate(
    const RefoldModel::MacroInvocation &invocation,
    const InvocationActualRecoveryContext &actualCtx,
    const ArgsOnlyFinalArgumentRewriteSet &rewriteSet) const {
  if (rewriteSet.Empty())
    return std::nullopt;

  std::optional<InvocationRewriteWithRange> rewrite =
      deps_.buildInvocationRewriteWithRange(
          actualCtx, rewriteSet.replacementsByArgIdx,
          &rewriteSet.materializedRangeByArgIdx);
  if (!rewrite)
    return std::nullopt;

  MacroPatch patch{*invocation.invB, *invocation.invE, std::move(rewrite->text),
                   invocation.id};
  deps_.proofCertifier.CertifyInvocationRewriteMaterializedOutputRange(
      patch, rewrite->materializedOutputByteStart,
      rewrite->materializedOutputByteEnd);
  // The materialized output byte range may remain narrowed to an inserted
  // payload inside one argument, but the target-PP proof for an
  // invocation-preserving macro repair is the B-side expansion envelope of the
  // whole macro owner.  Keeping the output byte range narrow is useful for
  // source-spelling edits; leaving the B-token range uncertified would make
  // append/pure-insertion repairs look theorem-incomplete even after the
  // occurrence replay proved that the rewritten invocation regenerates the
  // edited expansion.
  deps_.certifyMacroPatchWholeExpansionBRange(invocation, patch);
  deps_.proofCertifier.SetArgsOnlyStandardProof(
      patch, invocation, /*wholeEnvelopeReplayValidated=*/false);
  return patch;
}

} // namespace refold
} // namespace clang
