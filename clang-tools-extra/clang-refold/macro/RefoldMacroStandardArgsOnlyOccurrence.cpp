//===--- RefoldMacroStandardArgsOnlyOccurrence.cpp --------------*- C++ -*-===//
//
// Occurrence evidence collection for the standard args-only macro patch builder.
//
// This file is a private implementation split for
// RefoldMacroStandardArgsOnlyPatchBuilder.  It owns the occurrence-side proof
// obligations that collect touched-formal hunks, observe per-formal B-side
// replacements, and validate replayed or tuple-forwarded formal occurrences.
// It must not admit candidates, certify proofs, alter fallback policy, or change
// candidate ranking; the public builder still orchestrates those decisions.
//
//===----------------------------------------------------------------------===//

#include "macro/RefoldMacroStandardArgsOnlyInternals.h"

#include "core/RefoldModel.h"
#include "macro/RefoldArgTextRecovery.h"
#include "macro/RefoldMacroPlannerHelpers.h"
#include "macro/RefoldMacroReplay.h"
#include "source/RefoldSourceMapper.h"
#include "source/RefoldToken.h"
#include "util/StringUtils.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/StringRef.h"

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

using namespace llvm;

namespace clang {
namespace refold {

namespace {

/// Equality for token diff hunks.  Kept as a named helper so synthetic hunk
/// deduplication uses the same relation everywhere in the standard args-only
/// builder.
bool sameTokenHunk(const diffutils::Hunk &lhs, const diffutils::Hunk &rhs) {
  return lhs.aStart == rhs.aStart && lhs.aEnd == rhs.aEnd &&
         lhs.bStart == rhs.bStart && lhs.bEnd == rhs.bEnd;
}

/// Canonical ordering for token hunks before deterministic deduplication.
bool tokenHunkLess(const diffutils::Hunk &lhs, const diffutils::Hunk &rhs) {
  if (lhs.aStart != rhs.aStart)
    return lhs.aStart < rhs.aStart;
  if (lhs.aEnd != rhs.aEnd)
    return lhs.aEnd < rhs.aEnd;
  if (lhs.bStart != rhs.bStart)
    return lhs.bStart < rhs.bStart;
  return lhs.bEnd < rhs.bEnd;
}

/// Raw delimiter-balance summary used only by local recovery checks that have
/// already established that approximate byte-level balancing is sufficient.
struct DelimiterBalance {
  int paren = 0;
  int bracket = 0;
  int brace = 0;
};

DelimiterBalance computeDelimiterBalance(llvm::StringRef text) {
  DelimiterBalance balance;
  for (char c : text) {
    switch (c) {
    case '(':
      ++balance.paren;
      break;
    case ')':
      --balance.paren;
      break;
    case '[':
      ++balance.bracket;
      break;
    case ']':
      --balance.bracket;
      break;
    case '{':
      ++balance.brace;
      break;
    case '}':
      --balance.brace;
      break;
    default:
      break;
    }
  }
  return balance;
}

/// If a B-side occurrence envelope introduces extra unmatched opening
/// delimiters relative to the old A-side occurrence text, extend the right edge
/// over identical unchanged closer tokens.
///
/// This helper owns only the narrow delimiter-boundary repair shared by
/// occurrence observation and tuple-slice validation.  It does not widen over
/// changed text, does not inspect arbitrary neighboring tokens, and returns the
/// original envelope whenever the unchanged A/B token stream cannot prove the
/// exact missing closers.  That preserves the existing fail-closed behavior for
/// ambiguous or malformed boundary repairs.
std::pair<size_t, size_t> maybeExtendRightBoundaryClosers(
    const RefoldMacroStandardArgsOnlyPatchBuilder::Dependencies &deps,
    const RefoldModel::PPArgSpan &sp, std::pair<size_t, size_t> env,
    StringRef oldText) {
  StringRef curText = deps.sourceMapper.SliceBSource(env.first, env.second).trim();
  auto oldBal = computeDelimiterBalance(oldText);
  auto newBal = computeDelimiterBalance(curText);

  // Only extra opens introduced by the candidate B text need compensation.
  // Extra closers or unchanged balance do not require right-edge widening.
  int needParen = std::max(0, newBal.paren - oldBal.paren);
  int needBracket = std::max(0, newBal.bracket - oldBal.bracket);
  int needBrace = std::max(0, newBal.brace - oldBal.brace);
  if (needParen == 0 && needBracket == 0 && needBrace == 0)
    return env;

  uint64_t aPos = sp.end;
  size_t bPos = env.second;

  // Walk forward only through identical A/B closer tokens immediately after the
  // occurrence. This preserves semantics: widening is allowed only over text
  // that already matches on both sides and exactly satisfies the missing
  // delimiter balance.
  while ((needParen > 0 || needBracket > 0 || needBrace > 0) &&
         aPos < deps.aToks.size() && bPos < deps.bToks.size()) {
    StringRef aTok = deps.aToks[static_cast<size_t>(aPos)].spelling;
    StringRef bTok = deps.bToks[bPos].spelling;
    if (aTok != bTok)
      break;

    if (aTok == ")" && needParen > 0) {
      --needParen;
      ++aPos;
      ++bPos;
      env.second = bPos;
      continue;
    }
    if (aTok == "]" && needBracket > 0) {
      --needBracket;
      ++aPos;
      ++bPos;
      env.second = bPos;
      continue;
    }
    if (aTok == "}" && needBrace > 0) {
      --needBrace;
      ++aPos;
      ++bPos;
      env.second = bPos;
      continue;
    }

    // Stop at the first non-needed token; this helper is a narrow boundary
    // repair, not a general hunk-widening mechanism.
    break;
  }
  return env;
}

} // namespace

//===----------------------------------------------------------------------===//
// Standard args-only occurrence resolver implementations
//===----------------------------------------------------------------------===//

TouchedFormalHunkCollector::TouchedFormalHunkCollector(
    const RefoldMacroStandardArgsOnlyPatchBuilder::Dependencies &deps,
    const RefoldMacroOccurrenceReplay &occurrenceReplay)
    : deps_(deps), occurrenceReplay_(occurrenceReplay) {}

std::optional<TouchedFormalHunkCollection>
TouchedFormalHunkCollector::Collect(TouchedFormalHunkCollection collection,
        const RefoldModel::MacroInvocation &invocation,
        const diffutils::Hunk &primaryHunk,
        size_t invocationArgCount) const {
  std::vector<RefoldModel::PPArgSpan> &occs = collection.occurrences;
  if (occs.empty())
    return std::nullopt;

  std::vector<char> touchedOcc(occs.size(), 0);
  if (!deps_.sourceMapper.HunkFullyWithinArgSpans(primaryHunk, occs,
                                                  touchedOcc))
    return std::nullopt;

  // Convert touched occurrence spans into touched formal arguments. The
  // args-only path is valid only for edits fully contained inside recorded
  // argument occurrences; anything outside those spans must fail closed.
  unsigned occFormalCount = static_cast<unsigned>(invocationArgCount);
  for (const auto &sp : occs)
    occFormalCount = std::max(occFormalCount, (unsigned)sp.argIdx + 1);

  collection.touchedFormals.assign(occFormalCount, 0);
  std::vector<char> &touched = collection.touchedFormals;
  for (size_t i = 0; i < occs.size(); ++i) {
    if (!touchedOcc[i])
      continue;
    const auto &sp = occs[i];
    if (sp.argIdx >= touched.size())
      return std::nullopt;
    touched[sp.argIdx] = 1;
  }

  // A provenance-only LCS can split one logical macro-argument rewrite into
  // several pure-insertion islands around repeated punctuation. When the
  // current seed is a pure insertion, widen the set of touched formals to every
  // occurrence in this same invocation cover that is independently touched by
  // another token hunk. This does not move hunk boundaries and does not inspect
  // neighboring token spellings; it only lets the existing replay validator see
  // the complete split edit before deciding whether an args-only rewrite is
  // actually proven.
  if (primaryHunk.aStart == primaryHunk.aEnd &&
      primaryHunk.bStart < primaryHunk.bEnd) {
    for (const diffutils::Hunk &cand : deps_.abTokHunks) {
      if (cand.aStart < invocation.cover.begin ||
          cand.aEnd > invocation.cover.end)
        continue;
      if (cand.bStart >= cand.bEnd)
        continue;

      for (const auto &sp : occs) {
        if (sp.argIdx >= touched.size())
          return std::nullopt;
        if (HunkTouchesFormalOccurrence(cand, sp, occs))
          touched[sp.argIdx] = 1;
      }
    }
  }

  SmallVector<diffutils::Hunk, 8> &tokenHunks = collection.tokenHunks;
  tokenHunks.push_back(primaryHunk);

  // A-token bounds of the occurrences of the formals this hunk touched.  Both
  // arms of `HunkTouchesFormalOccurrence()` confine a touching candidate to
  // these bounds: a candidate with A width touches only by overlapping an
  // occurrence, and a pure insertion is owned by one only at a position inside
  // it, immediately before its first token, or exactly at its end.  A candidate
  // outside the bounds therefore cannot touch any touched formal, whichever arm
  // applies, so skipping it is a proof rather than a filter.
  //
  // The scan is over every hunk in the stream and runs once per args-only
  // candidate, so on a macro-dense unit it is quadratic in the hunk count --
  // 3292 hunks against 18000 calls on uxnmin.  Deciding the cheap bound first
  // keeps the expensive arm, which maps an occurrence into B, for candidates
  // that can still qualify.
  std::optional<uint64_t> touchedOccBegin;
  std::optional<uint64_t> touchedOccEnd;
  for (const auto &sp : occs) {
    if (sp.argIdx >= touched.size() || !touched[sp.argIdx])
      continue;
    touchedOccBegin = touchedOccBegin
                          ? std::min(*touchedOccBegin, sp.begin)
                          : sp.begin;
    touchedOccEnd = touchedOccEnd ? std::max(*touchedOccEnd, sp.end) : sp.end;
  }

  auto candidateCanTouchTouchedFormal =
      [&](const diffutils::Hunk &cand) -> bool {
    if (!touchedOccBegin || !touchedOccEnd)
      return false;
    if (cand.aStart == cand.aEnd) {
      // A pure insertion at `aPos` is owned only for
      // `sp.begin - 1 <= aPos <= sp.end`; the addition avoids underflowing at
      // an occurrence that begins at token zero.
      return cand.aStart + 1 >= *touchedOccBegin &&
             cand.aStart <= *touchedOccEnd;
    }
    return cand.aEnd > *touchedOccBegin && cand.aStart < *touchedOccEnd;
  };

  // Add sibling token hunks that also touch the same formal arguments. The
  // eventual argument replacement must explain the complete set of token edits
  // for those formals, otherwise we could accept a partial rewrite.
  for (const auto &cand : deps_.abTokHunks) {
    if (sameTokenHunk(cand, primaryHunk))
      continue;
    if (!candidateCanTouchTouchedFormal(cand))
      continue;
    if (!HunkTouchesTouchedFormal(cand, occs, touched))
      continue;
    tokenHunks.push_back(cand);
  }

  // Iterate over a snapshot of the currently known hunks. Synthetic-envelope
  // discovery may append to `tokenHunks`, so the seed copy avoids recursively
  // pairing newly synthesized hunks in the same pass.
  SmallVector<diffutils::Hunk, 8> seedTokenHunks(tokenHunks.begin(),
                                                 tokenHunks.end());

  for (size_t occIdx = 0; occIdx < occs.size(); ++occIdx) {
    const auto &sp = occs[occIdx];
    if (sp.argIdx >= touched.size() || !touched[sp.argIdx])
      continue;

    for (const auto &cand : seedTokenHunks) {
      // Only zero-width A-side insertion frontiers can participate in
      // synthetic envelope construction. Non-insertion hunks already expose
      // their A range.
      if (cand.aStart != cand.aEnd)
        continue;
      MaybeAddSyntheticTouchedFormalEnvelope(invocation, sp, cand, occs,
                                             tokenHunks);
    }
  }

  // Normalize the hunk set after adding synthetic envelopes. This prevents the
  // same physical edit from being observed multiple times through equivalent
  // primary/synthetic paths.
  llvm::sort(tokenHunks, tokenHunkLess);
  tokenHunks.erase(std::unique(tokenHunks.begin(), tokenHunks.end(),
                               sameTokenHunk),
                   tokenHunks.end());

  return collection;
}

bool TouchedFormalHunkCollector::HunkTouchesFormalOccurrence(
    const diffutils::Hunk &cand, const RefoldModel::PPArgSpan &sp,
    llvm::ArrayRef<RefoldModel::PPArgSpan> occs) const {
  // For replacements/deletions, touching is ordinary A-range overlap. For
  // pure insertions, the hunk has no A width, so require the existing
  // argument-span ownership helper to prove that the B insertion belongs to
  // this occurrence.
  if (cand.aStart == cand.aEnd) {
    auto bEnv = deps_.sourceMapper.MapAToBTokenEnvelopeByPPArgSpan(sp);
    if (!bEnv)
      return false;
    return occurrenceReplay_
        .GetOwnedPureInsertionBRangeForArgSpan(sp, occs, *bEnv, cand)
        .has_value();
  }

  return cand.aStart < sp.end && cand.aEnd > sp.begin;
}

bool TouchedFormalHunkCollector::HunkTouchesTouchedFormal(
    const diffutils::Hunk &cand,
    llvm::ArrayRef<RefoldModel::PPArgSpan> occs,
    const std::vector<char> &touched) const {
  // Keep only hunks that intersect an occurrence of a formal already touched
  // by the primary args-only hunk. This lets the rewrite validate all edits to
  // the same formal argument, not just the hunk that triggered this path.
  for (const auto &sp : occs) {
    if (sp.argIdx >= touched.size() || !touched[sp.argIdx])
      continue;
    if (HunkTouchesFormalOccurrence(cand, sp, occs))
      return true;
  }
  return false;
}

void TouchedFormalHunkCollector::MaybeAddSyntheticTouchedFormalEnvelope(
    const RefoldModel::MacroInvocation &invocation,
    const RefoldModel::PPArgSpan &sp, const diffutils::Hunk &anchor,
    llvm::ArrayRef<RefoldModel::PPArgSpan> occs,
    SmallVector<diffutils::Hunk, 8> &tokenHunks) const {
  // This synthesis is only for insertion-frontier hunks. Non-insertion hunks
  // already carry an A-side interval and do not need reconstruction.
  if (anchor.aStart != anchor.aEnd)
    return;
  if (anchor.aStart < invocation.cover.begin ||
      anchor.aEnd > invocation.cover.end)
    return;

  for (const auto &partner : deps_.abTokHunks) {
    if (sameTokenHunk(anchor, partner))
      continue;

    // Pair the anchor only with another insertion frontier from the same macro
    // cover. Their combined envelope may reveal the true touched argument span
    // after common edge tokens are trimmed.
    if (partner.aStart != partner.aEnd)
      continue;
    if (partner.aStart < invocation.cover.begin ||
        partner.aEnd > invocation.cover.end)
      continue;

    const diffutils::Hunk env =
        buildCombinedInsertionEnvelope(anchor, partner);
    const diffutils::Hunk envTrim =
        trimCommonEdgeTokens(env, deps_.aToks, deps_.bToks);

    // The trimmed synthetic envelope must expose a real A-side token range.
    if (envTrim.aStart >= envTrim.aEnd)
      continue;

    // The exposed range must be fully contained in the exact occurrence
    // currently being considered.
    if (!(sp.begin <= envTrim.aStart && envTrim.aEnd <= sp.end))
      continue;

    SmallVector<char, 8> envTouched(occs.size(), 0);
    if (!deps_.sourceMapper.HunkFullyWithinArgSpans(envTrim, occs,
                                                    envTouched))
      continue;

    bool touchesThisExactOccurrence = false;
    for (size_t occIdx = 0; occIdx < occs.size(); ++occIdx) {
      if (!envTouched[occIdx])
        continue;

      // Fail closed if the synthetic envelope touches any other formal or any
      // other occurrence. It is valid only as an explanation for this exact
      // argument occurrence.
      if (occs[occIdx].argIdx != sp.argIdx)
        return;
      if (occs[occIdx].begin != sp.begin || occs[occIdx].end != sp.end)
        return;

      touchesThisExactOccurrence = true;
    }
    if (!touchesThisExactOccurrence)
      continue;

    // Add the proof-compatible synthetic hunk so downstream args-only
    // replacement logic validates the complete touched formal edit.
    tokenHunks.push_back(envTrim);
  }
}

InvocationOccurrenceObservationCollector::
    InvocationOccurrenceObservationCollector(
        const RefoldMacroStandardArgsOnlyPatchBuilder::Dependencies &deps,
        const RefoldMacroOccurrenceReplay &occurrenceReplay)
    : deps_(deps), occurrenceReplay_(occurrenceReplay) {}

std::optional<InvocationOccurrenceObservationSet>
InvocationOccurrenceObservationCollector::Collect(const RefoldModel::MacroInvocation &invocation, uint32_t argIdx,
        StringRef baseArgText,
        llvm::ArrayRef<RefoldModel::PPArgSpan> occurrences,
        llvm::ArrayRef<char> occurrenceIsStringify,
        llvm::ArrayRef<diffutils::Hunk> tokenHunks,
        const diffutils::Hunk &primaryHunk) const {
  InvocationOccurrenceObservationSet result;

  // Tracks whether the current `result.unifiedNewArg` was fixed by an evaluated
  // (non-stringified) occurrence.  In relaxed mode an evaluated use is
  // authoritative for the refolded argument, and a stringified occurrence whose
  // spelling was not correspondingly edited is tolerated rather than forcing
  // tuple forwarding: re-expanding the refolded invocation regenerates `#arg`
  // from the new argument.  Strict mode still requires every occurrence to agree.
  bool unifiedNewArgFromEvaluated = false;

  for (size_t i = 0; i < occurrences.size(); ++i) {
    const auto &sp = occurrences[i];
    if (sp.argIdx != argIdx)
      continue;

    // Start with the B-token envelope corresponding to this A-side argument
    // occurrence. If the direct PP-arg mapping is unavailable, fall back to
    // the triggering hunk's B range so the path can still fail/validate
    // locally.
    auto bEnv = deps_.sourceMapper.MapAToBTokenEnvelopeByPPArgSpan(sp);
    if (!bEnv) {
      if (primaryHunk.bStart >= primaryHunk.bEnd)
        return std::nullopt;
      bEnv = {static_cast<size_t>(primaryHunk.bStart),
              static_cast<size_t>(primaryHunk.bEnd)};
    }

    std::optional<std::pair<size_t, size_t>> ownedInsertionBRange;
    bool contributedNonInsertionHunk = false;

    if (bEnv) {
      size_t e0 = bEnv->first;
      size_t e1 = bEnv->second;

      // Widen the occurrence envelope to include every token hunk that
      // belongs to this same touched formal. This prevents deriving a
      // replacement from only one fragment of a multi-hunk argument edit.
      for (const auto &candH : tokenHunks) {
        // Pure insertions have no A-side interval, so accept them only when
        // the ownership helper proves the inserted B range belongs to this
        // exact argument occurrence. Record those inserted B tokens
        // separately: they are the precise output-side materialization
        // surface for insertion-only argument rewrites.
        if (auto owned = occurrenceReplay_.GetOwnedPureInsertionBRangeForArgSpan(
                sp, occurrences, *bEnv, candH)) {
          const size_t insB0 = owned->first;
          const size_t insB1 = owned->second;
          if (!(insB1 < e0 || e1 < insB0)) {
            e0 = std::min(e0, insB0);
            e1 = std::max(e1, insB1);
            if (ownedInsertionBRange) {
              ownedInsertionBRange->first =
                  std::min(ownedInsertionBRange->first, insB0);
              ownedInsertionBRange->second =
                  std::max(ownedInsertionBRange->second, insB1);
            } else {
              ownedInsertionBRange = std::make_pair(insB0, insB1);
            }
          }
          continue;
        }

        // Non-insertion hunks can widen the envelope when their A-side
        // interval overlaps this argument occurrence and they carry concrete
        // B text.
        if (candH.aStart == candH.aEnd)
          continue;
        if (candH.aStart < sp.end && candH.aEnd > sp.begin &&
            candH.bStart < candH.bEnd) {
          contributedNonInsertionHunk = true;
          e0 = std::min(e0, static_cast<size_t>(candH.bStart));
          e1 = std::max(e1, static_cast<size_t>(candH.bEnd));
        }
      }

      if (e0 != bEnv->first || e1 != bEnv->second)
        bEnv = std::make_pair(e0, e1);
    }

    // `oldText` is the original occurrence spelling. `bEnv` is the candidate
    // B spelling that should replace this occurrence after all relevant hunks
    // for the same formal have been incorporated.
    StringRef oldText = deps_.sourceMapper.SliceASource(sp.begin, sp.end).trim();

    // For ordinary argument occurrences, allow a narrow right-edge repair over
    // unchanged closer tokens when the diff split leaves balancing delimiters
    // just outside the initial B envelope.
    if (sp.kind == PPArgSpanKind::Standard) {
      auto grownEnv = maybeExtendRightBoundaryClosers(deps_, sp, *bEnv, oldText);
      if (grownEnv.second != bEnv->second)
        bEnv = grownEnv;
    }

    StringRef bSlice = deps_.sourceMapper.SliceBSource(bEnv->first, bEnv->second)
                           .trim();
    std::string newArg = bSlice.str();

    std::optional<std::pair<uint64_t, uint64_t>> materializedNewTextRange;
    if (!occurrenceIsStringify[i] && ownedInsertionBRange &&
        !contributedNonInsertionHunk) {
      if (auto insertedBytes = deps_.sourceMapper.BTokenRangeToByteRange(
              ownedInsertionBRange->first, ownedInsertionBRange->second)) {
        const char *sourceBegin = deps_.bSource.data();
        const char *sourceEnd = sourceBegin + deps_.bSource.size();
        const char *sliceBeginPtr = bSlice.data();
        const char *sliceEndPtr = sliceBeginPtr + bSlice.size();
        if (sourceBegin <= sliceBeginPtr && sliceBeginPtr <= sourceEnd &&
            sourceBegin <= sliceEndPtr && sliceEndPtr <= sourceEnd) {
          const uint64_t sliceBegin =
              static_cast<uint64_t>(sliceBeginPtr - sourceBegin);
          const uint64_t sliceEnd =
              static_cast<uint64_t>(sliceEndPtr - sourceBegin);
          if (insertedBytes->first >= sliceBegin &&
              insertedBytes->second <= sliceEnd) {
            materializedNewTextRange =
                std::make_pair(insertedBytes->first - sliceBegin,
                               insertedBytes->second - sliceBegin);
          }
        }
      }
    }

    if (occurrenceIsStringify[i]) {
      // Stringify occurrences expose a string literal in the expansion, not
      // the raw argument spelling. Invert the literal back to argument text,
      // then require canonicalization to be stable so ambiguous escapes fail
      // closed.
      auto un = deps_.argTextRecovery.UnstringifyLiteralToArgText(
          bSlice, isMacroInvocationVariadicFormal(invocation, argIdx));
      if (!un)
        return std::nullopt;

      auto canon = stringutils::canonicalizeStringifyInversePayload(*un);
      if (!canon || StringRef(*canon).trim() != StringRef(*un).trim())
        return std::nullopt;

      newArg = std::move(*canon);

      // Normalize the old side into the same unstringified representation so
      // the later occurrence-consistency checks compare argument text to
      // argument text.
      auto oldUn =
          deps_.argTextRecovery.UnstringifyLiteralToArgText(oldText, true);
      if (oldUn)
        oldText = StringRef(*oldUn).trim();
    }

    if (!occurrenceIsStringify[i] && !invocation.pasteSpans.empty()) {
      bool argHasPaste = false;
      for (const auto &ps : invocation.pasteSpans) {
        if (ps.argIdx == argIdx) {
          argHasPaste = true;
          break;
        }
      }

      if (argHasPaste) {
        StringRef aSlice = deps_.sourceMapper.SliceASource(sp.begin, sp.end).trim();
        if (!aSlice.empty()) {
          size_t pos = baseArgText.find(aSlice);
          if (pos != StringRef::npos) {
            // Paste/lift case: the occurrence may represent only the pasted
            // segment inside a larger call-site argument. Replace that
            // original segment inside the full base argument rather than
            // replacing the whole argument with the pasted-token slice.
            std::string cand = baseArgText.substr(0, pos).str() + bSlice.str() +
                               baseArgText.substr(pos + aSlice.size()).str();
            newArg = StringRef(cand).trim().str();
            materializedNewTextRange = std::nullopt;
          } else {
            // The pasted occurrence could not be located inside the original
            // call-site argument, so leave `newArg` as the direct B slice and
            // let the later consistency/validation checks decide whether it is
            // usable.
          }
        }
      }
    }

    // Record this occurrence-level old/new observation. If all observations
    // for this formal agree on one replacement, the args-only path can rewrite
    // the formal directly; disagreement triggers the tuple-forwarding fallback.
    result.observations.push_back(
        OccObservation{oldText.str(), newArg, materializedNewTextRange});
    if (!materializedNewTextRange) {
      result.sawUntrackedMaterializedNewTextRange = true;
    } else if (result.unifiedMaterializedNewTextRange) {
      result.unifiedMaterializedNewTextRange->first = std::min(
          result.unifiedMaterializedNewTextRange->first,
          materializedNewTextRange->first);
      result.unifiedMaterializedNewTextRange->second = std::max(
          result.unifiedMaterializedNewTextRange->second,
          materializedNewTextRange->second);
    } else {
      result.unifiedMaterializedNewTextRange = *materializedNewTextRange;
    }

    const bool thisIsStringify =
        i < occurrenceIsStringify.size() && occurrenceIsStringify[i] != 0;

    if (!result.unifiedNewArg) {
      result.unifiedNewArg = newArg;
      unifiedNewArgFromEvaluated = !thisIsStringify;
    } else if (*result.unifiedNewArg != newArg) {
      if (!deps_.strict && thisIsStringify && unifiedNewArgFromEvaluated) {
        // Relaxed: an evaluated occurrence already fixed the argument; this
        // stringified occurrence kept a stale spelling.  Tolerate it — the
        // stringification is regenerated from the new argument on re-expansion.
        // Strict mode falls through below and forces tuple forwarding.
      } else if (!deps_.strict && !thisIsStringify &&
                 !unifiedNewArgFromEvaluated) {
        // Relaxed: an evaluated occurrence supersedes an argument previously
        // inferred only from a stringified occurrence, regardless of order.
        result.unifiedNewArg = newArg;
        unifiedNewArgFromEvaluated = true;
      } else {
        // Strict mode, or a genuine disagreement between two authoritative
        // (evaluated) occurrences, or between stringified-only occurrences.
        result.needTupleForwarding = true;
      }
    }
  }

  return result;
}

TupleSliceConsistencyValidator::TupleSliceConsistencyValidator(
    const RefoldMacroStandardArgsOnlyPatchBuilder::Dependencies &deps,
    const RefoldMacroOccurrenceReplay &occurrenceReplay)
    : deps_(deps), occurrenceReplay_(occurrenceReplay) {}

bool TupleSliceConsistencyValidator::Validate(uint32_t argIdx,
              llvm::ArrayRef<RefoldModel::PPArgSpan> standardArgSpans,
              llvm::ArrayRef<OccObservation> observations,
              llvm::ArrayRef<diffutils::Hunk> tokenHunks) const {
  // Tuple-forwarded rewrites are slice-based: each old tuple element/slice
  // must consistently map to exactly one new spelling across all observations.
  llvm::StringMap<std::string> newTextByOld;
  for (const auto &obs : observations) {
    StringRef oldKey = StringRef(obs.oldText).trim();
    StringRef newVal = StringRef(obs.newText).trim();
    auto it = newTextByOld.find(oldKey);
    if (it == newTextByOld.end()) {
      newTextByOld[oldKey] = newVal.str();
      continue;
    }
    if (StringRef(it->second).trim() != newVal)
      return false;
  }

  // Validate the slice map against every standard occurrence of this formal in
  // B, not only the occurrence that originally triggered the rewrite.
  for (const auto &s : standardArgSpans) {
    if (s.argIdx != argIdx || s.kind != PPArgSpanKind::Standard)
      continue;

    StringRef oldSlice = deps_.sourceMapper.SliceASource(s.begin, s.end).trim();
    auto expectedIt = newTextByOld.find(oldSlice);
    if (expectedIt == newTextByOld.end())
      return false;

    auto bEnv = deps_.sourceMapper.MapAToBTokenEnvelopeByPPArgSpan(s);
    if (!bEnv)
      return false;

    size_t lo = bEnv->first;
    size_t hi = bEnv->second;

    // Reconstruct the same widened B envelope used during observation
    // collection, incorporating owned insertions and overlapping token hunks for
    // this occurrence.
    for (const auto &hk : tokenHunks) {
      if (auto owned = occurrenceReplay_.GetOwnedPureInsertionBRangeForArgSpan(
              s, standardArgSpans, *bEnv, hk)) {
        lo = std::min(lo, owned->first);
        hi = std::max(hi, owned->second);
        continue;
      }
      if (hk.aStart == hk.aEnd)
        continue;
      if (hk.aStart < s.end && hk.aEnd > s.begin && hk.bStart < hk.bEnd) {
        lo = static_cast<size_t>(std::min<uint64_t>(lo, hk.bStart));
        hi = static_cast<size_t>(std::max<uint64_t>(hi, hk.bEnd));
      }
    }

    // Apply the same narrow delimiter-closer repair used for the primary
    // observation path so validation compares equivalent envelopes.
    if (s.kind == PPArgSpanKind::Standard) {
      auto grownEnv = maybeExtendRightBoundaryClosers(
          deps_, s, std::make_pair(lo, hi), oldSlice);
      lo = grownEnv.first;
      hi = grownEnv.second;
    }

    StringRef tokText = deps_.sourceMapper.SliceBSource(lo, hi).trim();
    if (tokText != StringRef(expectedIt->second).trim())
      return false;
  }

  return true;
}

ReplayedFormalOccurrenceValidator::ReplayedFormalOccurrenceValidator(
    const RefoldMacroStandardArgsOnlyPatchBuilder::Dependencies &deps,
    const RefoldMacroOccurrenceReplay &occurrenceReplay)
    : deps_(deps), occurrenceReplay_(occurrenceReplay) {}

/// Return whether `newArg` matches every repaired STANDARD occurrence.
bool ReplayedFormalOccurrenceValidator::Validate(uint32_t argIdx, StringRef newArg,
              ArrayRef<RefoldModel::PPArgSpan> standardArgSpans,
              ArrayRef<diffutils::Hunk> tokenHunks) const {
  const uint64_t maxTok =
      deps_.bTokOff.empty() ? 0ULL
                            : static_cast<uint64_t>(deps_.bTokOff.size() - 1);
  const StringRef expected = newArg.trim();
  bool sawOccurrence = false;

  for (const RefoldModel::PPArgSpan &s : standardArgSpans) {
    if (s.argIdx != argIdx || s.kind != PPArgSpanKind::Standard)
      continue;
    sawOccurrence = true;

    auto bEnv = deps_.sourceMapper.MapAToBTokenEnvelopeByPPArgSpan(s);
    if (!bEnv || bEnv->second < bEnv->first)
      return false;

    // Grow the mapped B envelope with any owned insertions or overlapping
    // hunks for this occurrence before comparing the materialized text.
    size_t lo = bEnv->first;
    size_t hi = bEnv->second;
    for (const diffutils::Hunk &hk : tokenHunks) {
      if (auto owned = occurrenceReplay_.GetOwnedPureInsertionBRangeForArgSpan(
              s, standardArgSpans, *bEnv, hk)) {
        lo = std::min(lo, owned->first);
        hi = std::max(hi, owned->second);
        continue;
      }
      if (hk.aStart == hk.aEnd)
        continue;
      if (hk.aStart < s.end && hk.aEnd > s.begin && hk.bStart < hk.bEnd) {
        lo = static_cast<size_t>(std::min<uint64_t>(lo, hk.bStart));
        hi = static_cast<size_t>(std::max<uint64_t>(hi, hk.bEnd));
      }
    }

    lo = static_cast<size_t>(std::clamp<uint64_t>(lo, 0ULL, maxTok));
    hi = static_cast<size_t>(std::clamp<uint64_t>(hi, lo, maxTok));

    StringRef oldText = deps_.sourceMapper.SliceASource(s.begin, s.end).trim();
    auto grownEnv = maybeExtendRightBoundaryClosers(
        deps_, s, std::make_pair(lo, hi), oldText);
    StringRef actual =
        deps_.sourceMapper.SliceBSource(grownEnv.first, grownEnv.second)
            .trim();
    if (actual != expected)
      return false;
  }

  return sawOccurrence;
}

} // namespace refold
} // namespace clang
