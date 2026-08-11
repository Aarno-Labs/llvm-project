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
#include "source/RefoldAlignmentDiagnostic.h"
#include "source/RefoldPreprocessingStructureIndex.h"
#include "source/RefoldPreprocessingStructureIndexProvider.h"
#include "source/RefoldSourceMapper.h"
#include "util/RefoldPathIdentity.h"
#include "util/StringUtils.h"

#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/StringRef.h"

#include <algorithm>
#include <cassert>
#include <cstdlib>
#include <cstddef>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <tuple>
#include <utility>

using namespace llvm;

namespace clang {
namespace refold {

namespace {

constexpr StringLiteral TestOnlyCertificationBudgetEnvironment =
    "CLANG_REFOLD_TEST_ONLY_LCS_CERTIFICATION_BYTE_BUDGET";
constexpr StringLiteral TestOnlyDiagnosticEvidenceBudgetEnvironment =
    "CLANG_REFOLD_TEST_ONLY_LCS_DIAGNOSTIC_EVIDENCE_BYTE_BUDGET";

/// Return one default byte budget unless a test explicitly injects a value.
///
/// The environment hook is intentionally test-only and has no default effect.
/// It lets llvm-lit exercise exact local threshold behavior without requesting
/// large real allocations or adding a user-facing policy option.
static uint64_t getTestOnlyLcsByteBudget(StringLiteral environment,
                                         uint64_t defaultBudget,
                                         StringRef budgetKind) {
  const char *injected = std::getenv(environment.data());
  if (injected == nullptr)
    return defaultBudget;

  uint64_t parsed = 0;
  if (StringRef(injected).getAsInteger(10, parsed)) {
    REFOLD_LOG_FATAL(
        "lcs/certification",
        "invalid test-only LCS {0} byte budget '{1}'", budgetKind, injected);
  }
  REFOLD_LOG_TRACE("lcs/certification", "using test-only LCS {0} byte "
                                         "budget={1}",
                   budgetKind, parsed);
  return parsed;
}

/// Return the production proof budget unless a test explicitly injects one.
static uint64_t getLcsCertificationByteBudget() {
  return getTestOnlyLcsByteBudget(TestOnlyCertificationBudgetEnvironment,
                                  diffutils::DEFAULT_MAX_BYTES,
                                  "certification");
}

/// Return the trace-only pair-evidence budget unless a test injects one.
static uint64_t getLcsDiagnosticEvidenceByteBudget() {
  return getTestOnlyLcsByteBudget(
      TestOnlyDiagnosticEvidenceBudgetEnvironment,
      diffutils::DEFAULT_MAX_DIAGNOSTIC_EVIDENCE_BYTES,
      "diagnostic evidence");
}

} // namespace

RefoldTokenDiffPlanner::RefoldTokenDiffPlanner(Dependencies deps)
    : deps_(deps) {}

RefoldTokenDiffPlanner::TokenDiffPlan RefoldTokenDiffPlanner::Plan() {
  std::vector<StringRef> aSeq = MapLexemes(deps_.aToks, deps_.aTokOff);
  std::vector<StringRef> bSeq = MapLexemes(deps_.bToks, deps_.bTokOff);

  deps_.ownerDepthGap = ComputeOwnerDepthGapsForPP();

  diffutils::CertifiedLcsResult alignment;
  if (deps_.alignmentOverride) {
    alignment.selectedMap = deps_.alignmentOverride->selectedMap;
    alignment.selectedAnchorProofs =
        deps_.alignmentOverride->selectedAnchorProofs;
    alignment.globalObjective = deps_.alignmentOverride->globalObjective;
    alignment.globalObjectiveIsExact =
        deps_.alignmentOverride->globalObjectiveIsExact;
    alignment.allWindowsCertified =
        deps_.alignmentOverride->allWindowsCertified;
    alignment.certifiedBoundaries =
        deps_.alignmentOverride->certifiedBoundaries;
    alignment.certificationWindows =
        deps_.alignmentOverride->certificationWindows;
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
    const uint64_t certificationByteBudget =
        getLcsCertificationByteBudget();
    uint64_t completeStreamRequiredBytes = 0;
    const bool completeStreamIsRepresentable =
        diffutils::getLcsCertificationRequiredBytes(
            aSeq.size(), bSeq.size(), /*retainCompleteOracle=*/true,
            completeStreamRequiredBytes);

    AlignmentProtectedBoundarySurfaces protectedBoundaries;
    std::vector<uint64_t> candidateABoundaries;
    diffutils::LcsCertificationDiagnosticEvidence diagnosticEvidence;
    diffutils::LcsCertificationDiagnosticEvidence *diagnosticEvidenceOut =
        inTraceMode() ? &diagnosticEvidence : nullptr;
    if (diagnosticEvidenceOut) {
      diagnosticEvidence.admissiblePairByteBudget =
          getLcsDiagnosticEvidenceByteBudget();
    }

    if (completeStreamIsRepresentable &&
        completeStreamRequiredBytes <= certificationByteBudget) {
      // A fitting complete-stream theorem is already exact and retains the
      // semantic oracle. Ordinary runs therefore avoid both partition-frontier
      // DP and protected-boundary collection. Trace runs still collect the
      // nominations needed by the permanent evidence transcript.
      if (diagnosticEvidenceOut) {
        protectedBoundaries = CollectProtectedAlignmentBoundarySurfaces(
            /*retainDiagnosticIdentities=*/true);
        initializeAlignmentDiagnosticEvidence(protectedBoundaries,
                                              diagnosticEvidence);
      }
      alignment = diffutils::certifiedLcsMapAB(
          aSeq, bSeq, gapProvenance, certificationByteBudget,
          diagnosticEvidenceOut);
      if (diagnosticEvidenceOut) {
        candidateABoundaries = diffutils::nominateLcsPartitionBoundaries(
            gapProvenance, /*forcedMap=*/{},
            protectedBoundaries.proofSchedulingCoordinates);
        diagnosticEvidence.candidateABoundaries.assign(
            candidateABoundaries.begin(), candidateABoundaries.end());
      }
    } else {
      protectedBoundaries = CollectProtectedAlignmentBoundarySurfaces(
          /*retainDiagnosticIdentities=*/diagnosticEvidenceOut != nullptr);
      if (diagnosticEvidenceOut)
        initializeAlignmentDiagnosticEvidence(protectedBoundaries,
                                              diagnosticEvidence);
      candidateABoundaries = diffutils::nominateLcsPartitionBoundaries(
          gapProvenance, /*forcedMap=*/{},
          protectedBoundaries.proofSchedulingCoordinates);
      if (!diffutils::certifyLcsWindowsWithinBudget(
              aSeq, bSeq, gapProvenance, candidateABoundaries,
              certificationByteBudget, alignment, diagnosticEvidenceOut)) {
        REFOLD_LOG_FATAL("lcs/map",
                         "window-local LCS certification failed");
      }
    }
    // Local certifiers publish immutable ambiguity records before releasing
    // their quadratic state. The planner only serializes those completed facts.
    if (inTraceMode()) {
      TraceProtectedAlignmentBoundaryIdentities(
          protectedBoundaries.diagnosticIdentities);
      TraceAlignmentAmbiguityWindows(
          aSeq, diagnosticEvidence.ambiguityWindows,
          protectedBoundaries.diagnosticIdentities);
    }
    if (deps_.semanticAlignmentResolver) {
      // The historical boundary policy is reconstructed only as a proposal.
      // Its B-gap surface ranks never grant authority: every proposed
      // non-forced anchor must subsequently pass the semantic resolver's
      // complete counterfactual and realization-equivalence theorems.
      std::vector<diffutils::LcsBGapProvenance> bGapProvenance =
          ComputeLcsBGapProvenanceForPP();
      deps_.semanticAlignmentResolver(aSeq, bSeq, gapProvenance,
                                      bGapProvenance, certificationByteBudget,
                                      alignment);
    }
    if (inTraceMode())
      TraceAlignmentCertificationRun(alignment, diagnosticEvidence,
                                     aSeq.size(), bSeq.size());
  }
  const std::vector<int64_t> &a2b = alignment.selectedMap;

  if (!alignment.globalObjectiveIsExact)
    REFOLD_LOG_FATAL("lcs/map", "global LCS objective is not exact");
  if (!alignment.CertificationPartitionIsWellFormed(aSeq.size(),
                                                     bSeq.size())) {
    REFOLD_LOG_FATAL("lcs/map",
                     "alignment certification windows do not form one "
                     "well-formed A/B partition");
  }
  if (a2b.size() != aSeq.size() ||
      alignment.forcedMap.size() != aSeq.size() ||
      alignment.selectedAnchorProofs.size() != aSeq.size()) {
    REFOLD_LOG_FATAL(
        "lcs/map",
        "alignment/proof surfaces have the wrong A-token cardinality");
  }

  int64_t lastForced = -1;
  for (size_t i = 0; i < alignment.forcedMap.size(); ++i) {
    const int64_t j = alignment.forcedMap[i];
    if (j < 0)
      continue;
    if (static_cast<size_t>(j) >= bSeq.size() || aSeq[i] != bSeq[j])
      REFOLD_LOG_FATAL("lcs/map",
                       "forced anchor does not identify equal A/B lexemes");
    if (j <= lastForced)
      REFOLD_LOG_FATAL("lcs/map",
                       "non-monotone forced map at A[{0}]={1} after {2}", i,
                       j, lastForced);
    if (!alignment.AnchorBelongsToCertifiedWindow(i,
                                                   static_cast<uint64_t>(j))) {
      REFOLD_LOG_FATAL(
          "lcs/map",
          "forced anchor lies outside every certified alignment window");
    }
    if (a2b[i] != j ||
        alignment.selectedAnchorProofs[i].kind !=
            diffutils::LcsAnchorProofKind::CoreOptimalPathForced ||
        alignment.selectedAnchorProofs[i].semanticWitnessId != 0) {
      REFOLD_LOG_FATAL(
          "lcs/map",
          "forced anchor is missing its exact selected-map theorem");
    }
    lastForced = j;
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
    if (!alignment.AnchorBelongsToCertifiedWindow(i,
                                                   static_cast<uint64_t>(j))) {
      REFOLD_LOG_FATAL(
          "lcs/map",
          "selected anchor lies outside every certified alignment window");
    }
    if (static_cast<size_t>(j) >= bSeq.size() || aSeq[i] != bSeq[j])
      REFOLD_LOG_FATAL("lcs/map",
                       "selected anchor does not identify equal A/B lexemes");
    if (j <= last) {
      REFOLD_LOG_FATAL("lcs/map", "non-monotone map at A[{0}]={1} after {2}", i,
                       j, last);
    }
    if (proof.kind == diffutils::LcsAnchorProofKind::CoreOptimalPathForced &&
        alignment.forcedMap[i] != j) {
      REFOLD_LOG_FATAL(
          "lcs/map",
          "core-forced anchor theorem does not match the exact forced map");
    }
    last = j;
  }
  deps_.abTokAnchorProofs = alignment.selectedAnchorProofs;

  std::vector<diffutils::Hunk> hunks = diffutils::hunksFromMap(
      a2b, alignment.certifiedBoundaries, aSeq.size(), bSeq.size());

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

  deps_.abTokHunks = hunks;

  // Raw byte hunks are built alongside the token diff so every later
  // A-byte -> B-byte projection observes caches derived from the same A/B
  // inputs as the token hunk plan.
  deps_.abByteHunks = deps_.sourceMapper.BuildByteHunksFromRawText();
  deps_.sourceMapper.BuildByteHunkPrefixDeltaCache();
  return TokenDiffPlan{std::move(hunks), std::move(alignment)};
}

namespace {

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

static std::string
formatAlignmentWindowAnchor(const AlignmentWindowAnchor &anchor) {
  switch (anchor.kind) {
  case AlignmentWindowAnchorKind::StreamBegin:
    return "<begin>";
  case AlignmentWindowAnchorKind::CertifiedBoundary:
    return formatv("boundary(A={0},B={1})", anchor.aToken, anchor.bToken)
        .str();
  case AlignmentWindowAnchorKind::ForcedToken:
    return formatv("A[{0}]->B[{1}]", anchor.aToken, anchor.bToken).str();
  case AlignmentWindowAnchorKind::StreamEnd:
    return "<end>";
  }
  llvm_unreachable("invalid alignment window anchor kind");
}

static bool sourceIntervalPrecedes(const RefoldModel::TokMapEntry &entry,
                                   uint64_t sourceBegin) {
  return entry.e <= sourceBegin;
}

static bool sourceIntervalFollows(const RefoldModel::TokMapEntry &entry,
                                  uint64_t sourceEnd) {
  return entry.b >= sourceEnd;
}

static StringRef alignmentBoundaryRoleName(AlignmentBoundaryRole role) {
  switch (role) {
  case AlignmentBoundaryRole::StructureBoundary:
    return "structure";
  case AlignmentBoundaryRole::IncludeCoverBegin:
    return "include-cover-begin";
  case AlignmentBoundaryRole::IncludeCoverEnd:
    return "include-cover-end";
  case AlignmentBoundaryRole::SourceOwnerUnavailable:
    return "source-owner-unavailable";
  }
  llvm_unreachable("invalid alignment boundary role");
}

static std::string formatOptionalIndex(std::optional<uint64_t> value,
                                       StringRef absent) {
  return value ? std::to_string(*value) : absent.str();
}

/// Collect source-owner boundary evidence without coupling it to proof policy.
///
/// Direct-TU projection deliberately preserves the historical path-only tokmap
/// surface used for production nominations. Header occurrences are trace-only:
/// each uses its exact include-owner tokmap slice and child-include covers, and
/// no projected header frontier is admitted to the production scheduling set.
class ProtectedAlignmentBoundaryCollector {
public:
  ProtectedAlignmentBoundaryCollector(
      const RefoldModel &model, const RefoldPathIdentity &pathIdentity,
      const RefoldPreprocessingStructureIndexProvider &structureIndexes,
      uint64_t aTokenCount, bool retainDiagnosticIdentities)
      : model_(model), pathIdentity_(pathIdentity),
        structureIndexes_(structureIndexes), aTokenCount_(aTokenCount),
        retainDiagnosticIdentities_(retainDiagnosticIdentities) {}

