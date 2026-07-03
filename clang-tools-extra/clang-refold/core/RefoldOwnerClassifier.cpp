//===--- RefoldOwnerClassifier.cpp -----------------------------*- C++ -*-===//
//
// Owner and translation-unit ownership classification service implementation.
//
//===----------------------------------------------------------------------===//

#include "core/RefoldOwnerClassifier.h"
#include "edit/RefoldTUEditPlanner.h"
#include "proof/RefoldSidebandReplayProof.h"
#include "util/RefoldPathIdentity.h"

#include "llvm/ADT/STLExtras.h"

#include <algorithm>
#include <cassert>
#include <optional>
#include <tuple>
#include <utility>
#include <vector>

using namespace llvm;

namespace clang {
namespace refold {

RefoldOwnerClassifier::RefoldOwnerClassifier(Deps deps)
    : deps_(std::move(deps)) {}

Owner RefoldOwnerClassifier::ClassifyOwnerWithSegments(
    StringRef tuPath, const diffutils::Hunk &h) const {
  uint64_t a0 = h.aStart;
  uint64_t a1 = h.aEnd;

  // Insertion ownership:
  //
  // If the PP gap aligns with a stable TU slot boundary (include boundary or
  // conditional-arm boundary), defer to the segment-based classification (using
  // TU byte anchoring).
  //
  // Otherwise, if both sides of the gap are unambiguously within the same
  // include's PP coverage, treat the insertion as include-owned.
  if (a0 == a1) {
    if (auto slotAnchor =
            deps_.tuEdits.FindExactSlotBoundaryFromPPGap(tuPath, a0)) {
      (void)slotAnchor;
      std::optional<uint64_t> leftInc =
          (a0 > 0) ? deps_.model.InnermostIncludeAtPP(a0 - 1) : std::nullopt;
      const uint64_t maxPP = deps_.model.GetTokensCountA();
      std::optional<uint64_t> rightInc =
          a0 < maxPP ? deps_.model.InnermostIncludeAtPP(a0) : std::nullopt;

      if (leftInc && rightInc && *leftInc == *rightInc)
        return Owner::Include(*rightInc);

      auto includeHasSidebandWork = [&](uint64_t includeId) {
        return llvm::any_of(deps_.sidebandPragmaEdits,
                            [&](const SidebandPragmaEdit &sideband) {
                              return sideband.TargetsInclude(includeId);
                            });
      };

      auto includeHasProvedSidebandInsertion = [&](uint64_t includeId) {
        return llvm::any_of(deps_.sidebandPragmaEdits,
                            [&](const SidebandPragmaEdit &sideband) {
                              // A header-owned sideband replacement/deletion
                              // proves that the header needs sideband work, but
                              // it does not prove that an unrelated ordinary
                              // B-token insertion before the include belongs to
                              // that header.  Prefix co-ownership is reserved
                              // for visible B-only sideband insertions, where
                              // the ordinary insertion island and the sideband
                              // replay line are the same owner-local boundary
                              // payload.
                              return sideband.TargetsInclude(includeId) &&
                                     sideband.SourceIsZeroWidthInsertion() &&
                                     sideband.EmitsVisibleReplayText();
                            });
      };

      if (!leftInc && rightInc &&
          includeHasProvedSidebandInsertion(*rightInc)) {
        // A PP gap exactly at an include boundary is ambiguous in pure token
        // space: a B-only insertion before the first header token may be a TU
        // insertion before the #include or a header-prefix insertion that
        // materializes the include.  When the same boundary also carries a
        // proved header-owned sideband insertion, choose the include owner so
        // the ordinary bytes and sideband bytes compose in one owner-local
        // materialization proof instead of being split across TU and header
        // surfaces.
        return Owner::Include(*rightInc);
      }

      if (!leftInc && rightInc && includeHasSidebandWork(*rightInc)) {
        // If the right owner is a child include with sideband replacement or
        // deletion work, the collapsed PP gap denotes the boundary in the
        // parent header before that child include.  Do not pull the ordinary
        // insertion into the child merely because the child's first ordinary
        // token is the first PP token in the TU; compose it on the parent
        // owner surface and let the child sideband edit remain child-owned.
        if (const auto *rightInclude = deps_.model.GetIncludeById(*rightInc)) {
          if (rightInclude->parent &&
              !includeHasProvedSidebandInsertion(*rightInc))
            return Owner::Include(*rightInclude->parent);
        }
      }

      if (leftInc && !rightInc && includeHasSidebandWork(*leftInc)) {
        // A suffix insertion after a header that already has proved sideband
        // work belongs to the same materialized header surface.  This is the
        // include-boundary mirror of preserving an untouched `#include`: when
        // the include is not otherwise materialized, parent source after the
        // directive is the stable spelling; once the header is being opened for
        // sideband replacement/deletion/insertion, the after-boundary payload
        // composes as a header suffix and the include wrapper performs the
        // return-to-parent line repair.
        return Owner::Include(*leftInc);
      }
    }
  }

  // First, get the TU byte span for this hunk. Even when the hunk ultimately
  // belongs to a header, we still anchor via the TU span because segments for
  // includes and conditional arms in that header are projected into the TU
  // through slots.
  auto span = deps_.tuEdits.PlanTUByteSpan(a0, a1, tuPath); // [b, e)

  // No truthful TU byte anchor exists for this PP segment, so choose its owner
  // using only preprocessed-token structure. This happens when the segment has
  // no TU-backed tokens in range, and a pure insertion cannot be safely tied to
  // a concrete TU byte position. In that case, recover ownership from the
  // include / conditional context at the PP boundaries:
  //   - for insertions, inspect the PP token immediately to the left and right
  //     of the insertion gap
  //   - for non-insertions, inspect the PP endpoints covered by the segment
  // If both sides live under a common include, assign the segment to that
  // least-common-ancestor include, and preserve a same-arm conditional owner
  // when both sides are in the same selected arm. If no include owner can be
  // established, fall back to TU ownership; this should be rare and typically
  // indicates a TU-boundary case without a stronger slot anchor.
  if (!span) {
    const bool isInsert = (a0 == a1);
    const size_t n = deps_.model.GetTokensCountA();

    std::optional<uint64_t> leftInc;
    std::optional<uint64_t> rightInc;

    std::optional<RefoldModel::ArmRef> leftArmRef;
    std::optional<RefoldModel::ArmRef> rightArmRef;

    if (isInsert) {
      // Pure insertion: classify the gap from the PP token just before and just
      // after the insertion site, when those neighbors exist.
      if (a0 > 0) {
        leftInc = deps_.model.InnermostIncludeAtPP(a0 - 1);
        leftArmRef = deps_.model.FindArmRefAtPP(a0 - 1);
      }
      if (static_cast<size_t>(a0) < n) {
        rightInc = deps_.model.InnermostIncludeAtPP(a0);
        rightArmRef = deps_.model.FindArmRefAtPP(a0);
      }
    } else {
      // Non-insertion: classify from the PP endpoints actually covered by the
      // segment.
      leftInc = deps_.model.InnermostIncludeAtPP(a0);
      rightInc = deps_.model.InnermostIncludeAtPP(a1 - 1);
      leftArmRef = deps_.model.FindArmRefAtPP(a0);
      rightArmRef = deps_.model.FindArmRefAtPP(a1 - 1);
    }

    // Use the least common ancestor include of the left/right PP contexts as
    // the structural include owner, if one exists.
    std::optional<uint64_t> lcaInc =
        deps_.model.LeastCommonAncestorInclude(leftInc, rightInc);

    // Preserve a conditional-arm owner only when both PP sides are in the same
    // selected arm.
    std::optional<uint64_t> condArmId;
    if (leftArmRef && rightArmRef && leftArmRef->arm && rightArmRef->arm &&
        leftArmRef->arm->id == rightArmRef->arm->id)
      condArmId = leftArmRef->arm->id;

    if (lcaInc)
      return Owner::Include(*lcaInc, condArmId);

    // No include owner could be recovered from PP structure; fall back to TU.
    return Owner::TU(condArmId);
  }

  uint64_t b = span->tuByteBegin, e = span->tuByteEnd;
  if (b > e)
    std::swap(b, e);

  // Build (or fetch) all segments projected into tuPath.
  ArrayRef<RefoldModel::Segment> segs = deps_.model.GetSegmentsForFile(tuPath);
  if (segs.empty())
    return Owner::Unknown();

  // Collect all segment candidates that could own this hunk at the current TU
  // path.
  //
  // Non-insertions own real byte coverage, so any segment that intersects the
  // hunk byte range [b,e) is a candidate.
  //
  // Pure insertions usually have no byte width in TU space. For those, treat
  // the insertion site as a single probe point at `b` and collect every segment
  // that contains that point. The comparison is right-closed (`s.b <= probe &&
  // probe <= s.e`) so an insertion that lands exactly on a segment boundary can
  // still be claimed by an enclosing/parent segment.
  const bool isInsert = (a0 == a1);
  const uint64_t probe = b;

  std::vector<const RefoldModel::Segment *> hits;
  for (const auto &s : segs) {
    if (!isInsert) {
      // Standard half-open interval intersection test for [b,e) vs [s.b,s.e).
      if (s.e <= b || e <= s.b)
        continue;
      hits.push_back(&s);
    } else {
      // Zero-width insertion: classify by containment of the insertion probe.
      if (s.b <= probe && probe <= s.e)
        hits.push_back(&s);
    }
  }

  if (hits.empty())
    return Owner::Unknown();

  // Choose the most specific candidate segment.
  //
  // Prefer the smallest byte span first. If multiple hits have the same size,
  // break ties deterministically by earlier start, then earlier end.
  const RefoldModel::Segment *selected = hits[0];
  for (size_t i = 1; i < hits.size(); ++i) {
    const auto *s = hits[i];
    uint64_t sLen = s->e - s->b;
    uint64_t selLen = selected->e - selected->b;

    if (sLen < selLen) {
      selected = s;
    } else if (sLen == selLen) {
      if (s->b < selected->b) {
        selected = s;
      } else if (s->b == selected->b && s->e < selected->e) {
        selected = s;
      }
    }
  }

  // Convert the selected segment's stored ownership into a concrete TU/include
  // owner result, preserving any conditional-arm owner attached to that
  // segment.
  if (!selected->ownerIncludeId)
    return Owner::TU(selected->ownerCondArmId);
  return Owner::Include(*selected->ownerIncludeId, selected->ownerCondArmId);
}

bool RefoldOwnerClassifier::HunkMapsToTU(uint64_t a0, uint64_t a1,
                                         StringRef tuPath) const {
  bool sawAnyTU = false;
  const auto &tokmapByPP = deps_.model.GetTokmapByPP();

  // Walk the A-side PP byte range and require every mapped byte to belong to
  // the translation unit itself. Unmapped bytes (whitespace/separators) are
  // ignored; the hunk ceases to be TU-owned as soon as any mapped byte resolves
  // to a different file.
  for (uint64_t pp = a0; pp < a1; ++pp) {
    auto it = tokmapByPP.find(pp);
    if (it == tokmapByPP.end())
      continue; // ignore unmapped (spaces/tabs/newlines)
    const auto &t = it->second;
    if (!deps_.pathIdentity.PathsEqual(t.file, tuPath))
      return false; // spans a non-TU mapping
    sawAnyTU = true;
  }

  // Non-empty range: if we only saw TU mappings (or nothing but whitespace),
  // then the hunk maps to the TU. Otherwise it mapped to some header above.
  if (a0 != a1)
    return sawAnyTU;

  // INSERTION (A gap): classify TU ownership only when we can derive a truthful
  // TU insertion anchor at that exact PP gap.
  return deps_.tuEdits.FindProvableTUInsertionAnchor(a0, tuPath).has_value();
}

} // namespace refold
} // namespace clang
