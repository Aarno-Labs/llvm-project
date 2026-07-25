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

std::vector<ForcedAnchor>
collectForcedAnchors(const diffutils::CertifiedLcsResult &alignment) {
  std::vector<ForcedAnchor> anchors;
  for (size_t aToken = 0; aToken < alignment.forcedMap.size(); ++aToken) {
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

  const diffutils::OptimalTokenAlignmentOracle &oracle =
      deps_.coreAlignment.oracle;
  if (!deps_.coreAlignment.HasCompleteSemanticOracleForWindow(
          /*windowIndex=*/0) ||
      !deps_.simulate ||
      deps_.coreAlignment.forcedMap.size() != deps_.aLexemes.size() ||
      oracle.GetATokenCount() != deps_.aLexemes.size() ||
      oracle.GetBTokenCount() != deps_.bLexemes.size())
    return result;

  const std::vector<ForcedAnchor> forcedAnchors =
      collectForcedAnchors(deps_.coreAlignment);
  std::vector<std::vector<std::vector<int64_t>>> windowMaps;
  uint64_t aBegin = 0;
  uint64_t bBegin = 0;
  for (size_t anchorIndex = 0; anchorIndex <= forcedAnchors.size();
       ++anchorIndex) {
    const uint64_t aEnd = anchorIndex < forcedAnchors.size()
                              ? forcedAnchors[anchorIndex].aToken
                              : deps_.aLexemes.size();
    const uint64_t bEnd = anchorIndex < forcedAnchors.size()
                              ? forcedAnchors[anchorIndex].bToken
                              : deps_.bLexemes.size();
    if (aBegin > aEnd || bBegin > bEnd)
      return result;

    diffutils::OptimalLcsMapEnumeration enumeration =
        oracle.EnumerateOptimalMapsForWindow(
            aBegin, aEnd, bBegin, bEnd, MaxUniqueMapsPerForcedWindow);
    if (!enumeration.complete || enumeration.maps.empty()) {
      REFOLD_LOG_TRACE(
          "lcs/semantic-resolver",
          "forced-only alignment retained: exact map enumeration incomplete "
          "for A=[{0},{1}) B=[{2},{3})",
          aBegin, aEnd, bBegin, bEnd);
      return result;
    }
    windowMaps.push_back(std::move(enumeration.maps));
    if (anchorIndex < forcedAnchors.size()) {
      aBegin = forcedAnchors[anchorIndex].aToken + 1;
      bBegin = forcedAnchors[anchorIndex].bToken + 1;
    }
  }

  size_t globalCandidateCount = 1;
  for (const auto &maps : windowMaps) {
    if (maps.size() > MaxGlobalSemanticCandidateMaps /
                          std::max<size_t>(globalCandidateCount, 1)) {
      REFOLD_LOG_TRACE(
          "lcs/semantic-resolver",
          "forced-only alignment retained: complete map product exceeds "
          "proof budget ({0})",
          MaxGlobalSemanticCandidateMaps);
      return result;
    }
    globalCandidateCount *= maps.size();
  }

  std::vector<std::vector<int64_t>> globalMaps;
  globalMaps.push_back(deps_.coreAlignment.forcedMap);
  aBegin = 0;
  for (size_t windowIndex = 0; windowIndex < windowMaps.size();
       ++windowIndex) {
    const uint64_t aEnd = windowIndex < forcedAnchors.size()
                              ? forcedAnchors[windowIndex].aToken
                              : deps_.aLexemes.size();
    std::vector<std::vector<int64_t>> expanded;
    expanded.reserve(globalMaps.size() * windowMaps[windowIndex].size());
    for (const std::vector<int64_t> &base : globalMaps) {
      for (const std::vector<int64_t> &local : windowMaps[windowIndex]) {
        if (local.size() != aEnd - aBegin)
          return result;
        std::vector<int64_t> candidate = base;
        for (size_t localA = 0; localA < local.size(); ++localA)
          candidate[static_cast<size_t>(aBegin) + localA] = local[localA];
        expanded.push_back(std::move(candidate));
      }
    }
    globalMaps = std::move(expanded);
    if (windowIndex < forcedAnchors.size())
      aBegin = forcedAnchors[windowIndex].aToken + 1;
  }

  llvm::sort(globalMaps,
             [](const std::vector<int64_t> &lhs,
                const std::vector<int64_t> &rhs) {
               return std::lexicographical_compare(
                   lhs.begin(), lhs.end(), rhs.begin(), rhs.end());
             });
  globalMaps.erase(std::unique(globalMaps.begin(), globalMaps.end()),
                   globalMaps.end());
  result.completeEnumeration = true;
  if (globalMaps.size() <= 1)
    return result;

  std::vector<AlignmentSemanticSimulationResult> simulations;
  simulations.reserve(globalMaps.size());
  for (const std::vector<int64_t> &candidateMap : globalMaps) {
    if (!isStrictlyMonotoneMap(candidateMap, deps_.bLexemes.size()) ||
        !mapLexemesAgree(candidateMap, deps_.aLexemes, deps_.bLexemes))
      return result;
    simulations.push_back(
        deps_.simulate(buildSimulationSelection(candidateMap,
                                                deps_.coreAlignment)));
  }

  auto commitRealizationClass =
      [&](StringRef equivalenceKey, ArrayRef<size_t> classMembers,
          ArrayRef<RequiredAnchor> requiredAnchors) -> ResolutionResult {
    if (classMembers.empty())
      return result;

    const size_t representativeIndex = classMembers.front();
    const uint64_t witnessId = 1;

    AlignmentSemanticResolutionWitness witness;
    witness.witnessId = witnessId;
    witness.enumeratedMapCount = globalMaps.size();
    witness.acceptedMapCount = classMembers.size();
    witness.rejectedMapCount =
        witness.enumeratedMapCount - witness.acceptedMapCount;
    witness.completeEnumeration = true;
    witness.equivalenceKey = equivalenceKey.str();
    witness.representativeMap = globalMaps[representativeIndex];

    std::map<std::pair<uint64_t, uint64_t>, AlignmentSemanticAnchorBasis>
        requiredBasis;
    for (const RequiredAnchor &anchor : requiredAnchors)
      requiredBasis[{anchor.aToken, anchor.bToken}] = anchor.basis;

    ResolutionResult committed = result;
    committed.selectedMap = witness.representativeMap;
    committed.selectedAnchorProofs.assign(committed.selectedMap.size(),
                                          diffutils::LcsAnchorProof{});
    for (size_t aToken = 0; aToken < committed.selectedMap.size(); ++aToken) {
      const int64_t bToken = committed.selectedMap[aToken];
      if (bToken < 0)
        continue;
      if (isForcedAnchor(deps_.coreAlignment, aToken, bToken)) {
        committed.selectedAnchorProofs[aToken] = diffutils::LcsAnchorProof{
            diffutils::LcsAnchorProofKind::CoreOptimalPathForced, 0};
        continue;
      }

      const auto required = requiredBasis.find(
          {aToken, static_cast<uint64_t>(bToken)});
      const AlignmentSemanticAnchorBasis basis =
          required == requiredBasis.end()
              ? AlignmentSemanticAnchorBasis::
                    EquivalentRealizationRepresentative
              : required->second;
      witness.anchorEvidence.push_back(AlignmentSemanticAnchorEvidence{
          aToken, static_cast<uint64_t>(bToken), basis});
      committed.selectedAnchorProofs[aToken] = diffutils::LcsAnchorProof{
          diffutils::LcsAnchorProofKind::EquivalentNormalizedHunkAndOwner,
          witnessId};
    }

    REFOLD_LOG_TRACE(
        "lcs/semantic-resolver",
        "committing realized-source class: candidates={0} survivors={1} "
        "classMembers={2} requiredAnchors={3} representative={4}",
        globalMaps.size(), classMembers.size(),
        formatMapIndices(classMembers), requiredAnchors.size(),
        representativeIndex);

    committed.witnesses.push_back(std::move(witness));
    committed.committedEquivalentClass = true;
    return committed;
  };

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

    std::map<std::string, std::vector<size_t>> leastRealizationClasses;
    for (size_t mapIndex : leastDestructiveMaps)
      leastRealizationClasses[simulations[mapIndex].realizationEquivalenceKey]
          .push_back(mapIndex);

    if (leastRealizationClasses.size() == 1) {
      const auto &onlyClass = *leastRealizationClasses.begin();
      REFOLD_LOG_TRACE(
          "lcs/semantic-resolver",
          "committing globally least source-mutation class: accepted={0} "
          "least={1} classMembers={2}",
          acceptedMaps.size(), leastDestructiveMaps.size(),
          formatMapIndices(onlyClass.second));
      return commitRealizationClass(onlyClass.first, onlyClass.second,
                                    ArrayRef<RequiredAnchor>());
    }
  }

  // Reconstruct the old boundary map strictly as a proposal. The old balance
  // and surface ranks cannot authorize an anchor; they only identify the
  // counterfactuals that the theorem below must discharge.
  if (deps_.aGapProvenance.size() != deps_.aLexemes.size() + 1 ||
      deps_.bGapProvenance.size() != deps_.bLexemes.size() + 1)
    return result;
  LegacyAlignmentDiagnosticResult proposal =
      reconstructLegacyBoundaryProposal(
          deps_.aLexemes, deps_.bLexemes, deps_.aGapProvenance,
          deps_.bGapProvenance, deps_.coreAlignment);
  if (!proposal.complete || !proposal.monotone || !proposal.lexemesAgree ||
      !proposal.jointlyCoreOptimal)
    return result;

  std::vector<RequiredAnchor> requiredAnchors;
  const AlignmentSemanticSimulationResult *proposalSimulation = nullptr;
  std::optional<AlignmentSemanticSimulationResult> ownedProposalSimulation;
  size_t proposalNonForcedCount = 0;
  for (size_t aToken = 0; aToken < proposal.selectedMap.size(); ++aToken) {
    const int64_t bToken = proposal.selectedMap[aToken];
    if (bToken >= 0 && !isForcedAnchor(deps_.coreAlignment, aToken, bToken))
      ++proposalNonForcedCount;
  }

  if (proposalNonForcedCount != 0) {
    if (proposalNonForcedCount > MaxProposalCounterfactuals)
      return result;
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
    if (!proposalSimulation->accepted)
      return result;

    for (size_t aToken = 0; aToken < proposal.selectedMap.size(); ++aToken) {
      const int64_t bToken = proposal.selectedMap[aToken];
      if (bToken < 0 ||
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
  if (survivingMaps.empty())
    return result;

  // Unknown witness dimensions are not semantic rejections. If a surviving
  // core-optimal map is proof-incomplete, uniqueness has not been established
  // and the resolver must retain only forced anchors.
  for (size_t mapIndex : survivingMaps) {
    if (simulations[mapIndex].disposition ==
        AlignmentSemanticSimulationDisposition::ProofIncomplete) {
      REFOLD_LOG_TRACE(
          "lcs/semantic-resolver",
          "forced-only alignment retained: surviving map {0} has incomplete "
          "proof ('{1}')",
          mapIndex, simulations[mapIndex].rejectionReason);
      return result;
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
        simulation.realizationEquivalenceKey.empty())
      return result;
    realizationClasses[simulation.realizationEquivalenceKey].push_back(
        mapIndex);
  }
  if (realizationClasses.size() != 1)
    return result;

  const auto &onlyClass = *realizationClasses.begin();
  return commitRealizationClass(onlyClass.first, onlyClass.second,
                                requiredAnchors);
}

} // namespace refold
} // namespace clang