  AlignmentProtectedBoundarySurfaces Collect() {
    CollectFromIndex(structureIndexes_.GetTUIndex(),
                     /*admitToProofScheduling=*/true,
                     /*exactOwnerMappedTokens=*/nullptr,
                     /*exactOwnerChildIncludes=*/nullptr);
    if (retainDiagnosticIdentities_)
      CollectHeaderOccurrences();

    CanonicalizeIdentities();
    surfaces_.proofSchedulingCoordinates.assign(
        proofSchedulingCoordinates_.begin(),
        proofSchedulingCoordinates_.end());
    return std::move(surfaces_);
  }

private:
  using TokMapEntries =
      std::vector<const RefoldModel::TokMapEntry *>;
  using IncludeEntries =
      std::vector<const RefoldModel::IncludeItem *>;

  void AppendBoundary(const PreprocessingStructureInterval &interval,
                      AlignmentBoundaryRole role,
                      std::optional<uint64_t> aBoundary,
                      bool admitToProofScheduling) {
    if (admitToProofScheduling && aBoundary)
      proofSchedulingCoordinates_.insert(*aBoundary);
    if (!retainDiagnosticIdentities_)
      return;

    AlignmentProtectedBoundaryIdentity identity;
    identity.aBoundary = aBoundary;
    identity.projectionComplete = aBoundary.has_value();
    identity.sourcePath = interval.sourcePath;
    identity.sourceBegin = interval.begin;
    identity.sourceEnd = interval.end;
    identity.structureKind = interval.kind;
    identity.modelKind = interval.modelKind;
    identity.modelItemId = interval.modelItemId;
    identity.ownerIncludeId = interval.ownerIncludeId;
    identity.ownerConditionalArmId = interval.ownerConditionalArmId;
    identity.conditionalGroupId = interval.conditionalGroupId;
    identity.conditionalArmId = interval.conditionalArmId;
    identity.role = role;
    if (!aBoundary) {
      identity.incompleteEvidenceReason =
          "exact tokmap/include-cover evidence does not determine one "
          "A frontier";
    }
    surfaces_.diagnosticIdentities.push_back(std::move(identity));
  }

