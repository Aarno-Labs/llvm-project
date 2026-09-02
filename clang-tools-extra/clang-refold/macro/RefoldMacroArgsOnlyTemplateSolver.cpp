//===--- RefoldMacroArgsOnlyTemplateSolver.cpp ------------------*- C++ -*-===//
//
// Recursive current-level template-surface recovery and template-solved
// args-only patch construction for clang-refold.
//
//===----------------------------------------------------------------------===//

#include "macro/RefoldMacroArgsOnlyTemplateSolver.h"

#include "macro/RefoldArgTextRecovery.h"
#include "macro/RefoldMacroPlannerHelpers.h"
#include "macro/RefoldMacroReplay.h"
#include "macro/RefoldMacroTopology.h"
#include "macro/RefoldMacroTupleHelpers.h"
#include "proof/RefoldProofLattice.h"
#include "source/RefoldSourceMapper.h"
#include "util/StringUtils.h"

#include "clang/Basic/SourceLocation.h"
#include "clang/Basic/TokenKinds.h"
#include "clang/Lex/Lexer.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/FormatVariadic.h"

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <limits>
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

// --- Free-function versions of planner methods, used by the template replay
// body. These do not depend on planner-private mutable state.

bool certifyMacroPatchWholeExpansionBRange(
    const RefoldSourceMapper &sourceMapper,
    const RefoldModel::MacroInvocation &m, MacroPatch &patch) {
  std::optional<std::pair<uint64_t, uint64_t>> cover =
      RefoldMacroWholeCoverProof::GetWholeCoverATokRange(m);
  if (!cover)
    return false;

  std::optional<std::pair<size_t, size_t>> bEnv =
      sourceMapper.MapATokRangeAToBTokenEnvelopePreserveBoundaryInsertions(
          cover->first, cover->second);
  if (!bEnv || bEnv->first >= bEnv->second)
    return false;

  patch.materialized.hasBTokenRange = true;
  patch.materialized.bTokStart = static_cast<uint64_t>(bEnv->first);
  patch.materialized.bTokEnd = static_cast<uint64_t>(bEnv->second);
  return true;
}

bool currentLevelInvocationIsInSubtreeOf(
    const RefoldMacroTopology &macroTopology,
    const RefoldModel::MacroInvocation &macro, uint64_t rootId) {
  uint64_t currentId = macro.id;
  SmallVector<uint64_t, 8> seen;
  while (true) {
    if (currentId == rootId)
      return true;
    if (std::find(seen.begin(), seen.end(), currentId) != seen.end())
      return false;
    seen.push_back(currentId);

    const RefoldModel::MacroInvocation *current =
        macroTopology.FindMacroInvocationById(currentId);
    if (!current || !current->callerMacroId)
      return false;
    currentId = *current->callerMacroId;
  }
}

bool currentLevelSubtreeContainsCounterInvocation(
    const RefoldMacroTopology &macroTopology, const RefoldModel *model,
    uint64_t rootId) {
  if (!macroTopology.FindMacroInvocationById(rootId))
    return false;
  for (const RefoldModel::MacroInvocation &macro :
       model->GetMacroInvocations()) {
    if (macro.name != "__COUNTER__")
      continue;
    if (currentLevelInvocationIsInSubtreeOf(macroTopology, macro, rootId))
      return true;
  }
  return false;
}

// This file uses `ArgsOnlyTemplateReplayContext` directly so it does not clash
// with the namespace-scope InvocationActualRecoveryContext shared by the
// planner and replay engines.

struct InvocationRewriteWithRangeLocal {
  std::string text;
  uint64_t materializedOutputByteStart = 0;
  uint64_t materializedOutputByteEnd = 0;
};

std::optional<InvocationRewriteWithRangeLocal>
buildInvocationRewriteWithRangeLocal(
    const ArgsOnlyTemplateReplayContext &ctx,
    const DenseMap<uint32_t, std::string> &replByArgIdx,
    const DenseMap<uint32_t, std::pair<uint64_t, uint64_t>>
        *materializedRangeByArgIdx = nullptr) {
  if (replByArgIdx.empty())
    return std::nullopt;

  const ArrayRef<std::pair<size_t, size_t>> invocationArgRanges =
      ctx.invocationArgRanges;
  StringRef baseInvocationText = ctx.baseInvocationText;

  SmallVector<uint32_t, 8> keys;
  keys.reserve(replByArgIdx.size());
  for (const auto &entry : replByArgIdx) {
    if (static_cast<size_t>(entry.first) >= invocationArgRanges.size())
      return std::nullopt;
    keys.push_back(entry.first);
  }

  llvm::sort(
      keys,
      [&invocationArgRanges](const uint32_t lhs, const uint32_t rhs) -> bool {
        const auto lhsRange = invocationArgRanges[lhs];
        const auto rhsRange = invocationArgRanges[rhs];
        if (lhsRange.first != rhsRange.first)
          return lhsRange.first < rhsRange.first;
        return lhs < rhs;
      });

  InvocationRewriteWithRangeLocal out;
  out.text.reserve(baseInvocationText.size());

  uint64_t cursor = 0;
  std::optional<uint64_t> mappedBegin;
  std::optional<uint64_t> mappedEnd;
  for (uint32_t argIdx : keys) {
    const auto rawRange = invocationArgRanges[argIdx];
    const size_t rBegin = rawRange.first;
    const size_t rEnd = rawRange.second;
    if (rEnd < rBegin || rEnd > baseInvocationText.size() || rBegin < cursor)
      return std::nullopt;

    out.text += baseInvocationText.slice(cursor, rBegin);

    auto replIt = replByArgIdx.find(argIdx);
    if (replIt == replByArgIdx.end())
      return std::nullopt;

    const uint64_t replBegin = static_cast<uint64_t>(out.text.size());
    out.text.append(replIt->second);
    const uint64_t replEnd = static_cast<uint64_t>(out.text.size());

    uint64_t materializedBegin = replBegin;
    uint64_t materializedEnd = replEnd;
    if (materializedRangeByArgIdx) {
      auto matIt = materializedRangeByArgIdx->find(argIdx);
      if (matIt != materializedRangeByArgIdx->end()) {
        const uint64_t relBegin = matIt->second.first;
        const uint64_t relEnd = matIt->second.second;
        if (relEnd < relBegin || relEnd > replIt->second.size())
          return std::nullopt;
        materializedBegin = replBegin + relBegin;
        materializedEnd = replBegin + relEnd;
      }
    }

    mappedBegin = mappedBegin ? std::min(*mappedBegin, materializedBegin)
                              : materializedBegin;
    mappedEnd =
        mappedEnd ? std::max(*mappedEnd, materializedEnd) : materializedEnd;

    cursor = rEnd;
  }

  out.text += baseInvocationText.substr(cursor);
  if (!mappedBegin || !mappedEnd)
    return std::nullopt;

  out.materializedOutputByteStart = *mappedBegin;
  out.materializedOutputByteEnd = *mappedEnd;
  return out;
}

/// Equality for producer argument-span records after canonical sorting.
bool samePPArgSpan(const RefoldModel::PPArgSpan &lhs,
                   const RefoldModel::PPArgSpan &rhs) {
  return lhs.begin == rhs.begin && lhs.end == rhs.end &&
         lhs.argIdx == rhs.argIdx && lhs.kind == rhs.kind &&
         lhs.byteBegin == rhs.byteBegin && lhs.byteEnd == rhs.byteEnd &&
         lhs.ppByteBegin == rhs.ppByteBegin && lhs.ppByteEnd == rhs.ppByteEnd;
}

