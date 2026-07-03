//===--- RefoldMacroDefinitionTapeSolver.cpp --------------------*- C++ -*-===//
//
// Definition replacement-list tape replay for macro args-only patch
// construction.
//
//===----------------------------------------------------------------------===//

#include "macro/RefoldMacroDefinitionTapeSolver.h"
#include "macro/RefoldArgTextRecovery.h"
#include "macro/RefoldMacroPlannerHelpers.h"
#include "macro/RefoldMacroReplay.h"
#include "proof/RefoldProofLattice.h"
#include "source/RefoldSourceMapper.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/FormatVariadic.h"

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

using namespace llvm;

namespace clang {
namespace refold {

namespace {

/// Return true when a definition parameter is variadic. Kept local to this
/// translation unit because it's only used here.
bool macroDefParamIsVariadic(const RefoldModel::MacroDefParam &param) {
  return param.variadic;
}

/// Check whether the A-token at `tok` has the given spelling.  Takes the
/// A-token array as an explicit parameter so the definition-tape solver can
/// remain independent of planner state.
bool matchLiteralAToken(ArrayRef<PPTok> aToks, uint64_t tok,
                        StringRef spelling) {
  return tok < aToks.size() &&
         aToks[static_cast<size_t>(tok)].spelling == spelling;
}

/// Definition directive for the invocation, or nullptr when the producer did
/// not record a corresponding `#define` site.  The lookup remains O(N) in the
/// model directive list since the model does not yet expose a directive index.
const RefoldModel::MacroDirective *
getDefinitionDirectiveForInvocation(const RefoldModel *model,
                                    const RefoldModel::MacroInvocation &m) {
  if (!m.definitionDirectiveId)
    return nullptr;
  for (const RefoldModel::MacroDirective &directive :
       model->GetMacroDirectives()) {
    if (directive.id == *m.definitionDirectiveId)
      return &directive;
  }
  return nullptr;
}

/// Normalized replacement-list node used by the definition-tape replay solver.
///
/// Keeping this carrier at file scope makes replay-tree helpers nameable
/// without changing the solver's fail-closed matching semantics.  Literal nodes
/// consume fixed replacement-list tokens, parameter nodes choose a B-token
/// interval for one formal, and VA_OPT nodes contain the recursively parsed
/// optional payload.
struct DefinitionTapeReplayElem {
  enum class Kind { Literal, Param, VaOpt } kind = Kind::Literal;
  std::string spelling;
  uint32_t argIdx = 0;
  std::vector<DefinitionTapeReplayElem> children;
};

/// One A-side formal occurrence observed while replaying a macro definition's
/// replacement-token tape over the producer's original expansion cover.
struct DefinitionTapeReplayAOcc {
  uint32_t argIdx = 0;
  uint64_t aBegin = 0;
  uint64_t aEnd = 0;
  std::optional<RefoldModel::PPArgSpan> span;
};

/// B-side formal assignment selected by the definition-tape replay solver.
///
/// `ranges[i]` is the B-token interval assigned to formal i; `assigned[i]`
/// records whether that formal actually appeared in the replay.  Repeated
/// formal occurrences must later agree on equivalent trimmed B spelling.
struct DefinitionTapeReplaySolution {
  std::vector<std::pair<size_t, size_t>> ranges;
  std::vector<char> assigned;
  unsigned vaOptIncludedCount = 0;
};

/// Replacement-list profile shared by every candidate replay solution for one
/// macro definition.  The profile captures proof-relevant tape structure only;
/// it intentionally does not record candidate-specific replacement spelling.
struct DefinitionTapeProfile {
  uint64_t literalCount = 0;
  uint64_t paramUseCount = 0;
  uint64_t vaOptNodeCount = 0;
  uint64_t duplicatedFormalCount = 0;
  uint64_t unusedFormalCount = 0;
  uint64_t emptySourceSlotCount = 0;
  uint64_t zeroTokenAOccurrenceCount = 0;
};

/// Canonicalization score for otherwise-valid definition-tape replay solutions.
/// These fields preserve the existing deterministic preference order; semantic
/// proof equivalence is still decided by the replay equivalence key.
struct DefinitionTapeScoredSolution {
  DefinitionTapeReplaySolution sol;
  uint64_t nonEmptyDeviation = 0;
  uint64_t emptySlotTokenCount = 0;
  uint64_t unusedFormalPreservedCount = 0;
  uint64_t zeroTokenAssignedFormalCount = 0;
  uint64_t vaOptIncludedCount = 0;
  std::string rewritten;
  std::string equivalenceKey;
};

/// Text edit against the original invocation spelling produced by a replay
/// assignment.  Edits are applied right-to-left so byte offsets remain stable.
struct DefinitionTapeInvocationEdit {
  size_t begin = 0;
  size_t end = 0;
  std::string repl;
};

/// Source-side variadic actual state used when building replay equivalence
/// signatures for VA_OPT and GNU comma-elision cases.
struct DefinitionTapeVariadicState {
  bool missing = false;
  bool explicitEmpty = false;
  bool nonEmpty = false;
  bool literalComma = false;
};

/// Deterministic ordering for equivalent definition-tape replay solutions.
///
/// This preserves the original canonical representative policy: lower
/// non-empty deviation wins, then stronger preservation of unused/empty/zero
/// token surfaces, then lexicographic invocation spelling.
static bool
definitionTapeScoredSolutionLess(const DefinitionTapeScoredSolution &lhs,
                                 const DefinitionTapeScoredSolution &rhs) {
  if (lhs.nonEmptyDeviation != rhs.nonEmptyDeviation)
    return lhs.nonEmptyDeviation < rhs.nonEmptyDeviation;
  if (lhs.unusedFormalPreservedCount != rhs.unusedFormalPreservedCount)
    return lhs.unusedFormalPreservedCount > rhs.unusedFormalPreservedCount;
  if (lhs.emptySlotTokenCount != rhs.emptySlotTokenCount)
    return lhs.emptySlotTokenCount > rhs.emptySlotTokenCount;
  if (lhs.zeroTokenAssignedFormalCount != rhs.zeroTokenAssignedFormalCount)
    return lhs.zeroTokenAssignedFormalCount > rhs.zeroTokenAssignedFormalCount;
  if (lhs.vaOptIncludedCount != rhs.vaOptIncludedCount)
    return lhs.vaOptIncludedCount > rhs.vaOptIncludedCount;
  return lhs.rewritten < rhs.rewritten;
}

} // namespace

std::optional<MacroPatch>
RefoldMacroDefinitionTapeSolver::TryDefinitionTapeReplayArgsOnlyPatch(
    const RefoldModel::MacroInvocation &invocation, const diffutils::Hunk &hunk,
    StringRef baseInvocationText,
    ArrayRef<std::pair<size_t, size_t>> invArgRangesParam) const {
  // Re-alias parameters so the original body referencing `m`, `h`,
  // `baseInvText`, and `invArgRanges` works verbatim.
  const RefoldModel::MacroInvocation &m = invocation;
  const diffutils::Hunk &h = hunk;
  StringRef baseInvText = baseInvocationText;
  ArrayRef<std::pair<size_t, size_t>> invArgRanges = invArgRangesParam;

  const RefoldModel::MacroDirective *definition =
      getDefinitionDirectiveForInvocation(deps_.model, m);
  if (!definition || definition->subkind != "#define" ||
      !definition->functionLike || definition->name != m.name ||
      definition->defParams.size() != m.defParams.size() ||
      definition->replacementTokens.empty() || !m.cover.IsValid() ||
      !m.stringifySpans.empty() || !m.pasteSpans.empty())
    return std::nullopt;

  auto cover = RefoldMacroWholeCoverProof::GetWholeCoverATokRange(m);
  if (!cover || cover->first >= cover->second)
    return std::nullopt;

  // Use the file-scope replacement-list node carrier so replay semantics are
  // shared by the solver helpers without introducing function-local types.
  using ReplayElem = DefinitionTapeReplayElem;

  // Parse the producer's replacement-token tape into ReplayElem nodes.
  // Stringification and token-paste are rejected here because they transform
  // argument spelling before it reaches the PP output; those cases require
  // the dedicated stringify/paste proof paths rather than raw token replay.
  std::function<bool(size_t, size_t, std::vector<ReplayElem> &)>
      parseReplayRange =
          [&](size_t begin, size_t end, std::vector<ReplayElem> &out) -> bool {
    for (size_t i = begin; i < end;) {
      const RefoldModel::MacroReplacementToken &tok =
          definition->replacementTokens[i];
      if (tok.kind == RefoldModel::MacroReplacementTokenKind::ParamRef) {
        if (!tok.paramIndex || *tok.paramIndex >= m.defParams.size())
          return false;
        ReplayElem elem;
        elem.kind = ReplayElem::Kind::Param;
        elem.argIdx = *tok.paramIndex;
        out.push_back(std::move(elem));
        ++i;
        continue;
      }

      if (tok.spelling == "#" || tok.spelling == "##")
        return false;

      if (tok.spelling == "__VA_OPT__") {
        // __VA_OPT__ contributes either nothing or its parenthesized payload.
        // Parse the payload recursively so later A/B replay can choose the
        // erased or exposed branch by matching concrete expansion tokens.
        if (i + 1 >= end ||
            definition->replacementTokens[i + 1].kind !=
                RefoldModel::MacroReplacementTokenKind::Literal ||
            definition->replacementTokens[i + 1].spelling != "(")
          return false;

        unsigned depth = 1;
        size_t j = i + 2;
        for (; j < end; ++j) {
          const auto &inner = definition->replacementTokens[j];
          if (inner.kind != RefoldModel::MacroReplacementTokenKind::Literal)
            continue;
          if (inner.spelling == "(") {
            ++depth;
            continue;
          }
          if (inner.spelling == ")") {
            if (--depth == 0)
              break;
          }
        }
        if (depth != 0 || j >= end)
          return false;

        ReplayElem elem;
        elem.kind = ReplayElem::Kind::VaOpt;
        if (!parseReplayRange(i + 2, j, elem.children))
          return false;
        out.push_back(std::move(elem));
        i = j + 1;
        continue;
      }

      ReplayElem elem;
      elem.kind = ReplayElem::Kind::Literal;
      elem.spelling = tok.spelling.str();
      out.push_back(std::move(elem));
      ++i;
    }
    return true;
  };

  std::vector<ReplayElem> pattern;
  if (!parseReplayRange(0, definition->replacementTokens.size(), pattern) ||
      pattern.empty())
    return std::nullopt;

  // Keep this heavier replay solver out of the ordinary non-empty argument
  // case.  It exists for missing proof surfaces: a token-empty source slot, a
  // formal with no recorded expansion span, or a __VA_OPT__ branch flip.
  bool hasVaOpt = false;
  std::function<void(ArrayRef<ReplayElem>)> markVaOpt =
      [&](ArrayRef<ReplayElem> elems) {
        for (const ReplayElem &elem : elems) {
          if (elem.kind == ReplayElem::Kind::VaOpt)
            hasVaOpt = true;
          markVaOpt(elem.children);
        }
      };
  markVaOpt(pattern);

  bool hasEmptyFormalSourceSlot = false;
  for (size_t i = 0; i < invArgRanges.size(); ++i) {
    auto r = invArgRanges[i];
    if (r.second < r.first || r.second > baseInvText.size())
      return std::nullopt;
    if (baseInvText.slice(r.first, r.second).trim().empty())
      hasEmptyFormalSourceSlot = true;
  }

  bool hasMissingExpansionFormal = false;
  for (size_t formalIdx = 0; formalIdx < invArgRanges.size(); ++formalIdx) {
    bool saw = false;
    for (const auto &as : m.argSpans) {
      if (as.kind == PPArgSpanKind::Standard && as.argIdx == formalIdx &&
          as.begin < as.end) {
        saw = true;
        break;
      }
    }
    if (!saw)
      hasMissingExpansionFormal = true;
  }

  // A token-bearing variadic pack can be edited down to an explicit empty
  // actual: `M(x, a, b)` -> `M(x, )`.  The ordinary local args-only proof can
  // synthesize that spelling, but the root fixed-body guard cannot prove the
  // adjacent replacement-list punctuation from a one-token A->B envelope when
  // the diff coalesces it with the erased pack.  Route this case through the
  // definition-tape solver so the whole replacement-list transducer, including
  // fixed punctuation and the zero-token variadic slot, is discharged once.
  bool hasVariadicFormalErasedInB = false;
  for (const auto &as : m.argSpans) {
    if (as.kind != PPArgSpanKind::Standard || as.begin >= as.end ||
        as.argIdx >= m.defParams.size() || !m.defParams[as.argIdx].variadic)
      continue;
    std::optional<std::pair<size_t, size_t>> bArg =
        (*deps_.sourceMapper).MapAToBTokenEnvelopeByPPArgSpan(as);
    if (bArg && bArg->first == bArg->second) {
      hasVariadicFormalErasedInB = true;
      break;
    }
  }

  // Token LCS may slide an inserted token left across identical fixed
  // replacement-list literals.  For example, with
  //
  //   #define M(x) ((x) >= 0)
  //
  // changing `x` to `(y).field` can appear as a pure insertion immediately
  // before the recorded macro cover, even though the inserted `(` is
  // semantically the first token of the rewritten actual.  When the producer
  // recorded the exact PP-byte expansion envelope, definition-tape replay can
  // re-anchor the fixed macro-body literals against that byte envelope and
  // recover the actual without emitting a separate TU insertion.
  const bool hasLeftBoundaryDefinitionTapeSlide =
      h.isInsertOnly() && h.bStart < h.bEnd && cover->first > 0 &&
      h.aStart + 1 == cover->first && m.invPPByteBegin && m.invPPByteEnd;

  if (!hasVaOpt && !hasEmptyFormalSourceSlot && !hasMissingExpansionFormal &&
      !hasVariadicFormalErasedInB && !hasLeftBoundaryDefinitionTapeSlide)
    return std::nullopt;

  // Use the file-scope A-occurrence carrier so replay matching helpers share a
  // stable parameter type across the definition-tape solver.
  using ReplayAOcc = DefinitionTapeReplayAOcc;

  std::vector<RefoldModel::PPArgSpan> standardSpans;
  for (const auto &as : m.argSpans) {
    if (as.kind == PPArgSpanKind::Standard && as.begin < as.end)
      standardSpans.push_back(as);
  }
  llvm::sort(standardSpans, ppArgSpanLessByTokenRangeAndArg);

  // Prove that the normalized replacement-list tree exactly regenerates the
  // original A expansion cover.  Literal nodes must match one token; formal
  // nodes either consume the next standard span for that formal or record a
  // zero-width occurrence; __VA_OPT__ consumes its payload only when the
  // payload has concrete A-side evidence.
  std::function<bool(ArrayRef<ReplayElem>, uint64_t &, size_t &,
                     std::vector<ReplayAOcc> &)>
      matchAReplay = [&](ArrayRef<ReplayElem> elems, uint64_t &cursor,
                         size_t &spanIdx,
                         std::vector<ReplayAOcc> &occs) -> bool {
    for (const ReplayElem &elem : elems) {
      if (spanIdx < standardSpans.size() &&
          standardSpans[spanIdx].begin < cursor)
        return false;

      switch (elem.kind) {
      case ReplayElem::Kind::Literal:
        if (!matchLiteralAToken(deps_.aToks, cursor, elem.spelling))
          return false;
        ++cursor;
        break;
      case ReplayElem::Kind::Param: {
        ReplayAOcc occ;
        occ.argIdx = elem.argIdx;
        // Start as a zero-width occurrence.  A following PPArgSpan for the
        // same formal turns this into an ordinary token-bearing occurrence;
        // otherwise the zero-width marker is the proof surface for an empty
        // source actual or a zero-token child actual.
        occ.aBegin = cursor;
        occ.aEnd = cursor;
        if (spanIdx < standardSpans.size() &&
            standardSpans[spanIdx].begin == cursor &&
            standardSpans[spanIdx].argIdx == elem.argIdx) {
          occ.span = standardSpans[spanIdx];
          occ.aBegin = standardSpans[spanIdx].begin;
          occ.aEnd = standardSpans[spanIdx].end;
          cursor = standardSpans[spanIdx].end;
          ++spanIdx;
        }
        occs.push_back(std::move(occ));
        break;
      }
      case ReplayElem::Kind::VaOpt: {
        uint64_t includeCursor = cursor;
        size_t includeSpanIdx = spanIdx;
        std::vector<ReplayAOcc> includeOccs = occs;
        const bool includeOK = matchAReplay(elem.children, includeCursor,
                                            includeSpanIdx, includeOccs);

        // Skipping __VA_OPT__ consumes no A tokens.  If both branches match
        // without consuming anything, the A-side replay is ambiguous; if the
        // include branch consumes tokens/spans, prefer it because those
        // tokens are concrete evidence that the payload was exposed in A.
        if (includeOK &&
            (includeCursor != cursor || includeSpanIdx != spanIdx)) {
          cursor = includeCursor;
          spanIdx = includeSpanIdx;
          occs = std::move(includeOccs);
        }
        break;
      }
      }
    }
    return true;
  };

  uint64_t aCursor = cover->first;
  size_t spanIdx = 0;
  std::vector<ReplayAOcc> aOccs;
  if (!matchAReplay(pattern, aCursor, spanIdx, aOccs) ||
      aCursor != cover->second || spanIdx != standardSpans.size())
    return std::nullopt;

  // Map the proven A replay cover to the B-side envelope that must be
  // segmented by the same replacement-list tree.  There is intentionally no
  // fixed token-count cutoff here: large empty-slot insertions and VA_OPT
  // flips are still finite replay problems.  Determinism is enforced by the
  // later solution ranking/ambiguity checks rather than by silently refusing
  // otherwise provable envelopes.
  std::optional<std::pair<size_t, size_t>> bEnv;
  if (hasLeftBoundaryDefinitionTapeSlide) {
    // Use the producer-recorded macro expansion byte envelope, not the
    // token-cover envelope.  The token-cover mapper sees the LCS-selected
    // pure insertion before the cover; the PP-byte envelope begins at the
    // first token produced by this macro invocation and therefore lets the
    // definition tape decide which identical boundary literal is fixed macro
    // body and which token belongs to the rewritten formal.
    bEnv = (*deps_.sourceMapper)
               .MapAByteRangeToBTokenEnvelope(
                   static_cast<size_t>(*m.invPPByteBegin),
                   static_cast<size_t>(*m.invPPByteEnd));

    // On some platforms the token LCS can slide the inserted token even
    // farther left within the same run of identical punctuation.  In the
    // motivating shape
    //
    //   if (M(x))        with        #define M(x) ((x) >= 0)
    //
    // rewriting the actual to `(y).field` creates four adjacent `(` tokens in
    // the preprocessed output: the source `if` condition, two fixed macro
    // body literals, and the new first actual token.  The token diff may mark
    // the first token in that run as the pure insertion.  The byte-level macro
    // envelope still starts at the producer-recorded macro expansion, but the
    // ordinary byte mapper can then start one fixed literal too far to the
    // right.  When the post-insertion frontier is followed by a proven prefix
    // of fixed replacement-list literals, shift the replay envelope back to
    // that frontier.  This does not accept ownership by proximity: the
    // definition-tape solver below must still replay the entire adjusted
    // envelope and reconstruct one concrete invocation rewrite.
    if (bEnv && h.bEnd <= static_cast<uint64_t>(bEnv->first) &&
        h.bEnd > h.bStart && h.bEnd <= deps_.bToks.size()) {
      SmallVector<StringRef, 8> fixedLiteralPrefix;
      for (const ReplayElem &elem : pattern) {
        if (elem.kind != ReplayElem::Kind::Literal)
          break;
        fixedLiteralPrefix.push_back(elem.spelling);
      }

      const size_t postInsertionFrontier = static_cast<size_t>(h.bEnd);
      const size_t skippedPrefixWidth = bEnv->first - postInsertionFrontier;
      bool canReanchorAtPostInsertionFrontier =
          skippedPrefixWidth > 0 &&
          skippedPrefixWidth <= fixedLiteralPrefix.size();

      // Require the slid insertion itself to have the same spelling as the
      // first fixed literal.  Otherwise this is not an ambiguous run of
      // identical replacement-list punctuation and the ordinary byte envelope
      // remains the only proven replay surface.
      if (canReanchorAtPostInsertionFrontier) {
        if (h.bStart >= deps_.bToks.size() || fixedLiteralPrefix.empty() ||
            deps_.bToks[static_cast<size_t>(h.bStart)].spelling !=
                fixedLiteralPrefix.front()) {
          canReanchorAtPostInsertionFrontier = false;
        }
      }

      for (size_t i = 0;
           canReanchorAtPostInsertionFrontier && i < skippedPrefixWidth; ++i) {
        if (postInsertionFrontier + i >= deps_.bToks.size() ||
            deps_.bToks[postInsertionFrontier + i].spelling !=
                fixedLiteralPrefix[i]) {
          canReanchorAtPostInsertionFrontier = false;
          break;
        }
      }

      if (canReanchorAtPostInsertionFrontier) {
        if (inTraceMode()) {
          REFOLD_LOG_TRACE(
              "macro/template",
              "definition replay left-boundary reanchor: macro id={0} "
              "name={1} hunkB=[{2},{3}) oldBtok=[{4},{5}) "
              "newBtok=[{6},{5}) skippedFixedPrefix={7}",
              m.id, m.name, h.bStart, h.bEnd, bEnv->first, bEnv->second,
              postInsertionFrontier, skippedPrefixWidth);
        }
        bEnv->first = postInsertionFrontier;
      }
    }

    if (inTraceMode() && bEnv) {
      REFOLD_LOG_TRACE(
          "macro/template",
          "definition replay left-boundary slide: macro id={0} name={1} "
          "hunkA=[{2},{3}) hunkB=[{4},{5}) ppBytes=[{6},{7}) "
          "Btok=[{8},{9})",
          m.id, m.name, h.aStart, h.aEnd, h.bStart, h.bEnd, *m.invPPByteBegin,
          *m.invPPByteEnd, bEnv->first, bEnv->second);
    }
  } else {
    bEnv = (*deps_.sourceMapper)
               .MapATokRangeAToBTokenEnvelopePreserveBoundaryInsertions(
                   cover->first, cover->second);
  }
  if (!bEnv || bEnv->first > bEnv->second || bEnv->second > deps_.bToks.size())
    return std::nullopt;
  // Count the old A-token contribution per formal.  The B replay uses this
  // to distinguish true empty-formal insertions, where a zero-length B range
  // is legal, from ordinary non-empty formals, where assigning no B tokens
  // would silently erase source structure.
  std::vector<unsigned> oldTokenCountByFormal(invArgRanges.size(), 0);
  for (const ReplayAOcc &occ : aOccs) {
    if (occ.argIdx < oldTokenCountByFormal.size())
      oldTokenCountByFormal[occ.argIdx] +=
          static_cast<unsigned>(occ.aEnd - occ.aBegin);
  }

  // Use the file-scope B-assignment carrier so replay enumeration and scoring
  // helpers can become named operations without carrying a function-local
  // result type.
  using ReplaySolution = DefinitionTapeReplaySolution;

  ReplaySolution seed;
  seed.ranges.resize(invArgRanges.size(), {0, 0});
  seed.assigned.resize(invArgRanges.size(), 0);
  std::vector<ReplaySolution> solutions;

  // Assign a candidate B-token interval to a formal.  If the formal appears
  // multiple times in the replacement list, every occurrence must spell the
  // same trimmed B text; otherwise one call-site argument could not satisfy
  // all replayed occurrences.
  auto assignFormalRange = [&](ReplaySolution &sol, uint32_t argIdx,
                               std::pair<size_t, size_t> range) -> bool {
    if (argIdx >= sol.ranges.size() || range.second < range.first)
      return false;
    if (sol.assigned[argIdx]) {
      StringRef oldText =
          (*deps_.sourceMapper)
              .SliceBSource(sol.ranges[argIdx].first, sol.ranges[argIdx].second)
              .trim();
      StringRef newText =
          (*deps_.sourceMapper).SliceBSource(range.first, range.second).trim();
      return oldText == newText;
    }
    sol.assigned[argIdx] = 1;
    sol.ranges[argIdx] = range;
    return true;
  };

  // Exhaustively segment the B envelope according to the same replay tree.
  // Literal nodes consume fixed tokens, parameter nodes choose a token range
  // for the corresponding formal, and __VA_OPT__ tries both the erased and
  // exposed branches.  Every recursive step either advances the pattern or
  // advances the B cursor, so the search is finite for a finite token
  // envelope. Ambiguity is handled after enumeration by scoring and tie
  // rejection; the solver must not pick an arbitrary partition just because
  // it is found first.
  std::function<void(ArrayRef<ReplayElem>, size_t, size_t, ReplaySolution &,
                     std::function<void(size_t, ReplaySolution &)>)>
      dfsElems;
  dfsElems = [&](ArrayRef<ReplayElem> elems, size_t elemIdx, size_t bPos,
                 ReplaySolution &sol,
                 std::function<void(size_t, ReplaySolution &)> done) {
    if (elemIdx == elems.size()) {
      done(bPos, sol);
      return;
    }

    const ReplayElem &elem = elems[elemIdx];
    switch (elem.kind) {
    case ReplayElem::Kind::Literal:
      if (bPos < bEnv->second && deps_.bToks[bPos].spelling == elem.spelling)
        dfsElems(elems, elemIdx + 1, bPos + 1, sol, done);
      return;
    case ReplayElem::Kind::Param: {
      // Only empty old formals and variadic formals may be assigned an empty
      // B interval.  A zero-token assignment for an ordinary non-variadic
      // formal would silently erase required source structure, but an
      // explicit empty variadic actual is a valid source spelling (`M(x, )`)
      // whose surrounding punctuation is replayed by the definition tape.
      const bool oldWasEmpty = elem.argIdx < oldTokenCountByFormal.size() &&
                               oldTokenCountByFormal[elem.argIdx] == 0;
      const bool variadicFormal =
          elem.argIdx < m.defParams.size() && m.defParams[elem.argIdx].variadic;
      for (size_t end = bPos; end <= bEnv->second; ++end) {
        if (!oldWasEmpty && !variadicFormal && end == bPos)
          continue;
        ReplaySolution next = sol;
        if (!assignFormalRange(next, elem.argIdx, {bPos, end}))
          continue;
        dfsElems(elems, elemIdx + 1, end, next, done);
      }
      return;
    }
    case ReplayElem::Kind::VaOpt: {
      // Erased branch.
      dfsElems(elems, elemIdx + 1, bPos, sol, done);

      // Exposed branch.  The payload must consume at least one B token; an
      // empty exposed payload is indistinguishable from the erased branch
      // here.
      ReplaySolution withPayload = sol;
      const size_t payloadBegin = bPos;
      dfsElems(elem.children, 0, bPos, withPayload,
               [&](size_t payloadEnd, ReplaySolution &afterPayload) {
                 if (payloadEnd == payloadBegin)
                   return;
                 ReplaySolution cont = afterPayload;
                 ++cont.vaOptIncludedCount;
                 dfsElems(elems, elemIdx + 1, payloadEnd, cont, done);
               });
      return;
    }
    }
  };

  dfsElems(pattern, 0, bEnv->first, seed,
           [&](size_t finalPos, ReplaySolution &sol) {
             if (finalPos == bEnv->second)
               solutions.push_back(sol);
           });

  if (solutions.empty())
    return std::nullopt;

  // Multiple raw B partitions may still describe the same semantic replay.
  // The rest of this method partitions the collected `solutions` by their
  // semantic definition-tape obligations first, and only applies canonical
  // preference (spelling/score) after the surviving equivalence class is
  // known.  This is intentionally different from a score-first solver:
  // score/spelling can pick a representative, but cannot prove that two
  // replay witnesses are equivalent.

  // Trimmed source spelling of a formal slot in the original invocation; used
  // only for deterministic scoring and no-op detection after B replay.
  auto formalSourceTrim = [&](uint32_t idx) -> StringRef {
    if (idx >= invArgRanges.size())
      return StringRef();
    auto r = invArgRanges[idx];
    if (r.second < r.first || r.second > baseInvText.size())
      return StringRef();
    return baseInvText.slice(r.first, r.second).trim();
  };

  // Replacement-list profile shared by every replay solution for this
  // definition.  The file-scope carrier records the semantic tape surface
  // without mentioning the candidate's source spelling.
  DefinitionTapeProfile tapeProfile;

  std::vector<uint64_t> replacementUseCount(invArgRanges.size(), 0);
  std::function<void(ArrayRef<ReplayElem>)> collectTapeProfile =
      [&](ArrayRef<ReplayElem> elems) {
        for (const ReplayElem &elem : elems) {
          switch (elem.kind) {
          case ReplayElem::Kind::Literal:
            ++tapeProfile.literalCount;
            break;
          case ReplayElem::Kind::Param:
            ++tapeProfile.paramUseCount;
            if (elem.argIdx < replacementUseCount.size())
              ++replacementUseCount[elem.argIdx];
            break;
          case ReplayElem::Kind::VaOpt:
            ++tapeProfile.vaOptNodeCount;
            collectTapeProfile(elem.children);
            break;
          }
        }
      };
  collectTapeProfile(pattern);

  for (uint64_t useCount : replacementUseCount) {
    if (useCount == 0)
      ++tapeProfile.unusedFormalCount;
    else if (useCount > 1)
      ++tapeProfile.duplicatedFormalCount;
  }
  for (size_t i = 0; i < invArgRanges.size(); ++i) {
    if (formalSourceTrim(static_cast<uint32_t>(i)).empty())
      ++tapeProfile.emptySourceSlotCount;
  }
  for (const ReplayAOcc &occ : aOccs) {
    if (occ.aBegin == occ.aEnd)
      ++tapeProfile.zeroTokenAOccurrenceCount;
  }

  // Use the file-scope score carrier so deterministic replay ranking does not
  // depend on a function-local type.
  using ScoredSolution = DefinitionTapeScoredSolution;

  // Convert a B replay assignment back into concrete call-site text.  This
  // edits parsed formal slots in the original invocation spelling rather than
  // emitting expansion text, preserving the macro call when the replay proof
  // determines a unique replacement for each produced slot.  Unused formals
  // are intentionally left untouched because they have no producer occurrence
  // in the definition tape.
  auto buildReplayInvocation =
      [&](const ReplaySolution &sol) -> std::optional<std::string> {
    using LocalEdit = DefinitionTapeInvocationEdit;
    SmallVector<LocalEdit, 8> edits;

    for (uint32_t i = 0; i < invArgRanges.size(); ++i) {
      if (i >= sol.assigned.size())
        return std::nullopt;
      std::string repl;
      if (sol.assigned[i])
        repl = (*deps_.sourceMapper)
                   .SliceBSource(sol.ranges[i].first, sol.ranges[i].second)
                   .trim()
                   .str();
      else if (i < m.defParams.size() && m.defParams[i].variadic)
        repl = "";
      else
        // A non-variadic formal that is absent from the replacement-list tape
        // is an unused macro parameter.  Definition-tape replay has no
        // producer edge that could justify changing it, so the only
        // owner-closed witness is to preserve the original argument spelling.
        // This makes unused formals explicit in the witness model instead of
        // rejecting otherwise valid replays of the used tape.
        continue;

      auto r = invArgRanges[i];
      if (r.second < r.first || r.second > baseInvText.size())
        return std::nullopt;

      if (StringRef(repl).trim() == baseInvText.slice(r.first, r.second).trim())
        continue;
      if (i < m.defParams.size() && !m.defParams[i].variadic && !repl.empty() &&
          replacementIntroducesTopLevelComma(repl, (*deps_.lexLang)))
        return std::nullopt;

      size_t editBegin = r.first;
      size_t editEnd = r.second;
      std::string editText = StringRef(repl).trim().str();

      // Variadic tail edits may need to create or remove the separating comma
      // in the invocation spelling.  For insertion into an empty tail, add
      // the comma with the new text; for erasure, widen the edit leftward to
      // the existing comma so `M(x, y)` becomes `M(x)`, not `M(x, )`.
      const bool isTrailingVariadic = i + 1 == invArgRanges.size() &&
                                      i < m.defParams.size() &&
                                      m.defParams[i].variadic;
      const bool assignedExplicitEmptyVariadic =
          isTrailingVariadic && sol.assigned[i] &&
          sol.ranges[i].first == sol.ranges[i].second;
      if (isTrailingVariadic && r.first == r.second && !editText.empty()) {
        editText = (", " + editText);
      } else if (isTrailingVariadic &&
                 !baseInvText.slice(r.first, r.second).empty() &&
                 editText.empty() && !assignedExplicitEmptyVariadic) {
        size_t prevEnd = 0;
        if (i > 0)
          prevEnd = invArgRanges[i - 1].second;
        size_t comma = StringRef::npos;
        for (size_t pos = r.first; pos > prevEnd; --pos) {
          if (baseInvText[pos - 1] == ',') {
            comma = pos - 1;
            break;
          }
        }
        if (comma != StringRef::npos)
          editBegin = comma;
      }

      edits.push_back(LocalEdit{editBegin, editEnd, std::move(editText)});
    }

    if (edits.empty())
      return std::nullopt;
    llvm::sort(edits, [](const LocalEdit &lhs, const LocalEdit &rhs) {
      if (lhs.begin != rhs.begin)
        return lhs.begin > rhs.begin;
      return lhs.end > rhs.end;
    });

    std::string rewritten = baseInvText.str();
    size_t previousBegin = std::numeric_limits<size_t>::max();
    for (const LocalEdit &edit : edits) {
      if (edit.end < edit.begin || edit.end > rewritten.size())
        return std::nullopt;
      if (previousBegin != std::numeric_limits<size_t>::max() &&
          edit.end > previousBegin)
        return std::nullopt;
      previousBegin = edit.begin;
      rewritten =
          stringutils::replaceRange(rewritten, edit.begin, edit.end, edit.repl);
    }
    return StringRef(rewritten).trim().str();
  };

  const bool hasVariadicFormal =
      llvm::any_of(m.defParams, macroDefParamIsVariadic);

  std::optional<uint32_t> variadicFormalIndex;
  for (uint32_t i = 0; i < m.defParams.size(); ++i) {
    if (m.defParams[i].variadic) {
      variadicFormalIndex = i;
      break;
    }
  }

  const bool hasVaOptCommaPayload = [&]() {
    std::function<bool(ArrayRef<ReplayElem>)> containsVaOptComma =
        [&](ArrayRef<ReplayElem> elems) -> bool {
      for (const ReplayElem &elem : elems) {
        if (elem.kind == ReplayElem::Kind::VaOpt) {
          for (const ReplayElem &child : elem.children)
            if (child.kind == ReplayElem::Kind::Literal &&
                child.spelling == ",")
              return true;
          if (containsVaOptComma(elem.children))
            return true;
        }
      }
      return false;
    };
    return containsVaOptComma(pattern);
  }();

  const bool hasGnuVariadicCommaPaste = [&]() {
    if (!variadicFormalIndex)
      return false;
    const auto &toks = definition->replacementTokens;
    for (size_t i = 0; i < toks.size(); ++i) {
      if (toks[i].spelling != "##")
        continue;
      const bool leftComma =
          i > 0 &&
          toks[i - 1].kind == RefoldModel::MacroReplacementTokenKind::Literal &&
          toks[i - 1].spelling == ",";
      const bool rightVariadic =
          i + 1 < toks.size() &&
          toks[i + 1].kind ==
              RefoldModel::MacroReplacementTokenKind::ParamRef &&
          toks[i + 1].paramIndex &&
          *toks[i + 1].paramIndex == *variadicFormalIndex;
      if (leftComma && rightVariadic)
        return true;
    }
    return false;
  }();

  const size_t parsedActualCount = [&]() -> size_t {
    auto parsed = RefoldArgTextRecovery::LexMacroInvocationActualContentRanges(
        baseInvText, (*deps_.lexLang));
    return parsed ? parsed->size() : invArgRanges.size();
  }();

  auto originalVariadicState = [&]() {
    DefinitionTapeVariadicState state;
    if (!variadicFormalIndex)
      return state;
    const uint32_t idx = *variadicFormalIndex;
    const StringRef text = formalSourceTrim(idx);
    state.missing = parsedActualCount <= idx;
    state.explicitEmpty = !state.missing && text.empty();
    state.nonEmpty = !text.empty();
    state.literalComma = state.nonEmpty && replacementIntroducesTopLevelComma(
                                               text, (*deps_.lexLang));
    return state;
  }();

  auto variadicStateSignatureForSolution = [&](const ReplaySolution &sol,
                                               unsigned vaOptIncludedCount) {
    if (!variadicFormalIndex)
      return std::string("non-variadic");
    const uint32_t idx = *variadicFormalIndex;
    const bool assigned = idx < sol.assigned.size() && sol.assigned[idx];
    const bool resultMissing = !assigned;
    const bool resultExplicitEmpty =
        assigned && sol.ranges[idx].first == sol.ranges[idx].second;
    const bool resultNonEmpty =
        assigned && sol.ranges[idx].first < sol.ranges[idx].second;
    const std::string resultText =
        assigned
            ? (*deps_.sourceMapper)
                  .SliceBSource(sol.ranges[idx].first, sol.ranges[idx].second)
                  .trim()
                  .str()
            : std::string();
    const bool literalComma =
        !resultText.empty() && replacementIntroducesTopLevelComma(
                                   StringRef(resultText), (*deps_.lexLang));
    const bool vaOptResultActive = hasVaOpt && vaOptIncludedCount != 0;
    const bool vaOptOriginallyActive =
        hasVaOpt && originalVariadicState.nonEmpty;
    const bool commaInserted =
        !originalVariadicState.nonEmpty && resultNonEmpty;
    const bool commaDeleted = originalVariadicState.nonEmpty && !resultNonEmpty;
    return llvm::formatv(
               "variadic:formal={0}:orig_missing={1}:orig_empty={2}:"
               "orig_nonempty={3}:orig_litcomma={4}:result_missing={5}:"
               "result_empty={6}:result_nonempty={7}:result_litcomma={8}:"
               "comma_inserted={9}:comma_deleted={10}:gnu_elision={11}:"
               "vaopt={12}:vaopt_orig={13}:vaopt_result={14}:"
               "vaopt_comma_ins={15}:vaopt_comma_del={16}:"
               "vaopt_nodes={17}:vaopt_included={18}:range=[{19},{20})",
               idx, originalVariadicState.missing ? 1 : 0,
               originalVariadicState.explicitEmpty ? 1 : 0,
               originalVariadicState.nonEmpty ? 1 : 0,
               originalVariadicState.literalComma ? 1 : 0,
               resultMissing ? 1 : 0, resultExplicitEmpty ? 1 : 0,
               resultNonEmpty ? 1 : 0, literalComma ? 1 : 0,
               commaInserted ? 1 : 0, commaDeleted ? 1 : 0,
               hasGnuVariadicCommaPaste ? 1 : 0, hasVaOpt ? 1 : 0,
               vaOptOriginallyActive ? 1 : 0, vaOptResultActive ? 1 : 0,
               (hasVaOptCommaPayload && !vaOptOriginallyActive &&
                vaOptResultActive)
                   ? 1
                   : 0,
               (hasVaOptCommaPayload && vaOptOriginallyActive &&
                !vaOptResultActive)
                   ? 1
                   : 0,
               tapeProfile.vaOptNodeCount, vaOptIncludedCount,
               assigned ? sol.ranges[idx].first : 0,
               assigned ? sol.ranges[idx].second : 0)
        .str();
  };

  auto makeVariadicCommaWitnessForSolution =
      [&](const ReplaySolution &sol,
          unsigned vaOptIncludedCount) -> std::optional<VariadicCommaWitness> {
    if (!variadicFormalIndex)
      return std::nullopt;
    const uint32_t idx = *variadicFormalIndex;
    VariadicCommaWitness witness;
    witness.rootMacroId = m.id;
    witness.variadicFormalIndex = idx;
    witness.arityStable = true;
    witness.originalMissing = originalVariadicState.missing;
    witness.originalExplicitEmpty = originalVariadicState.explicitEmpty;
    witness.originalNonEmpty = originalVariadicState.nonEmpty;
    witness.literalCommaInActual = originalVariadicState.literalComma;

    const bool assigned = idx < sol.assigned.size() && sol.assigned[idx];
    witness.resultMissing = !assigned;
    witness.resultExplicitEmpty =
        assigned && sol.ranges[idx].first == sol.ranges[idx].second;
    witness.resultNonEmpty =
        assigned && sol.ranges[idx].first < sol.ranges[idx].second;
    if (assigned && witness.resultNonEmpty) {
      std::string text =
          (*deps_.sourceMapper)
              .SliceBSource(sol.ranges[idx].first, sol.ranges[idx].second)
              .trim()
              .str();
      witness.literalCommaInActual |=
          replacementIntroducesTopLevelComma(StringRef(text), (*deps_.lexLang));
    }
    witness.commaInserted = !witness.originalNonEmpty && witness.resultNonEmpty;
    witness.commaDeleted = witness.originalNonEmpty && !witness.resultNonEmpty;
    witness.gnuCommaElision = hasGnuVariadicCommaPaste;
    witness.vaOptPresent = hasVaOpt;
    witness.vaOptOriginallyActive = hasVaOpt && witness.originalNonEmpty;
    witness.vaOptResultActive = hasVaOpt && vaOptIncludedCount != 0;
    witness.vaOptCommaIntroduced = hasVaOptCommaPayload &&
                                   !witness.vaOptOriginallyActive &&
                                   witness.vaOptResultActive;
    witness.vaOptCommaDeleted = hasVaOptCommaPayload &&
                                witness.vaOptOriginallyActive &&
                                !witness.vaOptResultActive;
    witness.vaOptNodeCount = static_cast<uint32_t>(tapeProfile.vaOptNodeCount);
    witness.vaOptIncludedCount = vaOptIncludedCount;
    witness.producerSignature = "{Forward,VariadicForward";
    if (witness.resultMissing)
      witness.producerSignature += ",VariadicMissing";
    if (witness.resultExplicitEmpty)
      witness.producerSignature += ",VariadicEmpty";
    if (witness.commaInserted || witness.vaOptCommaIntroduced)
      witness.producerSignature += ",VariadicCommaInsertion";
    if (witness.commaDeleted || witness.gnuCommaElision ||
        witness.vaOptCommaDeleted)
      witness.producerSignature += ",VariadicCommaElision";
    if (witness.vaOptPresent)
      witness.producerSignature +=
          witness.vaOptResultActive ? ",VaOptActivation" : ",VaOptErasure";
    witness.producerSignature += "}";
    witness.packStateSignature =
        variadicStateSignatureForSolution(sol, vaOptIncludedCount);
    return witness;
  };

  auto producerObligationKeyForSolution = [&](const ReplaySolution &sol) {
    bool hasForward = false;
    bool hasVariadicForward = false;
    for (size_t i = 0; i < sol.assigned.size(); ++i) {
      if (!sol.assigned[i])
        continue;
      if (i < m.defParams.size() && m.defParams[i].variadic)
        hasVariadicForward = true;
      else
        hasForward = true;
    }

    std::string out = "{";
    bool needComma = false;
    auto add = [&](StringRef name) {
      if (needComma)
        out += ",";
      out += name.str();
      needComma = true;
    };
    if (hasForward)
      add("Forward");
    if (hasVariadicForward)
      add("VariadicForward");
    if (!needComma)
      add("PreserveUnusedOnly");
    out += "}";
    return out;
  };

  auto equivalenceKeyForSolution = [&](const ScoredSolution &scored) {
    // Every valid candidate has replayed the same recorded replacement-list
    // tree over the same A cover and exactly segmented the same B envelope.
    // The semantic key therefore records the target PP envelope, the producer
    // obligations, and the tape features that affect proof obligations.  It
    // intentionally omits source spelling, slot byte ranges, and canonical
    // score.  Those belong to chooseCanonical(), not to proof equivalence.
    std::string key =
        llvm::formatv("definition_tape:def={0}:root={1}:b=[{2},{3}):"
                      "producer={4}:boundary=root-invocation:"
                      "literals={5}:param_uses={6}:duplicated={7}:"
                      "unused={8}:empty_slots={9}:zero_a_occs={10}",
                      definition->id, m.id, bEnv->first, bEnv->second,
                      producerObligationKeyForSolution(scored.sol),
                      tapeProfile.literalCount, tapeProfile.paramUseCount,
                      tapeProfile.duplicatedFormalCount,
                      tapeProfile.unusedFormalCount,
                      tapeProfile.emptySourceSlotCount,
                      tapeProfile.zeroTokenAOccurrenceCount)
            .str();

    // variadic and __VA_OPT__ replay use an explicit producer
    // profile. Missing pack, explicit empty pack, non-empty forwarding,
    // source-level comma insertion/deletion, GNU comma elision, literal
    // commas inside the pack, and VA_OPT activation are deliberately distinct
    // equivalence dimensions.
    if (hasVariadicFormal || hasVaOpt)
      key += ":" + variadicStateSignatureForSolution(scored.sol,
                                                     scored.vaOptIncludedCount);
    return key;
  };

  std::vector<ScoredSolution> validSolutions;
  for (const ReplaySolution &sol : solutions) {
    auto rewritten = buildReplayInvocation(sol);
    if (!rewritten)
      continue;

    ScoredSolution scored;
    scored.sol = sol;
    scored.rewritten = std::move(*rewritten);
    scored.vaOptIncludedCount = sol.vaOptIncludedCount;
    for (uint32_t i = 0; i < invArgRanges.size(); ++i) {
      if (!sol.assigned[i]) {
        if (!(i < m.defParams.size() && m.defParams[i].variadic))
          ++scored.unusedFormalPreservedCount;
        continue;
      }

      const unsigned oldN =
          i < oldTokenCountByFormal.size() ? oldTokenCountByFormal[i] : 0;
      const unsigned newN =
          static_cast<unsigned>(sol.ranges[i].second - sol.ranges[i].first);
      if (newN == 0)
        ++scored.zeroTokenAssignedFormalCount;
      if (formalSourceTrim(i).empty()) {
        scored.emptySlotTokenCount += newN;
      } else if (oldN > newN) {
        scored.nonEmptyDeviation += oldN - newN;
      } else {
        scored.nonEmptyDeviation += newN - oldN;
      }
    }
    scored.equivalenceKey = equivalenceKeyForSolution(scored);
    validSolutions.push_back(std::move(scored));
  }

  if (validSolutions.empty())
    return std::nullopt;

  std::map<std::string, std::vector<const ScoredSolution *>> equivalenceClasses;
  for (const ScoredSolution &scored : validSolutions)
    equivalenceClasses[scored.equivalenceKey].push_back(&scored);

  if ((*deps_.proofLattice).WitnessTrace().ShouldEmitProofLog()) {
    (*deps_.proofLattice)
        .WitnessTrace()
        .TraceWitnessAmbiguity("MacroActualDefinitionTapeReplay",
                               solutions.size(), validSolutions.size(),
                               equivalenceClasses.size());
  }

  // makes the variadic definition-tape partition authoritative when
  // the refined variadic/VA_OPT key leaves exactly one semantic class.  If a
  // variadic replay still exposes multiple non-equivalent classes, keep the
  // deterministic representative rather than guessing through this local gate;
  // the global resolver decides how such cross-class fallbacks compose with
  // weaker proof families.
  const bool definitionTapeEquivalenceAuthoritative =
      (!hasVariadicFormal && !hasVaOpt) || equivalenceClasses.size() == 1;
  if (definitionTapeEquivalenceAuthoritative &&
      equivalenceClasses.size() != 1) {
    if ((*deps_.proofLattice).WitnessTrace().ShouldEmitProofLog()) {
      RefoldWitness witness;
      witness.family = WitnessProofFamily::DefinitionTapeReplay;
      witness.owner = llvm::formatv("macro#{0}", m.id).str();
      witness.detail = llvm::formatv("definition={0} b=[{1},{2}) classes={3}",
                                     definition->id, bEnv->first, bEnv->second,
                                     equivalenceClasses.size())
                           .str();
      (*deps_.proofLattice)
          .WitnessTrace()
          .TraceWitnessRejected(
              witness, WitnessRejectReason::NonEquivalentAmbiguity,
              "definition-tape replay produced multiple semantic classes");
    }
    return std::nullopt;
  }

  const std::vector<const ScoredSolution *> *selectionPool = nullptr;
  if (definitionTapeEquivalenceAuthoritative) {
    selectionPool = &equivalenceClasses.begin()->second;
  }

  const ScoredSolution *best = nullptr;
  if (selectionPool) {
    best = selectionPool->front();
    for (const ScoredSolution *scored : *selectionPool) {
      if (scored != best && definitionTapeScoredSolutionLess(*scored, *best))
        best = scored;
    }
  } else {
    best = &validSolutions.front();
    for (size_t i = 1; i < validSolutions.size(); ++i) {
      const ScoredSolution &scored = validSolutions[i];
      if (definitionTapeScoredSolutionLess(scored, *best))
        best = &scored;
    }
  }

  if (StringRef(best->rewritten).trim() == baseInvText.trim())
    return std::nullopt;

  MacroPatch patch{*m.invB, *m.invE, best->rewritten, m.id};
  // The replacement text is the full rewritten invocation, so the
  // materialized output range covers the replacement string.  The B-token
  // proof range remains the mapped expansion envelope certified below.
  patch.materialized.outputByteStart = 0;
  patch.materialized.outputByteEnd = patch.replacement.size();
  patch.materialized.hasOutputByteRange = true;
  certifyMacroPatchMaterializedBTokenRange(patch,
                                           static_cast<uint64_t>(bEnv->first),
                                           static_cast<uint64_t>(bEnv->second));
  // Attach the args-only standard proof directly through the proof lattice so
  // this service does not depend on planner-owned proof stamping helpers.
  {
    MacroPatchProof proof =
        (*deps_.proofLattice)
            .MakeMacroPatchProof(MacroPatchProofKind::ArgsOnlyStandard,
                                 /*preservesInvocationStructure=*/true, m.id);
    WholeEnvelopeReplayWitness witness;
    witness.rootMacroId = m.id;
    witness.replayValidated = true;
    witness.definitionTapeReplayValidated = true;
    proof.wholeEnvelopeReplay = witness;
    (*deps_.proofLattice).SetMacroPatchProof(patch, std::move(proof));
  }
  if (auto variadicWitness = makeVariadicCommaWitnessForSolution(
          best->sol, static_cast<unsigned>(best->vaOptIncludedCount))) {
    MacroPatchProof proof = patch.proof;
    proof.variadicCommaReplay = std::move(*variadicWitness);
    (*deps_.proofLattice).SetMacroPatchProof(patch, std::move(proof));
  }

  // definition-tape replay is the producer proof for empty actuals
  // and zero-token replacement-list gaps.  When this accepted replay used a
  // zero-width A occurrence or assigned B tokens to an originally empty
  // source slot, carry that fact as a zero-token boundary witness.  This is
  // trace/equivalence metadata only; the exact same replay and canonical
  // representative selected above remain authoritative for the emitted text.
  if (tapeProfile.emptySourceSlotCount != 0 ||
      tapeProfile.zeroTokenAOccurrenceCount != 0 ||
      best->zeroTokenAssignedFormalCount != 0) {
    MacroPatchProof proof = patch.proof;
    ZeroTokenBoundaryWitness witness;
    witness.ownerId = m.id;
    witness.ownerKind = "macro";
    witness.hasSourceAnchor = true;
    witness.sourceAnchor = *m.invB;
    witness.hasBTokenRange = true;
    witness.bTokStart = static_cast<uint64_t>(bEnv->first);
    witness.bTokEnd = static_cast<uint64_t>(bEnv->second);
    witness.producerProven = true;
    witness.ownerClosed = true;
    witness.layoutStable = true;
    witness.observersStable = true;
    witness.counterStable = true;
    witness.fromEmptyActual = tapeProfile.emptySourceSlotCount != 0;
    witness.fromReplacementGap = tapeProfile.zeroTokenAOccurrenceCount != 0;
    witness.boundarySignature =
        llvm::formatv(
            "definition-tape-zero-token:def={0}:macro={1}:b=[{2},{3}):"
            "empty_slots={4}:zero_a_occs={5}:zero_assigned={6}:"
            "producer={7}",
            definition->id, m.id, bEnv->first, bEnv->second,
            tapeProfile.emptySourceSlotCount,
            tapeProfile.zeroTokenAOccurrenceCount,
            best->zeroTokenAssignedFormalCount,
            producerObligationKeyForSolution(best->sol))
            .str();
    proof.zeroTokenBoundaryReplay = std::move(witness);
    (*deps_.proofLattice).SetMacroPatchProof(patch, std::move(proof));
  }
  return patch;
}

} // namespace refold
} // namespace clang
