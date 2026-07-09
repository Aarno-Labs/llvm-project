//===--- RefoldMacroGeneratedCalleeReplayEngine.cpp -------------*- C++ -*-===//
//
// Implementation of the higher-order generated-callee replay engine.
//
// The engine has no back-reference to `RefoldMacroPatchPlanner`.
// Planner-side helpers still owned by the planner
// (`BuildInvocationRewriteWithRange`, `ResolveFunctionLikeMacro*`) are
// reached through the std::function callbacks supplied in the engine's
// Dependencies bundle.
//
//===----------------------------------------------------------------------===//

#include "macro/RefoldMacroGeneratedCalleeReplayEngine.h"

#include "core/RefoldLog.h"
#include "core/RefoldModel.h"
#include "macro/RefoldArgTextRecovery.h"
#include "macro/RefoldMacroPatchProofCertifier.h"
#include "macro/RefoldMacroPlannerHelpers.h"
#include "macro/RefoldMacroTupleHelpers.h"
#include "source/RefoldSourceMapper.h"
#include "source/RefoldToken.h"
#include "source/TokenTextHelpers.h"
#include "util/StringUtils.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/FormatVariadic.h"

#include <algorithm>
#include <optional>
#include <string>
#include <utility>
#include <vector>

using namespace llvm;

namespace clang {
namespace refold {

namespace {

/// Local token carrier used by generated-callee replay-token comparisons.
/// Local token spelling/range record used while analyzing a generated-callee
/// call surface.
struct ReplayTok {
  std::string spelling;
  size_t begin = 0;
  size_t end = 0;
};

/// Replacement-list shape for one deterministic generated call step.
///
/// The generated-callee engine constructs these shapes while replaying a callee
/// call that was produced by macro expansion rather than source spelling.
struct GeneratedCalleeCallShape {
  uint32_t calleeParamIdx = 0;
  llvm::SmallVector<std::pair<size_t, size_t>, 8> argTokenRanges;
  llvm::SmallVector<std::string, 8> prefixLiterals;
  llvm::SmallVector<std::string, 8> suffixLiterals;
};


/// Lexes one generated-callee replay surface into stable token spellings and
/// byte spans.  The helper trusts only the supplied text and language options;
/// it performs no admission, preserves lexer token order, and cannot introduce
/// ambiguity because callers decide uniqueness over the returned sequence.
void lexGeneratedCalleeReplayTokens(StringRef text,
                                    const clang::LangOptions &lexLang,
                                    SmallVectorImpl<ReplayTok> &out) {
  out.clear();
  SmallVector<RefoldLexBoundaryToken, 32> toks;
  refoldLexBoundaryTokens(text, lexLang, toks);
  for (const RefoldLexBoundaryToken &tok : toks)
    out.push_back(ReplayTok{tok.spelling, tok.begin, tok.end});
}

/// Returns token spellings for token-equivalence checks in replay solvers.
/// The input text is trusted as already-selected proof evidence, and the
/// ordering is the lexer order used by the old local helpers.
SmallVector<std::string, 16>
generatedCalleeTokenSpellingsForText(StringRef text,
                                      const clang::LangOptions &lexLang) {
  SmallVector<ReplayTok, 16> toks;
  lexGeneratedCalleeReplayTokens(text, lexLang, toks);
  SmallVector<std::string, 16> out;
  for (const ReplayTok &tok : toks)
    out.push_back(tok.spelling);
  return out;
}

/// Compares two replay texts by token spelling rather than raw whitespace.
/// This preserves the existing fail-closed ambiguity policy by answering only
/// equivalence; it never chooses among multiple edits or changes solver order.
bool generatedCalleeTextsTokenEquivalent(StringRef lhs, StringRef rhs,
                                          const clang::LangOptions &lexLang) {
  SmallVector<std::string, 16> lhsToks =
      generatedCalleeTokenSpellingsForText(lhs, lexLang);
  SmallVector<std::string, 16> rhsToks =
      generatedCalleeTokenSpellingsForText(rhs, lexLang);
  if (lhsToks.size() != rhsToks.size())
    return false;
  for (size_t i = 0; i < lhsToks.size(); ++i)
    if (lhsToks[i] != rhsToks[i])
      return false;
  return true;
}

/// Resolves the replacement-list range that supplies a generated callee.
///
/// The caller trusts the macro definition's replacement-token tape and the
/// replay-safe function-like resolver supplied by the planner.  This resolver
/// owns only the deterministic selector inversion proof: a range is accepted
/// when it is either one non-variadic formal reference or a chain of selector
/// calls whose replacement list is exactly one non-variadic formal.  Ambiguous
/// or structurally richer selectors fail closed by returning nullopt.  The
/// recursive selector walk preserves the caller's left-to-right scan order by
/// resolving only the range requested by the shape finder.
class SelectorCalleeResolver {
public:
  SelectorCalleeResolver(
      const RefoldMacroGeneratedCalleeReplayEngine::Dependencies &deps,
      const RefoldModel::MacroDirective &definition)
      : deps_(deps), definition_(definition),
        toks_(definition.replacementTokens.data(),
              definition.replacementTokens.size()) {}

  /// Resolves `begin..end` to the original generated-callee formal, if provable.
  ///
  /// The range must be either one direct parameter reference or a selector call
  /// whose replacement list selects one non-variadic actual that can be resolved
  /// recursively.  This method does not enumerate alternatives.
  std::optional<uint32_t> Resolve(size_t begin, size_t end,
                                  unsigned depth) const {
    // Reject malformed ranges and cap selector recursion by the finite macro
    // definition set so cyclic selector chains cannot become open-ended proof
    // obligations.
    if (begin >= end || end > toks_.size() ||
        depth > deps_.model.GetMacroDirectives().size())
      return std::nullopt;

    // Base case: the callee is already a direct reference to one formal in the
    // original definition's replacement tape.
    if (end == begin + 1 &&
        toks_[begin].kind == RefoldModel::MacroReplacementTokenKind::ParamRef &&
        toks_[begin].paramIndex &&
        *toks_[begin].paramIndex < definition_.defParams.size())
      return *toks_[begin].paramIndex;

    // Recursive case must begin as a function-like selector invocation:
    //   SELECTOR(...)
    // Richer token sequences are intentionally not interpreted.
    if (begin + 3 > end ||
        toks_[begin].kind != RefoldModel::MacroReplacementTokenKind::Literal ||
        !IsLiteralToken(begin + 1, "("))
      return std::nullopt;

    auto selectorClose = FindMatchingParen(begin + 1, end);
    if (!selectorClose || *selectorClose + 1 != end)
      return std::nullopt;

    const RefoldModel::MacroDirective *selectorDefinition =
        deps_.resolveFunctionLikeMacroForReplay(StringRef(toks_[begin].spelling));
    if (!selectorDefinition || selectorDefinition->defParams.empty())
      return std::nullopt;

    // The selector's actual ranges are collected in source order and validated
    // against the selector definition before the replacement-list selection is
    // trusted.
    SmallVector<std::pair<size_t, size_t>, 8> selectorArgs;
    if (!CollectTopLevelArgRanges(begin + 1, *selectorClose, selectorArgs) ||
        !macroDefinitionAcceptsActualCount(*selectorDefinition,
                                           selectorArgs.size()))
      return std::nullopt;

    // Only selectors that reduce to exactly one non-variadic formal reference
    // are invertible.  Stringification, paste, literals, multi-token bodies, and
    // variadic targets remain unsupported and fail closed.
    if (selectorDefinition->replacementTokens.size() != 1)
      return std::nullopt;
    const RefoldModel::MacroReplacementToken &selected =
        selectorDefinition->replacementTokens.front();
    if (selected.kind != RefoldModel::MacroReplacementTokenKind::ParamRef ||
        !selected.paramIndex ||
        *selected.paramIndex >= selectorDefinition->defParams.size() ||
        selectorDefinition->defParams[*selected.paramIndex].variadic ||
        *selected.paramIndex >= selectorArgs.size())
      return std::nullopt;

    // Recurse only into the selected actual.  This preserves deterministic
    // selector semantics instead of treating unselected arguments as candidates.
    const auto selectedArg = selectorArgs[*selected.paramIndex];
    return Resolve(selectedArg.first, selectedArg.second, depth + 1);
  }

private:
  /// Returns whether `idx` is the requested literal replacement token.
  bool IsLiteralToken(size_t idx, StringRef spelling) const {
    return idx < toks_.size() &&
           toks_[idx].kind == RefoldModel::MacroReplacementTokenKind::Literal &&
           toks_[idx].spelling == spelling;
  }

  /// Finds the close parenthesis matching `openIdx` before `limit`.
  ///
  /// Only literal parenthesis tokens affect nesting; malformed or unmatched
  /// input fails closed.
  std::optional<size_t> FindMatchingParen(size_t openIdx, size_t limit) const {
    if (!IsLiteralToken(openIdx, "("))
      return std::nullopt;
    unsigned depth = 1;
    for (size_t i = openIdx + 1; i < limit; ++i) {
      if (toks_[i].kind != RefoldModel::MacroReplacementTokenKind::Literal)
        continue;
      if (toks_[i].spelling == "(") {
        ++depth;
        continue;
      }
      if (toks_[i].spelling != ")")
        continue;
      if (--depth == 0)
        return i;
    }
    return std::nullopt;
  }

  /// Collects top-level selector actual ranges between matching parentheses.
  ///
  /// Ranges are emitted left-to-right.  Empty actuals and unbalanced nested
  /// parentheses fail closed; the caller validates the final argument count.
  bool CollectTopLevelArgRanges(
      size_t openIdx, size_t closeIdx,
      SmallVectorImpl<std::pair<size_t, size_t>> &out) const {
    if (openIdx >= closeIdx)
      return false;
    size_t argBegin = openIdx + 1;
    unsigned argDepth = 0;
    for (size_t i = openIdx + 1; i <= closeIdx; ++i) {
      const bool atEnd = i == closeIdx;
      if (!atEnd) {
        const auto &tok = toks_[i];
        if (tok.kind == RefoldModel::MacroReplacementTokenKind::Literal) {
          if (tok.spelling == "(") {
            ++argDepth;
          } else if (tok.spelling == ")") {
            // A close parenthesis at depth zero would escape the selector call
            // whose close was already identified by FindMatchingParen.
            if (argDepth == 0)
              return false;
            --argDepth;
          }
        }
      }

      // The matching close parenthesis acts as the final delimiter.  Otherwise,
      // only commas at top-level selector-argument depth split actual ranges.
      if (atEnd ||
          (argDepth == 0 &&
           toks_[i].kind == RefoldModel::MacroReplacementTokenKind::Literal &&
           toks_[i].spelling == ",")) {
        // Empty actuals are not replay-safe for selector inversion because they
        // cannot identify a unique non-empty replacement-token range.
        if (argBegin == i)
          return false;
        out.push_back({argBegin, i});
        argBegin = i + 1;
      }
    }
    return !out.empty();
  }

  const RefoldMacroGeneratedCalleeReplayEngine::Dependencies &deps_;
  const RefoldModel::MacroDirective &definition_;
  ArrayRef<RefoldModel::MacroReplacementToken> toks_;
};

/// Classifies one token-level element in a generated-callee replay pattern.
enum class GeneratedReplayKind {
  /// Literal replacement text.
  Literal,
  /// Formal parameter reference.
  Param,
  /// Stringification of a formal parameter.
  Stringify,
  /// Token-paste expression.
  Paste
};

/// One literal or parameter piece inside a generated token-paste expression.
struct GeneratedPastePiece {
  /// Whether this paste piece references a formal parameter.
  bool isParam = false;

