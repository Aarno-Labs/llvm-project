//===--- RefoldMacroRewriteRealizationProof.cpp -----------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Implementation of the rewrite realization refutation.  See the header for
// the contract.
//
//===----------------------------------------------------------------------===//

#include "macro/RefoldMacroRewriteRealizationProof.h"

#include "macro/RefoldMacroFormalDependenceProof.h"
#include "macro/RefoldMacroTopology.h"
#include "support/RefoldLog.h"

#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallVector.h"

#include <algorithm>
#include <optional>
#include <utility>

using namespace llvm;

namespace clang {
namespace refold {

namespace {

/// Return whether \p hunk is one of the edits in \p stagedHunks: the same A
/// range, or, for a hunk that replaces tokens, an overlapping one.
bool hunkIsStaged(const diffutils::Hunk &hunk,
                  ArrayRef<ATokenRange> stagedHunks) {
  return llvm::any_of(stagedHunks, [&](const ATokenRange &staged) {
    if (staged.begin == hunk.aStart && staged.end == hunk.aEnd)
      return true;
    return hunk.aStart < hunk.aEnd && staged.begin < staged.end &&
           staged.begin < hunk.aEnd && hunk.aStart < staged.end;
  });
}

/// Return whether \p inv's own call can only have been formed from fixed
/// text: an object-like invocation is formed by its name alone, and a
/// function-like one carries `arg_deps` only when the producer collected it,
/// name through closing parenthesis, from one buffer that is not a
/// substituted actual.  A call with no parameters carries none and counts as
/// unfixed.
bool invocationFormationIsFixed(const RefoldModel::MacroInvocation &inv) {
  return inv.subkind != "func" || !inv.argDeps.empty();
}

/// Pattern over a root invocation's cover: which A tokens a rewrite keeps
/// verbatim, and where material it may produce can stand.
struct CoverPattern {
  /// `fixed[i]`: A token `cover.first + i` re-expands verbatim and stays put
  /// relative to the other fixed tokens.
  SmallVector<bool, 64> fixed;
  /// `openGap[i]`: material may appear just before token `cover.first + i`;
  /// `openGap[n]` is the position after the last token.
  SmallVector<bool, 65> openGap;
};

/// Open the tokens through which macro rescanning can join fixed text to
/// open material: a fixed identifier just before open material may become an
/// invoked name, and a fixed parenthesized group just after open material may
/// become a call's argument list.  Repeat until no boundary changes.
void openRescanBoundaries(CoverPattern &pattern, ArrayRef<PPTok> aToks,
                          uint64_t coverBegin) {
  const size_t n = pattern.fixed.size();
  for (bool changed = true; changed;) {
    changed = false;
    for (size_t i = 0; i < n; ++i) {
      if (!pattern.fixed[i])
        continue;
      const PPTok &tok = aToks[coverBegin + i];
      const bool openAfter =
          pattern.openGap[i + 1] || (i + 1 < n && !pattern.fixed[i + 1]);
      if (tok.kind == "raw_identifier" && openAfter) {
        pattern.fixed[i] = false;
        changed = true;
        continue;
      }
      const bool openBefore =
          pattern.openGap[i] || (i > 0 && !pattern.fixed[i - 1]);
      if (tok.spelling != "(" || !openBefore)
        continue;
      unsigned depth = 0;
      for (size_t j = i; j < n; ++j) {
        const StringRef spelling = aToks[coverBegin + j].spelling;
        if (spelling == "(")
          ++depth;
        else if (spelling == ")")
          --depth;
        pattern.fixed[j] = false;
        if (depth == 0)
          break;
      }
      changed = true;
    }
  }
}

/// Return the maximal runs of fixed tokens in \p pattern, in A order.
SmallVector<SmallVector<StringRef, 8>, 8>
fixedSegments(const CoverPattern &pattern, ArrayRef<PPTok> aToks,
              uint64_t coverBegin) {
  SmallVector<SmallVector<StringRef, 8>, 8> segments(1);
  for (size_t i = 0; i < pattern.fixed.size(); ++i) {
    if (pattern.openGap[i] || !pattern.fixed[i]) {
      if (!segments.back().empty())
        segments.emplace_back();
      if (!pattern.fixed[i])
        continue;
    }
    segments.back().push_back(aToks[coverBegin + i].spelling);
  }
  if (segments.back().empty())
    segments.pop_back();
  return segments;
}

/// Return A boundary \p boundary moved by \p shift tokens, clamped at zero.
uint64_t shiftBoundary(uint64_t boundary, int64_t shift) {
  return static_cast<uint64_t>(
      std::max<int64_t>(0, static_cast<int64_t>(boundary) + shift));
}

/// Return the first B index at or after \p from where \p segment starts and
/// ends before \p end, or nullopt.
std::optional<uint64_t> findSegment(ArrayRef<PPTok> bToks, uint64_t from,
                                    uint64_t end, ArrayRef<StringRef> segment) {
  for (uint64_t start = from; start + segment.size() <= end; ++start) {
    bool equal = true;
    for (size_t k = 0; k < segment.size() && equal; ++k)
      equal = bToks[start + k].spelling == segment[k];
    if (equal)
      return start;
  }
  return std::nullopt;
}

} // namespace

bool RefoldMacroRewriteRealizationProof::RewriteCannotRealize(
    const RefoldModel::MacroInvocation &root,
    ArrayRef<uint32_t> respelledFormals,
    ArrayRef<ATokenRange> stagedHunks) const {
  if (RespellsInvisibleOccurrence(root, respelledFormals) ||
      ExpansionHasUnmodeledDefinition(root))
    return false;

  const std::optional<RefoldMacroFormalDependenceProof::CoverKeptTokens> kept =
      RefoldMacroFormalDependenceProof({deps_.model, deps_.macroTopology})
          .CoverTokensKeptByRewrite(root, respelledFormals);
  if (!kept || kept->cover.second > deps_.aToks.size())
    return false;
  const auto [coverBegin, coverEnd] = kept->cover;
  const size_t n = coverEnd - coverBegin;

  CoverPattern pattern;
  pattern.fixed.assign(kept->kept.begin(), kept->kept.end());
  pattern.openGap.assign(n + 1, false);

  // Output of a descendant whose call may have been formed from substituted
  // tokens can vanish or appear with the rewrite.
  SmallVector<const RefoldModel::MacroInvocation *, 16> worklist = {&root};
  DenseSet<uint64_t> visited;
  while (!worklist.empty()) {
    const RefoldModel::MacroInvocation *inv = worklist.pop_back_val();
    if (!visited.insert(inv->id).second)
      continue;
    if (inv != &root && !invocationFormationIsFixed(*inv))
      for (const RefoldModel::PPSpan &span : inv->spans)
        for (uint64_t tok = std::max(span.begin, coverBegin);
             tok < std::min(span.end, coverEnd); ++tok)
          pattern.fixed[tok - coverBegin] = false;
    append_range(worklist, deps_.macroTopology.MacroChildrenOf(inv->id));
  }

  // Edits not yet planned for this callsite may still change their tokens
  // and insert material at their position.
  for (const diffutils::Hunk &hunk : deps_.hunks) {
    if (hunk.aEnd < coverBegin || hunk.aStart > coverEnd ||
        hunkIsStaged(hunk, stagedHunks))
      continue;
    if (hunk.aStart == hunk.aEnd) {
      pattern.openGap[hunk.aStart - coverBegin] = true;
      continue;
    }
    for (uint64_t tok = std::max(hunk.aStart, coverBegin);
         tok < std::min(hunk.aEnd, coverEnd); ++tok)
      pattern.fixed[tok - coverBegin] = false;
  }

  openRescanBoundaries(pattern, deps_.aToks, coverBegin);

  const auto [slidBegin, slidEnd] =
      SlideOverIdenticalRuns(coverBegin, coverEnd);
  const auto [bBegin, bEnd] = BImageOf(slidBegin, slidEnd);
  uint64_t cursor = bBegin;
  for (const SmallVector<StringRef, 8> &segment :
       fixedSegments(pattern, deps_.aToks, coverBegin)) {
    const std::optional<uint64_t> start =
        findSegment(deps_.bToks, cursor, bEnd, segment);
    if (!start) {
      REFOLD_LOG_TRACE("macro/rewrite-realization",
                       "refute inv id={0} name={1}: {2} kept A token(s) "
                       "starting '{3}' do not occur in order in B [{4},{5})",
                       root.id, root.name, segment.size(), segment.front(),
                       bBegin, bEnd);
      return true;
    }
    cursor = *start + segment.size();
  }
  return false;
}

bool RefoldMacroRewriteRealizationProof::RespellsInvisibleOccurrence(
    const RefoldModel::MacroInvocation &root,
    ArrayRef<uint32_t> respelledFormals) {
  return llvm::any_of(respelledFormals, [&](uint32_t formal) {
    return llvm::none_of(
        root.argSpans, [&](const RefoldModel::PPArgSpan &span) {
          return span.kind == PPArgSpanKind::Standard &&
                 span.argIdx == formal && span.begin < span.end;
        });
  });
}

bool RefoldMacroRewriteRealizationProof::ExpansionHasUnmodeledDefinition(
    const RefoldModel::MacroInvocation &root) const {
  SmallVector<const RefoldModel::MacroInvocation *, 16> worklist = {&root};
  DenseSet<uint64_t> visited;
  while (!worklist.empty()) {
    const RefoldModel::MacroInvocation *inv = worklist.pop_back_val();
    if (!visited.insert(inv->id).second)
      continue;
    const RefoldModel::MacroDirective *definition =
        inv->definitionDirectiveId
            ? deps_.model.GetMacroDirectiveById(*inv->definitionDirectiveId)
            : nullptr;
    if (!definition ||
        llvm::any_of(definition->replacementTokens,
                     [](const RefoldModel::MacroReplacementToken &tok) {
                       return tok.spelling == "__VA_OPT__";
                     }))
      return true;
    append_range(worklist, deps_.macroTopology.MacroChildrenOf(inv->id));
  }
  return false;
}

std::pair<uint64_t, uint64_t>
RefoldMacroRewriteRealizationProof::SlideOverIdenticalRuns(
    uint64_t aBegin, uint64_t aEnd) const {
  if (aBegin >= aEnd || aEnd > deps_.aToks.size())
    return {aBegin, aEnd};

  const StringRef lastSpelling = deps_.aToks[aEnd - 1].spelling;
  for (bool moved = true; moved;) {
    moved = false;
    for (const diffutils::Hunk &hunk : deps_.hunks)
      if (hunk.aStart == aEnd && hunk.aStart < hunk.aEnd) {
        aEnd = hunk.aEnd;
        moved = true;
      }
    while (aEnd < deps_.aToks.size() &&
           deps_.aToks[aEnd].spelling == lastSpelling &&
           llvm::none_of(deps_.hunks, [&](const diffutils::Hunk &hunk) {
             return hunk.aStart == aEnd;
           })) {
      ++aEnd;
      moved = true;
    }
  }

  const StringRef firstSpelling = deps_.aToks[aBegin].spelling;
  for (bool moved = true; moved;) {
    moved = false;
    for (const diffutils::Hunk &hunk : deps_.hunks)
      if (hunk.aEnd == aBegin && hunk.aStart < hunk.aEnd) {
        aBegin = hunk.aStart;
        moved = true;
      }
    while (aBegin > 0 && deps_.aToks[aBegin - 1].spelling == firstSpelling &&
           llvm::none_of(deps_.hunks, [&](const diffutils::Hunk &hunk) {
             return hunk.aEnd == aBegin;
           })) {
      --aBegin;
      moved = true;
    }
  }
  return {aBegin, aEnd};
}

std::pair<uint64_t, uint64_t>
RefoldMacroRewriteRealizationProof::BImageOf(uint64_t aBegin,
                                             uint64_t aEnd) const {
  // A boundary inside a hunk maps to that hunk's B extent.  Elsewhere the
  // tokens before it are aligned one to one except inside earlier hunks,
  // whose length change shifts it.  The left bound stays before an insertion
  // at `aBegin` and the right bound after one at `aEnd`, so material the diff
  // placed at either edge stays in the image.
  int64_t beginShift = 0;
  int64_t endShift = 0;
  std::optional<uint64_t> begin;
  std::optional<uint64_t> end;
  for (const diffutils::Hunk &hunk : deps_.hunks) {
    const int64_t shift = static_cast<int64_t>(hunk.bEnd - hunk.bStart) -
                          static_cast<int64_t>(hunk.aEnd - hunk.aStart);
    if (hunk.aStart < aBegin && aBegin < hunk.aEnd)
      begin = hunk.bStart;
    if (hunk.aStart < aEnd && aEnd < hunk.aEnd)
      end = hunk.bEnd;
    if (hunk.aEnd < aBegin || (hunk.aEnd == aBegin && hunk.aStart < hunk.aEnd))
      beginShift += shift;
    if (hunk.aEnd <= aEnd)
      endShift += shift;
  }
  const uint64_t bSize = deps_.bToks.size();
  return {std::min(begin.value_or(shiftBoundary(aBegin, beginShift)), bSize),
          std::min(end.value_or(shiftBoundary(aEnd, endShift)), bSize)};
}

} // namespace refold
} // namespace clang
