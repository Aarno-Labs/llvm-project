//===--- RefoldStructuralHunkDispatcher.cpp ----------------------*- C++
//-*-===//
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

} // namespace refold
} // namespace clang