  /// Formal parameter index when `isParam` is true.
  uint32_t paramIdx = 0;

  /// Literal spelling when `isParam` is false.
  std::string literal;
};

/// One parsed element of a generated-callee replay pattern.
struct GeneratedReplayElem {
  /// Element kind controlling which payload fields are meaningful.
  GeneratedReplayKind kind = GeneratedReplayKind::Literal;

  /// Literal spelling for literal replay elements.
  std::string literal;

  /// Formal parameter index for parameter or stringification elements.
  uint32_t paramIdx = 0;

  /// Ordered paste pieces for token-paste replay elements.
  std::vector<GeneratedPastePiece> pastePieces;
};

using GeneratedSolvedActuals = SmallVector<std::string, 8>;

/// Parses the final generated-callee replacement tape into replay elements.
///
/// The caller trusts the current macro definition and the recovered old actual
/// slots.  The parser owns the syntactic rejection obligations for unsupported
/// token forms: malformed stringification, malformed paste chains,
/// out-of-range formal references, and `__VA_OPT__` all fail closed.  It sets
/// the generated-callee proof facts for stringification and paste exactly when
/// the old inline parser did, while preserving replacement-token order in the
/// emitted replay pattern.
class GeneratedCalleeReplayPatternParser {
public:
  GeneratedCalleeReplayPatternParser(
      const RefoldModel::MacroDirective &definition,
      ArrayRef<std::string> oldActuals, bool &usesStringification,
      bool &usesPaste)
      : definition_(definition), oldActuals_(oldActuals),
        usesStringification_(usesStringification), usesPaste_(usesPaste) {}

  /// Parses `begin..end` into ordered generated-callee replay elements.
  ///
  /// Unsupported replay syntax fails closed.  Stringification and paste proof
  /// facts are set only when the corresponding token form is accepted.
  bool Parse(size_t begin, size_t end,
             std::vector<GeneratedReplayElem> &out) const {
    const auto &tokens = definition_.replacementTokens;
    for (size_t i = begin; i < end;) {
      const auto &tok = tokens[i];

      // Accept only the canonical stringification form:
      //   # <formal-param>
      // Any missing, non-param, or out-of-range operand is rejected.
      if (tok.spelling == "#") {
        usesStringification_ = true;
        if (i + 1 >= end ||
            tokens[i + 1].kind !=
                RefoldModel::MacroReplacementTokenKind::ParamRef ||
            !tokens[i + 1].paramIndex)
          return false;
        const uint32_t paramIdx = *tokens[i + 1].paramIndex;
        if (paramIdx >= oldActuals_.size())
          return false;
        GeneratedReplayElem elem;
        elem.kind = GeneratedReplayKind::Stringify;
        elem.paramIdx = paramIdx;
        out.push_back(std::move(elem));
        i += 2;
        continue;
      }

      // A paste replay element starts when the next token is ##.  The chain is
      // consumed left-to-right so paste-piece ordering matches the replacement
      // tape exactly.
      if (i + 1 < end && tokens[i + 1].spelling == "##") {
        usesPaste_ = true;
        GeneratedReplayElem elem;
        elem.kind = GeneratedReplayKind::Paste;
        GeneratedPastePiece first;
        if (!PastePieceFromReplacementToken(tok, first))
          return false;
        elem.pastePieces.push_back(std::move(first));
        i += 2;
        while (true) {
          // A dangling ## has no replay-safe right operand.
          if (i >= end)
            return false;
          GeneratedPastePiece next;
          if (!PastePieceFromReplacementToken(tokens[i], next))
            return false;
          elem.pastePieces.push_back(std::move(next));
          ++i;
          if (i >= end || tokens[i].spelling != "##")
            break;
          ++i;
        }
        out.push_back(std::move(elem));
        continue;
      }

      // Plain parameter references replay as direct old-actual substitutions.
      if (tok.kind == RefoldModel::MacroReplacementTokenKind::ParamRef) {
        if (!tok.paramIndex || *tok.paramIndex >= oldActuals_.size())
          return false;
        GeneratedReplayElem elem;
        elem.kind = GeneratedReplayKind::Param;
        elem.paramIdx = *tok.paramIndex;
        out.push_back(std::move(elem));
        ++i;
        continue;
      }

      // Standalone paste, standalone stringification handled above, and
      // __VA_OPT__ are not part of this generated-callee replay language.
      if (tok.spelling == "##" || tok.spelling == "__VA_OPT__")
        return false;
      GeneratedReplayElem elem;
      elem.kind = GeneratedReplayKind::Literal;
      elem.literal = tok.spelling.str();
      out.push_back(std::move(elem));
      ++i;
    }
    return true;
  }

private:
  /// Converts one replacement token into a replay-safe paste piece.
  ///
  /// Only literals and in-range formal references are accepted.  Operators and
  /// `__VA_OPT__` fail closed because they cannot be replayed as paste operands
  /// by this generated-callee parser.
  bool PastePieceFromReplacementToken(
      const RefoldModel::MacroReplacementToken &tok,
      GeneratedPastePiece &piece) const {
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

  const RefoldModel::MacroDirective &definition_;
  ArrayRef<std::string> oldActuals_;
  bool &usesStringification_;
  bool &usesPaste_;
};

/// Solves one generated-callee paste token against a candidate token spelling.
///
/// The trusted inputs are the already-parsed paste pieces, the spelling of the
/// single expansion token currently being inverted, and the seed assignments
/// accumulated by the surrounding replay solver.  The solver preserves the
/// existing piece order, uses original actual widths for unanchored paste forms,
/// and stops as soon as more than one assignment is found so ambiguity remains
/// fail-closed at the caller's unique-solution check.
class GeneratedCalleePasteActualSolver {
public:
  GeneratedCalleePasteActualSolver(ArrayRef<std::string> oldActuals,
                                   const clang::LangOptions &lexLang)
      : oldActuals_(oldActuals), lexLang_(lexLang) {}

  /// Assigns one solved actual while preserving existing compatible bindings.
  ///
  /// A slot still equal to its old actual may be refined to `value`.  A slot
  /// already refined must remain token-equivalent to `value`; conflicts fail
  /// closed.
  bool AssignSolvedActual(GeneratedSolvedActuals &actuals, uint32_t paramIdx,
                          StringRef value) const {
    if (paramIdx >= actuals.size())
      return false;

    // Existing assignments are compared by token equivalence, not raw spelling,
    // so harmless lexical differences do not create false conflicts.
    if (!generatedCalleeTextsTokenEquivalent(actuals[paramIdx],
                                             oldActuals_[paramIdx], lexLang_) &&
        !generatedCalleeTextsTokenEquivalent(actuals[paramIdx], value,
                                             lexLang_))
      return false;

    // The old actual is the unassigned sentinel for this slot.
    if (generatedCalleeTextsTokenEquivalent(actuals[paramIdx],
                                            oldActuals_[paramIdx], lexLang_)) {
      actuals[paramIdx] = value.trim().str();
      return true;
    }

    return generatedCalleeTextsTokenEquivalent(actuals[paramIdx], value,
                                               lexLang_);
  }

  /// Enumerates replay-safe assignments for one pasted expansion token.
  ///
  /// Literal-anchored paste forms use DFS over the candidate spelling.  Paste
  /// forms without a literal anchor keep the old deterministic inverse by using
  /// original actual widths instead of inventing arbitrary split points.
  SmallVector<GeneratedSolvedActuals, 4>
  Solve(ArrayRef<GeneratedPastePiece> pieces, StringRef spelling,
        const GeneratedSolvedActuals &seed) const {
    SmallVector<GeneratedSolvedActuals, 4> solutions;
    const bool hasLiteralAnchor =
        llvm::any_of(pieces, [](const GeneratedPastePiece &piece) {
          return !piece.isParam && !piece.literal.empty();
        });

    // Without a fixed literal anchor, preserve the old deterministic inverse:
    // use the original actual widths instead of inventing arbitrary cuts.
    if (!hasLiteralAnchor) {
      GeneratedSolvedActuals cur = seed;
      size_t cursor = 0;
      for (const GeneratedPastePiece &piece : pieces) {
        if (!piece.isParam) {
          if (!spelling.substr(cursor).starts_with(piece.literal))
            return solutions;
          cursor += piece.literal.size();
          continue;
        }

        // Parameter pieces are sliced by the original actual width so a paste
        // like A##B has one deterministic inverse instead of many partitions.
        if (piece.paramIdx >= oldActuals_.size())
          return solutions;
        const size_t width =
            StringRef(oldActuals_[piece.paramIdx]).trim().size();
        if (cursor + width > spelling.size())
          return solutions;
        if (!AssignSolvedActual(cur, piece.paramIdx,
                                spelling.slice(cursor, cursor + width)))
          return solutions;
        cursor += width;
      }
      if (cursor == spelling.size())
        solutions.push_back(std::move(cur));
      return solutions;
    }

    GeneratedSolvedActuals start = seed;
    DfsPaste(pieces, spelling, 0, 0, start, solutions);
    return solutions;
  }

private:
  /// DFSes paste-piece assignments in left-to-right piece order.
  ///
  /// Literal pieces must match exactly at the current cursor.  Parameter pieces
  /// enumerate candidate slices in increasing end-offset order.  Search stops
  /// after two solutions because the caller only accepts unique inversions.
  void DfsPaste(ArrayRef<GeneratedPastePiece> pieces, StringRef spelling,
                size_t pieceIdx, size_t cursor, GeneratedSolvedActuals &cur,
                SmallVectorImpl<GeneratedSolvedActuals> &solutions) const {
    if (solutions.size() > 1)
      return;

    if (pieceIdx == pieces.size()) {
      if (cursor == spelling.size())
        solutions.push_back(cur);
      return;
    }

    const GeneratedPastePiece &piece = pieces[pieceIdx];
    if (!piece.isParam) {
      // Literal paste pieces are anchors: they consume exactly their spelling
      // and do not introduce alternate split points.
      if (spelling.substr(cursor).starts_with(piece.literal))
        DfsPaste(pieces, spelling, pieceIdx + 1,
                 cursor + piece.literal.size(), cur, solutions);
      return;
    }

    // Parameter pieces enumerate slices in deterministic increasing-end order.
    // Ambiguity is preserved by retaining at most two solutions.
    for (size_t end = cursor; end <= spelling.size(); ++end) {
      GeneratedSolvedActuals next = cur;
      if (!AssignSolvedActual(next, piece.paramIdx, spelling.slice(cursor, end)))
        continue;
      DfsPaste(pieces, spelling, pieceIdx + 1, end, next, solutions);
      if (solutions.size() > 1)
        return;
    }
  }

  ArrayRef<std::string> oldActuals_;
  const clang::LangOptions &lexLang_;
};

/// Solves a generated-callee replay pattern against one expansion surface.
///
/// The replay pattern and old actual vector are produced by earlier structural
/// proof stages and are treated as trusted inputs.  This resolver owns the
/// recursive token-position DFS, string-literal inversion, paste-token solving,
/// and the unique-solution ambiguity cutoff.  It preserves replay-element order
/// and B-side token scan order exactly: literals advance one token, parameters
/// enumerate end positions from the current token through the suffix, and paste
/// solutions are replayed in the order produced by the paste solver.
class FormalActualConstraintSolver {
public:
  FormalActualConstraintSolver(ArrayRef<GeneratedReplayElem> replayPattern,
                               ArrayRef<std::string> oldActuals,
                               const clang::LangOptions &lexLang)
      : replayPattern_(replayPattern), oldActuals_(oldActuals),
        lexLang_(lexLang), pasteSolver_(oldActuals, lexLang) {}

