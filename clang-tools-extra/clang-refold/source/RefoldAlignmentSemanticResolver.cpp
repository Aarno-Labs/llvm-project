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

/// Largest set of candidate maps a single commit rule may realize in order to
/// decide itself.
///
/// Realizing one map costs a complete refold of the translation unit.  Two of
/// the three commit rules below name the set they must realize before they can
/// decide, and neither set admits a denial from a prefix of itself:
///
///   least source mutation: a least element must compare against every accepted
///     map, so a prefix that contains none proves nothing about the maps still
///     unrealized (see the enumeration loop below).  Its set is the complete
///     enumeration;
///   legacy boundary proposal: its uniqueness test spans every map that carries
///     the anchors its counterfactuals proved necessary, so a prefix that spans
///     one realization class proves nothing about the survivors still
///     unrealized.  Its set is the surviving maps.
///
/// This budget is the point past which a single rule may not spend them.  It is
/// a cost bound, not a proof: a rule that exceeds it declines, and a window
/// with no rule left retains the forced-only map, exactly as every other budget
/// here does.
///
/// The bound applies per rule rather than per window so that a window over it
/// still reaches the rules whose set is small.  A window whose enumeration fits
/// inside the budget is unaffected in every case: no rule's set is larger than
/// the enumeration, so none of them can decline on cost.
///
/// It is deliberately a *completeness* budget, and completeness under it is not
/// monotone in the value.  A window whose surviving set is larger than the
/// budget declines a rule it would have committed had it been allowed to spend
/// the realizations, and the anchors that rule would have contributed are lost
/// with it -- the window keeps only what core certification forced.  Raising
/// the value therefore resolves strictly more windows and costs strictly more
/// whole-translation-unit refolds, and lowering it does the reverse; neither
/// direction can change a committed answer, only whether one is reached.
/// `alignment_window_legacy_proposal_declines_on_realization_budget.c` pins
/// that: it is the input
/// `alignment_window_over_budget_still_reaches_the_legacy_proposal.c` commits,
/// run with the budget injected below set low enough to deny the rule, and it
/// asserts that what is lost is the resolution and never the output.
///
/// The value must leave room for the largest enumeration on which a rule is
/// known to commit, because declining costs resolution the window would
/// otherwise have kept.  The binding case is
/// `semantic_alignment_counter_argument_growth_two_windows.c`, whose window
/// enumerates 54 maps and commits the least-source-mutation rule on map 14.
constexpr size_t DefaultMaxRealizedMapsPerCommitRule = 64;

constexpr StringLiteral TestOnlyRealizationBudgetEnvironment =
    "CLANG_REFOLD_TEST_ONLY_SEMANTIC_REALIZATION_BUDGET";