  void AppendUnavailableOwner(StringRef physicalSourcePath,
                              uint64_t ownerIncludeId, StringRef reason) {
    AlignmentProtectedBoundaryIdentity identity;
    identity.sourcePath = physicalSourcePath.str();
    identity.ownerIncludeId = ownerIncludeId;
    identity.role = AlignmentBoundaryRole::SourceOwnerUnavailable;
    identity.incompleteEvidenceReason = reason.str();
    surfaces_.diagnosticIdentities.push_back(std::move(identity));
  }

  TokMapEntries CollectMappedTokens(
      StringRef indexedPath, const TokMapEntries *exactOwnerMappedTokens) const {
    TokMapEntries mappedTokens;
    if (exactOwnerMappedTokens) {
      for (const RefoldModel::TokMapEntry *entry :
           *exactOwnerMappedTokens) {
        if (pathIdentity_.PathsEqual(entry->file, indexedPath))
          mappedTokens.push_back(entry);
      }
      return mappedTokens;
    }

    // Preserve the historical direct-TU path-only tokmap surface.
    for (const RefoldModel::TokMapEntry &entry : model_.GetTokmap()) {
      if (entry.pp < aTokenCount_ &&
          pathIdentity_.PathsEqual(entry.file, indexedPath)) {
        mappedTokens.push_back(&entry);
      }
    }
    return mappedTokens;
  }