  /// Solves the replay pattern against `expansion`, if the solution is unique.
  ///
  /// The expansion is lexed once, then the DFS attempts to bind formal actuals
  /// while preserving replay-element order.  Zero or multiple solutions fail
  /// closed by returning nullopt.
  std::optional<GeneratedSolvedActuals> SolveExpansion(StringRef expansion) const {
    SmallVector<ReplayTok, 32> toks;
    lexGeneratedCalleeReplayTokens(expansion, lexLang_, toks);

    SmallVector<GeneratedSolvedActuals, 4> solutions;
    GeneratedSolvedActuals seed;
    for (const std::string &actual : oldActuals_)
      seed.push_back(actual);

    Dfs(expansion, toks, replayPattern_, 0, seed, solutions);
    if (solutions.size() != 1)
      return std::nullopt;
    return solutions.front();
  }

private:
  /// DFSes replay elements against the lexed expansion token stream.
  ///
  /// Literal and stringification elements consume exactly one token.  Parameter
  /// elements enumerate suffix end positions in increasing order.  Paste
  /// elements delegate one-token inversion to the paste solver.  Search stops
  /// after two solutions because only a unique assignment is admissible.
  void Dfs(StringRef expansion, ArrayRef<ReplayTok> toks,
           ArrayRef<GeneratedReplayElem> elems, size_t tokPos,
           GeneratedSolvedActuals &cur,
           SmallVectorImpl<GeneratedSolvedActuals> &solutions) const {
    if (solutions.size() > 1)
      return;

    // Reaching the end of the replay pattern is successful only if the B-side
    // token stream was consumed exactly.
    if (elems.empty()) {
      if (tokPos == toks.size())
        solutions.push_back(cur);
      return;
    }

    const GeneratedReplayElem &elem = elems.front();
    ArrayRef<GeneratedReplayElem> rest = elems.drop_front();
    switch (elem.kind) {
    case GeneratedReplayKind::Literal:
      // Literal replay elements are fixed anchors: they match exactly one
      // expansion token and introduce no alternate bindings.
      if (tokPos < toks.size() && toks[tokPos].spelling == elem.literal)
        Dfs(expansion, toks, rest, tokPos + 1, cur, solutions);
      return;

    case GeneratedReplayKind::Param: {
      // Parameter replay elements may cover any token suffix beginning at the
      // current position, including the empty slice.  Increasing end order is
      // part of the deterministic replay proof and must be preserved.
      for (size_t end = tokPos; end <= toks.size(); ++end) {
        StringRef value;
        if (end > tokPos) {
          const size_t byteBegin = toks[tokPos].begin;
          const size_t byteEnd = toks[end - 1].end;
          value = expansion.slice(byteBegin, byteEnd);
        }

        GeneratedSolvedActuals next = cur;
        if (!pasteSolver_.AssignSolvedActual(next, elem.paramIdx, value))
          continue;

        Dfs(expansion, toks, rest, end, next, solutions);
        if (solutions.size() > 1)
          return;
      }
      return;
    }

    case GeneratedReplayKind::Stringify: {
      // Stringification inverts only a simple string-literal token.  Unsupported
      // string literal forms fail closed instead of being heuristically decoded.
      if (tokPos >= toks.size())
        return;
      std::optional<std::string> content =
          decodeSimpleStringLiteralToken(toks[tokPos].spelling);
      if (!content)
        return;

      GeneratedSolvedActuals next = cur;
      if (!pasteSolver_.AssignSolvedActual(next, elem.paramIdx,
                                           StringRef(*content)))
        return;

      Dfs(expansion, toks, rest, tokPos + 1, next, solutions);
      return;
    }

    case GeneratedReplayKind::Paste: {
      // Paste replay consumes exactly one expansion token, then replays each
      // paste-solver assignment in the solver's deterministic order.
      if (tokPos >= toks.size())
        return;
      SmallVector<GeneratedSolvedActuals, 4> pasteSolutions =
          pasteSolver_.Solve(
              ArrayRef<GeneratedPastePiece>(elem.pastePieces.data(),
                                            elem.pastePieces.size()),
              toks[tokPos].spelling, cur);

      for (GeneratedSolvedActuals &pasteSol : pasteSolutions) {
        Dfs(expansion, toks, rest, tokPos + 1, pasteSol, solutions);
        if (solutions.size() > 1)
          return;
      }
      return;
    }
    }
  }

  ArrayRef<GeneratedReplayElem> replayPattern_;
  ArrayRef<std::string> oldActuals_;
  const clang::LangOptions &lexLang_;
  GeneratedCalleePasteActualSolver pasteSolver_;
};

/// Classifies one token-level element in a tuple generated-callee replay pattern.
enum class TupleCalleeReplayKind {
  /// Literal replacement text.
  Literal,
  /// Formal parameter reference.
  Param,
  /// Stringification of a formal parameter.
  Stringify,
  /// Token-paste expression.
  Paste,
  /// `__VA_OPT__` payload replay.
  VaOpt
};

/// One literal or parameter piece inside a tuple token-paste expression.
struct TupleCalleePastePiece {
  /// Whether this paste piece references a formal parameter.
  bool isParam = false;

  /// Formal parameter index when `isParam` is true.
  uint32_t paramIdx = 0;

  /// Literal spelling when `isParam` is false.
  std::string literal;
};

/// One parsed element of a tuple generated-callee replay pattern.
struct TupleCalleeReplayElem {
  /// Element kind controlling which payload fields are meaningful.
  TupleCalleeReplayKind kind = TupleCalleeReplayKind::Literal;

  /// Literal spelling for literal replay elements.
  std::string literal;

  /// Formal parameter index for parameter or stringification elements.
  uint32_t paramIdx = 0;

  /// Ordered nested replay elements for `__VA_OPT__` payloads.
  std::vector<TupleCalleeReplayElem> children;

  /// Ordered paste pieces for token-paste replay elements.
  std::vector<TupleCalleePastePiece> pastePieces;
};

using TupleSolvedActuals = SmallVector<std::string, 8>;

/// Parses the tuple generated-callee replacement tape without sharing carrier
/// types with the non-tuple generated-callee parser.
///
/// The caller trusts the tuple forwarder proof and the recovered old tuple-slot
/// actuals.  This parser owns the tuple replay syntactic gates: malformed
/// stringification, malformed paste chains, out-of-range formal references,
/// and unsupported `__VA_OPT__` all fail closed.  It preserves replacement-token
/// order and records tuple-specific stringification/paste facts exactly when
/// the local parser did.
class TupleGeneratedCalleeReplayPatternParser {
public:
  TupleGeneratedCalleeReplayPatternParser(
      const RefoldModel::MacroDirective &definition,
      ArrayRef<std::string> oldActuals, bool &usesStringification,
      bool &usesPaste)
      : definition_(definition), oldActuals_(oldActuals),
        usesStringification_(usesStringification), usesPaste_(usesPaste) {}

  /// Parses `begin..end` into ordered tuple generated-callee replay elements.
  ///
  /// Unsupported tuple replay syntax fails closed.  Stringification and paste
  /// facts are set only after the corresponding replay form is accepted.
  bool Parse(size_t begin, size_t end,
             std::vector<TupleCalleeReplayElem> &out) const {
    for (size_t i = begin; i < end;) {
      const auto &tok = definition_.replacementTokens[i];

      // Accept only the canonical stringification form:
      //   # <formal-param>
      // Missing, non-param, and out-of-range operands are not replay-safe.
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

        usesStringification_ = true;
        TupleCalleeReplayElem elem;
        elem.kind = TupleCalleeReplayKind::Stringify;
        elem.paramIdx = paramIdx;
        out.push_back(std::move(elem));
        i += 2;
        continue;
      }

      // A paste replay element begins when the next replacement token is ##.
      // The chain is consumed left-to-right to preserve replacement-tape order.
      if (i + 1 < end &&
          definition_.replacementTokens[i + 1].spelling == "##") {
        usesPaste_ = true;
        TupleCalleeReplayElem elem;
        elem.kind = TupleCalleeReplayKind::Paste;
        TupleCalleePastePiece first;
        if (!PastePieceFromReplacementToken(tok, first))
          return false;
        elem.pastePieces.push_back(std::move(first));
        i += 2;
        while (true) {
          // A trailing ## is malformed because there is no replay-safe right
          // operand to append to the paste chain.
          if (i >= end)
            return false;
          TupleCalleePastePiece next;
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

      // Plain formal references replay as direct old tuple-slot substitutions.
      if (tok.kind == RefoldModel::MacroReplacementTokenKind::ParamRef) {
        if (!tok.paramIndex || *tok.paramIndex >= oldActuals_.size())
          return false;
        TupleCalleeReplayElem elem;
        elem.kind = TupleCalleeReplayKind::Param;
        elem.paramIdx = *tok.paramIndex;
        out.push_back(std::move(elem));
        ++i;
        continue;
      }

      // Standalone paste and __VA_OPT__ are not accepted in this tuple replay
      // parser.  They remain fail-closed rather than being approximated.
      if (tok.spelling == "##")
        return false;
      if (tok.spelling == "__VA_OPT__")
        return false;

      TupleCalleeReplayElem elem;
      elem.kind = TupleCalleeReplayKind::Literal;
      elem.literal = tok.spelling.str();
      out.push_back(std::move(elem));
      ++i;
    }
    return true;
  }

private:
  /// Converts one replacement token into a tuple paste piece.
  ///
  /// Only literals and in-range formal references are accepted.  Operators and
  /// `__VA_OPT__` fail closed because they cannot be replayed as paste operands.
  bool PastePieceFromReplacementToken(
      const RefoldModel::MacroReplacementToken &tok,
      TupleCalleePastePiece &piece) const {
    if (tok.kind == RefoldModel::MacroReplacementTokenKind::ParamRef) {
      if (!tok.paramIndex || *tok.paramIndex >= oldActuals_.size())
        return false;
      piece.isParam = true;
      piece.paramIdx = *tok.paramIndex;
      return true;
    }

    // Do not reinterpret replay operators or VA_OPT as literal paste text.
    if (tok.spelling == "#" || tok.spelling == "##" ||
        tok.spelling == "__VA_OPT__")
      return false;

    piece.isParam = false;
    piece.literal = tok.spelling.str();
    return true;
  }

  const RefoldModel::MacroDirective &definition_;
  ArrayRef<std::string> oldActuals_;
  bool &usesStringification_;
  bool &usesPaste_;
};

/// Resolves tuple-generated replay actuals against old and new expansion text.
///
/// The tuple-specific replay pattern and old actual slots are trusted outputs
/// from the tuple parser and owner proof.  This resolver owns the tuple replay
/// DFS, paste-piece assignment enumeration, and unique-solution ambiguity
/// cutoff.  It intentionally uses tuple carrier types rather than the
/// non-tuple generated-callee carriers, preserves token/end-position ordering,
/// and rejects ambiguous or unsupported `VaOpt` elements fail-closed.
class TupleGeneratedCalleeReplayResolver {
public:
  TupleGeneratedCalleeReplayResolver(
      ArrayRef<TupleCalleeReplayElem> replayPattern,
      ArrayRef<std::string> oldActuals, const clang::LangOptions &lexLang)
      : replayPattern_(replayPattern), oldActuals_(oldActuals),
        lexLang_(lexLang) {}

