//===--- RefoldFinalAssemblyVerifier.cpp -----------------------*- C++ -*-===//
//
// Closing soundness check for one assembled refold result.
//
//===----------------------------------------------------------------------===//

#include "core/RefoldFinalAssemblyVerifier.h"

#include "core/RefoldLangOptions.h"
#include "core/RefoldLog.h"
#include "line-control/FinalLineControlModel.h"
#include "line-control/RefoldNoLinesPruning.h"
#include "source/DiffAlgorithms.h"

#include "llvm/Support/Error.h"

using namespace llvm;

namespace clang {
namespace refold {

namespace {

/// Preprocess \p bytes and lex the result into \p toks / \p offsets.
///
/// Both sides of the comparison go through this one helper so neither can drift
/// from the other in preprocessing flags, temporary-file placement, or lexer
/// language options -- a difference in any of those would show up as a token
/// divergence and be mistaken for an unsound refold.
bool preprocessAndLex(const FinalSourcePreprocessCallback &preprocess,
                      StringRef lang, StringRef bytes, std::vector<PPTok> &toks,
                      std::vector<std::size_t> &offsets) {
  const std::optional<std::string> preprocessed = preprocess(bytes);
  if (!preprocessed)
    return false;

  toks.clear();
  offsets.clear();
  lexPPTokens(*preprocessed, toks, offsets, makeRefoldLexLangOptions(lang));
  return true;
}

/// Merge one relaxation mask into \p combined.
///
/// Masks are independent per-token permissions, so combining them is a union.
/// A mask built against a different token count cannot be indexed by the same
/// positions and is dropped rather than applied at the wrong offsets; dropping
/// it only makes the comparison stricter.
void mergeIgnoreMask(std::vector<std::uint8_t> &combined,
                     std::vector<std::uint8_t> mask) {
  if (combined.empty()) {
    combined = std::move(mask);
    return;
  }
  if (mask.size() != combined.size())
    return;
  for (std::size_t index = 0; index < combined.size(); ++index)
    combined[index] |= mask[index];
}

} // namespace

std::optional<RefoldFinalAssemblyVerifier> RefoldFinalAssemblyVerifier::Create(
    const json::Object &rootJson, const RefoldModel::PreprocessContext &ctx,
    StringRef editedStreamBytes, bool noLines, bool strict,
    StringRef scratchNeighborPath, ArrayRef<std::string> verifyIncludeDirs) {
  RefoldFinalAssemblyVerifier verifier;
  verifier.ctx_ = ctx;
  verifier.scratchNeighborPath_ = scratchNeighborPath.str();
  verifier.verifyIncludeDirs_.assign(verifyIncludeDirs.begin(),
                                     verifyIncludeDirs.end());

  const FinalSourcePreprocessCallback preprocess =
      buildFinalSourcePreprocessCallback(scratchNeighborPath, ctx,
                                         verifyIncludeDirs);

  // Preprocess the edited stream once.  This is the fixed side of every later
  // comparison, and it is what makes the relation idempotence rather than
  // equality against the stream as written.
  if (!preprocessAndLex(preprocess, ctx.lang, editedStreamBytes,
                        verifier.editedTokens_, verifier.editedTokenOffsets_)) {
    REFOLD_LOG_DEBUG("assembly-verify",
                     "unavailable: the edited stream could not be preprocessed");
    return std::nullopt;
  }

  // Relaxations must match `--check` exactly, or this check would reject
  // assemblies that mode accepts.  Both are indexed by the *preprocessed*
  // edited tokens, which is why they are built after the step above.
  if (noLines) {
    if (auto maskOrErr =
            buildNoLinesIgnoreMask(rootJson, ctx, verifier.editedTokens_)) {
      mergeIgnoreMask(verifier.ignoreMask_, std::move(*maskOrErr));
    } else {
      // A mask that cannot be built would make location-sensitive builtins
      // compare exactly, which is not this run's contract.  Verifying under the
      // wrong contract is worse than not verifying.
      consumeError(maskOrErr.takeError());
      REFOLD_LOG_DEBUG("assembly-verify",
                       "unavailable: --no-lines relaxation mask unavailable");
      return std::nullopt;
    }
  }
  if (!strict) {
    if (auto maskOrErr = buildRelaxedStringifyIgnoreMask(
            rootJson, ctx, verifier.editedTokens_)) {
      mergeIgnoreMask(verifier.ignoreMask_, std::move(*maskOrErr));
    } else {
      consumeError(maskOrErr.takeError());
      REFOLD_LOG_DEBUG("assembly-verify",
                       "unavailable: relaxed stringify mask unavailable");
      return std::nullopt;
    }
  }

  return verifier;
}

FinalAssemblyVerdict
RefoldFinalAssemblyVerifier::Verify(StringRef finalSource) const {
  FinalAssemblyVerdict verdict;

  const FinalSourcePreprocessCallback preprocess =
      buildFinalSourcePreprocessCallback(scratchNeighborPath_, ctx_,
                                         verifyIncludeDirs_);

  std::vector<PPTok> assemblyTokens;
  std::vector<std::size_t> assemblyOffsets;
  if (!preprocessAndLex(preprocess, ctx_.lang, finalSource, assemblyTokens,
                        assemblyOffsets)) {
    verdict.kind = FinalAssemblyVerdictKind::Inconclusive;
    return verdict;
  }

  Error err = ignoreMask_.empty()
                  ? compareTokens(assemblyTokens, editedTokens_)
                  : compareTokensNoLinesAware(assemblyTokens, editedTokens_,
                                              ignoreMask_);
  if (!err) {
    verdict.kind = FinalAssemblyVerdictKind::Verified;
    return verdict;
  }

  verdict.kind = FinalAssemblyVerdictKind::Diverged;
  verdict.reason = toString(std::move(err));
  AppendDivergentRanges(assemblyTokens, verdict.divergentRanges);
  verdict.mismatchTokenIndex = verdict.divergentRanges.empty()
                                   ? std::min(assemblyTokens.size(),
                                              editedTokens_.size())
                                   : verdict.divergentRanges.front().first;
  return verdict;
}

void RefoldFinalAssemblyVerifier::AppendDivergentRanges(
    llvm::ArrayRef<PPTok> assemblyTokens,
    std::vector<std::pair<std::size_t, std::size_t>> &ranges) const {
  auto masked = [&](std::size_t index) {
    return !ignoreMask_.empty() && index < ignoreMask_.size() &&
           ignoreMask_[index];
  };
  auto append = [&](std::size_t begin, std::size_t end) {
    if (begin >= end)
      return;
    if (!ranges.empty() && ranges.back().second == begin)
      ranges.back().second = end;
    else
      ranges.emplace_back(begin, end);
  };

  // Equal lengths mean the streams correspond position for position, which is
  // the ordinary case: both are `-E -P` output of the same translation unit.
  // A direct scan is then exact, and each maximal run of differing positions is
  // one diverging region.
  if (assemblyTokens.size() == editedTokens_.size()) {
    std::size_t runBegin = 0;
    bool inRun = false;
    for (std::size_t index = 0; index < editedTokens_.size(); ++index) {
      const bool differs =
          !masked(index) &&
          assemblyTokens[index].spelling != editedTokens_[index].spelling;
      if (differs && !inRun) {
        runBegin = index;
        inRun = true;
      } else if (!differs && inRun) {
        append(runBegin, index);
        inRun = false;
      }
    }
    if (inRun)
      append(runBegin, editedTokens_.size());
    return;
  }

  // Unequal lengths mean tokens were inserted or removed, so position `i` on
  // one side no longer names position `i` on the other and a direct scan would
  // report every position after the first shift.  Align the two streams first
  // and take the gaps, which is the same operation the refolder already
  // performs on the original and edited streams.
  llvm::SmallVector<llvm::StringRef, 0> assemblySpellings;
  llvm::SmallVector<llvm::StringRef, 0> editedSpellings;
  assemblySpellings.reserve(assemblyTokens.size());
  editedSpellings.reserve(editedTokens_.size());
  for (const PPTok &token : assemblyTokens)
    assemblySpellings.push_back(token.spelling);
  for (const PPTok &token : editedTokens_)
    editedSpellings.push_back(token.spelling);

  const std::vector<int64_t> alignment =
      diffutils::lcsMapAB(assemblySpellings, editedSpellings);
  for (const diffutils::Hunk &hunk : diffutils::hunksFromMap(
           alignment, assemblySpellings.size(), editedSpellings.size())) {
    // A hunk every one of whose edited positions is masked is a permitted
    // difference, not a divergence.
    bool everyPositionMasked = hunk.bStart < hunk.bEnd;
    for (uint64_t index = hunk.bStart; index < hunk.bEnd; ++index)
      everyPositionMasked = everyPositionMasked && masked(index);
    if (everyPositionMasked)
      continue;
    append(static_cast<std::size_t>(hunk.bStart),
           static_cast<std::size_t>(hunk.bEnd));
  }
}

} // namespace refold
} // namespace clang