  IncludeEntries CollectChildIncludes(
      StringRef indexedPath, std::optional<uint64_t> ownerIncludeId,
      const IncludeEntries *exactOwnerChildIncludes) const {
    IncludeEntries childIncludes;
    if (exactOwnerChildIncludes) {
      for (const RefoldModel::IncludeItem *include :
           *exactOwnerChildIncludes) {
        if (pathIdentity_.PathsEqual(include->sitePath, indexedPath))
          childIncludes.push_back(include);
      }
      return childIncludes;
    }

    // Preserve the historical direct-TU include-cover surface.
    for (const RefoldModel::IncludeItem &include : model_.GetIncludes()) {
      if (include.parent == ownerIncludeId && include.cover.IsValid() &&
          include.cover.end <= aTokenCount_ &&
          pathIdentity_.PathsEqual(include.sitePath, indexedPath)) {
        childIncludes.push_back(&include);
      }
    }
    return childIncludes;
  }

  bool AppendBoundIncludeSeams(
      const PreprocessingStructureInterval &interval,
      ArrayRef<const RefoldModel::IncludeItem *> childIncludes,
      bool admitToProofScheduling) {
    if (interval.modelKind !=
            PreprocessingStructureModelKind::IncludeDirective ||
        !interval.modelItemId)
      return false;

    for (const RefoldModel::IncludeItem *include : childIncludes) {
      if (include->id != *interval.modelItemId)
        continue;
      AppendBoundary(interval, AlignmentBoundaryRole::IncludeCoverBegin,
                     include->cover.begin, admitToProofScheduling);
      AppendBoundary(interval, AlignmentBoundaryRole::IncludeCoverEnd,
                     include->cover.end, admitToProofScheduling);
      return true;
    }
    return false;
  }

