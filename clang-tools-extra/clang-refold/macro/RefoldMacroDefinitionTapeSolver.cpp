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
#include "llvm/ADT/STLFunctionalExtras.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/FormatVariadic.h"

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
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
/// not record a corresponding `#define` site.
const RefoldModel::MacroDirective *
getDefinitionDirectiveForInvocation(const RefoldModel *model,
                                    const RefoldModel::MacroInvocation &m) {
  if (!m.definitionDirectiveId)
    return nullptr;
  return model->GetMacroDirectiveById(*m.definitionDirectiveId);
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

/// Return whether the parsed definition replay tree contains any `__VA_OPT__`
/// node.
///
/// Proof obligation: this is the pure tree query that decides whether the
/// definition-tape solver must stay enabled for a possible VA_OPT branch flip.
/// The trusted input is the already parsed replacement-list replay tree; this
/// helper does not inspect source text, candidate edits, or proof state.  It
/// does not make an ambiguity decision itself, but preserving the complete
/// pre-order walk keeps the solver's fail-closed gate tied to exactly the same
/// tree surface as the former local recursive query.
bool definitionTapePatternHasVaOpt(ArrayRef<DefinitionTapeReplayElem> elems) {
  bool hasVaOpt = false;
  for (const DefinitionTapeReplayElem &elem : elems) {
    if (elem.kind == DefinitionTapeReplayElem::Kind::VaOpt)
      hasVaOpt = true;
    if (definitionTapePatternHasVaOpt(elem.children))
      hasVaOpt = true;
  }
  return hasVaOpt;
}

/// Accumulate replay-tree profile fields while preserving replacement-list
/// traversal order.
///
/// Proof obligation: count the literal, formal-use, and VA_OPT structure that
/// every later replay witness must explain.  The trusted inputs are the parsed
/// replay tree and a replacement-use vector sized to the macro's formal count.
/// Out-of-range formals are ignored exactly as before so malformed surfaces
/// remain rejected by the surrounding solver gates, not by this profile pass.
/// This helper introduces no ambiguity cutoff; it only preserves the original
/// depth-first accumulation order used before equivalence-class scoring.
void collectDefinitionTapeProfileInto(
    ArrayRef<DefinitionTapeReplayElem> elems, DefinitionTapeProfile &profile,
    std::vector<uint64_t> &replacementUseCount) {
  for (const DefinitionTapeReplayElem &elem : elems) {
    switch (elem.kind) {
    case DefinitionTapeReplayElem::Kind::Literal:
      ++profile.literalCount;
      break;
    case DefinitionTapeReplayElem::Kind::Param:
      ++profile.paramUseCount;
      if (elem.argIdx < replacementUseCount.size())
        ++replacementUseCount[elem.argIdx];
      break;
    case DefinitionTapeReplayElem::Kind::VaOpt:
      ++profile.vaOptNodeCount;
      collectDefinitionTapeProfileInto(elem.children, profile,
                                       replacementUseCount);
      break;
    }
  }
}

/// Build the definition-tape profile shared by all replay candidates.
///
/// Proof obligation: summarize the parsed replacement-list tree without
/// looking at a candidate's chosen B-token assignment.  The trusted input is a
/// validated replay tree plus the macro formal count.  Duplicate and unused
/// formal counts are derived after the same ordered tree walk, preserving the
/// scoring/proof surface used by the existing equivalence-class logic.  No
/// ambiguity is accepted here; unique-solution rejection remains in the replay
/// solver that consumes this profile.
DefinitionTapeProfile collectDefinitionTapeProfile(
    ArrayRef<DefinitionTapeReplayElem> elems, size_t formalCount) {
  DefinitionTapeProfile profile;
  std::vector<uint64_t> replacementUseCount(formalCount, 0);
  collectDefinitionTapeProfileInto(elems, profile, replacementUseCount);

  for (uint64_t useCount : replacementUseCount) {
    if (useCount == 0)
      ++profile.unusedFormalCount;
    else if (useCount > 1)
      ++profile.duplicatedFormalCount;
  }
  return profile;
}

/// Return whether any `__VA_OPT__` payload contains a literal comma.
///
/// Proof obligation: detect the proof-relevant VA_OPT comma surface that can
/// interact with variadic empty/missing actual replay.  The trusted input is the
/// already parsed replacement-list replay tree.  The helper preserves the prior
/// ordering by inspecting each VA_OPT node's immediate payload before recursing
/// into nested VA_OPT payloads.  It does not resolve ambiguity or admit a
/// candidate; fail-closed unique-solution handling remains in the caller's
/// replay-equivalence checks.
bool definitionTapeVaOptPayloadContainsComma(
    ArrayRef<DefinitionTapeReplayElem> elems) {
  for (const DefinitionTapeReplayElem &elem : elems) {
    if (elem.kind == DefinitionTapeReplayElem::Kind::VaOpt) {
      for (const DefinitionTapeReplayElem &child : elem.children) {
        if (child.kind == DefinitionTapeReplayElem::Kind::Literal &&
            child.spelling == ",")
          return true;
      }
      if (definitionTapeVaOptPayloadContainsComma(elem.children))
        return true;
    }
  }
  return false;
}

/// Parser for the definition replacement-token tape used by replay proof.
///
/// Proof/search obligation: normalize the recorded `#define` replacement list
/// into replay nodes without interpreting any candidate B-side spelling.  The
/// trusted inputs are the macro directive's recorded replacement tokens and the
/// caller-validated formal count.  Unsupported stringification and token-paste
/// nodes, malformed `__VA_OPT__` parentheses, and invalid formal references all
/// reject fail-closed here because raw definition-tape replay cannot prove those
/// transformed surfaces.  The parser appends nodes in replacement-list order and
/// recurses into `__VA_OPT__` payloads at the same point as the original local
/// parser, preserving the later matcher/enumerator ordering exactly.
class DefinitionTapeParser {
public:
  DefinitionTapeParser(const RefoldModel::MacroDirective &definition,
                       size_t formalCount)
      : definition_(definition), formalCount_(formalCount) {}

  /// Parse the entire replacement-token tape into replay nodes.
  bool Parse(std::vector<DefinitionTapeReplayElem> &out) const {
    return ParseRange(0, definition_.replacementTokens.size(), out);
  }

private:
  bool ParseRange(size_t begin, size_t end,
                  std::vector<DefinitionTapeReplayElem> &out) const {
    for (size_t i = begin; i < end;) {
      const RefoldModel::MacroReplacementToken &tok =
          definition_.replacementTokens[i];
      if (tok.kind == RefoldModel::MacroReplacementTokenKind::ParamRef) {
        if (!tok.paramIndex || *tok.paramIndex >= formalCount_)
          return false;
        DefinitionTapeReplayElem elem;
        elem.kind = DefinitionTapeReplayElem::Kind::Param;
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
            definition_.replacementTokens[i + 1].kind !=
                RefoldModel::MacroReplacementTokenKind::Literal ||
            definition_.replacementTokens[i + 1].spelling != "(")
          return false;

        unsigned depth = 1;
        size_t j = i + 2;
        for (; j < end; ++j) {
          const auto &inner = definition_.replacementTokens[j];
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

        DefinitionTapeReplayElem elem;
        elem.kind = DefinitionTapeReplayElem::Kind::VaOpt;
        if (!ParseRange(i + 2, j, elem.children))
          return false;
        out.push_back(std::move(elem));
        i = j + 1;
        continue;
      }

      DefinitionTapeReplayElem elem;
      elem.kind = DefinitionTapeReplayElem::Kind::Literal;
      elem.spelling = tok.spelling.str();
      out.push_back(std::move(elem));
      ++i;
    }
    return true;
  }

  const RefoldModel::MacroDirective &definition_;
  size_t formalCount_ = 0;
};

/// A-side matcher for proving the definition tape regenerates the recorded
/// producer expansion cover.
///
/// Proof/search obligation: replay the parsed definition tree over the A-token
/// cover and record standard formal occurrences in the same order as the
/// producer spans.  The trusted inputs are the parsed replay tree, the full
/// A-token stream, and caller-sorted standard PPArgSpans.  A literal mismatch,
/// out-of-order span, uncovered token, or unconsumed span rejects fail-closed.
/// `__VA_OPT__` preserves the prior branch policy: include the payload only
/// when it consumes concrete A tokens or spans; otherwise keep the erased branch
/// and leave ambiguity-sensitive acceptance to later B-side equivalence checks.
class DefinitionTapeAReplayMatcher {
public:
  DefinitionTapeAReplayMatcher(ArrayRef<DefinitionTapeReplayElem> pattern,
                               ArrayRef<PPTok> aToks,
                               ArrayRef<RefoldModel::PPArgSpan> standardSpans)
      : pattern_(pattern), aToks_(aToks), standardSpans_(standardSpans) {}

  /// Match the entire A cover and return ordered formal occurrences on success.
  bool Match(uint64_t coverBegin, uint64_t coverEnd,
             std::vector<DefinitionTapeReplayAOcc> &occs) const {
    uint64_t cursor = coverBegin;
    size_t spanIdx = 0;
    std::vector<DefinitionTapeReplayAOcc> matchedOccs;
    if (!MatchElems(pattern_, cursor, spanIdx, matchedOccs) ||
        cursor != coverEnd || spanIdx != standardSpans_.size())
      return false;
    occs = std::move(matchedOccs);
    return true;
  }

private:
  bool MatchElems(ArrayRef<DefinitionTapeReplayElem> elems, uint64_t &cursor,
                  size_t &spanIdx,
                  std::vector<DefinitionTapeReplayAOcc> &occs) const {
    for (const DefinitionTapeReplayElem &elem : elems) {
      if (spanIdx < standardSpans_.size() &&
          standardSpans_[spanIdx].begin < cursor)
        return false;

      switch (elem.kind) {
      case DefinitionTapeReplayElem::Kind::Literal:
        if (!matchLiteralAToken(aToks_, cursor, elem.spelling))
          return false;
        ++cursor;
        break;
      case DefinitionTapeReplayElem::Kind::Param: {
        DefinitionTapeReplayAOcc occ;
        occ.argIdx = elem.argIdx;
        // Start as a zero-width occurrence.  A following PPArgSpan for the
        // same formal turns this into an ordinary token-bearing occurrence;
        // otherwise the zero-width marker is the proof surface for an empty
        // source actual or a zero-token child actual.
        occ.aBegin = cursor;
        occ.aEnd = cursor;
        if (spanIdx < standardSpans_.size() &&
            standardSpans_[spanIdx].begin == cursor &&
            standardSpans_[spanIdx].argIdx == elem.argIdx) {
          occ.span = standardSpans_[spanIdx];
          occ.aBegin = standardSpans_[spanIdx].begin;
          occ.aEnd = standardSpans_[spanIdx].end;
          cursor = standardSpans_[spanIdx].end;
          ++spanIdx;
        }
        occs.push_back(std::move(occ));
        break;
      }
      case DefinitionTapeReplayElem::Kind::VaOpt: {
        uint64_t includeCursor = cursor;
        size_t includeSpanIdx = spanIdx;
        std::vector<DefinitionTapeReplayAOcc> includeOccs = occs;
        const bool includeOK = MatchElems(elem.children, includeCursor,
                                          includeSpanIdx, includeOccs);

        // Skipping __VA_OPT__ consumes no A tokens.  If the include branch
        // consumes tokens/spans, prefer it because those tokens are concrete
        // evidence that the payload was exposed in A.  A zero-width include
        // branch is indistinguishable here and remains the erased branch.
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
  }

  ArrayRef<DefinitionTapeReplayElem> pattern_;
  ArrayRef<PPTok> aToks_;
  ArrayRef<RefoldModel::PPArgSpan> standardSpans_;
};

/// B-side enumerator for assigning replacement-tape formal occurrences to the
/// target B-token envelope.
///
/// Proof/search obligation: enumerate every B-token segmentation that can be
/// replayed by the parsed definition tree, while enforcing repeated-formal
/// spelling equality.  The trusted inputs are the parsed replay tree, the
/// caller-proven B envelope, the current B tokens/source mapper, macro formal
/// metadata, and the A-side token count per formal.  This resolver deliberately
/// does not choose a winner: ambiguous semantic classes must still be rejected
/// by the downstream equivalence/scoring code.  DFS visits literals, increasing
/// formal interval ends, erased VA_OPT branches, and exposed VA_OPT payloads in
/// the exact order used by the former local recursive enumerator.
class DefinitionTapeBReplayEnumerator {
public:
  DefinitionTapeBReplayEnumerator(
      ArrayRef<DefinitionTapeReplayElem> pattern, ArrayRef<PPTok> bToks,
      const RefoldSourceMapper &sourceMapper,
      ArrayRef<RefoldModel::MacroDefParam> defParams,
      ArrayRef<unsigned> oldTokenCountByFormal,
      std::pair<size_t, size_t> bEnv, size_t formalCount)
      : pattern_(pattern), bToks_(bToks), sourceMapper_(sourceMapper),
        defParams_(defParams), oldTokenCountByFormal_(oldTokenCountByFormal),
        bEnv_(bEnv), formalCount_(formalCount) {}

  /// Enumerate all B replay solutions in deterministic DFS order.
  void Enumerate(std::vector<DefinitionTapeReplaySolution> &solutions) const {
    DefinitionTapeReplaySolution seed;
    seed.ranges.resize(formalCount_, {0, 0});
    seed.assigned.resize(formalCount_, 0);

    DfsElems(pattern_, 0, bEnv_.first, seed,
             [&](size_t finalPos, DefinitionTapeReplaySolution &sol) {
               if (finalPos == bEnv_.second)
                 solutions.push_back(sol);
             });
  }

private:
  bool AssignFormalRange(DefinitionTapeReplaySolution &sol, uint32_t argIdx,
                         std::pair<size_t, size_t> range) const {
    if (argIdx >= sol.ranges.size() || range.second < range.first)
      return false;
    if (sol.assigned[argIdx]) {
      StringRef oldText =
          sourceMapper_.SliceBSource(sol.ranges[argIdx].first,
                                     sol.ranges[argIdx].second)
              .trim();
      StringRef newText =
          sourceMapper_.SliceBSource(range.first, range.second).trim();
      return oldText == newText;
    }
    sol.assigned[argIdx] = 1;
    sol.ranges[argIdx] = range;
    return true;
  }

  void DfsElems(
      ArrayRef<DefinitionTapeReplayElem> elems, size_t elemIdx, size_t bPos,
      DefinitionTapeReplaySolution &sol,
      function_ref<void(size_t, DefinitionTapeReplaySolution &)> done) const {
    if (elemIdx == elems.size()) {
      done(bPos, sol);
      return;
    }

    const DefinitionTapeReplayElem &elem = elems[elemIdx];
    switch (elem.kind) {
    case DefinitionTapeReplayElem::Kind::Literal:
      if (bPos < bEnv_.second && bToks_[bPos].spelling == elem.spelling)
        DfsElems(elems, elemIdx + 1, bPos + 1, sol, done);
      return;
    case DefinitionTapeReplayElem::Kind::Param: {
      // Only empty old formals and variadic formals may be assigned an empty
      // B interval.  A zero-token assignment for an ordinary non-variadic
      // formal would silently erase required source structure, but an explicit
      // empty variadic actual is a valid source spelling (`M(x, )`) whose
      // surrounding punctuation is replayed by the definition tape.
      const bool oldWasEmpty = elem.argIdx < oldTokenCountByFormal_.size() &&
                               oldTokenCountByFormal_[elem.argIdx] == 0;
      const bool variadicFormal =
          elem.argIdx < defParams_.size() && defParams_[elem.argIdx].variadic;
      for (size_t end = bPos; end <= bEnv_.second; ++end) {
        if (!oldWasEmpty && !variadicFormal && end == bPos)
          continue;
        DefinitionTapeReplaySolution next = sol;
        if (!AssignFormalRange(next, elem.argIdx, {bPos, end}))
          continue;
        DfsElems(elems, elemIdx + 1, end, next, done);
      }
      return;
    }
    case DefinitionTapeReplayElem::Kind::VaOpt: {
      // Erased branch.
      DfsElems(elems, elemIdx + 1, bPos, sol, done);

      // Exposed branch.  The payload must consume at least one B token; an
      // empty exposed payload is indistinguishable from the erased branch here.
      DefinitionTapeReplaySolution withPayload = sol;
      const size_t payloadBegin = bPos;
      DfsElems(elem.children, 0, bPos, withPayload,
               [&](size_t payloadEnd,
                   DefinitionTapeReplaySolution &afterPayload) {
                 if (payloadEnd == payloadBegin)
                   return;
                 DefinitionTapeReplaySolution cont = afterPayload;
                 ++cont.vaOptIncludedCount;
                 DfsElems(elems, elemIdx + 1, payloadEnd, cont, done);
               });
      return;
    }
    }
  }

  ArrayRef<DefinitionTapeReplayElem> pattern_;
  ArrayRef<PPTok> bToks_;
  const RefoldSourceMapper &sourceMapper_;
  ArrayRef<RefoldModel::MacroDefParam> defParams_;
  ArrayRef<unsigned> oldTokenCountByFormal_;
  std::pair<size_t, size_t> bEnv_ = {0, 0};
  size_t formalCount_ = 0;
};

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

  std::vector<DefinitionTapeReplayElem> pattern;
  DefinitionTapeParser parser(*definition, m.defParams.size());
  if (!parser.Parse(pattern) || pattern.empty())
    return std::nullopt;

  // Keep this heavier replay solver out of the ordinary non-empty argument
  // case.  It exists for missing proof surfaces: a token-empty source slot, a
  // formal with no recorded expansion span, or a __VA_OPT__ branch flip.
  const bool hasVaOpt = definitionTapePatternHasVaOpt(pattern);

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

  std::vector<RefoldModel::PPArgSpan> standardSpans;
  for (const auto &as : m.argSpans) {
    if (as.kind == PPArgSpanKind::Standard && as.begin < as.end)
      standardSpans.push_back(as);
  }
  llvm::sort(standardSpans, ppArgSpanLessByTokenRangeAndArg);

  std::vector<DefinitionTapeReplayAOcc> aOccs;
  DefinitionTapeAReplayMatcher aReplayMatcher(pattern, deps_.aToks,
                                              standardSpans);
  if (!aReplayMatcher.Match(cover->first, cover->second, aOccs))
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
      for (const DefinitionTapeReplayElem &elem : pattern) {
        if (elem.kind != DefinitionTapeReplayElem::Kind::Literal)
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
  for (const DefinitionTapeReplayAOcc &occ : aOccs) {
    if (occ.argIdx < oldTokenCountByFormal.size())
      oldTokenCountByFormal[occ.argIdx] +=
          static_cast<unsigned>(occ.aEnd - occ.aBegin);
  }

  using ReplaySolution = DefinitionTapeReplaySolution;
  std::vector<ReplaySolution> solutions;
  DefinitionTapeBReplayEnumerator bReplayEnumerator(
      pattern, deps_.bToks, *deps_.sourceMapper, m.defParams,
      oldTokenCountByFormal, *bEnv, invArgRanges.size());
  bReplayEnumerator.Enumerate(solutions);

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
  // definition.  The helper records the semantic tape surface without
  // mentioning any candidate's chosen source spelling.
  DefinitionTapeProfile tapeProfile =
      collectDefinitionTapeProfile(pattern, invArgRanges.size());

  for (size_t i = 0; i < invArgRanges.size(); ++i) {
    if (formalSourceTrim(static_cast<uint32_t>(i)).empty())
      ++tapeProfile.emptySourceSlotCount;
  }
  for (const DefinitionTapeReplayAOcc &occ : aOccs) {
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
        // The reconstructed variadic text may begin with a comma that belongs
        // to the separator before the tail rather than to the argument itself.
        // This happens when the edit activates a `__VA_OPT__(,)` payload
        // (empty -> non-empty varargs): the expansion gains a comma that the
        // definition-tape replay folds into the variadic slot.  Drop one such
        // leading comma so the tail is spelled once with the separator added
        // below, rather than doubling it (`M2(1, , 2)` instead of `M2(1, 2)`).
        // Mirrors the leading-comma normalization in the standard args-only
        // patch builder.
        StringRef tail = StringRef(editText).ltrim();
        if (tail.starts_with(",")) {
          tail = tail.drop_front().ltrim();
          editText = tail.str();
        }
        if (editText.empty())
          continue;
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

  const bool hasVaOptCommaPayload =
      definitionTapeVaOptPayloadContainsComma(pattern);

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
      out += name;
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
  // this service does not depend on planner-owned proof certification helpers.
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
