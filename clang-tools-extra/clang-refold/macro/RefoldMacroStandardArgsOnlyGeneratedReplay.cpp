//===--- RefoldMacroStandardArgsOnlyGeneratedReplay.cpp ---------*- C++ -*-===//
//
// Private generated replay and tuple-forwarding helpers for the standard
// args-only macro patch builder.
//
// This file is an internal implementation split for
// `RefoldMacroStandardArgsOnlyPatchBuilder`.  It moves the generated-callee,
// generated-leaf, parent-tuple, forwarded-tuple, and caller-tuple replay
// resolvers out of the public builder translation unit without creating a new
// public service boundary.  Candidate ranking, replay admission, ambiguity
// rejection, and fail-closed behavior remain owned by the same standard
// args-only pipeline.
//
//===----------------------------------------------------------------------===//

#include "macro/RefoldMacroStandardArgsOnlyInternals.h"

#include "edit/RefoldBInsertionLedger.h"
#include "macro/RefoldMacroGeneratedCalleeReplayEngine.h"
#include "macro/RefoldMacroGeneratedLeafReplayEngine.h"
#include "macro/RefoldMacroPlannerHelpers.h"
#include "macro/RefoldMacroRecursiveTupleGeneratedReplay.h"
#include "macro/RefoldMacroReplay.h"
#include "macro/RefoldMacroTopology.h"
#include "macro/RefoldMacroTupleHelpers.h"
#include "source/RefoldSourceMapper.h"
#include "source/RefoldToken.h"
#include "source/TokenTextHelpers.h"
#include "util/StringUtils.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/StringRef.h"

#include <algorithm>
#include <optional>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

using namespace llvm;

namespace clang {
namespace refold {
namespace {

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
/// actual slots.  It preserves replacement-token order and fails closed for
/// malformed stringification, malformed paste chains, out-of-range formal
/// references, and unsupported `__VA_OPT__` payloads.
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
  /// agrees on the same solved actuals.  Stringification is supported only when
  /// it has an exact inverse back to one tuple element; unsupported or malformed
  /// paste and `__VA_OPT__` forms, ambiguous macro definitions, conflicting
  /// occurrence solutions, or non-unique replay solutions reject this bridge and
  /// leave the caller's later tuple-forwarding/fallback policy unchanged.
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
    if (forwarderToks.empty() ||
        forwarderToks[0].kind !=
            RefoldModel::MacroReplacementTokenKind::ParamRef ||
        !forwarderToks[0].paramIndex)
      return false;
    const uint32_t calleeForwarderParam = *forwarderToks[0].paramIndex;
    if (calleeForwarderParam >= forwarderDefinition->defParams.size() ||
        calleeForwarderParam >= tupleElems.size())
      return false;

    const bool explicitParenGeneratedCall =
        forwarderToks.size() >= 4 &&
        forwarderToks[1].kind ==
            RefoldModel::MacroReplacementTokenKind::Literal &&
        forwarderToks[1].spelling == "(" &&
        forwarderToks.back().kind ==
            RefoldModel::MacroReplacementTokenKind::Literal &&
        forwarderToks.back().spelling == ")";

    const bool adjacencyGeneratedCall =
        forwarderToks.size() == 2 &&
        forwarderToks[1].kind ==
            RefoldModel::MacroReplacementTokenKind::ParamRef &&
        forwarderToks[1].paramIndex &&
        *forwarderToks[1].paramIndex != calleeForwarderParam;

    if (!explicitParenGeneratedCall && !adjacencyGeneratedCall)
      return false;