/// Canonically order producer argument spans by all identity-bearing fields.
bool ppArgSpanLessByFullIdentity(const RefoldModel::PPArgSpan &lhs,
                                 const RefoldModel::PPArgSpan &rhs) {
  if (lhs.begin != rhs.begin)
    return lhs.begin < rhs.begin;
  if (lhs.end != rhs.end)
    return lhs.end < rhs.end;
  if (lhs.argIdx != rhs.argIdx)
    return lhs.argIdx < rhs.argIdx;
  if (lhs.kind != rhs.kind)
    return static_cast<unsigned>(lhs.kind) < static_cast<unsigned>(rhs.kind);
  if (lhs.byteBegin != rhs.byteBegin)
    return lhs.byteBegin < rhs.byteBegin;
  if (lhs.byteEnd != rhs.byteEnd)
    return lhs.byteEnd < rhs.byteEnd;
  if (lhs.ppByteBegin != rhs.ppByteBegin)
    return lhs.ppByteBegin < rhs.ppByteBegin;
  return lhs.ppByteEnd < rhs.ppByteEnd;
}

/// Deduplicate equivalent standard argument spans after deterministic sorting.
void canonicalizeStandardArgSpans(
    SmallVectorImpl<RefoldModel::PPArgSpan> &spans) {
  llvm::sort(spans, ppArgSpanLessByFullIdentity);
  spans.erase(std::unique(spans.begin(), spans.end(), samePPArgSpan),
              spans.end());
}

/// Order tuple argument references by caller spelling position.
bool tupleArgRefLessByCallerByteBegin(const RefoldModel::TupleArgRef &lhs,
                                      const RefoldModel::TupleArgRef &rhs) {
  return lhs.callerByteBegin < rhs.callerByteBegin;
}

/// Child invocation projection used when a current-level formal slot contains
/// a nested macro call that must be replayed through old/new expansion text.
struct CurrentLevelChildSlotRewrite {
  size_t relBegin = 0;
  size_t relEnd = 0;
  std::string oldExpansion;
  std::string newExpansion;
  std::string newSyntax;
};


/// Rebuilds current-level invocation spelling from proven formal-slot templates.
///
/// The resolver trusts the model's recorded invocation/source ranges and the
/// current-level template-surface helpers.  It owns the recursive child walk:
/// child invocations are visited in recorded model order, recursion is bounded
/// by the finite invocation count, and ambiguous or overlapping child rewrites
/// fail closed instead of choosing a source spelling heuristically.
class CurrentLevelInvocationSyntaxBuilder {
public:
  CurrentLevelInvocationSyntaxBuilder(
      const RefoldMacroArgsOnlyTemplateSolver &solver,
      const RefoldMacroArgsOnlyTemplateSolver::Dependencies &deps)
      : solver_(solver), deps_(deps) {}

  /// Rebuilds `inv` as source syntax when every changed formal is provable.
  /// `completeEnvelopeReplayValidated` reports whether the complete-template
  /// resolver, rather than the legacy projection fallback, supplied the
  /// formal expansions used to construct the spelling.
  std::optional<std::string>
  Build(const RefoldModel::MacroInvocation &inv,
        bool &completeEnvelopeReplayValidated) const {
    completeEnvelopeReplayValidated = false;
    return Build(inv, 0, &completeEnvelopeReplayValidated);
  }

private:
  /// Recursively rebuilds `inv`, preserving child traversal and depth bounds.
  std::optional<std::string>
  Build(const RefoldModel::MacroInvocation &inv, unsigned depth,
        bool *completeEnvelopeReplayValidated = nullptr) const {
    // The recursion follows recorded child invocation edges.  A path deeper
    // than the number of recorded invocations implies a cycle or stale
    // metadata, so use that structural bound instead of a fixed depth cap.
    if (depth > (*deps_.model).GetMacroInvocations().size() || !inv.invText ||
        !inv.invB || !inv.invE || !inv.stringifySpans.empty() ||
        !inv.pasteSpans.empty())
      return std::nullopt;

    auto formalRangesOpt =
        RefoldMacroActualLayout({deps_.lexLang})
            .GetMacroInvocationFormalArgContentRanges(inv, *inv.invText);
    if (!formalRangesOpt)
      return std::nullopt;
    const auto &formalRanges = *formalRangesOpt;

    // Prefer the complete fixed-body template proof introduced for boundary
    // insertion recovery.  It is the only authority allowed to widen a
    // producer argument span beyond its ordinary byte projection.
    if (auto replay =
            solver_.ResolveCurrentLevelStandardArgReplay(inv, formalRanges)) {
      if (auto rewritten = BuildFromReplay(
              inv, formalRanges, replay->surface.standardSpans,
              replay->bExpansionByFormal, depth)) {
        if (completeEnvelopeReplayValidated)
          *completeEnvelopeReplayValidated = true;
        return rewritten;
      }
    }

    // Preserve the established projection-backed current-level replay as a
    // fallback.  Some nested-call surfaces are not uniquely segmentable from
    // fixed replacement-list tokens alone: repeated body punctuation can admit
    // several complete B partitions even though child invocation provenance
    // reconstructs one exact source spelling.  The fallback does not widen any
    // span.  It uses the producer byte projection exactly as before and is
    // admitted only after `BuildFromReplay` proves the old/new child expansion
    // bridge and validates that the rebuilt text changes producer callsite
    // syntax solely inside formal slots.
    auto standardSpansOpt =
        solver_.GetCurrentLevelStandardArgSpansForInvocation(inv, formalRanges);
    if (!standardSpansOpt ||
        standardSpansOpt->size() != formalRanges.size())
      return std::nullopt;

    std::vector<std::string> projectedExpansionByFormal(formalRanges.size());
    for (size_t i = 0; i < standardSpansOpt->size(); ++i) {
      // Preserve boundary insertions: this projects a *nested* invocation's
      // own argument, whose neighbouring A material is the enclosing macro's
      // body text.  Trimming there drops an edit appended at the argument's
      // trailing edge, and the reconstructed callsite is emitted with the token
      // missing rather than being refused.
      auto bEnvelope = (*deps_.sourceMapper)
                           .MapAToBTokenEnvelopeByPPArgSpan(
                               (*standardSpansOpt)[i],
                               /*preserveBoundaryInsertions=*/true);
      if (!bEnvelope || bEnvelope->first >= bEnvelope->second)
        return std::nullopt;
      std::string expansion =
          (*deps_.sourceMapper)
              .SliceBSource(bEnvelope->first, bEnvelope->second)
              .trim()
              .str();
      if (expansion.empty())
        return std::nullopt;
      projectedExpansionByFormal[i] = std::move(expansion);
    }

    return BuildFromReplay(inv, formalRanges, *standardSpansOpt,
                           projectedExpansionByFormal, depth);
  }