  /// Solves the tuple replay pattern against `expansion`, if unique.
  ///
  /// The expansion is lexed once, then replay elements bind tuple actuals in
  /// pattern order.  Zero solutions or multiple solutions fail closed.
  std::optional<TupleSolvedActuals> SolveExpansion(StringRef expansion) const {
    SmallVector<ReplayTok, 32> toks;
    lexGeneratedCalleeReplayTokens(expansion, lexLang_, toks);

    SmallVector<TupleSolvedActuals, 4> solutions;
    TupleSolvedActuals seed;
    seed.reserve(oldActuals_.size());
    for (const std::string &actual : oldActuals_)
      seed.push_back(actual);

    Dfs(expansion, toks, replayPattern_, 0, seed, solutions);
    if (solutions.size() != 1)
      return std::nullopt;
    return solutions.front();
  }

private:
  /// Assigns one tuple actual while preserving compatible prior bindings.
  ///
  /// A slot still equal to its old actual may be refined to `value`.  A slot
  /// already refined must remain token-equivalent to `value`.
  bool AssignSolvedActual(TupleSolvedActuals &actuals, uint32_t paramIdx,
                          StringRef value) const {
    if (paramIdx >= actuals.size())
      return false;

    // Compare by token equivalence so harmless spelling differences do not
    // create false conflicts during tuple replay inversion.
    if (!generatedCalleeTextsTokenEquivalent(actuals[paramIdx],
                                             oldActuals_[paramIdx], lexLang_) &&
        !generatedCalleeTextsTokenEquivalent(actuals[paramIdx], value,
                                             lexLang_))
      return false;

    // The old actual spelling is the unassigned sentinel for this slot.
    if (generatedCalleeTextsTokenEquivalent(actuals[paramIdx],
                                            oldActuals_[paramIdx], lexLang_)) {
      actuals[paramIdx] = value.trim().str();
      return true;
    }
    return generatedCalleeTextsTokenEquivalent(actuals[paramIdx], value,
                                               lexLang_);
  }

  /// Enumerates replay-safe assignments for one tuple pasted token.
  ///
  /// Literal-anchored paste forms use DFS over candidate slices.  Paste forms
  /// without a literal anchor keep the deterministic old-width inverse.
  SmallVector<TupleSolvedActuals, 4>
  SolvePasteToken(ArrayRef<TupleCalleePastePiece> pieces, StringRef spelling,
                  const TupleSolvedActuals &seed) const {
    SmallVector<TupleSolvedActuals, 4> solutions;

    const bool hasLiteralAnchor =
        llvm::any_of(pieces, [](const TupleCalleePastePiece &piece) {
          return !piece.isParam && !piece.literal.empty();
        });

    // Without a fixed literal anchor, preserve the tuple solver's prior
    // deterministic inverse: original actual widths, not arbitrary cuts.
    if (!hasLiteralAnchor) {
      TupleSolvedActuals cur = seed;
      size_t cursor = 0;
      for (const TupleCalleePastePiece &piece : pieces) {
        if (!piece.isParam) {
          if (!spelling.substr(cursor).starts_with(piece.literal))
            return solutions;
          cursor += piece.literal.size();
          continue;
        }

        // Parameter pieces use the original actual width so unanchored paste
        // replay has one deterministic partition instead of many candidates.
        if (piece.paramIdx >= oldActuals_.size())
          return solutions;
        const size_t width =
            StringRef(oldActuals_[piece.paramIdx]).trim().size();
        if (cursor + width > spelling.size())
          return solutions;
        if (!AssignSolvedActual(cur, piece.paramIdx,
                                spelling.slice(cursor, cursor + width)))
          return solutions;
        cursor += width;
      }
      if (cursor == spelling.size())
        solutions.push_back(std::move(cur));
      return solutions;
    }

    TupleSolvedActuals start = seed;
    DfsPaste(pieces, spelling, 0, 0, start, solutions);
    return solutions;
  }

  /// DFSes tuple paste pieces in left-to-right order.
  ///
  /// Literal pieces consume fixed text.  Parameter pieces enumerate slices in
  /// increasing end-offset order.  Search stops after two solutions.
  void DfsPaste(ArrayRef<TupleCalleePastePiece> pieces, StringRef spelling,
                size_t pieceIdx, size_t cursor, TupleSolvedActuals &cur,
                SmallVectorImpl<TupleSolvedActuals> &solutions) const {
    if (solutions.size() > 1)
      return;

    if (pieceIdx == pieces.size()) {
      if (cursor == spelling.size())
        solutions.push_back(cur);
      return;
    }

    const TupleCalleePastePiece &piece = pieces[pieceIdx];
    if (!piece.isParam) {
      // Literal paste pieces are fixed anchors and do not introduce alternate
      // split points.
      if (spelling.substr(cursor).starts_with(piece.literal))
        DfsPaste(pieces, spelling, pieceIdx + 1,
                 cursor + piece.literal.size(), cur, solutions);
      return;
    }

    // Parameter slices are tried in deterministic increasing-end order.
    // Retaining at most two solutions preserves the ambiguity cutoff.
    for (size_t end = cursor; end <= spelling.size(); ++end) {
      StringRef slice = spelling.slice(cursor, end);
      TupleSolvedActuals next = cur;
      if (!AssignSolvedActual(next, piece.paramIdx, slice))
        continue;
      DfsPaste(pieces, spelling, pieceIdx + 1, end, next, solutions);
      if (solutions.size() > 1)
        return;
    }
  }

  /// DFSes tuple replay elements against the lexed expansion token stream.
  ///
  /// Literals and stringification consume one token.  Parameters enumerate token
  /// suffixes in increasing end order.  Paste consumes one token through the
  /// tuple paste solver.  `VaOpt` remains unsupported and fails closed.
  void Dfs(StringRef expansion, ArrayRef<ReplayTok> toks,
           ArrayRef<TupleCalleeReplayElem> elems, size_t tokPos,
           TupleSolvedActuals &cur,
           SmallVectorImpl<TupleSolvedActuals> &solutions) const {
    if (solutions.size() > 1)
      return;

    // The replay pattern is successful only when it consumes the entire B-side
    // token stream.
    if (elems.empty()) {
      if (tokPos == toks.size())
        solutions.push_back(cur);
      return;
    }

    const TupleCalleeReplayElem &elem = elems.front();
    ArrayRef<TupleCalleeReplayElem> rest = elems.drop_front();
    switch (elem.kind) {
    case TupleCalleeReplayKind::Literal:
      // Literal replay elements are fixed token anchors.
      if (tokPos < toks.size() && toks[tokPos].spelling == elem.literal)
        Dfs(expansion, toks, rest, tokPos + 1, cur, solutions);
      return;

    case TupleCalleeReplayKind::Param: {
      // Parameter replay elements may bind any token suffix from the current
      // position, including the empty slice.
      for (size_t end = tokPos; end <= toks.size(); ++end) {
        StringRef value;
        if (end > tokPos) {
          const size_t byteBegin = toks[tokPos].begin;
          const size_t byteEnd = toks[end - 1].end;
          value = expansion.slice(byteBegin, byteEnd);
        }
        TupleSolvedActuals next = cur;
        if (!AssignSolvedActual(next, elem.paramIdx, value))
          continue;
        Dfs(expansion, toks, rest, end, next, solutions);
        if (solutions.size() > 1)
          return;
      }
      return;
    }

    case TupleCalleeReplayKind::Stringify: {
      // Stringification inversion accepts only simple string-literal tokens.
      if (tokPos >= toks.size())
        return;
      std::optional<std::string> content =
          decodeSimpleStringLiteralToken(toks[tokPos].spelling);
      if (!content)
        return;
      TupleSolvedActuals next = cur;
      if (!AssignSolvedActual(next, elem.paramIdx, StringRef(*content)))
        return;
      Dfs(expansion, toks, rest, tokPos + 1, next, solutions);
      return;
    }

    case TupleCalleeReplayKind::Paste: {
      // Tuple paste replay consumes exactly one expansion token and preserves
      // the paste solver's assignment order.
      if (tokPos >= toks.size())
        return;
      SmallVector<TupleSolvedActuals, 4> pasteSolutions = SolvePasteToken(
          ArrayRef<TupleCalleePastePiece>(elem.pastePieces.data(),
                                          elem.pastePieces.size()),
          toks[tokPos].spelling, cur);
      for (TupleSolvedActuals &pasteSol : pasteSolutions) {
        Dfs(expansion, toks, rest, tokPos + 1, pasteSol, solutions);
        if (solutions.size() > 1)
          return;
      }
      return;
    }

    case TupleCalleeReplayKind::VaOpt:
      // Tuple generated-callee replay does not currently prove VA_OPT payloads.
      return;
    }
  }

