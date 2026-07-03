//===--- RefoldTokenDiffPlanner.cpp -----------------------------*- C++ -*-===//
//
// Implements token-diff planning for clang-refold. The planner owns lexeme
// mapping, LCS provenance construction, normalized token
// hunk derivation, and raw byte-hunk cache construction.
//
//===----------------------------------------------------------------------===//

#include "source/RefoldTokenDiffPlanner.h"

#include "core/RefoldLog.h"
#include "core/RefoldModel.h"
#include "macro/RefoldMacroTopology.h"
#include "source/RefoldSourceMapper.h"
#include "util/StringUtils.h"

#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/StringRef.h"

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <limits>
#include <optional>
#include <utility>

using namespace llvm;

namespace clang {
namespace refold {

RefoldTokenDiffPlanner::RefoldTokenDiffPlanner(Dependencies deps)
    : deps_(deps) {}

RefoldTokenDiffPlanner::TokenDiffPlan RefoldTokenDiffPlanner::Plan() {
  std::vector<StringRef> aSeq = MapLexemes(deps_.aToks, deps_.aTokOff);
  std::vector<StringRef> bSeq = MapLexemes(deps_.bToks, deps_.bTokOff);

  deps_.ownerDepthGap = ComputeOwnerDepthGapsForPP();

  std::vector<diffutils::LcsAGapProvenance> gapProvenance =
      ComputeLcsAGapProvenanceForPP(deps_.ownerDepthGap);
  std::vector<diffutils::LcsBGapProvenance> bGapProvenance =
      ComputeLcsBGapProvenanceForPP();
  std::vector<int64_t> a2b =
      diffutils::lcsMapAB(aSeq, bSeq, gapProvenance, bGapProvenance);

  int64_t last = -1;
  for (size_t i = 0; i < a2b.size(); ++i) {
    const int64_t j = a2b[i];
    if (j < 0)
      continue;
    if (j < last) {
      REFOLD_LOG_FATAL("lcs/map", "non-monotone map at A[{0}]={1} after {2}", i,
                       j, last);
    }
    last = j;
  }

  std::vector<diffutils::Hunk> hunks =
      diffutils::hunksFromMap(a2b, aSeq.size(), bSeq.size());

  std::vector<int64_t> b2a(bSeq.size(), -1);
  for (size_t ai = 0; ai < a2b.size(); ++ai) {
    const int64_t bj = a2b[ai];
    if (bj >= 0 && static_cast<size_t>(bj) < b2a.size())
      b2a[static_cast<size_t>(bj)] = static_cast<int64_t>(ai);
  }

  deps_.abTokMapA2B = a2b;
  deps_.abTokMapB2A = b2a;

  for (diffutils::Hunk &h : hunks) {
    if (!h.isInsertOnly())
      continue;

    // Insert-only hunks should contain only unmatched B tokens. Under
    // ambiguous token-LCS tie-breaks, matched context tokens can appear on a
    // hunk edge; trim them away before downstream owner classification.
    while (h.bStart < h.bEnd && static_cast<size_t>(h.bStart) < b2a.size() &&
           b2a[static_cast<size_t>(h.bStart)] >= 0) {
      ++h.bStart;
    }
    while (h.bStart < h.bEnd && static_cast<size_t>(h.bEnd - 1) < b2a.size() &&
           b2a[static_cast<size_t>(h.bEnd - 1)] >= 0) {
      --h.bEnd;
    }
  }

  if (hunks.size() > 1) {
    std::vector<diffutils::Hunk> merged;
    merged.reserve(hunks.size());
    for (const diffutils::Hunk &h : hunks) {
      const bool isIns = h.isInsertOnly();
      if (!merged.empty()) {
        diffutils::Hunk &prev = merged.back();
        const bool prevIns = prev.isInsertOnly();
        if (isIns && prevIns && prev.aStart == h.aStart &&
            prev.aEnd == h.aEnd && prev.bEnd == h.bStart) {
          prev.bEnd = h.bEnd;
          continue;
        }
      }
      merged.push_back(h);
    }
    if (merged.size() != hunks.size())
      hunks = std::move(merged);
  }

  deps_.abTokHunks = hunks;

  // Raw byte hunks are built alongside the token diff so every later
  // A-byte -> B-byte projection observes caches derived from the same A/B
  // inputs as the token hunk plan.
  deps_.abByteHunks = deps_.sourceMapper.BuildByteHunksFromRawText();
  deps_.sourceMapper.BuildByteHunkPrefixDeltaCache();
  return TokenDiffPlan{std::move(hunks)};
}

std::vector<StringRef>
RefoldTokenDiffPlanner::MapLexemes(ArrayRef<PPTok> toks,
                                   ArrayRef<size_t> offs) {
  std::vector<StringRef> out;
  out.reserve(toks.size());
  for (std::size_t i = 0; i < toks.size(); ++i) {
    const auto &s = toks[i].spelling;
    if (stringutils::isWs(s)) {
      // We should never encounter a whitespace token
      REFOLD_LOG_FATAL("map/lexemes", "token at index {0} is whitespace", i);
    } else {
      out.emplace_back(StringRef(s));
    }
  }
  return out;
}

std::vector<uint32_t>
RefoldTokenDiffPlanner::ComputeOwnerDepthGapsForPP() const {
  // aTokOff.size() == (#tokens) + 1 (sentinel). LCS expects N == #tokens,
  // and ownerDepthGap.size() == N + 1.
  const size_t n = deps_.aTokOff.size() - 1;
  std::vector<uint32_t> ownerDepthGap(n + 1, 0);

  for (size_t k = 0; k <= n; ++k) {
    // --------------------------- Include depth ----------------------------
    std::optional<uint64_t> leftInc;
    std::optional<uint64_t> rightInc;

    if (k > 0) {
      leftInc = deps_.model.InnermostIncludeAtPP(k - 1);
    }
    if (k < n) {
      rightInc = deps_.model.InnermostIncludeAtPP(k);
    }

    std::optional<uint64_t> lca =
        deps_.model.LeastCommonAncestorInclude(leftInc, rightInc);
    uint32_t incDepth = deps_.model.GetIncludeDepth(lca);

    // ------------------------- Conditional depth --------------------------
    std::optional<RefoldModel::ArmRef> leftArmRef;
    std::optional<RefoldModel::ArmRef> rightArmRef;

    if (k > 0) {
      leftArmRef = deps_.model.FindArmRefAtPP(k - 1);
    }
    if (k < n) {
      rightArmRef = deps_.model.FindArmRefAtPP(k);
    }

    uint32_t leftCondDepth =
        leftArmRef ? deps_.model.GetCondArmDepth(leftArmRef->arm->id) : 0;
    uint32_t rightCondDepth =
        rightArmRef ? deps_.model.GetCondArmDepth(rightArmRef->arm->id) : 0;
    uint32_t condDepth = std::min(leftCondDepth, rightCondDepth);

    ownerDepthGap[k] = incDepth + condDepth;
  }

  return ownerDepthGap;
}

std::vector<diffutils::LcsAGapProvenance>
RefoldTokenDiffPlanner::ComputeLcsAGapProvenanceForPP(
    ArrayRef<uint32_t> ownerDepthGap) const {
  using diffutils::LcsAGapProvenance;

  // The core LCS still consumes the same scalar owner-depth array as before.
  // The remaining fields below carry identity, not extra cost: they let the
  // diff layer prove whether an ambiguous equal-token frontier preserves an
  // include, conditional, or macro boundary.

  const size_t n = deps_.aTokOff.size() - 1;
  std::vector<LcsAGapProvenance> profiles(n + 1);
  assert(ownerDepthGap.size() == n + 1 &&
         "A-side LCS provenance requires one owner-depth entry per PP gap");

  struct MacroTokenContext {
    uint64_t rootId = LcsAGapProvenance::noId;
    uint64_t leafId = LcsAGapProvenance::noId;
    uint32_t depth = 0;
    uint32_t roleMask = 0;
  };

  auto spanContains = [](const auto &span, uint64_t pp) -> bool {
    return span.IsValid() && span.begin <= pp && pp < span.end;
  };

  auto macroDepthAndRoot = [&](const RefoldModel::MacroInvocation &m,
                               uint64_t &rootId) -> uint32_t {
    // Walk the caller chain to identify both the leaf depth and the root macro
    // that owns this token. The seen set makes malformed/cyclic metadata
    // benign.
    rootId = m.id;
    uint32_t depth = 1;
    const RefoldModel::MacroInvocation *cur = &m;
    SmallDenseSet<uint64_t, 8> seen;
    seen.insert(cur->id);
    while (cur->callerMacroId) {
      const uint64_t parentId = *cur->callerMacroId;
      if (seen.contains(parentId))
        break;
      seen.insert(parentId);
      const RefoldModel::MacroInvocation *parent =
          deps_.macroTopology.FindMacroInvocationById(parentId);
      if (!parent)
        break;
      cur = parent;
      rootId = cur->id;
      ++depth;
    }
    return depth;
  };

  auto macroRoleMaskAtPP = [&](const RefoldModel::MacroInvocation &m,
                               uint64_t pp) -> uint32_t {
    // Encode which producer span families cover the token. The diff layer uses
    // this as structural provenance, never as a spelling-level tie-breaker.
    uint32_t mask = 0;
    for (const auto &span : m.argSpans) {
      if (spanContains(span, pp)) {
        mask |= 1U; // argument contribution
        break;
      }
    }
    for (const auto &span : m.stringifySpans) {
      if (spanContains(span, pp)) {
        mask |= 2U; // stringification contribution
        break;
      }
    }
    for (const auto &span : m.pasteSpans) {
      if (spanContains(span, pp)) {
        mask |= 4U; // token-paste contribution
        break;
      }
    }
    for (const auto &span : m.bodySpans) {
      if (spanContains(span, pp)) {
        mask |= 8U; // replacement-list/body contribution
        break;
      }
    }
    for (const auto &span : m.spans) {
      if (spanContains(span, pp)) {
        mask |= 16U; // general expansion coverage
        break;
      }
    }
    return mask;
  };

  auto macroContextAtPP = [&](uint64_t pp) -> MacroTokenContext {
    MacroTokenContext best;
    uint64_t bestCoverWidth = std::numeric_limits<uint64_t>::max();
    for (const auto &m : deps_.model.GetMacroInvocations()) {
      if (!m.Covers(pp, pp + 1))
        continue;

      uint64_t rootId = m.id;
      const uint32_t depth = macroDepthAndRoot(m, rootId);
      const uint64_t coverWidth = m.cover.IsValid()
                                      ? (m.cover.end - m.cover.begin)
                                      : std::numeric_limits<uint64_t>::max();

      // Prefer the deepest invocation in the caller chain. If two records have
      // the same caller depth for this token, the narrower cover is the more
      // precise leaf owner. This ranking feeds the production LCS provenance
      // certificate; it does not directly choose an edit by token spelling.
      if (depth > best.depth ||
          (depth == best.depth && coverWidth < bestCoverWidth)) {
        best.rootId = rootId;
        best.leafId = m.id;
        best.depth = depth;
        best.roleMask = macroRoleMaskAtPP(m, pp);
        bestCoverWidth = coverWidth;
      }
    }
    return best;
  };

  auto fillSide = [&](LcsAGapProvenance &profile, uint64_t pp, bool leftSide) {
    // A gap has independent left/right token provenance. Preserve the side so
    // the LCS certifier can distinguish boundaries from interiors.
    const std::optional<uint64_t> includeId =
        deps_.model.InnermostIncludeAtPP(pp);
    const std::optional<RefoldModel::ArmRef> armRef =
        deps_.model.FindArmRefAtPP(pp);
    const MacroTokenContext macro = macroContextAtPP(pp);

    if (leftSide) {
      profile.leftIncludeId = includeId.value_or(LcsAGapProvenance::noId);
      if (armRef) {
        profile.leftCondGroupId = armRef->group->id;
        profile.leftCondArmId = armRef->arm->id;
      }
      profile.leftMacroRootId = macro.rootId;
      profile.leftMacroLeafId = macro.leafId;
      profile.leftMacroRoleMask = macro.roleMask;
    } else {
      profile.rightIncludeId = includeId.value_or(LcsAGapProvenance::noId);
      if (armRef) {
        profile.rightCondGroupId = armRef->group->id;
        profile.rightCondArmId = armRef->arm->id;
      }
      profile.rightMacroRootId = macro.rootId;
      profile.rightMacroLeafId = macro.leafId;
      profile.rightMacroRoleMask = macro.roleMask;
    }

    profile.macroDepth = std::max(profile.macroDepth, macro.depth);
  };

  for (size_t k = 0; k <= n; ++k) {
    LcsAGapProvenance profile;

    // `k` names the gap between PP tokens:
    //
    //   k == 0     : before the first token
    //   0 < k < N  : between tokens k-1 and k
    //   k == N     : after the last token
    //
    // `ownerDepthGap` is the precomputed macro-owner boundary strength for this
    // exact gap. It is kept separate from include/conditional provenance
    // because macro ownership comes from the token expansion graph, not from
    // source-file containment.
    profile.ownerDepth = ownerDepthGap[k];

    std::optional<uint64_t> leftInc;
    std::optional<uint64_t> rightInc;
    std::optional<RefoldModel::ArmRef> leftArmRef;
    std::optional<RefoldModel::ArmRef> rightArmRef;

    // Inspect the token immediately to the left of the gap, if one exists. This
    // captures the include/conditional/macro context that the gap inherits from
    // its left boundary.
    if (k > 0) {
      const uint64_t leftPP = static_cast<uint64_t>(k - 1);
      leftInc = deps_.model.InnermostIncludeAtPP(leftPP);
      leftArmRef = deps_.model.FindArmRefAtPP(leftPP);
      fillSide(profile, leftPP, /*leftSide=*/true);
    }

    // Inspect the token immediately to the right of the gap, if one exists. For
    // an interior gap, the final profile is therefore the shared boundary
    // context between adjacent PP tokens; for edge gaps, it is whichever side
    // exists.
    if (k < n) {
      const uint64_t rightPP = static_cast<uint64_t>(k);
      rightInc = deps_.model.InnermostIncludeAtPP(rightPP);
      rightArmRef = deps_.model.FindArmRefAtPP(rightPP);
      fillSide(profile, rightPP, /*leftSide=*/false);
    }

    // The include provenance for a gap is the deepest include region that
    // contains both sides of the boundary. If the two adjacent tokens come
    // from different headers, this deliberately walks upward to their least
    // common include ancestor rather than pretending the gap belongs
    // exclusively to either side.
    const std::optional<uint64_t> lca =
        deps_.model.LeastCommonAncestorInclude(leftInc, rightInc);
    profile.lcaIncludeId = lca.value_or(LcsAGapProvenance::noId);
    profile.includeDepth = deps_.model.GetIncludeDepth(lca);

    // Conditional provenance is also boundary-shared: a gap can only safely
    // claim the conditional nesting common to both sides. Taking the minimum
    // prevents a boundary between different conditional arms or between an arm
    // and its parent from being ranked as if it were fully inside the deeper
    // side.
    const uint32_t leftCondDepth =
        leftArmRef ? deps_.model.GetCondArmDepth(leftArmRef->arm->id) : 0;
    const uint32_t rightCondDepth =
        rightArmRef ? deps_.model.GetCondArmDepth(rightArmRef->arm->id) : 0;
    profile.conditionalDepth = std::min(leftCondDepth, rightCondDepth);

    // Store the completed gap profile. Later LCS certification uses these
    // profiles to prefer structurally meaningful anchors without consulting
    // lexical-neighbor spellings.
    profiles[k] = profile;
  }

  return profiles;
}

std::vector<diffutils::LcsBGapProvenance>
RefoldTokenDiffPlanner::ComputeLcsBGapProvenanceForPP() const {
  using diffutils::LcsBGapProvenance;

  // B-side tokens have no producer ownership graph, so this records only source
  // surface facts around each edited token gap. The LCS certificate may use
  // these facts to break otherwise equivalent pure-insertion frontiers without
  // reintroducing the removed neighboring-token spelling heuristic.

  const size_t n = deps_.bToks.size();
  std::vector<LcsBGapProvenance> profiles(n + 1);

  auto tokenBegin = [&](size_t tok) -> size_t {
    // Token offsets come from B-source lexing; clamp every query so malformed
    // or truncated metadata cannot point outside the edited buffer.
    if (tok >= deps_.bTokOff.size())
      return deps_.bSource.size();
    return std::min(deps_.bTokOff[tok], deps_.bSource.size());
  };

  auto tokenEnd = [&](size_t tok) -> size_t {
    if (tok >= n)
      return deps_.bSource.size();
    const size_t begin = tokenBegin(tok);
    const size_t spellingEnd = begin + deps_.bToks[tok].spelling.size();
    if (tok + 1 < deps_.bTokOff.size())
      return std::min(spellingEnd, deps_.bTokOff[tok + 1]);
    return std::min(spellingEnd, deps_.bSource.size());
  };

  for (size_t gap = 0; gap <= n; ++gap) {
    LcsBGapProvenance profile;

    // `gap` names a boundary in the B token stream:
    //
    //   gap == 0     : before the first B token
    //   0 < gap < N  : between B tokens gap-1 and gap
    //   gap == N     : after the last B token
    //
    // Unlike the A-side provenance profile, this B-side profile is concerned
    // with surface placement: byte offsets, surrounding token presence, and
    // line-shape facts that help choose deterministic insertion frontiers
    // without looking at neighboring token spellings.
    profile.hasLeftToken = gap > 0;
    profile.hasRightToken = gap < n;

    // The physical gap is the byte interval after the left token spelling and
    // before the right token spelling. For edge gaps, clamp to the beginning
    // or end of the B source buffer.
    const size_t gapBegin = profile.hasLeftToken ? tokenEnd(gap - 1) : 0;
    const size_t gapEnd =
        profile.hasRightToken ? tokenBegin(gap) : deps_.bSource.size();

    profile.gapBeginByte = static_cast<uint64_t>(gapBegin);
    profile.gapEndByte = static_cast<uint64_t>(gapEnd);

    // Record whitespace/newline shape of the gap itself. These facts distin-
    // guish distinguish ordinary intra-line spacing from line-boundary or
    // blank-line frontiers, while staying purely structural. They are not
    // lexical-neighbor heuristics.
    profile.gapContainsNewline =
        stringutils::rangeContainsNewline(deps_.bSource, gapBegin, gapEnd);
    profile.gapContainsOnlyWs =
        stringutils::rangeContainsOnlyWs(deps_.bSource, gapBegin, gapEnd);
    profile.gapAtLineStart =
        stringutils::beginsLineAfterWs(deps_.bSource, gapBegin);
    profile.gapAtLineEnd = stringutils::endsLineBeforeWs(deps_.bSource, gapEnd);

    // If there is a token to the left, record its byte extent and whether that
    // token itself touches a logical line boundary. Later ranking can then
    // prefer anchors/frontiers that preserve existing line structure without
    // re-lexing the source.
    if (profile.hasLeftToken) {
      const size_t leftBegin = tokenBegin(gap - 1);
      const size_t leftEnd = tokenEnd(gap - 1);
      profile.leftTokenBeginByte = static_cast<uint64_t>(leftBegin);
      profile.leftTokenEndByte = static_cast<uint64_t>(leftEnd);
      profile.leftTokenStartsLine =
          stringutils::beginsLineAfterWs(deps_.bSource, leftBegin);
      profile.leftTokenEndsLine =
          stringutils::endsLineBeforeWs(deps_.bSource, leftEnd);
    }

    // Symmetrically record the right token's byte extent and line-boundary
    // shape. Edge gaps intentionally leave these fields absent/defaulted
    // because there is no neighboring token on that side.
    if (profile.hasRightToken) {
      const size_t rightBegin = tokenBegin(gap);
      const size_t rightEnd = tokenEnd(gap);
      profile.rightTokenBeginByte = static_cast<uint64_t>(rightBegin);
      profile.rightTokenEndByte = static_cast<uint64_t>(rightEnd);
      profile.rightTokenStartsLine =
          stringutils::beginsLineAfterWs(deps_.bSource, rightBegin);
      profile.rightTokenEndsLine =
          stringutils::endsLineBeforeWs(deps_.bSource, rightEnd);
    }

    // Store the completed B-gap profile. LCS tie resolution and edit-frontier
    // construction consume these precomputed facts so they can remain
    // deterministic and provenance/surface driven.
    profiles[gap] = profile;
  }

  return profiles;
}

} // namespace refold
} // namespace clang