    // Accept either a direct generated-call replacement list:
    //   calleeFormal '(' generatedActualFormals... ')'
    // or the narrower adjacency stringifier shape:
    //   calleeFormal parenthesizedPayloadFormal
    // The callee formal itself is not rewritten here; it is resolved to a
    // function-like macro definition below, while the remaining formals become
    // positional tuple-edit targets.
    SmallVector<GeneratedTupleCalleeArgRef, 8> &generatedArgs =
        state.generatedArgs;
    if (explicitParenGeneratedCall) {
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
    } else {
      const uint32_t payloadForwarderParam = *forwarderToks[1].paramIndex;
      if (payloadForwarderParam >= forwarderDefinition->defParams.size())
        return false;
      GeneratedTupleCalleeArgRef ref;
      ref.forwarderParamIdx = payloadForwarderParam;
      ref.variadicPack =
          forwarderDefinition->defParams[payloadForwarderParam].variadic;
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

    if (adjacencyGeneratedCall && !IsSingleFormalStringifier(*calleeDefinition))
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
    for (size_t i = 0; i < fixedCalleeActuals; ++i) {
      if (adjacencyGeneratedCall) {
        std::optional<StringRef> actual =
            GetSingleParenthesizedAdjacencyActual(oldGeneratedActualPieces[i]);
        if (!actual)
          return false;
        oldActuals.push_back(actual->str());
        continue;
      }
      oldActuals.push_back(oldGeneratedActualPieces[i].str());
    }
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
      const bool singleStringifyPattern =
          calleePattern.size() == 1 &&
          calleePattern.front().kind ==
              StandardArgsGeneratedCalleeReplayKind::Stringify;
      const uint32_t stringifyParamIdx =
          singleStringifyPattern ? calleePattern.front().paramIdx : 0;

      bool matchedOldExpansion =
          MatchOldExpansion(oldExpansionMatcher, StringRef(obs.oldText));
      if (!matchedOldExpansion && singleStringifyPattern &&
          stringifyParamIdx < oldActuals.size()) {
        StringRef oldActual = StringRef(oldActuals[stringifyParamIdx]).trim();
        StringRef observedOldText = StringRef(obs.oldText).trim();
        matchedOldExpansion =
            oldActual == observedOldText ||
            findUniqueTrimmedSubstring(oldActual, observedOldText).has_value();
      }
      if (!matchedOldExpansion)
        continue;

      auto solved = SolveStringifyOrPasteNewExpansion(
          calleePattern, *calleeDefinition, oldActuals, obs.newText);
      if (!solved && singleStringifyPattern &&
          stringifyParamIdx < calleeDefinition->defParams.size()) {
        SmallVector<std::string, 8> actuals;
        actuals.resize(calleeDefinition->defParams.size());
        for (size_t i = 0; i < oldActuals.size(); ++i)
          actuals[i] = oldActuals[i];
        actuals[stringifyParamIdx] = StringRef(obs.newText).trim().str();
        solved = std::move(actuals);
      }
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
          StringRef observedOldText = StringRef(obs.oldText).trim();
          StringRef sourcePiece =
              StringRef(oldGeneratedPiecesForRewrite[paramIdx]);
          if (sourcePiece == observedOldText ||
              findUniqueTrimmedSubstring(sourcePiece, observedOldText)) {
            oldGeneratedPiecesForRewrite[paramIdx] = observedOldText.str();
            break;
          }

          std::optional<std::string> token =
              SingleTokenSpelling(StringRef(obs.oldText));
          if (!token)
            continue;
          std::optional<std::string> content =
              decodeSimpleStringLiteralToken(*token);
          if (!content)
            continue;
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

  /// Return the sole macro actual inside a parenthesized adjacency payload.
  ///
  /// A forwarding definition such as `CALL(f, t) f t` forms a generated macro
  /// invocation only when the tuple element bound to `t` supplies the call
  /// parentheses, for example `CALL(STR, (alpha + beta))`.  The source tuple
  /// element that must be preserved is `(alpha + beta)`, but the actual consumed
  /// by the generated one-argument callee is the payload inside those parens.
  /// This helper accepts only that fully proved one-argument shape: a balanced
  /// outer parenthesized spelling whose top-level payload does not split into
  /// multiple macro arguments.
  std::optional<StringRef> GetSingleParenthesizedAdjacencyActual(
      StringRef sourceTupleElement) const {
    StringRef trimmed = sourceTupleElement.trim();
    if (!trimmed.starts_with("(") || !trimmed.ends_with(")") ||
        trimmed.size() < 2)
      return std::nullopt;

    StringRef payload = trimmed.drop_front().drop_back();
    SmallVector<TupleElementSlice, 2> pieces;
    if (!splitTopLevelTupleElementsWithLexer(payload, deps_.lexLang, pieces) ||
        pieces.size() != 1)
      return std::nullopt;
    return TupleElementText(payload, pieces, 0);
  }

  /// Return whether `definition` is exactly `#` applied to its only formal.
  ///
  /// The adjacency-generated path below is intentionally limited to this
  /// single-argument stringification shape.  General `f t` replay for ordinary
  /// parameter substitution would need to invert the callee's macro-argument
  /// parser and map edits back into one tuple element, so unsupported callees
  /// continue to fail closed rather than guessing.
  static bool IsSingleFormalStringifier(
      const RefoldModel::MacroDirective &definition) {
    return definition.functionLike && definition.defParams.size() == 1 &&
           !definition.defParams.front().variadic &&
           definition.replacementTokens.size() == 2 &&
           definition.replacementTokens[0].spelling == "#" &&
           definition.replacementTokens[1].kind ==
               RefoldModel::MacroReplacementTokenKind::ParamRef &&
           definition.replacementTokens[1].paramIndex &&
           *definition.replacementTokens[1].paramIndex == 0;
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

    // First replay the child replacement list itself.  Some tuple forwarders do
    // not create a generated callee invocation at all: `CALL(f, t) f t` fed with
    // `(STR, pair(1, 2))` expands to the adjacent token run
    // `STR pair(1, 2)`, where `STR` is inert because it is not followed by a
    // macro-call parenthesis.  In that shape the generic old-text map contains
    // only the whole adjacent run, so preserving the parent tuple requires a
    // positional replay proof that derives the changed tuple element from the
    // child replacement tape.
    if (!DeriveDirectReplacementListRewrites(*childDefinition, childArgs,
                                             occObservations, newTextByOld))
      return false;

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
  /// Add tuple-element rewrites by replaying the child replacement list itself.
  ///
  /// This proof is intentionally separate from generated-callee replay.  It
  /// handles direct tuple-ref forwarding such as `f t`, where the expansion is a
  /// concatenation of forwarded tuple elements rather than the body of a second
  /// macro.  New text is solved by allowing exactly one forwarded formal to
  /// change while every other forwarded formal and literal token remains an
  /// old-spelling anchor.  If more than one anchored solution exists, the proof
  /// rejects so ordinary fallback behavior remains responsible for the edit.
  bool DeriveDirectReplacementListRewrites(
      const RefoldModel::MacroDirective &childDefinition,
      ArrayRef<std::pair<uint32_t, StringRef>> childArgs,
      ArrayRef<OccObservation> occObservations,
      StringMap<std::string> &newTextByOld) const {
    if (childArgs.empty() || childDefinition.replacementTokens.empty())
      return true;

    SmallVector<ForwardedGeneratedCalleeReplayElem, 8> replayPattern;
    SmallVector<std::string, 4> oldActuals;
    DenseMap<uint32_t, uint32_t> childArgToCompactParam;

    for (const RefoldModel::MacroReplacementToken &tok :
         childDefinition.replacementTokens) {
      if (tok.spelling == "#" || tok.spelling == "##" ||
          tok.spelling == "__VA_OPT__")
        return true;

      ForwardedGeneratedCalleeReplayElem elem;
      if (tok.kind != RefoldModel::MacroReplacementTokenKind::ParamRef) {
        elem.literal = tok.spelling.str();
        replayPattern.push_back(std::move(elem));
        continue;
      }

      if (!tok.paramIndex)
        return true;
      std::optional<StringRef> oldText = ChildArgText(childArgs, *tok.paramIndex);
      if (!oldText)
        return true;

      auto compactIt = childArgToCompactParam.find(*tok.paramIndex);
      uint32_t compactParam = 0;
      if (compactIt == childArgToCompactParam.end()) {
        compactParam = static_cast<uint32_t>(oldActuals.size());
        childArgToCompactParam[*tok.paramIndex] = compactParam;
        oldActuals.push_back(oldText->trim().str());
      } else {
        compactParam = compactIt->second;
      }

      elem.isParam = true;
      elem.paramIdx = compactParam;
      replayPattern.push_back(std::move(elem));
    }

    if (oldActuals.empty())
      return true;

    SmallVector<SmallVector<std::string, 8>, 4> oldActualTokSpellings;
    for (const std::string &actual : oldActuals)
      oldActualTokSpellings.push_back(
          tokenSpellingsForReplayText<8>(actual, deps_.lexLang));

    ForwardedGeneratedCalleeReplaySolver oldReplayMatcher(
        ArrayRef<ForwardedGeneratedCalleeReplayElem>(replayPattern.data(),
                                                     replayPattern.size()),
        ArrayRef<SmallVector<std::string, 8>>(oldActualTokSpellings.data(),
                                              oldActualTokSpellings.size()),
        oldActuals.size(), deps_.lexLang);

    for (const OccObservation &obs : occObservations) {
      if (!oldReplayMatcher.MatchOldExpansion(StringRef(obs.oldText)))
        continue;

      std::optional<SmallVector<std::string, 4>> solvedActuals =
          SolveSingleChangedDirectReplay(
              ArrayRef<ForwardedGeneratedCalleeReplayElem>(
                  replayPattern.data(), replayPattern.size()),
              ArrayRef<SmallVector<std::string, 8>>(
                  oldActualTokSpellings.data(), oldActualTokSpellings.size()),
              ArrayRef<std::string>(oldActuals.data(), oldActuals.size()),
              obs.newText);
      if (!solvedActuals)
        return false;
      if (solvedActuals->size() != oldActuals.size())
        return false;

      for (size_t i = 0; i < solvedActuals->size(); ++i) {
        StringRef oldKey = StringRef(oldActuals[i]).trim();
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

  /// Solve a direct child replacement replay where exactly one forwarded formal
  /// is allowed to change and all other formals remain old-spelling anchors.
  std::optional<SmallVector<std::string, 4>> SolveSingleChangedDirectReplay(
      ArrayRef<ForwardedGeneratedCalleeReplayElem> replayPattern,
      ArrayRef<SmallVector<std::string, 8>> oldActualTokSpellings,
      ArrayRef<std::string> oldActuals, StringRef newExpansion) const {
    SmallVector<ParentTupleCalleeReplayTok, 16> toks;
    lexReplayTokens(newExpansion, deps_.lexLang, toks);

    SmallVector<SmallVector<std::string, 4>, 4> solutions;
    for (size_t changedParam = 0; changedParam < oldActuals.size();
         ++changedParam) {
      std::optional<std::pair<size_t, size_t>> assignedChangedRange;
      SolveDirectReplayDfs(replayPattern, oldActualTokSpellings, oldActuals,
                           newExpansion, toks, changedParam, 0, 0,
                           assignedChangedRange, solutions);
      if (solutions.size() > 1)
        return std::nullopt;
    }

    if (solutions.size() != 1)
      return std::nullopt;
    return solutions.front();
  }

  /// DFS used by `SolveSingleChangedDirectReplay`.  Unchanged formals are fixed
  /// anchors; the candidate changed formal is the only variable-width slice.
  void SolveDirectReplayDfs(
      ArrayRef<ForwardedGeneratedCalleeReplayElem> replayPattern,
      ArrayRef<SmallVector<std::string, 8>> oldActualTokSpellings,
      ArrayRef<std::string> oldActuals, StringRef newExpansion,
      ArrayRef<ParentTupleCalleeReplayTok> toks, size_t changedParam,
      size_t elemIdx, size_t tokPos,
      std::optional<std::pair<size_t, size_t>> &assignedChangedRange,
      SmallVectorImpl<SmallVector<std::string, 4>> &solutions) const {
    if (solutions.size() > 1)
      return;

    if (elemIdx == replayPattern.size()) {
      if (tokPos != toks.size() || !assignedChangedRange)
        return;
      SmallVector<std::string, 4> actuals;
      actuals.reserve(oldActuals.size());
      for (size_t i = 0; i < oldActuals.size(); ++i) {
        if (i != changedParam) {
          actuals.push_back(oldActuals[i]);
          continue;
        }
        const size_t beginTok = assignedChangedRange->first;
        const size_t endTok = assignedChangedRange->second;
        if (beginTok == endTok) {
          actuals.push_back(std::string());
          continue;
        }
        const size_t byteBegin = toks[beginTok].begin;
        const size_t byteEnd = toks[endTok - 1].end;
        actuals.push_back(newExpansion.slice(byteBegin, byteEnd).str());
      }
      if (StringRef(actuals[changedParam]).trim() !=
          StringRef(oldActuals[changedParam]).trim())
        solutions.push_back(std::move(actuals));
      return;
    }

    const ForwardedGeneratedCalleeReplayElem &elem = replayPattern[elemIdx];
    if (!elem.isParam) {
      if (tokPos < toks.size() && toks[tokPos].spelling == elem.literal)
        SolveDirectReplayDfs(replayPattern, oldActualTokSpellings, oldActuals,
                             newExpansion, toks, changedParam, elemIdx + 1,
                             tokPos + 1, assignedChangedRange, solutions);
      return;
    }

    if (elem.paramIdx >= oldActuals.size())
      return;

    if (elem.paramIdx != changedParam) {
      const SmallVector<std::string, 8> &expected =
          oldActualTokSpellings[elem.paramIdx];
      if (replayTokenRangeSpellingsEqual(toks, tokPos, expected))
        SolveDirectReplayDfs(replayPattern, oldActualTokSpellings, oldActuals,
                             newExpansion, toks, changedParam, elemIdx + 1,
                             tokPos + expected.size(), assignedChangedRange,
                             solutions);
      return;
    }

    if (assignedChangedRange) {
      const size_t width = assignedChangedRange->second -
                           assignedChangedRange->first;
      if (tokPos + width <= toks.size() &&
          TokenRangesHaveSameSpellings(toks, assignedChangedRange->first,
                                       tokPos, width))
        SolveDirectReplayDfs(replayPattern, oldActualTokSpellings, oldActuals,
                             newExpansion, toks, changedParam, elemIdx + 1,
                             tokPos + width, assignedChangedRange, solutions);
      return;
    }

    for (size_t endTok = tokPos; endTok <= toks.size(); ++endTok) {
      assignedChangedRange = std::make_pair(tokPos, endTok);
      SolveDirectReplayDfs(replayPattern, oldActualTokSpellings, oldActuals,
                           newExpansion, toks, changedParam, elemIdx + 1,
                           endTok, assignedChangedRange, solutions);
      assignedChangedRange.reset();
      if (solutions.size() > 1)
        return;
    }
  }

  /// Compare two token ranges inside one replay token vector by spelling.
  static bool TokenRangesHaveSameSpellings(
      ArrayRef<ParentTupleCalleeReplayTok> toks, size_t lhsBegin,
      size_t rhsBegin, size_t width) {
    if (lhsBegin + width > toks.size() || rhsBegin + width > toks.size())
      return false;
    for (size_t i = 0; i < width; ++i)
      if (toks[lhsBegin + i].spelling != toks[rhsBegin + i].spelling)
        return false;
    return true;
  }

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

} // namespace

CallerTupleForwardedRewriteResolver::CallerTupleForwardedRewriteResolver(
    const RefoldMacroStandardArgsOnlyPatchBuilder::Dependencies &deps,
    const RefoldModel::MacroInvocation &invocation)
    : deps_(deps), invocation_(invocation) {}

/// Try to rebuild `baseArgText` as a structural caller-tuple edit.
bool CallerTupleForwardedRewriteResolver::TryRewrite(
    uint32_t callerArgIdx, StringRef baseArgText,
    ArrayRef<OccObservation> occObservations, std::string &outNewArg) const {
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


/// Return a child argument slice from normalized invocation text. This is used
/// by tuple-ref mode because tuple refs are expressed over normalized child
/// argument text/ranges.
std::optional<StringRef>
CallerTupleForwardedRewriteResolver::GetNormalizedArgText(
    const RefoldModel::MacroInvocation &inv, uint32_t argIdx) {
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
std::optional<StringRef>
CallerTupleForwardedRewriteResolver::GetInvocationArgText(
    const RefoldModel::MacroInvocation &inv, uint32_t argIdx) {
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


HigherOrderGeneratedReplayProbe::HigherOrderGeneratedReplayProbe(
    const RefoldMacroStandardArgsOnlyPatchBuilder::Dependencies &deps)
    : deps_(deps) {}

/// Return the first higher-order generated replay candidate in theorem order,
/// or nullopt when no generated replay proof applies.
std::optional<MacroPatch> HigherOrderGeneratedReplayProbe::TryBuild(
    const RefoldModel::MacroInvocation &invocation, const diffutils::Hunk &hunk,
    StringRef baseInvocationText,
    ArrayRef<std::pair<size_t, size_t>> invocationArgRanges) const {
  if (auto generatedCalleePatch = TryBuildGeneratedCalleeReplay(
          invocation, baseInvocationText, invocationArgRanges))
    return generatedCalleePatch;

  if (auto pasteGeneratedPatch = TryBuildPasteGeneratedCalleeReplay(
          invocation, baseInvocationText, invocationArgRanges))
    return pasteGeneratedPatch;

  if (auto objectSelectorTupleGeneratedPatch =
          TryBuildObjectSelectorTupleGeneratedCalleeReplay(
              invocation, baseInvocationText, invocationArgRanges))
    return objectSelectorTupleGeneratedPatch;

  if (auto pasteTupleGeneratedPatch = TryBuildPasteTupleGeneratedCalleeReplay(
          invocation, baseInvocationText, invocationArgRanges))
    return pasteTupleGeneratedPatch;

  if (auto tupleGeneratedPatch = TryBuildTupleGeneratedCalleeReplay(
          invocation, baseInvocationText, invocationArgRanges))
    return tupleGeneratedPatch;

  if (auto recursiveTupleGeneratedPatch =
          TryBuildRecursiveTupleGeneratedCalleeReplay(
              invocation, baseInvocationText, invocationArgRanges))
    return recursiveTupleGeneratedPatch;

  return TryBuildGeneratedLeafReplay(invocation, hunk, baseInvocationText,
                                     invocationArgRanges);
}


/// Locate the invocation's defining directive with the same linear scan used
/// by the former inline probes.  The helper performs no alias resolution and
/// returns null for missing producer metadata.
const RefoldModel::MacroDirective *
HigherOrderGeneratedReplayProbe::FindRootDefinition(
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
std::optional<std::pair<size_t, size_t>>
HigherOrderGeneratedReplayProbe::MapWholeCoverBEnvelope(
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
std::optional<MacroPatch>
HigherOrderGeneratedReplayProbe::TryBuildGeneratedCalleeReplay(
    const RefoldModel::MacroInvocation &invocation, StringRef baseInvocationText,
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
std::optional<MacroPatch>
HigherOrderGeneratedReplayProbe::TryBuildGeneratedLeafReplay(
    const RefoldModel::MacroInvocation &invocation, const diffutils::Hunk &hunk,
    StringRef baseInvocationText,
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


/// Try the recursive tuple-generated-callee theorem after direct generated and
/// direct tuple-generated proofs have declined, but before generated-leaf /
/// whole-cover fallback can materialize the edited expansion directly.
std::optional<MacroPatch>
HigherOrderGeneratedReplayProbe::TryBuildRecursiveTupleGeneratedCalleeReplay(
    const RefoldModel::MacroInvocation &invocation, StringRef baseInvocationText,
    ArrayRef<std::pair<size_t, size_t>> invocationArgRanges) const {
  if (!invocation.definitionDirectiveId || !invocation.invB ||
      !invocation.invE)
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
      !rootDefinition->functionLike)
    return std::nullopt;

  RefoldMacroRecursiveTupleGeneratedReplay recursiveReplay(
      RefoldMacroRecursiveTupleGeneratedReplay::Dependencies{
          deps_.model, deps_.sourceMapper, deps_.macroTopology,
          deps_.generatedCalleeReplayEngine, deps_.proofCertifier,
          deps_.lexLang});
  RecursiveTupleGeneratedReplayRequest recursiveRequest{
      invocation, *rootDefinition, baseInvocationText, invocationArgRanges,
      *cover, *bEnv};
  return recursiveReplay.BuildCandidate(recursiveRequest);
}



/// Try the paste/generated-callee theorem for roots such as `a##b(x)`.
///
/// This probe is separate from paste/tuple replay: the pasted token supplies the
/// generated callee name, but the generated call actuals are ordinary
/// replacement-list arguments rather than elements of one parenthesized tuple.
/// It is ranked before tuple-specific replay and remains fail-closed when the
/// root replacement tape is anything other than one paste-derived call.
std::optional<MacroPatch>
HigherOrderGeneratedReplayProbe::TryBuildPasteGeneratedCalleeReplay(
    const RefoldModel::MacroInvocation &invocation, StringRef baseInvocationText,
    ArrayRef<std::pair<size_t, size_t>> invocationArgRanges) const {
  if (!invocation.definitionDirectiveId || !invocation.invB ||
      !invocation.invE)
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
      !rootDefinition->functionLike)
    return std::nullopt;

  PasteGeneratedCalleeReplayContext pasteGeneratedContext{
      invocation, baseInvocationText, invocationArgRanges, *cover, *bEnv,
      *rootDefinition};
  return deps_.generatedCalleeReplayEngine
      .BuildPasteGeneratedCalleeReplayCandidate(pasteGeneratedContext);
}

/// Try the paste/tuple generated-callee theorem for roots such as `a##b t`.
///
/// This probe sits after ordinary generated-callee replay and before the
/// direct tuple-generated bridge.  It only recognizes a root replacement tape
/// whose final token is the tuple actual and whose preceding tape contains a
/// paste expression that can deterministically synthesize the generated callee
/// token.
std::optional<MacroPatch>
HigherOrderGeneratedReplayProbe::TryBuildPasteTupleGeneratedCalleeReplay(
    const RefoldModel::MacroInvocation &invocation, StringRef baseInvocationText,
    ArrayRef<std::pair<size_t, size_t>> invocationArgRanges) const {
  if (!invocation.definitionDirectiveId || !invocation.invB ||
      !invocation.invE)
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
      !rootDefinition->functionLike)
    return std::nullopt;

  RefoldMacroRecursiveTupleGeneratedReplay tupleReplay(
      RefoldMacroRecursiveTupleGeneratedReplay::Dependencies{
          deps_.model, deps_.sourceMapper, deps_.macroTopology,
          deps_.generatedCalleeReplayEngine, deps_.proofCertifier,
          deps_.lexLang});
  RecursiveTupleGeneratedReplayRequest tupleRequest{
      invocation, *rootDefinition, baseInvocationText, invocationArgRanges,
      *cover, *bEnv};
  return tupleReplay.BuildPasteTupleCandidate(tupleRequest);
}

/// Try the object-selector/tuple generated-callee theorem for roots such as
/// `f t` where `f` is an object-like selector actual.
///
/// This probe sits before paste/tuple and ordinary tuple-generated replay.  It
/// is intentionally limited to the source-level `f t` shape so ordinary
/// args-only and existing generated-callee proofs keep owning simpler direct
/// cases.
std::optional<MacroPatch>
HigherOrderGeneratedReplayProbe::TryBuildObjectSelectorTupleGeneratedCalleeReplay(
    const RefoldModel::MacroInvocation &invocation, StringRef baseInvocationText,
    ArrayRef<std::pair<size_t, size_t>> invocationArgRanges) const {
  if (!invocation.definitionDirectiveId || !invocation.invB ||
      !invocation.invE)
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
      !rootDefinition->functionLike)
    return std::nullopt;

  RefoldMacroRecursiveTupleGeneratedReplay tupleReplay(
      RefoldMacroRecursiveTupleGeneratedReplay::Dependencies{
          deps_.model, deps_.sourceMapper, deps_.macroTopology,
          deps_.generatedCalleeReplayEngine, deps_.proofCertifier,
          deps_.lexLang});
  RecursiveTupleGeneratedReplayRequest tupleRequest{
      invocation, *rootDefinition, baseInvocationText, invocationArgRanges,
      *cover, *bEnv};
  return tupleReplay.BuildObjectSelectorTupleCandidate(tupleRequest);
}

std::optional<MacroPatch>
HigherOrderGeneratedReplayProbe::TryBuildTupleGeneratedCalleeReplay(
    const RefoldModel::MacroInvocation &invocation, StringRef baseInvocationText,
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

} // namespace refold
} // namespace clang
