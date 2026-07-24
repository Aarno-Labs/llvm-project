//===--- RefoldStructuralHunkDispatcher.cpp ---------------------*- C++ -*-===//
//
// Structural hunk dispatch staging for clang-refold.
//
//===----------------------------------------------------------------------===//

#include "source/RefoldStructuralHunkDispatcher.h"

#include "core/RefoldModel.h"
#include "line-control/RefoldLineObserverLayout.h"
#include "macro/RefoldMacroTopology.h"
#include "proof/RefoldProofLattice.h"
#include "source/DiffAlgorithms.h"
#include "source/RefoldToken.h"
#include "source/TokenTextHelpers.h"
#include "util/RefoldPathIdentity.h"
#include "util/StringUtils.h"
#include "clang/Basic/TokenKinds.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/FormatVariadic.h"

#include <algorithm>
#include <cassert>
#include <optional>
#include <utility>

using namespace llvm;

namespace clang {
namespace refold {

void RefoldStructuralHunkDispatcher::AddTUEdit(TextEdit edit) {
  tuEdits_.push_back(std::move(edit));
}

std::vector<TextEdit> &
RefoldStructuralHunkDispatcher::MutableTUEditsForRepairAndEmission() {
  return tuEdits_;
}

IncludeEdits &RefoldStructuralHunkDispatcher::EnsureIncludeEdits(
    const RefoldModel::IncludeItem *include) {
  assert(include && "structural hunk dispatcher requires an include item");
  return perInclude_.try_emplace(include->id, include).first->second;
}

void RefoldStructuralHunkDispatcher::AddIncludePatch(
    const RefoldModel::IncludeItem *include, IncludePatch patch) {
  EnsureIncludeEdits(include).Add(std::move(patch));
}

void RefoldStructuralHunkDispatcher::OrderIncludeInsertions() {
  for (auto &it : perInclude_) {
    IncludeEdits &ie = it.second;
    if (ie.patches.empty())
      continue;

    // Sort deterministically by owner-local A position, then by the B payload
    // position that produced the insertion text.
    llvm::sort(ie.patches,
               [](const IncludePatch &lhs, const IncludePatch &rhs) {
                 if (lhs.aStart != rhs.aStart)
                   return lhs.aStart < rhs.aStart;
                 return lhs.bStart < rhs.bStart;
               });
  }
}

bool RefoldStructuralHunkDispatcher::AppendLineObserverRealizationEdits(
    RefoldLineObserverLayout &layout, StringRef tuPath, StringRef tuBytes) {
  if (!layout.AppendTURealizationEdits(tuPath, tuBytes, tuEdits_))
    return false;
  return layout.AppendIncludeRealizationEdits(perInclude_);
}

bool RefoldStructuralHunkDispatcher::HasIncludePatchesFor(
    uint64_t includeId) const {
  auto it = perInclude_.find(includeId);
  return it != perInclude_.end() && !it->second.patches.empty();
}

RefoldStructuralHunkDispatcher::IncludeEditMap &
RefoldStructuralHunkDispatcher::MutableIncludeEditBucketsForMaterialization() {
  return perInclude_;
}

RefoldStructuralHunkDispatcher::MacroPatchByMacroIdMap &
RefoldStructuralHunkDispatcher::MacroPatchBucketForOwner(
    std::optional<uint64_t> ownerIncludeId) {
  return macroPatchByOwnerByMacroId_[ownerIncludeId];
}

RefoldStructuralHunkDispatcher::MacroPatchByMacroIdMap *
RefoldStructuralHunkDispatcher::FindMacroPatchBucketForOwner(
    std::optional<uint64_t> ownerIncludeId) {
  auto it = macroPatchByOwnerByMacroId_.find(ownerIncludeId);
  if (it == macroPatchByOwnerByMacroId_.end())
    return nullptr;
  return &it->second;
}

const RefoldStructuralHunkDispatcher::MacroPatchByMacroIdMap *
RefoldStructuralHunkDispatcher::FindMacroPatchBucketForOwner(
    std::optional<uint64_t> ownerIncludeId) const {
  auto it = macroPatchByOwnerByMacroId_.find(ownerIncludeId);
  if (it == macroPatchByOwnerByMacroId_.end())
    return nullptr;
  return &it->second;
}

RefoldStructuralHunkDispatcher::MacroPatchStagingSlot
RefoldStructuralHunkDispatcher::PrepareMacroPatchStagingSlot(
    const RefoldModel::MacroInvocation &macro) {
  assert(macro.invB && macro.invE &&
         "macro patch staging requires a physical invocation span");

  MacroPatchStagingSlot slot;
  slot.ownerIncludeId = macro.ownerIncludeId;
  slot.patchKey = FindMacroPatchKeyByInvocationSpan(macro.ownerIncludeId,
                                                    *macro.invB, *macro.invE)
                      .value_or(macro.id);
  slot.existingPatch = FindMacroPatchByKey(macro.ownerIncludeId, slot.patchKey);
  slot.existingIsCallsite =
      slot.existingPatch &&
      RefoldLineObserverLayout::InvocationSpanMatchesCallsitePrefix(
          StringRef(slot.existingPatch->replacement), macro);

  // When the existing patch is an expanded/non-callsite realization, keep the
  // original callsite text as the next planning base.  Later hunks can still
  // attempt an args-only or DAG reconstruction from the stable invocation
  // surface instead of compounding an already-expanded replacement.
  slot.currentInvocationText =
      (slot.existingPatch && slot.existingIsCallsite)
          ? slot.existingPatch->replacement
          : (macro.invText ? macro.invText->str() : "");
  return slot;
}

bool RefoldStructuralHunkDispatcher::HasMacroPatchForInvocation(
    const RefoldModel::MacroInvocation &macro) const {
  if (!macro.invB || !macro.invE)
    return false;
  return FindMacroPatchKeyByInvocationSpan(macro.ownerIncludeId, *macro.invB,
                                           *macro.invE)
      .has_value();
}

RefoldStructuralHunkDispatcher::MacroPatchByOwnerByMacroIdMap &
RefoldStructuralHunkDispatcher::MacroPatchMergeBucketsForPlanner() {
  return macroPatchByOwnerByMacroId_;
}

MacroPatch *RefoldStructuralHunkDispatcher::FindMacroPatchByKey(
    std::optional<uint64_t> ownerIncludeId, uint64_t macroPatchKey) {
  MacroPatchByMacroIdMap *bucket = FindMacroPatchBucketForOwner(ownerIncludeId);
  if (!bucket)
    return nullptr;
  auto it = bucket->find(macroPatchKey);
  if (it == bucket->end())
    return nullptr;
  return &it->second;
}

std::optional<uint64_t>
RefoldStructuralHunkDispatcher::FindMacroPatchKeyByInvocationSpan(
    std::optional<uint64_t> ownerIncludeId, uint64_t invStart,
    uint64_t invEnd) const {
  const MacroPatchByMacroIdMap *bucket =
      FindMacroPatchBucketForOwner(ownerIncludeId);
  if (!bucket)
    return std::nullopt;

  std::optional<uint64_t> existingKey;
  for (const auto &kv : *bucket) {
    const MacroPatch &patch = kv.second;
    if (patch.invRange.begin == invStart && patch.invRange.end == invEnd) {
      if (!existingKey || kv.first < *existingKey)
        existingKey = kv.first;
    }
  }
  return existingKey;
}

void RefoldStructuralHunkDispatcher::MergeMaterializedBTokenRangeFromSlot(
    MacroPatch &patch, const MacroPatchStagingSlot &slot,
    const diffutils::Hunk &hunk) const {
  if (slot.existingPatch && slot.existingPatch->materialized.hasBTokenRange) {
    if (!patch.materialized.hasBTokenRange) {
      patch.materialized.hasBTokenRange = true;
      patch.materialized.bTokStart = slot.existingPatch->materialized.bTokStart;
      patch.materialized.bTokEnd = slot.existingPatch->materialized.bTokEnd;
    } else {
      patch.materialized.bTokStart =
          std::min(patch.materialized.bTokStart,
                   slot.existingPatch->materialized.bTokStart);
      patch.materialized.bTokEnd = std::max(
          patch.materialized.bTokEnd, slot.existingPatch->materialized.bTokEnd);
    }
  }

  if (!patch.materialized.hasBTokenRange) {
    patch.materialized.hasBTokenRange = true;
    patch.materialized.bTokStart = hunk.bStart;
    patch.materialized.bTokEnd = hunk.bEnd;
    return;
  }

  patch.materialized.bTokStart = std::min(patch.materialized.bTokStart,
                                          static_cast<uint64_t>(hunk.bStart));
  patch.materialized.bTokEnd =
      std::max(patch.materialized.bTokEnd, static_cast<uint64_t>(hunk.bEnd));
}

void RefoldStructuralHunkDispatcher::StageMacroPatch(
    const MacroPatchStagingSlot &slot, MacroPatch patch) {
  StageMacroPatchUnderKey(slot.ownerIncludeId, slot.patchKey, std::move(patch));
}

void RefoldStructuralHunkDispatcher::StageMacroPatchUnderKey(
    std::optional<uint64_t> ownerIncludeId, uint64_t macroPatchKey,
    MacroPatch patch) {
  patch.macroId = macroPatchKey;
  MacroPatchBucketForOwner(ownerIncludeId)[macroPatchKey] = std::move(patch);
}

void RefoldStructuralHunkDispatcher::FinalizeMacroPatchBuckets(
    RefoldProofLattice &proofLattice) {
  macroPatchesByOwner_.clear();

  SmallVector<std::optional<uint64_t>, 16> ownerKeys;
  ownerKeys.reserve(macroPatchByOwnerByMacroId_.size());
  for (const auto &outerEntry : macroPatchByOwnerByMacroId_)
    ownerKeys.push_back(outerEntry.first);

  llvm::sort(ownerKeys, [](const std::optional<uint64_t> &a,
                           const std::optional<uint64_t> &b) {
    if (!a && b)
      return true;
    if (a && !b)
      return false;
    if (!a && !b)
      return false;
    return *a < *b;
  });

  for (const auto &owner : ownerKeys) {
    auto outerIt = macroPatchByOwnerByMacroId_.find(owner);
    if (outerIt == macroPatchByOwnerByMacroId_.end())
      continue;

    MacroPatchByMacroIdMap &patchesById = outerIt->second;
    std::vector<MacroPatch> &finalPatches = macroPatchesByOwner_[owner];

    SmallVector<uint64_t, 32> macroIds;
    macroIds.reserve(patchesById.size());
    for (const auto &kv : patchesById)
      macroIds.push_back(kv.first);
    llvm::sort(macroIds);

    for (uint64_t id : macroIds) {
      auto it = patchesById.find(id);
      if (it == patchesById.end())
        continue;

      MacroPatch &patch = it->second;
      if (!proofLattice.AcceptedCandidateBuilder()
               .FinalizeSelectedMacroPatchForEmission(
                   patch, "macro/final-emission-bucket"))
        continue;
      finalPatches.push_back(std::move(patch));
    }
  }
}

std::vector<MacroPatch> *
RefoldStructuralHunkDispatcher::FindFinalMacroPatchesForOwner(
    std::optional<uint64_t> ownerIncludeId) {
  auto it = macroPatchesByOwner_.find(ownerIncludeId);
  if (it == macroPatchesByOwner_.end())
    return nullptr;
  return &it->second;
}

const std::vector<MacroPatch> *
RefoldStructuralHunkDispatcher::FindFinalMacroPatchesForOwner(
    std::optional<uint64_t> ownerIncludeId) const {
  auto it = macroPatchesByOwner_.find(ownerIncludeId);
  if (it == macroPatchesByOwner_.end())
    return nullptr;
  return &it->second;
}

bool RefoldStructuralHunkDispatcher::HasFinalMacroPatchesForOwner(
    std::optional<uint64_t> ownerIncludeId) const {
  const std::vector<MacroPatch> *patches =
      FindFinalMacroPatchesForOwner(ownerIncludeId);
  return patches && !patches->empty();
}

bool RefoldStructuralHunkDispatcher::IncludeBucketsHavePatches() const {
  for (const auto &entry : perInclude_)
    if (!entry.second.patches.empty())
      return true;
  return false;
}

bool RefoldStructuralHunkDispatcher::MacroBucketsHavePatches() const {
  for (const auto &entry : macroPatchesByOwner_)
    if (!entry.second.empty())
      return true;
  return false;
}

size_t RefoldStructuralHunkDispatcher::CountTUMacroPatches() const {
  const std::vector<MacroPatch> *patches =
      FindFinalMacroPatchesForOwner(std::nullopt);
  return patches ? patches->size() : 0;
}

size_t RefoldStructuralHunkDispatcher::CountIncludePatches() const {
  size_t count = 0;
  for (const auto &entry : perInclude_)
    count += entry.second.patches.size();
  return count;
}

size_t RefoldStructuralHunkDispatcher::IncludeBucketCount() const {
  return perInclude_.size();
}

size_t RefoldStructuralHunkDispatcher::MacroOwnerBucketCount() const {
  return macroPatchesByOwner_.size();
}

void RefoldStructuralHunkDispatcher::AddDirectIncludeEditSeeds(
    DenseSet<uint64_t> &seeds) const {
  for (const auto &entry : perInclude_)
    seeds.insert(entry.first);
}

void RefoldStructuralHunkDispatcher::AddHeaderMacroPatchSeeds(
    DenseSet<uint64_t> &seeds) const {
  for (const auto &entry : macroPatchesByOwner_) {
    if (entry.first)
      seeds.insert(*entry.first);
  }
}

SmallVector<std::pair<uint64_t, uint64_t>, 8>
RefoldStructuralHunkDispatcher::BuildTUClosureSourceIntervals() const {
  SmallVector<std::pair<uint64_t, uint64_t>, 8> intervals;
  for (const TextEdit &edit : tuEdits_)
    intervals.push_back({edit.start, edit.end});
  if (const MacroPatchByMacroIdMap *bucket =
          FindMacroPatchBucketForOwner(std::nullopt)) {
    for (const auto &kv : *bucket)
      intervals.push_back({kv.second.invRange.begin, kv.second.invRange.end});
  }
  return intervals;
}

RefoldStructuralHunkDispatcher::MacroPatchByOwnerMap &
RefoldStructuralHunkDispatcher::FinalMacroPatchesByOwnerForMaterialization() {
  return macroPatchesByOwner_;
}

DenseSet<uint64_t> &
RefoldStructuralHunkDispatcher::MutableAppliedExpandedMacroRootIds() {
  return appliedExpandedMacroRootIds_;
}

void RefoldStructuralHunkDispatcher::
    RecordExpandedMacroRootsInMaterializedIncludes(
        const RefoldModel &model, const RefoldMacroTopology &macroTopology,
        const DenseSet<uint64_t> &expandedIncludeIds) {
  for (const auto &mi : model.GetMacroInvocations()) {
    if (mi.ownerIncludeId &&
        expandedIncludeIds.find(*mi.ownerIncludeId) != expandedIncludeIds.end())
      appliedExpandedMacroRootIds_.insert(macroTopology.GetRootMacroId(mi.id));
  }
}

size_t RefoldStructuralHunkDispatcher::ExpandedMacroRootCount() const {
  return appliedExpandedMacroRootIds_.size();
}

const DenseSet<uint64_t> &
RefoldStructuralHunkDispatcher::ExpandedMacroRootIds() const {
  return appliedExpandedMacroRootIds_;
}

namespace {
/// Return true iff \p kind is separator punctuation that may legitimately
/// replace a horizontal source gap between two tokens.
///
/// This is intentionally narrower than "left-attachable punctuation": closing
/// delimiters and operators can carry context-sensitive spacing conventions, so
/// they are not treated as gap replacements here. Callers must still prove with
/// `refoldNeedsLexicalSeparator()` that attaching the punctuation to the
/// token on its left preserves lexical tokenization.
bool isSeparatorGapReplacementPunctuation(tok::TokenKind kind) {
  switch (kind) {
  case tok::comma:
  case tok::semi:
  case tok::colon:
    return true;
  default:
    return false;
  }
}
} // namespace

bool maybeConsumeOrdinarySeparatorGapForPunctuation(
    const RefoldModel &model, const RefoldPathIdentity &pathIdentity,
    StringRef tuPath, StringRef tuBytes, std::pair<uint64_t, uint64_t> &span,
    StringRef replacement, const clang::LangOptions &lexLang) {
  if (span.first != span.second || replacement.empty() ||
      stringutils::isWs(replacement.front()) || span.first == 0 ||
      span.first >= tuBytes.size() ||
      (tuBytes[span.first - 1] != ' ' && tuBytes[span.first - 1] != '\t') ||
      stringutils::isWs(tuBytes[span.first]))
    return false;

  auto intervalOverlapsSpelledArtifact = [&](uint64_t begin,
                                             uint64_t end) -> bool {
    for (const auto &inc : model.GetIncludes()) {
      if (pathIdentity.PathsEqual(inc.sitePath, tuPath) && inc.siteB < end &&
          begin < inc.siteE)
        return true;
    }
    for (const auto &m : model.GetMacroInvocations()) {
      if (m.invFile && !m.invFile->empty() &&
          !pathIdentity.PathsEqual(*m.invFile, tuPath))
        continue;
      if (m.invB && m.invE && *m.invB < end && begin < *m.invE)
        return true;
    }
    return false;
  };

  uint64_t gapBegin = span.first;
  while (gapBegin > 0 &&
         (tuBytes[gapBegin - 1] == ' ' || tuBytes[gapBegin - 1] == '\t'))
    --gapBegin;

  if (gapBegin >= span.first ||
      intervalOverlapsSpelledArtifact(gapBegin, span.first))
    return false;

  std::optional<RefoldLexBoundaryToken> leftTok =
      refoldLastLexToken(tuBytes.take_front(gapBegin), lexLang);
  std::optional<RefoldLexBoundaryToken> rightTok =
      refoldFirstLexToken(tuBytes.drop_front(span.first), lexLang);
  std::optional<RefoldLexBoundaryToken> replFirstTok =
      refoldFirstLexToken(replacement, lexLang);
  std::optional<RefoldLexBoundaryToken> replLastTok =
      refoldLastLexToken(replacement, lexLang);

  if (!leftTok || !rightTok || !replFirstTok || !replLastTok ||
      leftTok->end != gapBegin || rightTok->begin != 0 ||
      !isSeparatorGapReplacementPunctuation(replFirstTok->kind) ||
      refoldNeedsLexicalSeparator(*leftTok, *replFirstTok, lexLang) ||
      refoldNeedsLexicalSeparator(*replLastTok, *rightTok, lexLang))
    return false;

  span.first = gapBegin;
  return true;
}

AlignmentSemanticTopologyKeyResult
RefoldStructuralHunkDispatcher::BuildAlignmentSemanticTopologyKey(
    const RefoldWitnessEquivalenceKeyBuilder &equivalenceKeyBuilder,
    StringRef tuSourceBytes) const {
  AlignmentSemanticTopologyKeyResult result;
  std::string &key = result.key;
  AlignmentSemanticPreservationFootprint &preservation =
      result.preservationFootprint;
  std::vector<std::string> semanticPostconditions;
  std::vector<std::string> realizationPostconditions;
  auto appendU64 = [&](uint64_t value) {
    key.append(std::to_string(value));
    key.push_back(';');
  };
  auto appendBool = [&](bool value) { appendU64(value ? 1 : 0); };
  auto appendString = [&](StringRef value) {
    appendU64(value.size());
    key.append(value.data(), value.size());
    key.push_back(';');
  };
  auto appendOptionalU64 = [&](const std::optional<uint64_t> &value) {
    appendBool(value.has_value());
    if (value)
      appendU64(*value);
  };
  auto appendProofSummary = [&](const ProofSummary &proof) {
    appendU64(static_cast<uint64_t>(proof.theoremClass));
    appendU64(static_cast<uint64_t>(proof.acceptedClass));
    appendU64(static_cast<uint64_t>(proof.realizationMode));
    appendU64(static_cast<uint64_t>(proof.surfaceDisposition));
    appendU64(static_cast<uint64_t>(proof.discharge.status));
    appendU64(static_cast<uint64_t>(proof.discharge.failureReason));
    appendU64(static_cast<uint64_t>(proof.discharge.failedObligation));
    appendU64(proof.discharge.obligationsEvaluated);
    appendU64(proof.discharge.obligationsSatisfied);
    appendBool(proof.structurePreserving);
    appendU64(proof.proofRootMacroId);
    appendBool(proof.hasOwnerRealizationWitness);
    appendBool(proof.hasMixedOwnerTilingWitness);
    if (proof.hasMixedOwnerTilingWitness) {
      appendString(proof.mixedOwnerTilingWitness.globalTargetPPTokenSignature);
      appendString(proof.mixedOwnerTilingWitness.globalCompositionSignature);
    }
  };
  auto appendAcceptedEquivalenceKey =
      [&](const AcceptedResultCandidate &accepted, StringRef carrierRole) {
        const WitnessEquivalenceKey witnessKey =
            equivalenceKeyBuilder.Build(accepted);
        appendString(witnessKey.PartitionString(/*witnessId=*/0));
        appendBool(!witnessKey.HasUnknownDimensions());

        const bool semanticComplete =
            witnessKey.suffixState.known &&
            witnessKey.preservedObservers.known &&
            witnessKey.counterState.known &&
            witnessKey.compositionClass != WitnessCompositionClass::Unknown;
        const bool realizationComplete =
            semanticComplete && witnessKey.producerKinds.known &&
            witnessKey.boundaryClass != WitnessBoundaryClass::Unknown &&
            witnessKey.diagnosticClass != WitnessDiagnosticClass::Unknown;
        semanticPostconditions.push_back(
            llvm::formatv(
                "suffix={0}|observers={1}|counter={2}|composition={3}",
                witnessKey.suffixState.PartitionValue("suffix_state", 0),
                witnessKey.preservedObservers.PartitionValue("observers", 0),
                witnessKey.counterState.PartitionValue("counter", 0),
                WitnessEquivalenceKey::PartitionEnumValue(
                    "composition", toString(witnessKey.compositionClass),
                    witnessKey.compositionClass !=
                        WitnessCompositionClass::Unknown,
                    0))
                .str());
        realizationPostconditions.push_back(
            llvm::formatv(
                "suffix={0}|observers={1}|counter={2}|producers={3}|"
                "boundary={4}|diagnostics={5}|composition={6}",
                witnessKey.suffixState.PartitionValue("suffix_state", 0),
                witnessKey.preservedObservers.PartitionValue("observers", 0),
                witnessKey.counterState.PartitionValue("counter", 0),
                witnessKey.producerKinds.PartitionValue(0),
                WitnessEquivalenceKey::PartitionEnumValue(
                    "boundary", toString(witnessKey.boundaryClass),
                    witnessKey.boundaryClass != WitnessBoundaryClass::Unknown,
                    0),
                WitnessEquivalenceKey::PartitionEnumValue(
                    "diagnostics", toString(witnessKey.diagnosticClass),
                    witnessKey.diagnosticClass !=
                        WitnessDiagnosticClass::Unknown,
                    0),
                WitnessEquivalenceKey::PartitionEnumValue(
                    "composition", toString(witnessKey.compositionClass),
                    witnessKey.compositionClass !=
                        WitnessCompositionClass::Unknown,
                    0))
                .str());
        preservation.semanticPostconditionComplete &= semanticComplete;
        preservation.realizationPostconditionComplete &= realizationComplete;
        if ((!semanticComplete || !realizationComplete) &&
            preservation.incompletePostconditionDimensions.empty())
          preservation.incompletePostconditionDimensions =
              witnessKey.UnknownDimensionSummary();

        if (!witnessKey.HasUnknownDimensions() || !result.failure.empty())
          return;
        result.complete = false;
        result.failure =
            llvm::formatv("{0} has incomplete witness-equivalence dimensions: "
                          "{1}",
                          carrierRole, witnessKey.UnknownDimensionSummary())
                .str();
      };

  std::vector<const TextEdit *> tuEdits;
  tuEdits.reserve(tuEdits_.size());
  for (const TextEdit &edit : tuEdits_)
    tuEdits.push_back(&edit);
  llvm::sort(tuEdits, [](const TextEdit *lhs, const TextEdit *rhs) {
    if (lhs->start != rhs->start)
      return lhs->start < rhs->start;
    if (lhs->end != rhs->end)
      return lhs->end < rhs->end;
    return lhs->text < rhs->text;
  });
  appendString("tu-edits");
  appendU64(tuEdits.size());
  for (const TextEdit *edit : tuEdits) {
    AlignmentSemanticMutationRecord tuMutation{
        AlignmentSemanticMutationDomain::TUBytes, std::nullopt, 0,
        edit->start, edit->end, edit->text.size(),
        !edit->protectedSourceAuthorizations.empty(),
        /*originalText=*/{}, /*replacementText=*/{},
        /*hasExactByteTransformation=*/false};
    tuMutation.replacementText = edit->text;
    if (edit->start <= edit->end && edit->end <= tuSourceBytes.size()) {
      tuMutation.originalText =
          tuSourceBytes.slice(edit->start, edit->end).str();
      tuMutation.hasExactByteTransformation = true;
    }
    preservation.mutations.push_back(std::move(tuMutation));
    for (const ProtectedSourceEditAuthorization &authorization :
         edit->protectedSourceAuthorizations) {
      preservation.mutations.push_back(AlignmentSemanticMutationRecord{
          AlignmentSemanticMutationDomain::ProtectedTUBytes, std::nullopt,
          static_cast<uint64_t>(authorization.authority),
          authorization.begin, authorization.end, 0,
          /*protectedStructure=*/true,
          /*originalText=*/{}, /*replacementText=*/{},
          /*hasExactByteTransformation=*/false});
    }
    appendU64(edit->start);
    appendU64(edit->end);
    appendString(edit->text);
    appendBool(edit->isDirectTUHunkEdit);
    appendOptionalU64(edit->directTUHunkIndex);
    appendOptionalU64(edit->directTUHunkAStart);
    appendOptionalU64(edit->directTUHunkAEnd);
    appendOptionalU64(edit->directTUHunkBStart);
    appendOptionalU64(edit->directTUHunkBEnd);
    appendOptionalU64(edit->directTURawStart);
    appendOptionalU64(edit->directTURawEnd);
    appendOptionalU64(edit->directTUFinalStart);
    appendOptionalU64(edit->directTUFinalEnd);
    appendOptionalU64(edit->materializedBByteBegin);
    appendOptionalU64(edit->materializedBByteEnd);
    appendOptionalU64(edit->materializedOutputTextBegin);
    appendOptionalU64(edit->materializedOutputTextEnd);
    appendU64(edit->protectedSourceAuthorizations.size());
    for (const ProtectedSourceEditAuthorization &authorization :
         edit->protectedSourceAuthorizations) {
      appendU64(static_cast<uint64_t>(authorization.authority));
      appendU64(static_cast<uint64_t>(authorization.structureKind));
      appendU64(static_cast<uint64_t>(authorization.modelKind));
      appendOptionalU64(authorization.modelItemId);
      appendOptionalU64(authorization.ownerConditionalArmId);
      appendOptionalU64(authorization.conditionalGroupId);
      appendOptionalU64(authorization.conditionalArmId);
      appendU64(authorization.begin);
      appendU64(authorization.end);
    }
    appendU64(edit->acceptedResults.size());
    for (const auto &accepted : edit->acceptedResults) {
      appendBool(static_cast<bool>(accepted));
      if (!accepted)
        continue;
      appendU64(static_cast<uint64_t>(accepted->kind));
      appendU64(accepted->begin);
      appendU64(accepted->end);
      appendBool(accepted->hasOwnerIncludeId);
      appendU64(accepted->ownerIncludeId);
      appendBool(accepted->hasRootMacroId);
      appendU64(accepted->rootMacroId);
      appendBool(accepted->hasTargetBTokenRange);
      appendU64(accepted->targetBTokStart);
      appendU64(accepted->targetBTokEnd);
      appendProofSummary(accepted->proofSummary);
      appendAcceptedEquivalenceKey(*accepted, "TU edit accepted carrier");
    }
  }

  SmallVector<uint64_t, 16> includeIds;
  includeIds.reserve(perInclude_.size());
  for (const auto &entry : perInclude_)
    includeIds.push_back(entry.first);
  llvm::sort(includeIds);
  appendString("include-patches");
  appendU64(includeIds.size());
  for (uint64_t includeId : includeIds) {
    const auto it = perInclude_.find(includeId);
    assert(it != perInclude_.end());
    appendU64(includeId);
    std::vector<const IncludePatch *> patches;
    patches.reserve(it->second.patches.size());
    for (const IncludePatch &patch : it->second.patches)
      patches.push_back(&patch);
    llvm::sort(patches,
               [](const IncludePatch *lhs, const IncludePatch *rhs) {
                 if (lhs->aStart != rhs->aStart)
                   return lhs->aStart < rhs->aStart;
                 if (lhs->aEnd != rhs->aEnd)
                   return lhs->aEnd < rhs->aEnd;
                 if (lhs->bStart != rhs->bStart)
                   return lhs->bStart < rhs->bStart;
                 if (lhs->bEnd != rhs->bEnd)
                   return lhs->bEnd < rhs->bEnd;
                 if (lhs->hasDirectHeaderByteRange !=
                     rhs->hasDirectHeaderByteRange)
                   return lhs->hasDirectHeaderByteRange <
                          rhs->hasDirectHeaderByteRange;
                 if (lhs->directHeaderByteBegin !=
                     rhs->directHeaderByteBegin)
                   return lhs->directHeaderByteBegin <
                          rhs->directHeaderByteBegin;
                 if (lhs->directHeaderByteEnd != rhs->directHeaderByteEnd)
                   return lhs->directHeaderByteEnd < rhs->directHeaderByteEnd;
                 return lhs->insertBytes < rhs->insertBytes;
               });
    appendU64(patches.size());
    for (const IncludePatch *patch : patches) {
      AlignmentSemanticMutationRecord includeMutation{
          patch->hasDirectHeaderByteRange
              ? AlignmentSemanticMutationDomain::IncludeSourceBytes
              : AlignmentSemanticMutationDomain::IncludeTokens,
          includeId, includeId,
          patch->hasDirectHeaderByteRange ? patch->directHeaderByteBegin
                                          : patch->aStart,
          patch->hasDirectHeaderByteRange ? patch->directHeaderByteEnd
                                          : patch->aEnd,
          patch->insertBytes.size(),
          patch->hasDirectHeaderByteRange &&
              patch->directHeaderByteAuthority !=
                  DirectHeaderByteEditAuthorityKind::None,
          /*originalText=*/{}, /*replacementText=*/{},
          /*hasExactByteTransformation=*/false};
      includeMutation.replacementText = patch->insertBytes;
      preservation.mutations.push_back(std::move(includeMutation));
      appendU64(patch->aStart);
      appendU64(patch->aEnd);
      appendU64(patch->bStart);
      appendU64(patch->bEnd);
      appendString(patch->insertBytes);
      appendBool(patch->condArm.present);
      appendU64(patch->condArm.armId);
      appendBool(patch->hasDirectHeaderByteRange);
      appendU64(patch->directHeaderByteBegin);
      appendU64(patch->directHeaderByteEnd);
      appendU64(static_cast<uint64_t>(patch->directHeaderByteAuthority));
      appendProofSummary(patch->proofSummary);
    }
  }

  SmallVector<std::optional<uint64_t>, 16> ownerIds;
  ownerIds.reserve(macroPatchesByOwner_.size());
  for (const auto &entry : macroPatchesByOwner_)
    ownerIds.push_back(entry.first);
  llvm::sort(ownerIds, [](const std::optional<uint64_t> &lhs,
                          const std::optional<uint64_t> &rhs) {
    if (!lhs)
      return rhs.has_value();
    if (!rhs)
      return false;
    return *lhs < *rhs;
  });
  appendString("macro-patches");
  appendU64(ownerIds.size());
  for (const std::optional<uint64_t> &ownerId : ownerIds) {
    appendOptionalU64(ownerId);
    const auto it = macroPatchesByOwner_.find(ownerId);
    assert(it != macroPatchesByOwner_.end());
    std::vector<const MacroPatch *> patches;
    patches.reserve(it->second.size());
    for (const MacroPatch &patch : it->second)
      patches.push_back(&patch);
    llvm::sort(patches, [](const MacroPatch *lhs, const MacroPatch *rhs) {
      if (lhs->invRange.begin != rhs->invRange.begin)
        return lhs->invRange.begin < rhs->invRange.begin;
      if (lhs->invRange.end != rhs->invRange.end)
        return lhs->invRange.end < rhs->invRange.end;
      return lhs->macroId < rhs->macroId;
    });
    appendU64(patches.size());
    for (const MacroPatch *patch : patches) {
      AlignmentSemanticMutationRecord macroMutation{
          AlignmentSemanticMutationDomain::MacroInvocationTokens, ownerId,
          patch->macroId, patch->invRange.begin, patch->invRange.end,
          patch->replacement.size(), /*protectedStructure=*/false,
          /*originalText=*/{}, /*replacementText=*/{},
          /*hasExactByteTransformation=*/false};
      macroMutation.replacementText = patch->replacement;
      // TU-owned macro invocation ranges use physical TU byte coordinates.
      // Header-owned ranges require a separate owner-source buffer and remain
      // exact-identity-only evidence until that buffer is available here.
      if (!ownerId && patch->invRange.begin <= patch->invRange.end &&
          patch->invRange.end <= tuSourceBytes.size()) {
        macroMutation.originalText =
            tuSourceBytes.slice(patch->invRange.begin,
                                patch->invRange.end).str();
        macroMutation.hasExactByteTransformation = true;
      }
      preservation.mutations.push_back(std::move(macroMutation));
      appendU64(patch->invRange.begin);
      appendU64(patch->invRange.end);
      appendU64(patch->macroId);
      appendString(patch->replacement);
      appendBool(patch->materialized.hasBTokenRange);
      appendU64(patch->materialized.bTokStart);
      appendU64(patch->materialized.bTokEnd);
      appendBool(patch->materialized.hasOutputByteRange);
      appendU64(patch->materialized.outputByteStart);
      appendU64(patch->materialized.outputByteEnd);
      appendBool(patch->ownerCert.present);
      appendBool(patch->ownerCert.mixedWitness);
      appendU64(patch->ownerCert.kindCode);
      appendU64(patch->ownerCert.includeId);
      appendBool(patch->ownerCert.condArm.present);
      appendU64(patch->ownerCert.condArm.armId);
      appendU64(patch->ownerCert.witnessCount);
      appendProofSummary(patch->proofSummary);
      appendBool(patch->selectedAcceptedCandidate.has_value());
      if (patch->selectedAcceptedCandidate)
        appendAcceptedEquivalenceKey(*patch->selectedAcceptedCandidate,
                                     "macro patch accepted carrier");
    }
  }

  llvm::sort(preservation.mutations,
             [](const AlignmentSemanticMutationRecord &lhs,
                const AlignmentSemanticMutationRecord &rhs) {
               if (lhs.domain != rhs.domain)
                 return static_cast<uint8_t>(lhs.domain) <
                        static_cast<uint8_t>(rhs.domain);
               if (lhs.ownerIncludeId != rhs.ownerIncludeId)
                 return lhs.ownerIncludeId < rhs.ownerIncludeId;
               if (lhs.ownerId != rhs.ownerId)
                 return lhs.ownerId < rhs.ownerId;
               if (lhs.begin != rhs.begin)
                 return lhs.begin < rhs.begin;
               if (lhs.end != rhs.end)
                 return lhs.end < rhs.end;
               if (lhs.replacementBytes != rhs.replacementBytes)
                 return lhs.replacementBytes < rhs.replacementBytes;
               return lhs.protectedStructure < rhs.protectedStructure;
             });
  llvm::sort(semanticPostconditions);
  llvm::sort(realizationPostconditions);
  auto serializePostconditions = [](ArrayRef<std::string> postconditions) {
    std::string serialized;
    serialized.append(std::to_string(postconditions.size()));
    serialized.push_back(';');
    for (const std::string &postcondition : postconditions) {
      serialized.append(std::to_string(postcondition.size()));
      serialized.push_back(';');
      serialized.append(postcondition);
      serialized.push_back(';');
    }
    return serialized;
  };
  preservation.semanticPostconditionKey =
      serializePostconditions(semanticPostconditions);
  preservation.realizationPostconditionKey =
      serializePostconditions(realizationPostconditions);

  return result;
}

} // namespace refold
} // namespace clang
