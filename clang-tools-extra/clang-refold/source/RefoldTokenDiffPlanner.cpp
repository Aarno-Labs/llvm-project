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
#include "source/RefoldPreprocessingStructureIndex.h"
#include "source/RefoldSourceMapper.h"
#include "util/RefoldPathIdentity.h"
#include "util/StringUtils.h"

#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/StringRef.h"

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <limits>
#include <optional>
#include <set>
#include <string>
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

  diffutils::CertifiedLcsResult alignment;
  if (deps_.alignmentOverride) {
    alignment.completeCertification = true;
    alignment.selectedMap = deps_.alignmentOverride->selectedMap;
    alignment.selectedAnchorProofs =
        deps_.alignmentOverride->selectedAnchorProofs;
    alignment.forcedMap.assign(alignment.selectedMap.size(), -1);
    for (size_t aToken = 0; aToken < alignment.selectedMap.size(); ++aToken) {
      if (alignment.selectedMap[aToken] < 0 ||
          aToken >= alignment.selectedAnchorProofs.size())
        continue;
      if (alignment.selectedAnchorProofs[aToken].kind ==
          diffutils::LcsAnchorProofKind::CoreOptimalPathForced)
        alignment.forcedMap[aToken] = alignment.selectedMap[aToken];
    }
  } else {
    std::vector<diffutils::LcsAGapProvenance> gapProvenance =
        ComputeLcsAGapProvenanceForPP(deps_.ownerDepthGap);
    alignment = diffutils::certifiedLcsMapAB(aSeq, bSeq, gapProvenance);
    // The permanent ambiguity census is evidence-only and may inspect every
    // admissible repeated-token pair. Do not pay that cost when trace output is
    // disabled.
    if (inTraceMode())
      TraceAlignmentAmbiguityWindows(aSeq, bSeq, alignment);
    if (deps_.semanticAlignmentResolver) {
      // The historical boundary policy is reconstructed only as a proposal.
      // Its B-gap surface ranks never grant authority: every proposed
      // non-forced anchor must subsequently pass the semantic resolver's
      // complete counterfactual and realization-equivalence theorems.
      std::vector<diffutils::LcsBGapProvenance> bGapProvenance =
          ComputeLcsBGapProvenanceForPP();
      deps_.semanticAlignmentResolver(aSeq, bSeq, gapProvenance,
                                      bGapProvenance, alignment);
    }
  }
  const std::vector<int64_t> &a2b = alignment.selectedMap;

  // An incomplete structured result is not an alignment certificate. The
  // diff layer deliberately publishes no anchors in that case, causing the
  // downstream owner/structure planners to see one conservative edit island.
  // Keep this assertion at the refolding boundary so a future compatibility
  // fallback cannot silently reintroduce uncertified provenance.
  if (!alignment.completeCertification &&
      std::any_of(a2b.begin(), a2b.end(),
                  [](int64_t bToken) { return bToken >= 0; })) {
    REFOLD_LOG_FATAL(
        "lcs/map",
        "incomplete structured LCS result exposed an uncertified token anchor");
  }

  if (a2b.size() != aSeq.size() ||
      alignment.selectedAnchorProofs.size() != aSeq.size()) {
    REFOLD_LOG_FATAL(
        "lcs/map",
        "selected alignment/proof surface has the wrong A-token cardinality");
  }

  int64_t last = -1;
  for (size_t i = 0; i < a2b.size(); ++i) {
    const int64_t j = a2b[i];
    const diffutils::LcsAnchorProof &proof =
        alignment.selectedAnchorProofs[i];
    if (j < 0) {
      if (proof.kind != diffutils::LcsAnchorProofKind::None ||
          proof.semanticWitnessId != 0)
        REFOLD_LOG_FATAL("lcs/map",
                         "unmapped A token carries an anchor theorem");
      continue;
    }
    if (!proof.IsAuthorized())
      REFOLD_LOG_FATAL("lcs/map",
                       "mapped A token has no authorized anchor theorem");
    if (static_cast<size_t>(j) >= bSeq.size() || aSeq[i] != bSeq[j])
      REFOLD_LOG_FATAL("lcs/map",
                       "selected anchor does not identify equal A/B lexemes");
    if (j <= last) {
      REFOLD_LOG_FATAL("lcs/map", "non-monotone map at A[{0}]={1} after {2}", i,
                       j, last);
    }
    if (proof.kind == diffutils::LcsAnchorProofKind::CoreOptimalPathForced &&
        (i >= alignment.forcedMap.size() || alignment.forcedMap[i] != j)) {
      REFOLD_LOG_FATAL(
          "lcs/map",
          "core-forced anchor theorem does not match the exact forced map");
    }
    last = j;
  }
  deps_.abTokAnchorProofs = alignment.selectedAnchorProofs;

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

    // Insert-only hunks should contain only unmatched B tokens.  A
    // theorem-authorized semantic representative may retain matched context
    // at a hunk edge, so trim that context before downstream owner
    // classification without changing the selected anchor proof.
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
  return TokenDiffPlan{std::move(hunks), std::move(alignment)};
}