  /// Rebuild one invocation from a proven per-formal B expansion assignment.
  ///
  /// This routine owns the source-syntax inversion shared by the complete
  /// template and projection-backed authorities.  Direct textual inversions,
  /// unique substring replacement, and recursive child-expansion bridges are
  /// accepted exactly as before; the final producer-slot transport check makes
  /// the resulting source spelling a complete function-like invocation rather
  /// than merely a token partition of its expansion.
  std::optional<std::string> BuildFromReplay(
      const RefoldModel::MacroInvocation &inv,
      ArrayRef<std::pair<size_t, size_t>> formalRanges,
      ArrayRef<RefoldModel::PPArgSpan> standardSpans,
      ArrayRef<std::string> bExpansionByFormal, unsigned depth) const {
    if (standardSpans.size() != formalRanges.size() ||
        bExpansionByFormal.size() != formalRanges.size())
      return std::nullopt;

    // Accumulate replacements by parsed formal slot.  Each replacement is
    // derived from one standard span's old expansion and its proven B-side
    // expansion, then later applied directly to the invocation spelling.
    DenseMap<uint32_t, std::string> replByFormal;
    for (size_t i = 0; i < standardSpans.size(); ++i) {
      const RefoldModel::PPArgSpan &sp = standardSpans[i];
      if (sp.argIdx != i || i >= formalRanges.size())
        return std::nullopt;

      const auto &argRange = formalRanges[i];
      if (argRange.second < argRange.first ||
          argRange.second > inv.invText->size())
        return std::nullopt;

      StringRef rawArg =
          StringRef(*inv.invText).slice(argRange.first, argRange.second);
      size_t trimLead = 0;
      size_t trimEnd = rawArg.size();
      std::tie(trimLead, trimEnd) =
          stringutils::trimWsRange(rawArg, 0, rawArg.size());
      StringRef baseTrim = rawArg.slice(trimLead, trimEnd);

      StringRef oldExpansion =
          (*deps_.sourceMapper).SliceASource(sp.begin, sp.end).trim();
      StringRef newExpansion = bExpansionByFormal[i];
      if (oldExpansion.empty() || newExpansion.empty())
        return std::nullopt;

      std::optional<std::string> replacement;
      if (oldExpansion == baseTrim) {
        replacement = newExpansion.str();
      } else if (auto loc = findUniqueTrimmedSubstring(baseTrim, oldExpansion)) {
        replacement = stringutils::replaceRange(baseTrim.str(), loc->first,
                                                loc->second, newExpansion);
      } else {
        // The formal spelling does not directly contain its old expansion; it
        // may contain a nested macro invocation whose expansion accounts for
        // that text.  Preserve such a child only if replacing the child
        // spelling with its proven old expansion reconstructs `oldExpansion`,
        // and replacing it with its proven new expansion reconstructs
        // `newExpansion`.
        if (!inv.invFile)
          return std::nullopt;

        SmallVector<CurrentLevelChildSlotRewrite, 4> childSlots;

        const uint64_t absTrimBegin = *inv.invB + argRange.first + trimLead;
        const uint64_t absTrimEnd = *inv.invB + argRange.first + trimEnd;

        // Search lexical child invocations contained in this formal slot.
        // `callerMacroId` is honored when present, but absence of that edge is
        // not enough to accept a child; the file/range containment and the
        // expansion-bridge proof below still have to succeed.
        for (const auto &child : (*deps_.model).GetMacroInvocations()) {
          if (child.id == inv.id || !child.invFile || !child.invB ||
              !child.invE || !child.invText)
            continue;
          if (*child.invFile != *inv.invFile)
            continue;
          if (child.callerMacroId && *child.callerMacroId != inv.id)
            continue;
          if (*child.invB < absTrimBegin || *child.invE > absTrimEnd ||
              *child.invE <= *child.invB)
            continue;

          auto childExpansion =
              solver_.GetCurrentLevelExpansionTextForInvocation(child);
          if (!childExpansion)
            continue;
          auto childNewSyntax = Build(child, depth + 1);
          if (!childNewSyntax)
            continue;

          const uint64_t relB64 = *child.invB - absTrimBegin;
          const uint64_t relE64 = *child.invE - absTrimBegin;
          if (relE64 < relB64 || relE64 > baseTrim.size())
            continue;

          childSlots.push_back(CurrentLevelChildSlotRewrite{
              static_cast<size_t>(relB64), static_cast<size_t>(relE64),
              StringRef(childExpansion->first).trim().str(),
              StringRef(childExpansion->second).trim().str(),
              StringRef(*childNewSyntax).trim().str()});
        }

        if (childSlots.empty())
          return std::nullopt;

        // Apply child replacements from right to left so byte offsets remain
        // relative to the original formal spelling.  Overlap is rejected by
        // the monotonic `previousBegin` check in the loop.
        llvm::sort(childSlots, [](const CurrentLevelChildSlotRewrite &lhs,
                                  const CurrentLevelChildSlotRewrite &rhs) {
          if (lhs.relBegin != rhs.relBegin)
            return lhs.relBegin > rhs.relBegin;
          return lhs.relEnd > rhs.relEnd;
        });

        // Maintain three parallel projections of the same formal spelling:
        //   * oldExpanded: child syntax replaced by old local expansions;
        //   * newExpanded: child syntax replaced by new local expansions;
        //   * syntaxExpanded: child syntax replaced by updated child calls.
        // The first two must exactly equal the parent formal's old/new
        // expansion slices before the third may be used as source output.
        std::string oldExpanded = baseTrim.str();
        std::string newExpanded = baseTrim.str();
        std::string syntaxExpanded = baseTrim.str();
        size_t previousBegin = std::numeric_limits<size_t>::max();
        for (const CurrentLevelChildSlotRewrite &slot : childSlots) {
          if (slot.relEnd < slot.relBegin || slot.relEnd > baseTrim.size())
            return std::nullopt;
          if (previousBegin != std::numeric_limits<size_t>::max() &&
              slot.relEnd > previousBegin)
            return std::nullopt;
          previousBegin = slot.relBegin;

          oldExpanded = stringutils::replaceRange(
              oldExpanded, slot.relBegin, slot.relEnd, slot.oldExpansion);
          newExpanded = stringutils::replaceRange(
              newExpanded, slot.relBegin, slot.relEnd, slot.newExpansion);
          syntaxExpanded = stringutils::replaceRange(
              syntaxExpanded, slot.relBegin, slot.relEnd, slot.newSyntax);
        }

        if (StringRef(oldExpanded).trim() != oldExpansion ||
            StringRef(newExpanded).trim() != newExpansion)
          return std::nullopt;
        replacement = StringRef(syntaxExpanded).trim().str();
      }

      if (!replacement || StringRef(*replacement).trim().empty())
        return std::nullopt;

      // Do not emit a replacement that would change the current invocation's
      // arity.  Macro argument collection protects commas only with nested
      // parentheses; brackets and braces deliberately do not suppress this
      // check for non-variadic formals.
      const bool allowComma =
          i < inv.defParams.size() && inv.defParams[i].variadic;
      if (!allowComma && refoldMacroActualHasTopLevelComma(
                             StringRef(*replacement), (*deps_.lexLang)))
        return std::nullopt;

      if (StringRef(*replacement).trim() != baseTrim)
        replByFormal[static_cast<uint32_t>(i)] =
            StringRef(*replacement).trim().str();
    }

    if (replByFormal.empty())
      return std::nullopt;

    // Apply formal-slot edits to the invocation spelling right-to-left, again
    // preserving original byte offsets and avoiding dependence on map order.
    struct LocalEdit {
      size_t begin = 0;
      size_t end = 0;
      std::string repl;
    };
    SmallVector<LocalEdit, 8> edits;
    for (const auto &entry : replByFormal) {
      const uint32_t argIdx = entry.first;
      if (argIdx >= formalRanges.size())
        return std::nullopt;
      const auto &range = formalRanges[argIdx];
      edits.push_back(LocalEdit{range.first, range.second, entry.second});
    }
    llvm::sort(edits, [](const LocalEdit &lhs, const LocalEdit &rhs) {
      return lhs.begin > rhs.begin;
    });

    std::string rewritten = inv.invText->str();
    for (const LocalEdit &edit : edits) {
      if (edit.end < edit.begin || edit.end > rewritten.size())
        return std::nullopt;
      rewritten =
          stringutils::replaceRange(rewritten, edit.begin, edit.end, edit.repl);
    }

    // A complete expansion-template partition is not, by itself, proof that
    // the recovered variable interval is a valid source-level macro actual.
    // Runs of identical replacement-list punctuation can otherwise let a
    // fixed token match the wrong B-side occurrence and leave the inferred
    // formal text containing an unmatched delimiter.  Reparse the rebuilt
    // callsite through the producer-aware actual-layout service.  For rewritten
    // text, that service accepts only when the producer's fixed callsite text
    // plus the newly parsed formal slots reconstructs the entire spelling
    // exactly.  Therefore no prefix/suffix text may escape the invocation and
    // no token outside a formal slot may be reassigned to an argument.
    auto rewrittenFormalRanges =
        RefoldMacroActualLayout({deps_.lexLang})
            .GetMacroInvocationFormalArgContentRanges(inv, rewritten);
    if (!rewrittenFormalRanges ||
        rewrittenFormalRanges->size() != formalRanges.size())
      return std::nullopt;

    return StringRef(rewritten).trim().str();
  }

