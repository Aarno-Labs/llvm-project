//===--- RefoldMacroPasteArgumentBuilder.cpp --------------------*- C++ -*-===//
//
// Paste-aware argument derivation for macro args-only patch construction.
//
//===----------------------------------------------------------------------===//

#include "macro/RefoldMacroPasteArgumentBuilder.h"

#include "macro/RefoldMacroReplay.h"
#include "source/RefoldSourceMapper.h"
#include "util/StringUtils.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallVector.h"

#include <algorithm>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

using namespace llvm;

namespace clang {
namespace refold {

namespace {

/// Copy text to a string while removing line breaks that may be included by
/// token-range slice helpers at pasted-token frontiers.
std::string stripNewlines(StringRef text) {
  std::string result = text.str();
  result.erase(std::remove(result.begin(), result.end(), '\n'), result.end());
  return result;
}

/// Sort argument-span pointers by ascending pasted-token byte contribution.
bool ppArgSpanPtrLessByByteBegin(const RefoldModel::PPArgSpan *lhs,
                                 const RefoldModel::PPArgSpan *rhs) {
  if (!lhs->byteBegin)
    return false;
  if (!rhs->byteBegin)
    return true;
  if (*lhs->byteBegin != *rhs->byteBegin)
    return *lhs->byteBegin < *rhs->byteBegin;
  return lhs->byteEnd.value_or(0) < rhs->byteEnd.value_or(0);
}

/// Sort argument spans from right to left for in-place pasted-token rewriting.
bool ppArgSpanGreaterByByteBegin(const RefoldModel::PPArgSpan &lhs,
                                 const RefoldModel::PPArgSpan &rhs) {
  if (!lhs.byteBegin)
    return false;
  if (!rhs.byteBegin)
    return true;
  if (*lhs.byteBegin != *rhs.byteBegin)
    return *lhs.byteBegin > *rhs.byteBegin;
  return lhs.byteEnd.value_or(0) > rhs.byteEnd.value_or(0);
}

/// Outcome of inverting an adjacent, delimiter-free pasted-argument run.
///
/// `Unique` is the only result that may be used to rewrite source structure.
/// `Ambiguous` and `Unsupported` fail closed because the original `##` surface
/// contains no internal delimiter to justify invented boundaries.
enum class PasteRunInvertibilityKind {
  Unique,
  NoMatch,
  Ambiguous,
  Unsupported,
};

/// Certificate produced when a delimiter-free paste run is invertible.
///
/// `derivedSegs` is ordered by the original paste-span sequence and contains
/// the replacement text assigned to each segment.
struct PasteRunInvertibilityCertificate {
  PasteRunInvertibilityKind kind = PasteRunInvertibilityKind::Unsupported;
  std::vector<std::string> derivedSegs;
};

/// Prepend one derived segment to a unique paste-run certificate.
PasteRunInvertibilityCertificate
prependDerivedSegment(PasteRunInvertibilityCertificate cert, StringRef seg) {
  if (cert.kind != PasteRunInvertibilityKind::Unique)
    return cert;

  cert.derivedSegs.insert(cert.derivedSegs.begin(), seg.str());
  return cert;
}

/// Merge two paste-run inversion attempts, preserving uniqueness only when
/// all successful witnesses agree on exactly the same derived segments.
PasteRunInvertibilityCertificate
mergePasteRunCertificates(PasteRunInvertibilityCertificate lhs,
                          const PasteRunInvertibilityCertificate &rhs) {
  if (lhs.kind == PasteRunInvertibilityKind::Unsupported ||
      rhs.kind == PasteRunInvertibilityKind::Unsupported) {
    lhs.kind = PasteRunInvertibilityKind::Unsupported;
    lhs.derivedSegs.clear();
    return lhs;
  }

  if (rhs.kind == PasteRunInvertibilityKind::NoMatch)
    return lhs;
  if (lhs.kind == PasteRunInvertibilityKind::NoMatch)
    return rhs;

  if (lhs.kind == PasteRunInvertibilityKind::Ambiguous ||
      rhs.kind == PasteRunInvertibilityKind::Ambiguous) {
    lhs.kind = PasteRunInvertibilityKind::Ambiguous;
    lhs.derivedSegs.clear();
    return lhs;
  }

  if (lhs.derivedSegs == rhs.derivedSegs)
    return lhs;

  lhs.kind = PasteRunInvertibilityKind::Ambiguous;
  lhs.derivedSegs.clear();
  return lhs;
}

/// Try to invert an edited spelling for one adjacent `##` argument run.
///
/// This helper accepts only runs that are anchored enough to derive a unique
/// per-argument split. Adjacent changed segments remain ambiguous because no
/// literal byte separates their boundary in the pasted token.
PasteRunInvertibilityCertificate buildAdjacentPasteRunInvertibilityCertificate(
    StringRef aTok, StringRef bRun,
    ArrayRef<const RefoldModel::PPArgSpan *> runSpans) {
  PasteRunInvertibilityCertificate cert;
  if (runSpans.empty())
    return cert;

  std::vector<std::string> oldSegs;
  oldSegs.reserve(runSpans.size());

  std::optional<uint32_t> prevEnd;
  for (const auto *ps : runSpans) {
    if (!ps || !ps->byteBegin || !ps->byteEnd || *ps->byteEnd < *ps->byteBegin)
      return cert;
    if (static_cast<size_t>(*ps->byteEnd) > aTok.size())
      return cert;
    if (prevEnd && *prevEnd != *ps->byteBegin)
      return cert;

    StringRef oldSeg =
        aTok.substr(static_cast<size_t>(*ps->byteBegin),
                    static_cast<size_t>(*ps->byteEnd - *ps->byteBegin));
    oldSegs.push_back(oldSeg.str());
    prevEnd = *ps->byteEnd;
  }

  using MemoKey = std::tuple<size_t, size_t, bool>;
  std::map<MemoKey, PasteRunInvertibilityCertificate> memo;

  // A contiguous ## run is invertible when its edited B spelling can be split
  // into per-segment spellings such that every changed segment is isolated by
  // unchanged neighbors (or a run edge). Adjacent edited segments in the same
  // undelimited run are treated as ambiguous because no internal anchor pins
  // their boundary.
  auto solveRun = [&](auto &&self, size_t idx, size_t pos,
                      bool prevChanged) -> PasteRunInvertibilityCertificate {
    MemoKey key{idx, pos, prevChanged};
    auto it = memo.find(key);
    if (it != memo.end())
      return it->second;

    PasteRunInvertibilityCertificate result;
    result.kind = PasteRunInvertibilityKind::NoMatch;

    if (pos > bRun.size()) {
      memo.emplace(key, result);
      return result;
    }

    if (idx == oldSegs.size()) {
      if (pos == bRun.size())
        result.kind = PasteRunInvertibilityKind::Unique;
      memo.emplace(key, result);
      return result;
    }

    StringRef oldSeg = oldSegs[idx];
    StringRef rest = bRun.drop_front(pos);

    if (rest.starts_with(oldSeg)) {
      PasteRunInvertibilityCertificate unchanged = prependDerivedSegment(
          self(self, idx + 1, pos + oldSeg.size(), /*prevChanged=*/false),
          oldSeg);
      result = mergePasteRunCertificates(std::move(result), unchanged);
      if (result.kind == PasteRunInvertibilityKind::Unsupported ||
          result.kind == PasteRunInvertibilityKind::Ambiguous) {
        memo.emplace(key, result);
        return result;
      }
    }

    if (prevChanged) {
      memo.emplace(key, result);
      return result;
    }

    // Current segment edited. The next segment, if any, must remain unchanged
    // and therefore acts as the first available anchor for the edited piece.
    if (idx + 1 == oldSegs.size()) {
      PasteRunInvertibilityCertificate changed;
      changed.kind = PasteRunInvertibilityKind::Unique;
      changed.derivedSegs.push_back(rest.str());
      result = mergePasteRunCertificates(std::move(result), changed);
      memo.emplace(key, result);
      return result;
    }

    StringRef nextAnchor = oldSegs[idx + 1];
    if (nextAnchor.empty()) {
      result.kind = PasteRunInvertibilityKind::Unsupported;
      result.derivedSegs.clear();
      memo.emplace(key, result);
      return result;
    }

    for (size_t searchPos = 0;; ++searchPos) {
      size_t found = rest.find(nextAnchor, searchPos);
      if (found == StringRef::npos)
        break;

      PasteRunInvertibilityCertificate changed = prependDerivedSegment(
          self(self, idx + 1, pos + found, /*prevChanged=*/true),
          rest.take_front(found));
      result = mergePasteRunCertificates(std::move(result), changed);
      if (result.kind == PasteRunInvertibilityKind::Unsupported ||
          result.kind == PasteRunInvertibilityKind::Ambiguous)
        break;
    }

    memo.emplace(key, result);
    return result;
  };

  return solveRun(solveRun, 0, 0, /*prevChanged=*/false);
}

/// Classification for replaying an edited pasted token through producer-side
/// `paste_tokens` metadata.
///
/// `Unique` means the edited token has exactly one segmentation compatible
/// with the replay witness. `Ambiguous` means multiple different segmentations
/// are possible and the consumer must fail closed. `NoMatch` means this witness
/// cannot explain the edited token. `Unsupported` denotes malformed or
/// currently out-of-domain witness shapes.
enum class PasteReplaySegmentationKind {
  Unique,
  NoMatch,
  Ambiguous,
  Unsupported
};

/// Result of replaying one edited pasted-token spelling through one original
/// replay witness. For a successful replay, `argSegments` contains replacement
/// text for argument-derived parts in witness order; literal parts are omitted
/// because they must match the edited token exactly.
struct PasteReplaySegmentationResult {
  PasteReplaySegmentationKind kind = PasteReplaySegmentationKind::Unsupported;
  std::vector<std::string> argSegments;
};

/// Merge two replay attempts for the same pasted token. If two distinct
/// successful segmentations exist, preserving the paste expression would invent
/// source structure, so the merged result is ambiguous.
PasteReplaySegmentationResult
mergePasteReplayResults(PasteReplaySegmentationResult lhs,
                        const PasteReplaySegmentationResult &rhs) {
  if (lhs.kind == PasteReplaySegmentationKind::Unsupported ||
      rhs.kind == PasteReplaySegmentationKind::Unsupported) {
    lhs.kind = PasteReplaySegmentationKind::Unsupported;
    lhs.argSegments.clear();
    return lhs;
  }
  if (rhs.kind == PasteReplaySegmentationKind::NoMatch)
    return lhs;
  if (lhs.kind == PasteReplaySegmentationKind::NoMatch)
    return rhs;
  if (lhs.kind == PasteReplaySegmentationKind::Ambiguous ||
      rhs.kind == PasteReplaySegmentationKind::Ambiguous) {
    lhs.kind = PasteReplaySegmentationKind::Ambiguous;
    lhs.argSegments.clear();
    return lhs;
  }
  if (lhs.argSegments == rhs.argSegments)
    return lhs;
  lhs.kind = PasteReplaySegmentationKind::Ambiguous;
  lhs.argSegments.clear();
  return lhs;
}

/// Return the `paste_tokens[]` witness corresponding to a paste span.
///
/// `paste_spans[]` contains one entry per argument contribution, while
/// `paste_tokens[]` contains one entry per final pasted token. The producer
/// serializes both in token order, so this helper reconstructs the distinct
/// `(begin,end)` paste-token order from `paste_spans[]` and uses the same
/// ordinal to find the replay witness. The spelling check prevents accidental
/// matches if the metadata is malformed or from a different schema generation.
const RefoldModel::PasteToken *
findPasteTokenWitnessForSpan(const RefoldModel::MacroInvocation &m,
                             const RefoldModel::PPArgSpan &span,
                             StringRef aTokSpelling) {
  if (m.pasteTokens.empty())
    return nullptr;

  // One pass assigns each distinct `(begin,end)` its producer token-order
  // ordinal.  The map keeps the first ordinal for a repeated range, which is
  // the same ordinal the previous append-if-absent list produced, but without
  // rescanning the accumulated list per paste span.
  SmallDenseMap<std::pair<uint64_t, uint64_t>, size_t, 8> ordinalByTokenRange;
  for (const auto &ps : m.pasteSpans)
    ordinalByTokenRange.try_emplace(
        std::pair<uint64_t, uint64_t>{ps.begin, ps.end},
        ordinalByTokenRange.size());

  auto it = ordinalByTokenRange.find(
      std::pair<uint64_t, uint64_t>{span.begin, span.end});
  if (it == ordinalByTokenRange.end())
    return nullptr;
  const size_t index = it->second;
  if (index >= m.pasteTokens.size())
    return nullptr;

  const RefoldModel::PasteToken &witness = m.pasteTokens[index];
  if (witness.spelling != aTokSpelling)
    return nullptr;
  return &witness;
}

/// Segment a delimiter-free run of adjacent argument-derived paste parts.
///
/// When adjacent argument parts have no literal bytes between them, the edited
/// token spelling contains no internal boundary marker. The only accepted rule
/// here is shape preservation: the edited run must have the same total width as
/// the original run and is split by the original part widths. For example,
/// `ab | cd` may replay `wxyz` as `wx | yz`; a different total width remains
/// out of domain unless literal anchors elsewhere pin the boundaries.
PasteReplaySegmentationResult
segmentArgRunByReplayWidths(StringRef bRun,
                            ArrayRef<RefoldModel::PastePart> parts) {
  PasteReplaySegmentationResult result;
  if (parts.empty())
    return result;

  if (parts.size() == 1) {
    result.kind = PasteReplaySegmentationKind::Unique;
    result.argSegments.push_back(bRun.str());
    return result;
  }

  size_t requiredLen = 0;
  for (const auto &part : parts) {
    if (part.kind != RefoldModel::PastePartKind::Arg ||
        part.byteEnd < part.byteBegin)
      return result;
    requiredLen += static_cast<size_t>(part.byteEnd - part.byteBegin);
  }
  if (requiredLen != bRun.size()) {
    result.kind = PasteReplaySegmentationKind::NoMatch;
    return result;
  }

  result.kind = PasteReplaySegmentationKind::Unique;
  size_t pos = 0;
  for (const auto &part : parts) {
    const size_t width = static_cast<size_t>(part.byteEnd - part.byteBegin);
    result.argSegments.push_back(bRun.substr(pos, width).str());
    pos += width;
  }
  return result;
}

/// Replay an edited pasted-token spelling through the producer witness.
///
/// Literal witness parts are exact anchors and must occur unchanged in B. Runs
/// of argument parts are segmented by `segmentArgRunByReplayWidths()`. If a
/// literal anchor can be placed in more than one way and those placements imply
/// different argument segments, the result is ambiguous and rejected by the
/// caller.
PasteReplaySegmentationResult
segmentPastedTokenByReplayWitness(StringRef bTokSpelling,
                                  const RefoldModel::PasteToken &witness) {
  PasteReplaySegmentationResult unsupported;
  if (witness.parts.empty())
    return unsupported;

  uint32_t expectedBegin = 0;
  for (const auto &part : witness.parts) {
    if (part.byteBegin != expectedBegin || part.byteEnd < part.byteBegin ||
        part.byteEnd > witness.spelling.size())
      return unsupported;
    if (part.spelling != witness.spelling.slice(part.byteBegin, part.byteEnd))
      return unsupported;
    if (part.kind == RefoldModel::PastePartKind::Arg && !part.argIndex)
      return unsupported;
    if (part.kind == RefoldModel::PastePartKind::Literal && part.argIndex)
      return unsupported;
    expectedBegin = part.byteEnd;
  }
  if (expectedBegin != witness.spelling.size())
    return unsupported;

  using MemoKey = std::pair<size_t, size_t>;
  std::map<MemoKey, PasteReplaySegmentationResult> memo;

  // Recursively prove a unique segmentation of BTokSpelling against the paste
  // witness. Literal parts are fixed anchors; maximal runs of argument parts
  // are segmented by replay-width evidence. The solver accepts only a unique
  // full consumption of BTokSpelling and reports ambiguity instead of choosing
  // among multiple possible anchor occurrences.
  auto solve = [&](auto &&self, size_t partIdx,
                   size_t posB) -> PasteReplaySegmentationResult {
    // State is defined by where we are in the paste-part stream and where we
    // are in the rewritten pasted-token spelling. Memoization prevents
    // repeated rescans when the same anchor occurrence can be reached through
    // multiple earlier splits.
    MemoKey key{partIdx, posB};
    auto memoIt = memo.find(key);
    if (memoIt != memo.end())
      return memoIt->second;

    PasteReplaySegmentationResult result;
    result.kind = PasteReplaySegmentationKind::NoMatch;

    // A split that has already consumed past the rewritten spelling cannot be
    // valid. Cache the negative result so later anchor searches fail cheaply.
    if (posB > bTokSpelling.size()) {
      memo.emplace(key, result);
      return result;
    }

    // If all paste parts were consumed, the replay is valid only if it consumed
    // the entire rewritten pasted-token spelling. Otherwise this path matched
    // only a prefix and must be rejected.
    if (partIdx == witness.parts.size()) {
      if (posB == bTokSpelling.size())
        result.kind = PasteReplaySegmentationKind::Unique;
      memo.emplace(key, result);
      return result;
    }

    const RefoldModel::PastePart &part = witness.parts[partIdx];

    // Literal paste pieces are fixed anchors from the original paste
    // expression. They must appear verbatim at the current B spelling position.
    if (part.kind == RefoldModel::PastePartKind::Literal) {
      if (!bTokSpelling.substr(posB).starts_with(part.spelling)) {
        memo.emplace(key, result);
        return result;
      }

      // After consuming the literal, continue with the next paste part.
      result = self(self, partIdx + 1, posB + part.spelling.size());
      memo.emplace(key, result);
      return result;
    }

    // We are at an argument-derived paste part. Collapse the maximal consecu-
    // tive consecutive run of argument parts and segment that whole run as one
    // unit. This avoids independently guessing byte cuts between adjacent
    // pasted arguments.
    size_t runEnd = partIdx;
    while (runEnd < witness.parts.size() &&
           witness.parts[runEnd].kind == RefoldModel::PastePartKind::Arg) {
      ++runEnd;
    }

    auto tryRun = [&](size_t runEndB) -> PasteReplaySegmentationResult {
      // The candidate B interval for this argument run must be a valid slice of
      // the rewritten pasted-token spelling.
      if (runEndB < posB || runEndB > bTokSpelling.size())
        return PasteReplaySegmentationResult{};

      ArrayRef<RefoldModel::PastePart> witnessParts(witness.parts);

      // Segment the rewritten B substring across the consecutive argument parts
      // using the replay-width information recorded in the paste witness. A
      // non-unique segmentation is propagated upward so the caller can fail
      // closed rather than choosing an arbitrary split.
      auto runSeg = segmentArgRunByReplayWidths(
          bTokSpelling.substr(posB, runEndB - posB),
          witnessParts.slice(partIdx, runEnd - partIdx));

      if (runSeg.kind != PasteReplaySegmentationKind::Unique)
        return runSeg;

      // The argument run itself was uniquely segmented; now verify that the
      // remaining paste parts uniquely consume the suffix after this run.
      auto suffix = self(self, runEnd, runEndB);
      if (suffix.kind != PasteReplaySegmentationKind::Unique)
        return suffix;

      // Preserve argument segments in paste-order: current run first, then the
      // recursively validated suffix.
      runSeg.argSegments.insert(runSeg.argSegments.end(),
                                suffix.argSegments.begin(),
                                suffix.argSegments.end());
      return runSeg;
    };

    // If the paste expression ends with this argument run, the run must consume
    // the rest of the rewritten pasted-token spelling.
    if (runEnd == witness.parts.size()) {
      result = tryRun(bTokSpelling.size());
      memo.emplace(key, result);
      return result;
    }

    // Otherwise, the next literal part is used as a concrete anchor. Each
    // occurrence of that literal in the remaining B spelling defines one
    // possible endpoint for the current argument run.
    StringRef anchor = witness.parts[runEnd].spelling;
    if (anchor.empty()) {
      // Empty literal anchors do not constrain the split, so they cannot
      // provide a deterministic replay boundary.
      result.kind = PasteReplaySegmentationKind::Unsupported;
      memo.emplace(key, result);
      return result;
    }

    for (size_t found = bTokSpelling.find(anchor, posB);
         found != StringRef::npos;
         found = bTokSpelling.find(anchor, found + 1)) {
      // Try treating this anchor occurrence as the end of the argument run.
      // Multiple successful segmentations are merged into Ambiguous rather than
      // resolved heuristically.
      result = mergePasteReplayResults(std::move(result), tryRun(found));

      // Unsupported and ambiguous outcomes are terminal for this state: later
      // anchor occurrences cannot restore the uniqueness proof.
      if (result.kind == PasteReplaySegmentationKind::Unsupported ||
          result.kind == PasteReplaySegmentationKind::Ambiguous)
        break;
    }

    memo.emplace(key, result);
    return result;
  };

  return solve(solve, /*partIdx=*/0, /*posB=*/0);
}

} // namespace

bool RefoldMacroPasteArgumentBuilder::HunkTouchesAnyPasteToken(
    const RefoldModel::MacroInvocation &m, const diffutils::Hunk &h) {
  if (m.pasteSpans.empty())
    return false;

  const uint64_t a0 = h.aStart;
  const uint64_t a1 = h.aEnd;

  // Insertion hunk: treat as touching if the insertion point lies "on" a paste
  // span boundary.
  if (a0 == a1) {
    for (const auto &s : m.pasteSpans) {
      if (a0 >= s.begin && a0 <= s.end)
        return true;
    }
    return false;
  }

  // Replacement/deletion hunk: interval intersection between [a0, a1) and
  // [s.begin, s.end).
  for (const auto &s : m.pasteSpans) {
    if (a0 < s.end && a1 > s.begin)
      return true;
  }

  return false;
}

std::optional<PasteArgEdit> RefoldMacroPasteArgumentBuilder::DerivePasteArgEdit(
    const RefoldModel::MacroInvocation &m, const diffutils::Hunk &h) const {
  // This helper only applies when the producer reported paste spans.
  if (m.pasteSpans.empty())
    return std::nullopt;

  // Collect all paste span occurrences that intersect the hunk in A-token
  // space.
  //
  // Each PPArgSpan in pasteSpans corresponds to one argument's contribution to
  // a single pasted token emitted in A_PP. Multiple arguments may contribute
  // disjoint (or adjacent) byte segments inside the same pasted token, and the
  // hunk may touch one of those segments.
  std::vector<const RefoldModel::PPArgSpan *> cands;
  for (const auto &ps : m.pasteSpans) {
    if (ps.begin < h.aEnd && h.aStart < ps.end)
      cands.push_back(&ps);
  }

  if (cands.empty())
    return std::nullopt;

  // All candidates must refer to the same pasted-token occurrence. In practice,
  // each candidate has the same [begin,end) A-token envelope (the pasted
  // token), but different argIdx and [byteBegin,byteEnd) describing which byte
  // subrange of the pasted token came from that argument.
  const auto *tokenSpan = cands[0];

  // Map the pasted token envelope in A to its corresponding envelope in B. For
  // paste edits we require a strict mapping: the A pasted token must map to
  // exactly one B token that we will diff against.
  auto bEnvOpt =
      (*deps_.sourceMapper).MapAToBTokenEnvelopeByPPArgSpan(*tokenSpan);
  if (!bEnvOpt || bEnvOpt->second <= bEnvOpt->first)
    return std::nullopt;

  // Paste-aware edits handled here must stay within a single B token. If the
  // pasted token turned into multiple tokens in B, the edit is not a pure
  // within-token paste segment rewrite, so bail out.
  if (bEnvOpt->second - bEnvOpt->first != 1)
    return std::nullopt;

  // Grab the raw token spellings for the pasted token in A and B.
  // tokenSpan.begin/end are A-token indices; bEnv[0]/bEnv[1] are B-token
  // indices.
  StringRef aTokRaw =
      (*deps_.sourceMapper).SliceASource(tokenSpan->begin, tokenSpan->end);
  StringRef bTokRaw =
      (*deps_.sourceMapper).SliceBSource(bEnvOpt->first, bEnvOpt->second);

  // Strip trailing newlines to stabilize within-token diffs.
  StringRef aTok = stringutils::stripTrailingNewlines(aTokRaw);
  StringRef bTok = stringutils::stripTrailingNewlines(bTokRaw);

  // Compute the minimal differing region between the two token spellings:
  // aTok = [common prefix][DIFF_A][common suffix]
  // bTok = [common prefix][DIFF_B][common suffix]
  size_t pref = 0;
  size_t minLen = std::min(aTok.size(), bTok.size());
  while (pref < minLen && aTok[pref] == bTok[pref]) {
    pref++;
  }

  size_t aLen = aTok.size();
  size_t bLen = bTok.size();
  size_t suff = 0;

  // While we haven't reached the prefix on either side
  // and the characters from the back match...
  while (suff < (aLen - pref) && suff < (bLen - pref) &&
         aTok[aLen - 1 - suff] == bTok[bLen - 1 - suff]) {
    suff++;
  }

  const size_t diffStart = pref;
  const size_t diffEndA = aLen - suff;

  // If there is no difference at all, this hunk cannot be explained as a
  // paste-segment rewrite.
  if (diffStart >= diffEndA && aTok.size() == bTok.size())
    return std::nullopt;

  // Now choose exactly one candidate argument contribution whose
  // [byteBegin,byteEnd) overlaps the differing region. The producer provided
  // byteBegin/byteEnd in pasted-token text coordinates.
  //
  // We require the edit to be attributable to a single argument slice. If
  // multiple slices overlap the diff region, we cannot express it as a
  // single-arg args-only rewrite.
  const RefoldModel::PPArgSpan *chosen = nullptr;
  for (const auto *ps : cands) {
    if (!ps->byteBegin || !ps->byteEnd || *ps->byteEnd < *ps->byteBegin)
      continue;

    const size_t bBegin = *ps->byteBegin;
    const size_t bEnd = *ps->byteEnd;

    bool hit = false;
    if (diffStart == diffEndA) {
      // Pure insertion/deletion at a point (no width in A). Treat as
      // overlapping if the point lies strictly inside the candidate slice.
      hit = (bBegin <= diffStart) && (diffStart < bEnd);
    } else {
      // General overlap between [diffStart,diffEndA) and
      // [ps.byteBegin,ps.byteEnd).
      const size_t lo = std::max(bBegin, diffStart);
      const size_t hi = std::min(bEnd, diffEndA);
      hit = (hi > lo);
    }

    if (hit) {
      if (!chosen)
        chosen = ps;
      else
        return std::nullopt; // Overlaps multiple args.
    }
  }

  if (!chosen)
    return std::nullopt;

  // Extract the old contributed segment from the A pasted token.
  const int64_t bbA = static_cast<int64_t>(*chosen->byteBegin);
  const int64_t beA = static_cast<int64_t>(*chosen->byteEnd);

  if (bbA < 0 || beA < bbA || static_cast<uint64_t>(beA) > aTok.size())
    return std::nullopt;

  // Compute the corresponding segment coordinates in the B pasted token.
  //
  // We assume the token-level edit does not permute the contribution
  // boundaries; instead, the chosen segment grows/shrinks by the overall token
  // length delta (bTokLen - aTokLen). This allows us to map [bb,be) in A to
  // [bb,be+delta) in B.
  const int64_t delta =
      static_cast<int64_t>(bTok.size()) - static_cast<int64_t>(aTok.size());
  const int64_t bbB_signed = bbA; // Assumption: prefix is stable
  const int64_t beB_signed = beA + delta;
  if (bbB_signed < 0 || beB_signed < bbB_signed ||
      static_cast<uint64_t>(beB_signed) > bTok.size())
    return std::nullopt;

  const size_t bb = static_cast<size_t>(bbA);
  const size_t be = static_cast<size_t>(beA);
  const size_t bbB = static_cast<size_t>(bbB_signed);
  const size_t beB = static_cast<size_t>(beB_signed);

  // Safety gate: ensure the only edits to the pasted token are within the
  // chosen segment.
  //
  // This requires both:
  // - the prefix before bb matches exactly
  // - the suffix after be matches exactly (after shifting by delta in B)
  if (aTok.substr(0, bb) != bTok.substr(0, bbB))
    return std::nullopt;

  if (aTok.substr(be) != bTok.substr(beB))
    return std::nullopt;

  std::string oldSeg = aTok.substr(bb, be - bb).str();
  std::string newSeg = bTok.substr(bbB, beB - bbB).str();

  return PasteArgEdit(chosen->argIdx, std::move(newSeg), std::move(oldSeg));
}

std::optional<std::vector<PasteArgEdit>>
RefoldMacroPasteArgumentBuilder::DerivePasteArgEdits(
    const RefoldModel::MacroInvocation &m, const diffutils::Hunk &h) const {
  if (m.pasteSpans.empty())
    return std::nullopt;

  // Gather all paste spans that intersect this hunk in the A-stream.
  std::vector<const RefoldModel::PPArgSpan *> cands;
  for (const auto &ps : m.pasteSpans) {
    if (ps.begin < h.aEnd && h.aStart < ps.end)
      cands.push_back(&ps);
  }

  if (cands.empty())
    return std::nullopt;

  // All candidates should reference the same pasted token range [begin, end) in
  // A.
  const auto *tokenSpan = cands[0];

  auto bEnvOpt =
      (*deps_.sourceMapper).MapAToBTokenEnvelopeByPPArgSpan(*tokenSpan);
  if (!bEnvOpt || bEnvOpt->second <= bEnvOpt->first)
    return std::nullopt;

  // Paste edits are only representable as args-only when the A-span maps to
  // exactly one B token.
  if (bEnvOpt->second - bEnvOpt->first != 1)
    return std::nullopt;

  StringRef aTokRaw =
      (*deps_.sourceMapper).SliceASource(tokenSpan->begin, tokenSpan->end);
  StringRef bTokRaw =
      (*deps_.sourceMapper).SliceBSource(bEnvOpt->first, bEnvOpt->second);

  // Strip trailing newlines to stabilize within-token diffs for the
  // legacy source-slice segmenter. The replay-witness path below uses lexer
  // token spellings instead, because `Slice*Source()` may include trailing
  // whitespace between adjacent PP tokens.
  StringRef aTok = stringutils::stripTrailingNewlines(aTokRaw);
  StringRef bTok = stringutils::stripTrailingNewlines(bTokRaw);

  // Prefer the producer's exact paste replay witness when it is present. The
  // witness gives the original ordered arg/literal decomposition of the pasted
  // token, so we can refold adjacent changed paste parts without inventing a
  // split from raw text alone. For delimiter-free adjacent arg runs, the only
  // accepted policy is shape preservation: the edited run must split by the
  // original part widths. All literal parts remain exact anchors.
  if (tokenSpan->end == tokenSpan->begin + 1 &&
      bEnvOpt->second == bEnvOpt->first + 1 &&
      tokenSpan->begin < deps_.aToks.size() &&
      bEnvOpt->first < deps_.bToks.size()) {
    StringRef aTokSpelling = deps_.aToks[tokenSpan->begin].spelling;
    StringRef bTokSpelling = deps_.bToks[bEnvOpt->first].spelling;
    if (const auto *witness =
            findPasteTokenWitnessForSpan(m, *tokenSpan, aTokSpelling)) {
      auto replay = segmentPastedTokenByReplayWitness(bTokSpelling, *witness);
      if (replay.kind == PasteReplaySegmentationKind::Unique) {
        std::vector<PasteArgEdit> edits;
        size_t argPartIdx = 0;
        for (const auto &part : witness->parts) {
          if (part.kind != RefoldModel::PastePartKind::Arg)
            continue;
          if (!part.argIndex || argPartIdx >= replay.argSegments.size())
            return std::nullopt;
          StringRef oldSeg = part.spelling;
          const std::string &newSeg = replay.argSegments[argPartIdx++];
          if (oldSeg == newSeg)
            continue;
          edits.emplace_back(*part.argIndex, newSeg, oldSeg.str(),
                             part.argByteBegin, part.argByteEnd);
        }
        if (argPartIdx != replay.argSegments.size())
          return std::nullopt;
        if (!edits.empty()) {
          return edits;
        }
        return std::nullopt;
      }
      if (replay.kind == PasteReplaySegmentationKind::Ambiguous ||
          replay.kind == PasteReplaySegmentationKind::Unsupported)
        return std::nullopt;
    }
  }

  // Multi-span paste edits may change the overall pasted token length (e.g.,
  // a_b_c -> foo_bar_baz). This is still safe to refold *as long as* the
  // non-arg "fixed" slices of the pasted token remain unchanged, and we can
  // deterministically segment the B token into the per-arg regions.
  //
  // We derive the new per-arg segments by walking the A token left-to-right and
  // using the fixed (non-span) substrings between paste spans as anchors. If
  // spans are adjacent (no fixed anchor) and the total length changes,
  // segmentation is ambiguous and we conservatively return std::nullopt.
  std::vector<const RefoldModel::PPArgSpan *> spans = cands;
  std::sort(spans.begin(), spans.end(), ppArgSpanPtrLessByByteBegin);

  std::optional<std::vector<std::string>> newSegs =
      SegmentPastedTokenArgsByFixedSlices(aTok, bTok, spans);
  if (!newSegs)
    return std::nullopt;

  std::vector<PasteArgEdit> edits;

  // Compare each projected argument segment that contributed to the pasted A
  // token against its derived replacement segment in B, and record only the
  // argument-local edits whose projected text actually changed.
  for (size_t i = 0; i < spans.size(); ++i) {
    const auto *ps = spans[i];
    if (!ps->byteBegin || *ps->byteEnd < *ps->byteBegin)
      return std::nullopt;

    size_t bb = static_cast<size_t>(*ps->byteBegin);
    size_t be = static_cast<size_t>(*ps->byteEnd);
    if (be > aTok.size())
      return std::nullopt;

    StringRef oldSeg = aTok.substr(bb, be - bb);
    const std::string &newSeg = (*newSegs)[i];

    if (oldSeg == newSeg)
      continue;

    // Note: the same argument may contribute multiple segments to the same
    // pasted token (e.g. X##_..._##X). We allow repeated argIdx here and let
    // the caller merge implied argument replacements conservatively.
    edits.emplace_back(ps->argIdx, newSeg, oldSeg.str());
  }

  if (edits.empty())
    return std::nullopt;

  return edits;
}

std::optional<std::vector<std::string>>
RefoldMacroPasteArgumentBuilder::SegmentPastedTokenArgsByFixedSlices(
    StringRef aTok, StringRef bTok,
    ArrayRef<const RefoldModel::PPArgSpan *> spansAsc) {
  if (spansAsc.empty())
    return std::nullopt;

  // Basic span sanity
  for (const auto *ps : spansAsc) {
    if (!ps->byteBegin || *ps->byteEnd < *ps->byteBegin)
      return std::nullopt;
    if (static_cast<size_t>(*ps->byteEnd) > aTok.size())
      return std::nullopt;
  }

  using MemoKey = std::pair<size_t, size_t>;
  std::map<MemoKey, std::optional<std::vector<std::string>>> memo;

  // Recursively invert the rewritten B token against the original A token and
  // the ordered paste-span list. Fixed A-token text between spans acts as an
  // anchor; each contiguous run of adjacent paste spans is mapped to the
  // corresponding B substring. The result is the ordered list of derived
  // argument segment spellings, or nullopt if the A/B token pair cannot be
  // uniquely replayed.
  auto solve = [&](auto &&self, size_t idx,
                   size_t posB) -> std::optional<std::vector<std::string>> {
    // State is defined by the next paste span to consume and the current byte
    // position in the rewritten B token. Memoization prevents repeated anchor
    // searches from re-solving the same suffix.
    MemoKey key{idx, posB};
    auto it = memo.find(key);
    if (it != memo.end())
      return it->second;

    // `posA` is the byte position in the original A token immediately after the
    // previous consumed paste span. The fixed text from `posA` to the next span
    // must still appear verbatim in B.
    size_t posA = 0;
    if (idx > 0) {
      if (!spansAsc[idx - 1]->byteEnd) {
        memo.emplace(key, std::nullopt);
        return std::nullopt;
      }
      posA = static_cast<size_t>(*spansAsc[idx - 1]->byteEnd);
    }

    // Base case: all paste spans were consumed. The remaining B suffix must
    // exactly match the remaining fixed A suffix.
    if (idx >= spansAsc.size()) {
      StringRef tail = aTok.substr(posA);
      std::optional<std::vector<std::string>> result =
          (bTok.substr(posB) == tail) ? std::optional<std::vector<std::string>>(
                                            std::vector<std::string>())
                                      : std::nullopt;
      memo.emplace(key, result);
      return result;
    }

    const auto *ps = spansAsc[idx];

    // Each paste span must provide a valid byte interval inside the original A
    // token. Without that interval there is no proof-grade way to align the
    // fixed A text and the editable paste contribution.
    if (!ps->byteBegin || !ps->byteEnd || *ps->byteEnd < *ps->byteBegin) {
      memo.emplace(key, std::nullopt);
      return std::nullopt;
    }

    const size_t bA = static_cast<size_t>(*ps->byteBegin);
    const size_t eA = static_cast<size_t>(*ps->byteEnd);

    // Spans must be processed in non-overlapping ascending order. Overlap would
    // make the fixed/paste decomposition ambiguous.
    if (bA < posA) {
      memo.emplace(key, std::nullopt);
      return std::nullopt;
    }

    // The fixed A text before this paste span anchors the next B position.
    StringRef fixedBefore = aTok.substr(posA, bA - posA);
    if (!bTok.substr(posB).starts_with(fixedBefore)) {
      memo.emplace(key, std::nullopt);
      return std::nullopt;
    }

    // The paste-derived B run begins immediately after the fixed prefix.
    const size_t runStartB = posB + fixedBefore.size();

    // Coalesce adjacent paste spans with no fixed A text between them. Such
    // spans must be segmented as a single run; otherwise we would invent
    // arbitrary byte cuts between adjacent pasted argument contributions.
    size_t runEnd = idx;
    size_t nextPosA = eA;
    StringRef fixedAfter;
    while (true) {
      if (runEnd + 1 >= spansAsc.size()) {
        // No more spans: the remaining A-token suffix is the anchor after this
        // final paste run.
        fixedAfter = aTok.substr(nextPosA);
        break;
      }

      const auto *cur = spansAsc[runEnd];
      const auto *next = spansAsc[runEnd + 1];

      // Adjacent-run discovery requires ordered, non-overlapping span
      // intervals.
      if (!cur->byteEnd || !next->byteBegin ||
          *next->byteBegin < *cur->byteEnd) {
        memo.emplace(key, std::nullopt);
        return std::nullopt;
      }

      // A non-empty fixed gap terminates the run and becomes the next B-side
      // anchor used to find possible endpoints for the current paste run.
      StringRef gap =
          aTok.substr(static_cast<size_t>(*cur->byteEnd),
                      static_cast<size_t>(*next->byteBegin - *cur->byteEnd));
      if (!gap.empty()) {
        fixedAfter = gap;
        break;
      }

      // Empty fixed gap: the next span is adjacent to this run and must be
      // segmented together with it.
      ++runEnd;
      if (!spansAsc[runEnd]->byteEnd) {
        memo.emplace(key, std::nullopt);
        return std::nullopt;
      }
      nextPosA = static_cast<size_t>(*spansAsc[runEnd]->byteEnd);
    }

    auto tryRun =
        [&](size_t runEndB) -> std::optional<std::vector<std::string>> {
      // Candidate endpoint for the current paste run must define a valid B
      // slice.
      if (runEndB < runStartB || runEndB > bTok.size())
        return std::nullopt;

      // First try the proof-grade adjacent-run certificate. It verifies that
      // the B substring can be uniquely decomposed according to the paste-span
      // run.
      auto cert = buildAdjacentPasteRunInvertibilityCertificate(
          aTok, bTok.substr(runStartB, runEndB - runStartB),
          spansAsc.slice(idx, runEnd - idx + 1));
      if (cert.kind == PasteRunInvertibilityKind::Unique &&
          cert.derivedSegs.size() == runEnd - idx + 1) {
        // The current run is uniquely invertible; now require the suffix after
        // the run to be invertible as well.
        if (auto suffix = self(self, runEnd + 1, runEndB)) {
          std::vector<std::string> combined = cert.derivedSegs;
          combined.insert(combined.end(), suffix->begin(), suffix->end());
          return combined;
        }
      }

      // Legacy single-span fallback: when the current run contains only one
      // argument contribution, the fixed slices on either side already pin the
      // segment boundary. The newer adjacent-run certificate is stricter, but
      // some pure-paste cases (e.g. CONCAT-style token assembly) are still
      // structurally invertible via this simpler anchor-based split.
      if (runEnd == idx) {
        if (auto suffix = self(self, idx + 1, runEndB)) {
          std::vector<std::string> combined;
          combined.reserve(1 + suffix->size());
          combined.push_back(bTok.substr(runStartB, runEndB - runStartB).str());
          combined.insert(combined.end(), suffix->begin(), suffix->end());
          return combined;
        }
      }

      return std::nullopt;
    };

    // If there is no fixed text after the run, the paste run must consume the
    // remainder of the rewritten B token.
    if (fixedAfter.empty()) {
      auto result = tryRun(bTok.size());
      memo.emplace(key, result);
      return result;
    }

    // Otherwise, every occurrence of the next fixed A anchor in B is a
    // candidate endpoint for the paste run. Accept the first endpoint whose
    // run and suffix both replay successfully.
    for (size_t k = bTok.find(fixedAfter, runStartB); k != StringRef::npos;
         k = bTok.find(fixedAfter, k + 1)) {
      if (auto result = tryRun(k)) {
        memo.emplace(key, result);
        return result;
      }
    }

    // No anchor position produced a valid replay.
    memo.emplace(key, std::nullopt);
    return std::nullopt;
  };

  return solve(solve, /*idx=*/0, /*posB=*/0);
}

StringRef
RefoldMacroPasteArgumentBuilder::DeriveNewPasteSegmentFromSpellingReplacement(
    StringRef baseArg, StringRef newArg, StringRef oldSeg) {
  // Trim all inputs.
  baseArg = baseArg.trim();
  newArg = newArg.trim();
  oldSeg = oldSeg.trim();

  // If the segment is the entire argument, the replacement is the entire new
  // argument.
  if (oldSeg == baseArg)
    return newArg;

  // Case 1: oldSeg is a prefix of baseArg.
  // Example: base="foo_v1", oldSeg="foo_", new="bar_v1" -> returns "bar_"
  if (baseArg.starts_with(oldSeg)) {
    StringRef suffix = baseArg.substr(oldSeg.size());
    if (!newArg.ends_with(suffix)) {
      // Special case: when the pasted segment is the macro-name token of a raw
      // invocation argument used under ##, edits to the *invocation arguments*
      // do not change the contributed pasted segment. For example:
      //   baseArg = XCAT(pre_,int)
      //   newArg  = XCAT(pre_,long)
      //   oldSeg  = XCAT
      // In that situation the spelled segment participating in ## remains the
      // raw callee token "XCAT". Preserve the old segment rather than trying
      // to derive it from the full invocation text.
      size_t oldLParen = baseArg.find('(');
      size_t newLParen = newArg.find('(');
      if (oldLParen != StringRef::npos && newLParen != StringRef::npos &&
          oldSeg == baseArg.substr(0, oldLParen) &&
          oldSeg == newArg.substr(0, newLParen)) {
        return oldSeg;
      }
      return StringRef();
    }

    // Return the part of newArg that precedes the suffix.
    StringRef result = newArg.substr(0, newArg.size() - suffix.size());
    return result;
  }

  // Case 2: oldSeg is a suffix of baseArg.
  // Example: base="v1_foo", oldSeg="_foo", new="v1_bar" -> returns "_bar"
  if (baseArg.ends_with(oldSeg)) {
    StringRef prefix = baseArg.substr(0, baseArg.size() - oldSeg.size());
    if (!newArg.starts_with(prefix)) {
      return StringRef();
    }

    // Return the part of newArg that follows the prefix.
    StringRef result = newArg.substr(prefix.size());
    return result;
  }

  return StringRef();
}

bool RefoldMacroPasteArgumentBuilder::
    PasteArgReplacementsMatchAllPasteTokensInB(
        const RefoldModel::MacroInvocation &m, StringRef baseInvText,
        ArrayRef<std::pair<size_t, size_t>> invArgRanges,
        const DenseMap<uint32_t, std::string> &replByArgIdx) const {
  if (m.pasteSpans.empty())
    return true;

  // Recover original formal ranges through the model-aware helper.  It prefers
  // producer-owned inv_arg_ranges and uses lexical parsing only as a legacy or
  // edited-text fallback, so paste validation does not maintain a separate raw
  // invocation parser path.
  std::optional<std::vector<std::pair<size_t, size_t>>> origFormalRanges;
  if (m.invText) {
    RefoldMacroActualLayout actualLayout({deps_.lexLang});
    origFormalRanges =
        actualLayout.GetMacroInvocationFormalArgContentRanges(m, *m.invText);
  }

  auto getOrigArgTrim = [&](uint32_t argIdx) -> StringRef {
    if (!m.invText || !origFormalRanges || argIdx >= origFormalRanges->size())
      return StringRef();

    StringRef invText = *m.invText;
    size_t b = (*origFormalRanges)[argIdx].first;
    size_t e = (*origFormalRanges)[argIdx].second;
    if (b > e || e > invText.size())
      return StringRef();
    return invText.slice(b, e).trim();
  };

  // Precompute the original (base) spelling text for each argument we are
  // proposing to replace. We need this to derive a stable mapping from
  // "argument replacement" -> "paste segment update".
  DenseMap<uint32_t, std::string> baseArgByIdx;
  for (const auto &entry : replByArgIdx) {
    uint32_t argIdx = entry.first;
    if (static_cast<size_t>(argIdx) >= invArgRanges.size())
      return false;

    auto range = invArgRanges[argIdx];
    StringRef rawArg =
        baseInvText.substr(range.first, range.second - range.first);
    baseArgByIdx[argIdx] = rawArg.trim().str();
  }

  // Build a per-arg set of PP-byte envelopes for *standard* occurrences of the
  // current macro's formals. A paste span whose PP-byte envelope exactly
  // matches one of these ranges is not a direct paste contribution of the
  // current macro; it is a propagated nested-child paste inside a standard
  // occurrence of this formal and must be validated at the child level rather
  // than against the parent's raw arg replacement text.
  DenseMap<uint32_t, SmallVector<std::pair<uint64_t, uint64_t>, 4>>
      standardOccByteRangesByArg;
  for (const auto &occ : m.argSpans) {
    if (occ.kind != PPArgSpanKind::Standard || !occ.ppByteBegin ||
        !occ.ppByteEnd)
      continue;
    standardOccByteRangesByArg[occ.argIdx].push_back(
        {static_cast<uint64_t>(*occ.ppByteBegin),
         static_cast<uint64_t>(*occ.ppByteEnd)});
  }

  // Group paste spans by the specific pasted-token occurrence they contribute
  // to. The grouping key is the A token interval [beginTok,endTok) of the
  // pasted token. In practice, paste spans are expected to describe a single
  // token, so (endTok - beginTok) should be 1.
  std::vector<std::pair<uint64_t, uint64_t>> tokenOrder;
  DenseMap<std::pair<uint64_t, uint64_t>, std::vector<RefoldModel::PPArgSpan>>
      spansByTok;
  for (const auto &ps : m.pasteSpans) {
    std::pair<uint64_t, uint64_t> key = {ps.begin, ps.end};
    if (spansByTok.find(key) == spansByTok.end()) {
      tokenOrder.push_back(key);
    }
    spansByTok[key].push_back(ps);
  }

  // For each pasted-token occurrence, simulate applying the per-arg
  // replacements to its sub-token argument segments and compare against the
  // edited B token spelling.
  for (const auto &key : tokenOrder) {
    uint64_t beginTok = key.first;
    uint64_t endTok = key.second;

    // We only support pasted-token occurrences that correspond to exactly one
    // token in A.
    if (endTok != beginTok + 1)
      return false;

    // Map the A pasted-token occurrence to a single B token envelope.
    auto bEnv =
        (*deps_.sourceMapper).MapATokRangeAToBTokenEnvelope(beginTok, endTok);
    if (!bEnv || bEnv->second != bEnv->first + 1)
      return false;

    // Extract the pasted token text as produced in A and B. We strip newlines
    // defensively since slice helpers may include trailing '\n' depending on
    // how token ranges were formed.
    std::string aTok =
        stripNewlines((*deps_.sourceMapper).SliceASource(beginTok, endTok));
    std::string bTok = stripNewlines(
        (*deps_.sourceMapper).SliceBSource(bEnv->first, bEnv->second));

    // Paste spans for this token reference character slices inside the pasted
    // token spelling. Apply edits in descending byteBegin so earlier rewrites
    // do not shift later offsets.
    std::vector<RefoldModel::PPArgSpan> &spans = spansByTok[key];

    // Apply in descending byteBegin so replacements cannot shift the offsets
    // of later spans.
    std::sort(spans.begin(), spans.end(), ppArgSpanGreaterByByteBegin);

    // Start from the A token spelling and simulate the token-paste result after
    // applying the candidate arg replacements.
    std::string expected = aTok;
    bool sawCurrentLevelDirectSpan = false;
    for (const auto &ps : spans) {
      // Only apply span updates for arguments that we are actively replacing.
      auto it = replByArgIdx.find(ps.argIdx);
      if (it == replByArgIdx.end())
        continue;

      // If this paste span's PP-byte envelope exactly matches a *standard*
      // occurrence of the same formal in the current macro expansion, this
      // span is a propagated nested-child paste contributor rather than a
      // direct paste contribution of the current macro itself. Validate those
      // at the child level, not against the parent's raw arg replacement.
      if (ps.ppByteBegin && ps.ppByteEnd) {
        auto stdIt = standardOccByteRangesByArg.find(ps.argIdx);
        if (stdIt != standardOccByteRangesByArg.end()) {
          std::pair<uint64_t, uint64_t> spanBytes = {
              static_cast<uint64_t>(*ps.ppByteBegin),
              static_cast<uint64_t>(*ps.ppByteEnd)};
          bool matchesStandardOcc = false;
          for (const auto &r : stdIt->second) {
            if (r == spanBytes) {
              matchesStandardOcc = true;
              break;
            }
          }
          if (matchesStandardOcc) {
            continue;
          }
          sawCurrentLevelDirectSpan = true;
        } else {
          sawCurrentLevelDirectSpan = true;
        }
      } else {
        sawCurrentLevelDirectSpan = true;
      }

      StringRef newArg = it->second;

      // The segment derivation also needs the original spelling of the
      // argument.
      auto baseIt = baseArgByIdx.find(ps.argIdx);
      if (baseIt == baseArgByIdx.end())
        return false;
      StringRef baseArg = baseIt->second;

      if (!ps.byteBegin || !ps.byteEnd)
        return false;

      size_t b = static_cast<size_t>(*ps.byteBegin);
      size_t e = static_cast<size_t>(*ps.byteEnd);

      // The paste span must define a valid character slice inside the A
      // pasted-token spelling.
      if (e < b || e > aTok.size())
        return false;

      // Extract the original pasted-token segment contributed by this argument.
      StringRef oldSeg = StringRef(aTok).substr(b, e - b);

      // Derive the new pasted-token segment from the argument replacement. This
      // is intentionally conservative and must be deterministic; if we cannot
      // derive a segment safely, fail.
      StringRef newSeg =
          DeriveNewPasteSegmentFromSpellingReplacement(baseArg, newArg, oldSeg);
      if (newSeg.data() == nullptr) { // Check for "null" StringRef
        // Idempotence: later hunks may be checking a pasted-token that is
        // already consistent with an earlier spelling edit. In this common
        // case, the argument text in the current invocation equals the new
        // argument text, and the original arg text equals the old pasted
        // segment. When that holds, we can treat the paste segment as the
        // entire argument.
        uint32_t argIdx = ps.argIdx;
        StringRef origTrim = getOrigArgTrim(argIdx);
        StringRef oldTrim = oldSeg.trim();
        StringRef newTrim = newArg.trim();
        StringRef baseTrim = baseArg.trim();
        if (!origTrim.empty() && origTrim == oldTrim && baseTrim == newTrim) {
          newSeg = newTrim;
        } else {
          return false;
        }
      }

      // Rewrite only the identified segment region inside the synthetic pasted-
      // token spelling.
      expected = stringutils::replaceRange(expected, b, e, newSeg);
    }

    // If every span for this token was filtered out as a propagated child
    // contribution, then this token has no direct current-macro paste work to
    // validate at this level. The child invocation is responsible for it.
    if (!sawCurrentLevelDirectSpan) {
      continue;
    }

    if (expected != bTok) {
      return false;
    }
  }

  return true;
}

} // namespace refold
} // namespace clang