namespace {

struct ForcedAlignmentAnchor {
  int64_t aToken = -1;
  int64_t bToken = -1;
};

static std::string formatTokenFrontiers(ArrayRef<uint64_t> frontiers) {
  std::string text;
  raw_string_ostream os(text);
  os << '[';
  for (size_t i = 0; i < frontiers.size(); ++i) {
    if (i != 0)
      os << ',';
    os << frontiers[i];
  }
  os << ']';
  os.flush();
  return text;
}

static std::string formatForcedAnchor(const ForcedAlignmentAnchor &anchor,
                                      bool isLeftSentinel,
                                      bool isRightSentinel) {
  if (isLeftSentinel)
    return "<begin>";
  if (isRightSentinel)
    return "<end>";
  return formatv("A[{0}]->B[{1}]", anchor.aToken, anchor.bToken).str();
}

static bool sourceIntervalPrecedes(const RefoldModel::TokMapEntry &entry,
                                   uint64_t sourceBegin) {
  return entry.e <= sourceBegin;
}

static bool sourceIntervalFollows(const RefoldModel::TokMapEntry &entry,
                                  uint64_t sourceEnd) {
  return entry.b >= sourceEnd;
}

} // namespace

void RefoldTokenDiffPlanner::TraceAlignmentAmbiguityWindows(
    ArrayRef<StringRef> aSeq, ArrayRef<StringRef> bSeq,
    const diffutils::CertifiedLcsResult &alignment) const {
  if (!inTraceMode())
    return;

  const uint64_t aTokenCount = static_cast<uint64_t>(aSeq.size());
  const uint64_t bTokenCount = static_cast<uint64_t>(bSeq.size());

  // Project exact protected physical intervals to A-token boundaries.  This is
  // diagnostic evidence only: failure to find one exact boundary is recorded
  // and never replaced by a nearest-token or source-distance approximation.
  std::set<uint64_t> protectedABoundaries;
  size_t unmappedProtectedIntervals = 0;
  const StringRef indexedPath =
      deps_.preprocessingStructureIndex.GetSourcePath();

  auto addBoundIncludeSeams = [&](const PreprocessingStructureInterval &interval)
      -> bool {
    if (interval.modelKind !=
            PreprocessingStructureModelKind::IncludeDirective ||
        !interval.modelItemId)
      return false;

    for (const RefoldModel::IncludeItem &include : deps_.model.GetIncludes()) {
      if (include.id != *interval.modelItemId || !include.cover.IsValid())
        continue;
      if (include.cover.end > aTokenCount)
        return false;
      if (!deps_.pathIdentity.PathsEqual(include.sitePath, indexedPath) ||
          include.parent !=
              deps_.preprocessingStructureIndex.GetOwnerIncludeId())
        return false;
      protectedABoundaries.insert(include.cover.begin);
      protectedABoundaries.insert(include.cover.end);
      return true;
    }
    return false;
  };

  auto projectTokenlessInterval =
      [&](const PreprocessingStructureInterval &interval)
      -> std::optional<uint64_t> {
    uint64_t lowerBoundary = 0;
    uint64_t upperBoundary = aTokenCount;
    bool overlapsMappedToken = false;

    for (const RefoldModel::TokMapEntry &entry : deps_.model.GetTokmap()) {
      if (entry.pp >= aTokenCount ||
          !deps_.pathIdentity.PathsEqual(entry.file, indexedPath))
        continue;
      if (sourceIntervalPrecedes(entry, interval.begin)) {
        lowerBoundary = std::max(lowerBoundary, entry.pp + 1);
      } else if (sourceIntervalFollows(entry, interval.end)) {
        upperBoundary = std::min(upperBoundary, entry.pp);
      } else {
        overlapsMappedToken = true;
      }
    }

    // A top-level include contributes tokens whose tokmap entries name the
    // included file rather than this TU.  Its producer cover is nevertheless
    // exact A-token evidence, so include covers before/after the directive are
    // part of the physical-order projection.
    for (const RefoldModel::IncludeItem &include : deps_.model.GetIncludes()) {
      if (include.parent !=
              deps_.preprocessingStructureIndex.GetOwnerIncludeId() ||
          !include.cover.IsValid() || include.cover.end > aTokenCount ||
          !deps_.pathIdentity.PathsEqual(include.sitePath, indexedPath))
        continue;
      if (include.siteE <= interval.begin) {
        lowerBoundary = std::max(lowerBoundary, include.cover.end);
      } else if (include.siteB >= interval.end) {
        upperBoundary = std::min(upperBoundary, include.cover.begin);
      }
    }

    if (overlapsMappedToken || lowerBoundary != upperBoundary)
      return std::nullopt;
    return lowerBoundary;
  };

  for (const PreprocessingStructureInterval &interval :
       deps_.preprocessingStructureIndex.GetIntervals()) {
    if (addBoundIncludeSeams(interval))
      continue;
    std::optional<uint64_t> boundary = projectTokenlessInterval(interval);
    if (!boundary) {
      ++unmappedProtectedIntervals;
      continue;
    }
    protectedABoundaries.insert(*boundary);
  }

  auto traceWindowHeader = [&](uint64_t aBegin, uint64_t aEnd,
                               uint64_t bBegin, uint64_t bEnd,
                               const ForcedAlignmentAnchor &left,
                               const ForcedAlignmentAnchor &right,
                               bool leftSentinel, bool rightSentinel,
                               const diffutils::LcsObjective &objective) {
    REFOLD_LOG_TRACE("lcs/ambiguity", "alignment ambiguity window:");
    REFOLD_LOG_TRACE("lcs/ambiguity", "A window=[{0},{1})", aBegin,
                     aEnd);
    REFOLD_LOG_TRACE("lcs/ambiguity", "B window=[{0},{1})", bBegin,
                     bEnd);
    REFOLD_LOG_TRACE(
        "lcs/ambiguity", "forced left anchor={0}",
        formatForcedAnchor(left, leftSentinel, /*isRightSentinel=*/false));
    REFOLD_LOG_TRACE(
        "lcs/ambiguity", "forced right anchor={0}",
        formatForcedAnchor(right, /*isLeftSentinel=*/false, rightSentinel));
    REFOLD_LOG_TRACE(
        "lcs/ambiguity",
        "core objective matchedTokens={0} ownerDepthCost={1}",
        objective.matchedTokenCount, objective.ownerDepthCost);
  };

  if (!alignment.completeCertification ||
      !alignment.oracle.HasCompleteCertification()) {
    const ForcedAlignmentAnchor beginSentinel;
    const ForcedAlignmentAnchor endSentinel{
        static_cast<int64_t>(aTokenCount),
        static_cast<int64_t>(bTokenCount)};
    traceWindowHeader(0, aTokenCount, 0, bTokenCount, beginSentinel,
                      endSentinel, /*leftSentinel=*/true,
                      /*rightSentinel=*/true, alignment.globalObjective);
    REFOLD_LOG_TRACE("lcs/ambiguity",
                     "admissible repeated-token pairs=<unavailable>");
    for (uint64_t aBoundary : protectedABoundaries) {
      REFOLD_LOG_TRACE(
          "lcs/ambiguity",
          "possible B frontiers protectedA={0} frontiers=<incomplete>",
          aBoundary);
    }
    REFOLD_LOG_TRACE("lcs/ambiguity",
                     "unmapped protected intervals={0}",
                     unmappedProtectedIntervals);
    REFOLD_LOG_TRACE("lcs/ambiguity",
                     "certification completeness=false");
    return;
  }

  std::vector<ForcedAlignmentAnchor> anchors;
  anchors.reserve(alignment.forcedMap.size() + 2);
  anchors.push_back(ForcedAlignmentAnchor{});
  for (size_t aToken = 0; aToken < alignment.forcedMap.size(); ++aToken) {
    const int64_t bToken = alignment.forcedMap[aToken];
    if (bToken < 0)
      continue;
    anchors.push_back(ForcedAlignmentAnchor{
        static_cast<int64_t>(aToken), bToken});
  }
  anchors.push_back(ForcedAlignmentAnchor{
      static_cast<int64_t>(aTokenCount),
      static_cast<int64_t>(bTokenCount)});

  for (size_t anchorIndex = 0; anchorIndex + 1 < anchors.size();
       ++anchorIndex) {
    const ForcedAlignmentAnchor &left = anchors[anchorIndex];
    const ForcedAlignmentAnchor &right = anchors[anchorIndex + 1];
    const bool leftSentinel = anchorIndex == 0;
    const bool rightSentinel = anchorIndex + 2 == anchors.size();
    const uint64_t aBegin =
        leftSentinel ? 0 : static_cast<uint64_t>(left.aToken + 1);
    const uint64_t bBegin =
        leftSentinel ? 0 : static_cast<uint64_t>(left.bToken + 1);
    const uint64_t aEnd = static_cast<uint64_t>(right.aToken);
    const uint64_t bEnd = static_cast<uint64_t>(right.bToken);
    if (aBegin > aEnd || bBegin > bEnd)
      continue;

    StringMap<uint32_t> aSpellingCount;
    StringMap<uint32_t> bSpellingCount;
    StringMap<std::vector<uint64_t>> bTokensBySpelling;
    for (uint64_t aToken = aBegin; aToken < aEnd; ++aToken)
      ++aSpellingCount[aSeq[static_cast<size_t>(aToken)]];
    for (uint64_t bToken = bBegin; bToken < bEnd; ++bToken) {
      const StringRef spelling = bSeq[static_cast<size_t>(bToken)];
      ++bSpellingCount[spelling];
      bTokensBySpelling[spelling].push_back(bToken);
    }

    struct AdmissiblePair {
      uint64_t aToken = 0;
      uint64_t bToken = 0;
      StringRef spelling;
    };
    // A match edge can exist only between equal lexemes. Indexing the B side
    // preserves the original A-major/B-minor diagnostic order while avoiding a
    // Cartesian scan over provably unequal pairs.
    bool hasOptionalPair = false;
    std::vector<AdmissiblePair> repeatedPairs;
    for (uint64_t aToken = aBegin; aToken < aEnd; ++aToken) {
      const StringRef spelling = aSeq[static_cast<size_t>(aToken)];
      const auto positions = bTokensBySpelling.find(spelling);
      if (positions == bTokensBySpelling.end())
        continue;
      const bool repeated = aSpellingCount.lookup(spelling) > 1 ||
                            bSpellingCount.lookup(spelling) > 1;
      for (uint64_t bToken : positions->second) {
        if (!alignment.oracle.PairOccursOnOptimalPath(aToken, bToken) ||
            alignment.oracle.PairIsForced(aToken, bToken))
          continue;
        hasOptionalPair = true;
        if (repeated)
          repeatedPairs.push_back(
              AdmissiblePair{aToken, bToken, spelling});
      }
    }

    std::vector<std::pair<uint64_t, std::vector<uint64_t>>>
        protectedFrontiers;
    bool ambiguousProtectedFrontier = false;
    for (uint64_t aBoundary : protectedABoundaries) {
      if (aBoundary < aBegin || aBoundary > aEnd)
        continue;
      std::vector<uint64_t> frontiers =
          alignment.oracle.ProjectATokenBoundaryToOptimalBFrontiers(
              aBegin, aEnd, bBegin, bEnd, aBoundary);
      if (frontiers.size() != 1)
        ambiguousProtectedFrontier = true;
      protectedFrontiers.emplace_back(aBoundary, std::move(frontiers));
    }

    // An ambiguity window exists when at least one non-forced match edge can
    // participate in an optimal path, or an exact protected A boundary has
    // multiple/no certified B frontiers. Pure insertion/deletion regions with
    // no optional anchors do not create token-alignment ambiguity.
    if (!hasOptionalPair && !ambiguousProtectedFrontier)
      continue;

    const diffutils::LcsObjective objective =
        alignment.oracle.ObjectiveForWindow(aBegin, aEnd, bBegin, bEnd);
    traceWindowHeader(aBegin, aEnd, bBegin, bEnd, left, right, leftSentinel,
                      rightSentinel, objective);

    std::string repeatedPairsText;
    raw_string_ostream repeatedPairsStream(repeatedPairsText);
    repeatedPairsStream << '[';
    bool firstRepeatedPair = true;
    for (const AdmissiblePair &pair : repeatedPairs) {
      if (!firstRepeatedPair)
        repeatedPairsStream << ", ";
      firstRepeatedPair = false;
      repeatedPairsStream << "A[" << pair.aToken << "]->B[" << pair.bToken
                          << "] '" << pair.spelling << "'";
    }
    repeatedPairsStream << ']';
    repeatedPairsStream.flush();
    REFOLD_LOG_TRACE("lcs/ambiguity",
                     "admissible repeated-token pairs={0}",
                     repeatedPairsText);

    for (const auto &entry : protectedFrontiers) {
      REFOLD_LOG_TRACE(
          "lcs/ambiguity", "possible B frontiers protectedA={0} frontiers={1}",
          entry.first, formatTokenFrontiers(entry.second));
    }
    REFOLD_LOG_TRACE("lcs/ambiguity", "unmapped protected intervals={0}",
                     unmappedProtectedIntervals);
    REFOLD_LOG_TRACE("lcs/ambiguity", "certification completeness=true");
  }
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

  // B-side tokens have no producer ownership graph, so this records only
  // source-surface facts around each edited token gap.  The evidence-only
  // Patch 6 shadow reconstruction consumes the profile to reproduce the last
  // known regression-passing frontier policy exactly.  Production alignment
  // certification and semantic class construction never consult these facts.

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

    // Record whitespace/newline shape of the gap itself. These facts
    // distinguish ordinary intra-line spacing from line-boundary or blank-line
    // frontiers. They are retained only to reconstruct the historical policy
    // faithfully; they are not production proof facts.
    profile.gapContainsNewline =
        stringutils::rangeContainsNewline(deps_.bSource, gapBegin, gapEnd);
    profile.gapContainsOnlyWs =
        stringutils::rangeContainsOnlyWs(deps_.bSource, gapBegin, gapEnd);
    profile.gapAtLineStart =
        stringutils::beginsLineAfterWs(deps_.bSource, gapBegin);
    profile.gapAtLineEnd = stringutils::endsLineBeforeWs(deps_.bSource, gapEnd);

    // If there is a token to the left, record its byte extent and whether that
    // token itself touches a logical line boundary. The shadow reconstruction
    // can then reproduce the historical rank without re-lexing the source.
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

    // Store the completed B-gap profile for the trace-only historical shadow.
    // No production LCS or edit-frontier decision consumes this profile.
    profiles[gap] = profile;
  }

  return profiles;
}

} // namespace refold
} // namespace clang