  const RefoldMacroArgsOnlyTemplateSolver &solver_;
  const RefoldMacroArgsOnlyTemplateSolver::Dependencies &deps_;
};

/// Enumerates bounded B-token assignments for args-only template occurrences.
///
/// The caller supplies an exact A-side template partition and the whole B-side
/// token envelope.  This resolver owns only the mutable DFS state: fixed body
/// spans must match literally, argument occurrences enumerate B-token intervals
/// in increasing end-position order, and enumeration stops after the existing
/// ambiguity cap so downstream candidate construction stays fail-closed.
class TemplateAssignmentEnumerator {
public:
  using Assignment = std::vector<std::pair<size_t, size_t>>;

  TemplateAssignmentEnumerator(
      ArrayRef<ArgsOnlyTemplateElem> elems, std::pair<size_t, size_t> bEnv,
      const RefoldMacroArgsOnlyTemplateSolver::Dependencies &deps)
      : elems_(elems), bEnv_(bEnv), deps_(deps) {}

  /// Enumerates all assignments up to the existing ambiguity cutoff.
  std::vector<Assignment> Enumerate(size_t occurrenceCount) {
    const std::pair<size_t, size_t> unset = {
        std::numeric_limits<size_t>::max(),
        std::numeric_limits<size_t>::max()};
    curAssign_.assign(occurrenceCount, unset);
    solutions_.clear();
    Dfs(0, bEnv_.first);
    return solutions_;
  }

private:
  /// Returns whether fixed A-side body tokens match at `bPos`.
  bool BodyMatchesAt(const ArgsOnlyTemplateElem &elem, size_t bPos) const {
    const size_t len = static_cast<size_t>(elem.aEnd - elem.aBegin);
    if (bPos + len > bEnv_.second)
      return false;
    for (size_t i = 0; i < len; ++i) {
      if (deps_.aToks[static_cast<size_t>(elem.aBegin) + i].spelling !=
          deps_.bToks[bPos + i].spelling)
        return false;
    }
    return true;
  }

  /// DFSes template elements while preserving B-side interval order.
  void Dfs(size_t elemIdx, size_t bPos) {
    if (solutions_.size() > 16)
      return;
    if (elemIdx == elems_.size()) {
      if (bPos == bEnv_.second)
        solutions_.push_back(curAssign_);
      return;
    }

    const ArgsOnlyTemplateElem &elem = elems_[elemIdx];
    if (!elem.isArg) {
      const size_t len = static_cast<size_t>(elem.aEnd - elem.aBegin);
      if (BodyMatchesAt(elem, bPos))
        Dfs(elemIdx + 1, bPos + len);
      return;
    }

    // Argument occurrences enumerate non-empty B-token intervals in increasing
    // end order.  Reset the slot after recursion so sibling branches cannot
    // inherit speculative assignments.
    for (size_t end = bPos + 1; end <= bEnv_.second; ++end) {
      curAssign_[elem.occurrenceOrdinal] = {bPos, end};
      Dfs(elemIdx + 1, end);
      curAssign_[elem.occurrenceOrdinal] = {
          std::numeric_limits<size_t>::max(),
          std::numeric_limits<size_t>::max()};
      if (solutions_.size() > 16)
        return;
    }
  }

  ArrayRef<ArgsOnlyTemplateElem> elems_;
  std::pair<size_t, size_t> bEnv_;
  const RefoldMacroArgsOnlyTemplateSolver::Dependencies &deps_;
  Assignment curAssign_;
  std::vector<Assignment> solutions_;
};

} // namespace

std::optional<std::vector<RefoldModel::PPArgSpan>>
RefoldMacroArgsOnlyTemplateSolver::GetCurrentLevelStandardArgSpans(
    const ArgsOnlyTemplateReplayContext &ctx) const {
  const RefoldModel::MacroInvocation &invocation = ctx.invocation;
  auto coverOpt =
      RefoldMacroWholeCoverProof::GetWholeCoverATokRange(invocation);
  if (ctx.invocationArgRanges.empty() || !coverOpt ||
      coverOpt->first >= coverOpt->second)
    return std::nullopt;

  auto surface = GetCurrentLevelTemplateSurfaceForInvocation(
      invocation, ctx.invocationArgRanges);
  if (!surface || surface->coverBegin != coverOpt->first ||
      surface->coverEnd != coverOpt->second)
    return std::nullopt;

  return std::move(surface->standardSpans);
}

std::optional<CurrentLevelTemplateSurface>
RefoldMacroArgsOnlyTemplateSolver::GetCurrentLevelTemplateSurfaceForInvocation(
    const RefoldModel::MacroInvocation &invocation,
    ArrayRef<std::pair<size_t, size_t>> formalRanges) const {
  if (formalRanges.empty())
    return std::nullopt;

  SmallVector<RefoldModel::PPArgSpan, 16> standard;
  for (const auto &as : invocation.argSpans) {
    if (as.kind == PPArgSpanKind::Standard && as.begin < as.end)
      standard.push_back(as);
  }
  if (standard.empty())
    return std::nullopt;
  canonicalizeStandardArgSpans(standard);

  // Drop spans contained in another standard span so nested child-argument
  // evidence cannot be mistaken for a current invocation formal.
  SmallVector<RefoldModel::PPArgSpan, 16> maximal;
  for (const auto &cand : standard) {
    bool contained = false;
    for (const auto &other : standard) {
      if (&cand == &other)
        continue;
      if (other.begin <= cand.begin && cand.end <= other.end &&
          (other.begin < cand.begin || cand.end < other.end)) {
        contained = true;
        break;
      }
    }
    if (!contained)
      maximal.push_back(cand);
  }

  if (maximal.size() != formalRanges.size())
    return std::nullopt;

  llvm::sort(maximal, ppArgSpanLessByTokenRangeAndArg);

  struct CoverElem {
    uint64_t begin = 0;
    uint64_t end = 0;
  };
  SmallVector<CoverElem, 32> coverElems;
  for (const auto &bs : invocation.bodySpans) {
    if (bs.begin < bs.end)
      coverElems.push_back({bs.begin, bs.end});
  }
  for (const auto &as : maximal)
    coverElems.push_back({as.begin, as.end});
  if (coverElems.empty())
    return std::nullopt;

  llvm::sort(coverElems, [](const CoverElem &lhs, const CoverElem &rhs) {
    if (lhs.begin != rhs.begin)
      return lhs.begin < rhs.begin;
    return lhs.end < rhs.end;
  });

  const uint64_t coverBegin = coverElems.front().begin;
  const uint64_t coverEnd = coverElems.back().end;
  if (coverBegin >= coverEnd)
    return std::nullopt;

  uint64_t cursor = coverBegin;
  for (const CoverElem &elem : coverElems) {
    if (elem.begin != cursor || elem.end < elem.begin || elem.end > coverEnd)
      return std::nullopt;
    cursor = elem.end;
  }
  if (cursor != coverEnd)
    return std::nullopt;

  CurrentLevelTemplateSurface surface;
  surface.coverBegin = coverBegin;
  surface.coverEnd = coverEnd;
  surface.standardSpans.reserve(maximal.size());
  for (size_t i = 0; i < maximal.size(); ++i) {
    RefoldModel::PPArgSpan span = maximal[i];
    span.argIdx = static_cast<uint32_t>(i);
    surface.standardSpans.push_back(span);
  }
  return surface;
}

std::optional<std::vector<RefoldModel::PPArgSpan>>
RefoldMacroArgsOnlyTemplateSolver::GetCurrentLevelStandardArgSpansForInvocation(
    const RefoldModel::MacroInvocation &invocation,
    ArrayRef<std::pair<size_t, size_t>> formalRanges) const {
  auto surface =
      GetCurrentLevelTemplateSurfaceForInvocation(invocation, formalRanges);
  if (!surface)
    return std::nullopt;
  return std::move(surface->standardSpans);
}

