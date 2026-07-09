//===--- RefoldMacroStandardArgsOnlyPatchBuilder.cpp ------------*- C++ -*-===//
//
// Implementation of the standard args-only patch builder.
//
// Planner-side helpers still owned by `RefoldMacroPatchPlanner` are reached
// through the std::function callbacks supplied in `Dependencies`.  The
// public service boundary intentionally remains this one builder; local replay
// policy is split into private helpers only when the helper can state its
// proof obligation, mutation boundary, ordering constraint, and fail-closed
// behavior explicitly.
//
//===----------------------------------------------------------------------===//

#include "macro/RefoldMacroStandardArgsOnlyPatchBuilder.h"

#include "core/RefoldLog.h"
#include "core/RefoldModel.h"
#include "edit/RefoldBInsertionLedger.h"
#include "macro/RefoldArgTextRecovery.h"
#include "macro/RefoldMacroGeneratedCalleeReplayEngine.h"
#include "macro/RefoldMacroGeneratedLeafReplayEngine.h"
#include "macro/RefoldMacroPatchProofCertifier.h"
#include "macro/RefoldMacroPlannerHelpers.h"
#include "macro/RefoldMacroReplay.h"
#include "macro/RefoldMacroTopology.h"
#include "macro/RefoldMacroTupleHelpers.h"
#include "proof/RefoldProofLattice.h"
#include "source/RefoldSourceMapper.h"
#include "source/RefoldToken.h"
#include "source/TokenTextHelpers.h"
#include "util/StringUtils.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"

#include <algorithm>
#include <optional>
#include <string>
#include <utility>
#include <vector>

using namespace llvm;

namespace clang {
namespace refold {

RefoldMacroStandardArgsOnlyPatchBuilder::
    RefoldMacroStandardArgsOnlyPatchBuilder(Dependencies deps)
    : deps_(std::move(deps)) {}

RefoldMacroArgsOnlyTemplateSolver
RefoldMacroStandardArgsOnlyPatchBuilder::TemplateSolver() const {
  return RefoldMacroArgsOnlyTemplateSolver(
      {&deps_.model, deps_.aToks, deps_.bToks, deps_.bTokOff,
       &deps_.sourceMapper, &deps_.macroTopology, &deps_.proofLattice,
       &deps_.lexLang, deps_.strict});
}

RefoldMacroOccurrenceReplay
RefoldMacroStandardArgsOnlyPatchBuilder::OccurrenceReplay() const {
  return RefoldMacroOccurrenceReplay({deps_.aToks, deps_.bTokOff,
                                      &deps_.macroTopology, &deps_.sourceMapper,
                                      deps_.strict});
}

RefoldMacroPasteArgumentBuilder
RefoldMacroStandardArgsOnlyPatchBuilder::PasteArgumentBuilder() const {
  return RefoldMacroPasteArgumentBuilder(
      {&deps_.sourceMapper, deps_.aToks, deps_.bToks, &deps_.lexLang});
}

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

/// Boundary-lexed token carrier for parent-tuple generated-callee replay.
///
struct ParentTupleCalleeReplayTok {
  std::string spelling;
  size_t begin = 0;
  size_t end = 0;
};

/// Boundary-lexed token with byte offsets inside a replay text slice.
///
/// The standard args-only builder uses this local carrier for replay helpers
/// that need only a token spelling and its half-open byte range inside the
/// replay text being analyzed.
struct ReplayTok {
  std::string spelling;
  size_t begin = 0;
  size_t end = 0;
};

/// Compare a parent-tuple callee replay token subrange against expected
/// spellings.  The parent-tuple solver uses its own replay-token carrier, so
/// this helper is deliberately typed to that carrier instead of the generic
/// generated-callee replay token used by other local solvers.
bool replayTokenRangeSpellingsEqual(
    llvm::ArrayRef<ParentTupleCalleeReplayTok> toks, size_t begin,
    llvm::ArrayRef<std::string> expected) {
  if (begin + expected.size() > toks.size())
    return false;
  for (size_t i = 0; i < expected.size(); ++i)
    if (toks[begin + i].spelling != expected[i])
      return false;
  return true;
}

/// Result of replaying a producer definition tape to repair standard argument
/// span formal indices.
struct DefinitionReplayedStandardArgSpanRepair {
  /// Standard argument spans to use for the ordinary args-only replay path.
  std::vector<RefoldModel::PPArgSpan> argSpans;

  /// Whether the definition-tape proof changed any recorded formal index.
  bool replayedFormalIndices = false;
};

/// Repairs recorded standard arg-span formal indices through the immutable
/// producer definition tape when that tape proves an ordinary, non-stringify,
/// non-paste replay of the invocation's complete A-side cover.
///
/// This helper owns only the definition-tape proof/search obligation.  It does
/// not mutate planner state, candidate state, proof carriers, or occurrence
/// ordering; the caller receives the former local side effect as an explicit
/// result bit.  Any unsupported ambiguity -- mismatched definition identity,
/// stringify/paste syntax, invalid parameter references, incomplete cover
/// replay, empty parameter spans, or literal-token mismatch -- fails closed by
/// returning the originally recorded invocation spans with no repair flag.
/// Successful replay preserves the existing sorted standard-occurrence order
/// used by the old local lambda and only rewrites the formal indices proven by
/// the replacement-list parameter-reference sequence.
DefinitionReplayedStandardArgSpanRepair getDefinitionReplayedStandardArgSpans(
    const RefoldModel &model, llvm::ArrayRef<PPTok> aToks,
    const RefoldModel::MacroInvocation &m) {
  auto originalRepair = [&]() {
    DefinitionReplayedStandardArgSpanRepair result;
    result.argSpans = m.argSpans;
    return result;
  };

  std::vector<RefoldModel::PPArgSpan> out = m.argSpans;

  // Producer arg indices can be ambiguous when an actual contains a comma
  // that is not protected by parentheses, e.g. `M(arr[1, 2], 3)`.  Clang's
  // source range for the first written argument may cover the bracketed text,
  // while macro replacement still substitutes the comma-separated pieces into
  // successive formals.  Repair only the fully provable case: the definition
  // replacement-list tape and the recorded standard spans must replay the
  // macro's complete A-side cover exactly, with one non-empty standard span
  // per replacement-list parameter reference.
  const RefoldModel::MacroDirective *definition =
      getDefinitionDirectiveForInvocation(model, m);
  if (!definition || definition->subkind != "#define" ||
      !definition->functionLike || definition->name != m.name ||
      definition->defParams.size() != m.defParams.size() || out.empty() ||
      !m.stringifySpans.empty() || !m.pasteSpans.empty() ||
      !m.cover.IsValid() || m.cover.end > aToks.size())
    return originalRepair();

  // Extract the formal-reference order from the macro replacement-list tape.
  // Stringify and paste are excluded because their spelling/segmentation rules
  // are not ordinary standard-argument substitution.
  SmallVector<uint32_t, 8> formalSeq;
  formalSeq.reserve(definition->replacementTokens.size());
  for (const RefoldModel::MacroReplacementToken &token :
       definition->replacementTokens) {
    if (token.spelling == "#" || token.spelling == "##")
      return originalRepair();
    if (token.kind != RefoldModel::MacroReplacementTokenKind::ParamRef)
      continue;
    if (!token.paramIndex || *token.paramIndex >= m.defParams.size())
      return originalRepair();
    formalSeq.push_back(*token.paramIndex);
  }

  if (formalSeq.size() != out.size())
    return originalRepair();

  llvm::sort(out, ppArgSpanLessByTokenRangeAndArg);

  // Replay the definition replacement-list tape over the A-side macro cover.
  // Literals must match real expanded tokens; each parameter reference must
  // consume the next recorded standard span exactly at the current cursor.
  uint64_t tok = m.cover.begin;
  size_t argSpanIdx = 0;
  for (const RefoldModel::MacroReplacementToken &repTok :
       definition->replacementTokens) {
    switch (repTok.kind) {
    case RefoldModel::MacroReplacementTokenKind::Literal:
      if (tok >= m.cover.end || tok >= aToks.size() ||
          aToks[static_cast<size_t>(tok)].spelling != repTok.spelling)
        return originalRepair();
      ++tok;
      break;
    case RefoldModel::MacroReplacementTokenKind::ParamRef: {
      if (argSpanIdx >= out.size())
        return originalRepair();
      const RefoldModel::PPArgSpan &sp = out[argSpanIdx];
      if (sp.kind != PPArgSpanKind::Standard || sp.begin != tok ||
          sp.begin >= sp.end || sp.end > m.cover.end || sp.end > aToks.size())
        return originalRepair();
      tok = sp.end;
      ++argSpanIdx;
      break;
    }
    }
  }

  if (tok != m.cover.end || argSpanIdx != out.size())
    return originalRepair();

  bool changed = false;
  for (size_t i = 0; i < out.size(); ++i) {
    if (out[i].argIdx != formalSeq[i])
      changed = true;
    out[i].argIdx = formalSeq[i];
  }

  DefinitionReplayedStandardArgSpanRepair result;
  result.argSpans = std::move(out);
  result.replayedFormalIndices = changed;
  return result;
}

/// Token hunk and occurrence state for the touched-formal collection phase.
///
/// This carrier is intentionally private to this translation unit: it owns the
/// mutable collection state used to prove that the args-only rewrite explains
/// every token hunk touching the same formal arguments as the primary hunk.  The
/// collection must preserve sibling hunk iteration order until the existing
/// deterministic normalization point, must append synthetic insertion-frontier
/// envelopes in the same order as before, and must fail closed rather than widen
/// beyond the current invocation cover when ownership cannot be proven.
struct TouchedFormalHunkCollection {
  /// Standard and stringify occurrences used for hunk-ownership checks.
  std::vector<RefoldModel::PPArgSpan> occurrences;

  /// Parallel flag for `occurrences`; true entries are stringify observations.
  std::vector<char> occurrenceIsStringify;

  /// Formal-index bitmap derived from the touched occurrences.
  std::vector<char> touchedFormals;

  /// Primary, sibling, and synthetic token hunks that downstream replay must
  /// explain for the touched formals.
  SmallVector<diffutils::Hunk, 8> tokenHunks;
};

/// Collects every token hunk that must be explained by the touched-formal
/// args-only proof.
///
/// The collector owns only the hunk-coverage obligation for the already built
/// standard/stringify occurrence set.  It is allowed to mutate the returned
/// `touchedFormals` bitmap and `tokenHunks` list; it does not observe or change
/// occurrence-rewrite evidence, tuple-generated-callee replay state, proof
/// carriers, or candidate ordering.  Unsupported ownership cases fail closed by
/// returning `std::nullopt`: the collector never widens outside the current
/// invocation cover and never invents a fallback hunk.  Sibling token hunks are
/// appended in the original diff order, synthetic insertion-frontier envelopes
/// are appended after the seed hunk snapshot in the same order as the former
/// local lambdas, and the existing deterministic sort/unique normalization is
/// preserved as the final collection step.
class TouchedFormalHunkCollector {
public:
  TouchedFormalHunkCollector(
      const RefoldMacroStandardArgsOnlyPatchBuilder::Dependencies &deps,
      const RefoldMacroOccurrenceReplay &occurrenceReplay)
      : deps_(deps), occurrenceReplay_(occurrenceReplay) {}

