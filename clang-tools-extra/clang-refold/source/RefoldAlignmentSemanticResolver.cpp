//===--- RefoldAlignmentSemanticResolver.cpp -------------------------------===//
//
// Resolve ambiguity left by the exact weighted-LCS core theorem.
//
// The historical boundary policy is retained only as a proposal generator. A
// proposed non-forced anchor becomes production authority only when removing
// it changes a complete end-to-end realization or yields a strictly more
// destructive realization. Complete optimal maps may also prove one globally
// least source-mutation class by exact transformation containment. Remaining
// maps must prove one byte-exact realized-source class before a deterministic
// representative is selected. Unknown proof dimensions block commitment rather
// than eliminating a competing map.
//
//===----------------------------------------------------------------------===//

#include "source/RefoldAlignmentSemanticResolver.h"

#include "source/RefoldLegacyAlignmentDiagnostic.h"

#include "core/RefoldLog.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/FormatVariadic.h"

#include <algorithm>
#include <cstdlib>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <utility>

using namespace llvm;

namespace clang {
namespace refold {
namespace {

/// Exact enumeration and counterfactual limits are proof budgets. Exceeding a
/// limit retains the forced-only map; no prefix or ranked subset becomes
/// authority.
constexpr size_t MaxUniqueMapsPerForcedWindow = 256;
constexpr size_t MaxGlobalSemanticCandidateMaps = 256;
constexpr size_t MaxProposalCounterfactuals = 256;

/// Environment override for the per-run candidate-simulation work budget.
constexpr const char *CandidateSimulationBudgetEnvironment =
    "CLANG_REFOLD_CANDIDATE_SIMULATION_WORK_BUDGET";

/// Work one run may spend re-planning candidate alignments, in token-squared
/// units.
///
/// Each candidate is realized by a complete refold of the translation unit, so
/// a limit expressed in candidates is not a cost bound: the same allowance is
/// a few seconds on one stream and many minutes on another. Measured
/// per-candidate cost grows faster than stream length -- roughly with its
/// square, since certification dominates a pass -- so a candidate is charged
/// the square of the A-token count. That makes the ceiling mean about the same
/// machine work on any input, and it stays a deterministic function of the
/// inputs; a wall-clock budget would model cost better but would let two
/// machines commit different alignments for the same source.
///
/// The size is calibrated on measured behaviour: across the corpus the window
/// that *commits* enumerates four candidates, while windows enumerating
/// sixteen have never committed -- sixteen candidates emitting sixteen
/// distinct concrete outputs can no longer collapse to one class. This admits
/// the former with margin and declines the latter.
///
/// Calibration can be wrong for an input not yet seen: a window needing more
/// candidates than this is declined where a larger budget would have resolved
/// it. That costs completeness, never soundness. A declined window keeps the
/// anchors the core theorem published, and a window that does run still
/// enumerates its complete candidate set and is judged by the unchanged commit
/// theorems -- the budget gates whether a window runs, never how it is judged.
constexpr uint64_t DefaultCandidateSimulationWorkBudget = 2500000000ULL;

/// Return how many candidate simulations this run may spend in total.
///
/// A run that cannot afford two candidates cannot resolve anything, since a
/// window needs at least two competing maps to be ambiguous, so there is no
/// floor propping small allowances up to a usable number.
size_t getCandidateSimulationBudget(size_t aTokenCount) {
  uint64_t workBudget = DefaultCandidateSimulationWorkBudget;
  if (const char *injected =
          std::getenv(CandidateSimulationBudgetEnvironment)) {
    if (*injected != '\0' &&
        StringRef(injected).getAsInteger(10, workBudget)) {
      REFOLD_LOG_WARN(
          "lcs/semantic-resolver",
          "invalid {0} value '{1}'; using the default work budget {2}",
          CandidateSimulationBudgetEnvironment, injected,
          DefaultCandidateSimulationWorkBudget);
      workBudget = DefaultCandidateSimulationWorkBudget;
    }
  }
  if (aTokenCount == 0)
    return 0;
  const uint64_t tokens = static_cast<uint64_t>(aTokenCount);
  if (tokens > std::numeric_limits<uint64_t>::max() / tokens)
    return 0;
  return static_cast<size_t>(workBudget / (tokens * tokens));
}

struct ForcedAnchor {
  uint64_t aToken = 0;
  uint64_t bToken = 0;
};

struct RequiredAnchor {
  uint64_t aToken = 0;
  uint64_t bToken = 0;
  AlignmentSemanticAnchorBasis basis =
      AlignmentSemanticAnchorBasis::EquivalentRealizationRepresentative;
};

bool isForcedAnchor(const diffutils::CertifiedLcsResult &alignment,
                    size_t aToken, int64_t bToken) {
  return aToken < alignment.forcedMap.size() &&
         alignment.forcedMap[aToken] == bToken;
}

/// Collect the forced anchors whose A token lies inside `[aBegin, aEnd)`.
///
/// Forced anchors delimit the sub-rectangles the all-optimal enumeration is
/// conditioned on. Restricting them to one certification window is exact
/// because that window's endpoints are themselves proved DP seams, so every
/// locally optimal path enters and leaves at the same two states.
std::vector<ForcedAnchor>
collectForcedAnchors(const diffutils::CertifiedLcsResult &alignment,
                     uint64_t aBegin, uint64_t aEnd) {
  std::vector<ForcedAnchor> anchors;
  const size_t limit =
      std::min<size_t>(static_cast<size_t>(aEnd), alignment.forcedMap.size());
  for (size_t aToken = static_cast<size_t>(aBegin); aToken < limit; ++aToken) {
    const int64_t bToken = alignment.forcedMap[aToken];
    if (bToken >= 0)
      anchors.push_back(
          ForcedAnchor{aToken, static_cast<uint64_t>(bToken)});
  }
  return anchors;
}

bool isStrictlyMonotoneMap(ArrayRef<int64_t> map, size_t bTokenCount) {
  int64_t previous = -1;
  for (int64_t mapped : map) {
    if (mapped < 0)
      continue;
    if (static_cast<uint64_t>(mapped) >= bTokenCount || mapped <= previous)
      return false;
    previous = mapped;
  }
  return true;
}

bool mapLexemesAgree(ArrayRef<int64_t> map, ArrayRef<StringRef> aLexemes,
                     ArrayRef<StringRef> bLexemes) {
  if (map.size() != aLexemes.size())
    return false;
  for (size_t aToken = 0; aToken < map.size(); ++aToken) {
    const int64_t bToken = map[aToken];
    if (bToken < 0)
      continue;
    if (static_cast<size_t>(bToken) >= bLexemes.size() ||
        aLexemes[aToken] != bLexemes[static_cast<size_t>(bToken)])
      return false;
  }
  return true;
}

AlignmentSelectionOverride buildSimulationSelection(
    ArrayRef<int64_t> map,
    const diffutils::CertifiedLcsResult &coreAlignment) {
  AlignmentSelectionOverride selection;
  selection.selectedMap.assign(map.begin(), map.end());
  selection.selectedAnchorProofs.assign(map.size(),
                                         diffutils::LcsAnchorProof{});
  selection.globalObjective = coreAlignment.globalObjective;
  selection.globalObjectiveIsExact = coreAlignment.globalObjectiveIsExact;
  selection.allWindowsCertified = coreAlignment.allWindowsCertified;
  selection.certifiedBoundaries = coreAlignment.certifiedBoundaries;
  selection.certificationWindows = coreAlignment.certificationWindows;
  for (size_t aToken = 0; aToken < map.size(); ++aToken) {
    const int64_t bToken = map[aToken];
    if (bToken < 0)
      continue;
    if (isForcedAnchor(coreAlignment, aToken, bToken)) {
      selection.selectedAnchorProofs[aToken] = diffutils::LcsAnchorProof{
          diffutils::LcsAnchorProofKind::CoreOptimalPathForced, 0};
      continue;
    }

    // Isolated runs are theorem checks, not production authority. The sentinel
    // witness id is replaced only after the outer resolver commits one complete
    // realized-source class.
    selection.selectedAnchorProofs[aToken] = diffutils::LcsAnchorProof{
        diffutils::LcsAnchorProofKind::EquivalentNormalizedHunkAndOwner,
        std::numeric_limits<uint64_t>::max()};
  }
  return selection;
}

bool mapContainsRequiredAnchors(ArrayRef<int64_t> map,
                                ArrayRef<RequiredAnchor> required) {
  for (const RequiredAnchor &anchor : required) {
    if (anchor.aToken >= map.size() ||
        map[static_cast<size_t>(anchor.aToken)] !=
            static_cast<int64_t>(anchor.bToken))
      return false;
  }
  return true;
}

bool sameMutationOwner(const AlignmentSemanticMutationRecord &lhs,
                       const AlignmentSemanticMutationRecord &rhs) {
  return lhs.domain == rhs.domain &&
         lhs.ownerIncludeId == rhs.ownerIncludeId &&
         lhs.ownerId == rhs.ownerId &&
         lhs.protectedStructure == rhs.protectedStructure;
}

bool mutationCoveredBy(const AlignmentSemanticMutationRecord &mutation,
                       ArrayRef<AlignmentSemanticMutationRecord> covering) {
  if (mutation.IsInsertion())
    return false;
  return llvm::any_of(covering,
                      [&](const AlignmentSemanticMutationRecord &candidate) {
                        return !candidate.IsInsertion() &&
                               sameMutationOwner(mutation, candidate) &&
                               candidate.begin <= mutation.begin &&
                               mutation.end <= candidate.end;
                      });
}

bool hasWellFormedReplacement(
    const AlignmentSemanticMutationRecord &mutation) {
  return mutation.replacementBytes == mutation.replacementText.size();
}

bool sameExactMutation(const AlignmentSemanticMutationRecord &lhs,
                       const AlignmentSemanticMutationRecord &rhs) {
  return hasWellFormedReplacement(lhs) && hasWellFormedReplacement(rhs) &&
         sameMutationOwner(lhs, rhs) && lhs.begin == rhs.begin &&
         lhs.end == rhs.end &&
         lhs.replacementBytes == rhs.replacementBytes &&
         lhs.replacementText == rhs.replacementText;
}

/// Return true when applying `nested` to the original bytes of `covering`
/// reproduces exactly the replacement emitted by `covering`.
///
/// This is the theorem needed to distinguish a genuinely narrower source edit
/// from a different realization that merely happens to occupy a contained
/// interval. The relation is byte-exact: every nested edit must name the same
/// original bytes at its relative offset, nested edits must be disjoint, and
/// the reconstructed replacement must equal the wider replacement verbatim.
bool exactNestedTransformation(
    ArrayRef<const AlignmentSemanticMutationRecord *> nested,
    const AlignmentSemanticMutationRecord &covering) {
  if (!covering.hasExactByteTransformation || nested.empty() ||
      covering.end < covering.begin ||
      covering.originalText.size() != covering.end - covering.begin ||
      !hasWellFormedReplacement(covering))
    return false;

  std::vector<const AlignmentSemanticMutationRecord *> ordered(nested.begin(),
                                                                nested.end());
  llvm::sort(ordered,
             [](const AlignmentSemanticMutationRecord *lhs,
                const AlignmentSemanticMutationRecord *rhs) {
               if (lhs->begin != rhs->begin)
                 return lhs->begin < rhs->begin;
               return lhs->end < rhs->end;
             });

  uint64_t previousEnd = covering.begin;
  for (const AlignmentSemanticMutationRecord *mutation : ordered) {
    if (!mutation || mutation->IsInsertion() ||
        !mutation->hasExactByteTransformation ||
        mutation->end < mutation->begin ||
        mutation->originalText.size() != mutation->end - mutation->begin ||
        !hasWellFormedReplacement(*mutation) ||
        !sameMutationOwner(*mutation, covering) ||
        mutation->begin < covering.begin || covering.end < mutation->end ||
        mutation->begin < previousEnd)
      return false;

    const uint64_t relativeBegin = mutation->begin - covering.begin;
    const uint64_t relativeEnd = mutation->end - covering.begin;
    if (relativeEnd > covering.originalText.size() ||
        StringRef(mutation->originalText) !=
            StringRef(covering.originalText).slice(relativeBegin, relativeEnd))
      return false;
    previousEnd = mutation->end;
  }

  std::string reconstructed = covering.originalText;
  for (auto it = ordered.rbegin(); it != ordered.rend(); ++it) {
    const AlignmentSemanticMutationRecord &mutation = **it;
    const uint64_t relativeBegin = mutation.begin - covering.begin;
    const uint64_t relativeEnd = mutation.end - covering.begin;
    reconstructed.replace(static_cast<size_t>(relativeBegin),
                          static_cast<size_t>(relativeEnd - relativeBegin),
                          mutation.replacementText);
  }
  return reconstructed == covering.replacementText;
}

/// Prove that every source transformation in `candidate` is either identical
/// to, or byte-exactly nested inside, one transformation in `covering`.
///
/// Insertions require exact frontier and payload identity. Non-insertion edits
/// may share a wider carrier only when replaying all candidate edits inside that
/// carrier reproduces the wider replacement exactly. Ambiguous overlapping
/// covering records fail closed rather than choosing one by rank or proximity.
bool exactMutationSetCoveredBy(
    ArrayRef<AlignmentSemanticMutationRecord> candidate,
    ArrayRef<AlignmentSemanticMutationRecord> covering) {
  std::vector<std::vector<const AlignmentSemanticMutationRecord *>>
      nestedByCovering(covering.size());

  for (const AlignmentSemanticMutationRecord &mutation : candidate) {
    if (mutation.IsInsertion()) {
      const bool exactInsertion = llvm::any_of(
          covering, [&](const AlignmentSemanticMutationRecord &other) {
            return other.IsInsertion() && sameExactMutation(mutation, other);
          });
      if (!exactInsertion)
        return false;
      continue;
    }

    std::optional<size_t> coveringIndex;
    for (size_t index = 0; index < covering.size(); ++index) {
      const AlignmentSemanticMutationRecord &other = covering[index];
      if (other.IsInsertion() || !sameMutationOwner(mutation, other) ||
          other.begin > mutation.begin || mutation.end > other.end)
        continue;
      if (coveringIndex)
        return false;
      coveringIndex = index;
    }
    if (!coveringIndex)
      return false;
    nestedByCovering[*coveringIndex].push_back(&mutation);
  }

  for (size_t index = 0; index < covering.size(); ++index) {
    ArrayRef<const AlignmentSemanticMutationRecord *> nested =
        nestedByCovering[index];
    if (nested.empty())
      continue;
    if (nested.size() == 1 && sameExactMutation(*nested.front(), covering[index]))
      continue;
    if (!exactNestedTransformation(nested, covering[index]))
      return false;
  }
  return true;
}

template <typename T>
bool sortedSetIsSubset(ArrayRef<T> subset, ArrayRef<T> superset) {
  return std::includes(superset.begin(), superset.end(), subset.begin(),
                       subset.end());
}

/// Return true when `candidate` mutates no additional existing source carrier
/// and expands no additional producer owner relative to `other`.
///
/// Insertions are intentionally excluded: moving an insertion frontier is not
/// source destruction and is handled by exact realized-source equivalence.
/// Semantic postconditions must agree before this partial order is applied.
bool noMoreDestructive(
    const AlignmentSemanticSimulationResult &candidate,
    const AlignmentSemanticSimulationResult &other) {
  const AlignmentSemanticPreservationFootprint &lhs =
      candidate.components.preservationFootprint;
  const AlignmentSemanticPreservationFootprint &rhs =
      other.components.preservationFootprint;
  if (!lhs.semanticPostconditionComplete ||
      !rhs.semanticPostconditionComplete ||
      lhs.semanticPostconditionKey != rhs.semanticPostconditionKey)
    return false;

  for (const AlignmentSemanticMutationRecord &mutation : lhs.mutations) {
    if (!mutation.IsInsertion() &&
        !mutationCoveredBy(mutation, rhs.mutations))
      return false;
  }
  return sortedSetIsSubset<uint64_t>(lhs.expandedIncludeIds,
                                     rhs.expandedIncludeIds) &&
         sortedSetIsSubset<uint64_t>(lhs.expandedMacroRootIds,
                                     rhs.expandedMacroRootIds);
}

bool strictlyMorePreserving(
    const AlignmentSemanticSimulationResult &candidate,
    const AlignmentSemanticSimulationResult &other) {
  return noMoreDestructive(candidate, other) &&
         !noMoreDestructive(other, candidate);
}

/// Return true when `candidate` changes a subset of the source carriers changed
/// by `other`.
///
/// This relation is used only after both simulations have independently passed
/// the complete end-to-end theorem for the same A/B token streams. It compares
/// exact source transformations rather than interval size: a narrower edit is
/// contained only when applying it inside the wider original carrier reproduces
/// the wider replacement byte-for-byte. Insertions require an identical
/// recorded frontier and payload.
bool noMoreSourceDestructive(
    const AlignmentSemanticSimulationResult &candidate,
    const AlignmentSemanticSimulationResult &other) {
  const AlignmentSemanticPreservationFootprint &lhs =
      candidate.components.preservationFootprint;
  const AlignmentSemanticPreservationFootprint &rhs =
      other.components.preservationFootprint;
  return exactMutationSetCoveredBy(lhs.mutations, rhs.mutations) &&
         sortedSetIsSubset<uint64_t>(lhs.expandedIncludeIds,
                                     rhs.expandedIncludeIds) &&
         sortedSetIsSubset<uint64_t>(lhs.expandedMacroRootIds,
                                     rhs.expandedMacroRootIds);
}


[[maybe_unused]] StringRef basisName(AlignmentSemanticAnchorBasis basis) {
  switch (basis) {
  case AlignmentSemanticAnchorBasis::FinalSourceNecessary:
    return "final-source-necessary";
  case AlignmentSemanticAnchorBasis::SemanticPostconditionNecessary:
    return "semantic-postcondition-necessary";
  case AlignmentSemanticAnchorBasis::SourcePreservationNecessary:
    return "source-preservation-necessary";
  case AlignmentSemanticAnchorBasis::EquivalentRealizationRepresentative:
    return "equivalent-realization-representative";
  }
  return "unknown";
}

[[maybe_unused]] std::string formatMapIndices(ArrayRef<size_t> indices) {
  std::string text = "[";
  for (size_t index = 0; index < indices.size(); ++index) {
    if (index != 0)
      text.push_back(',');
    text.append(std::to_string(indices[index]));
  }
  text.push_back(']');
  return text;
}

} // namespace

RefoldAlignmentSemanticResolver::RefoldAlignmentSemanticResolver(
    Dependencies deps)
    : deps_(std::move(deps)) {}

RefoldAlignmentSemanticResolver::ResolutionResult
RefoldAlignmentSemanticResolver::Resolve() const {
  ResolutionResult result;
  result.selectedMap = deps_.coreAlignment.forcedMap;
  result.selectedAnchorProofs.assign(result.selectedMap.size(),
                                     diffutils::LcsAnchorProof{});
  for (size_t aToken = 0; aToken < result.selectedMap.size(); ++aToken) {
    if (result.selectedMap[aToken] >= 0) {
      result.selectedAnchorProofs[aToken] = diffutils::LcsAnchorProof{
          diffutils::LcsAnchorProofKind::CoreOptimalPathForced, 0};
    }
  }

  if (!deps_.simulate ||
      deps_.coreAlignment.forcedMap.size() != deps_.aLexemes.size())
    return result;

  // Resolve window by window in source order. A committed window's anchors
  // become part of the base map for every later window, so each simulation
  // observes the alignment production would actually use up to that point.
  std::vector<int64_t> baseMap = deps_.coreAlignment.forcedMap;
  std::vector<WindowResolution> committedWindows;
  size_t simulationBudget =
      getCandidateSimulationBudget(deps_.aLexemes.size());
  size_t windowsWithOracle = 0;
  for (size_t windowIndex = 0;
       windowIndex < deps_.coreAlignment.certificationWindows.size();
       ++windowIndex) {
    // Materialize this window's pair facts, resolve, then release them before
    // moving on. Holding every window's facts at once would reintroduce the
    // complete-grid payload that partitioning exists to avoid.
    if (deps_.retainWindowOracle)
      (void)deps_.retainWindowOracle(windowIndex);
    if (!deps_.coreAlignment.HasCompleteSemanticOracleForWindow(windowIndex)) {
      REFOLD_LOG_TRACE(
          "lcs/semantic-resolver",
          "window {0} keeps core-forced anchors: no retained all-optimal "
          "pair facts",
          windowIndex);
      continue;
    }
    ++windowsWithOracle;
    WindowResolution resolution =
        ResolveCertificationWindow(windowIndex, baseMap, simulationBudget);
    if (deps_.releaseWindowOracle)
      deps_.releaseWindowOracle(windowIndex);
    // Charge whether or not the window committed: a declined window still
    // re-planned the unit once per candidate.
    simulationBudget -= std::min(simulationBudget, resolution.simulationsSpent);
    if (!resolution.committed)
      continue;
    baseMap = resolution.selectedMap;
    committedWindows.push_back(std::move(resolution));
  }

  result.completeEnumeration = windowsWithOracle != 0;
  if (committedWindows.empty()) {
    REFOLD_LOG_TRACE(
        "lcs/semantic-resolver",
        "forced-only alignment retained: {0} window(s) had retained pair "
        "facts, none resolved to one admissible realization",
        windowsWithOracle);
    return result;
  }

  // Each committed window carries its own durable witness. Every witness
  // records the final complete-stream map so the production planner can check
  // any anchor against any witness, while anchor evidence stays scoped to the
  // window that proved it.
  result.selectedMap = baseMap;
  result.selectedAnchorProofs.assign(result.selectedMap.size(),
                                     diffutils::LcsAnchorProof{});
  for (size_t aToken = 0; aToken < result.selectedMap.size(); ++aToken) {
    if (result.selectedMap[aToken] >= 0 &&
        isForcedAnchor(deps_.coreAlignment, aToken,
                       result.selectedMap[aToken])) {
      result.selectedAnchorProofs[aToken] = diffutils::LcsAnchorProof{
          diffutils::LcsAnchorProofKind::CoreOptimalPathForced, 0};
    }
  }

  uint64_t nextWitnessId = 1;
  for (WindowResolution &resolution : committedWindows) {
    AlignmentSemanticResolutionWitness witness;
    witness.witnessId = nextWitnessId++;
    witness.enumeratedMapCount = resolution.enumeratedMapCount;
    witness.acceptedMapCount = resolution.acceptedMapCount;
    witness.rejectedMapCount = resolution.rejectedMapCount;
    witness.completeEnumeration = true;
    witness.equivalenceKey = std::move(resolution.equivalenceKey);
    witness.representativeMap = result.selectedMap;
    witness.anchorEvidence = std::move(resolution.anchorEvidence);
    for (const AlignmentSemanticAnchorEvidence &evidence :
         witness.anchorEvidence) {
      if (evidence.aToken >= result.selectedAnchorProofs.size())
        return ResolutionResult{};
      result.selectedAnchorProofs[evidence.aToken] = diffutils::LcsAnchorProof{
          diffutils::LcsAnchorProofKind::EquivalentNormalizedHunkAndOwner,
          witness.witnessId};
    }
    result.witnesses.push_back(std::move(witness));
  }

  // Every mapped A token must carry authority by now: forced anchors above,
  // window-proved anchors from the loop. An unexplained mapped token would
  // reach the planner as an unauthorized anchor, so fail closed instead.
  for (size_t aToken = 0; aToken < result.selectedMap.size(); ++aToken) {
    if (result.selectedMap[aToken] >= 0 &&
        !result.selectedAnchorProofs[aToken].IsAuthorized()) {
      REFOLD_LOG_TRACE(
          "lcs/semantic-resolver",
          "forced-only alignment retained: A[{0}] has no anchor authority "
          "after per-window resolution",
          aToken);
      ResolutionResult forcedOnly;
      forcedOnly.selectedMap = deps_.coreAlignment.forcedMap;
      forcedOnly.selectedAnchorProofs.assign(
          forcedOnly.selectedMap.size(), diffutils::LcsAnchorProof{});
      for (size_t token = 0; token < forcedOnly.selectedMap.size(); ++token) {
        if (forcedOnly.selectedMap[token] >= 0) {
          forcedOnly.selectedAnchorProofs[token] = diffutils::LcsAnchorProof{
              diffutils::LcsAnchorProofKind::CoreOptimalPathForced, 0};
        }
      }
      forcedOnly.completeEnumeration = result.completeEnumeration;
      return forcedOnly;
    }
  }

  result.committedEquivalentClass = true;
  return result;
}

RefoldAlignmentSemanticResolver::WindowResolution
RefoldAlignmentSemanticResolver::ResolveCertificationWindow(
    size_t windowIndex, ArrayRef<int64_t> baseMap,
    size_t simulationBudget) const {
  WindowResolution result;

  const diffutils::OptimalTokenAlignmentOracle *oraclePtr =
      deps_.coreAlignment.GetSemanticOracleForWindow(windowIndex);
  if (!oraclePtr ||
      windowIndex >= deps_.coreAlignment.certificationWindows.size() ||
      baseMap.size() != deps_.aLexemes.size())
    return result;
  const diffutils::OptimalTokenAlignmentOracle &oracle = *oraclePtr;
  const diffutils::LcsCertificationWindow &window =
      deps_.coreAlignment.certificationWindows[windowIndex];

  // Oracle coordinates are window-local; the enumeration below therefore
  // translates each stream rectangle into the oracle's frame and translates
  // every returned B index back.
  const uint64_t oracleABegin = window.aBegin;
  const uint64_t oracleBBegin = window.bBegin;

  const std::vector<ForcedAnchor> forcedAnchors =
      collectForcedAnchors(deps_.coreAlignment, window.aBegin, window.aEnd);
  std::vector<std::vector<std::vector<int64_t>>> subWindowMaps;
  std::vector<uint64_t> subWindowABegins;
  uint64_t aBegin = window.aBegin;
  uint64_t bBegin = window.bBegin;
  for (size_t anchorIndex = 0; anchorIndex <= forcedAnchors.size();
       ++anchorIndex) {
    const uint64_t aEnd = anchorIndex < forcedAnchors.size()
                              ? forcedAnchors[anchorIndex].aToken
                              : window.aEnd;
    const uint64_t bEnd = anchorIndex < forcedAnchors.size()
                              ? forcedAnchors[anchorIndex].bToken
                              : window.bEnd;
    if (aBegin > aEnd || bBegin > bEnd || aEnd > window.aEnd ||
        bEnd > window.bEnd)
      return result;

    diffutils::OptimalLcsMapEnumeration enumeration =
        oracle.EnumerateOptimalMapsForWindow(
            aBegin - oracleABegin, aEnd - oracleABegin, bBegin - oracleBBegin,
            bEnd - oracleBBegin, MaxUniqueMapsPerForcedWindow);
    if (!enumeration.complete || enumeration.maps.empty()) {
      REFOLD_LOG_TRACE(
          "lcs/semantic-resolver",
          "window {0} keeps core-forced anchors: exact map enumeration "
          "incomplete for A=[{1},{2}) B=[{3},{4})",
          windowIndex, aBegin, aEnd, bBegin, bEnd);
      return result;
    }
    for (std::vector<int64_t> &map : enumeration.maps) {
      for (int64_t &mapped : map) {
        if (mapped >= 0)
          mapped += static_cast<int64_t>(oracleBBegin);
      }
    }
    subWindowMaps.push_back(std::move(enumeration.maps));
    subWindowABegins.push_back(aBegin);
    if (anchorIndex < forcedAnchors.size()) {
      aBegin = forcedAnchors[anchorIndex].aToken + 1;
      bBegin = forcedAnchors[anchorIndex].bToken + 1;
    }
  }

  size_t globalCandidateCount = 1;
  for (const auto &maps : subWindowMaps) {
    if (maps.size() > MaxGlobalSemanticCandidateMaps /
                          std::max<size_t>(globalCandidateCount, 1)) {
      REFOLD_LOG_TRACE(
          "lcs/semantic-resolver",
          "window {0} keeps core-forced anchors: complete map product "
          "exceeds proof budget ({1})",
          windowIndex, MaxGlobalSemanticCandidateMaps);
      return result;
    }
    globalCandidateCount *= maps.size();
  }

  std::vector<std::vector<int64_t>> globalMaps;
  globalMaps.emplace_back(baseMap.begin(), baseMap.end());
  for (size_t subWindow = 0; subWindow < subWindowMaps.size(); ++subWindow) {
    const uint64_t subABegin = subWindowABegins[subWindow];
    std::vector<std::vector<int64_t>> expanded;
    expanded.reserve(globalMaps.size() * subWindowMaps[subWindow].size());
    for (const std::vector<int64_t> &base : globalMaps) {
      for (const std::vector<int64_t> &local : subWindowMaps[subWindow]) {
        if (static_cast<size_t>(subABegin) + local.size() > base.size())
          return result;
        std::vector<int64_t> candidate = base;
        for (size_t localA = 0; localA < local.size(); ++localA)
          candidate[static_cast<size_t>(subABegin) + localA] = local[localA];
        expanded.push_back(std::move(candidate));
      }
    }
    globalMaps = std::move(expanded);
  }

  llvm::sort(globalMaps,
             [](const std::vector<int64_t> &lhs,
                const std::vector<int64_t> &rhs) {
               return std::lexicographical_compare(
                   lhs.begin(), lhs.end(), rhs.begin(), rhs.end());
             });
  globalMaps.erase(std::unique(globalMaps.begin(), globalMaps.end()),
                   globalMaps.end());
  if (globalMaps.size() <= 1) {
    REFOLD_LOG_TRACE(
        "lcs/semantic-resolver",
        "window {0} has no alignment ambiguity: exactly one complete "
        "core-optimal map",
        windowIndex);
    return result;
  }

  if (globalMaps.size() > simulationBudget) {
    REFOLD_LOG_TRACE(
        "lcs/semantic-resolver",
        "window {0} keeps core-forced anchors: {1} candidates exceed the "
        "run's remaining simulation budget ({2})",
        windowIndex, globalMaps.size(), simulationBudget);
    return result;
  }

  std::vector<AlignmentSemanticSimulationResult> simulations;
  simulations.reserve(globalMaps.size());
  for (const std::vector<int64_t> &candidateMap : globalMaps) {
    if (!isStrictlyMonotoneMap(candidateMap, deps_.bLexemes.size()) ||
        !mapLexemesAgree(candidateMap, deps_.aLexemes, deps_.bLexemes)) {
      REFOLD_LOG_TRACE(
          "lcs/semantic-resolver",
          "window {0} keeps core-forced anchors: an enumerated map is not a "
          "monotone lexeme-agreeing alignment",
          windowIndex);
      return WindowResolution{};
    }
    simulations.push_back(
        deps_.simulate(buildSimulationSelection(candidateMap,
                                                deps_.coreAlignment)));
  }
  result.simulationsSpent = simulations.size();

  auto commitRealizationClass =
      [&](StringRef equivalenceKey, ArrayRef<size_t> classMembers,
          ArrayRef<RequiredAnchor> requiredAnchors) -> WindowResolution {
    WindowResolution committed;
    if (classMembers.empty())
      return committed;

    const size_t representativeIndex = classMembers.front();
    committed.enumeratedMapCount = globalMaps.size();
    committed.acceptedMapCount = classMembers.size();
    committed.rejectedMapCount =
        committed.enumeratedMapCount - committed.acceptedMapCount;
    committed.equivalenceKey = equivalenceKey.str();
    committed.selectedMap = globalMaps[representativeIndex];

    std::map<std::pair<uint64_t, uint64_t>, AlignmentSemanticAnchorBasis>
        requiredBasis;
    for (const RequiredAnchor &anchor : requiredAnchors)
      requiredBasis[{anchor.aToken, anchor.bToken}] = anchor.basis;

    // Record evidence only for anchors this window actually chose. Anchors
    // outside the window are either core-forced or already proved by an
    // earlier window's witness, and restating them here would claim authority
    // this enumeration never established.
    for (size_t aToken = 0; aToken < committed.selectedMap.size(); ++aToken) {
      const int64_t bToken = committed.selectedMap[aToken];
      if (bToken < 0 || aToken < window.aBegin || aToken >= window.aEnd ||
          isForcedAnchor(deps_.coreAlignment, aToken, bToken))
        continue;

      const auto required = requiredBasis.find(
          {aToken, static_cast<uint64_t>(bToken)});
      const AlignmentSemanticAnchorBasis basis =
          required == requiredBasis.end()
              ? AlignmentSemanticAnchorBasis::
                    EquivalentRealizationRepresentative
              : required->second;
      committed.anchorEvidence.push_back(AlignmentSemanticAnchorEvidence{
          aToken, static_cast<uint64_t>(bToken), basis});
    }

    REFOLD_LOG_TRACE(
        "lcs/semantic-resolver",
        "window {0} committing realized-source class: candidates={1} "
        "classMembers={2} requiredAnchors={3} representative={4} anchors={5}",
        windowIndex, globalMaps.size(), formatMapIndices(classMembers),
        requiredAnchors.size(), representativeIndex,
        committed.anchorEvidence.size());

    committed.committed = true;
    return committed;
  };

  // Permanent census of the simulated candidate set. Every commit rule below
  // is a statement about these counts, so reporting them makes a declined
  // window explain itself instead of failing silently. Reading finished
  // simulation records cannot affect candidate order or any proof decision.
  if (inTraceMode()) {
    size_t acceptedCount = 0;
    size_t terminalFallbackCount = 0;
    size_t proofIncompleteCount = 0;
    std::set<StringRef> concreteOutputKeys;
    std::set<StringRef> realizationKeys;
    for (const AlignmentSemanticSimulationResult &simulation : simulations) {
      switch (simulation.disposition) {
      case AlignmentSemanticSimulationDisposition::Accepted:
        ++acceptedCount;
        break;
      case AlignmentSemanticSimulationDisposition::TerminalFallback:
        ++terminalFallbackCount;
        break;
      case AlignmentSemanticSimulationDisposition::ProofIncomplete:
        ++proofIncompleteCount;
        break;
      }
      if (!simulation.concreteOutputEquivalenceKey.empty())
        concreteOutputKeys.insert(simulation.concreteOutputEquivalenceKey);
      if (!simulation.realizationEquivalenceKey.empty())
        realizationKeys.insert(simulation.realizationEquivalenceKey);
    }
    REFOLD_LOG_TRACE(
        "lcs/semantic-resolver",
        "window {0} candidate census: enumerated={1} accepted={2} "
        "terminalFallback={3} proofIncomplete={4} distinctConcreteOutputs={5} "
        "distinctRealizations={6}",
        windowIndex, simulations.size(), acceptedCount, terminalFallbackCount,
        proofIncompleteCount, concreteOutputKeys.size(),
        realizationKeys.size());
  }

  // The strongest equivalence theorem needs no historical boundary proposal:
  // when every complete core-optimal map independently satisfies the full
  // theorem audit and all maps produce one exact concrete output plan, the
  // alignment ambiguity is observationally irrelevant.  Internal accepted-
  // carrier provenance is intentionally excluded here: different hunk
  // partitions may select different proof paths while emitting identical
  // source bytes, mappings, source-graph artifacts, and line-pruning inputs.
  // Those proof paths remain available in the stricter realization key below
  // whenever the concrete plans differ.
  std::map<std::string, std::vector<size_t>> completeOutputClasses;
  bool everyCompleteMapAccepted = true;
  for (size_t mapIndex = 0; mapIndex < simulations.size(); ++mapIndex) {
    const AlignmentSemanticSimulationResult &simulation =
        simulations[mapIndex];
    if (simulation.disposition !=
            AlignmentSemanticSimulationDisposition::Accepted ||
        !simulation.accepted ||
        simulation.concreteOutputEquivalenceKey.empty()) {
      everyCompleteMapAccepted = false;
      break;
    }
    completeOutputClasses[simulation.concreteOutputEquivalenceKey]
        .push_back(mapIndex);
  }
  if (everyCompleteMapAccepted && completeOutputClasses.size() == 1) {
    const auto &onlyClass = *completeOutputClasses.begin();
    return commitRealizationClass(onlyClass.first, onlyClass.second,
                                  ArrayRef<RequiredAnchor>());
  }
  REFOLD_LOG_TRACE(
      "lcs/semantic-resolver",
      "window {0} is not observationally irrelevant: everyMapAccepted={1} "
      "distinctConcreteOutputClasses={2}",
      windowIndex, everyCompleteMapAccepted, completeOutputClasses.size());

  // A complete accepted simulation is an independent proof that its source
  // realization reproduces the requested B stream and preserves every tracked
  // preprocessing postcondition.  Carrier-specific witness spellings may still
  // separate two such simulations even when one changes a strict subset of the
  // other's original source carriers.  Select a map only when exhaustive
  // enumeration proves a global least element under exact source-
  // transformation containment. Incomparable minima, incomplete proofs, and
  // multiple realization classes remain ambiguous and retain the forced-only
  // map.
  bool hasProofIncompleteMap = false;
  std::vector<size_t> acceptedMaps;
  for (size_t mapIndex = 0; mapIndex < simulations.size(); ++mapIndex) {
    const AlignmentSemanticSimulationResult &simulation =
        simulations[mapIndex];
    if (simulation.disposition ==
        AlignmentSemanticSimulationDisposition::ProofIncomplete) {
      hasProofIncompleteMap = true;
      break;
    }
    if (simulation.disposition ==
            AlignmentSemanticSimulationDisposition::Accepted &&
        simulation.accepted &&
        !simulation.realizationEquivalenceKey.empty())
      acceptedMaps.push_back(mapIndex);
  }

  if (!hasProofIncompleteMap && !acceptedMaps.empty()) {
    std::vector<size_t> leastDestructiveMaps;
    for (size_t candidateIndex : acceptedMaps) {
      const bool noMoreDestructiveThanEveryAccepted = llvm::all_of(
          acceptedMaps, [&](size_t otherIndex) {
            return noMoreSourceDestructive(simulations[candidateIndex],
                                           simulations[otherIndex]);
          });
      if (noMoreDestructiveThanEveryAccepted)
        leastDestructiveMaps.push_back(candidateIndex);
    }

    // Class the surviving minima by their concrete emitted artifact, not by
    // internal proof-carrier provenance. Every member of `acceptedMaps` has
    // already passed the complete end-to-end theorem audit independently, which
    // is exactly the precondition `concreteOutputEquivalenceKey` documents for
    // itself: at that point the authoritative question is whether alignment
    // ambiguity changes the emitted source and the deterministic post-emission
    // pruning inputs, not which carrier proved the same bytes. Two minima that
    // differ only in the structural-tiling witness spelling emit identical
    // source, so separating them here would manufacture ambiguity the output
    // does not have. The stricter realization key still governs the
    // counterfactual path below, where members have not all been audited.
    std::map<std::string, std::vector<size_t>> leastRealizationClasses;
    for (size_t mapIndex : leastDestructiveMaps) {
      const AlignmentSemanticSimulationResult &simulation =
          simulations[mapIndex];
      if (simulation.concreteOutputEquivalenceKey.empty()) {
        leastRealizationClasses.clear();
        break;
      }
      leastRealizationClasses[simulation.concreteOutputEquivalenceKey]
          .push_back(mapIndex);
    }

    if (leastRealizationClasses.size() == 1) {
      const auto &onlyClass = *leastRealizationClasses.begin();
      REFOLD_LOG_TRACE(
          "lcs/semantic-resolver",
          "window {0} committing globally least source-mutation class: "
          "accepted={1} least={2} classMembers={3}",
          windowIndex, acceptedMaps.size(), leastDestructiveMaps.size(),
          formatMapIndices(onlyClass.second));
      return commitRealizationClass(onlyClass.first, onlyClass.second,
                                    ArrayRef<RequiredAnchor>());
    }
    REFOLD_LOG_TRACE(
        "lcs/semantic-resolver",
        "window {0} has no unique least source-mutation class: accepted={1} "
        "leastDestructive={2} leastRealizationClasses={3}",
        windowIndex, acceptedMaps.size(), leastDestructiveMaps.size(),
        leastRealizationClasses.size());
  } else {
    REFOLD_LOG_TRACE(
        "lcs/semantic-resolver",
        "window {0} skips source-mutation containment: proofIncomplete={1} "
        "accepted={2}",
        windowIndex, hasProofIncompleteMap, acceptedMaps.size());
  }

  // Reconstruct the old boundary map strictly as a proposal. The old balance
  // and surface ranks cannot authorize an anchor; they only identify the
  // counterfactuals that the theorem below must discharge.
  if (deps_.aGapProvenance.size() != deps_.aLexemes.size() + 1 ||
      deps_.bGapProvenance.size() != deps_.bLexemes.size() + 1)
    return WindowResolution{};
  LegacyAlignmentDiagnosticResult proposal =
      reconstructLegacyBoundaryProposal(
          deps_.aLexemes, deps_.bLexemes, deps_.aGapProvenance,
          deps_.bGapProvenance, deps_.coreAlignment);
  if (!proposal.complete || !proposal.monotone || !proposal.lexemesAgree ||
      !proposal.jointlyCoreOptimal) {
    REFOLD_LOG_TRACE(
        "lcs/semantic-resolver",
        "window {0} keeps core-forced anchors: legacy boundary proposal is "
        "not a complete jointly core-optimal map",
        windowIndex);
    return WindowResolution{};
  }

  // Only anchors inside this window are counterfactual candidates. A proposal
  // anchor elsewhere is not something this window's enumeration varies, so
  // testing it would spend a full simulation to prove an anchor no candidate
  // here disagrees about.
  std::vector<RequiredAnchor> requiredAnchors;
  const AlignmentSemanticSimulationResult *proposalSimulation = nullptr;
  std::optional<AlignmentSemanticSimulationResult> ownedProposalSimulation;
  size_t proposalNonForcedCount = 0;
  for (size_t aToken = 0; aToken < proposal.selectedMap.size(); ++aToken) {
    const int64_t bToken = proposal.selectedMap[aToken];
    if (bToken >= 0 && aToken >= window.aBegin && aToken < window.aEnd &&
        !isForcedAnchor(deps_.coreAlignment, aToken, bToken))
      ++proposalNonForcedCount;
  }

  if (proposalNonForcedCount != 0) {
    if (proposalNonForcedCount > MaxProposalCounterfactuals) {
      REFOLD_LOG_TRACE(
          "lcs/semantic-resolver",
          "window {0} keeps core-forced anchors: {1} proposal anchors exceed "
          "the counterfactual budget ({2})",
          windowIndex, proposalNonForcedCount, MaxProposalCounterfactuals);
      return WindowResolution{};
    }
    // The historical proposal is commonly one of the already simulated
    // complete maps. Reuse that byte-identical theorem result instead of
    // cloning and refolding the entire translation unit a second time.
    const auto proposalMap = std::lower_bound(
        globalMaps.begin(), globalMaps.end(), proposal.selectedMap);
    if (proposalMap != globalMaps.end() &&
        *proposalMap == proposal.selectedMap) {
      const size_t proposalIndex =
          static_cast<size_t>(proposalMap - globalMaps.begin());
      proposalSimulation = &simulations[proposalIndex];
    } else {
      ownedProposalSimulation.emplace(deps_.simulate(
          buildSimulationSelection(proposal.selectedMap,
                                   deps_.coreAlignment)));
      proposalSimulation = &*ownedProposalSimulation;
    }
    if (!proposalSimulation->accepted) {
      REFOLD_LOG_TRACE(
          "lcs/semantic-resolver",
          "window {0} keeps core-forced anchors: the legacy proposal's own "
          "simulation was not accepted ('{1}')",
          windowIndex, proposalSimulation->rejectionReason);
      return WindowResolution{};
    }

    for (size_t aToken = 0; aToken < proposal.selectedMap.size(); ++aToken) {
      const int64_t bToken = proposal.selectedMap[aToken];
      if (bToken < 0 || aToken < window.aBegin || aToken >= window.aEnd ||
          isForcedAnchor(deps_.coreAlignment, aToken, bToken))
        continue;

      std::vector<int64_t> counterfactualMap = proposal.selectedMap;
      counterfactualMap[aToken] = -1;
      AlignmentSemanticSimulationResult counterfactual = deps_.simulate(
          buildSimulationSelection(counterfactualMap,
                                   deps_.coreAlignment));
      if (!counterfactual.accepted) {
        // A leave-one-out map is intentionally partial. If the isolated
        // planner cannot certify that partial realization, the result supplies
        // no affirmative basis for requiring this anchor. It must not erase
        // independent necessity proofs already established for other anchors:
        // leaving this anchor optional preserves every complete core-optimal
        // competitor, and the exhaustive proof-incompleteness and exact
        // realization-class checks below still fail closed when ambiguity
        // remains.
        REFOLD_LOG_TRACE(
            "lcs/semantic-resolver/counterfactual",
            "anchor A[{0}]->B[{1}] remains optional: leave-one-out "
            "simulation was not accepted ('{2}')",
            aToken, bToken, counterfactual.rejectionReason);
        continue;
      }

      std::optional<AlignmentSemanticAnchorBasis> basis;
      if (proposalSimulation->components.finalTU !=
          counterfactual.components.finalTU) {
        basis = AlignmentSemanticAnchorBasis::FinalSourceNecessary;
      } else if (proposalSimulation->components.semanticPostconditions !=
                 counterfactual.components.semanticPostconditions) {
        basis =
            AlignmentSemanticAnchorBasis::SemanticPostconditionNecessary;
      } else if (strictlyMorePreserving(*proposalSimulation,
                                        counterfactual)) {
        basis = AlignmentSemanticAnchorBasis::SourcePreservationNecessary;
      }

      if (basis) {
        requiredAnchors.push_back(RequiredAnchor{
            aToken, static_cast<uint64_t>(bToken), *basis});
        REFOLD_LOG_TRACE(
            "lcs/semantic-resolver/counterfactual",
            "required A[{0}]->B[{1}] basis={2}", aToken, bToken,
            basisName(*basis));
      }
    }
  }

  std::vector<size_t> survivingMaps;
  for (size_t mapIndex = 0; mapIndex < globalMaps.size(); ++mapIndex) {
    if (mapContainsRequiredAnchors(globalMaps[mapIndex], requiredAnchors))
      survivingMaps.push_back(mapIndex);
  }
  if (survivingMaps.empty()) {
    REFOLD_LOG_TRACE(
        "lcs/semantic-resolver",
        "window {0} keeps core-forced anchors: no enumerated map contains "
        "every one of the {1} required anchors",
        windowIndex, requiredAnchors.size());
    return WindowResolution{};
  }

  // Unknown witness dimensions are not semantic rejections. If a surviving
  // core-optimal map is proof-incomplete, uniqueness has not been established
  // and the resolver must retain only forced anchors.
  for (size_t mapIndex : survivingMaps) {
    if (simulations[mapIndex].disposition ==
        AlignmentSemanticSimulationDisposition::ProofIncomplete) {
      REFOLD_LOG_TRACE(
          "lcs/semantic-resolver",
          "window {0} keeps core-forced anchors: surviving map {1} has "
          "incomplete proof ('{2}')",
          windowIndex, mapIndex, simulations[mapIndex].rejectionReason);
      return WindowResolution{};
    }
  }

  std::map<std::string, std::vector<size_t>> realizationClasses;
  for (size_t mapIndex : survivingMaps) {
    const AlignmentSemanticSimulationResult &simulation =
        simulations[mapIndex];
    if (simulation.disposition ==
        AlignmentSemanticSimulationDisposition::TerminalFallback)
      continue;
    if (!simulation.accepted ||
        simulation.realizationEquivalenceKey.empty()) {
      REFOLD_LOG_TRACE(
          "lcs/semantic-resolver",
          "window {0} keeps core-forced anchors: surviving map {1} carries no "
          "realization key ('{2}')",
          windowIndex, mapIndex, simulation.rejectionReason);
      return WindowResolution{};
    }
    realizationClasses[simulation.realizationEquivalenceKey].push_back(
        mapIndex);
  }
  if (realizationClasses.size() != 1) {
    REFOLD_LOG_TRACE(
        "lcs/semantic-resolver",
        "window {0} keeps core-forced anchors: {1} surviving map(s) span {2} "
        "distinct realization classes after {3} required anchor(s)",
        windowIndex, survivingMaps.size(), realizationClasses.size(),
        requiredAnchors.size());
    return WindowResolution{};
  }

  const auto &onlyClass = *realizationClasses.begin();
  return commitRealizationClass(onlyClass.first, onlyClass.second,
                                requiredAnchors);
}

} // namespace refold
} // namespace clang