std::optional<CurrentLevelStandardArgReplay>
RefoldMacroArgsOnlyTemplateSolver::ResolveCurrentLevelStandardArgReplay(
    const RefoldModel::MacroInvocation &invocation,
    ArrayRef<std::pair<size_t, size_t>> formalRanges) const {
  auto surface =
      GetCurrentLevelTemplateSurfaceForInvocation(invocation, formalRanges);
  if (!surface || surface->coverBegin >= surface->coverEnd ||
      surface->standardSpans.size() != formalRanges.size())
    return std::nullopt;

  // Build the exact current-level template.  Body spans are fixed terminals;
  // the maximal standard spans are the only variable B intervals.
  SmallVector<ArgsOnlyTemplateElem, 32> elems;
  for (const auto &bodySpan : invocation.bodySpans) {
    if (bodySpan.begin < bodySpan.end)
      elems.push_back({false, bodySpan.begin, bodySpan.end, 0, 0});
  }

  size_t occurrenceCount = 0;
  for (const RefoldModel::PPArgSpan &span : surface->standardSpans) {
    if (span.kind != PPArgSpanKind::Standard || span.begin >= span.end ||
        static_cast<size_t>(span.argIdx) >= formalRanges.size())
      return std::nullopt;
    elems.push_back(
        {true, span.begin, span.end, span.argIdx, occurrenceCount++});
  }

  if (occurrenceCount != formalRanges.size() || elems.empty())
    return std::nullopt;

  llvm::sort(elems,
             [](const ArgsOnlyTemplateElem &lhs,
                const ArgsOnlyTemplateElem &rhs) {
               if (lhs.aBegin != rhs.aBegin)
                 return lhs.aBegin < rhs.aBegin;
               if (lhs.aEnd != rhs.aEnd)
                 return lhs.aEnd < rhs.aEnd;
               return lhs.isArg < rhs.isArg;
             });

  // A missing, overlapping, or duplicated surface would leave some expansion
  // token unexplained.  Reject rather than recovering a formal from an
  // approximate byte boundary.
  uint64_t aCursor = surface->coverBegin;
  for (const ArgsOnlyTemplateElem &elem : elems) {
    if (elem.aBegin != aCursor || elem.aEnd <= elem.aBegin ||
        elem.aEnd > surface->coverEnd)
      return std::nullopt;
    aCursor = elem.aEnd;
  }
  if (aCursor != surface->coverEnd)
    return std::nullopt;

  auto bEnvelope =
      (*deps_.sourceMapper)
          .MapATokRangeAToBTokenEnvelopePreserveBoundaryInsertions(
              surface->coverBegin, surface->coverEnd);
  if (!bEnvelope || bEnvelope->first >= bEnvelope->second ||
      bEnvelope->second > deps_.bToks.size())
    return std::nullopt;

  // Preserve the existing producer-byte projection whenever it already
  // constitutes an exact replay of the complete current-level template.  This
  // is not a fallback to approximate slicing: the whole-envelope validator
  // requires every projected body/formal element to be contiguous, requires
  // every fixed body token to match literally, and requires the partition to
  // consume the complete B envelope.  The failing bracket cases do not pass
  // this test because their trimmed standard span leaves an unexplained B
  // token between the formal and the following fixed body span.
  if (ArgsOnlyTemplateReplayPreservesEnvelope(invocation, *surface)) {
    std::vector<std::string> projectedExpansionByFormal(formalRanges.size());
    for (size_t i = 0; i < surface->standardSpans.size(); ++i) {
      auto projected = (*deps_.sourceMapper)
                           .MapAToBTokenEnvelopeByPPArgSpan(
                               surface->standardSpans[i]);
      if (!projected || projected->first >= projected->second ||
          projected->second > bEnvelope->second)
        return std::nullopt;
      std::string expansion =
          (*deps_.sourceMapper)
              .SliceBSource(projected->first, projected->second)
              .trim()
              .str();
      if (expansion.empty())
        return std::nullopt;
      projectedExpansionByFormal[i] = std::move(expansion);
    }

    CurrentLevelStandardArgReplay replay;
    replay.surface = std::move(*surface);
    replay.bExpansionByFormal = std::move(projectedExpansionByFormal);
    replay.bEnvelope = *bEnvelope;
    return replay;
  }

  // If producer-byte projection does not tile the complete envelope, solve the
  // exact fixed-body template instead.  Match the bounds already used by the
  // complete template solver so current-level recovery remains deterministic
  // and pathological covers cannot trigger an unbounded segmentation search.
  if (elems.size() > 64 ||
      (bEnvelope->second - bEnvelope->first) > 256 ||
      occurrenceCount > 32)
    return std::nullopt;

  TemplateAssignmentEnumerator assignmentEnumerator(
      ArrayRef<ArgsOnlyTemplateElem>(elems.data(), elems.size()), *bEnvelope,
      deps_);
  std::vector<TemplateAssignmentEnumerator::Assignment> solutions =
      assignmentEnumerator.Enumerate(occurrenceCount);
  if (solutions.empty() || solutions.size() > 16)
    return std::nullopt;

  std::optional<std::vector<std::string>> uniqueExpansionByFormal;
  for (const TemplateAssignmentEnumerator::Assignment &solution : solutions) {
    if (solution.size() != occurrenceCount)
      return std::nullopt;

    std::vector<std::string> expansionByFormal(formalRanges.size());
    for (const ArgsOnlyTemplateElem &elem : elems) {
      if (!elem.isArg)
        continue;
      if (elem.occurrenceOrdinal >= solution.size() ||
          static_cast<size_t>(elem.argIdx) >= expansionByFormal.size())
        return std::nullopt;

      const auto &range = solution[elem.occurrenceOrdinal];
      if (range.first >= range.second || range.second > bEnvelope->second)
        return std::nullopt;
      std::string expansion =
          (*deps_.sourceMapper)
              .SliceBSource(range.first, range.second)
              .trim()
              .str();
      if (expansion.empty())
        return std::nullopt;
      expansionByFormal[elem.argIdx] = std::move(expansion);
    }

    for (const std::string &expansion : expansionByFormal) {
      if (expansion.empty())
        return std::nullopt;
    }

    if (!uniqueExpansionByFormal) {
      uniqueExpansionByFormal = std::move(expansionByFormal);
      continue;
    }
    if (*uniqueExpansionByFormal != expansionByFormal) {
      REFOLD_LOG_TRACE(
          "macro/template",
          "reject current-level standard replay: inv id={0} name={1} "
          "complete fixed-body template has divergent formal assignments",
          invocation.id, invocation.name);
      return std::nullopt;
    }
  }

  if (!uniqueExpansionByFormal)
    return std::nullopt;

  CurrentLevelStandardArgReplay replay;
  replay.surface = std::move(*surface);
  replay.bExpansionByFormal = std::move(*uniqueExpansionByFormal);
  replay.bEnvelope = *bEnvelope;
  return replay;
}

std::optional<std::pair<std::string, std::string>>
RefoldMacroArgsOnlyTemplateSolver::GetCurrentLevelExpansionTextForInvocation(
    const RefoldModel::MacroInvocation &invocation) const {
  if (!invocation.invText)
    return std::nullopt;
  auto formalRangesOpt = RefoldMacroActualLayout({deps_.lexLang})
                             .GetMacroInvocationFormalArgContentRanges(
                                 invocation, *invocation.invText);
  if (!formalRangesOpt)
    return std::nullopt;

  auto surface =
      GetCurrentLevelTemplateSurfaceForInvocation(invocation, *formalRangesOpt);
  if (!surface || surface->coverBegin >= surface->coverEnd)
    return std::nullopt;

  auto bEnv = (*deps_.sourceMapper)
                  .MapATokRangeAToBTokenEnvelopePreserveBoundaryInsertions(
                      surface->coverBegin, surface->coverEnd);
  if (!bEnv || bEnv->first >= bEnv->second)
    return std::nullopt;

  return std::make_pair(
      (*deps_.sourceMapper)
          .SliceASource(surface->coverBegin, surface->coverEnd)
          .trim()
          .str(),
      (*deps_.sourceMapper)
          .SliceBSource(bEnv->first, bEnv->second)
          .trim()
          .str());
}