  std::optional<uint64_t> ProjectTokenlessInterval(
      const PreprocessingStructureInterval &interval,
      ArrayRef<const RefoldModel::TokMapEntry *> mappedTokens,
      ArrayRef<const RefoldModel::IncludeItem *> childIncludes) const {
    uint64_t lowerBoundary = 0;
    uint64_t upperBoundary = aTokenCount_;
    bool overlapsMappedToken = false;

    for (const RefoldModel::TokMapEntry *entry : mappedTokens) {
      if (sourceIntervalPrecedes(*entry, interval.begin)) {
        lowerBoundary = std::max(lowerBoundary, entry->pp + 1);
      } else if (sourceIntervalFollows(*entry, interval.end)) {
        upperBoundary = std::min(upperBoundary, entry->pp);
      } else {
        overlapsMappedToken = true;
      }
    }

    // Tokmap names the file that spelled a token, while an exact child include
    // cover fixes where a nested occurrence entered and left the owning source
    // stream. Both forms are producer-backed coordinates.
    for (const RefoldModel::IncludeItem *include : childIncludes) {
      if (include->siteE <= interval.begin) {
        lowerBoundary = std::max(lowerBoundary, include->cover.end);
      } else if (include->siteB >= interval.end) {
        upperBoundary = std::min(upperBoundary, include->cover.begin);
      }
    }

    if (overlapsMappedToken || lowerBoundary != upperBoundary)
      return std::nullopt;
    return lowerBoundary;
  }

  void CollectFromIndex(
      const RefoldPreprocessingStructureIndex &structureIndex,
      bool admitToProofScheduling,
      const TokMapEntries *exactOwnerMappedTokens,
      const IncludeEntries *exactOwnerChildIncludes) {
    const StringRef indexedPath = structureIndex.GetSourcePath();
    const std::optional<uint64_t> ownerIncludeId =
        structureIndex.GetOwnerIncludeId();
    const TokMapEntries mappedTokens =
        CollectMappedTokens(indexedPath, exactOwnerMappedTokens);
    const IncludeEntries childIncludes = CollectChildIncludes(
        indexedPath, ownerIncludeId, exactOwnerChildIncludes);

    for (const PreprocessingStructureInterval &interval :
         structureIndex.GetIntervals()) {
      assert(interval.ownerIncludeId == ownerIncludeId &&
             "occurrence index contains a foreign include owner");
      if (AppendBoundIncludeSeams(interval, childIncludes,
                                  admitToProofScheduling))
        continue;
      AppendBoundary(interval, AlignmentBoundaryRole::StructureBoundary,
                     ProjectTokenlessInterval(interval, mappedTokens,
                                              childIncludes),
                     admitToProofScheduling);
    }
  }

