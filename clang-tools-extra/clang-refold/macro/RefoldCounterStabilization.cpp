//===--- RefoldCounterStabilization.cpp -------------------------*- C++ -*-===//
//
// __COUNTER__ stabilization planner implementation.
//
// This file implements the read-only suffix-forcing policy for
// counter-sensitive macro realizations.  The planner uses macro topology plus
// explicit owner and patch-disposition predicates to decide which later
// counter-bearing macro callsites must remain expanded.
//
//===----------------------------------------------------------------------===//

#include "macro/RefoldCounterStabilization.h"

#include "macro/RefoldMacroPatchPlanner.h"
#include "macro/RefoldMacroPlannerHelpers.h"
#include "macro/RefoldMacroWholeCoverPlanBuilder.h"
#include "proof/RefoldMacroPatchProofClassifier.h"
#include "proof/RefoldOwnerStateProof.h"
#include "proof/RefoldWitnessTrace.h"
#include "source/RefoldSourceMapper.h"
#include "source/RefoldStructuralHunkDispatcher.h"
#include "support/RefoldLog.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/Hashing.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/FormatVariadic.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <limits>
#include <string>
#include <utility>
#include <vector>

using namespace llvm;

namespace clang {
namespace refold {

namespace {

/// Return true when an expanded realization staged in \p root's owner
/// replaces source bytes that contain \p root's callsite `[invB, invE)`.
///
/// The patch need not be \p root's own: a whole cover replaces every byte its
/// expansion consumed, so a callsite spelled in an actual of the covered call,
/// or in source after its closing parenthesis that a call it generated read,
/// disappears with it, and the replacement is B text.  A `__COUNTER__` read at
/// that callsite has then left the replay-time counter sequence.  A patch that
/// keeps its invocation's callsite spelling does not count.
bool callsiteReplacedByExpandedPatch(
    const RefoldMacroTopology &macroTopology,
    const DenseMap<std::optional<uint64_t>, DenseMap<uint64_t, MacroPatch>>
        &macroPatchByOwnerByMacroId,
    const RefoldModel::MacroInvocation &root) {
  const auto oit = macroPatchByOwnerByMacroId.find(root.ownerIncludeId);
  if (oit == macroPatchByOwnerByMacroId.end())
    return false;
  if (!root.invB || !root.invE)
    return false;
  for (const auto &kv : oit->second) {
    const MacroPatch &patch = kv.second;
    if (*root.invB < patch.invRange.begin || patch.invRange.end < *root.invE)
      continue;
    if (macroTopology.MacroPatchRemainsExpanded(patch))
      return true;
  }
  return false;
}

} // namespace

RefoldCounterStabilization::RefoldCounterStabilization(
    const RefoldModel &model, ArrayRef<PPTok> aToks, ArrayRef<PPTok> bToks,
    const RefoldMacroTopology &macroTopology,
    const RefoldOwnerClassifier &ownerClassifier)
    : model_(model), aToks_(aToks), bToks_(bToks),
      macroTopology_(macroTopology), ownerClassifier_(ownerClassifier) {}

SmallVector<CounterOccurrence, 32>
RefoldCounterStabilization::CollectCounterOccurrences(StringRef tuPath) const {
  SmallVector<CounterOccurrence, 32> occs;

  uint64_t occurrenceOrdinal = 0;

  // Record a __COUNTER__ occurrence in PP-token space. Zero-length producer
  // anchors are widened to one token so pasted/stringified counter output still
  // participates in stabilization.  A stable CounterEventIdentity lets the
  // stabilization proof name a concrete counter event rather than a global
  // textual guess.
  auto addOcc = [&](const RefoldModel::MacroInvocation &mi, uint64_t b,
                    uint64_t e) {
    if (e == b && b < aToks_.size())
      e = b + 1;
    if (e <= b)
      return;

    CounterEventIdentity event =
        macroTopology_.BuildCounterEventIdentity(mi, occurrenceOrdinal++, b, e);
    occs.push_back(CounterOccurrence{&mi, b, e, std::nullopt, event});
  };

  // Collect all recorded __COUNTER__ outputs. Prefer the most precise producer
  // ranges first: bodySpans, then explicit spans, then the fallback cover.
  for (const auto &mi : model_.GetMacroInvocations()) {
    if (mi.name != "__COUNTER__")
      continue;
    if (!mi.invB || !mi.invE || !mi.invText)
      continue;

    if (!mi.bodySpans.empty()) {
      for (const auto &bs : mi.bodySpans)
        addOcc(mi, bs.begin, bs.end);
      continue;
    }

    if (!mi.spans.empty()) {
      for (const auto &range : mi.spans)
        addOcc(mi, range.begin, range.end);
      continue;
    }

    // Finally use the producer cover.  A zero-width cover is not a byte-
    // recovery fallback; it is a producer anchor for the concrete counter
    // literal token.  Normalize it through addOcc() so the same one-token
    // widening used for body/spans also applies to covers.
    if (mi.cover.IsValid())
      addOcc(mi, mi.cover.begin, mi.cover.end);
  }

  if (occs.empty())
    return {};

  // Determine the actual PP-owner include for each occurrence. This filters out
  // producer-merged duplicate records whose recorded ownerIncludeId does not
  // match the owner inferred from the current TU segmentation.
  SmallVector<CounterOccurrence, 32> filtered;
  filtered.reserve(occs.size());
  for (const CounterOccurrence &occ : occs) {
    diffutils::Hunk dummy;
    dummy.aStart = occ.aStart;
    dummy.aEnd = occ.aEnd;
    dummy.bStart = 0;
    dummy.bEnd = 0;

    Owner owner = ownerClassifier_.ClassifyOwnerWithSegments(tuPath, dummy);
    std::optional<uint64_t> ownerInc;
    if (owner.kind == OwnerKind::Include)
      ownerInc = owner.includeId;

    // If the producer says this macro belongs to a specific include instance,
    // require the token occurrence to be owned by that same include instance.
    if (occ.macro->ownerIncludeId && ownerInc &&
        *occ.macro->ownerIncludeId != *ownerInc)
      continue;

    CounterOccurrence normalized = occ;
    normalized.ownerIncludeId = ownerInc;
    normalized.event.ownerIncludeId = ownerInc;
    filtered.push_back(std::move(normalized));
  }

  SortCounterOccurrences(filtered);
  return filtered;
}

void RefoldCounterStabilization::SortCounterOccurrences(
    SmallVectorImpl<CounterOccurrence> &occs) {
  llvm::sort(occs,
             [](const CounterOccurrence &lhs, const CounterOccurrence &rhs) {
               if (lhs.aStart != rhs.aStart)
                 return lhs.aStart < rhs.aStart;
               if (lhs.aEnd != rhs.aEnd)
                 return lhs.aEnd < rhs.aEnd;
               if (lhs.ownerIncludeId != rhs.ownerIncludeId)
                 return lhs.ownerIncludeId < rhs.ownerIncludeId;
               return lhs.macro->id < rhs.macro->id;
             });
}

SmallVector<ForcedMacroPatchRequest, 32>
RefoldCounterStabilization::ComputeForcedCounterPatches(
    StringRef tuPath, ArrayRef<int64_t> a2b) const {
  SmallVector<CounterOccurrence, 32> filtered =
      CollectCounterOccurrences(tuPath);
  if (filtered.empty())
    return {};

  // A counter occurrence is considered edited if any A-side token in its
  // occurrence range is unmapped, maps outside B, or maps to a token with a
  // different spelling.
  auto isEditedOcc = [&](const CounterOccurrence &o) -> bool {
    for (uint64_t ai = o.aStart; ai < o.aEnd; ++ai) {
      if (static_cast<size_t>(ai) >= a2b.size() ||
          static_cast<size_t>(ai) >= aToks_.size())
        return true;
      const int64_t bj = a2b[static_cast<size_t>(ai)];
      if (bj < 0 || static_cast<size_t>(bj) >= bToks_.size())
        return true;
      if (aToks_[static_cast<size_t>(ai)].spelling !=
          bToks_[static_cast<size_t>(bj)].spelling)
        return true;
    }
    return false;
  };

  auto isEditedInvocationOutput =
      [&](const RefoldModel::MacroInvocation &m) -> bool {
    auto spanEdited = [&](uint64_t lo, uint64_t hi) -> bool {
      for (uint64_t ai = lo; ai < hi; ++ai) {
        if (static_cast<size_t>(ai) >= a2b.size() ||
            static_cast<size_t>(ai) >= aToks_.size())
          return true;
        const int64_t bj = a2b[static_cast<size_t>(ai)];
        if (bj < 0 || static_cast<size_t>(bj) >= bToks_.size())
          return true;
        if (aToks_[static_cast<size_t>(ai)].spelling !=
            bToks_[static_cast<size_t>(bj)].spelling)
          return true;
      }
      return false;
    };

    // Compare emitted output segments directly when the producer recorded them.
    // Do not collapse discontiguous bodySpans into one merged envelope: the
    // holes between body spans are caller-formal substitution sites, and edits
    // to those sites must not be misclassified as edits to an embedded
    // __COUNTER__ occurrence.
    if (!m.bodySpans.empty()) {
      for (const auto &s : m.bodySpans) {
        if (s.begin < s.end && spanEdited(s.begin, s.end))
          return true;
      }
      return false;
    }

    // Explicit spans may also include zero-length paste/stringify anchors. For
    // those anchors, compare the anchored token as the effective output slice.
    if (!m.spans.empty()) {
      for (const auto &s : m.spans) {
        if (s.begin < s.end) {
          if (spanEdited(s.begin, s.end))
            return true;
          continue;
        }
        if (s.begin == s.end && s.begin < aToks_.size()) {
          if (spanEdited(s.begin, s.begin + 1))
            return true;
        }
      }
      return false;
    }

    if (m.cover.IsValid()) {
      if (m.cover.end > m.cover.begin)
        return spanEdited(m.cover.begin, m.cover.end);
      if (m.cover.end == m.cover.begin && m.cover.begin < aToks_.size())
        return spanEdited(m.cover.begin, m.cover.begin + 1);
    }

    return false;
  };

  int firstEditedIdx = -1;
  for (size_t i = 0; i < filtered.size(); ++i) {
    if (isEditedOcc(filtered[i])) {
      firstEditedIdx = static_cast<int>(i);
      break;
    }
  }

  // Fallback: some nested-macro scenarios record __COUNTER__ with a zero-length
  // PP span anchored at a caller site that is not the edited output token
  // itself. In those cases, the direct __COUNTER__ occurrence comparison above
  // can miss the edit. Recover by lifting each __COUNTER__ occurrence to a
  // patchable caller macro whose bodySpans/cover correspond to the actual
  // expanded token slice, then redo edit detection.
  if (firstEditedIdx < 0) {
    DenseMap<uint64_t, const RefoldModel::MacroInvocation *> invById;
    invById.reserve(model_.GetMacroInvocations().size());
    for (const auto &mi : model_.GetMacroInvocations())
      invById[mi.id] = &mi;

    // Walk out of macro-definition-only frames until reaching the invocation
    // that can actually be patched in source.
    auto findPatchableCaller = [&](const RefoldModel::MacroInvocation &mi)
        -> const RefoldModel::MacroInvocation * {
      const RefoldModel::MacroInvocation *cur = &mi;
      while (cur) {
        if (!macroTopology_.IsInvocationInsideDefineDirective(*cur))
          return cur;
        if (!cur->callerMacroId)
          break;
        auto it = invById.find(*cur->callerMacroId);
        if (it == invById.end())
          break;
        cur = it->second;
      }
      return nullptr;
    };

    auto computeOccRange = [&](const RefoldModel::MacroInvocation &m)
        -> std::optional<std::pair<uint64_t, uint64_t>> {
      uint64_t lo = std::numeric_limits<uint64_t>::max();
      uint64_t hi = 0;
      for (const auto &range :
           macroTopology_.CounterOutputRangesForInvocation(m)) {
        uint64_t begin = range.first;
        uint64_t end = range.second;
        if (end == begin && begin < aToks_.size())
          end = begin + 1;
        if (end <= begin)
          continue;
        lo = std::min(lo, begin);
        hi = std::max(hi, end);
      }
      if (lo == std::numeric_limits<uint64_t>::max() || lo >= hi)
        return std::nullopt;
      return std::make_pair(lo, hi);
    };

    SmallVector<CounterOccurrence, 32> lifted;
    lifted.reserve(filtered.size());
    for (const auto &o : filtered) {
      const RefoldModel::MacroInvocation *root = findPatchableCaller(*o.macro);
      if (!root)
        continue;

      auto r = computeOccRange(*root);
      if (!r)
        continue;

      CounterEventIdentity event = macroTopology_.BuildCounterEventIdentity(
          *root, o.event.occurrenceOrdinal, r->first, r->second,
          root->ownerIncludeId);
      lifted.push_back(CounterOccurrence{root, r->first, r->second,
                                         root->ownerIncludeId, event});
    }

    if (!lifted.empty()) {
      SortCounterOccurrences(lifted);

      int idx = -1;
      for (size_t i = 0; i < lifted.size(); ++i) {
        if (isEditedInvocationOutput(*lifted[i].macro)) {
          idx = static_cast<int>(i);
          break;
        }
      }

      if (idx >= 0) {
        REFOLD_LOG_TRACE(
            "counter",
            "__COUNTER__: lifted edit detection found firstEditedIdx={0} at "
            "A=[{1},{2}) root='{3}'",
            idx, lifted[static_cast<size_t>(idx)].aStart,
            lifted[static_cast<size_t>(idx)].aEnd,
            lifted[static_cast<size_t>(idx)].macro->name);
        filtered.swap(lifted);
        firstEditedIdx = idx;
      }
    }
  }

  if (firstEditedIdx < 0) {
    REFOLD_LOG_TRACE("counter",
                     "__COUNTER__: no edited occurrences; no forced expansion");
    return {};
  }

  // Unique key for forced patch requests. Counter stabilization may encounter
  // multiple counter occurrences covered by the same patchable macro; emit only
  // one forced request per owner/include + invocation byte range.
  SmallVector<ForcedMacroPatchRequest, 32> forced;
  llvm::DenseSet<llvm::hash_code> seen;

  // Once one __COUNTER__ occurrence is edited, every subsequent occurrence in
  // PP order must be forced through macro realization as well. Otherwise the
  // counter sequence after the edit could remain stale relative to the edited
  // preprocessed stream.
  for (const CounterOccurrence &o :
       llvm::drop_begin(filtered, firstEditedIdx)) {
    const auto *root = macroTopology_.SmallestCoveringPatchableMacro(
        o.aStart, o.aEnd, o.ownerIncludeId);

    // Safety: never patch macro definitions.
    if (!root || macroTopology_.IsInvocationInsideDefineDirective(*root))
      continue;

    const auto invStart = root->invB;
    const auto invEnd = root->invE;
    if (!invStart || !invEnd)
      continue;

    auto key = llvm::hash_combine(root->ownerIncludeId.value_or(0), *invStart,
                                  *invEnd);
    if (!seen.insert(key).second)
      continue;

    forced.push_back({root, o.aStart, o.aEnd, o.event});

    REFOLD_LOG_TRACE("counter",
                     "__COUNTER__: force root id={0} name='{1}' ownerInc={2} "
                     "inv=[{3},{4}) for occ A=[{5},{6})",
                     root->id, root->name, root->ownerIncludeId, *invStart,
                     *invEnd, o.aStart, o.aEnd);
  }

  REFOLD_LOG_DEBUG("counter",
                   "__COUNTER__: occurrences={0} firstEditedIdx={1} forced={2} "
                   "firstOccA=[{3},{4})",
                   filtered.size(), firstEditedIdx, forced.size(),
                   filtered[static_cast<size_t>(firstEditedIdx)].aStart,
                   filtered[static_cast<size_t>(firstEditedIdx)].aEnd);

  return forced;
}

SmallVector<ForcedMacroPatchRequest, 32>
RefoldCounterStabilization::ComputeForcedCounterPatchesFromExpandedMacros(
    StringRef tuPath,
    const DenseMap<std::optional<uint64_t>, DenseMap<uint64_t, MacroPatch>>
        &macroPatchByOwnerByMacroId) const {
  SmallVector<CounterOccurrence, 32> filtered =
      CollectCounterOccurrences(tuPath);
  if (filtered.empty())
    return {};

  // Find the first __COUNTER__ occurrence whose smallest patchable owner is
  // already going to remain expanded. That point starts the
  // forced-stabilization suffix: once one counter-bearing macro remains
  // expanded, all following counter-bearing roots must also be forced to
  // preserve counter sequencing.
  int firstExpandedIdx = -1;
  for (size_t i = 0; i < filtered.size(); ++i) {
    const auto *root = macroTopology_.SmallestCoveringPatchableMacro(
        filtered[i].aStart, filtered[i].aEnd, filtered[i].ownerIncludeId);
    if (!root || macroTopology_.IsInvocationInsideDefineDirective(*root))
      continue;
    if (callsiteReplacedByExpandedPatch(macroTopology_,
                                        macroPatchByOwnerByMacroId, *root)) {
      firstExpandedIdx = static_cast<int>(i);
      break;
    }
  }

  if (firstExpandedIdx < 0)
    return {};

  SmallVector<ForcedMacroPatchRequest, 32> forced;
  llvm::DenseSet<llvm::hash_code> seen;

  // Force every counter occurrence from the first expanded one onward. Dedup by
  // owner/include plus invocation byte span because multiple occurrences may be
  // covered by the same patchable macro invocation.
  for (const CounterOccurrence &o :
       llvm::drop_begin(filtered, firstExpandedIdx)) {
    const auto *root = macroTopology_.SmallestCoveringPatchableMacro(
        o.aStart, o.aEnd, o.ownerIncludeId);
    if (!root || macroTopology_.IsInvocationInsideDefineDirective(*root))
      continue;
    const auto invStart = root->invB;
    const auto invEnd = root->invE;
    if (!invStart || !invEnd)
      continue;

    auto key = llvm::hash_combine(root->ownerIncludeId.value_or(0), *invStart,
                                  *invEnd);
    if (!seen.insert(key).second)
      continue;

    forced.push_back({root, o.aStart, o.aEnd, o.event});
    REFOLD_LOG_TRACE(
        "counter",
        "__COUNTER__: expanded-macro stabilization firstExpandedIdx={0} "
        "force root id={1} name='{2}' ownerInc={3} inv=[{4},{5}) for occ "
        "A=[{6},{7})",
        firstExpandedIdx, root->id, root->name, root->ownerIncludeId, *invStart,
        *invEnd, o.aStart, o.aEnd);
  }

  return forced;
}

// =================== Forced __COUNTER__ patch construction =================

namespace {

/// The B material a forced counter patch emits in place of its invocation.
struct CounterReplacementSurface {
  std::string text;
  /// The end of the source `text` replaces from the invocation's `invB`.
  uint64_t sourceEnd = 0;
  /// The B-token envelope the replacement realizes.
  std::pair<uint64_t, uint64_t> envelope;
  /// The B tokens `text` spells: the envelope, or the segment of it a whole
  /// cover kept after boundary claims.
  std::pair<uint64_t, uint64_t> spelled;
};

} // namespace

/// The literal B prints for the `__COUNTER__` occurrence at A tokens
/// `[aStart, aEnd)`.  It is tied to that occurrence, not to reusable
/// macro-body text, and is realized from the bytes its B tokens occupy: the
/// trivia after them can hold a printed `#pragma` line whose source operator
/// still stands.
static std::optional<CounterReplacementSurface>
counterLiteralSurface(const RefoldSourceMapper &sourceMapper, uint64_t aStart,
                      uint64_t aEnd, uint64_t invEnd) {
  std::optional<std::pair<size_t, size_t>> bEnv =
      sourceMapper.MapATokRangeAToBTokenEnvelope(aStart, aEnd);
  if (!bEnv)
    return std::nullopt;
  std::optional<StringRef> material =
      sourceMapper.SliceBTokenMaterial(bEnv->first, bEnv->second);
  if (!material)
    return std::nullopt;
  const std::pair<uint64_t, uint64_t> range{bEnv->first, bEnv->second};
  return CounterReplacementSurface{material->trim().str(), invEnd, range,
                                   range};
}

/// The whole cover of an invocation whose expansion reads the counter, from
/// the same plan every other whole-cover realization uses: it replaces the
/// callsite and all the source the expansion read.
static std::optional<CounterReplacementSurface>
wholeCoverSurface(const RefoldMacroWholeCoverPlanBuilder &wholeCoverPlanBuilder,
                  const RefoldModel::MacroInvocation &m) {
  std::optional<WholeCoverPlan> plan =
      wholeCoverPlanBuilder.ComputeWholeCoverPlan(m);
  if (!plan || !plan->sourceEnd)
    return std::nullopt;
  return CounterReplacementSurface{std::move(plan->clippedText),
                                   *plan->sourceEnd,
                                   {plan->bTokStart, plan->bTokEnd},
                                   {plan->keptBTokStart, plan->keptBTokEnd}};
}

void applyForcedCounterPatches(
    ArrayRef<ForcedMacroPatchRequest> forced,
    const RefoldSourceMapper &sourceMapper,
    const RefoldMacroTopology &macroTopology,
    const RefoldMacroPatchPlanner &macroPatchPlanner,
    const RefoldMacroWholeCoverPlanBuilder &wholeCoverPlanBuilder,
    const RefoldMacroPatchProofClassifier &macroPatchProofClassifier,
    const RefoldOwnerStateProof &ownerStateProof,
    RefoldStructuralHunkDispatcher &structuralHunkDispatcher) {
  for (const auto &req : forced) {
    const RefoldModel::MacroInvocation *pm = req.macro;
    if (!pm)
      continue;
    const RefoldModel::MacroInvocation &m = *pm;

    // Safety: do not patch macro definitions.
    if (macroTopology.IsInvocationInsideDefineDirective(m))
      continue;

    // Forced patches still require a concrete physical invocation span; without
    // it there is no call-site text to replace.
    const auto invStart = m.invB;
    const auto invEnd = m.invE;
    if (!invStart || !invEnd || *invEnd < *invStart)
      continue;

    // Other forced counter-related requests realize the whole cover of the
    // enclosing invocation, aligned with whole macro invocation replay.
    std::optional<CounterReplacementSurface> surface =
        m.name == "__COUNTER__"
            ? counterLiteralSurface(sourceMapper, req.aStart, req.aEnd, *invEnd)
            : wholeCoverSurface(wholeCoverPlanBuilder, m);
    if (!surface)
      continue;

    // Coalesce by physical invocation span (inv_b/inv_e), matching the hunk
    // coalescing logic used during normal classification.
    RefoldStructuralHunkDispatcher::MacroPatchStagingSlot stagingSlot =
        structuralHunkDispatcher.PrepareMacroPatchStagingSlot(m);

    // Preserve an existing non-callsite (already-expanded) replacement.
    if (stagingSlot.existingPatch && !stagingSlot.existingIsCallsite)
      continue;

    // An expanded realization around this callsite already removed its read
    // from the counter sequence; a literal nested inside it would overlap it.
    if (callsiteReplacedByExpandedPatch(
            macroTopology,
            structuralHunkDispatcher.MacroPatchMergeBucketsForPlanner(), m))
      continue;

    CounterEventIdentity counterEventForWitness;
    SuffixStabilityWitness counterWitness = SuffixStabilityWitness::None();
    {
      CounterEventIdentity event = req.event;
      if (event.macroInvocationId == 0)
        event = macroTopology.BuildCounterEventIdentity(
            m, /*occurrenceOrdinal=*/0, req.aStart, req.aEnd, m.ownerIncludeId);
      if (!event.expectedBValue)
        event.expectedBValue = surface->text;
      counterEventForWitness = event;
      const OwnerStateBoundary boundary =
          ownerStateProof.CounterStateBoundaryForEvent(event);
      const std::string detail =
          llvm::formatv(
              "forced materialization of counter-sensitive invocation "
              "#{0} after edited counter occurrence A=[{1},{2})",
              m.id, req.aStart, req.aEnd)
              .str();
      counterWitness = ownerStateProof.BuildStateTransitionWitness(
          SuffixStabilityWitnessKind::OwnerMaterialization,
          OwnerStateComponent::Counter, boundary,
          llvm::formatv("{0}; {1}", detail,
                        ownerStateProof.FormatCounterEventForWitness(event))
              .str());
      (void)ownerStateProof.CheckStateTransitionAcrossEditBoundary(
          boundary, OwnerStateComponent::Counter,
          StateMutationKind::Materialized, counterWitness, "counter", detail,
          /*requireKnownObserver=*/false);
    }

    // Install the forced patch under the coalesced physical-span key. If this
    // replaces a previous call-site-shaped patch, carry its owner certificate
    // forward before certifying the current invocation owner.  The target-PP
    // proof is the B-token envelope the surface realizes, and its payload the
    // B tokens the text spells.
    MacroPatch patch{*invStart, surface->sourceEnd, std::move(surface->text)};
    certifyMacroPatchMaterializedBTokenRange(patch, surface->envelope.first,
                                             surface->envelope.second);
    patch.materialized.bPayload =
        BPayloadTokenCertificate{{surface->spelled}, BPayloadRest::None};
    if (stagingSlot.existingPatch)
      macroPatchPlanner.CarryMacroPatchOwnerCertificate(
          patch, *stagingSlot.existingPatch);
    macroPatchPlanner.CertifyMacroPatchOwnerWitness(
        patch,
        m.ownerIncludeId ? Owner::Include(*m.ownerIncludeId) : Owner::TU());

    // Forced __COUNTER__ stabilization is an invocation-realization proof, not
    // an anonymous text edit.  The forced root is often the spelling of
    // __COUNTER__ itself, but it can also be an enclosing macro invocation
    // whose expansion observes __COUNTER__ (for example PRINT(...) wrapping
    // __COUNTER__).  In both cases the emitted patch is required solely to keep
    // the counter sequence consistent after an earlier counter occurrence was
    // realized, so certify every forced counter-stabilization patch with the
    // explicit counter proof class and its typed state-stability witness.
    MacroPatchProof proof =
        makeMacroPatchProof(MacroPatchProofKind::CounterLiteral,
                            /*preservesInvocationStructure=*/false, m.id);
    CounterStateWitness counterState;
    counterState.hasCounterEvents = true;
    counterState.counterOrderKnown = true;
    counterState.suffixStateStable = true;
    counterState.materializationStable = true;
    counterState.counterConsumptionCount = 1;
    counterState.counterMutationCount = 1;
    if (counterWitness.kind != SuffixStabilityWitnessKind::None)
      counterState.preservedSuffixObserverCount = 1;
    if (counterEventForWitness.expectedBValue) {
      counterState.hasExpectedBValues = true;
      counterState.expectedBValueCount = 1;
      counterState.suffixValueSignature =
          llvm::formatv("expected={0}",
                        RefoldWitnessTrace::FormatWitnessTraceHash(
                            *counterEventForWitness.expectedBValue))
              .str();
    } else {
      counterState.hasMissingExpectedBValues = true;
      counterState.missingExpectedBValueCount = 1;
    }
    counterState.consumptionSignature =
        ownerStateProof.FormatCounterEventForWitness(counterEventForWitness);
    counterState.orderSignature =
        llvm::formatv("ordinal={0}:macro={1}:A=[{2},{3})",
                      counterEventForWitness.occurrenceOrdinal,
                      counterEventForWitness.macroInvocationId,
                      counterEventForWitness.aTokenBegin,
                      counterEventForWitness.aTokenEnd)
            .str();
    counterState.suffixObserverSignature =
        llvm::formatv("forced-materialization:{0}", counterWitness.kind).str();
    proof.suffixStability = std::move(counterWitness);
    proof.counterState = std::move(counterState);
    macroPatchProofClassifier.SetMacroPatchProof(patch, std::move(proof));

    // Use the coalesced key as the patch macro ID so later owner/macro maps see
    // one canonical patch per physical invocation span.
    structuralHunkDispatcher.StageMacroPatch(stagingSlot, std::move(patch));
  }
}

} // namespace refold
} // namespace clang