bool RefoldMacroArgsOnlyTemplateSolver::ArgsOnlyTemplateReplayPreservesEnvelope(
    const RefoldModel::MacroInvocation &invocation,
    const CurrentLevelTemplateSurface &surface) const {
  std::optional<std::pair<size_t, size_t>> wholeBEnv =
      (*deps_.sourceMapper)
          .MapATokRangeAToBTokenEnvelopePreserveBoundaryInsertions(
              surface.coverBegin, surface.coverEnd);
  if (!wholeBEnv || wholeBEnv->first >= wholeBEnv->second ||
      wholeBEnv->second > deps_.bToks.size())
    return false;

  struct ReplayElem {
    bool isArg = false;
    uint64_t begin = 0;
    uint64_t end = 0;
    const RefoldModel::PPArgSpan *argSpan = nullptr;
  };

  SmallVector<ReplayElem, 32> elems;
  for (const auto &bs : invocation.bodySpans) {
    if (bs.begin < bs.end) {
      ReplayElem elem;
      elem.isArg = false;
      elem.begin = bs.begin;
      elem.end = bs.end;
      elems.push_back(elem);
    }
  }
  for (const RefoldModel::PPArgSpan &sp : surface.standardSpans) {
    if (sp.begin < sp.end) {
      ReplayElem elem;
      elem.isArg = true;
      elem.begin = sp.begin;
      elem.end = sp.end;
      elem.argSpan = &sp;
      elems.push_back(elem);
    }
  }

  llvm::sort(elems, [](const ReplayElem &lhs, const ReplayElem &rhs) {
    if (lhs.begin != rhs.begin)
      return lhs.begin < rhs.begin;
    if (lhs.end != rhs.end)
      return lhs.end < rhs.end;
    return lhs.isArg < rhs.isArg;
  });

  size_t bCursor = wholeBEnv->first;
  for (const ReplayElem &elem : elems) {
    std::optional<std::pair<size_t, size_t>> elemBEnv;
    if (elem.isArg) {
      if (!elem.argSpan)
        return false;
      elemBEnv =
          (*deps_.sourceMapper).MapAToBTokenEnvelopeByPPArgSpan(*elem.argSpan);
    } else {
      elemBEnv = (*deps_.sourceMapper)
                     .MapATokRangeAToBTokenEnvelopePreserveBoundaryInsertions(
                         elem.begin, elem.end);
    }
    if (!elemBEnv || elemBEnv->first != bCursor ||
        elemBEnv->second < elemBEnv->first ||
        elemBEnv->second > wholeBEnv->second)
      return false;

    if (!elem.isArg) {
      const uint64_t aLen = elem.end - elem.begin;
      if (elemBEnv->second - elemBEnv->first != aLen)
        return false;
      for (uint64_t i = 0; i < aLen; ++i) {
        const size_t ai = static_cast<size_t>(elem.begin + i);
        const size_t bi = elemBEnv->first + static_cast<size_t>(i);
        if (ai >= deps_.aToks.size() || bi >= deps_.bToks.size() ||
            deps_.aToks[ai].spelling != deps_.bToks[bi].spelling)
          return false;
      }
    }

    bCursor = elemBEnv->second;
  }

  return bCursor == wholeBEnv->second;
}