  void CollectHeaderOccurrences() {
    std::map<uint64_t, TokMapEntries> mappedTokensByIncludeOwner;
    std::map<std::optional<uint64_t>, IncludeEntries> childIncludesByOwner;
    for (const RefoldModel::IncludeItem &include : model_.GetIncludes()) {
      if (include.cover.IsValid() && include.cover.end <= aTokenCount_)
        childIncludesByOwner[include.parent].push_back(&include);
    }
    for (const RefoldModel::TokMapEntry &entry : model_.GetTokmap()) {
      if (entry.pp >= aTokenCount_)
        continue;
      const std::optional<uint64_t> ownerIncludeId =
          model_.InnermostIncludeAtPP(entry.pp);
      if (ownerIncludeId)
        mappedTokensByIncludeOwner[*ownerIncludeId].push_back(&entry);
    }

    const TokMapEntries emptyMappedTokens;
    const IncludeEntries emptyChildIncludes;
    for (const RefoldModel::IncludeItem &include : model_.GetIncludes()) {
      if (!include.cover.IsValid() || include.cover.end > aTokenCount_)
        continue;

      const std::optional<std::string> physicalSourcePath =
          pathIdentity_.ProducerPhysicalIncludePath(include);
      if (!physicalSourcePath) {
        AppendUnavailableOwner(
            /*physicalSourcePath=*/{}, include.id,
            formatv("include occurrence {0} has no producer physical path",
                    include.id)
                .str());
        continue;
      }

      const RefoldPreprocessingStructureIndexProvider::LookupResult lookup =
          structureIndexes_.Get(*physicalSourcePath, include.id);
      if (!lookup.HasIndex()) {
        AppendUnavailableOwner(lookup.physicalSourcePath, include.id,
                               lookup.incompleteEvidenceReason);
        continue;
      }

      assert(lookup.index->GetOwnerIncludeId() == include.id &&
             "provider returned the wrong occurrence-local index");
      const auto ownerTokens = mappedTokensByIncludeOwner.find(include.id);
      const auto ownerChildren = childIncludesByOwner.find(include.id);
      CollectFromIndex(
          *lookup.index, /*admitToProofScheduling=*/false,
          ownerTokens == mappedTokensByIncludeOwner.end()
              ? &emptyMappedTokens
              : &ownerTokens->second,
          ownerChildren == childIncludesByOwner.end()
              ? &emptyChildIncludes
              : &ownerChildren->second);
    }
  }

  void CanonicalizeIdentities() {
    std::sort(
        surfaces_.diagnosticIdentities.begin(),
        surfaces_.diagnosticIdentities.end(),
        [](const AlignmentProtectedBoundaryIdentity &lhs,
           const AlignmentProtectedBoundaryIdentity &rhs) {
          return std::tie(
                     lhs.sourcePath, lhs.sourceBegin, lhs.sourceEnd, lhs.role,
                     lhs.structureKind, lhs.modelKind, lhs.modelItemId,
                     lhs.ownerIncludeId, lhs.ownerConditionalArmId,
                     lhs.conditionalGroupId, lhs.conditionalArmId,
                     lhs.projectionComplete, lhs.aBoundary,
                     lhs.incompleteEvidenceReason) <
                 std::tie(
                     rhs.sourcePath, rhs.sourceBegin, rhs.sourceEnd, rhs.role,
                     rhs.structureKind, rhs.modelKind, rhs.modelItemId,
                     rhs.ownerIncludeId, rhs.ownerConditionalArmId,
                     rhs.conditionalGroupId, rhs.conditionalArmId,
                     rhs.projectionComplete, rhs.aBoundary,
                     rhs.incompleteEvidenceReason);
        });

    for (size_t index = 0; index < surfaces_.diagnosticIdentities.size();
         ++index) {
      surfaces_.diagnosticIdentities[index].identityId =
          static_cast<uint64_t>(index);
    }
  }