  /// Return the completed touched-formal hunk collection, or nullopt when the
  /// primary hunk or any required sibling/synthetic ownership proof cannot be
  /// established inside this invocation's recorded argument occurrences.
  std::optional<TouchedFormalHunkCollection>
  Collect(TouchedFormalHunkCollection collection,
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

    // Add sibling token hunks that also touch the same formal arguments. The
    // eventual argument replacement must explain the complete set of token edits
    // for those formals, otherwise we could accept a partial rewrite.
    for (const auto &cand : deps_.abTokHunks) {
      if (sameTokenHunk(cand, primaryHunk))
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

private:
  bool HunkTouchesFormalOccurrence(
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

  bool HunkTouchesTouchedFormal(
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

  void MaybeAddSyntheticTouchedFormalEnvelope(
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

  const RefoldMacroStandardArgsOnlyPatchBuilder::Dependencies &deps_;
  const RefoldMacroOccurrenceReplay &occurrenceReplay_;
};

/// One old/new observation for a formal occurrence in the args-only replay.
struct OccObservation {
  /// The original spelling contributed by one occurrence of a formal argument.
  ///
  /// Stored by value so extracted observation collection can safely preserve
  /// unstringified old-side spellings after the collector returns.
  std::string oldText;

  /// The rewritten spelling inferred for that same occurrence from the token
  /// hunk set.
  std::string newText;

  /// Optional byte range inside `newText` that corresponds exactly to B-only
  /// insertion payload owned by this occurrence.  When absent, the whole
  /// rewritten argument remains the conservative materialized output range.
  std::optional<std::pair<uint64_t, uint64_t>> materializedNewTextRange;
};

/// Mutable evidence accumulated while observing one touched formal argument.
///
/// The caller fills this carrier in the existing occurrence order and then uses
/// it to decide whether a formal has a uniform replacement or requires the
/// narrower tuple-forwarding proof.  Missing source maps, untracked
/// materialized ranges, delimiter-boundary repairs, and inconsistent
/// observations retain their existing fail-closed behavior; the collector
/// returns this carrier without admitting a candidate or certifying proof state.
struct InvocationOccurrenceObservationSet {
  /// Ordered old/new observations for the current formal.
  SmallVector<OccObservation, 8> observations;

  /// Uniform replacement if all observations seen so far agree.
  std::optional<std::string> unifiedNewArg;

  /// Union of precise materialized B ranges when every observation tracked one.
  std::optional<std::pair<uint64_t, uint64_t>>
      unifiedMaterializedNewTextRange;

  /// True when at least one observation could not prove a precise materialized
  /// B range, forcing conservative proof certification for that formal.
  bool sawUntrackedMaterializedNewTextRange = false;

  /// True when observations disagree and tuple-forwarding must prove a narrower
  /// slice rewrite before the args-only candidate may be accepted.
  bool needTupleForwarding = false;
};

/// Final per-argument rewrite set handed to the args-only proof certifier.
///
/// This carrier is the boundary between occurrence/rewrite discovery and
/// accepted-candidate construction.  Discovery records exactly the same
/// replacement spellings and materialized output subranges as before, in the
/// same touched-argument order; the certifier only consumes the completed maps.
/// Tuple-forwarding status is recorded for the internal proof boundary but does
/// not alter the current public proof carrier, candidate ranking, or fallback
/// policy.
struct ArgsOnlyFinalArgumentRewriteSet {
  /// Rewritten invocation actual text by formal argument index.
  DenseMap<uint32_t, std::string> replacementsByArgIdx;

  /// Precise materialized output subrange by formal argument index when proved.
  DenseMap<uint32_t, std::pair<uint64_t, uint64_t>> materializedRangeByArgIdx;

  /// Whether the final spelling for an argument came through tuple forwarding.
  DenseMap<uint32_t, bool> tupleForwardedByArgIdx;

  /// Record one finalized formal replacement without changing admission policy.
  void Record(uint32_t argIdx, std::string finalNewArg,
              std::optional<std::pair<uint64_t, uint64_t>> materializedRange,
              bool tupleForwarded) {
    tupleForwardedByArgIdx[argIdx] = tupleForwarded;
    if (materializedRange && materializedRange->second <= finalNewArg.size())
      materializedRangeByArgIdx[argIdx] = *materializedRange;
    replacementsByArgIdx[argIdx] = std::move(finalNewArg);
  }

  bool Empty() const { return replacementsByArgIdx.empty(); }
};

/// Collects the old/new occurrence evidence for one touched formal argument.
///
/// The collector owns the per-formal observation obligation: every recorded
/// standard/stringify occurrence for `argIdx` is visited in the caller-provided
/// order, its B-side envelope is widened with only the already-proven hunk set,
/// stringify and paste inversions are applied exactly as before, and the result
/// carrier records whether all observations collapse to one replacement or need
/// tuple forwarding.  The only mutation is to the returned carrier.  Missing
/// source maps, unsupported stringify inversion, and unprovable materialized
/// ranges retain the existing fail-closed behavior by returning nullopt or by
/// marking the range untracked; this component never admits a candidate and never
/// changes proof certification or candidate ordering.
class InvocationOccurrenceObservationCollector {
public:
  InvocationOccurrenceObservationCollector(
      const RefoldMacroStandardArgsOnlyPatchBuilder::Dependencies &deps,
      const RefoldMacroOccurrenceReplay &occurrenceReplay)
      : deps_(deps), occurrenceReplay_(occurrenceReplay) {}

  /// Return the ordered occurrence evidence for `argIdx`, or nullopt when the
  /// occurrence-level rewrite cannot be proven under the existing rules.
  std::optional<InvocationOccurrenceObservationSet>
  Collect(const RefoldModel::MacroInvocation &invocation, uint32_t argIdx,
          StringRef baseArgText,
          llvm::ArrayRef<RefoldModel::PPArgSpan> occurrences,
          llvm::ArrayRef<char> occurrenceIsStringify,
          llvm::ArrayRef<diffutils::Hunk> tokenHunks,
          const diffutils::Hunk &primaryHunk) const {
    InvocationOccurrenceObservationSet result;

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

      if (!result.unifiedNewArg)
        result.unifiedNewArg = newArg;
      else if (*result.unifiedNewArg != newArg)
        result.needTupleForwarding = true;
    }

    return result;
  }

private:
  const RefoldMacroStandardArgsOnlyPatchBuilder::Dependencies &deps_;
  const RefoldMacroOccurrenceReplay &occurrenceReplay_;
};

/// Validates tuple-forwarded occurrence slices against every B-side occurrence.
///
/// Tuple forwarding rewrites one old slice to one new slice, so this validator
/// first requires the collected observations to induce a deterministic old->new
/// map with no conflicts.  It then rebuilds the same widened B envelopes used by
/// the collector for all standard occurrences of the formal, including owned
/// pure insertions and the narrow delimiter-closer repair.  It mutates no caller
/// state and rejects missing source maps or conflicting slice observations
/// exactly as the former local lambda did.
class TupleSliceConsistencyValidator {
public:
  TupleSliceConsistencyValidator(
      const RefoldMacroStandardArgsOnlyPatchBuilder::Dependencies &deps,
      const RefoldMacroOccurrenceReplay &occurrenceReplay)
      : deps_(deps), occurrenceReplay_(occurrenceReplay) {}

  bool Validate(uint32_t argIdx,
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

private:
  const RefoldMacroStandardArgsOnlyPatchBuilder::Dependencies &deps_;
  const RefoldMacroOccurrenceReplay &occurrenceReplay_;
};


/// Validates replacement text for formals whose producer arg indices were
/// repaired from definition replay.
///
/// Definition-replayed standard spans can replace stale producer occurrence
/// indices with current-level/declaration-derived indices.  This validator owns
/// only the corresponding all-occurrences B-side proof: for every repaired
/// STANDARD occurrence of the formal, it remaps the occurrence envelope, widens
/// it with the same owned-insertion and overlapping-hunk evidence used by the
/// inline implementation, applies the same narrow delimiter-closer repair, and
/// compares the materialized B spelling against the proposed replacement.  It
/// mutates no caller state, preserves the existing token-hunk order, and rejects
/// missing or inverted source maps fail-closed rather than accepting a partial
/// occurrence rewrite.
class ReplayedFormalOccurrenceValidator {
public:
  ReplayedFormalOccurrenceValidator(
      const RefoldMacroStandardArgsOnlyPatchBuilder::Dependencies &deps,
      const RefoldMacroOccurrenceReplay &occurrenceReplay)
      : deps_(deps), occurrenceReplay_(occurrenceReplay) {}

  /// Return whether `newArg` matches every repaired STANDARD occurrence.
  bool Validate(uint32_t argIdx, StringRef newArg,
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

private:
  const RefoldMacroStandardArgsOnlyPatchBuilder::Dependencies &deps_;
  const RefoldMacroOccurrenceReplay &occurrenceReplay_;
};

/// Tuple rewrite modes are ordered from strongest proof to weakest.
///
/// DirectTupleRefs uses producer-supplied tuple element byte ranges in the
/// normalized child invocation.  VariadicIdentityForward is the fallback for
/// variadic forwarding wrappers whose immediate child preserves the caller's
/// variadic tuple as one full-width forwarded argument.
enum class TupleRewriteMode {
  None,
  DirectTupleRefs,
  VariadicIdentityForward,
};

/// One forwarder-formal reference inside a parent tuple generated-callee proof.
struct GeneratedTupleCalleeArgRef {
  /// Formal index in the forwarding macro definition.
  uint32_t forwarderParamIdx = 0;

  /// Whether this reference consumes the remaining tuple tail positionally.
  bool variadicPack = false;
};

/// Explicit state for the parent tuple generated-callee bridge.
///
/// The bridge is allowed to mutate only the evidence needed to connect a caller
/// tuple element to a generated callee replay: child invocation witnesses,
/// tuple-element slices, old expansion observations, and merged solved actuals.
/// Ambiguous children, duplicate tuple keys, generated-callee replay ambiguity,
/// and conflicting solved actuals must continue to reject this proof path; the
/// state carrier does not introduce a new fallback or change occurrence merge
/// ordering.
struct ParentTupleGeneratedCalleeRewriteState {
  /// Unique child invocation that proves a tuple-forwarding witness, if any.
  const RefoldModel::MacroInvocation *tupleChild = nullptr;

  /// Proof mode established by the child witness search.
  TupleRewriteMode rewriteMode = TupleRewriteMode::None;

  /// Child argument texts paired with their child argument indices.
  SmallVector<std::pair<uint32_t, StringRef>, 8> childArgs;

  /// Direct tuple-reference metadata for the selected child witness.
  SmallVector<RefoldModel::TupleArgRef, 8> childTupleRefs;

  /// Child argument index for the variadic identity-forwarding witness.
  std::optional<uint32_t> identityForwardChildArgIdx;

  /// Parent tuple elements, in source order, for generated-callee replay.
  SmallVector<TupleElementSlice, 8> tupleElements;

  /// Positional mapping from forwarding macro formals to generated actuals.
  SmallVector<GeneratedTupleCalleeArgRef, 8> generatedArgs;

  /// Original generated-callee actual pieces read from the parent tuple.
  SmallVector<StringRef, 8> oldGeneratedActualPieces;

  /// Old actual spellings passed to the generated callee solver.
  SmallVector<std::string, 8> oldActuals;

  /// Consensus solved generated-callee actuals across all matching occurrences.
  std::optional<SmallVector<std::string, 8>> mergedSolvedActuals;
};


/// Lexes replay text into the requested spelling/byte-offset token carrier.
///
/// The carrier type remains caller-specific so standard-args replay and the
/// parent-tuple bridge keep distinct semantic state.  This helper only
/// centralizes the shared boundary-token projection.
template <typename ReplayTokenT>
void lexReplayTokens(StringRef text, const clang::LangOptions &lexLang,
                     SmallVectorImpl<ReplayTokenT> &out) {
  out.clear();
  SmallVector<RefoldLexBoundaryToken, 32> toks;
  refoldLexBoundaryTokens(text, lexLang, toks);
  for (const RefoldLexBoundaryToken &tok : toks)
    out.push_back(ReplayTokenT{tok.spelling, tok.begin, tok.end});
}

/// Returns the replay-token spelling sequence for one text slice.
template <unsigned InlineCapacity = 16>
SmallVector<std::string, InlineCapacity>
tokenSpellingsForReplayText(StringRef text, const clang::LangOptions &lexLang) {
  SmallVector<RefoldLexBoundaryToken, InlineCapacity> toks;
  refoldLexBoundaryTokens(text, lexLang, toks);
  SmallVector<std::string, InlineCapacity> out;
  for (const RefoldLexBoundaryToken &tok : toks)
    out.push_back(tok.spelling);
  return out;
}

/// Classifies one element in the standard-args generated-callee replay tree.
enum class StandardArgsGeneratedCalleeReplayKind {
  /// Literal replacement token.
  Literal,
  /// Formal parameter reference.
  Param,
  /// Conditional `__VA_OPT__` payload.
  VaOpt,
  /// Stringification of a formal parameter.
  Stringify,
  /// Token-paste expression.
  Paste
};

/// One literal or parameter piece inside a generated-callee paste expression.
struct StandardArgsGeneratedCalleePastePiece {
  /// Whether this paste piece references a formal parameter.
  bool isParam = false;

  /// Formal parameter index when `isParam` is true.
  uint32_t paramIdx = 0;

  /// Literal spelling when `isParam` is false.
  std::string literal;
};

/// One parsed element of the standard-args generated-callee replay tree.
struct StandardArgsGeneratedCalleeReplayElem {
  /// Element kind controlling which payload fields are meaningful.
  StandardArgsGeneratedCalleeReplayKind kind =
      StandardArgsGeneratedCalleeReplayKind::Literal;

  /// Literal spelling for literal replay elements.
  std::string literal;

  /// Formal parameter index for parameter or stringification elements.
  uint32_t paramIdx = 0;

  /// Ordered nested replay elements for `__VA_OPT__` payloads.
  std::vector<StandardArgsGeneratedCalleeReplayElem> children;

  /// Ordered paste pieces for token-paste replay elements.
  std::vector<StandardArgsGeneratedCalleePastePiece> pastePieces;
};

/// Parses a standard-args generated-callee replacement list into replay nodes.
///
/// The parser trusts the selected callee definition and the recovered old
/// actual slots.  It preserves replacement-token order and rejects malformed
/// stringification, paste chains, out-of-range formal references, and malformed
/// `__VA_OPT__` payloads fail-closed.
class StandardArgsGeneratedCalleeReplayParser {
public:
  StandardArgsGeneratedCalleeReplayParser(
      const RefoldModel::MacroDirective &definition,
      ArrayRef<std::string> oldActuals)
      : definition_(definition), oldActuals_(oldActuals) {}

  /// Parses `begin..end` into ordered replay elements.
  bool Parse(size_t begin, size_t end,
             std::vector<StandardArgsGeneratedCalleeReplayElem> &out) const {
    for (size_t i = begin; i < end;) {
      const auto &tok = definition_.replacementTokens[i];

      // Accept only the canonical stringification form: # <formal-param>.
      if (tok.spelling == "#") {
        if (i + 1 >= end ||
            definition_.replacementTokens[i + 1].kind !=
                RefoldModel::MacroReplacementTokenKind::ParamRef ||
            !definition_.replacementTokens[i + 1].paramIndex)
          return false;
        const uint32_t paramIdx =
            *definition_.replacementTokens[i + 1].paramIndex;
        if (paramIdx >= oldActuals_.size())
          return false;
        StandardArgsGeneratedCalleeReplayElem elem;
        elem.kind = StandardArgsGeneratedCalleeReplayKind::Stringify;
        elem.paramIdx = paramIdx;
        out.push_back(std::move(elem));
        i += 2;
        continue;
      }

      // A paste replay element begins when the next replacement token is ##.
      // The full paste chain is consumed left-to-right.
      if (i + 1 < end &&
          definition_.replacementTokens[i + 1].spelling == "##") {
        StandardArgsGeneratedCalleeReplayElem elem;
        elem.kind = StandardArgsGeneratedCalleeReplayKind::Paste;
        StandardArgsGeneratedCalleePastePiece first;
        if (!PastePieceFromReplacementToken(tok, first))
          return false;
        elem.pastePieces.push_back(std::move(first));
        i += 2;
        while (true) {
          if (i >= end)
            return false;
          StandardArgsGeneratedCalleePastePiece next;
          if (!PastePieceFromReplacementToken(definition_.replacementTokens[i],
                                             next))
            return false;
          elem.pastePieces.push_back(std::move(next));
          ++i;
          if (i >= end || definition_.replacementTokens[i].spelling != "##")
            break;
          ++i;
        }
        out.push_back(std::move(elem));
        continue;
      }

      if (tok.kind == RefoldModel::MacroReplacementTokenKind::ParamRef) {
        if (!tok.paramIndex || *tok.paramIndex >= oldActuals_.size())
          return false;
        StandardArgsGeneratedCalleeReplayElem elem;
        elem.kind = StandardArgsGeneratedCalleeReplayKind::Param;
        elem.paramIdx = *tok.paramIndex;
        out.push_back(std::move(elem));
        ++i;
        continue;
      }
      if (tok.spelling == "##")
        return false;
      if (tok.spelling == "__VA_OPT__") {
        std::optional<size_t> close = FindVaOptPayloadClose(i, end);
        if (!close)
          return false;
        StandardArgsGeneratedCalleeReplayElem elem;
        elem.kind = StandardArgsGeneratedCalleeReplayKind::VaOpt;
        if (!Parse(i + 2, *close, elem.children))
          return false;
        out.push_back(std::move(elem));
        i = *close + 1;
        continue;
      }
      StandardArgsGeneratedCalleeReplayElem elem;
      elem.kind = StandardArgsGeneratedCalleeReplayKind::Literal;
      elem.literal = tok.spelling.str();
      out.push_back(std::move(elem));
      ++i;
    }
    return true;
  }

private:
  /// Converts one replacement token into a replay-safe paste piece.
  bool PastePieceFromReplacementToken(
      const RefoldModel::MacroReplacementToken &tok,
      StandardArgsGeneratedCalleePastePiece &piece) const {
    if (tok.kind == RefoldModel::MacroReplacementTokenKind::ParamRef) {
      if (!tok.paramIndex || *tok.paramIndex >= oldActuals_.size())
        return false;
      piece.isParam = true;
      piece.paramIdx = *tok.paramIndex;
      return true;
    }
    if (tok.spelling == "#" || tok.spelling == "##" ||
        tok.spelling == "__VA_OPT__")
      return false;
    piece.isParam = false;
    piece.literal = tok.spelling.str();
    return true;
  }

  /// Finds the close parenthesis for a `__VA_OPT__(...)` payload.
  std::optional<size_t> FindVaOptPayloadClose(size_t vaOptIdx,
                                              size_t end) const {
    if (vaOptIdx + 1 >= end ||
        definition_.replacementTokens[vaOptIdx + 1].kind !=
            RefoldModel::MacroReplacementTokenKind::Literal ||
        definition_.replacementTokens[vaOptIdx + 1].spelling != "(")
      return std::nullopt;
    unsigned depth = 1;
    size_t close = vaOptIdx + 2;
    for (; close < end; ++close) {
      const auto &inner = definition_.replacementTokens[close];
      if (inner.kind != RefoldModel::MacroReplacementTokenKind::Literal)
        continue;
      if (inner.spelling == "(") {
        ++depth;
        continue;
      }
      if (inner.spelling == ")" && --depth == 0)
        return close;
    }
    return std::nullopt;
  }

  const RefoldModel::MacroDirective &definition_;
  ArrayRef<std::string> oldActuals_;
};

/// Matches the old generated-callee expansion against the replay tree.
///
/// This is the A-side proof for the parent-tuple bridge.  It preserves replay
/// element order, records every reachable end token, and treats `__VA_OPT__` as
/// either erased or payload-present without inventing additional alternatives.
class OldExpansionReplayMatcher {
public:
  OldExpansionReplayMatcher(
      ArrayRef<StandardArgsGeneratedCalleeReplayElem> replayPattern,
      ArrayRef<SmallVector<std::string, 16>> oldActualTokSpellings,
      ArrayRef<std::string> oldActuals, const clang::LangOptions &lexLang)
      : replayPattern_(replayPattern),
        oldActualTokSpellings_(oldActualTokSpellings),
        oldActuals_(oldActuals), lexLang_(lexLang) {}

  /// Returns whether `oldExpansion` is explained by the old actual slots.
  bool Match(StringRef oldExpansion) const {
    SmallVector<ReplayTok, 32> toks;
    lexReplayTokens(oldExpansion, lexLang_, toks);
    SmallVector<size_t, 4> ends;
    MatchEnds(replayPattern_, toks, 0, ends);
    return llvm::is_contained(ends, toks.size());
  }

private:
  /// Recursively collects token end positions reachable from `pos`.
  void MatchEnds(ArrayRef<StandardArgsGeneratedCalleeReplayElem> elems,
                 ArrayRef<ReplayTok> toks, size_t pos,
                 SmallVectorImpl<size_t> &ends) const {
    if (elems.empty()) {
      ends.push_back(pos);
      return;
    }
    const StandardArgsGeneratedCalleeReplayElem &elem = elems.front();
    ArrayRef<StandardArgsGeneratedCalleeReplayElem> rest = elems.drop_front();
    switch (elem.kind) {
    case StandardArgsGeneratedCalleeReplayKind::Literal:
      if (pos < toks.size() && toks[pos].spelling == elem.literal)
        MatchEnds(rest, toks, pos + 1, ends);
      return;
    case StandardArgsGeneratedCalleeReplayKind::Param: {
      const auto &expected = oldActualTokSpellings_[elem.paramIdx];
      if (pos + expected.size() > toks.size())
        return;
      for (size_t i = 0; i < expected.size(); ++i)
        if (toks[pos + i].spelling != expected[i])
          return;
      MatchEnds(rest, toks, pos + expected.size(), ends);
      return;
    }
    case StandardArgsGeneratedCalleeReplayKind::Stringify: {
      if (pos >= toks.size())
        return;
      std::optional<std::string> content =
          decodeSimpleStringLiteralToken(toks[pos].spelling);
      if (!content)
        return;
      StringRef oldActual = StringRef(oldActuals_[elem.paramIdx]).trim();
      if (StringRef(*content).trim() != oldActual &&
          !findUniqueTrimmedSubstring(oldActual, StringRef(*content)))
        return;
      MatchEnds(rest, toks, pos + 1, ends);
      return;
    }
    case StandardArgsGeneratedCalleeReplayKind::Paste: {
      if (pos >= toks.size())
        return;
      std::string expected;
      for (const StandardArgsGeneratedCalleePastePiece &piece :
           elem.pastePieces) {
        if (piece.isParam)
          expected += StringRef(oldActuals_[piece.paramIdx]).trim().str();
        else
          expected += piece.literal;
      }
      if (toks[pos].spelling != expected)
        return;
      MatchEnds(rest, toks, pos + 1, ends);
      return;
    }
    case StandardArgsGeneratedCalleeReplayKind::VaOpt: {
      // First preserve the erased-payload path, then try the payload-present
      // path and continue only when the payload consumes at least one token.
      MatchEnds(rest, toks, pos, ends);
      SmallVector<size_t, 4> childEnds;
      MatchEnds(elem.children, toks, pos, childEnds);
      for (size_t childEnd : childEnds)
        if (childEnd != pos)
          MatchEnds(rest, toks, childEnd, ends);
      return;
    }
    }
  }

  ArrayRef<StandardArgsGeneratedCalleeReplayElem> replayPattern_;
  ArrayRef<SmallVector<std::string, 16>> oldActualTokSpellings_;
  ArrayRef<std::string> oldActuals_;
  const clang::LangOptions &lexLang_;
};

/// Solves new generated-callee actuals from a B-side expansion surface.
///
/// The solver preserves replay-element order, enumerates parameter end tokens
/// in increasing order, and stops after two solutions so the existing unique
/// solution ambiguity cutoff remains fail-closed.
class NewExpansionUniqueSolver {
public:
  NewExpansionUniqueSolver(
      ArrayRef<StandardArgsGeneratedCalleeReplayElem> replayPattern,
      size_t calleeParamCount, const clang::LangOptions &lexLang)
      : replayPattern_(replayPattern), calleeParamCount_(calleeParamCount),
        lexLang_(lexLang) {}

  /// Solves `newExpansion` if and only if the assignment is unique.
  std::optional<SmallVector<std::string, 8>>
  Solve(StringRef newExpansion) const {
    SmallVector<ReplayTok, 32> toks;
    lexReplayTokens(newExpansion, lexLang_, toks);
    AssignedRanges assigned;
    assigned.resize(calleeParamCount_);
    SmallVector<SmallVector<std::string, 8>, 4> solutions;

    Dfs(newExpansion, toks, replayPattern_, 0, assigned, solutions);
    if (solutions.size() != 1)
      return std::nullopt;
    return solutions.front();
  }

private:
  using AssignedRanges = SmallVector<std::optional<std::pair<size_t, size_t>>, 8>;

  /// DFSes replay elements against the token stream in deterministic order.
  void Dfs(StringRef expansion, ArrayRef<ReplayTok> toks,
           ArrayRef<StandardArgsGeneratedCalleeReplayElem> elems,
           size_t tokPos, AssignedRanges &curAssigned,
           SmallVectorImpl<SmallVector<std::string, 8>> &solutions) const {
    if (solutions.size() > 1)
      return;
    if (elems.empty()) {
      MaybeRecordSolution(expansion, toks, tokPos, curAssigned, solutions);
      return;
    }

    const StandardArgsGeneratedCalleeReplayElem &elem = elems.front();
    ArrayRef<StandardArgsGeneratedCalleeReplayElem> rest = elems.drop_front();
    switch (elem.kind) {
    case StandardArgsGeneratedCalleeReplayKind::Literal:
      if (tokPos < toks.size() && toks[tokPos].spelling == elem.literal)
        Dfs(expansion, toks, rest, tokPos + 1, curAssigned, solutions);
      return;
    case StandardArgsGeneratedCalleeReplayKind::Param:
      DfsParam(expansion, toks, rest, elem.paramIdx, tokPos, curAssigned,
               solutions);
      return;
    case StandardArgsGeneratedCalleeReplayKind::Stringify:
    case StandardArgsGeneratedCalleeReplayKind::Paste:
      return;
    case StandardArgsGeneratedCalleeReplayKind::VaOpt: {
      // Preserve the erased-payload branch before the payload-present branch.
      Dfs(expansion, toks, rest, tokPos, curAssigned, solutions);
      AssignedRanges withPayload = curAssigned;
      DfsVaOptChildren(expansion, toks, elem.children, rest, tokPos, tokPos,
                       withPayload, solutions);
      return;
    }
    }
  }

  /// Enumerates a parameter binding in increasing token-end order.
  void DfsParam(StringRef expansion, ArrayRef<ReplayTok> toks,
                ArrayRef<StandardArgsGeneratedCalleeReplayElem> rest,
                uint32_t paramIdx, size_t tokPos, AssignedRanges &curAssigned,
                SmallVectorImpl<SmallVector<std::string, 8>> &solutions) const {
    if (paramIdx >= curAssigned.size())
      return;
    if (curAssigned[paramIdx]) {
      const auto range = *curAssigned[paramIdx];
      const size_t width = range.second - range.first;
      if (tokPos + width <= toks.size() &&
          TokenRangesHaveSameSpellings(toks, range.first, tokPos, width))
        Dfs(expansion, toks, rest, tokPos + width, curAssigned, solutions);
      return;
    }

    for (size_t end = tokPos; end <= toks.size(); ++end) {
      curAssigned[paramIdx] = std::make_pair(tokPos, end);
      Dfs(expansion, toks, rest, end, curAssigned, solutions);
      curAssigned[paramIdx].reset();
      if (solutions.size() > 1)
        return;
    }
  }

  /// DFSes a `__VA_OPT__` payload and then resumes the parent suffix.
  void DfsVaOptChildren(
      StringRef expansion, ArrayRef<ReplayTok> toks,
      ArrayRef<StandardArgsGeneratedCalleeReplayElem> childElems,
      ArrayRef<StandardArgsGeneratedCalleeReplayElem> parentRest,
      size_t parentTokPos, size_t childTokPos, AssignedRanges &childAssigned,
      SmallVectorImpl<SmallVector<std::string, 8>> &solutions) const {
    if (childElems.empty()) {
      if (childTokPos != parentTokPos)
        Dfs(expansion, toks, parentRest, childTokPos, childAssigned, solutions);
      return;
    }

    const StandardArgsGeneratedCalleeReplayElem &child = childElems.front();
    ArrayRef<StandardArgsGeneratedCalleeReplayElem> childRest =
        childElems.drop_front();
    switch (child.kind) {
    case StandardArgsGeneratedCalleeReplayKind::Literal:
      if (childTokPos < toks.size() &&
          toks[childTokPos].spelling == child.literal)
        DfsVaOptChildren(expansion, toks, childRest, parentRest, parentTokPos,
                         childTokPos + 1, childAssigned, solutions);
      return;
    case StandardArgsGeneratedCalleeReplayKind::Param:
      DfsVaOptParam(expansion, toks, childRest, parentRest, parentTokPos,
                    child.paramIdx, childTokPos, childAssigned, solutions);
      return;
    case StandardArgsGeneratedCalleeReplayKind::Stringify:
    case StandardArgsGeneratedCalleeReplayKind::Paste:
    case StandardArgsGeneratedCalleeReplayKind::VaOpt:
      return;
    }
  }

  /// Enumerates parameter slices inside a `__VA_OPT__` payload.
  void DfsVaOptParam(
      StringRef expansion, ArrayRef<ReplayTok> toks,
      ArrayRef<StandardArgsGeneratedCalleeReplayElem> childRest,
      ArrayRef<StandardArgsGeneratedCalleeReplayElem> parentRest,
      size_t parentTokPos, uint32_t paramIdx, size_t childTokPos,
      AssignedRanges &childAssigned,
      SmallVectorImpl<SmallVector<std::string, 8>> &solutions) const {
    if (paramIdx >= childAssigned.size())
      return;
    if (childAssigned[paramIdx]) {
      const auto range = *childAssigned[paramIdx];
      const size_t width = range.second - range.first;
      if (childTokPos + width <= toks.size() &&
          TokenRangesHaveSameSpellings(toks, range.first, childTokPos, width))
        DfsVaOptChildren(expansion, toks, childRest, parentRest, parentTokPos,
                         childTokPos + width, childAssigned, solutions);
      return;
    }

    for (size_t end = childTokPos; end <= toks.size(); ++end) {
      childAssigned[paramIdx] = std::make_pair(childTokPos, end);
      DfsVaOptChildren(expansion, toks, childRest, parentRest, parentTokPos, end,
                       childAssigned, solutions);
      childAssigned[paramIdx].reset();
      if (solutions.size() > 1)
        return;
    }
  }

  /// Records a solved actual vector when the token stream is fully consumed.
  void MaybeRecordSolution(
      StringRef expansion, ArrayRef<ReplayTok> toks, size_t tokPos,
      const AssignedRanges &assigned,
      SmallVectorImpl<SmallVector<std::string, 8>> &solutions) const {
    if (tokPos != toks.size())
      return;
    SmallVector<std::string, 8> actuals;
    for (const auto &range : assigned) {
      if (!range)
        return;
      if (range->first == range->second) {
        actuals.push_back(std::string());
        continue;
      }
      const size_t byteBegin = toks[range->first].begin;
      const size_t byteEnd = toks[range->second - 1].end;
      actuals.push_back(expansion.slice(byteBegin, byteEnd).str());
    }
    solutions.push_back(std::move(actuals));
  }

  /// Compares two same-width token ranges by spelling.
  bool TokenRangesHaveSameSpellings(ArrayRef<ReplayTok> toks, size_t lhsBegin,
                                    size_t rhsBegin, size_t width) const {
    for (size_t i = 0; i < width; ++i)
      if (toks[lhsBegin + i].spelling != toks[rhsBegin + i].spelling)
        return false;
    return true;
  }

  ArrayRef<StandardArgsGeneratedCalleeReplayElem> replayPattern_;
  size_t calleeParamCount_ = 0;
  const clang::LangOptions &lexLang_;
};

/// One literal or parameter element in the simple forwarded-callee replay.
struct ForwardedGeneratedCalleeReplayElem {
  /// Whether this element references a callee formal parameter.
  bool isParam = false;

  /// Literal spelling when `isParam` is false.
  std::string literal;

  /// Formal parameter index when `isParam` is true.
  uint32_t paramIdx = 0;
};

/// Solves the direct tuple-ref forwarded-callee replay sub-engine.
///
/// This resolver keeps the simpler carrier used by the child-forwarding bridge:
/// only literal and parameter elements are accepted, while `#`, `##`, and
/// `__VA_OPT__` remain unsupported and cause the caller to skip this optional
/// proof path without changing fallback policy.
class ForwardedGeneratedCalleeReplaySolver {
public:
  ForwardedGeneratedCalleeReplaySolver(
      ArrayRef<ForwardedGeneratedCalleeReplayElem> replayPattern,
      ArrayRef<SmallVector<std::string, 8>> oldActualTokSpellings,
      size_t calleeParamCount, const clang::LangOptions &lexLang)
      : replayPattern_(replayPattern),
        oldActualTokSpellings_(oldActualTokSpellings),
        calleeParamCount_(calleeParamCount), lexLang_(lexLang) {}

  /// Returns whether the old expansion is exactly explained by old actuals.
  bool MatchOldExpansion(StringRef oldExpansion) const {
    SmallVector<ParentTupleCalleeReplayTok, 16> toks;
    lexReplayTokens(oldExpansion, lexLang_, toks);
    size_t pos = 0;
    for (const ForwardedGeneratedCalleeReplayElem &elem : replayPattern_) {
      if (!elem.isParam) {
        if (pos >= toks.size() || toks[pos].spelling != elem.literal)
          return false;
        ++pos;
        continue;
      }
      const auto &expected = oldActualTokSpellings_[elem.paramIdx];
      if (!replayTokenRangeSpellingsEqual(toks, pos, expected))
        return false;
      pos += expected.size();
    }
    return pos == toks.size();
  }

  /// Solves new callee actual text if the B-side assignment is unique.
  std::optional<SmallVector<std::string, 4>>
  SolveNewExpansion(StringRef newExpansion) const {
    SmallVector<ParentTupleCalleeReplayTok, 16> toks;
    lexReplayTokens(newExpansion, lexLang_, toks);
    AssignedRanges assigned;
    assigned.resize(calleeParamCount_);
    SmallVector<SmallVector<std::string, 4>, 4> solutions;

    Dfs(newExpansion, toks, 0, 0, assigned, solutions);
    if (solutions.size() != 1)
      return std::nullopt;
    return solutions.front();
  }

private:
  using AssignedRanges = SmallVector<std::optional<std::pair<size_t, size_t>>, 4>;

  /// DFSes the simple forwarded replay pattern in left-to-right order.
  void Dfs(StringRef expansion, ArrayRef<ParentTupleCalleeReplayTok> toks,
           size_t elemIdx, size_t tokPos, AssignedRanges &assigned,
           SmallVectorImpl<SmallVector<std::string, 4>> &solutions) const {
    if (solutions.size() > 1)
      return;
    if (elemIdx == replayPattern_.size()) {
      MaybeRecordSolution(expansion, toks, tokPos, assigned, solutions);
      return;
    }

    const ForwardedGeneratedCalleeReplayElem &elem = replayPattern_[elemIdx];
    if (!elem.isParam) {
      if (tokPos < toks.size() && toks[tokPos].spelling == elem.literal)
        Dfs(expansion, toks, elemIdx + 1, tokPos + 1, assigned, solutions);
      return;
    }

    DfsParam(expansion, toks, elemIdx, tokPos, elem.paramIdx, assigned,
             solutions);
  }

  /// Enumerates one parameter binding in increasing token-end order.
  void DfsParam(StringRef expansion,
                ArrayRef<ParentTupleCalleeReplayTok> toks, size_t elemIdx,
                size_t tokPos, uint32_t paramIdx, AssignedRanges &assigned,
                SmallVectorImpl<SmallVector<std::string, 4>> &solutions) const {
    if (paramIdx >= assigned.size())
      return;
    if (assigned[paramIdx]) {
      const auto range = *assigned[paramIdx];
      const size_t width = range.second - range.first;
      if (tokPos + width <= toks.size() &&
          TokenRangesHaveSameSpellings(toks, range.first, tokPos, width))
        Dfs(expansion, toks, elemIdx + 1, tokPos + width, assigned, solutions);
      return;
    }

    for (size_t end = tokPos; end <= toks.size(); ++end) {
      assigned[paramIdx] = std::make_pair(tokPos, end);
      Dfs(expansion, toks, elemIdx + 1, end, assigned, solutions);
      assigned[paramIdx].reset();
      if (solutions.size() > 1)
        return;
    }
  }

  /// Records a solved actual vector when all replay tokens are consumed.
  void MaybeRecordSolution(
      StringRef expansion, ArrayRef<ParentTupleCalleeReplayTok> toks,
      size_t tokPos, const AssignedRanges &assigned,
      SmallVectorImpl<SmallVector<std::string, 4>> &solutions) const {
    if (tokPos != toks.size())
      return;
    SmallVector<std::string, 4> actuals;
    for (const auto &range : assigned) {
      if (!range)
        return;
      if (range->first == range->second) {
        actuals.push_back(std::string());
        continue;
      }
      const size_t byteBegin = toks[range->first].begin;
      const size_t byteEnd = toks[range->second - 1].end;
      actuals.push_back(expansion.slice(byteBegin, byteEnd).str());
    }
    solutions.push_back(std::move(actuals));
  }

  /// Compares two same-width token ranges by spelling.
  bool TokenRangesHaveSameSpellings(
      ArrayRef<ParentTupleCalleeReplayTok> toks, size_t lhsBegin,
      size_t rhsBegin, size_t width) const {
    for (size_t i = 0; i < width; ++i)
      if (toks[lhsBegin + i].spelling != toks[rhsBegin + i].spelling)
        return false;
    return true;
  }

  ArrayRef<ForwardedGeneratedCalleeReplayElem> replayPattern_;
  ArrayRef<SmallVector<std::string, 8>> oldActualTokSpellings_;
  size_t calleeParamCount_ = 0;
  const clang::LangOptions &lexLang_;
};


/// Resolves the parent tuple generated-callee bridge for one caller argument.
///
/// This resolver owns only the early tuple-generated-callee proof path inside
/// standard args-only replay.  It proves the wrapper shape from replacement-token
/// tapes, resolves only deterministic object-like alias chains, replays the
/// generated callee through the existing parser/matcher/unique-solver helpers,
/// and mutates only the returned `outNewArg` when every observed occurrence
/// agrees on the same solved actuals.  Unsupported or malformed
/// stringification, paste, and `__VA_OPT__` forms, ambiguous macro definitions,
/// conflicting occurrence solutions, or non-unique replay solutions reject this
/// bridge and leave the caller's later tuple-forwarding/fallback policy
/// unchanged.
class ParentTupleGeneratedCalleeRewriteResolver {
public:
  ParentTupleGeneratedCalleeRewriteResolver(
      const RefoldMacroStandardArgsOnlyPatchBuilder::Dependencies &deps,
      const RefoldModel::MacroInvocation &invocation)
      : deps_(deps), invocation_(invocation) {}

  /// Tries to rewrite a parent tuple argument through a generated callee replay.
  bool TryRewrite(uint32_t callerArgIdx, StringRef baseArgText,
                  ArrayRef<OccObservation> occObservations,
                  std::string &outNewArg) const {
    // Handle the tuple-generated-callee case before the generic text-keyed
    // tuple rewrite.  In shapes such as
    //
    //   #define WRAP(PAIR) CALL PAIR
    //   #define CALL(F, X) F(X)
    //   WRAP((ADD_ONE, 10))
    //
    // the parent argument occurrence in PP output is the callee expansion
    // `((10) + 1)`, not the tuple element `10` by itself.  A whole-argument
    // replacement would therefore validate but collapse the tuple to
    // `WRAP(((20) + 1))`.  This proof reconstructs the generated call
    // positionally: tuple element 0 supplies the callee, later tuple elements
    // supply the generated actuals, and the callee replacement list is
    // replayed to determine which tuple slot changed.
    if (!invocation_.definitionDirectiveId)
      return false;

    const RefoldModel::MacroDirective *rootDefinition = nullptr;
    for (const RefoldModel::MacroDirective &directive :
         deps_.model.GetMacroDirectives()) {
      if (directive.id == *invocation_.definitionDirectiveId) {
        rootDefinition = &directive;
        break;
      }
    }
    if (!rootDefinition || rootDefinition->subkind != "#define" ||
        !rootDefinition->functionLike)
      return false;

    // This fallback proves the common tuple-wrapper shape directly from the
    // parent definition: the wrapper replacement is a literal forwarding macro
    // followed by exactly the caller tuple formal, e.g. `CALL PAIR`.  Keeping the
    // shape this narrow prevents the proof from guessing about arbitrary wrapper
    // bodies.
    if (rootDefinition->replacementTokens.size() != 2)
      return false;
    const auto &rootTok0 = rootDefinition->replacementTokens[0];
    const auto &rootTok1 = rootDefinition->replacementTokens[1];
    if (rootTok0.kind != RefoldModel::MacroReplacementTokenKind::Literal ||
        rootTok1.kind != RefoldModel::MacroReplacementTokenKind::ParamRef ||
        !rootTok1.paramIndex || *rootTok1.paramIndex != callerArgIdx)
      return false;
    const StringRef forwarderName = rootTok0.spelling;

    const RefoldModel::MacroDirective *forwarderDefinition =
        ResolveFunctionLikeMacroThroughObjectAliases(forwarderName);
    if (!forwarderDefinition || forwarderDefinition->defParams.empty())
      return false;

    // The forwarded caller argument must be a real parenthesized tuple.  The byte
    // ranges returned by the splitter are relative to the tuple payload between
    // the outer parentheses; edits are later applied to that payload and wrapped
    // back in the original tuple parens.
    StringRef parentTrim = baseArgText.trim();
    if (!parentTrim.starts_with("(") || !parentTrim.ends_with(")") ||
        parentTrim.size() < 2)
      return false;
    StringRef tuplePayload = parentTrim.drop_front().drop_back();
    ParentTupleGeneratedCalleeRewriteState state;
    SmallVector<TupleElementSlice, 8> &tupleElems = state.tupleElements;
    if (!splitTopLevelTupleElementsWithLexer(tuplePayload, deps_.lexLang,
                                             tupleElems))
      return false;
    if (tupleElems.size() < 2)
      return false;

    const auto &forwarderToks = forwarderDefinition->replacementTokens;
    if (forwarderToks.size() < 4)
      return false;
    if (forwarderToks[0].kind !=
            RefoldModel::MacroReplacementTokenKind::ParamRef ||
        !forwarderToks[0].paramIndex)
      return false;
    const uint32_t calleeForwarderParam = *forwarderToks[0].paramIndex;
    if (calleeForwarderParam >= forwarderDefinition->defParams.size() ||
        calleeForwarderParam >= tupleElems.size())
      return false;
    if (forwarderToks[1].kind !=
            RefoldModel::MacroReplacementTokenKind::Literal ||
        forwarderToks[1].spelling != "(" ||
        forwarderToks.back().kind !=
            RefoldModel::MacroReplacementTokenKind::Literal ||
        forwarderToks.back().spelling != ")")
      return false;

    // Accept only a direct generated-call replacement list:
    //   calleeFormal '(' generatedActualFormals... ')'
    // The callee formal itself is not rewritten here; it is resolved to a
    // function-like macro definition below, while the remaining formals become
    // positional tuple-edit targets.
    SmallVector<GeneratedTupleCalleeArgRef, 8> &generatedArgs =
        state.generatedArgs;
    for (size_t i = 2, e = forwarderToks.size() - 1; i < e; ++i) {
      const auto &tok = forwarderToks[i];
      if (tok.spelling == "#" || tok.spelling == "##" ||
          tok.spelling == "__VA_OPT__")
        return false;
      if (tok.kind != RefoldModel::MacroReplacementTokenKind::ParamRef)
        continue;
      if (!tok.paramIndex ||
          *tok.paramIndex >= forwarderDefinition->defParams.size())
        return false;
      if (*tok.paramIndex == calleeForwarderParam)
        return false;
      GeneratedTupleCalleeArgRef ref;
      ref.forwarderParamIdx = *tok.paramIndex;
      ref.variadicPack =
          forwarderDefinition->defParams[*tok.paramIndex].variadic;
      generatedArgs.push_back(ref);
    }
    if (generatedArgs.empty())
      return false;

    const StringRef calleeSourceText =
        TupleElementText(tuplePayload, tupleElems,
                         static_cast<size_t>(calleeForwarderParam));
    const RefoldModel::MacroDirective *calleeDefinition =
        ResolveFunctionLikeCallee(calleeSourceText);
    if (!calleeDefinition || calleeDefinition->defParams.empty())
      return false;

    // Read the generated actuals from the original tuple spelling.  This is
    // positional, so duplicate spellings like `(ADD2, 10, 10)` remain
    // distinguishable by tuple slot even though their text is identical.
    SmallVector<StringRef, 8> &oldGeneratedActualPieces =
        state.oldGeneratedActualPieces;
    for (const GeneratedTupleCalleeArgRef &ref : generatedArgs) {
      if (ref.variadicPack) {
        if (ref.forwarderParamIdx >= tupleElems.size())
          return false;
        for (size_t i = ref.forwarderParamIdx; i < tupleElems.size(); ++i)
          oldGeneratedActualPieces.push_back(
              TupleElementText(tuplePayload, tupleElems, i));
        continue;
      }
      if (ref.forwarderParamIdx >= tupleElems.size())
        return false;
      oldGeneratedActualPieces.push_back(
          TupleElementText(tuplePayload, tupleElems, ref.forwarderParamIdx));
    }

    const bool calleeHasVariadic = !calleeDefinition->defParams.empty() &&
                                   calleeDefinition->defParams.back().variadic;
    const size_t fixedCalleeActuals =
        calleeHasVariadic ? calleeDefinition->defParams.size() - 1
                          : calleeDefinition->defParams.size();
    if ((!calleeHasVariadic && oldGeneratedActualPieces.size() !=
                                   calleeDefinition->defParams.size()) ||
        (calleeHasVariadic &&
         oldGeneratedActualPieces.size() < fixedCalleeActuals))
      return false;

    // Repackage the tuple pieces as the actual list of the generated callee.  For
    // a variadic callee, all generated tail pieces are joined into the one
    // variadic formal spelling; the inverse mapping back to tuple elements is
    // performed after solving the new expansion.
    SmallVector<std::string, 8> &oldActuals = state.oldActuals;
    oldActuals.reserve(calleeDefinition->defParams.size());
    for (size_t i = 0; i < fixedCalleeActuals; ++i)
      oldActuals.push_back(oldGeneratedActualPieces[i].str());
    if (calleeHasVariadic) {
      std::string variadicText;
      raw_string_ostream os(variadicText);
      for (size_t i = fixedCalleeActuals; i < oldGeneratedActualPieces.size();
           ++i) {
        if (i != fixedCalleeActuals)
          os << ", ";
        os << oldGeneratedActualPieces[i].trim();
      }
      os.flush();
      oldActuals.push_back(std::move(variadicText));
    }
    if (oldActuals.size() != calleeDefinition->defParams.size())
      return false;

    std::vector<StandardArgsGeneratedCalleeReplayElem> calleePattern;
    StandardArgsGeneratedCalleeReplayParser calleeParser(
        *calleeDefinition,
        ArrayRef<std::string>(oldActuals.data(), oldActuals.size()));
    if (!calleeParser.Parse(0, calleeDefinition->replacementTokens.size(),
                            calleePattern) ||
        calleePattern.empty())
      return false;

    SmallVector<SmallVector<std::string, 16>, 8> oldActualTokSpellings;
    for (const std::string &actual : oldActuals)
      oldActualTokSpellings.push_back(
          tokenSpellingsForReplayText(actual, deps_.lexLang));

    OldExpansionReplayMatcher oldExpansionMatcher(
        ArrayRef<StandardArgsGeneratedCalleeReplayElem>(calleePattern.data(),
                                                        calleePattern.size()),
        ArrayRef<SmallVector<std::string, 16>>(
            oldActualTokSpellings.data(), oldActualTokSpellings.size()),
        ArrayRef<std::string>(oldActuals.data(), oldActuals.size()),
        deps_.lexLang);

    NewExpansionUniqueSolver newExpansionSolver(
        ArrayRef<StandardArgsGeneratedCalleeReplayElem>(calleePattern.data(),
                                                        calleePattern.size()),
        calleeDefinition->defParams.size(), deps_.lexLang);

    // Every observed occurrence of the parent argument must agree on the same
    // solved generated-callee actuals.  If one occurrence cannot be explained by
    // the old tuple-derived call, or if two occurrences imply different B
    // actuals, the tuple bridge is unproved.
    std::optional<SmallVector<std::string, 8>> &mergedSolvedActuals =
        state.mergedSolvedActuals;
    for (const OccObservation &obs : occObservations) {
      if (!MatchOldExpansion(oldExpansionMatcher, StringRef(obs.oldText)))
        continue;
      auto solved = SolveStringifyOrPasteNewExpansion(
          calleePattern, *calleeDefinition, oldActuals, obs.newText);
      if (!solved)
        solved = newExpansionSolver.Solve(obs.newText);
      if (!solved || solved->size() != calleeDefinition->defParams.size())
        return false;
      if (!mergedSolvedActuals) {
        mergedSolvedActuals = std::move(*solved);
        continue;
      }
      if (mergedSolvedActuals->size() != solved->size())
        return false;
      for (size_t i = 0; i < solved->size(); ++i)
        if (StringRef((*mergedSolvedActuals)[i]).trim() !=
            StringRef((*solved)[i]).trim())
          return false;
    }
    if (!mergedSolvedActuals)
      return false;

    // For ordinary param replay, the source tuple element and the old generated
    // actual are the same spelling.  Stringification is different: the B-side
    // expansion is a string literal whose payload corresponds to the generated
    // actual after forwarding/prescan, while the tuple slot may still contain a
    // structural spelling such as `ID(alpha)`.  Keep a separate old-expansion
    // projection for tuple editing so `"alpha" -> "beta"` can become
    // `ID(alpha) -> ID(beta)` instead of replacing the whole slot with `beta`.
    SmallVector<std::string, 8> oldGeneratedPiecesForRewrite;
    for (StringRef piece : oldGeneratedActualPieces)
      oldGeneratedPiecesForRewrite.push_back(piece.trim().str());
    if (calleePattern.size() == 1 &&
        calleePattern.front().kind ==
            StandardArgsGeneratedCalleeReplayKind::Stringify) {
      const uint32_t paramIdx = calleePattern.front().paramIdx;
      if (paramIdx < oldGeneratedPiecesForRewrite.size()) {
        for (const OccObservation &obs : occObservations) {
          std::optional<std::string> token =
              SingleTokenSpelling(StringRef(obs.oldText));
          if (!token)
            continue;
          std::optional<std::string> content =
              decodeSimpleStringLiteralToken(*token);
          if (!content)
            continue;
          StringRef sourcePiece =
              StringRef(oldGeneratedPiecesForRewrite[paramIdx]);
          if (sourcePiece == StringRef(*content) ||
              findUniqueTrimmedSubstring(sourcePiece, StringRef(*content))) {
            oldGeneratedPiecesForRewrite[paramIdx] = std::move(*content);
            break;
          }
        }
      }
    }

    // Split the solved callee actuals back into the generated tuple pieces.  A
    // non-empty variadic tail appends more tuple elements; an empty tail deletes
    // the old variadic tail slice during the tuple-edit pass below.
    SmallVector<std::string, 8> newGeneratedPieces;
    for (size_t i = 0; i < fixedCalleeActuals; ++i)
      newGeneratedPieces.push_back((*mergedSolvedActuals)[i]);
    if (calleeHasVariadic) {
      StringRef tail = StringRef((*mergedSolvedActuals).back()).trim();
      if (!tail.empty())
        newGeneratedPieces.push_back(tail.str());
    }

    // Translate the solved callee actuals back to the tuple elements consumed by
    // the forwarding macro.  Non-variadic forwarding parameters map to one tuple
    // element each.  A variadic forwarding parameter maps to the whole remaining
    // tuple tail, so insertion/removal of callee variadic actuals is represented
    // by replacing that tail slice.
    SmallVector<TupleEdit, 8> edits;
    size_t pieceCursor = 0;
    for (const GeneratedTupleCalleeArgRef &ref : generatedArgs) {
      if (ref.variadicPack) {
        if (ref.forwarderParamIdx >= tupleElems.size())
          return false;
        std::string text;
        raw_string_ostream os(text);
        bool first = true;
        while (pieceCursor < newGeneratedPieces.size()) {
          if (!first)
            os << ", ";
          first = false;
          os << StringRef(newGeneratedPieces[pieceCursor]).trim();
          ++pieceCursor;
        }
        os.flush();
        const TupleElementSlice &firstElem = tupleElems[ref.forwarderParamIdx];
        const TupleElementSlice &lastElem = tupleElems.back();
        edits.push_back(
            TupleEdit{firstElem.trimBegin, lastElem.trimEnd, std::move(text)});
        continue;
      }
      if (pieceCursor >= newGeneratedPieces.size() ||
          ref.forwarderParamIdx >= tupleElems.size())
        return false;
      const TupleElementSlice &elem = tupleElems[ref.forwarderParamIdx];
      StringRef oldText = pieceCursor < oldGeneratedPiecesForRewrite.size()
                              ? StringRef(oldGeneratedPiecesForRewrite[pieceCursor])
                                    .trim()
                              : StringRef(oldGeneratedActualPieces[pieceCursor])
                                    .trim();
      StringRef newText = StringRef(newGeneratedPieces[pieceCursor]).trim();
      if (newText != tuplePayload.slice(elem.trimBegin, elem.trimEnd).trim()) {
        std::optional<std::string> rewrittenElem =
            RewriteTupleElementFromSolvedExpansion(
                TupleElementText(tuplePayload, tupleElems, ref.forwarderParamIdx),
                oldText, newText);
        if (!rewrittenElem)
          return false;
        edits.push_back(
            TupleEdit{elem.trimBegin, elem.trimEnd, std::move(*rewrittenElem)});
      }
      ++pieceCursor;
    }
    if (pieceCursor != newGeneratedPieces.size())
      return false;
    if (edits.empty())
      return false;

    llvm::sort(edits, [](const TupleEdit &lhs, const TupleEdit &rhs) {
      if (lhs.begin != rhs.begin)
        return lhs.begin > rhs.begin;
      return lhs.end > rhs.end;
    });

    std::string rebuiltPayload = tuplePayload.str();
    size_t previousBegin = std::numeric_limits<size_t>::max();
    for (const TupleEdit &edit : edits) {
      if (edit.end < edit.begin || edit.end > rebuiltPayload.size())
        return false;
      if (previousBegin != std::numeric_limits<size_t>::max() &&
          edit.end > previousBegin)
        return false;
      previousBegin = edit.begin;
      rebuiltPayload = stringutils::replaceRange(rebuiltPayload, edit.begin,
                                                 edit.end, edit.text);
    }

    outNewArg = ("(" + StringRef(rebuiltPayload).trim().str() + ")");
    return true;
  }

private:
  struct TupleEdit {
    size_t begin = 0;
    size_t end = 0;
    std::string text;
  };

  /// Resolves a macro name through a deterministic object-like alias chain to a
  /// unique function-like definition.
  ///
  /// Tuple-generated-callee proofs use this for two different source-preserving
  /// cases: the root wrapper can name the forwarding macro through an alias
  /// (`CALL_ALIAS PAIR`), and the tuple's callee element can itself be an alias
  /// chain (`FSEL1 -> FSEL2 -> ADD_ONE`).  The alias is used only as proof
  /// evidence; the tuple source spelling is never replaced by the resolved name.
  /// Each hop must be a unique object-like `#define` with exactly one literal
  /// replacement token, and the walk is bounded by the directive table so cycles
  /// or ambiguous macro-state histories fail closed.
  const RefoldModel::MacroDirective *
  ResolveFunctionLikeMacroThroughObjectAliases(StringRef startName) const {
    if (startName.empty())
      return nullptr;

    SmallVector<std::string, 8> seen;
    std::string current = startName.str();
    for (size_t depth = 0; depth <= deps_.model.GetMacroDirectives().size();
         ++depth) {
      if (llvm::is_contained(seen, current))
        return nullptr;
      seen.push_back(current);

      const RefoldModel::MacroDirective *functionLike = nullptr;
      const RefoldModel::MacroDirective *alias = nullptr;
      for (const RefoldModel::MacroDirective &directive :
           deps_.model.GetMacroDirectives()) {
        if (directive.subkind != "#define" ||
            directive.name != StringRef(current))
          continue;
        if (directive.functionLike) {
          if (functionLike)
            return nullptr;
          functionLike = &directive;
          continue;
        }
        if (directive.replacementTokens.size() == 1 &&
            directive.replacementTokens[0].kind ==
                RefoldModel::MacroReplacementTokenKind::Literal) {
          if (alias)
            return nullptr;
          alias = &directive;
        }
      }

      if (functionLike)
        return functionLike;
      if (!alias)
        return nullptr;
      current = alias->replacementTokens[0].spelling.str();
    }
    return nullptr;
  }

  /// Resolves the tuple callee element through the same exact alias proof used
  /// for the forwarding macro.  This admits chains such as
  /// `FSEL1 -> FSEL2 -> ADD_ONE` while preserving the original tuple spelling in
  /// the reconstructed source.
  const RefoldModel::MacroDirective *
  ResolveFunctionLikeCallee(StringRef calleeSpelling) const {
    return ResolveFunctionLikeMacroThroughObjectAliases(calleeSpelling);
  }

  /// Returns the trimmed source text for one positional tuple element.
  static StringRef TupleElementText(StringRef tuplePayload,
                                    ArrayRef<TupleElementSlice> tupleElems,
                                    size_t elemIdx) {
    const TupleElementSlice &elem = tupleElems[elemIdx];
    return tuplePayload.slice(elem.trimBegin, elem.trimEnd).trim();
  }

  /// Returns a single replay-token spelling, rejecting multi-token text.
  std::optional<std::string> SingleTokenSpelling(StringRef text) const {
    SmallVector<ReplayTok, 4> toks;
    lexReplayTokens(text, deps_.lexLang, toks);
    if (toks.size() != 1)
      return std::nullopt;
    return toks.front().spelling;
  }

  /// Applies one solved generated-callee change back into the source tuple
  /// element.  The replacement is accepted only when the old expansion is either
  /// the whole element or a unique trimmed subrange of that element.
  static std::optional<std::string> RewriteTupleElementFromSolvedExpansion(
      StringRef source, StringRef oldExpansion, StringRef newExpansion) {
    oldExpansion = oldExpansion.trim();
    newExpansion = newExpansion.trim();
    if (source == oldExpansion)
      return newExpansion.str();
    if (auto loc = findUniqueTrimmedSubstring(source, oldExpansion))
      return stringutils::replaceRange(source.str(), loc->first, loc->second,
                                       newExpansion);
    return std::nullopt;
  }

  /// Delegates old-expansion proof to the existing matcher without changing its
  /// left-to-right replay semantics or ambiguity handling.
  static bool MatchOldExpansion(const OldExpansionReplayMatcher &matcher,
                                StringRef oldExpansion) {
    return matcher.Match(oldExpansion);
  }

  /// Solves the single-node stringify/paste cases that have exact inverse
  /// spelling rules.  All other generated-callee patterns are left to
  /// `NewExpansionUniqueSolver`, preserving the existing unique-solution policy.
  std::optional<SmallVector<std::string, 8>> SolveStringifyOrPasteNewExpansion(
      ArrayRef<StandardArgsGeneratedCalleeReplayElem> calleePattern,
      const RefoldModel::MacroDirective &calleeDefinition,
      ArrayRef<std::string> oldActuals, StringRef newExpansion) const {
    if (calleePattern.size() != 1)
      return std::nullopt;
    const StandardArgsGeneratedCalleeReplayElem &elem = calleePattern.front();
    SmallVector<std::string, 8> actuals;
    actuals.resize(calleeDefinition.defParams.size());
    for (size_t i = 0; i < oldActuals.size(); ++i)
      actuals[i] = oldActuals[i];

    if (elem.kind == StandardArgsGeneratedCalleeReplayKind::Stringify) {
      std::optional<std::string> token = SingleTokenSpelling(newExpansion);
      if (!token)
        return std::nullopt;
      std::optional<std::string> content =
          decodeSimpleStringLiteralToken(*token);
      if (!content)
        return std::nullopt;
      actuals[elem.paramIdx] = std::move(*content);
      return actuals;
    }

    if (elem.kind != StandardArgsGeneratedCalleeReplayKind::Paste)
      return std::nullopt;
    std::optional<std::string> pasted = SingleTokenSpelling(newExpansion);
    if (!pasted)
      return std::nullopt;

    size_t cursor = 0;
    SmallVector<std::optional<std::string>, 8> assigned;
    assigned.resize(calleeDefinition.defParams.size());
    for (const StandardArgsGeneratedCalleePastePiece &piece : elem.pastePieces) {
      if (piece.isParam) {
        const size_t width = StringRef(oldActuals[piece.paramIdx]).trim().size();
        if (cursor + width > pasted->size())
          return std::nullopt;
        std::string slice =
            StringRef(*pasted).slice(cursor, cursor + width).str();
        cursor += width;
        if (assigned[piece.paramIdx] && *assigned[piece.paramIdx] != slice)
          return std::nullopt;
        assigned[piece.paramIdx] = std::move(slice);
        continue;
      }
      if (!StringRef(*pasted).substr(cursor).starts_with(piece.literal))
        return std::nullopt;
      cursor += piece.literal.size();
    }
    if (cursor != pasted->size())
      return std::nullopt;
    for (size_t i = 0; i < assigned.size(); ++i)
      if (assigned[i])
        actuals[i] = std::move(*assigned[i]);
    return actuals;
  }

  const RefoldMacroStandardArgsOnlyPatchBuilder::Dependencies &deps_;
  const RefoldModel::MacroInvocation &invocation_;
};

/// Derives missing direct tuple-element rewrites through a forwarded callee.
///
/// Direct tuple-ref reconstruction first maps observed child expansion text back
/// to tuple-element text.  A generated-callee child can hide a tuple element
/// inside a second macro expansion, leaving no direct old-text key for that
/// element.  This resolver owns only that forwarded-callee bridge: it proves the
/// child replacement list is a generated function-like invocation built from
/// tuple-ref formals, resolves the generated callee to one unique definition,
/// replays that callee with `ForwardedGeneratedCalleeReplaySolver`, and mutates
/// only `newTextByOld` with additional tuple-element rewrites that are uniquely
/// implied by every matching occurrence.
///
/// Unsupported replacement-list operators, malformed generated invocation
/// shapes, missing definitions, or non-applicable child shapes return success
/// without adding rewrites so the caller's existing fallback policy is
/// preserved.  Ambiguous callee definitions, non-unique replay solutions, or
/// conflicts with already-derived rewrites return false and reject the tuple
/// rewrite fail-closed.
class ForwardedTupleElementRewriteResolver {
public:
  explicit ForwardedTupleElementRewriteResolver(
      const RefoldMacroStandardArgsOnlyPatchBuilder::Dependencies &deps)
      : deps_(deps) {}

  /// Attempts to add tuple-element rewrites implied through the forwarded callee.
  bool Derive(const RefoldModel::MacroInvocation *tupleChild,
              ArrayRef<std::pair<uint32_t, StringRef>> childArgs,
              ArrayRef<OccObservation> occObservations,
              StringMap<std::string> &newTextByOld) const {
    if (!tupleChild || !tupleChild->definitionDirectiveId)
      return true;

    const RefoldModel::MacroDirective *childDefinition = nullptr;
    for (const RefoldModel::MacroDirective &directive :
         deps_.model.GetMacroDirectives()) {
      if (directive.id == *tupleChild->definitionDirectiveId) {
        childDefinition = &directive;
        break;
      }
    }
    if (!childDefinition || childDefinition->subkind != "#define" ||
        !childDefinition->functionLike)
      return true;

    // The accepted forwarding shape is a replacement list of the form
    //   <callee-param> '(' <argument-param/literal tape> ')'
    // with the callee and each argument coming from direct tuple refs. This is a
    // syntactic proof of a generated call, not a name-based heuristic.
    const auto &repToks = childDefinition->replacementTokens;
    if (repToks.size() < 4)
      return true;
    if (repToks[0].kind != RefoldModel::MacroReplacementTokenKind::ParamRef ||
        !repToks[0].paramIndex || *repToks[0].paramIndex >= childArgs.size())
      return true;
    if (repToks[1].kind != RefoldModel::MacroReplacementTokenKind::Literal ||
        repToks[1].spelling != "(")
      return true;
    if (repToks.back().kind !=
            RefoldModel::MacroReplacementTokenKind::Literal ||
        repToks.back().spelling != ")")
      return true;

    const uint32_t calleeChildArgIdx = *repToks[0].paramIndex;
    std::optional<StringRef> calleeName =
        ChildArgText(childArgs, calleeChildArgIdx);
    if (!calleeName || calleeName->empty())
      return true;

    FunctionLikeCalleeResolution calleeResolution =
        ResolveUniqueFunctionLikeCallee(*calleeName);
    if (calleeResolution.ambiguous)
      return false;
    const RefoldModel::MacroDirective *calleeDefinition =
        calleeResolution.definition;
    if (!calleeDefinition)
      return true;

    SmallVector<uint32_t, 4> forwardedChildArgs;
    for (size_t i = 2, e = repToks.size() - 1; i < e; ++i) {
      const auto &tok = repToks[i];
      if (tok.spelling == "#" || tok.spelling == "##" ||
          tok.spelling == "__VA_OPT__")
        return true;
      if (tok.kind != RefoldModel::MacroReplacementTokenKind::ParamRef)
        continue;
      if (!tok.paramIndex || *tok.paramIndex >= childArgs.size())
        return true;
      forwardedChildArgs.push_back(*tok.paramIndex);
    }
    if (forwardedChildArgs.empty() ||
        forwardedChildArgs.size() != calleeDefinition->defParams.size())
      return true;

    SmallVector<std::string, 4> oldActuals;
    oldActuals.reserve(forwardedChildArgs.size());
    for (uint32_t childArgIdx : forwardedChildArgs) {
      auto text = ChildArgText(childArgs, childArgIdx);
      if (!text)
        return true;
      oldActuals.push_back(text->str());
    }

    SmallVector<ForwardedGeneratedCalleeReplayElem, 16> calleePattern;
    for (const RefoldModel::MacroReplacementToken &tok :
         calleeDefinition->replacementTokens) {
      if (tok.spelling == "#" || tok.spelling == "##" ||
          tok.spelling == "__VA_OPT__")
        return true;
      ForwardedGeneratedCalleeReplayElem elem;
      if (tok.kind == RefoldModel::MacroReplacementTokenKind::ParamRef) {
        if (!tok.paramIndex || *tok.paramIndex >= oldActuals.size())
          return true;
        elem.isParam = true;
        elem.paramIdx = *tok.paramIndex;
      } else {
        elem.literal = tok.spelling.str();
      }
      calleePattern.push_back(std::move(elem));
    }
    if (calleePattern.empty())
      return true;

    SmallVector<SmallVector<std::string, 8>, 4> oldActualTokSpellings;
    for (const std::string &actual : oldActuals)
      oldActualTokSpellings.push_back(
          tokenSpellingsForReplayText<8>(actual, deps_.lexLang));

    ForwardedGeneratedCalleeReplaySolver forwardedCalleeSolver(
        ArrayRef<ForwardedGeneratedCalleeReplayElem>(calleePattern.data(),
                                                     calleePattern.size()),
        ArrayRef<SmallVector<std::string, 8>>(oldActualTokSpellings.data(),
                                              oldActualTokSpellings.size()),
        calleeDefinition->defParams.size(), deps_.lexLang);

    for (const OccObservation &obs : occObservations) {
      if (!forwardedCalleeSolver.MatchOldExpansion(StringRef(obs.oldText)))
        continue;
      auto solvedActuals = forwardedCalleeSolver.SolveNewExpansion(obs.newText);
      if (!solvedActuals)
        return false;
      if (solvedActuals->size() != forwardedChildArgs.size())
        return false;

      for (size_t i = 0; i < forwardedChildArgs.size(); ++i) {
        auto oldText = ChildArgText(childArgs, forwardedChildArgs[i]);
        if (!oldText)
          return false;
        StringRef oldKey = oldText->trim();
        StringRef newValue = StringRef((*solvedActuals)[i]).trim();
        if (oldKey == newValue)
          continue;
        auto it = newTextByOld.find(oldKey);
        if (it == newTextByOld.end()) {
          newTextByOld[oldKey] = newValue.str();
          continue;
        }
        if (StringRef(it->second).trim() != newValue)
          return false;
      }
    }
    return true;
  }

private:
  struct FunctionLikeCalleeResolution {
    const RefoldModel::MacroDirective *definition = nullptr;
    bool ambiguous = false;
  };

  /// Returns the trimmed child argument text by positional child argument index.
  static std::optional<StringRef>
  ChildArgText(ArrayRef<std::pair<uint32_t, StringRef>> childArgs,
               uint32_t childArgIdx) {
    for (const auto &arg : childArgs)
      if (arg.first == childArgIdx)
        return arg.second.trim();
    return std::nullopt;
  }

  /// Resolves the generated callee to exactly one visible function-like define.
  /// More than one matching definition would make the replay proof ambiguous and
  /// therefore rejects the bridge fail-closed; no matching definition remains a
  /// non-applicable bridge so the caller may continue its existing fallback path.
  FunctionLikeCalleeResolution
  ResolveUniqueFunctionLikeCallee(StringRef calleeName) const {
    FunctionLikeCalleeResolution result;
    for (const RefoldModel::MacroDirective &directive :
         deps_.model.GetMacroDirectives()) {
      if (directive.subkind == "#define" && directive.functionLike &&
          directive.name == calleeName) {
        if (result.definition) {
          result.ambiguous = true;
          result.definition = nullptr;
          return result;
        }
        result.definition = &directive;
      }
    }
    return result;
  }

  const RefoldMacroStandardArgsOnlyPatchBuilder::Dependencies &deps_;
};

/// Rebuilds a caller tuple argument from child-invocation occurrence evidence.
///
/// The resolver owns the caller-side tuple-forwarding proof that runs after the
/// parent generated-callee and forwarded-callee bridges. It accepts only the same
/// two certified child shapes as the inline path it replaces:
/// a unique direct tuple-ref child, or a unique variadic identity-forward child.
/// The only mutation is assigning `outNewArg` after a complete rebuilt caller
/// tuple has been proven. Non-applicable shapes return false so the existing
/// caller fallback policy remains unchanged; ambiguous witnesses, duplicate old
/// text keys, invalid tuple-ref slices, and conflicting forwarded-callee
/// derivations reject fail-closed. Child iteration order, occurrence ordering,
/// right-to-left tuple-slice replacement, and generated-callee ambiguity handling
/// are intentionally preserved.
class CallerTupleForwardedRewriteResolver {
public:
  CallerTupleForwardedRewriteResolver(
      const RefoldMacroStandardArgsOnlyPatchBuilder::Dependencies &deps,
      const RefoldModel::MacroInvocation &invocation)
      : deps_(deps), invocation_(invocation) {}

  /// Try to rebuild `baseArgText` as a structural caller-tuple edit.
  bool TryRewrite(uint32_t callerArgIdx, StringRef baseArgText,
                  ArrayRef<OccObservation> occObservations,
                  std::string &outNewArg) const {
    StringRef parentTrim = baseArgText.trim();

    // There is no caller tuple to rewrite if the parent argument is empty.
    if (parentTrim.empty())
      return false;

    if (ParentTupleGeneratedCalleeRewriteResolver(deps_, invocation_)
            .TryRewrite(callerArgIdx, parentTrim, occObservations, outNewArg))
      return true;

    ParentTupleGeneratedCalleeRewriteState tupleRewriteState;
    const RefoldModel::MacroInvocation *&tupleChild =
        tupleRewriteState.tupleChild;
    TupleRewriteMode &rewriteMode = tupleRewriteState.rewriteMode;
    SmallVector<std::pair<uint32_t, StringRef>, 8> &childArgs =
        tupleRewriteState.childArgs;
    SmallVector<RefoldModel::TupleArgRef, 8> &childTupleRefs =
        tupleRewriteState.childTupleRefs;
    std::optional<uint32_t> &identityForwardChildArgIdx =
        tupleRewriteState.identityForwardChildArgIdx;

    // Search direct children of the current macro invocation for exactly one
    // forwarding witness. Multiple usable children would make the caller tuple
    // rewrite ambiguous, so the code fails closed if more than one is found.
    for (const auto &cand : deps_.model.GetMacroInvocations()) {
      if (!cand.callerMacroId || *cand.callerMacroId != invocation_.id)
        continue;

      if (cand.normalizedInvText && !cand.normalizedInvArgTextRanges.empty() &&
          !cand.argTupleRefs.empty() &&
          cand.normalizedInvArgTextRanges.size() == cand.argTupleRefs.size()) {
        SmallVector<std::pair<uint32_t, StringRef>, 8> localChildArgs;
        SmallVector<RefoldModel::TupleArgRef, 8> localTupleRefs;
        DenseSet<StringRef> seenOldTexts;
        bool ok = false;

        for (uint32_t childArgIdx = 0; childArgIdx < cand.argTupleRefs.size();
             ++childArgIdx) {
          const auto &refs = cand.argTupleRefs[childArgIdx];

          // This proof mode accepts only one direct tuple-ref per child
          // argument. Multi-ref composition is outside this local
          // reconstruction proof.
          if (refs.size() != 1)
            continue;

          const auto &ref = refs.front();
          if (ref.callerParamIndex != callerArgIdx)
            continue;

          auto oldArgText = GetNormalizedArgText(cand, childArgIdx);
          if (!oldArgText)
            return false;

          // Duplicate old text would make the observation map ambiguous because
          // old occurrence text is used as the key when applying replacements.
          if (seenOldTexts.find(*oldArgText) != seenOldTexts.end())
            return false;
          seenOldTexts.insert(*oldArgText);

          // Validate that the tuple-ref byte range is a real slice of the
          // parent argument and that it textually agrees with the child
          // argument text.
          if (ref.callerByteEnd < ref.callerByteBegin ||
              ref.callerByteEnd > parentTrim.size())
            return false;
          StringRef slice =
              parentTrim.slice(ref.callerByteBegin, ref.callerByteEnd).trim();
          if (slice != oldArgText->trim())
            return false;

          localChildArgs.push_back({childArgIdx, *oldArgText});
          localTupleRefs.push_back(ref);
          ok = true;
        }

        if (ok) {
          // Accept exactly one child as the tuple-ref witness. A second witness
          // would give two possible reconstructions of the same caller argument.
          if (tupleChild)
            return false;
          tupleChild = &cand;
          rewriteMode = TupleRewriteMode::DirectTupleRefs;
          childArgs = std::move(localChildArgs);
          childTupleRefs = std::move(localTupleRefs);
          continue;
        }
      }

      if (!isMacroInvocationVariadicFormal(invocation_, callerArgIdx) ||
          !cand.invText || !cand.invB || cand.invArgRanges.empty() ||
          cand.argRefs.empty())
        continue;

      // Variadic forwarding wrappers may not carry tuple-specific metadata.
      // Accept a second certified shape where one child argument is a full-width
      // identity forward of the caller variadic formal. That proves the caller
      // tuple survives unchanged at the child hop, so we can safely rebuild it
      // element-by-element from the occurrence observations.
      std::optional<uint32_t> localIdentityArgIdx;
      for (uint32_t childArgIdx = 0; childArgIdx < cand.invArgRanges.size() &&
                                     childArgIdx < cand.argRefs.size();
           ++childArgIdx) {
        const auto &rng = cand.invArgRanges[childArgIdx];
        if (!rng.first || !rng.second || *rng.second < *rng.first ||
            *rng.first < *cand.invB)
          continue;

        const auto &refs = cand.argRefs[childArgIdx];
        if (refs.size() != 1)
          continue;

        const auto &ref = refs.front();
        if (ref.callerParamIndex != callerArgIdx)
          continue;

        const uint64_t relB = *rng.first - *cand.invB;
        const uint64_t relE = *rng.second - *cand.invB;
        if (relE < relB || relE > cand.invText->size())
          continue;

        StringRef rawArg =
            StringRef(*cand.invText).slice((size_t)relB, (size_t)relE);

        // Compare the arg-ref byte coverage against the trimmed child argument
        // bounds. Full-width trimmed coverage is the identity-forward proof: the
        // child argument is exactly the caller argument, modulo surrounding
        // space.
        size_t trimLead = 0;
        size_t trimEnd = rawArg.size();
        std::tie(trimLead, trimEnd) =
            stringutils::trimWsRange(rawArg, 0, rawArg.size());
        if (trimLead == trimEnd)
          continue;

        const uint64_t trimmedAbsBegin = relB + trimLead;
        const uint64_t trimmedAbsEnd = relB + trimEnd;
        if (ref.byteBegin != trimmedAbsBegin || ref.byteEnd != trimmedAbsEnd)
          continue;

        auto oldArgText = GetInvocationArgText(cand, childArgIdx);
        if (!oldArgText || oldArgText->empty())
          continue;

        // More than one identity-forwarding child argument would not provide a
        // unique positional tuple reconstruction.
        if (localIdentityArgIdx)
          return false;
        localIdentityArgIdx = childArgIdx;
      }

      if (!localIdentityArgIdx)
        continue;

      // As above, the child witness must be unique across both proof modes.
      if (tupleChild)
        return false;

      tupleChild = &cand;
      rewriteMode = TupleRewriteMode::VariadicIdentityForward;
      identityForwardChildArgIdx = *localIdentityArgIdx;
    }

    // No direct child proved either tuple-ref forwarding or identity forwarding.
    if (!tupleChild)
      return false;

    std::string rebuilt;
    bool changed = false;

    if (rewriteMode == TupleRewriteMode::DirectTupleRefs) {
      if (childArgs.empty() || childArgs.size() != childTupleRefs.size())
        return false;

      StringMap<std::string> newTextByOld;

      // Collapse occurrence observations by old text. Every occurrence of the
      // same old child text must request the same new text.
      for (const auto &obs : occObservations) {
        auto it = newTextByOld.find(StringRef(obs.oldText));
        if (it == newTextByOld.end()) {
          newTextByOld[StringRef(obs.oldText)] = obs.newText;
          continue;
        }
        if (it->second != obs.newText)
          return false;
      }

      // A tuple-forwarding child can use one tuple element as a callee and a
      // different tuple element as that callee's argument, e.g.
      // `WRAP((ADD_ONE, 10))` -> `CALL(ADD_ONE, 10)` -> `ADD_ONE(10)`. In that
      // shape the only expansion occurrence visible at the WRAP level is the
      // full callee expansion `((10) + 1)`, so the direct tuple-ref map above
      // has no key for the tuple element `10`. The resolver below owns that
      // narrow bridge and mutates only `newTextByOld`; non-applicable shapes
      // preserve the existing fallback path, while ambiguity or conflict rejects
      // this tuple rewrite fail-closed.
      if (!ForwardedTupleElementRewriteResolver(deps_).Derive(
              tupleChild, childArgs, occObservations, newTextByOld))
        return false;

      rebuilt = parentTrim.str();

      // Replace parent tuple slices from right to left so tuple-ref byte offsets
      // remain valid while editing `rebuilt`.
      SmallVector<unsigned, 8> order(childTupleRefs.size());
      for (unsigned i = 0; i < childTupleRefs.size(); ++i)
        order[i] = i;
      llvm::sort(order, [&](unsigned a, unsigned b) {
        return childTupleRefs[a].callerByteBegin >
               childTupleRefs[b].callerByteBegin;
      });

      for (unsigned idx : order) {
        const auto &pair = childArgs[idx];
        StringRef oldChildText = pair.second.trim();
        auto it = newTextByOld.find(oldChildText);
        if (it == newTextByOld.end())
          continue;

        const auto &ref = childTupleRefs[idx];
        rebuilt = stringutils::replaceRange(rebuilt, ref.callerByteBegin,
                                            ref.callerByteEnd, it->second);
        if (it->second != oldChildText)
          changed = true;
      }

      if (!changed)
        return false;

    } else if (rewriteMode == TupleRewriteMode::VariadicIdentityForward) {
      SmallVector<TupleElementSlice, 8> tupleElems;

      // Split the caller variadic argument into top-level elements using the
      // lexer-backed splitter so nested commas do not create false elements.
      if (!splitTopLevelTupleElementsWithLexer(parentTrim, deps_.lexLang,
                                               tupleElems))
        return false;

      // Identity-forward rewrites are positional: the immediate child keeps the
      // caller variadic tuple intact, so each observed occurrence must correspond
      // to exactly one top-level tuple element in order.
      if (tupleElems.size() != occObservations.size())
        return false;

      rebuilt = parentTrim.str();
      for (size_t i = tupleElems.size(); i > 0; --i) {
        const auto &elem = tupleElems[i - 1];
        StringRef oldElemText =
            parentTrim.slice(elem.trimBegin, elem.trimEnd).trim();

        // Replacements apply from right to left so earlier byte offsets stay
        // valid while we splice into the rebuilt caller tuple.
        if (oldElemText != StringRef(occObservations[i - 1].oldText).trim())
          return false;

        rebuilt = stringutils::replaceRange(rebuilt, elem.trimBegin,
                                            elem.trimEnd,
                                            occObservations[i - 1].newText);
        if (occObservations[i - 1].newText != oldElemText)
          changed = true;
      }

      if (!changed)
        return false;

    } else {
      return false;
    }

    // Return the rebuilt caller argument, normalized to the same trimmed spelling
    // convention used throughout this tuple-forwarding path.
    outNewArg = StringRef(rebuilt).trim().str();

    return true;
  }

private:
  /// Return a child argument slice from normalized invocation text. This is used
  /// by tuple-ref mode because tuple refs are expressed over normalized child
  /// argument text/ranges.
  static std::optional<StringRef>
  GetNormalizedArgText(const RefoldModel::MacroInvocation &inv,
                       uint32_t argIdx) {
    if (!inv.normalizedInvText)
      return std::nullopt;
    if (argIdx >= inv.normalizedInvArgTextRanges.size())
      return std::nullopt;
    const auto &rng = inv.normalizedInvArgTextRanges[argIdx];
    if (!rng.first || !rng.second || *rng.second < *rng.first)
      return std::nullopt;
    if (*rng.second > inv.normalizedInvText->size())
      return std::nullopt;
    return StringRef(*inv.normalizedInvText)
        .slice((size_t)*rng.first, (size_t)*rng.second)
        .trim();
  }

  /// Return a child argument slice from the raw invocation spelling. This is
  /// used by identity-forward mode, where the proof comes from raw arg-ref byte
  /// coverage rather than tuple-ref metadata.
  static std::optional<StringRef>
  GetInvocationArgText(const RefoldModel::MacroInvocation &inv,
                       uint32_t argIdx) {
    if (!inv.invText || !inv.invB)
      return std::nullopt;
    if (argIdx >= inv.invArgRanges.size())
      return std::nullopt;
    const auto &rng = inv.invArgRanges[argIdx];
    if (!rng.first || !rng.second || *rng.second < *rng.first ||
        *rng.first < *inv.invB)
      return std::nullopt;
    const uint64_t relB = *rng.first - *inv.invB;
    const uint64_t relE = *rng.second - *inv.invB;
    if (relE < relB || relE > inv.invText->size())
      return std::nullopt;
    return StringRef(*inv.invText).slice((size_t)relB, (size_t)relE).trim();
  }

  const RefoldMacroStandardArgsOnlyPatchBuilder::Dependencies &deps_;
  const RefoldModel::MacroInvocation &invocation_;
};

/// Probes higher-order generated replay candidates before ordinary occurrence
/// collection.
///
/// The builder owns only the ranking-sensitive discovery sequence that was
/// previously inline in `BuildStandardArgsOnlyPatch`: generated-callee chain
/// replay first, generated-leaf replay second, and tuple-generated-callee
/// replay third.  It mutates only the short-lived context storage required by
/// the replay engines.  A miss is non-terminal and returns nullopt; ambiguity,
/// unsupported shapes, or unprovable B envelopes therefore continue to fail
/// closed through the existing replay-engine contracts instead of creating a
/// fallback candidate.
class HigherOrderGeneratedReplayProbe {
public:
  explicit HigherOrderGeneratedReplayProbe(
      const RefoldMacroStandardArgsOnlyPatchBuilder::Dependencies &deps)
      : deps_(deps) {}

  /// Return the first higher-order generated replay candidate in the exact
  /// historical probe order, or nullopt when no generated replay proof applies.
  std::optional<MacroPatch>
  TryBuild(const RefoldModel::MacroInvocation &invocation,
           const diffutils::Hunk &hunk, StringRef baseInvocationText,
           ArrayRef<std::pair<size_t, size_t>> invocationArgRanges) const {
    if (auto generatedCalleePatch = TryBuildGeneratedCalleeReplay(
            invocation, baseInvocationText, invocationArgRanges))
      return generatedCalleePatch;

    if (auto generatedLeafPatch = TryBuildGeneratedLeafReplay(
            invocation, hunk, baseInvocationText, invocationArgRanges))
      return generatedLeafPatch;

    return TryBuildTupleGeneratedCalleeReplay(invocation, baseInvocationText,
                                             invocationArgRanges);
  }

private:
  /// Locate the invocation's defining directive with the same linear scan used
  /// by the former inline probes.  The helper performs no alias resolution and
  /// returns null for missing producer metadata.
  const RefoldModel::MacroDirective *FindRootDefinition(
      const RefoldModel::MacroInvocation &invocation) const {
    if (!invocation.definitionDirectiveId)
      return nullptr;
    for (const RefoldModel::MacroDirective &directive :
         deps_.model.GetMacroDirectives()) {
      if (directive.id == *invocation.definitionDirectiveId)
        return &directive;
    }
    return nullptr;
  }

  /// Recover the root whole-cover B envelope using the same primary mapping and
  /// boundary-insertion-preserving fallback as the former inline blocks.
  std::optional<std::pair<size_t, size_t>> MapWholeCoverBEnvelope(
      const std::pair<uint64_t, uint64_t> &wholeCoverATokens) const {
    auto bEnv = deps_.sourceMapper.MapATokRangeAToBTokenEnvelope(
        wholeCoverATokens.first, wholeCoverATokens.second);
    if (!bEnv || bEnv->first >= bEnv->second)
      bEnv = deps_.sourceMapper
                 .MapATokRangeAToBTokenEnvelopePreserveBoundaryInsertions(
                     wholeCoverATokens.first, wholeCoverATokens.second);
    if (!bEnv || bEnv->first >= bEnv->second)
      return std::nullopt;
    return bEnv;
  }

  /// Try the ordinary generated-callee chain replay proof.  This probe is first
  /// in the higher-order ranking and remains limited to non-stringify/non-paste
  /// root invocations whose current-level actuals are recoverable.
  std::optional<MacroPatch> TryBuildGeneratedCalleeReplay(
      const RefoldModel::MacroInvocation &invocation,
      StringRef baseInvocationText,
      ArrayRef<std::pair<size_t, size_t>> invocationArgRanges) const {
    if (!invocation.definitionDirectiveId || !invocation.invB ||
        !invocation.invE || !invocation.stringifySpans.empty() ||
        !invocation.pasteSpans.empty())
      return std::nullopt;

    auto cover = RefoldMacroWholeCoverProof::GetWholeCoverATokRange(invocation);
    if (!cover || cover->first >= cover->second)
      return std::nullopt;

    auto bEnv = MapWholeCoverBEnvelope(*cover);
    if (!bEnv)
      return std::nullopt;

    const RefoldModel::MacroDirective *rootDefinition =
        FindRootDefinition(invocation);
    if (!rootDefinition || rootDefinition->subkind != "#define" ||
        !rootDefinition->functionLike || rootDefinition->defParams.empty())
      return std::nullopt;

    SmallVector<GeneratedCalleeSourceSlot, 8> currentActuals;
    currentActuals.reserve(invocationArgRanges.size());
    bool actualsRecoverable = true;
    for (uint32_t i = 0; i < invocationArgRanges.size(); ++i) {
      const auto r = invocationArgRanges[i];
      if (r.second < r.first || r.second > baseInvocationText.size()) {
        actualsRecoverable = false;
        break;
      }
      GeneratedCalleeSourceSlot slot;
      slot.text = baseInvocationText.slice(r.first, r.second).trim().str();
      slot.rootSourceText = slot.text;
      slot.rootArgIdx = i;
      currentActuals.push_back(std::move(slot));
    }

    if (!actualsRecoverable ||
        !macroDefinitionAcceptsActualCount(*rootDefinition,
                                           currentActuals.size()))
      return std::nullopt;

    SmallVector<std::string, 8> replayPrefixLiterals;
    SmallVector<SmallVector<std::string, 8>, 8> replaySuffixStack;
    const RefoldModel::MacroDirective *currentDefinition = rootDefinition;
    bool followedGeneratedCall = false;
    uint32_t generatedCallDepth = 0;
    uint32_t objectAliasHopCount = 0;
    bool generatedReplayUsesStringification = false;
    bool generatedReplayUsesPaste = false;
    bool generatedReplayUsesVariadicForwarding = false;

    GeneratedCalleeReplayContext generatedCalleeCtx{
        invocation,
        baseInvocationText,
        invocationArgRanges,
        *rootDefinition,
        *cover,
        *bEnv,
        replayPrefixLiterals,
        replaySuffixStack,
        currentDefinition,
        currentActuals,
        followedGeneratedCall,
        generatedCallDepth,
        objectAliasHopCount,
        generatedReplayUsesStringification,
        generatedReplayUsesPaste,
        generatedReplayUsesVariadicForwarding};

    return deps_.generatedCalleeReplayEngine.BuildGeneratedCalleeReplayCandidate(
        generatedCalleeCtx);
  }

  /// Try the generated-leaf fallback proof after the generated-callee chain
  /// declines.  This method preserves the previous envelope repair over
  /// same-pass pure insertions and rejects non-different or empty expansion
  /// surfaces before delegating to the leaf replay engine.
  std::optional<MacroPatch> TryBuildGeneratedLeafReplay(
      const RefoldModel::MacroInvocation &invocation,
      const diffutils::Hunk &hunk, StringRef baseInvocationText,
      ArrayRef<std::pair<size_t, size_t>> invocationArgRanges) const {
    if (!invocation.definitionDirectiveId || !invocation.invB ||
        !invocation.invE)
      return std::nullopt;

    const RefoldModel::MacroDirective *rootDefinition =
        FindRootDefinition(invocation);
    if (!rootDefinition || rootDefinition->subkind != "#define" ||
        !rootDefinition->functionLike || rootDefinition->defParams.empty())
      return std::nullopt;

    bool hasGeneratedCall = false;
    const auto &rootToks = rootDefinition->replacementTokens;
    for (size_t i = 0; i + 1 < rootToks.size(); ++i) {
      if (rootToks[i].kind == RefoldModel::MacroReplacementTokenKind::ParamRef &&
          rootToks[i].paramIndex &&
          rootToks[i + 1].kind ==
              RefoldModel::MacroReplacementTokenKind::Literal &&
          rootToks[i + 1].spelling == "(") {
        hasGeneratedCall = true;
        break;
      }
    }

    auto cover = RefoldMacroWholeCoverProof::GetWholeCoverATokRange(invocation);
    if (!hasGeneratedCall || !cover || cover->first >= cover->second)
      return std::nullopt;

    auto bEnv = MapWholeCoverBEnvelope(*cover);
    if (!bEnv)
      return std::nullopt;

    StringRef oldExpansion = deps_.sourceMapper
                                 .SliceASource(cover->first, cover->second)
                                 .trim();
    StringRef newExpansion = deps_.sourceMapper
                                 .SliceBSource(bEnv->first, bEnv->second)
                                 .trim();

    if (oldExpansion == newExpansion && hunk.isInsertOnly() &&
        hunk.aStart == cover->second && hunk.bStart == bEnv->second &&
        hunk.bStart < hunk.bEnd) {
      bEnv->second = static_cast<size_t>(hunk.bEnd);
      newExpansion = deps_.sourceMapper.SliceBSource(bEnv->first, bEnv->second)
                         .trim();
    }

    bool rootHasGeneratedSelectorDescendant = false;
    for (const RefoldModel::MacroInvocation &candidate :
         deps_.model.GetMacroInvocations()) {
      const RefoldModel::MacroInvocation *cur = &candidate;
      bool isDescendant = false;
      for (size_t depth = 0;
           cur && depth <= deps_.model.GetMacroInvocations().size(); ++depth) {
        if (cur->id == invocation.id) {
          isDescendant = true;
          break;
        }
        if (!cur->callerMacroId)
          break;
        cur = deps_.macroTopology.FindMacroInvocationById(*cur->callerMacroId);
      }
      if (!isDescendant)
        continue;
      if (candidate.calleeOrigin.kind == MacroCalleeOriginKind::CallerParam &&
          !candidate.calleeOrigin.callerParamIndices.empty()) {
        rootHasGeneratedSelectorDescendant = true;
        break;
      }
    }

    if (hunk.isInsertOnly() && rootHasGeneratedSelectorDescendant &&
        (hunk.aStart == cover->first || hunk.aStart == cover->second)) {
      while (bEnv->first > 0 &&
             bEnv->first - 1 <
                 deps_.bInsertionLedger.BTokToInsertionId().size()) {
        int32_t insId =
            deps_.bInsertionLedger.BTokToInsertionId()[bEnv->first - 1];
        if (insId < 0)
          break;
        const BInsertionProv &ins =
            deps_.bInsertionLedger.Insertions()[static_cast<size_t>(insId)];
        if (ins.claim == BInsertionClaim::Standalone ||
            ins.aGap != cover->first || ins.b1 != bEnv->first)
          break;
        bEnv->first = ins.b0;
      }
      while (bEnv->second < deps_.bInsertionLedger.BTokToInsertionId().size()) {
        int32_t insId = deps_.bInsertionLedger.BTokToInsertionId()[bEnv->second];
        if (insId < 0)
          break;
        const BInsertionProv &ins =
            deps_.bInsertionLedger.Insertions()[static_cast<size_t>(insId)];
        if (ins.claim == BInsertionClaim::Standalone ||
            ins.aGap != cover->second || ins.b0 != bEnv->second)
          break;
        bEnv->second = ins.b1;
      }
      newExpansion = deps_.sourceMapper.SliceBSource(bEnv->first, bEnv->second)
                         .trim();
    }

    if (oldExpansion.empty() || newExpansion.empty() ||
        oldExpansion == newExpansion)
      return std::nullopt;

    GeneratedLeafReplayContext generatedLeafCtx{
        invocation,          hunk,   baseInvocationText, invocationArgRanges,
        *cover,             *bEnv,  *rootDefinition,    oldExpansion,
        newExpansion};
    return deps_.generatedLeafReplayEngine.BuildGeneratedLeafReplayCandidate(
        generatedLeafCtx);
  }

  /// Try the tuple-generated-callee proof after generated-leaf replay.  The
  /// probe remains restricted to a literal callee plus one forwarded tuple
  /// formal and preserves alias-hop accounting for the replay proof.
  std::optional<MacroPatch> TryBuildTupleGeneratedCalleeReplay(
      const RefoldModel::MacroInvocation &invocation,
      StringRef baseInvocationText,
      ArrayRef<std::pair<size_t, size_t>> invocationArgRanges) const {
    if (!invocation.definitionDirectiveId || !invocation.invB ||
        !invocation.invE || !invocation.stringifySpans.empty() ||
        !invocation.pasteSpans.empty())
      return std::nullopt;

    auto cover = RefoldMacroWholeCoverProof::GetWholeCoverATokRange(invocation);
    if (!cover || cover->first >= cover->second)
      return std::nullopt;

    auto bEnv = MapWholeCoverBEnvelope(*cover);
    if (!bEnv)
      return std::nullopt;

    const RefoldModel::MacroDirective *rootDefinition =
        FindRootDefinition(invocation);
    if (!rootDefinition || rootDefinition->subkind != "#define" ||
        !rootDefinition->functionLike ||
        rootDefinition->replacementTokens.size() != 2)
      return std::nullopt;

    const auto &rootTok0 = rootDefinition->replacementTokens[0];
    const auto &rootTok1 = rootDefinition->replacementTokens[1];
    if (rootTok0.kind != RefoldModel::MacroReplacementTokenKind::Literal ||
        rootTok1.kind != RefoldModel::MacroReplacementTokenKind::ParamRef ||
        !rootTok1.paramIndex ||
        *rootTok1.paramIndex >= invocationArgRanges.size())
      return std::nullopt;

    const uint32_t callerArgIdx = *rootTok1.paramIndex;
    uint32_t tupleObjectAliasHopCount = 0;
    uint32_t forwarderAliasHops = 0;
    const RefoldModel::MacroDirective *forwarderDefinition =
        deps_.resolveFunctionLikeMacroThroughAliasesWithHops(
            rootTok0.spelling, &forwarderAliasHops);
    tupleObjectAliasHopCount += forwarderAliasHops;
    if (!forwarderDefinition || forwarderDefinition->defParams.empty())
      return std::nullopt;

    TupleGeneratedCalleeReplayContext tupleGeneratedCtx{
        invocation,
        baseInvocationText,
        invocationArgRanges,
        *cover,
        *bEnv,
        *rootDefinition,
        *forwarderDefinition,
        callerArgIdx,
        tupleObjectAliasHopCount};
    return deps_.generatedCalleeReplayEngine
        .BuildTupleGeneratedCalleeReplayCandidate(tupleGeneratedCtx);
  }

  const RefoldMacroStandardArgsOnlyPatchBuilder::Dependencies &deps_;
};

/// Builds the pure paste-only args-only candidate once the caller has proven
/// that no STANDARD or STRINGIFY occurrence evidence is available.
///
/// This private builder owns only the paste-only fallback construction block:
/// it derives per-argument paste edits, requires every paste occurrence for the
/// invocation to agree with the merged replacements, rebuilds the invocation,
/// and certifies the same paste-only proof as the former inline path.  It
/// mutates only the returned `MacroPatch` and the existing proof lattice /
/// certifier state.  It does not decide candidate ordering, does not relax the
/// caller's pure-paste admission guard, and fails closed on every ambiguous
/// segment split, arity-changing comma introduction, conflicting repeated paste
/// occurrence, or failed invocation reconstruction.
class PurePasteOnlyArgsOnlyCandidateBuilder {
public:
  PurePasteOnlyArgsOnlyCandidateBuilder(
      const RefoldMacroStandardArgsOnlyPatchBuilder::Dependencies &deps,
      RefoldMacroPasteArgumentBuilder pasteBuilder)
      : deps_(deps), pasteBuilder_(std::move(pasteBuilder)) {}

  /// Return the paste-only args-only candidate for this invocation, or nullopt
  /// under exactly the same rejection conditions as the previous inline
  /// fallback block.
  std::optional<MacroPatch>
  TryBuild(const RefoldModel::MacroInvocation &invocation,
           const diffutils::Hunk &hunk, StringRef baseInvocationText,
           ArrayRef<std::pair<size_t, size_t>> invocationArgRanges,
           const InvocationActualRecoveryContext &actualCtx) const {
    // First derive per-argument paste edits from the current hunk. The
    // derivation proves that the edited pasted-token spelling can be mapped
    // back to argument segments rather than arbitrary token substrings.
    auto edits = pasteBuilder_.DerivePasteArgEdits(invocation, hunk);
    if (!edits || edits->empty())
      return std::nullopt;

    // Merge all derived paste edits into one replacement spelling per
    // invocation argument. Multiple pasted-token occurrences may refer to the
    // same formal, but they must all demand the same final argument spelling.
    DenseMap<uint32_t, std::string> replByArgIdx;
    for (const auto &pae : *edits) {
      const uint32_t argIdx = pae.argIdx;
      if (static_cast<size_t>(argIdx) >= invocationArgRanges.size())
        return std::nullopt;

      // Reconstruct the full invocation-argument spelling by replacing the
      // derived old paste segment with the derived new paste segment. Prefer
      // the exact byte-window splice when the paste witness identifies the
      // segment boundaries inside the argument.
      auto range = invocationArgRanges[argIdx];
      StringRef baseArgText = baseInvocationText.substr(
          range.first, range.second - range.first);
      std::string newArg =
          (pae.argByteBegin && pae.argByteEnd)
              ? RefoldMacroPasteSpelling::
                    SplicePasteSegmentIntoSpellingArgExact(
                        baseArgText, *pae.argByteBegin, *pae.argByteEnd,
                        pae.oldSeg, pae.newSeg)
              : RefoldMacroPasteSpelling::SplicePasteSegmentIntoSpellingArg(
                    baseArgText, pae.oldSeg, pae.newSeg);

      // An empty splice result normally means the old segment could not be
      // found or replaced safely. The one accepted empty-result case is a true
      // no-op where the new segment is empty and the original argument was
      // exactly the old segment after trimming.
      if (newArg.empty()) {
        if (!(StringRef(pae.newSeg).trim().empty() &&
              baseArgText.trim() == StringRef(pae.oldSeg).trim()))
          return std::nullopt;
      }

      // A non-variadic macro formal cannot be rewritten to text containing a
      // top-level comma, because that would change call-site arity.
      if (!isMacroInvocationVariadicFormal(invocation, argIdx) &&
          replacementIntroducesTopLevelComma(newArg, deps_.lexLang))
        return std::nullopt;

      // If the same formal was observed through multiple pasted tokens, require
      // every occurrence to reconstruct the exact same replacement argument.
      auto existing = replByArgIdx.find(argIdx);
      if (existing != replByArgIdx.end()) {
        if (existing->second != newArg)
          return std::nullopt;
        continue;
      }

      replByArgIdx[argIdx] = std::move(newArg);
    }

    // No argument changed after merging, so there is no invocation rewrite to
    // propose from this fallback.
    if (replByArgIdx.empty())
      return std::nullopt;

    // Validate the merged argument replacements globally against every pasted
    // token occurrence in B. This prevents accepting a rewrite that explains
    // only the touched token while breaking another paste occurrence from the
    // same invocation.
    if (!pasteBuilder_.PasteArgReplacementsMatchAllPasteTokensInB(
            invocation, baseInvocationText, invocationArgRanges,
            replByArgIdx))
      return std::nullopt;

    std::optional<InvocationRewriteWithRange> rewrite =
        deps_.buildInvocationRewriteWithRange(
            actualCtx, replByArgIdx,
            /*materializedRangeByArgIdx=*/nullptr);
    if (!rewrite)
      return std::nullopt;

    MacroPatch patch{*invocation.invB, *invocation.invE,
                     std::move(rewrite->text), invocation.id};
    deps_.proofCertifier.CertifyInvocationRewriteMaterializedOutputRange(
        patch, rewrite->materializedOutputByteStart,
        rewrite->materializedOutputByteEnd);
    // Pure-paste-only replay has no standard/stringify occurrence to define a
    // smaller B-side surface. The proved replay unit is the full expansion cover
    // reconstructed from the rewritten invocation arguments.
    deps_.certifyMacroPatchWholeExpansionBRange(invocation, patch);
    deps_.proofLattice.SetMacroPatchProof(
        patch, deps_.proofLattice.MakeMacroPatchProof(
                   MacroPatchProofKind::ArgsOnlyPurePasteOnly,
                   /*preservesInvocationStructure=*/true, invocation.id));
    // Pure-paste-only rewrites have no standard or stringify occurrences to
    // lean on, so successful all-paste replay is the decisive proof source.
    patch.pasteReplayValidated = true;
    deps_.proofLattice.MacroPatchProofClassifier().SyncMacroPatchProofSummary(
        patch);
    return patch;
  }

private:
  const RefoldMacroStandardArgsOnlyPatchBuilder::Dependencies &deps_;
  RefoldMacroPasteArgumentBuilder pasteBuilder_;
};

/// Builds the final standard-args-only invocation patch and certifies its proof.
///
/// This resolver owns only the final proof/candidate construction obligation for
/// the ordinary standard-argument replay path.  It consumes the already-finalized
/// argument replacement map, delegates invocation text rebuilding to the existing
/// planner callback, certifies the same materialized output and whole-expansion
/// B range as before, and assigns the same args-only standard proof summary.
/// It mutates only the returned `MacroPatch` and the existing proof certifier; it
/// does not discover new rewrites, reorder candidates, relax replay validation,
/// or introduce a fallback when the rewrite cannot be rebuilt.
class ArgsOnlyProofCertifier {
public:
  explicit ArgsOnlyProofCertifier(
      const RefoldMacroStandardArgsOnlyPatchBuilder::Dependencies &deps)
      : deps_(deps) {}

  /// Return the accepted patch for the completed rewrite set, or nullopt when
  /// there is no replacement or invocation reconstruction fails exactly as in
  /// the previous inline certification block.
  std::optional<MacroPatch>
  BuildAcceptedCandidate(const RefoldModel::MacroInvocation &invocation,
                         const InvocationActualRecoveryContext &actualCtx,
                         const ArgsOnlyFinalArgumentRewriteSet &rewriteSet)
      const {
    if (rewriteSet.Empty())
      return std::nullopt;

    std::optional<InvocationRewriteWithRange> rewrite =
        deps_.buildInvocationRewriteWithRange(
            actualCtx, rewriteSet.replacementsByArgIdx,
            &rewriteSet.materializedRangeByArgIdx);
    if (!rewrite)
      return std::nullopt;

    MacroPatch patch{*invocation.invB, *invocation.invE,
                     std::move(rewrite->text), invocation.id};
    deps_.proofCertifier.CertifyInvocationRewriteMaterializedOutputRange(
        patch, rewrite->materializedOutputByteStart,
        rewrite->materializedOutputByteEnd);
    // The materialized output byte range may remain narrowed to an inserted
    // payload inside one argument, but the target-PP proof for an
    // invocation-preserving macro repair is the B-side expansion envelope of the
    // whole macro owner.  Keeping the output byte range narrow is useful for
    // source-spelling edits; leaving the B-token range uncertified would make
    // append/pure-insertion repairs look theorem-incomplete even after the
    // occurrence replay proved that the rewritten invocation regenerates the
    // edited expansion.
    deps_.certifyMacroPatchWholeExpansionBRange(invocation, patch);
    deps_.proofCertifier.SetArgsOnlyStandardProof(
        patch, invocation, /*wholeEnvelopeReplayValidated=*/false);
    return patch;
  }

private:
  const RefoldMacroStandardArgsOnlyPatchBuilder::Dependencies &deps_;
};

} // namespace

std::optional<MacroPatch>
RefoldMacroStandardArgsOnlyPatchBuilder::BuildStandardArgsOnlyPatch(
    const ArgsOnlyPlanningContext &ctx) const {
  const RefoldModel::MacroInvocation &m = ctx.invocation;
  const diffutils::Hunk &h = ctx.hunk;
  const diffutils::Hunk &hArgs = ctx.hunk;
  StringRef baseInvText = ctx.baseInvocationText;
  ArrayRef<std::pair<size_t, size_t>> invArgRanges =
      ctx.actualLayout.rangePairs();
  const InvocationActualRecoveryContext actualRecoveryCtx{m, baseInvText,
                                                          invArgRanges};
  const ArgsOnlyTemplateReplayContext argsOnlyTemplateCtx{m, baseInvText,
                                                          invArgRanges};

  // Standard (non-paste) args-only policy:
  // Collect arg-span occurrences (and stringify occurrences) and require the
  // entire hunk to be covered by those spans. Then derive per-arg replacements
  // from the B slices. The immutable producer #define repair remains private to
  // this translation unit and reports its former side effect explicitly.
  bool replayedStandardArgSpanFormalIndices = false;

  // Prefer the direct current-level invocation parse; fall back to definition
  // replay only for older producer shapes where the replacement-list tape
  // proves the same reindexing.
  std::vector<RefoldModel::PPArgSpan> standardArgSpans;
  if (auto currentLevelSpans = TemplateSolver().GetCurrentLevelStandardArgSpans(
          argsOnlyTemplateCtx)) {
    standardArgSpans = std::move(*currentLevelSpans);
  } else {
    DefinitionReplayedStandardArgSpanRepair repair =
        getDefinitionReplayedStandardArgSpans(deps_.model, deps_.aToks, m);
    replayedStandardArgSpanFormalIndices = repair.replayedFormalIndices;
    standardArgSpans = std::move(repair.argSpans);
  }

  TouchedFormalHunkCollection touchedFormalHunks;
  std::vector<RefoldModel::PPArgSpan> &occs =
      touchedFormalHunks.occurrences;
  std::vector<char> &occIsStringify =
      touchedFormalHunks.occurrenceIsStringify;

  append_range(occs, standardArgSpans);
  append_range(occs, m.stringifySpans);

  occIsStringify.resize(occs.size());
  std::fill_n(occIsStringify.begin(), standardArgSpans.size(), false);
  std::fill_n(occIsStringify.begin() + standardArgSpans.size(),
              m.stringifySpans.size(), true);

  // Pure paste-only invocations have no STANDARD or STRINGIFY evidence, so the
  // normal args-only path bottoms out at occs.empty(). Keep this ranking guard
  // in the main orchestration body, then delegate only the deterministic
  // paste-edit derivation and proof-candidate construction.
  if (occs.empty() && !m.pasteSpans.empty()) {
    return PurePasteOnlyArgsOnlyCandidateBuilder(deps_, PasteArgumentBuilder())
        .TryBuild(m, hArgs, baseInvText, invArgRanges, actualRecoveryCtx);
  }

  // Higher-order generated replay remains in its historical ranking position
  // before ordinary occurrence collection.  The private probe owns only the
  // generated-callee / generated-leaf / tuple-generated-callee discovery
  // sequence and returns nullopt for a non-terminal miss.
  if (auto higherOrderGeneratedPatch =
          HigherOrderGeneratedReplayProbe(deps_).TryBuild(m, h, baseInvText,
                                                          invArgRanges))
    return higherOrderGeneratedPatch;

  RefoldMacroOccurrenceReplay occurrenceReplay = OccurrenceReplay();
  std::optional<TouchedFormalHunkCollection> collectedTouchedFormalHunks =
      TouchedFormalHunkCollector(deps_, occurrenceReplay)
          .Collect(touchedFormalHunks, m, hArgs, invArgRanges.size());
  if (!collectedTouchedFormalHunks)
    return std::nullopt;
  touchedFormalHunks = std::move(*collectedTouchedFormalHunks);

  // `touched` is now indexed by formal argument, not occurrence. Later checks
  // use it to decide which invocation arguments need replacement and which must
  // remain unchanged.  `tokenHunks` is the normalized authoritative hunk set for
  // those touched formal arguments.
  std::vector<char> &touched = touchedFormalHunks.touchedFormals;
  ArrayRef<diffutils::Hunk> tokenHunks(touchedFormalHunks.tokenHunks);

  // Caller-tuple forwarding remains in the same ranking position as before;
  // only the proof/search body is isolated in a private resolver.
  CallerTupleForwardedRewriteResolver callerTupleForwardedRewriteResolver(
      deps_, m);
  // Definition-replayed span repair has its own all-occurrences proof because it
  // validates against the repaired `standardArgSpans` vector rather than the raw
  // producer occurrence indices.
  ReplayedFormalOccurrenceValidator replayedFormalOccurrenceValidator(
      deps_, occurrenceReplay);

  // Compute argument replacements implied by each touched occurrence. Multiple
  // occurrences of the same argIdx must imply the exact same replacement,
  // otherwise the macro cannot be refolded args-only.  Finalized replacements
  // are recorded in a carrier so proof certification consumes explicit, completed
  // state rather than continuing to own occurrence-discovery locals.
  ArgsOnlyFinalArgumentRewriteSet finalArgumentRewrites;
  SmallVector<uint32_t, 8> touchedArgIdxs;
  for (const auto &sp : occs) {
    if (sp.argIdx >= touched.size() || !touched[sp.argIdx])
      continue;
    if (!llvm::is_contained(touchedArgIdxs, sp.argIdx))
      touchedArgIdxs.push_back(sp.argIdx);
  }

  for (uint32_t argIdx : touchedArgIdxs) {
    if (static_cast<size_t>(argIdx) >= invArgRanges.size())
      return std::nullopt;

    auto r0 = invArgRanges[argIdx];
    StringRef baseArgText = baseInvText.substr(r0.first, r0.second - r0.first);

    std::optional<InvocationOccurrenceObservationSet> collectedOccurrences =
        InvocationOccurrenceObservationCollector(deps_, occurrenceReplay)
            .Collect(m, argIdx, baseArgText, occs, occIsStringify, tokenHunks, h);
    if (!collectedOccurrences)
      return std::nullopt;

    InvocationOccurrenceObservationSet occurrenceObservation =
        std::move(*collectedOccurrences);
    SmallVector<OccObservation, 8> &occObservations =
        occurrenceObservation.observations;
    const std::optional<std::string> &unifiedNewArg =
        occurrenceObservation.unifiedNewArg;
    const std::optional<std::pair<uint64_t, uint64_t>>
        &unifiedMaterializedNewTextRange =
            occurrenceObservation.unifiedMaterializedNewTextRange;
    const bool sawUntrackedMaterializedNewTextRange =
        occurrenceObservation.sawUntrackedMaterializedNewTextRange;
    const bool needTupleForwarding = occurrenceObservation.needTupleForwarding;

    std::string finalNewArg;
    bool tupleForwarded = false;

    if (needTupleForwarding) {
      // Occurrence observations for this formal did not collapse to one uniform
      // replacement. Try the narrower tuple-forwarding proof before rejecting
      // the args-only rewrite outright.
      if (!callerTupleForwardedRewriteResolver.TryRewrite(
          argIdx, baseArgText, occObservations, finalNewArg)) {
        return std::nullopt;
      }
      tupleForwarded = true;
    } else if (unifiedNewArg) {
      // All observed occurrences of this formal agreed on one replacement
      // spelling.  Before accepting a whole-argument expansion replacement,
      // give direct tuple-ref forwarding a chance to prove a more structural
      // edit of a caller tuple element.  This covers generated-callee shapes
      // such as `WRAP((ADD_ONE, 10))`, where the root occurrence is the full
      // callee expansion but the actual source edit belongs to the tuple
      // element `10`.
      if (callerTupleForwardedRewriteResolver.TryRewrite(
          argIdx, baseArgText, occObservations, finalNewArg)) {
        tupleForwarded = true;
      } else {
        finalNewArg = *unifiedNewArg;
      }
    } else {
      // This formal had no usable observation from the touched hunk set.
      continue;
    }

    std::optional<std::pair<uint64_t, uint64_t>> finalMaterializedRange;
    if (!tupleForwarded && !sawUntrackedMaterializedNewTextRange &&
        unifiedMaterializedNewTextRange && unifiedNewArg &&
        finalNewArg == *unifiedNewArg) {
      finalMaterializedRange = *unifiedMaterializedNewTextRange;
    }

    // Replacing a non-variadic formal with a top-level comma would change macro
    // invocation arity, so reject it before validating occurrence consistency.
    if (!isMacroInvocationVariadicFormal(m, argIdx) &&
        replacementIntroducesTopLevelComma(finalNewArg, deps_.lexLang))
      return std::nullopt;

    TupleSliceConsistencyValidator tupleSliceConsistencyValidator(deps_,
                                                                 occurrenceReplay);

    const bool matchesAllOccurrences =
        tupleForwarded ? tupleSliceConsistencyValidator.Validate(
                             argIdx, standardArgSpans, occObservations, tokenHunks)
        : replayedStandardArgSpanFormalIndices
            ? replayedFormalOccurrenceValidator.Validate(
                  argIdx, finalNewArg, standardArgSpans, tokenHunks)
            : OccurrenceReplay().MacroArgReplacementMatchesAllOccurrencesInB(
                  m, argIdx, baseArgText, finalNewArg, tokenHunks);

    if (!matchesAllOccurrences) {
      // The local observations were explainable, but the completed replacement
      // failed the global occurrence proof.  There is no recovery or diagnostic
      // side channel at this point: reject the args-only candidate fail-closed
      // rather than attempting to preserve unused hunk-effect trace strings.
      return std::nullopt;
    }

    if (isMacroInvocationVariadicFormal(m, argIdx)) {
      StringRef trimmedFinal(finalNewArg);
      trimmedFinal = trimmedFinal.trim();

      // The replay check above validates the edited expansion surface. For the
      // invocation spelling, however, a variadic formal does not own the fixed
      // separator before it. If deletion of the first tuple element left that
      // separator at the front of the reconstructed replacement, remove exactly
      // one leading comma so the callsite spells the shortened tuple rather
      // than an empty first variadic argument.
      if (!trimmedFinal.empty() && trimmedFinal.front() == ',') {
        trimmedFinal = trimmedFinal.drop_front();
        while (!trimmedFinal.empty() &&
               (trimmedFinal.front() == ' ' || trimmedFinal.front() == '\t'))
          trimmedFinal = trimmedFinal.drop_front();
        finalNewArg = trimmedFinal.str();
        finalMaterializedRange = std::nullopt;
      }
    }

    finalArgumentRewrites.Record(argIdx, std::move(finalNewArg),
                                  finalMaterializedRange, tupleForwarded);
  }

  return ArgsOnlyProofCertifier(deps_).BuildAcceptedCandidate(
      m, actualRecoveryCtx, finalArgumentRewrites);
}
} // namespace refold
} // namespace clang