std::optional<MacroPatch>
RefoldMacroArgsOnlyTemplateSolver::TryTemplateSolvedArgsOnlyPatch(
    const ArgsOnlyTemplateReplayContext &ctx) const {
  const ArgsOnlyTemplateReplayContext actualRecoveryCtx{
      ctx.invocation, ctx.baseInvocationText, ctx.invocationArgRanges};
  const RefoldModel::MacroInvocation &templateInvocation = ctx.invocation;
  StringRef templateBaseInvocationText = ctx.baseInvocationText;
  ArrayRef<std::pair<size_t, size_t>> templateInvocationArgRanges =
      ctx.invocationArgRanges;

  // Treat the whole macro expansion as a deterministic template made of fixed
  // body tokens and argument-occurrence variables. This proves split edits that
  // the LCS exposes as separate islands around macro-body punctuation, e.g.
  // repeated formals and tuple forwarding wrappers.
  if (!templateInvocation.stringifySpans.empty() ||
      !templateInvocation.pasteSpans.empty())
    return std::nullopt;

  auto cover =
      RefoldMacroWholeCoverProof::GetWholeCoverATokRange(templateInvocation);
  if (!cover) {
    return std::nullopt;
  }

  CurrentLevelInvocationSyntaxBuilder currentLevelSyntaxBuilder(*this, deps_);

  // Prefer the current-level template proof when it can rewrite the whole
  // invocation. It captures brace/bracket comma-split cases before a narrower
  // hunk-local proof can accept a partial patch.
  bool completeEnvelopeReplayValidated = false;
  if (auto currentLevelRewrite = currentLevelSyntaxBuilder.Build(
          templateInvocation, completeEnvelopeReplayValidated)) {
    if (StringRef(*currentLevelRewrite).trim() !=
        templateBaseInvocationText.trim()) {
      // Preserve the authority used to construct the spelling.  Re-running the
      // boundary-preserving byte projection here is both redundant and weaker:
      // a token replacement at a formal boundary can be absorbed into the
      // adjacent body envelope even after the complete token-template solver
      // has uniquely assigned that token to the formal slot.
      const bool counterWholeEnvelopeReplayValidated =
          completeEnvelopeReplayValidated &&
          currentLevelSubtreeContainsCounterInvocation(
              *deps_.macroTopology, deps_.model, templateInvocation.id);

      MacroPatch patch{*templateInvocation.invB, *templateInvocation.invE,
                       std::move(*currentLevelRewrite), templateInvocation.id};
      patch.materialized.outputByteStart = 0;
      patch.materialized.outputByteEnd = patch.replacement.size();
      patch.materialized.hasOutputByteRange = true;
      certifyMacroPatchWholeExpansionBRange(*deps_.sourceMapper,
                                            templateInvocation, patch);
      // Inlined AttachArgsOnlyProofCarrier (proof attached directly via
      // lattice).
      {
        MacroPatchProof proof =
            (*deps_.proofLattice)
                .MakeMacroPatchProof(MacroPatchProofKind::ArgsOnlyStandard,
                                     /*preservesInvocationStructure=*/true,
                                     ctx.invocation.id);
        if (counterWholeEnvelopeReplayValidated) {
          WholeEnvelopeReplayWitness witness;
          witness.rootMacroId = ctx.invocation.id;
          witness.replayValidated = true;
          witness.definitionTapeReplayValidated = false;
          proof.wholeEnvelopeReplay = witness;
        }
        (*deps_.proofLattice).SetMacroPatchProof(patch, std::move(proof));
      }
      return patch;
    }
  }

  // Build a linear template over the macro's A-side whole cover. Body spans
  // are fixed terminals; standard argument spans are variables whose B-side
  // slices must be solved consistently across all occurrences.
  SmallVector<ArgsOnlyTemplateElem, 32> elems;
  for (const auto &bs : templateInvocation.bodySpans) {
    if (bs.begin < bs.end)
      elems.push_back({false, bs.begin, bs.end, 0, 0});
  }

  // Reuse the current-level formal repair in the older template path too.
  // If exact tiling cannot be proven, leave the producer arg-span indexing
  // untouched and let the existing checks fail closed as before.
  std::vector<RefoldModel::PPArgSpan> templateArgSpans;
  if (auto currentLevelSpans = GetCurrentLevelStandardArgSpans(ctx))
    templateArgSpans = std::move(*currentLevelSpans);
  else
    templateArgSpans = templateInvocation.argSpans;

  size_t occurrenceCount = 0;
  for (const auto &as : templateArgSpans) {
    if (as.kind != PPArgSpanKind::Standard || as.begin >= as.end)
      continue;
    if (static_cast<size_t>(as.argIdx) >= templateInvocationArgRanges.size())
      return std::nullopt;
    elems.push_back({true, as.begin, as.end, as.argIdx, occurrenceCount++});
  }

  if (occurrenceCount == 0 || elems.empty())
    return std::nullopt;

  bool needsCrossOccurrenceProof = false;
  DenseMap<uint32_t, unsigned> argOccurrenceCounts;
  for (const auto &elem : elems) {
    if (!elem.isArg)
      continue;
    ++argOccurrenceCounts[elem.argIdx];
    auto r = templateInvocationArgRanges[elem.argIdx];
    StringRef baseArg =
        templateBaseInvocationText.substr(r.first, r.second - r.first).trim();
    StringRef occText =
        (*deps_.sourceMapper).SliceASource(elem.aBegin, elem.aEnd).trim();
    if (occText != baseArg)
      needsCrossOccurrenceProof = true;
  }
  for (const auto &entry : argOccurrenceCounts) {
    if (entry.second > 1)
      needsCrossOccurrenceProof = true;
  }
  if (!needsCrossOccurrenceProof)
    return std::nullopt;

  llvm::sort(elems,
             [](const ArgsOnlyTemplateElem &a, const ArgsOnlyTemplateElem &b) {
               if (a.aBegin != b.aBegin)
                 return a.aBegin < b.aBegin;
               if (a.aEnd != b.aEnd)
                 return a.aEnd < b.aEnd;
               return a.isArg < b.isArg;
             });

  // Reject unless body/argument spans form an exact partition of the macro
  // cover. Any gap would be unmodelled fixed syntax, so the template would
  // not explain the whole expansion surface.
  uint64_t cursor = cover->first;
  for (const auto &elem : elems) {
    if (elem.aBegin != cursor)
      return std::nullopt;
    cursor = elem.aEnd;
  }
  if (cursor != cover->second)
    return std::nullopt;

  auto bEnv = (*deps_.sourceMapper)
                  .MapATokRangeAToBTokenEnvelopePreserveBoundaryInsertions(
                      cover->first, cover->second);
  if (!bEnv || bEnv->first >= bEnv->second)
    return std::nullopt;

  // Keep this proof path bounded and deterministic. Large covers stay on the
  // existing conservative paths rather than using an expensive solver.
  if (elems.size() > 64 || (bEnv->second - bEnv->first) > 256 ||
      occurrenceCount > 32)
    return std::nullopt;

  TemplateAssignmentEnumerator assignmentEnumerator(
      ArrayRef<ArgsOnlyTemplateElem>(elems.data(), elems.size()), *bEnv, deps_);
  std::vector<TemplateAssignmentEnumerator::Assignment> solutions =
      assignmentEnumerator.Enumerate(occurrenceCount);
  if (solutions.empty() || solutions.size() > 16)
    return std::nullopt;

  // Find the unique child invocation that directly reuses slices of caller
  // argument `callerArgIdx` through tuple refs. Each accepted ref is
  // verified by comparing the child argument text against the referenced
  // caller-argument slice, so the result is usable only when the tuple-ref
  // metadata has one unambiguous, text-consistent child witness.
  auto findDirectTupleRefsForArg =
      [&](uint32_t callerArgIdx,
          SmallVectorImpl<RefoldModel::TupleArgRef> &outRefs) -> bool {
    outRefs.clear();

    // Work against the trimmed caller argument text because tuple-ref byte
    // ranges are relative to the normalized/trimmed caller payload, not the
    // full invocation spelling.
    StringRef parentTrim =
        templateBaseInvocationText
            .substr(templateInvocationArgRanges[callerArgIdx].first,
                    templateInvocationArgRanges[callerArgIdx].second -
                        templateInvocationArgRanges[callerArgIdx].first)
            .trim();

    bool matched = false;
    SmallVector<RefoldModel::TupleArgRef, 8> matchedRefs;

    for (const auto &cand : (*deps_.model).GetMacroInvocations()) {
      // Only direct children of the current invocation can provide direct
      // tuple references for this caller argument.
      if (!cand.callerMacroId || *cand.callerMacroId != templateInvocation.id)
        continue;

      // Tuple-ref validation requires normalized child invocation text, one
      // normalized text range per child argument, and tuple-ref metadata for
      // those same child arguments.
      if (!cand.normalizedInvText || cand.normalizedInvArgTextRanges.empty() ||
          cand.argTupleRefs.empty() ||
          cand.normalizedInvArgTextRanges.size() != cand.argTupleRefs.size())
        continue;

      SmallVector<RefoldModel::TupleArgRef, 8> localRefs;
      bool any = false;

      for (uint32_t childArgIdx = 0; childArgIdx < cand.argTupleRefs.size();
           ++childArgIdx) {
        const auto &refs = cand.argTupleRefs[childArgIdx];

        // This path only accepts direct one-to-one tuple refs. Multi-ref
        // child arguments are composition cases and are deliberately ignored
        // here.
        if (refs.size() != 1)
          continue;

        const auto &ref = refs.front();
        if (ref.callerParamIndex != callerArgIdx)
          continue;

        // The referenced caller slice must be a valid byte interval inside
        // the trimmed parent argument.
        if (ref.callerByteEnd < ref.callerByteBegin ||
            ref.callerByteEnd > parentTrim.size())
          return false;

        const auto &rng = cand.normalizedInvArgTextRanges[childArgIdx];

        // The child argument range must be a valid byte interval inside the
        // normalized child invocation text.
        if (!rng.first || !rng.second || *rng.second < *rng.first ||
            *rng.second > cand.normalizedInvText->size())
          return false;

        StringRef childText =
            StringRef(*cand.normalizedInvText)
                .slice((size_t)*rng.first, (size_t)*rng.second)
                .trim();
        StringRef parentSlice =
            parentTrim.slice(ref.callerByteBegin, ref.callerByteEnd).trim();

        // Require textual agreement between the child argument and the caller
        // slice named by the tuple ref. This prevents stale or mismatched
        // producer metadata from becoming a replay witness.
        if (childText != parentSlice)
          return false;

        localRefs.push_back(ref);
        any = true;
      }

      // This child did not reference the requested caller argument.
      if (!any)
        continue;

      // More than one child witness would make the direct tuple-ref source
      // ambiguous, so fail closed instead of choosing one.
      if (matched)
        return false;

      matched = true;
      matchedRefs = std::move(localRefs);
    }

    // No direct child supplied tuple refs for this caller argument.
    if (!matched)
      return false;

    // Return refs in caller-text order so downstream tuple reconstruction
    // sees a deterministic left-to-right decomposition of the caller
    // argument.
    llvm::sort(matchedRefs, tupleArgRefLessByCallerByteBegin);

    outRefs.append(matchedRefs.begin(), matchedRefs.end());
    return true;
  };

  // Build a concrete rewritten invocation from one solved template
  // segmentation. Each argument occurrence in the expansion must map back to
  // exactly one call-site argument rewrite: whole-argument occurrences must
  // agree globally, while variadic/tuple-shaped occurrences may rewrite
  // individual top-level caller slices only when the slice metadata proves
  // the correspondence.
  auto buildCandidateInvocation = [&](ArrayRef<std::pair<size_t, size_t>> sol)
      -> std::optional<InvocationRewriteWithRangeLocal> {
    // Group template argument occurrences by the caller formal they came
    // from. Each group must collapse into one replacement for that formal.
    DenseMap<uint32_t, SmallVector<size_t, 8>> occByArg;
    for (const auto &elem : elems) {
      if (elem.isArg)
        occByArg[elem.argIdx].push_back(elem.occurrenceOrdinal);
    }

    DenseMap<uint32_t, std::string> replByArg;
    bool changed = false;

    for (const auto &entry : occByArg) {
      const uint32_t argIdx = entry.first;
      auto argRange = templateInvocationArgRanges[argIdx];
      StringRef baseArgText = templateBaseInvocationText.substr(
          argRange.first, argRange.second - argRange.first);
      StringRef baseTrim = baseArgText.trim();

      // Recover the old expansion text and the candidate new expansion text
      // for every occurrence of this formal in the solved template.
      SmallVector<std::string, 8> oldOccs;
      SmallVector<std::string, 8> newOccs;
      for (size_t occOrdinal : entry.second) {
        const ArgsOnlyTemplateElem *occElem = nullptr;
        for (const auto &elem : elems) {
          if (elem.isArg && elem.occurrenceOrdinal == occOrdinal) {
            occElem = &elem;
            break;
          }
        }
        if (!occElem)
          return std::nullopt;

        oldOccs.push_back((*deps_.sourceMapper)
                              .SliceASource(occElem->aBegin, occElem->aEnd)
                              .trim()
                              .str());

        const auto &range = sol[occOrdinal];
        newOccs.push_back((*deps_.sourceMapper)
                              .SliceBSource(range.first, range.second)
                              .trim()
                              .str());

        // Empty replacement arguments are not accepted here because the
        // replay path expects every matched occurrence to carry concrete
        // replacement text.
        if (newOccs.back().empty())
          return std::nullopt;
      }

      // The simple case is a whole-argument rewrite: every old occurrence
      // equals the full caller argument, and every new occurrence asks for
      // the same text.
      bool allOldAreWholeArg = true;
      bool allNewSame = !newOccs.empty();
      for (size_t i = 0; i < oldOccs.size(); ++i) {
        if (StringRef(oldOccs[i]).trim() != baseTrim)
          allOldAreWholeArg = false;
        if (StringRef(newOccs[i]).trim() != StringRef(newOccs[0]).trim())
          allNewSame = false;
      }

      std::string replacement;
      if (allOldAreWholeArg && allNewSame) {
        // All expansion occurrences agree on replacing the entire caller
        // argument, so the argument rewrite is just that single replacement.
        replacement = StringRef(newOccs[0]).trim().str();
      } else if (isMacroInvocationVariadicFormal(templateInvocation, argIdx)) {
        // Variadic arguments can map occurrence-by-occurrence to top-level
        // tuple elements. The lexer split avoids treating commas inside
        // nested syntax, comments, strings, or character literals as element
        // separators.
        SmallVector<TupleElementSlice, 8> tupleElems;
        if (!splitTopLevelTupleElementsWithLexer(baseTrim, (*deps_.lexLang),
                                                 tupleElems))
          return std::nullopt;
        if (tupleElems.size() != oldOccs.size())
          return std::nullopt;

        replacement = baseTrim.str();

        // Apply replacements from right to left so earlier byte offsets
        // remain valid while editing the string.
        for (size_t i = tupleElems.size(); i > 0; --i) {
          const size_t idx = i - 1;
          const auto &elem = tupleElems[idx];
          StringRef oldElem =
              baseTrim.slice(elem.trimBegin, elem.trimEnd).trim();
          if (oldElem != StringRef(oldOccs[idx]).trim())
            return std::nullopt;
          replacement = stringutils::replaceRange(replacement, elem.trimBegin,
                                                  elem.trimEnd, newOccs[idx]);
        }
      } else {
        // Non-variadic tuple-like rewrites require explicit tuple-ref
        // metadata from a direct child invocation. Without that metadata,
        // partial call-site argument replacement would be an unproven
        // substring edit.
        SmallVector<RefoldModel::TupleArgRef, 8> tupleRefs;
        if (!findDirectTupleRefsForArg(argIdx, tupleRefs))
          return std::nullopt;
        if (tupleRefs.size() != oldOccs.size())
          return std::nullopt;

        replacement = baseTrim.str();

        // As above, edit from right to left to preserve source offsets.
        for (size_t i = tupleRefs.size(); i > 0; --i) {
          const size_t idx = i - 1;
          const auto &ref = tupleRefs[idx];
          StringRef oldElem =
              baseTrim.slice(ref.callerByteBegin, ref.callerByteEnd).trim();
          if (oldElem != StringRef(oldOccs[idx]).trim())
            return std::nullopt;
          replacement =
              stringutils::replaceRange(replacement, ref.callerByteBegin,
                                        ref.callerByteEnd, newOccs[idx]);
        }
      }

      replacement = StringRef(replacement).trim().str();

      // Reject rewrites that would erase the argument or introduce a
      // top-level comma into a non-variadic formal, because either would
      // change invocation arity/syntax rather than merely replacing the
      // argument payload.
      if (replacement.empty())
        return std::nullopt;
      if (!isMacroInvocationVariadicFormal(templateInvocation, argIdx) &&
          replacementIntroducesTopLevelComma(replacement, (*deps_.lexLang)))
        return std::nullopt;

      if (StringRef(replacement).trim() != baseTrim)
        changed = true;
      replByArg[argIdx] = std::move(replacement);
    }

    // Do not synthesize a candidate invocation unless the solved template
    // actually changes at least one call-site argument.
    if (!changed || replByArg.empty())
      return std::nullopt;

    return buildInvocationRewriteWithRangeLocal(actualRecoveryCtx, replByArg);
  };

  // Multiple token-template assignments are acceptable only when they all
  // reconstruct the same invocation spelling. Otherwise the expansion surface
  // is underdetermined and this proof path fails closed.
  std::optional<InvocationRewriteWithRangeLocal> uniqueRewrite;
  for (const auto &sol : solutions) {
    std::optional<InvocationRewriteWithRangeLocal> candidate =
        buildCandidateInvocation(sol);
    if (!candidate)
      continue;
    if (!uniqueRewrite) {
      uniqueRewrite = std::move(*candidate);
      continue;
    }
    if (uniqueRewrite->text != candidate->text)
      return std::nullopt;
    uniqueRewrite->materializedOutputByteStart =
        std::min(uniqueRewrite->materializedOutputByteStart,
                 candidate->materializedOutputByteStart);
    uniqueRewrite->materializedOutputByteEnd =
        std::max(uniqueRewrite->materializedOutputByteEnd,
                 candidate->materializedOutputByteEnd);
  }

  if (!uniqueRewrite)
    return std::nullopt;

  MacroPatch patch{*templateInvocation.invB, *templateInvocation.invE,
                   std::move(uniqueRewrite->text), templateInvocation.id};
  // Inlined CertifyArgsOnlyAcceptedCandidate: record materialized output bytes
  // and certify the materialized B-token envelope.
  patch.materialized.hasOutputByteRange = true;
  patch.materialized.outputByteStart =
      uniqueRewrite->materializedOutputByteStart;
  patch.materialized.outputByteEnd = uniqueRewrite->materializedOutputByteEnd;
  certifyMacroPatchMaterializedBTokenRange(patch,
                                           static_cast<uint64_t>(bEnv->first),
                                           static_cast<uint64_t>(bEnv->second));
  // Inlined AttachArgsOnlyProofCarrier (proof attached directly via lattice).
  {
    MacroPatchProof proof =
        (*deps_.proofLattice)
            .MakeMacroPatchProof(MacroPatchProofKind::ArgsOnlyStandard,
                                 /*preservesInvocationStructure=*/true,
                                 ctx.invocation.id);
    WholeEnvelopeReplayWitness witness;
    witness.rootMacroId = ctx.invocation.id;
    witness.replayValidated = true;
    witness.definitionTapeReplayValidated = false;
    proof.wholeEnvelopeReplay = witness;
    (*deps_.proofLattice).SetMacroPatchProof(patch, std::move(proof));
  }
  return patch;
}

} // namespace refold
} // namespace clang