  const RefoldModel &model_;
  const RefoldPathIdentity &pathIdentity_;
  const RefoldPreprocessingStructureIndexProvider &structureIndexes_;
  uint64_t aTokenCount_ = 0;
  bool retainDiagnosticIdentities_ = false;
  AlignmentProtectedBoundarySurfaces surfaces_;
  std::set<uint64_t> proofSchedulingCoordinates_;
};

} // namespace

AlignmentProtectedBoundarySurfaces
RefoldTokenDiffPlanner::CollectProtectedAlignmentBoundarySurfaces(
    bool retainDiagnosticIdentities) const {
  return ProtectedAlignmentBoundaryCollector(
             deps_.model, deps_.pathIdentity,
             deps_.preprocessingStructureIndexes,
             static_cast<uint64_t>(deps_.aToks.size()),
             retainDiagnosticIdentities)
      .Collect();
}

void RefoldTokenDiffPlanner::TraceProtectedAlignmentBoundaryIdentities(
    ArrayRef<AlignmentProtectedBoundaryIdentity> identities) const {
  if (!inTraceMode())
    return;

  for (const AlignmentProtectedBoundaryIdentity &identity : identities) {
    REFOLD_LOG_TRACE(
        "lcs/boundary-identity",
        "identity={0} path='{1}' source=[{2},{3}) ownerIncludeId={4} "
        "role={5} structure={6} model={7} A={8} projectionComplete={9}",
        identity.identityId, identity.sourcePath, identity.sourceBegin,
        identity.sourceEnd,
        formatOptionalIndex(identity.ownerIncludeId, "<tu>"),
        alignmentBoundaryRoleName(identity.role),
        toString(identity.structureKind),
        toString(identity.modelKind),
        formatOptionalIndex(identity.aBoundary, "<unmapped>"),
        identity.projectionComplete ? "true" : "false");
    if (!identity.incompleteEvidenceReason.empty()) {
      REFOLD_LOG_TRACE("lcs/boundary-identity", "identity={0} reason={1}",
                       identity.identityId,
                       identity.incompleteEvidenceReason);
    }
  }
}

void RefoldTokenDiffPlanner::TraceAlignmentCertificationRun(
    const diffutils::CertifiedLcsResult &alignment,
    const diffutils::LcsCertificationDiagnosticEvidence &diagnosticEvidence,
    uint64_t aTokenCount, uint64_t bTokenCount) const {
  if (!inTraceMode())
    return;
  for (const std::string &line : diffutils::describeLcsCertificationRun(
           alignment, diagnosticEvidence, aTokenCount, bTokenCount)) {
    REFOLD_LOG_TRACE("lcs/certification", "{0}", line);
  }
}

void RefoldTokenDiffPlanner::TraceAlignmentAmbiguityWindows(
    ArrayRef<StringRef> aSeq,
    ArrayRef<AlignmentAmbiguityWindow> windows,
    ArrayRef<AlignmentProtectedBoundaryIdentity> identities) const {
  if (!inTraceMode())
    return;

  std::map<uint64_t, const AlignmentProtectedBoundaryIdentity *> identitiesById;
  for (const AlignmentProtectedBoundaryIdentity &identity : identities)
    identitiesById.emplace(identity.identityId, &identity);

  auto traceWindowHeader = [](const AlignmentAmbiguityWindow &window) {
    REFOLD_LOG_TRACE("lcs/ambiguity", "alignment ambiguity window:");
    REFOLD_LOG_TRACE("lcs/ambiguity", "A window=[{0},{1})", window.aBegin,
                     window.aEnd);
    REFOLD_LOG_TRACE("lcs/ambiguity", "B window=[{0},{1})", window.bBegin,
                     window.bEnd);
    REFOLD_LOG_TRACE(
        "lcs/ambiguity", "forced left anchor={0}",
        formatAlignmentWindowAnchor(window.leftForcedAnchor));
    REFOLD_LOG_TRACE(
        "lcs/ambiguity", "forced right anchor={0}",
        formatAlignmentWindowAnchor(window.rightForcedAnchor));
    REFOLD_LOG_TRACE(
        "lcs/ambiguity",
        "core objective matchedTokens={0} ownerDepthCost={1}",
        window.objective.matchedTokenCount, window.objective.ownerDepthCost);
  };

  for (const AlignmentAmbiguityWindow &window : windows) {
    traceWindowHeader(window);

    if (!window.admissiblePairEnumerationComplete) {
      REFOLD_LOG_TRACE("lcs/ambiguity",
                       "admissible repeated-token pairs=<unavailable>");
    } else {
      std::string repeatedPairsText;
      raw_string_ostream repeatedPairsStream(repeatedPairsText);
      repeatedPairsStream << '[';
      bool firstRepeatedPair = true;
      for (const AlignmentAdmissiblePair &pair : window.admissiblePairs) {
        if (!pair.repeatedInA && !pair.repeatedInB)
          continue;
        if (!firstRepeatedPair)
          repeatedPairsStream << ", ";
        firstRepeatedPair = false;
        repeatedPairsStream
            << "A[" << pair.aToken << "]->B[" << pair.bToken << "] '"
            << aSeq[static_cast<size_t>(pair.aToken)] << "'";
      }
      repeatedPairsStream << ']';
      repeatedPairsStream.flush();
      REFOLD_LOG_TRACE("lcs/ambiguity",
                       "admissible repeated-token pairs={0}",
                       repeatedPairsText);
    }

    // Several physical structures may share one numeric proof frontier. Keep
    // their identity records distinct while retaining the historical one-line
    // transcript per sorted A coordinate.
    std::map<uint64_t, AlignmentProtectedBoundaryProjection>
        projectionsByABoundary;
    size_t unmappedProtectedIntervals = 0;
    for (const AlignmentProtectedBoundaryProjection &projection :
         window.protectedBoundaryProjections) {
      if (!projection.aBoundary) {
        ++unmappedProtectedIntervals;
        continue;
      }
      auto inserted = projectionsByABoundary.emplace(*projection.aBoundary,
                                                     projection);
      if (!inserted.second &&
          (inserted.first->second.projectionComplete !=
               projection.projectionComplete ||
           inserted.first->second.admissibleBFrontiers !=
               projection.admissibleBFrontiers)) {
        // Conflicting evidence for one numeric frontier is never serialized as
        // complete. This diagnostic-only downgrade cannot affect proof state.
        inserted.first->second.projectionComplete = false;
        inserted.first->second.admissibleBFrontiers.clear();
      }
    }

    for (const auto &entry : projectionsByABoundary) {
      const AlignmentProtectedBoundaryProjection &projection = entry.second;
      if (!projection.projectionComplete) {
        REFOLD_LOG_TRACE(
            "lcs/ambiguity",
            "possible B frontiers protectedA={0} frontiers=<incomplete>",
            entry.first);
        continue;
      }
      REFOLD_LOG_TRACE(
          "lcs/ambiguity", "possible B frontiers protectedA={0} frontiers={1}",
          entry.first,
          formatTokenFrontiers(projection.admissibleBFrontiers));
    }
    REFOLD_LOG_TRACE("lcs/ambiguity", "unmapped protected intervals={0}",
                     unmappedProtectedIntervals);
    // Keep the historical core phrase stable while exposing both optional
    // evidence states independently on the same deterministic transcript line.
    REFOLD_LOG_TRACE(
        "lcs/ambiguity",
        "certification completeness={0} "
        "admissible pair enumeration completeness={1} "
        "boundary projection completeness={2}",
        window.coreCertificationComplete ? "true" : "false",
        window.admissiblePairEnumerationComplete ? "true" : "false",
        window.boundaryProjectionComplete ? "true" : "false");

    // Preserve the historical numeric transcript above, then append the
    // identity-bearing view. Existing FileCheck sequences remain stable while
    // header diagnostics gain exact path and occurrence ownership.
    for (const AlignmentProtectedBoundaryProjection &projection :
         window.protectedBoundaryProjections) {
      const auto identity = identitiesById.find(projection.boundaryIdentityId);
      if (identity == identitiesById.end())
        continue;
      const AlignmentProtectedBoundaryIdentity &physical = *identity->second;
      REFOLD_LOG_TRACE(
          "lcs/ambiguity",
          "protected identity={0} path='{1}' ownerIncludeId={2} role={3} "
          "A={4} frontiers={5} projectionComplete={6}",
          physical.identityId, physical.sourcePath,
          formatOptionalIndex(physical.ownerIncludeId, "<tu>"),
          alignmentBoundaryRoleName(physical.role),
          formatOptionalIndex(projection.aBoundary, "<unmapped>"),
          projection.projectionComplete
              ? formatTokenFrontiers(projection.admissibleBFrontiers)
              : std::string("<incomplete>"),
          projection.projectionComplete ? "true" : "false");
    }
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
  // Legacy shadow reconstruction consumes the profile to reproduce the last
  // known regression-passing frontier policy exactly. Production alignment
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