  ArrayRef<TupleCalleeReplayElem> replayPattern_;
  ArrayRef<std::string> oldActuals_;
  const clang::LangOptions &lexLang_;
};

} // namespace

RefoldMacroGeneratedCalleeReplayEngine::RefoldMacroGeneratedCalleeReplayEngine(
    Dependencies deps)
    : deps_(std::move(deps)) {}

bool RefoldMacroGeneratedCalleeReplayEngine::
    GeneratedCalleeReplayPreservesEnvelope(
        const GeneratedCalleeReplayContext &ctx) const {
  return ctx.wholeCoverATokens.first < ctx.wholeCoverATokens.second &&
         ctx.bTokenEnvelope.first < ctx.bTokenEnvelope.second;
}

bool RefoldMacroGeneratedCalleeReplayEngine::GeneratedCalleeReplayIsAdmissible(
    const GeneratedCalleeReplayContext &ctx) const {
  return ctx.followedGeneratedCall && ctx.currentDefinition &&
         !ctx.currentActuals.empty() &&
         macroDefinitionAcceptsActualCount(*ctx.currentDefinition,
                                           ctx.currentActuals.size());
}

std::optional<MacroPatch>
RefoldMacroGeneratedCalleeReplayEngine::BuildGeneratedCalleeReplayCandidate(
    const GeneratedCalleeReplayContext &generatedCalleeCtx) const {
  // Keep the same fail-closed entry gates after the caller has computed the
  // owner cover and B-token envelope.
  if (!GeneratedCalleeReplayPreservesEnvelope(generatedCalleeCtx))
    return std::nullopt;

  const RefoldModel::MacroInvocation &m = generatedCalleeCtx.invocation;
  StringRef baseInvText = generatedCalleeCtx.baseInvocationText;
  ArrayRef<std::pair<size_t, size_t>> invArgRanges =
      generatedCalleeCtx.invocationArgRanges;
  const std::pair<uint64_t, uint64_t> *cover =
      &generatedCalleeCtx.wholeCoverATokens;
  const std::pair<size_t, size_t> *bEnv = &generatedCalleeCtx.bTokenEnvelope;
  InvocationActualRecoveryContext actualRecoveryCtx{m, baseInvText,
                                                    invArgRanges};

  auto findGeneratedCallShape =
      [&](const RefoldModel::MacroDirective &definition)
      -> std::optional<GeneratedCalleeCallShape> {
    const auto &toks = definition.replacementTokens;
    if (toks.empty())
      return std::nullopt;

    auto isLiteralToken = [&](size_t idx, StringRef spelling) {
      return idx < toks.size() &&
             toks[idx].kind ==
                 RefoldModel::MacroReplacementTokenKind::Literal &&
             toks[idx].spelling == spelling;
    };

    auto findMatchingParen = [&](size_t openIdx,
                                 size_t limit) -> std::optional<size_t> {
      if (!isLiteralToken(openIdx, "("))
        return std::nullopt;
      unsigned depth = 1;
      for (size_t i = openIdx + 1; i < limit; ++i) {
        if (toks[i].kind != RefoldModel::MacroReplacementTokenKind::Literal)
          continue;
        if (toks[i].spelling == "(") {
          ++depth;
          continue;
        }
        if (toks[i].spelling != ")")
          continue;
        if (--depth == 0)
          return i;
      }
      return std::nullopt;
    };

    auto collectTopLevelArgRanges =
        [&](size_t openIdx, size_t closeIdx,
            SmallVectorImpl<std::pair<size_t, size_t>> &out) {
          if (openIdx >= closeIdx)
            return false;
          size_t argBegin = openIdx + 1;
          unsigned argDepth = 0;
          for (size_t i = openIdx + 1; i <= closeIdx; ++i) {
            const bool atEnd = i == closeIdx;
            if (!atEnd) {
              const auto &tok = toks[i];
              if (tok.kind == RefoldModel::MacroReplacementTokenKind::Literal) {
                if (tok.spelling == "(") {
                  ++argDepth;
                } else if (tok.spelling == ")") {
                  if (argDepth == 0)
                    return false;
                  --argDepth;
                }
              }
            }

            if (atEnd || (argDepth == 0 &&
                          toks[i].kind ==
                              RefoldModel::MacroReplacementTokenKind::Literal &&
                          toks[i].spelling == ",")) {
              if (argBegin == i)
                return false;
              out.push_back({argBegin, i});
              argBegin = i + 1;
            }
          }
          return !out.empty();
        };

    // Resolve generated-callee owner ranges with the named selector resolver.
    // The surrounding shape scan still owns call ordering and ambiguity checks;
    // the resolver only proves that a candidate callee expression maps to one
    // deterministic root formal or fails closed.
    const SelectorCalleeResolver selectorResolver(deps_, definition);

    bool found = false;
    size_t callBegin = toks.size();
    size_t callOpen = toks.size();
    size_t callClose = toks.size();
    uint32_t calleeParamIdx = 0;

    for (size_t open = 1; open < toks.size(); ++open) {
      if (!isLiteralToken(open, "("))
        continue;

      auto close = findMatchingParen(open, toks.size());
      if (!close)
        return std::nullopt;

      std::optional<size_t> matchedBegin;
      std::optional<uint32_t> matchedParam;
      for (size_t begin = 0; begin < open; ++begin) {
        auto resolved = selectorResolver.Resolve(begin, open, 0);
        if (!resolved)
          continue;
        if (matchedBegin)
          return std::nullopt;
        matchedBegin = begin;
        matchedParam = *resolved;
      }
      if (!matchedBegin)
        continue;

      if (found)
        return std::nullopt;
      found = true;
      callBegin = *matchedBegin;
      callOpen = open;
      callClose = *close;
      calleeParamIdx = *matchedParam;
      // Nested generated calls inside this call's arguments are argument
      // expressions, not competing owner calls.  Skip the body after
      // recording the outer call so shapes such as `H(G(X))` remain one
      // generated call whose first actual is the expression `G(X)`.
      open = *close;
    }

    if (!found || calleeParamIdx >= definition.defParams.size())
      return std::nullopt;

    auto collectLiteralContext = [&](size_t begin, size_t end,
                                     SmallVectorImpl<std::string> &out) {
      for (size_t i = begin; i < end; ++i) {
        const auto &tok = toks[i];
        if (tok.kind != RefoldModel::MacroReplacementTokenKind::Literal ||
            tok.spelling == "#" || tok.spelling == "##" ||
            tok.spelling == "__VA_OPT__")
          return false;
        out.push_back(tok.spelling.str());
      }
      return true;
    };

    GeneratedCalleeCallShape shape;
    shape.calleeParamIdx = calleeParamIdx;
    if (!collectLiteralContext(0, callBegin, shape.prefixLiterals) ||
        !collectLiteralContext(callClose + 1, toks.size(),
                               shape.suffixLiterals)) {
      return std::nullopt;
    }

    if (!collectTopLevelArgRanges(callOpen, callClose, shape.argTokenRanges))
      return std::nullopt;
    return shape;
  };

  auto splitVariadicPackSourceSlot =
      [&](const GeneratedCalleeSourceSlot &slot,
          SmallVectorImpl<GeneratedCalleeSourceSlot> &out) {
        SmallVector<RefoldLexBoundaryToken, 32> toks;
        refoldLexBoundaryTokens(StringRef(slot.text), deps_.lexLang, toks);

        size_t elemBegin = 0;
        int parenDepth = 0;
        bool sawComma = false;
        SmallVector<std::pair<size_t, size_t>, 8> pieces;

        for (const RefoldLexBoundaryToken &tok : toks) {
          if (tok.spelling == "(") {
            ++parenDepth;
            continue;
          }
          if (tok.spelling == ")") {
            if (parenDepth > 0)
              --parenDepth;
            continue;
          }
          if (tok.spelling != "," || parenDepth != 0)
            continue;

          sawComma = true;
          StringRef elem =
              StringRef(slot.text).slice(elemBegin, tok.begin).trim();
          if (elem.empty())
            return false;
          pieces.push_back(
              {static_cast<size_t>(elem.data() - slot.text.data()),
               static_cast<size_t>(elem.data() - slot.text.data()) +
                   elem.size()});
          elemBegin = tok.end;
        }

        if (!sawComma) {
          out.push_back(slot);
          return true;
        }

        StringRef finalElem = StringRef(slot.text).drop_front(elemBegin).trim();
        if (finalElem.empty())
          return false;
        pieces.push_back(
            {static_cast<size_t>(finalElem.data() - slot.text.data()),
             static_cast<size_t>(finalElem.data() - slot.text.data()) +
                 finalElem.size()});

        // A variadic formal can be forwarded into a fixed-arity generated
        // callee. The producer records the root variadic tail as one invocation
        // argument range (`foo, bar, baz`), but substituting `__VA_ARGS__` into
        // a generated call exposes those comma-separated elements as positional
        // actuals. Split only at commas that macro argument collection would
        // see: nested parentheses protect commas, while brackets/braces
        // intentionally do not. Each piece keeps the whole root variadic source
        // as its rewrite owner so multiple solved final parameters can be
        // composed back into one root argument replacement.
        for (const auto &piece : pieces) {
          GeneratedCalleeSourceSlot split = slot;
          split.text =
              StringRef(slot.text).slice(piece.first, piece.second).str();
          split.rootSourceText = slot.rootSourceText;
          split.rootArgIdx = slot.rootArgIdx;
          out.push_back(std::move(split));
        }
        return true;
      };

  auto appendActualsForParam =
      [&](const RefoldModel::MacroDirective &definition,
          ArrayRef<GeneratedCalleeSourceSlot> actuals, uint32_t paramIdx,
          SmallVectorImpl<GeneratedCalleeSourceSlot> &out) {
        if (paramIdx >= definition.defParams.size())
          return false;
        if (!isMacroDirectiveVariadicParam(definition, paramIdx)) {
          if (paramIdx >= actuals.size())
            return false;
          out.push_back(actuals[paramIdx]);
          return true;
        }
        generatedCalleeCtx.usesVariadicForwarding = true;
        if (actuals.size() < paramIdx)
          return false;

        // If this variadic parameter is still represented by one root
        // invocation range, distribute the pack now.  If it has already been
        // distributed by an earlier generated-call step, preserve the
        // existing positional slots.
        if (actuals.size() == paramIdx + 1)
          return splitVariadicPackSourceSlot(actuals[paramIdx], out);

        for (size_t i = paramIdx; i < actuals.size(); ++i)
          out.push_back(actuals[i]);
        return true;
      };

  auto findEditableParamInGeneratedArgument =
      [&](const RefoldModel::MacroDirective &definition,
          ArrayRef<GeneratedCalleeSourceSlot> actuals, size_t begin,
          size_t end) -> std::optional<GeneratedCalleeSourceSlot> {
    std::optional<GeneratedCalleeSourceSlot> editable;
    const auto &toks = definition.replacementTokens;
    for (size_t i = begin; i < end; ++i) {
      const auto &tok = toks[i];
      if (tok.kind != RefoldModel::MacroReplacementTokenKind::ParamRef)
        continue;
      if (!tok.paramIndex || *tok.paramIndex >= definition.defParams.size())
        return std::nullopt;
      if (isMacroDirectiveVariadicParam(definition, *tok.paramIndex))
        return std::nullopt;
      if (*tok.paramIndex >= actuals.size())
        return std::nullopt;

      // A function-like macro actual immediately followed by `(` is a
      // generated callee selector inside this argument expression, not the
      // source slot whose value should be edited.  Treat it as fixed proof
      // context and keep looking for the ordinary data argument.
      const bool selectorPosition =
          i + 1 < end &&
          toks[i + 1].kind == RefoldModel::MacroReplacementTokenKind::Literal &&
          toks[i + 1].spelling == "(" &&
          deps_.resolveFunctionLikeMacroForReplay(
              StringRef(actuals[*tok.paramIndex].text));
      if (selectorPosition)
        continue;

      const GeneratedCalleeSourceSlot &slot = actuals[*tok.paramIndex];
      if (editable) {
        // A generated actual expression with multiple distinct data leaves
        // (for example `A ## B` or `G(A, B)`) is not a single final-callee
        // source slot.  Let the generated-leaf pattern solver below invert
        // the whole literal skeleton and compose all root leaves together;
        // accepting it here would greedily rewrite the first leaf to the
        // complete joined token, e.g. `foo, <empty>` -> `foobar, <empty>`.
        return std::nullopt;
      }
      editable = slot;
    }
    return editable;
  };

  auto instantiateGeneratedArgument =
      [&](const RefoldModel::MacroDirective &definition,
          ArrayRef<GeneratedCalleeSourceSlot> actuals, size_t begin, size_t end,
          SmallVectorImpl<GeneratedCalleeSourceSlot> &out) {
        const auto &toks = definition.replacementTokens;
        if (begin >= end)
          return false;

        // `__VA_ARGS__` as a complete generated actual distributes the
        // variadic pack positionally into the next generated callee.  This is
        // the same owner proof as ordinary argument replay; the only extra
        // obligation is that each pack element keeps its own root source
        // slot.
        if (end == begin + 1 &&
            toks[begin].kind ==
                RefoldModel::MacroReplacementTokenKind::ParamRef &&
            toks[begin].paramIndex &&
            isMacroDirectiveVariadicParam(definition, *toks[begin].paramIndex))
          return appendActualsForParam(definition, actuals,
                                       *toks[begin].paramIndex, out);

        // Active `__VA_OPT__(, __VA_ARGS__)` in a generated-call argument
        // list contributes additional positional arguments rather than bytes
        // inside the preceding argument.  Accept only the canonical
        // comma-plus-variadic form here; anything more complex remains
        // outside this proof.
        for (size_t i = begin; i < end; ++i) {
          if (toks[i].spelling != "__VA_OPT__")
            continue;
          if (i != begin + 1 || begin >= end ||
              toks[begin].kind !=
                  RefoldModel::MacroReplacementTokenKind::ParamRef ||
              !toks[begin].paramIndex || i + 5 != end ||
              toks[i + 1].spelling != "(" || toks[i + 2].spelling != "," ||
              toks[i + 3].kind !=
                  RefoldModel::MacroReplacementTokenKind::ParamRef ||
              !toks[i + 3].paramIndex ||
              !isMacroDirectiveVariadicParam(definition,
                                             *toks[i + 3].paramIndex) ||
              toks[i + 4].spelling != ")")
            return false;
          if (!appendActualsForParam(definition, actuals,
                                     *toks[begin].paramIndex, out))
            return false;
          return appendActualsForParam(definition, actuals,
                                       *toks[i + 3].paramIndex, out);
        }

        std::optional<GeneratedCalleeSourceSlot> editable =
            findEditableParamInGeneratedArgument(definition, actuals, begin,
                                                 end);
        if (!editable)
          return false;

        std::string text;
        for (size_t i = begin; i < end; ++i) {
          const auto &tok = toks[i];
          if (tok.spelling == "##") {
            generatedCalleeCtx.usesPaste = true;
            continue;
          }
          if (tok.spelling == "#") {
            generatedCalleeCtx.usesStringification = true;
            if (i + 1 >= end ||
                toks[i + 1].kind !=
                    RefoldModel::MacroReplacementTokenKind::ParamRef ||
                !toks[i + 1].paramIndex ||
                *toks[i + 1].paramIndex >= definition.defParams.size() ||
                isMacroDirectiveVariadicParam(definition,
                                              *toks[i + 1].paramIndex) ||
                *toks[i + 1].paramIndex >= actuals.size())
              return false;
            // A stringified generated argument is still an expression over
            // the same root source slot.  Materialize the old string-literal
            // spelling for replay, but keep the editable owner as the
            // unstringified source argument so the solved value rewrites `X`,
            // not `#X` or `"X"`.
            text.push_back('"');
            text += actuals[*toks[i + 1].paramIndex].text;
            text.push_back('"');
            ++i;
            continue;
          }
          if (tok.spelling == "__VA_OPT__")
            return false;
          if (tok.kind == RefoldModel::MacroReplacementTokenKind::ParamRef) {
            if (!tok.paramIndex ||
                *tok.paramIndex >= definition.defParams.size() ||
                isMacroDirectiveVariadicParam(definition, *tok.paramIndex) ||
                *tok.paramIndex >= actuals.size())
              return false;
            text += actuals[*tok.paramIndex].text;
            continue;
          }
          text += tok.spelling.str();
        }

        GeneratedCalleeSourceSlot slot;
        slot.text = std::move(text);
        slot.rootSourceText = editable->rootSourceText;
        slot.rootArgIdx = editable->rootArgIdx;
        out.push_back(std::move(slot));
        return true;
      };

  for (size_t depth = 0; depth <= deps_.model.GetMacroDirectives().size();
       ++depth) {
    auto shape = findGeneratedCallShape(*generatedCalleeCtx.currentDefinition);
    if (!shape)
      break;
    if (shape->calleeParamIdx >= generatedCalleeCtx.currentActuals.size())
      return std::nullopt;

    uint32_t nextAliasHops = 0;
    const RefoldModel::MacroDirective *nextDefinition =
        deps_.resolveFunctionLikeMacroThroughAliasesWithHops(
            StringRef(
                generatedCalleeCtx.currentActuals[shape->calleeParamIdx].text),
            &nextAliasHops);
    generatedCalleeCtx.objectAliasHopCount += nextAliasHops;
    if (!nextDefinition || nextDefinition->defParams.empty())
      return std::nullopt;

    SmallVector<GeneratedCalleeSourceSlot, 8> nextActuals;
    for (const auto &argRange : shape->argTokenRanges) {
      if (!instantiateGeneratedArgument(*generatedCalleeCtx.currentDefinition,
                                        generatedCalleeCtx.currentActuals,
                                        argRange.first, argRange.second,
                                        nextActuals))
        return std::nullopt;
    }

    if (!macroDefinitionAcceptsActualCount(*nextDefinition, nextActuals.size()))
      return std::nullopt;

    generatedCalleeCtx.replayPrefixLiterals.append(
        shape->prefixLiterals.begin(), shape->prefixLiterals.end());
    generatedCalleeCtx.replaySuffixStack.push_back(shape->suffixLiterals);
    generatedCalleeCtx.currentDefinition = nextDefinition;
    generatedCalleeCtx.currentActuals = std::move(nextActuals);
    generatedCalleeCtx.followedGeneratedCall = true;
    ++generatedCalleeCtx.generatedCallDepth;
  }

  if (!GeneratedCalleeReplayIsAdmissible(generatedCalleeCtx))
    return std::nullopt;

  const bool finalHasVariadic =
      !generatedCalleeCtx.currentDefinition->defParams.empty() &&
      generatedCalleeCtx.currentDefinition->defParams.back().variadic;
  const size_t finalFixed =
      finalHasVariadic
          ? generatedCalleeCtx.currentDefinition->defParams.size() - 1
          : generatedCalleeCtx.currentDefinition->defParams.size();

  SmallVector<std::string, 8> oldActuals;
  SmallVector<uint32_t, 8> rootSlotByFinalParam;
  SmallVector<std::string, 8> rootSourceByFinalParam;
  for (size_t i = 0; i < finalFixed; ++i) {
    oldActuals.push_back(generatedCalleeCtx.currentActuals[i].text);
    rootSlotByFinalParam.push_back(
        generatedCalleeCtx.currentActuals[i].rootArgIdx);
    rootSourceByFinalParam.push_back(
        generatedCalleeCtx.currentActuals[i].rootSourceText);
  }
  if (finalHasVariadic) {
    std::string variadicText;
    raw_string_ostream os(variadicText);
    for (size_t i = finalFixed; i < generatedCalleeCtx.currentActuals.size();
         ++i) {
      if (i != finalFixed)
        os << ", ";
      os << StringRef(generatedCalleeCtx.currentActuals[i].text).trim();
    }
    os.flush();
    oldActuals.push_back(std::move(variadicText));
    rootSlotByFinalParam.push_back(
        generatedCalleeCtx.currentActuals[finalFixed].rootArgIdx);
    rootSourceByFinalParam.push_back(
        generatedCalleeCtx.currentActuals[finalFixed].rootSourceText);
  }
  if (oldActuals.size() !=
          generatedCalleeCtx.currentDefinition->defParams.size() ||
      rootSlotByFinalParam.size() != oldActuals.size() ||
      rootSourceByFinalParam.size() != oldActuals.size())
    return std::nullopt;

  std::vector<GeneratedReplayElem> finalPattern;
  const GeneratedCalleeReplayPatternParser finalPatternParser(
      *generatedCalleeCtx.currentDefinition,
      ArrayRef<std::string>(oldActuals.data(), oldActuals.size()),
      generatedCalleeCtx.usesStringification, generatedCalleeCtx.usesPaste);
  if (!finalPatternParser.Parse(
          0, generatedCalleeCtx.currentDefinition->replacementTokens.size(),
          finalPattern) ||
      finalPattern.empty())
    return std::nullopt;

  std::vector<GeneratedReplayElem> replayPattern;
  for (const std::string &literal : generatedCalleeCtx.replayPrefixLiterals) {
    GeneratedReplayElem elem;
    elem.kind = GeneratedReplayKind::Literal;
    elem.literal = literal;
    replayPattern.push_back(std::move(elem));
  }
  replayPattern.insert(replayPattern.end(), finalPattern.begin(),
                       finalPattern.end());
  for (auto it = generatedCalleeCtx.replaySuffixStack.rbegin();
       it != generatedCalleeCtx.replaySuffixStack.rend(); ++it) {
    for (const std::string &literal : *it) {
      GeneratedReplayElem elem;
      elem.kind = GeneratedReplayKind::Literal;
      elem.literal = literal;
      replayPattern.push_back(std::move(elem));
    }
  }

  const FormalActualConstraintSolver formalSolver(
      ArrayRef<GeneratedReplayElem>(replayPattern.data(), replayPattern.size()),
      ArrayRef<std::string>(oldActuals.data(), oldActuals.size()), deps_.lexLang);

  StringRef oldExpansion =
      deps_.sourceMapper.SliceASource(cover->first, cover->second).trim();
  StringRef newExpansion =
      deps_.sourceMapper.SliceBSource(bEnv->first, bEnv->second).trim();
  std::optional<GeneratedSolvedActuals> oldSolved =
      formalSolver.SolveExpansion(oldExpansion);
  std::optional<GeneratedSolvedActuals> newSolved =
      formalSolver.SolveExpansion(newExpansion);
  if (!oldSolved || !newSolved || oldSolved->size() != oldActuals.size() ||
      newSolved->size() != oldActuals.size())
    return std::nullopt;

  auto rewriteSourceActualFromSolvedExpansion =
      [&](StringRef source, StringRef oldText,
          StringRef newText) -> std::optional<std::string> {
    oldText = oldText.trim();
    newText = newText.trim();
    SmallVector<RefoldLexBoundaryToken, 16> sourceToks;
    SmallVector<RefoldLexBoundaryToken, 16> oldToks;
    SmallVector<RefoldLexBoundaryToken, 16> newToks;
    refoldLexBoundaryTokens(source, deps_.lexLang, sourceToks);
    refoldLexBoundaryTokens(oldText, deps_.lexLang, oldToks);
    refoldLexBoundaryTokens(newText, deps_.lexLang, newToks);
    if (!oldToks.empty() && oldToks.size() == newToks.size() &&
        sourceToks.size() >= oldToks.size()) {
      std::optional<size_t> matchBegin;
      bool ambiguous = false;
      for (size_t i = 0; i + oldToks.size() <= sourceToks.size(); ++i) {
        bool same = true;
        for (size_t j = 0; j < oldToks.size(); ++j) {
          if (sourceToks[i + j].spelling != oldToks[j].spelling) {
            same = false;
            break;
          }
        }
        if (!same)
          continue;
        if (matchBegin) {
          ambiguous = true;
          break;
        }
        matchBegin = i;
      }
      if (matchBegin && !ambiguous) {
        std::string rewritten = source.str();
        for (size_t j = oldToks.size(); j > 0; --j) {
          const size_t idx = *matchBegin + j - 1;
          rewritten = stringutils::replaceRange(
              rewritten, sourceToks[idx].begin, sourceToks[idx].end,
              newToks[j - 1].spelling);
        }
        return rewritten;
      }
    }
    if (source.trim() == oldText)
      return newText.str();
    size_t pos = source.find(oldText);
    if (pos != StringRef::npos) {
      if (source.find(oldText, pos + 1) != StringRef::npos)
        return std::nullopt;
      return stringutils::replaceRange(source.str(), pos, pos + oldText.size(),
                                       newText);
    }

    // The generated actual may wrap or paste the editable root contribution
    // before the final callee observes it: `(X)` stringified as `(beta)`,
    // `pre_##X` stringified as `pre_beta`, `G(X)` stringified as `ID(beta)`,
    // or `X##_tail` pasted before a later callee paste.  When old/new solved
    // values have a unique common context, invert only the changed middle
    // slice back into the original root source argument.
    size_t prefix = 0;
    while (prefix < oldText.size() && prefix < newText.size() &&
           oldText[prefix] == newText[prefix])
      ++prefix;
    size_t suffix = 0;
    while (suffix + prefix < oldText.size() &&
           suffix + prefix < newText.size() &&
           oldText[oldText.size() - suffix - 1] ==
               newText[newText.size() - suffix - 1])
      ++suffix;
    if (prefix + suffix < oldText.size()) {
      StringRef oldMiddle =
          oldText.slice(prefix, oldText.size() - suffix).trim();
      StringRef newMiddle =
          newText.slice(prefix, newText.size() - suffix).trim();
      if (!oldMiddle.empty()) {
        size_t middlePos = source.find(oldMiddle);
        if (middlePos != StringRef::npos &&
            source.find(oldMiddle, middlePos + 1) == StringRef::npos)
          return stringutils::replaceRange(
              source.str(), middlePos, middlePos + oldMiddle.size(), newMiddle);
      }
    }
    return std::nullopt;
  };

  DenseMap<uint32_t, std::string> replByRootArgIdx;
  DenseMap<uint32_t, std::string> workingRootTextByArgIdx;
  for (uint32_t i = 0; i < newSolved->size(); ++i) {
    if (i >= rootSlotByFinalParam.size())
      return std::nullopt;
    const uint32_t rootIdx = rootSlotByFinalParam[i];
    if (rootIdx >= invArgRanges.size())
      return std::nullopt;

    // Unchanged final-callee actuals impose no source rewrite obligation.
    // This is important after variadic-pack distribution: several final
    // parameters may all point back to one root `__VA_ARGS__` argument, and
    // unchanged pack elements must not compete with the changed element's
    // composed replacement for that same root range.
    if (generatedCalleeTextsTokenEquivalent(StringRef((*oldSolved)[i]),
                                             StringRef((*newSolved)[i]),
                                             deps_.lexLang))
      continue;

    if (!isMacroInvocationVariadicFormal(m, rootIdx) &&
        StringRef((*newSolved)[i]).trim().empty())
      return std::nullopt;

    StringRef originalRoot =
        baseInvText
            .slice(invArgRanges[rootIdx].first, invArgRanges[rootIdx].second)
            .trim();
    auto workingIt = workingRootTextByArgIdx.find(rootIdx);
    StringRef source = workingIt != workingRootTextByArgIdx.end()
                           ? StringRef(workingIt->second)
                           : (i < rootSourceByFinalParam.size()
                                  ? StringRef(rootSourceByFinalParam[i])
                                  : StringRef(oldActuals[i]));

    if (workingIt != workingRootTextByArgIdx.end()) {
      // Multiple final-callee parameters can impose the same edit on one
      // root actual.  For example, `FWD_DUP(G, X) -> G(X, X)` followed by
      // `JOIN(a, b) -> a ## b` solves both final parameters as `aa -> bb`,
      // but both obligations target the single root argument `X`.  After the
      // first obligation has rewritten that root text, replay the current
      // obligation against the original root spelling and accept it as
      // already discharged only when it yields the exact current root text.
      // This keeps duplicate-use composition deterministic while still
      // rejecting genuinely conflicting same-root obligations.
      auto alreadySatisfied = rewriteSourceActualFromSolvedExpansion(
          originalRoot, StringRef((*oldSolved)[i]), StringRef((*newSolved)[i]));
      if (alreadySatisfied &&
          generatedCalleeTextsTokenEquivalent(StringRef(*alreadySatisfied),
                                             source, deps_.lexLang))
        continue;
    }

    auto rewritten = rewriteSourceActualFromSolvedExpansion(
        source, StringRef((*oldSolved)[i]), StringRef((*newSolved)[i]));
    if (!rewritten)
      return std::nullopt;
    if (!isMacroInvocationVariadicFormal(m, rootIdx) &&
        replacementIntroducesTopLevelComma(*rewritten, deps_.lexLang))
      return std::nullopt;

    // Compose multiple solved final parameters that originate from the same
    // root argument, especially a distributed variadic pack.  Each step is
    // still uniquely inverted against the current source text; if two solved
    // obligations cannot be composed into one deterministic root replacement,
    // the proof fails closed instead of choosing an arbitrary pack rewrite.
    workingRootTextByArgIdx[rootIdx] = std::move(*rewritten);
    StringRef finalRoot = StringRef(workingRootTextByArgIdx[rootIdx]).trim();
    if (finalRoot != originalRoot)
      replByRootArgIdx[rootIdx] = finalRoot.str();
    else
      replByRootArgIdx.erase(rootIdx);
  }

  if (replByRootArgIdx.empty())
    return std::nullopt;

  std::optional<InvocationRewriteWithRange> rewrite =
      deps_.buildInvocationRewriteWithRange(
          actualRecoveryCtx, replByRootArgIdx,
          /*materializedRangeByArgIdx=*/nullptr);
  if (!rewrite)
    return std::nullopt;

  MacroPatch patch{*m.invB, *m.invE, std::move(rewrite->text), m.id};
  deps_.proofCertifier.CertifyInvocationRewriteMaterializedOutputRange(
      patch, rewrite->materializedOutputByteStart,
      rewrite->materializedOutputByteEnd);
  certifyMacroPatchMaterializedBTokenRange(patch,
                                           static_cast<uint64_t>(bEnv->first),
                                           static_cast<uint64_t>(bEnv->second));
  deps_.proofCertifier.SetArgsOnlyStandardProof(
      patch, m, /*wholeEnvelopeReplayValidated=*/true);
  deps_.proofCertifier.CertifyGeneratedCalleeReplayProof(
      patch, m,
      generatedCalleeCtx.currentDefinition
          ? generatedCalleeCtx.currentDefinition->id
          : 0,
      generatedCalleeCtx.generatedCallDepth,
      generatedCalleeCtx.objectAliasHopCount,
      generatedCalleeCtx.usesStringification, generatedCalleeCtx.usesPaste,
      generatedCalleeCtx.usesVariadicForwarding,
      /*decodedStringLiteralEvidenceOnly=*/
      generatedCalleeCtx.usesStringification);
  return patch;
}

std::optional<MacroPatch> RefoldMacroGeneratedCalleeReplayEngine::
    BuildTupleGeneratedCalleeReplayCandidate(
        const TupleGeneratedCalleeReplayContext &tupleGeneratedCtx) const {
  const RefoldModel::MacroInvocation &m = tupleGeneratedCtx.invocation;
  StringRef baseInvText = tupleGeneratedCtx.baseInvocationText;
  const std::pair<uint64_t, uint64_t> *cover =
      &tupleGeneratedCtx.wholeCoverATokens;
  const std::pair<size_t, size_t> *bEnv = &tupleGeneratedCtx.bTokenEnvelope;

  const auto argRange =
      tupleGeneratedCtx.invocationArgRanges[tupleGeneratedCtx.callerArgIdx];
  if (argRange.second < argRange.first ||
      argRange.second > tupleGeneratedCtx.baseInvocationText.size())
    return std::nullopt;
  StringRef parentTrim = tupleGeneratedCtx.baseInvocationText
                             .slice(argRange.first, argRange.second)
                             .trim();
  if (!parentTrim.starts_with("(") || !parentTrim.ends_with(")") ||
      parentTrim.size() < 2)
    return std::nullopt;

  StringRef tuplePayload = parentTrim.drop_front().drop_back();
  SmallVector<TupleElementSlice, 8> tupleElems;
  if (!splitTopLevelTupleElementsWithLexer(tuplePayload, deps_.lexLang,
                                           tupleElems) ||
      tupleElems.size() < 2)
    return std::nullopt;

  auto tupleElementText = [&](size_t elemIdx) -> StringRef {
    const TupleElementSlice &elem = tupleElems[elemIdx];
    return tuplePayload.slice(elem.trimBegin, elem.trimEnd).trim();
  };

  struct GeneratedArgRef {
    uint32_t forwarderParamIdx = 0;
    bool variadicPack = false;
  };

  const auto &forwarderToks =
      tupleGeneratedCtx.forwarderDefinition.replacementTokens;

  // Recover the generated call inside the forwarding macro as a token-level
  // context rather than requiring the whole replacement list to be exactly
  // `F(X, ...)`.  This is the owner-level invariant for tuple-generated
  // callees: fixed literal tokens before/after the generated call belong to
  // the forwarding template, while the callee formal and generated actual
  // formals still map positionally back to tuple slots.  Examples accepted by
  // this proof include `F(X)`, `(F(X))`, and `F(X) "!"`; anything with
  // operators such as #/## in the forwarding layer remains outside this proof.
  size_t generatedCallBegin = forwarderToks.size();
  size_t generatedCallOpen = forwarderToks.size();
  size_t generatedCallClose = forwarderToks.size();
  uint32_t calleeForwarderParam = 0;
  bool foundGeneratedCall = false;
  for (size_t i = 0; i + 1 < forwarderToks.size(); ++i) {
    const auto &calleeTok = forwarderToks[i];
    const auto &openTok = forwarderToks[i + 1];
    if (calleeTok.kind != RefoldModel::MacroReplacementTokenKind::ParamRef ||
        !calleeTok.paramIndex ||
        openTok.kind != RefoldModel::MacroReplacementTokenKind::Literal ||
        openTok.spelling != "(") {
      continue;
    }

    unsigned depth = 1;
    size_t close = i + 2;
    for (; close < forwarderToks.size(); ++close) {
      const auto &tok = forwarderToks[close];
      if (tok.kind != RefoldModel::MacroReplacementTokenKind::Literal)
        continue;
      if (tok.spelling == "(") {
        ++depth;
        continue;
      }
      if (tok.spelling == ")") {
        if (--depth == 0)
          break;
      }
    }
    if (depth != 0 || close >= forwarderToks.size())
      return std::nullopt;
    if (foundGeneratedCall)
      return std::nullopt;
    foundGeneratedCall = true;
    generatedCallBegin = i;
    generatedCallOpen = i + 1;
    generatedCallClose = close;
    calleeForwarderParam = *calleeTok.paramIndex;
  }
  if (!foundGeneratedCall ||
      calleeForwarderParam >=
          tupleGeneratedCtx.forwarderDefinition.defParams.size() ||
      calleeForwarderParam >= tupleElems.size())
    return std::nullopt;

  SmallVector<std::string, 8> forwarderPrefixLiterals;
  SmallVector<std::string, 8> forwarderSuffixLiterals;
  auto collectForwarderLiteralContext = [&](size_t begin, size_t end,
                                            SmallVectorImpl<std::string> &out) {
    for (size_t i = begin; i < end; ++i) {
      const auto &tok = forwarderToks[i];
      if (tok.kind != RefoldModel::MacroReplacementTokenKind::Literal ||
          tok.spelling == "#" || tok.spelling == "##" ||
          tok.spelling == "__VA_OPT__")
        return false;
      out.push_back(tok.spelling.str());
    }
    return true;
  };
  if (!collectForwarderLiteralContext(0, generatedCallBegin,
                                      forwarderPrefixLiterals) ||
      !collectForwarderLiteralContext(generatedCallClose + 1,
                                      forwarderToks.size(),
                                      forwarderSuffixLiterals)) {
    return std::nullopt;
  }

  SmallVector<GeneratedArgRef, 8> generatedArgs;
  for (size_t i = generatedCallOpen + 1; i < generatedCallClose; ++i) {
    const auto &tok = forwarderToks[i];
    if (tok.spelling == "#" || tok.spelling == "##" ||
        tok.spelling == "__VA_OPT__")
      return std::nullopt;
    if (tok.kind != RefoldModel::MacroReplacementTokenKind::ParamRef)
      continue;
    if (!tok.paramIndex ||
        *tok.paramIndex >=
            tupleGeneratedCtx.forwarderDefinition.defParams.size())
      return std::nullopt;
    if (*tok.paramIndex == calleeForwarderParam)
      return std::nullopt;
    GeneratedArgRef ref;
    ref.forwarderParamIdx = *tok.paramIndex;
    ref.variadicPack =
        tupleGeneratedCtx.forwarderDefinition.defParams[*tok.paramIndex]
            .variadic;
    generatedArgs.push_back(ref);
  }
  if (generatedArgs.empty())
    return std::nullopt;

  uint32_t calleeAliasHops = 0;
  const RefoldModel::MacroDirective *calleeDefinition =
      deps_.resolveFunctionLikeMacroThroughAliasesWithHops(
          tupleElementText(static_cast<size_t>(calleeForwarderParam)),
          &calleeAliasHops);
  tupleGeneratedCtx.objectAliasHopCount += calleeAliasHops;
  if (!calleeDefinition || calleeDefinition->defParams.empty())
    return std::nullopt;

  SmallVector<StringRef, 8> oldGeneratedPieces;
  for (const GeneratedArgRef &ref : generatedArgs) {
    if (ref.variadicPack) {
      if (ref.forwarderParamIdx >= tupleElems.size())
        return std::nullopt;
      for (size_t i = ref.forwarderParamIdx; i < tupleElems.size(); ++i)
        oldGeneratedPieces.push_back(tupleElementText(i));
      continue;
    }
    if (ref.forwarderParamIdx >= tupleElems.size())
      return std::nullopt;
    oldGeneratedPieces.push_back(tupleElementText(ref.forwarderParamIdx));
  }

  const bool calleeHasVariadic = !calleeDefinition->defParams.empty() &&
                                 calleeDefinition->defParams.back().variadic;
  bool tupleReplayUsesStringification = false;
  bool tupleReplayUsesPaste = false;
  bool tupleReplayUsesVariadicForwarding = false;
  for (const GeneratedArgRef &ref : generatedArgs)
    tupleReplayUsesVariadicForwarding |= ref.variadicPack;
  const size_t fixedCalleeActuals = calleeHasVariadic
                                        ? calleeDefinition->defParams.size() - 1
                                        : calleeDefinition->defParams.size();
  if ((!calleeHasVariadic &&
       oldGeneratedPieces.size() != calleeDefinition->defParams.size()) ||
      (calleeHasVariadic && oldGeneratedPieces.size() < fixedCalleeActuals))
    return std::nullopt;

  SmallVector<std::string, 8> oldActuals;
  oldActuals.reserve(calleeDefinition->defParams.size());
  for (size_t i = 0; i < fixedCalleeActuals; ++i)
    oldActuals.push_back(oldGeneratedPieces[i].str());
  if (calleeHasVariadic) {
    std::string variadicText;
    raw_string_ostream os(variadicText);
    for (size_t i = fixedCalleeActuals; i < oldGeneratedPieces.size(); ++i) {
      if (i != fixedCalleeActuals)
        os << ", ";
      os << oldGeneratedPieces[i].trim();
    }
    os.flush();
    oldActuals.push_back(std::move(variadicText));
  }
  if (oldActuals.size() != calleeDefinition->defParams.size())
    return std::nullopt;

  StringRef oldExpansion =
      deps_.sourceMapper.SliceASource(cover->first, cover->second).trim();
  StringRef newExpansion =
      deps_.sourceMapper.SliceBSource(bEnv->first, bEnv->second).trim();

  auto rewriteTupleElementFromSolvedExpansion =
      [&](size_t elemIdx, StringRef oldText,
          StringRef newText) -> std::optional<std::string> {
    StringRef source = tupleElementText(elemIdx);
    oldText = oldText.trim();
    newText = newText.trim();

    // Prefer token-position replacement over whole-slot replacement.  This is
    // what preserves spelling trivia in stringification inversions: `#x`
    // observes normalized whitespace (`"alpha + beta"`), but the tuple slot
    // may be `alpha   +   beta`.  If the old solved token sequence appears
    // once in the source slot, replace just those token spellings and leave
    // the original inter-token trivia untouched.
    SmallVector<RefoldLexBoundaryToken, 16> sourceToks;
    SmallVector<RefoldLexBoundaryToken, 16> oldToks;
    SmallVector<RefoldLexBoundaryToken, 16> newToks;
    refoldLexBoundaryTokens(source, deps_.lexLang, sourceToks);
    refoldLexBoundaryTokens(oldText, deps_.lexLang, oldToks);
    refoldLexBoundaryTokens(newText, deps_.lexLang, newToks);
    if (!oldToks.empty() && oldToks.size() == newToks.size() &&
        sourceToks.size() >= oldToks.size()) {
      std::optional<size_t> matchBegin;
      bool ambiguous = false;
      for (size_t i = 0; i + oldToks.size() <= sourceToks.size(); ++i) {
        bool same = true;
        for (size_t j = 0; j < oldToks.size(); ++j) {
          if (sourceToks[i + j].spelling != oldToks[j].spelling) {
            same = false;
            break;
          }
        }
        if (!same)
          continue;
        if (matchBegin) {
          ambiguous = true;
          break;
        }
        matchBegin = i;
      }
      if (matchBegin && !ambiguous) {
        std::string rewritten = source.str();
        for (size_t j = oldToks.size(); j > 0; --j) {
          const size_t idx = *matchBegin + j - 1;
          rewritten = stringutils::replaceRange(
              rewritten, sourceToks[idx].begin, sourceToks[idx].end,
              newToks[j - 1].spelling);
        }
        return rewritten;
      }
    }

    if (source == oldText)
      return newText.str();
    if (auto loc = findUniqueTrimmedSubstring(source, oldText))
      return stringutils::replaceRange(source.str(), loc->first, loc->second,
                                       newText);
    return std::nullopt;
  };
  // Replay the generated callee as a small replacement-list transducer over
  // tuple slots.  A generated callee contributes a sequence of literal
  // tokens, ordinary parameter projections, stringified projections, pasted-
  // token projections, and fixed forwarding-context tokens.  If that sequence
  // explains both the old whole-cover expansion and the new B-side expansion
  // uniquely, then the solved parameter values can be translated back to
  // positional tuple edits.
  std::vector<TupleCalleeReplayElem> calleePattern;
  const TupleGeneratedCalleeReplayPatternParser tuplePatternParser(
      *calleeDefinition, ArrayRef<std::string>(oldActuals.data(), oldActuals.size()),
      tupleReplayUsesStringification, tupleReplayUsesPaste);
  if (!tuplePatternParser.Parse(0, calleeDefinition->replacementTokens.size(),
                                calleePattern) ||
      calleePattern.empty())
    return std::nullopt;

  std::vector<TupleCalleeReplayElem> replayPattern;
  replayPattern.reserve(forwarderPrefixLiterals.size() + calleePattern.size() +
                        forwarderSuffixLiterals.size());
  for (const std::string &literal : forwarderPrefixLiterals) {
    TupleCalleeReplayElem elem;
    elem.kind = TupleCalleeReplayKind::Literal;
    elem.literal = literal;
    replayPattern.push_back(std::move(elem));
  }
  replayPattern.insert(replayPattern.end(), calleePattern.begin(),
                       calleePattern.end());
  for (const std::string &literal : forwarderSuffixLiterals) {
    TupleCalleeReplayElem elem;
    elem.kind = TupleCalleeReplayKind::Literal;
    elem.literal = literal;
    replayPattern.push_back(std::move(elem));
  }

  const TupleGeneratedCalleeReplayResolver tupleReplayResolver(
      ArrayRef<TupleCalleeReplayElem>(replayPattern.data(), replayPattern.size()),
      ArrayRef<std::string>(oldActuals.data(), oldActuals.size()), deps_.lexLang);

  std::optional<TupleSolvedActuals> oldSolved =
      tupleReplayResolver.SolveExpansion(oldExpansion);
  if (!oldSolved || oldSolved->size() != oldActuals.size())
    return std::nullopt;

  // The replay value observed at the final callee is not always textually the
  // same as the tuple element that supplied it.  For example, `STR(ID(x))`
  // stringifies the prescanned value `x`, while the source slot we want to
  // preserve is still `ID(x)`.  Do not require old replay values to equal the
  // whole tuple slot here.  The tuple edit step below uses the solved old
  // value as the replacement key and accepts it only if that token sequence
  // is uniquely found inside the original tuple element; otherwise the proof
  // fails closed.

  std::optional<TupleSolvedActuals> newSolved =
      tupleReplayResolver.SolveExpansion(newExpansion);
  if (!newSolved || newSolved->size() != oldActuals.size())
    return std::nullopt;

  SmallVector<std::string, 8> newActuals;
  newActuals.reserve(newSolved->size());
  for (const std::string &actual : *newSolved)
    newActuals.push_back(StringRef(actual).trim().str());

  SmallVector<std::string, 8> newGeneratedPieces;
  for (size_t i = 0; i < fixedCalleeActuals; ++i)
    newGeneratedPieces.push_back(newActuals[i]);
  if (calleeHasVariadic) {
    StringRef tail = StringRef(newActuals.back()).trim();
    if (!tail.empty())
      newGeneratedPieces.push_back(tail.str());
  }

  struct TupleEdit {
    size_t begin = 0;
    size_t end = 0;
    std::string text;
  };
  SmallVector<TupleEdit, 8> edits;
  size_t pieceCursor = 0;
  for (const GeneratedArgRef &ref : generatedArgs) {
    if (ref.variadicPack) {
      if (ref.forwarderParamIdx >= tupleElems.size())
        return std::nullopt;
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
      return std::nullopt;
    const TupleElementSlice &elem = tupleElems[ref.forwarderParamIdx];
    StringRef oldText = pieceCursor < oldSolved->size()
                            ? StringRef((*oldSolved)[pieceCursor]).trim()
                            : (pieceCursor < oldActuals.size()
                                   ? StringRef(oldActuals[pieceCursor]).trim()
                                   : oldGeneratedPieces[pieceCursor].trim());
    StringRef newText = StringRef(newGeneratedPieces[pieceCursor]).trim();
    if (newText != tuplePayload.slice(elem.trimBegin, elem.trimEnd).trim()) {
      auto rewrittenElem = rewriteTupleElementFromSolvedExpansion(
          ref.forwarderParamIdx, oldText, newText);
      if (!rewrittenElem)
        return std::nullopt;
      edits.push_back(
          TupleEdit{elem.trimBegin, elem.trimEnd, std::move(*rewrittenElem)});
    }
    ++pieceCursor;
  }
  if (pieceCursor != newGeneratedPieces.size() || edits.empty())
    return std::nullopt;

  llvm::sort(edits, [](const TupleEdit &lhs, const TupleEdit &rhs) {
    if (lhs.begin != rhs.begin)
      return lhs.begin > rhs.begin;
    return lhs.end > rhs.end;
  });

  std::string rebuiltPayload = tuplePayload.str();
  size_t previousBegin = std::numeric_limits<size_t>::max();
  for (const TupleEdit &edit : edits) {
    if (edit.end < edit.begin || edit.end > rebuiltPayload.size())
      return std::nullopt;
    if (previousBegin != std::numeric_limits<size_t>::max() &&
        edit.end > previousBegin)
      return std::nullopt;
    previousBegin = edit.begin;
    rebuiltPayload = stringutils::replaceRange(rebuiltPayload, edit.begin,
                                               edit.end, edit.text);
  }

  std::string rewrittenArg =
      ("(" + StringRef(rebuiltPayload).trim().str() + ")");
  std::string rewrittenInv = stringutils::replaceRange(
      baseInvText.str(), argRange.first, argRange.second, rewrittenArg);
  if (StringRef(rewrittenInv).trim() == baseInvText.trim())
    return std::nullopt;

  MacroPatch patch{*m.invB, *m.invE, std::move(rewrittenInv), m.id};
  patch.materialized.outputByteStart = 0;
  patch.materialized.outputByteEnd = patch.replacement.size();
  patch.materialized.hasOutputByteRange = true;
  certifyMacroPatchMaterializedBTokenRange(patch,
                                           static_cast<uint64_t>(bEnv->first),
                                           static_cast<uint64_t>(bEnv->second));
  deps_.proofCertifier.SetArgsOnlyStandardProof(
      patch, m, /*wholeEnvelopeReplayValidated=*/true);
  deps_.proofCertifier.CertifyGeneratedCalleeReplayProof(
      patch, m, calleeDefinition ? calleeDefinition->id : 0,
      /*generatedCallDepth=*/1, tupleGeneratedCtx.objectAliasHopCount,
      tupleReplayUsesStringification, tupleReplayUsesPaste,
      tupleReplayUsesVariadicForwarding,
      /*decodedStringLiteralEvidenceOnly=*/tupleReplayUsesStringification);
  return patch;
}

} // namespace refold
} // namespace clang