/// Return the production realization budget unless a test injects one.
///
/// The hook is test-only and has no default effect.  It exists because the
/// budget is a completeness policy rather than a proof: what a decline costs is
/// resolution, and asserting that requires driving a rule past the budget on an
/// input small enough to read, rather than one large enough to exceed 64
/// realizations on its own.
size_t maxRealizedMapsPerCommitRule() {
  // Read once.  The enumeration loop asks for the budget per candidate map, and
  // a process's environment does not change under it, so re-reading would cost
  // a lookup per realization and could not answer differently.
  static const size_t budget = [] {
    const char *injected =
        std::getenv(TestOnlyRealizationBudgetEnvironment.data());
    if (injected == nullptr)
      return DefaultMaxRealizedMapsPerCommitRule;

    uint64_t parsed = 0;
    if (StringRef(injected).getAsInteger(10, parsed)) {
      REFOLD_LOG_FATAL("lcs/semantic-resolver",
                       "invalid test-only semantic realization budget '{0}'",
                       injected);
    }
    REFOLD_LOG_TRACE("lcs/semantic-resolver",
                     "using test-only semantic realization budget={0}", parsed);
    return static_cast<size_t>(parsed);
  }();
  return budget;
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
  size_t windowsWithOracle = 0;
  for (size_t windowIndex = 0;
       windowIndex < deps_.coreAlignment.certificationWindows.size();
       ++windowIndex) {
    // A window the core theorem already determined has one optimal map, so
    // resolution would recompute its quadratic pair facts only to return the
    // anchors it already has. Skipping it changes no outcome; every window that
    // can carry ambiguity is still resolved, and none is passed over for cost.
    if (!WindowCarriesAmbiguity(windowIndex)) {
      REFOLD_LOG_TRACE(
          "lcs/semantic-resolver",
          "window {0} needs no resolution: the core theorem forced every one "
          "of its A tokens, so exactly one optimal map crosses it",
          windowIndex);
      continue;
    }

    // Materialize this window's pair facts, resolve, then release them before
    // moving on. Holding every window's facts at once would reintroduce the
    // complete-grid payload that partitioning exists to avoid.
    if (deps_.retainWindowOracle)
      (void)deps_.retainWindowOracle(windowIndex);
    if (!deps_.coreAlignment.HasCompleteSemanticOracleForWindow(windowIndex)) {
      REFOLD_LOG_INFO(
          "lcs/semantic-resolver",
          "window {0} keeps core-forced anchors: no retained all-optimal "
          "pair facts",
          windowIndex);
      continue;
    }
    ++windowsWithOracle;
    WindowResolution resolution =
        ResolveCertificationWindow(windowIndex, baseMap);
    if (deps_.releaseWindowOracle)
      deps_.releaseWindowOracle(windowIndex);
    if (!resolution.committed) {
      // One verdict line per window, and it must say which of the three ways
      // this window ended.  Only the last of them is a decline; reporting the
      // other two as one would claim a rule refused something it was never
      // offered.  Which rule declined, and on what evidence, stays at trace
      // level: the rules each report themselves there, and repeating one of
      // them here would be a second, weaker account of the same fact.
      if (resolution.enumeratedMapCount == 0)
        REFOLD_LOG_INFO("lcs/semantic-resolver",
                        "window {0} keeps its core-forced anchors: its optimal "
                        "maps could not be enumerated within the proof budget",
                        windowIndex);
      else if (resolution.enumeratedMapCount == 1)
        REFOLD_LOG_INFO("lcs/semantic-resolver",
                        "window {0} needs no resolution: enumeration found "
                        "exactly one complete core-optimal map",
                        windowIndex);
      else
        REFOLD_LOG_INFO("lcs/semantic-resolver",
                        "window {0} keeps its core-forced anchors: no commit "
                        "rule proved one admissible realization among its {1} "
                        "enumerated map(s)",
                        windowIndex, resolution.enumeratedMapCount);
      continue;
    }
    REFOLD_LOG_INFO(
        "lcs/semantic-resolver",
        "window {0} committed one realized-source class: {1} of {2} "
        "enumerated map(s) share it, {3} anchor(s) proved",
        windowIndex, resolution.acceptedMapCount, resolution.enumeratedMapCount,
        static_cast<uint64_t>(resolution.anchorEvidence.size()));
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

bool RefoldAlignmentSemanticResolver::WindowCarriesAmbiguity(
    size_t windowIndex) const {
  if (windowIndex >= deps_.coreAlignment.certificationWindows.size())
    return false;
  const diffutils::LcsCertificationWindow &window =
      deps_.coreAlignment.certificationWindows[windowIndex];
  if (!window.IsCertified())
    return false;

  // An A token the core theorem left unmatched is the only way a competing
  // optimal map can differ inside this rectangle. Its absence is what makes the
  // window's map unique; one occurrence is enough to have to resolve.
  const uint64_t aEnd =
      std::min<uint64_t>(window.aEnd, deps_.coreAlignment.forcedMap.size());
  for (uint64_t aToken = window.aBegin; aToken < aEnd; ++aToken)
    if (deps_.coreAlignment.forcedMap[aToken] < 0)
      return true;

  // A window reaching past the recorded forced map is not a window this
  // routine can speak for; resolve it rather than assume it is determined.
  return aEnd != window.aEnd;
}

/// Name one simulation disposition for the log.
static StringRef describeSimulationDisposition(
    AlignmentSemanticSimulationDisposition disposition) {
  switch (disposition) {
  case AlignmentSemanticSimulationDisposition::Accepted:
    return "accepted";
  case AlignmentSemanticSimulationDisposition::TerminalFallback:
    return "requested terminal fallback";
  case AlignmentSemanticSimulationDisposition::ProofIncomplete:
    return "proof incomplete";
  }
  return "unknown";
}

const AlignmentSemanticSimulationResult &
RefoldAlignmentSemanticResolver::RealizeCandidateMap(
    size_t windowIndex, size_t mapIndex, size_t mapCount,
    ArrayRef<int64_t> candidateMap,
    std::optional<AlignmentSemanticSimulationResult> &slot) const {
  // A map already in its slot was realized by an earlier rule and is replayed,
  // not re-planned.  Reporting it again would double-count the run's most
  // expensive step.
  if (slot)
    return *slot;

  REFOLD_LOG_INFO("lcs/semantic-resolver",
                  "window {0}: realizing candidate map {1} of {2} as a "
                  "complete refold",
                  windowIndex, mapIndex + 1, mapCount);
  slot.emplace(deps_.simulate(
      buildSimulationSelection(candidateMap, deps_.coreAlignment)));
  REFOLD_LOG_INFO("lcs/semantic-resolver",
                  "window {0}: candidate map {1} of {2} realized: {3}{4}{5}",
                  windowIndex, mapIndex + 1, mapCount,
                  describeSimulationDisposition(slot->disposition),
                  slot->rejectionReason.empty() ? "" : " -- ",
                  stringutils::showWsWithClip(slot->rejectionReason, 160));
  return *slot;
}

RefoldAlignmentSemanticResolver::WindowResolution
RefoldAlignmentSemanticResolver::ResolveCertificationWindow(
    size_t windowIndex, ArrayRef<int64_t> baseMap) const {
  // Every path that declines this window returns `result` rather than a fresh
  // value.  A commit is built and returned by `commitRealizationClass()`, so
  // `result` carries nothing but the enumeration census recorded below -- and
  // returning it keeps that census attached to the decline, which is what the
  // caller's verdict log reads to tell an incomplete enumeration apart from
  // real ambiguity that no rule closed.
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

  // Record the enumeration on the result before any verdict, so that a window
  // that goes on to decline is still distinguishable from one whose
  // enumeration never completed.  Every return above this point leaves the
  // count at zero, which is exactly what "did not complete" means.
  result.enumeratedMapCount = globalMaps.size();
  if (globalMaps.size() <= 1) {
    REFOLD_LOG_TRACE(
        "lcs/semantic-resolver",
        "window {0} has no alignment ambiguity: exactly one complete "
        "core-optimal map",
        windowIndex);
    return result;
  }

  // Enumeration is where this window's cost becomes knowable, and it is the
  // last point before the run starts spending it.  Every map counted here is a
  // distinct optimal alignment of the same rectangle, and deciding between them
  // is what the realizations below pay for.
  REFOLD_LOG_INFO(
      "lcs/semantic-resolver",
      "window {0} carries alignment ambiguity over A=[{1},{2}) B=[{3},{4}): "
      "{5} distinct core-optimal map(s) enumerated",
      windowIndex, window.aBegin, window.aEnd, window.bBegin, window.bEnd,
      static_cast<uint64_t>(globalMaps.size()));

  // Reachability of the legacy-proposal theorem is the one commit rule that no
  // simulation can influence: its proposal is reconstructed from the lexemes,
  // the gap provenance, and the core alignment alone.  Deciding it before any
  // candidate is realized is what lets the loop below stop as soon as no rule
  // can still fire; the proposal itself is reused at the rule's own position
  // below, so reconstructing it here costs nothing and decides nothing early.
  const bool gapProvenanceCoversStreams =
      deps_.aGapProvenance.size() == deps_.aLexemes.size() + 1 &&
      deps_.bGapProvenance.size() == deps_.bLexemes.size() + 1;
  LegacyAlignmentDiagnosticResult proposal;
  if (gapProvenanceCoversStreams)
    proposal = reconstructLegacyBoundaryProposal(
        deps_.aLexemes, deps_.bLexemes, deps_.aGapProvenance,
        deps_.bGapProvenance, deps_.coreAlignment);
  const bool legacyProposalRuleReachable =
      gapProvenanceCoversStreams && proposal.complete && proposal.monotone &&
      proposal.lexemesAgree && proposal.jointlyCoreOptimal;

  // Realizing one candidate costs a complete refold of the translation unit, so
  // the loop below tracks which commit rules a prefix of the enumeration has
  // already denied, and stops realizing as soon as the rules that read the
  // whole ground set are gone.  It is not the end of the window: the legacy
  // boundary proposal reads named maps rather than the set, so it is reached
  // from a stopped enumeration and realizes what it names.
  //
  // Only denials that carry from the prefix to the completed rule may be
  // recorded here.  Two do:
  //
  //   observational irrelevance: needs every candidate accepted with one shared
  //     concrete-output class, so a second class or one unaccepted candidate
  //     ends it.  Either fact holds of every superset of the prefix that
  //     produced it, so the completed rule declines too;
  //   least source mutation: skipped outright by the completed rule when any
  //     simulation is proof-incomplete, so one proof-incomplete candidate in
  //     the prefix denies it for the whole ground set.
  //
  // The least-source-mutation rule admits no other prefix denial.  It commits
  // on a map that is no more destructive than *every* accepted map, a test
  // quantified over the whole ground set, so writing L(S) for the least set
  // over S the exact relation between a prefix P and the ground set G is
  //
  //     L(G) INTERSECT P  is a subset of  L(P),
  //
  // and an empty L(P) says only that no map *already realized* is a global
  // least element.  A map still unrealized may be one:
  // semantic_alignment_counter_argument_growth_two_windows.c empties its
  // running least set inside the first 13 of 54 enumerated maps and the
  // completed rule commits on map 14, the unique least element, preserving that
  // window's LEFT_COUNTED/RIGHT_COUNTED invocations.  A running least set is
  // therefore not tracked at all: it would only invite being read as a denial
  // it cannot support.
  //
  // What bounds this rule instead is cost.  Deciding it costs one realization
  // per enumerated map, so an enumeration over the realization budget
  // declines the rule without realizing the remainder.  Declining a rule is not
  // declining the window: a rule whose set is small is still reached below, and
  // a window with no rule left keeps its core-forced anchors, which is the same
  // fail-closed answer every other exhausted budget here gives.
  bool observationalRuleReachable = true;
  bool containmentRuleReachable = true;
  std::optional<std::string> soleConcreteOutputKey;

  // Structural validity is a fail-closed guard over the whole ground set -- one
  // invalid map abandons the window -- so it is decided before anything is
  // realized.  Leaving it inside the realization loop would make the guard
  // depend on how far that loop happens to walk, and a loop that stops early
  // would pass a window an exhaustive one rejects.
  for (const std::vector<int64_t> &candidateMap : globalMaps) {
    if (isStrictlyMonotoneMap(candidateMap, deps_.bLexemes.size()) &&
        mapLexemesAgree(candidateMap, deps_.aLexemes, deps_.bLexemes))
      continue;
    REFOLD_LOG_TRACE(
        "lcs/semantic-resolver",
        "window {0} keeps core-forced anchors: an enumerated map is not a "
        "monotone lexeme-agreeing alignment",
        windowIndex);
    return result;
  }

  // Realized candidate maps, indexed by position in `globalMaps`.  A map is
  // realized when a commit rule first reads it and never realized twice; see
  // `RealizeCandidateMap()` for why the order the rules ask in cannot change
  // any answer.
  std::vector<std::optional<AlignmentSemanticSimulationResult>> realizedMaps(
      globalMaps.size());

  // Whether every enumerated map was realized.  The two rules that quantify
  // over the complete ground set may be decided only when this holds; the loop
  // below stops early exactly when both of them have already been denied.
  bool everyMapRealized = true;

  for (size_t mapIndex = 0; mapIndex < globalMaps.size(); ++mapIndex) {
    const AlignmentSemanticSimulationResult &realized =
        RealizeCandidateMap(windowIndex, mapIndex, globalMaps.size(),
                            globalMaps[mapIndex], realizedMaps[mapIndex]);
    const bool realizedIsCompleteAccepted =
        realized.disposition ==
            AlignmentSemanticSimulationDisposition::Accepted &&
        realized.accepted;

    if (observationalRuleReachable) {
      if (!realizedIsCompleteAccepted ||
          realized.concreteOutputEquivalenceKey.empty()) {
        observationalRuleReachable = false;
      } else if (!soleConcreteOutputKey) {
        soleConcreteOutputKey = realized.concreteOutputEquivalenceKey;
      } else if (*soleConcreteOutputKey !=
                 realized.concreteOutputEquivalenceKey) {
        observationalRuleReachable = false;
      }
    }

    // The completed rule skips itself entirely when any simulation is
    // proof-incomplete, so one such candidate denies it for the ground set.
    if (realized.disposition ==
        AlignmentSemanticSimulationDisposition::ProofIncomplete)
      containmentRuleReachable = false;

    // Observational irrelevance still needs the rest of the ground set.
    if (observationalRuleReachable)
      continue;

    // The least-source-mutation rule needs it too, and no prefix can deny it,
    // so cost is what decides whether it may have it.  The test is on the
    // enumeration rather than on the prefix realized so far: the rule's set is
    // the whole ground set however few of it has been paid for, and declining
    // on the first candidate spends one realization instead of the budget.
    if (containmentRuleReachable &&
        globalMaps.size() > maxRealizedMapsPerCommitRule()) {
      REFOLD_LOG_TRACE(
          "lcs/semantic-resolver",
          "window {0} declines the least-source-mutation rule after realizing "
          "{1} map(s): its {2} enumerated map(s) exceed the containment "
          "realization budget ({3})",
          windowIndex, mapIndex + 1, globalMaps.size(),
          maxRealizedMapsPerCommitRule());
      containmentRuleReachable = false;
    }

    if (containmentRuleReachable)
      continue;

    // Neither rule that reads the whole ground set can still fire.
    if (!legacyProposalRuleReachable) {
      REFOLD_LOG_TRACE(
          "lcs/semantic-resolver",
          "window {0} keeps core-forced anchors: no commit rule remains "
          "reachable after realizing {1} of {2} enumerated map(s)",
          windowIndex, mapIndex + 1, globalMaps.size());
      return result;
    }

    // The legacy boundary proposal is the only rule left, and it reads the
    // proposal's own realization, its leave-one-out counterfactuals, and the
    // maps that survive the anchors those prove necessary -- never the ground
    // set as such.  Stop realizing here and let it ask for what it needs.
    everyMapRealized = false;
    break;
  }

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

  // Permanent census of the realized candidate set. Every commit rule below is
  // a statement about these counts, so reporting them makes a declined window
  // explain itself instead of failing silently. Reading finished simulation
  // records cannot affect candidate order or any proof decision.
  //
  // Reported at `info` alongside the realizations it accounts for: this window
  // has just paid one complete refold per enumerated map, and the counts here
  // are what say whether that bought a commit.  It is bounded by the same
  // realization budget the rules are, so it cannot outgrow the cost it
  // explains -- at most one census, and one verdict per rule, per window that
  // enumerated ambiguity at all.
  if (inInfoMode()) {
    size_t realizedCount = 0;
    size_t acceptedCount = 0;
    size_t terminalFallbackCount = 0;
    size_t proofIncompleteCount = 0;
    std::set<StringRef> concreteOutputKeys;
    std::set<StringRef> realizationKeys;
    for (const std::optional<AlignmentSemanticSimulationResult> &realized :
         realizedMaps) {
      if (!realized)
        continue;
      ++realizedCount;
      const AlignmentSemanticSimulationResult &simulation = *realized;
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
    REFOLD_LOG_INFO(
        "lcs/semantic-resolver",
        "window {0} candidate census: enumerated={1} realized={2} accepted={3} "
        "terminalFallback={4} proofIncomplete={5} distinctConcreteOutputs={6} "
        "distinctRealizations={7}",
        windowIndex, globalMaps.size(), realizedCount, acceptedCount,
        terminalFallbackCount, proofIncompleteCount, concreteOutputKeys.size(),
        realizationKeys.size());
  }

  // Both rules below are statements about the complete ground set, so both are
  // reached only when every enumerated map was realized.  The enumeration stops
  // short only after each has been denied -- observational irrelevance by a
  // second concrete-output class or an unaccepted candidate, least source
  // mutation by a proof-incomplete candidate or by its realization budget --
  // and a denied rule cannot revive, so skipping them here decides nothing that
  // realizing the remainder would have decided differently.
  if (everyMapRealized) {
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
    for (size_t mapIndex = 0; mapIndex < globalMaps.size(); ++mapIndex) {
      const AlignmentSemanticSimulationResult &simulation =
          *realizedMaps[mapIndex];
      if (simulation.disposition !=
              AlignmentSemanticSimulationDisposition::Accepted ||
          !simulation.accepted ||
          simulation.concreteOutputEquivalenceKey.empty()) {
        everyCompleteMapAccepted = false;
        break;
      }
      completeOutputClasses[simulation.concreteOutputEquivalenceKey].push_back(
          mapIndex);
    }
    if (everyCompleteMapAccepted && completeOutputClasses.size() == 1) {
      const auto &onlyClass = *completeOutputClasses.begin();
      return commitRealizationClass(onlyClass.first, onlyClass.second,
                                    ArrayRef<RequiredAnchor>());
    }
    REFOLD_LOG_INFO(
        "lcs/semantic-resolver",
        "window {0} is not observationally irrelevant: everyMapAccepted={1} "
        "distinctConcreteOutputClasses={2}",
        windowIndex, everyCompleteMapAccepted, completeOutputClasses.size());

    // A complete accepted simulation is an independent proof that its source
    // realization reproduces the requested B stream and preserves every tracked
    // preprocessing postcondition.  Carrier-specific witness spellings may
    // still separate two such simulations even when one changes a strict subset
    // of the other's original source carriers.  Select a map only when
    // exhaustive enumeration proves a global least element under exact source-
    // transformation containment. Incomparable minima, incomplete proofs, and
    // multiple realization classes remain ambiguous and retain the forced-only
    // map.
    bool hasProofIncompleteMap = false;
    std::vector<size_t> acceptedMaps;
    for (size_t mapIndex = 0; mapIndex < globalMaps.size(); ++mapIndex) {
      const AlignmentSemanticSimulationResult &simulation =
          *realizedMaps[mapIndex];
      if (simulation.disposition ==
          AlignmentSemanticSimulationDisposition::ProofIncomplete) {
        hasProofIncompleteMap = true;
        break;
      }
      if (simulation.disposition ==
              AlignmentSemanticSimulationDisposition::Accepted &&
          simulation.accepted && !simulation.realizationEquivalenceKey.empty())
        acceptedMaps.push_back(mapIndex);
    }

    if (!hasProofIncompleteMap && !acceptedMaps.empty()) {
      std::vector<size_t> leastDestructiveMaps;
      for (size_t candidateIndex : acceptedMaps) {
        const bool noMoreDestructiveThanEveryAccepted =
            llvm::all_of(acceptedMaps, [&](size_t otherIndex) {
              return noMoreSourceDestructive(*realizedMaps[candidateIndex],
                                             *realizedMaps[otherIndex]);
            });
        if (noMoreDestructiveThanEveryAccepted)
          leastDestructiveMaps.push_back(candidateIndex);
      }

      // Class the surviving minima by their concrete emitted artifact, not by
      // internal proof-carrier provenance. Every member of `acceptedMaps` has
      // already passed the complete end-to-end theorem audit independently,
      // which is exactly the precondition `concreteOutputEquivalenceKey`
      // documents for itself: at that point the authoritative question is
      // whether alignment ambiguity changes the emitted source and the
      // deterministic post-emission pruning inputs, not which carrier proved
      // the same bytes. Two minima that differ only in the structural-tiling
      // witness spelling emit identical source, so separating them here would
      // manufacture ambiguity the output does not have. The stricter
      // realization key still governs the counterfactual path below, where
      // members have not all been audited.
      std::map<std::string, std::vector<size_t>> leastRealizationClasses;
      for (size_t mapIndex : leastDestructiveMaps) {
        const AlignmentSemanticSimulationResult &simulation =
            *realizedMaps[mapIndex];
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
      REFOLD_LOG_INFO(
          "lcs/semantic-resolver",
          "window {0} has no unique least source-mutation class: accepted={1} "
          "leastDestructive={2} leastRealizationClasses={3}",
          windowIndex, acceptedMaps.size(), leastDestructiveMaps.size(),
          leastRealizationClasses.size());
    } else {
      REFOLD_LOG_INFO(
          "lcs/semantic-resolver",
          "window {0} skips source-mutation containment: proofIncomplete={1} "
          "accepted={2}",
          windowIndex, hasProofIncompleteMap, acceptedMaps.size());
    }
  }

  // The old boundary map is held strictly as a proposal. The old balance and
  // surface ranks cannot authorize an anchor; they only identify the
  // counterfactuals that the theorem below must discharge.  It was
  // reconstructed before the enumeration above, which needed to know whether
  // this rule could still fire; the reachability decision recorded there is
  // the one this rule reaches here.
  if (!gapProvenanceCoversStreams)
    return result;
  if (!legacyProposalRuleReachable) {
    REFOLD_LOG_INFO(
        "lcs/semantic-resolver",
        "window {0} keeps core-forced anchors: legacy boundary proposal is "
        "not a complete jointly core-optimal map",
        windowIndex);
    return result;
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
      return result;
    }
    // The historical proposal is commonly one of the enumerated complete maps.
    // Realize it through that map's slot, so a proposal the enumeration already
    // paid for is reused instead of cloning and refolding the entire
    // translation unit a second time.
    const auto proposalMap = std::lower_bound(
        globalMaps.begin(), globalMaps.end(), proposal.selectedMap);
    if (proposalMap != globalMaps.end() &&
        *proposalMap == proposal.selectedMap) {
      const size_t proposalIndex =
          static_cast<size_t>(proposalMap - globalMaps.begin());
      proposalSimulation = &RealizeCandidateMap(
          windowIndex, proposalIndex, globalMaps.size(),
          globalMaps[proposalIndex], realizedMaps[proposalIndex]);
    } else {
      // The proposal is not one of the enumerated maps, so it has no slot and
      // costs a refold of its own.  Report it like any other realization.
      REFOLD_LOG_INFO("lcs/semantic-resolver",
                      "window {0}: realizing the legacy boundary proposal as a "
                      "complete refold; it is not among the {1} enumerated map(s)",
                      windowIndex, globalMaps.size());
      ownedProposalSimulation.emplace(deps_.simulate(
          buildSimulationSelection(proposal.selectedMap,
                                   deps_.coreAlignment)));
      proposalSimulation = &*ownedProposalSimulation;
      REFOLD_LOG_INFO(
          "lcs/semantic-resolver",
          "window {0}: legacy boundary proposal realized: {1}", windowIndex,
          describeSimulationDisposition(ownedProposalSimulation->disposition));
    }
    if (!proposalSimulation->accepted) {
      REFOLD_LOG_TRACE(
          "lcs/semantic-resolver",
          "window {0} keeps core-forced anchors: the legacy proposal's own "
          "simulation was not accepted ('{1}')",
          windowIndex, proposalSimulation->rejectionReason);
      return result;
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
    return result;
  }

  // The uniqueness test below spans every surviving map, so this rule's own
  // realization set is that survivor set, and it is bounded exactly as the
  // least-source-mutation rule's ground set is.  A window whose enumeration
  // fits inside the budget can never reach this decline: the survivors are a
  // subset of the enumeration.
  if (survivingMaps.size() > maxRealizedMapsPerCommitRule()) {
    REFOLD_LOG_TRACE(
        "lcs/semantic-resolver",
        "window {0} keeps core-forced anchors: {1} map(s) surviving the {2} "
        "required anchor(s) exceed the realization budget ({3})",
        windowIndex, survivingMaps.size(), requiredAnchors.size(),
        maxRealizedMapsPerCommitRule());
    return result;
  }

  // Unknown witness dimensions are not semantic rejections. If a surviving
  // core-optimal map is proof-incomplete, uniqueness has not been established
  // and the resolver must retain only forced anchors.
  for (size_t mapIndex : survivingMaps) {
    const AlignmentSemanticSimulationResult &simulation =
        RealizeCandidateMap(windowIndex, mapIndex, globalMaps.size(),
                            globalMaps[mapIndex], realizedMaps[mapIndex]);
    if (simulation.disposition ==
        AlignmentSemanticSimulationDisposition::ProofIncomplete) {
      REFOLD_LOG_TRACE(
          "lcs/semantic-resolver",
          "window {0} keeps core-forced anchors: surviving map {1} has "
          "incomplete proof ('{2}')",
          windowIndex, mapIndex, simulation.rejectionReason);
      return result;
    }
  }

  std::map<std::string, std::vector<size_t>> realizationClasses;
  for (size_t mapIndex : survivingMaps) {
    const AlignmentSemanticSimulationResult &simulation =
        *realizedMaps[mapIndex];
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
      return result;
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
    return result;
  }

  const auto &onlyClass = *realizationClasses.begin();
  return commitRealizationClass(onlyClass.first, onlyClass.second,
                                requiredAnchors);
}

/// Return the window partition identity carried by \p alignment.
static std::vector<AlignmentSemanticResolutionMemo::WindowIdentity>
collectWindowIdentities(const diffutils::CertifiedLcsResult &alignment) {
  std::vector<AlignmentSemanticResolutionMemo::WindowIdentity> identities;
  identities.reserve(alignment.certificationWindows.size());
  for (const diffutils::LcsCertificationWindow &window :
       alignment.certificationWindows) {
    identities.push_back(AlignmentSemanticResolutionMemo::WindowIdentity{
        window.aBegin, window.aEnd, window.bBegin, window.bEnd,
        window.IsCertified()});
  }
  return identities;
}

bool AlignmentSemanticResolutionMemo::MatchesInputs(
    size_t aLexemes, size_t bLexemes,
    const diffutils::CertifiedLcsResult &alignment) const {
  if (!recorded || aLexemeCount != aLexemes || bLexemeCount != bLexemes ||
      forcedMap != alignment.forcedMap)
    return false;

  const std::vector<WindowIdentity> identities =
      collectWindowIdentities(alignment);
  if (identities.size() != certificationWindows.size())
    return false;
  for (size_t window = 0; window < identities.size(); ++window) {
    const WindowIdentity &recordedWindow = certificationWindows[window];
    const WindowIdentity &current = identities[window];
    if (recordedWindow.aBegin != current.aBegin ||
        recordedWindow.aEnd != current.aEnd ||
        recordedWindow.bBegin != current.bBegin ||
        recordedWindow.bEnd != current.bEnd ||
        recordedWindow.certified != current.certified)
      return false;
  }
  return true;
}

void AlignmentSemanticResolutionMemo::Record(
    size_t aLexemes, size_t bLexemes,
    const diffutils::CertifiedLcsResult &alignment,
    RefoldAlignmentSemanticResolver::ResolutionResult result) {
  aLexemeCount = aLexemes;
  bLexemeCount = bLexemes;
  forcedMap = alignment.forcedMap;
  certificationWindows = collectWindowIdentities(alignment);
  resolution = std::move(result);
  recorded = true;
}

} // namespace refold
} // namespace clang
