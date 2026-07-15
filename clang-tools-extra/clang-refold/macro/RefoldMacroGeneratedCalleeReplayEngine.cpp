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
// The generated-callee replay implementation is intentionally split only after
// each local proof obligation is made explicit.  The companion census document
// records the remaining local composition blocks so future patches can move
// deterministic helpers and private resolvers without changing solver order,
// ambiguity rejection, proof certification, or fallback behavior.
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

/// Half-open replacement-token range owned by one generated-call actual.
///
/// The range uses replacement-list indexes rather than source offsets because
/// generated-callee replay is proving structure over a macro definition tape.
/// Keeping these indexes named prevents later replay-tape resolver extraction
/// from reintroducing anonymous `(begin, end)` pairs whose ordering policy is
/// hard to audit.
struct ReplacementTokenRange {
  size_t begin = 0;
  size_t end = 0;
};

/// Replacement-list shape for one deterministic generated call step.
///
/// The generated-callee engine constructs these shapes while replaying a callee
/// call that was produced by macro expansion rather than source spelling.  The
/// carrier owns only shape evidence: the selected callee formal, argument token
/// ranges, and literal prefix/suffix context.  It performs no replay admission,
/// solver invocation, or proof certification.
struct GeneratedCallShape {
  uint32_t calleeParamIdx = 0;
  size_t callBegin = 0;
  size_t openParenIndex = 0;
  size_t closeParenIndex = 0;
  llvm::SmallVector<ReplacementTokenRange, 8> argumentRanges;
  llvm::SmallVector<std::string, 8> prefixLiterals;
  llvm::SmallVector<std::string, 8> suffixLiterals;
};

/// Literal context that surrounds one generated call in a replacement tape.
///
/// The context is kept as replay spellings because the downstream proof builder
/// already reasons over literal strings.  Collection remains separate from call
/// shape discovery so prefix/suffix ordering and unsupported-token rejection are
/// an explicit proof obligation rather than an incidental pair of local loops.
struct GeneratedReplayLiteralContext {
  llvm::SmallVector<std::string, 8> prefix;
  llvm::SmallVector<std::string, 8> suffix;
};

/// Borrowed mutable state for one generated-callee replay chain.
///
/// The public replay context intentionally lends these fields by reference so
/// callers observe the same replay-depth, alias-hop, literal-stack, and proof-
/// flag mutations they observed before this cleanup.  This private carrier makes
/// that mutable chain state explicit without copying it: every update still
/// occurs at the original program point, and every fail-closed exit preserves
/// the side effects already performed by earlier successful chain steps.
struct GeneratedReplayChainState {
  explicit GeneratedReplayChainState(
      const GeneratedCalleeReplayContext &ctx)
      : replayPrefixLiterals(ctx.replayPrefixLiterals),
        replaySuffixStack(ctx.replaySuffixStack),
        currentDefinition(ctx.currentDefinition),
        currentActuals(ctx.currentActuals),
        followedGeneratedCall(ctx.followedGeneratedCall),
        generatedCallDepth(ctx.generatedCallDepth),
        objectAliasHopCount(ctx.objectAliasHopCount),
        usesStringification(ctx.usesStringification), usesPaste(ctx.usesPaste),
        usesVariadicForwarding(ctx.usesVariadicForwarding) {}

  /// Prefix literals accumulated in replay order before the final callee body.
  llvm::SmallVectorImpl<std::string> &replayPrefixLiterals;
  /// Per-step suffix literals replayed in reverse chain order after the body.
  llvm::SmallVectorImpl<llvm::SmallVector<std::string, 8>> &replaySuffixStack;
  /// Definition currently reached by generated-callee chain replay.
  const RefoldModel::MacroDirective *&currentDefinition;
  /// Actual source slots at the current replay level.
  llvm::SmallVectorImpl<GeneratedCalleeSourceSlot> &currentActuals;
  /// Set exactly when at least one generated-call step is accepted.
  bool &followedGeneratedCall;
  /// Number of accepted generated-call steps.
  uint32_t &generatedCallDepth;
  /// Object-like alias hops consumed by accepted callee resolution.
  uint32_t &objectAliasHopCount;
  /// Proof flag set when replay passes through stringification.
  bool &usesStringification;
  /// Proof flag set when replay passes through paste.
  bool &usesPaste;
  /// Proof flag set when replay distributes or forwards variadic actuals.
  bool &usesVariadicForwarding;
};

/// Final solved generated-callee state before patch construction.
///
/// The replay chain eventually reaches one concrete callee definition.  This
/// carrier keeps the final actual spellings, their root-invocation provenance,
/// and same-root replacement-composition maps together so later resolver
/// extraction can move solution assembly without obscuring ordering or conflict
/// policy.  It owns only final-solution state: solver invocation, replay
/// admission, and proof certification remain at their existing call sites.
struct FinalGeneratedCalleeSolution {
  /// Final-callee actual spellings in callee formal order.
  llvm::SmallVector<std::string, 8> oldActuals;
  /// Root invocation formal index that owns each final actual.
  llvm::SmallVector<uint32_t, 8> rootSlotByFinalParam;
  /// Original root source text used to invert each solved final actual.
  llvm::SmallVector<std::string, 8> rootSourceByFinalParam;
  /// Composed replacements keyed by root invocation formal index.
  llvm::DenseMap<uint32_t, std::string> replacementsByRootArgIdx;
  /// Working text for root actuals that receive multiple solved obligations.
  llvm::DenseMap<uint32_t, std::string> workingRootTextByArgIdx;
};

/// One generated forwarder argument reference in tuple replay.
///
/// The tuple-generated replay path proves that a forwarding macro builds a
/// generated callee invocation from tuple elements.  This carrier records the
/// forwarder formal whose corresponding tuple element supplies one generated
/// actual, plus whether that formal represents the variadic tail.  Keeping the
/// reference explicit preserves tuple-element order and variadic-tail joining
/// when the tuple replay algorithm is later moved into a resolver.
struct TupleGeneratedArgRef {
  /// Forwarder formal index, which also indexes the caller tuple element.
  uint32_t forwarderParamIdx = 0;
  /// True when this reference consumes the remaining tuple elements as a pack.
  bool variadicPack = false;
};

/// One positional edit to the caller tuple payload.
///
/// Edits are collected in tuple-payload byte coordinates and later applied from
/// right to left.  The carrier owns only the edit coordinates and replacement
/// text; overlap rejection and final invocation construction remain at the
/// existing proof site.
struct TupleGeneratedEdit {
  size_t begin = 0;
  size_t end = 0;
  std::string text;
};

/// Mutable state for tuple-generated forwarder replay.
///
/// This carrier groups the tuple slots, generated-argument references, solved
/// old actuals, positional edits, and rebuilt tuple payload used by
/// `BuildTupleGeneratedCalleeReplayCandidate`.  It intentionally does not own
/// generated-callee parsing, solver calls, proof certification, or replay
/// admission.  Those operations stay in place so tuple ordering, variadic-tail
/// handling, right-to-left edit application, and direct forwarder proof
/// requirements remain unchanged.
struct TupleGeneratedForwarderState {
  /// Trimmed text for each caller tuple element in original tuple order.
  llvm::SmallVector<std::string, 8> tupleElementTexts;
  /// Forwarder-formal references recovered from the generated call actuals.
  llvm::SmallVector<TupleGeneratedArgRef, 8> generatedArgRefs;
  /// Source tuple pieces used as old generated-callee actual evidence.
  llvm::SmallVector<std::string, 8> oldGeneratedPieces;
  /// Final callee actuals in callee formal order before solving.
  llvm::SmallVector<std::string, 8> oldActuals;
  /// Positional edits to apply to the tuple payload from right to left.
  llvm::SmallVector<TupleGeneratedEdit, 8> edits;
  /// Working tuple payload rebuilt after positional edits are accepted.
  std::string rebuiltPayload;

  /// Returns a tuple element spelling already trimmed by the tuple splitter.
  StringRef TupleElementText(size_t elemIdx) const {
    return tupleElementTexts[elemIdx];
  }
};

/// Solved tuple-generated replay result before proof certification.
///
/// The tuple resolver owns replay discovery and tuple-payload reconstruction,
/// but the public engine method still constructs the `MacroPatch` and certifies
/// the proof.  Keeping this result narrow preserves the existing boundary:
/// solution discovery may fail closed without mutating patch/proof state, while
/// successful replay returns only the rewritten invocation text, final callee
/// identity, and proof flags observed during deterministic replay.
struct TupleGeneratedReplaySolution {
  /// Invocation spelling after replacing the tuple argument payload.
  std::string rewrittenInvocation;
  /// Final generated callee definition proven by tuple replay.
  const RefoldModel::MacroDirective *calleeDefinition = nullptr;
  /// True when replay inverted stringification in the generated callee.
  bool usesStringification = false;
  /// True when replay inverted token paste in the generated callee.
  bool usesPaste = false;
  /// True when a forwarder variadic pack supplied tuple elements.
  bool usesVariadicForwarding = false;
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

/// Rewrites a uniquely matched solved token sequence inside source text.
///
/// Generated-callee solving observes final expansion text, which may normalize
/// trivia through stringification or macro replay.  This helper owns only the
/// shared token-position proof: if the old solved token spellings appear
/// exactly once in the source text and the old/new solved token counts match,
/// replace just those source token spellings from right to left so original
/// inter-token trivia outside the solved slice is preserved.  It performs no
/// fallback rewrite and therefore cannot accept a weaker proof on its own.
std::optional<std::string> rewriteSolvedExpansionTokenSequence(
    StringRef source, StringRef oldText, StringRef newText,
    const clang::LangOptions &lexLang) {
  SmallVector<RefoldLexBoundaryToken, 16> sourceToks;
  SmallVector<RefoldLexBoundaryToken, 16> oldToks;
  SmallVector<RefoldLexBoundaryToken, 16> newToks;
  refoldLexBoundaryTokens(source, lexLang, sourceToks);
  refoldLexBoundaryTokens(oldText, lexLang, oldToks);
  refoldLexBoundaryTokens(newText, lexLang, newToks);
  if (oldToks.empty() || oldToks.size() != newToks.size() ||
      sourceToks.size() < oldToks.size())
    return std::nullopt;

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

  if (!matchBegin || ambiguous)
    return std::nullopt;

  std::string rewritten = source.str();
  for (size_t j = oldToks.size(); j > 0; --j) {
    const size_t idx = *matchBegin + j - 1;
    rewritten = stringutils::replaceRange(rewritten, sourceToks[idx].begin,
                                          sourceToks[idx].end,
                                          newToks[j - 1].spelling);
  }
  return rewritten;
}

/// Rewrites a root invocation actual from a solved generated-callee expansion.
///
/// The root-actual path first tries the shared token-position proof, then keeps
/// the exact historical fallbacks: whole trimmed-slot replacement, unique raw
/// substring replacement, and unique changed-middle inversion for generated
/// arguments that wrapped or pasted the editable root contribution before the
/// final callee observed it.
std::optional<std::string> rewriteSourceActualFromSolvedExpansion(
    StringRef source, StringRef oldText, StringRef newText,
    const clang::LangOptions &lexLang) {
  oldText = oldText.trim();
  newText = newText.trim();

  if (std::optional<std::string> rewritten =
          rewriteSolvedExpansionTokenSequence(source, oldText, newText, lexLang))
    return rewritten;

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
    StringRef oldMiddle = oldText.slice(prefix, oldText.size() - suffix).trim();
    StringRef newMiddle = newText.slice(prefix, newText.size() - suffix).trim();
    if (!oldMiddle.empty()) {
      size_t middlePos = source.find(oldMiddle);
      if (middlePos != StringRef::npos &&
          source.find(oldMiddle, middlePos + 1) == StringRef::npos)
        return stringutils::replaceRange(source.str(), middlePos,
                                         middlePos + oldMiddle.size(),
                                         newMiddle);
    }
  }
  return std::nullopt;
}

/// Rewrites one tuple element from a solved tuple-generated expansion.
///
/// Tuple-generated replay shares the token-position replacement proof with root
/// actual rewriting, but deliberately keeps its narrower fallback policy: accept
/// only whole-slot equality or one uniquely trimmed substring inside the tuple
/// element.  It does not use the root-actual changed-middle inversion because
/// tuple edits must remain positional within one tuple slot.
std::optional<std::string> rewriteTupleElementFromSolvedExpansion(
    StringRef source, StringRef oldText, StringRef newText,
    const clang::LangOptions &lexLang) {
  oldText = oldText.trim();
  newText = newText.trim();

  if (std::optional<std::string> rewritten =
          rewriteSolvedExpansionTokenSequence(source, oldText, newText, lexLang))
    return rewritten;

  if (source == oldText)
    return newText.str();
  if (auto loc = findUniqueTrimmedSubstring(source, oldText))
    return stringutils::replaceRange(source.str(), loc->first, loc->second,
                                     newText);
  return std::nullopt;
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

/// Returns whether `idx` names the requested literal replacement token.
///
/// This is a shape-scanning helper only: it recognizes exact punctuation used
/// to delimit generated calls and leaves all admission decisions to the caller.
bool isReplacementLiteralToken(
    ArrayRef<RefoldModel::MacroReplacementToken> toks, size_t idx,
    StringRef spelling) {
  return idx < toks.size() &&
         toks[idx].kind == RefoldModel::MacroReplacementTokenKind::Literal &&
         toks[idx].spelling == spelling;
}

/// Returns whether a replacement token may be replayed as literal context.
///
/// Stringification, paste, and `__VA_OPT__` remain unsupported in generated
/// replay literal prefixes/suffixes.  Rejecting them here preserves the old
/// fail-closed policy instead of letting literal context become a fallback path.
bool isReplayLiteralToken(const RefoldModel::MacroReplacementToken &tok) {
  return tok.kind == RefoldModel::MacroReplacementTokenKind::Literal &&
         tok.spelling != "#" && tok.spelling != "##" &&
         tok.spelling != "__VA_OPT__";
}

/// Finds the close parenthesis matching `openIdx` before `limit`.
///
/// Only literal parenthesis tokens affect nesting.  Malformed or unmatched
/// input returns nullopt so the caller rejects the generated-call shape rather
/// than guessing a replay boundary.
std::optional<size_t> findMatchingReplacementParen(
    ArrayRef<RefoldModel::MacroReplacementToken> toks, size_t openIdx,
    size_t limit) {
  if (!isReplacementLiteralToken(toks, openIdx, "("))
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
}

/// Collects top-level generated-call argument ranges between matching parens.
///
/// Ranges are emitted left-to-right and preserve the replacement-token indexes
/// consumed by the old local lambda.  Empty arguments and unbalanced nested
/// parentheses fail closed because they cannot be replayed as deterministic
/// actual-source obligations.
bool collectTopLevelReplacementArgumentRanges(
    ArrayRef<RefoldModel::MacroReplacementToken> toks, size_t openIdx,
    size_t closeIdx, SmallVectorImpl<ReplacementTokenRange> &out) {
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

    if (atEnd ||
        (argDepth == 0 &&
         toks[i].kind == RefoldModel::MacroReplacementTokenKind::Literal &&
         toks[i].spelling == ",")) {
      if (argBegin == i)
        return false;
      out.push_back({argBegin, i});
      argBegin = i + 1;
    }
  }
  return !out.empty();
}

/// Appends replay-safe literal spellings from one replacement-token range.
///
/// This helper performs no filtering beyond the existing literal-context proof
/// rule: only ordinary literal replacement tokens may surround a generated call.
/// Unsupported `#`, `##`, `__VA_OPT__`, or non-literal tokens reject the range
/// so replay cannot silently weaken to a different fallback proof.
bool appendGeneratedReplayLiteralRange(
    ArrayRef<RefoldModel::MacroReplacementToken> toks, size_t begin,
    size_t end, SmallVectorImpl<std::string> &out) {
  for (size_t i = begin; i < end; ++i) {
    const auto &tok = toks[i];
    if (!isReplayLiteralToken(tok))
      return false;
    out.push_back(tok.spelling.str());
  }
  return true;
}

/// Collects the prefix and suffix literal context around one generated call.
///
/// `generatedCallBegin` is the first token owned by the generated callee
/// expression, and `generatedCallEnd` is one past the matching close paren.
/// Prefix and suffix tokens are copied in original replacement-tape order.
/// Any unsupported context token rejects the entire generated-call shape,
/// preserving the previous fail-closed behavior.
std::optional<GeneratedReplayLiteralContext>
collectGeneratedReplayLiteralContext(
    ArrayRef<RefoldModel::MacroReplacementToken> toks,
    size_t generatedCallBegin, size_t generatedCallEnd) {
  GeneratedReplayLiteralContext context;
  if (!appendGeneratedReplayLiteralRange(toks, 0, generatedCallBegin,
                                         context.prefix) ||
      !appendGeneratedReplayLiteralRange(toks, generatedCallEnd, toks.size(),
                                         context.suffix))
    return std::nullopt;
  return context;
}

/// Source slots produced for one generated-callee formal parameter.
///
/// The carrier makes variadic pack distribution explicit without changing the
/// ordering of positional slots.  Each emitted slot retains the original root
/// owner so later solved final parameters can still compose replacements back
/// onto the root invocation argument.
struct ParamActualSourceSlots {
  SmallVector<GeneratedCalleeSourceSlot, 8> slots;
};

/// Splits one root variadic source slot into positional generated-callee slots.
///
/// The split is intentionally the old local policy: only top-level commas split
/// the pack, nested parentheses protect commas, and brackets/braces do not add
/// protection.  Empty elements reject the replay step fail-closed, while a
/// pack with no top-level comma remains a single source slot.
bool splitVariadicPackSourceSlot(const GeneratedCalleeSourceSlot &slot,
                                 const clang::LangOptions &lexLang,
                                 ParamActualSourceSlots &out) {
  SmallVector<RefoldLexBoundaryToken, 32> toks;
  refoldLexBoundaryTokens(StringRef(slot.text), lexLang, toks);

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
    StringRef elem = StringRef(slot.text).slice(elemBegin, tok.begin).trim();
    if (elem.empty())
      return false;
    pieces.push_back({static_cast<size_t>(elem.data() - slot.text.data()),
                      static_cast<size_t>(elem.data() - slot.text.data()) +
                          elem.size()});
    elemBegin = tok.end;
  }

  if (!sawComma) {
    out.slots.push_back(slot);
    return true;
  }

  StringRef finalElem = StringRef(slot.text).drop_front(elemBegin).trim();
  if (finalElem.empty())
    return false;
  pieces.push_back({static_cast<size_t>(finalElem.data() - slot.text.data()),
                    static_cast<size_t>(finalElem.data() - slot.text.data()) +
                        finalElem.size()});

  // A variadic formal can be forwarded into a fixed-arity generated callee.
  // The producer records the root variadic tail as one invocation argument
  // range (`foo, bar, baz`), but substituting `__VA_ARGS__` into a generated
  // call exposes those comma-separated elements as positional actuals.  Each
  // piece keeps the whole root variadic source as its rewrite owner so multiple
  // solved final parameters can be composed back into one root replacement.
  for (const auto &piece : pieces) {
    GeneratedCalleeSourceSlot split = slot;
    split.text = StringRef(slot.text).slice(piece.first, piece.second).str();
    split.rootSourceText = slot.rootSourceText;
    split.rootArgIdx = slot.rootArgIdx;
    out.slots.push_back(std::move(split));
  }
  return true;
}

/// Appends the source slots that instantiate one formal parameter.
///
/// Non-variadic formals append their single positional actual.  Variadic
/// formals either split one still-packed root source slot or append the already
/// distributed positional tail unchanged.  The boolean proof flag is mutated at
/// the same point as the old local lambda so variadic-forwarding evidence is
/// neither delayed nor weakened.
bool appendActualsForParam(const RefoldModel::MacroDirective &definition,
                           ArrayRef<GeneratedCalleeSourceSlot> actuals,
                           uint32_t paramIdx,
                           const clang::LangOptions &lexLang,
                           bool &usesVariadicForwarding,
                           SmallVectorImpl<GeneratedCalleeSourceSlot> &out) {
  if (paramIdx >= definition.defParams.size())
    return false;
  if (!isMacroDirectiveVariadicParam(definition, paramIdx)) {
    if (paramIdx >= actuals.size())
      return false;
    out.push_back(actuals[paramIdx]);
    return true;
  }
  usesVariadicForwarding = true;
  if (actuals.size() < paramIdx)
    return false;

  // If this variadic parameter is still represented by one root invocation
  // range, distribute the pack now.  If it has already been distributed by an
  // earlier generated-call step, preserve the existing positional slots.
  if (actuals.size() == paramIdx + 1) {
    ParamActualSourceSlots split;
    if (!splitVariadicPackSourceSlot(actuals[paramIdx], lexLang, split))
      return false;
    out.append(split.slots.begin(), split.slots.end());
    return true;
  }

  for (size_t i = paramIdx; i < actuals.size(); ++i)
    out.push_back(actuals[i]);
  return true;
}


/// Finds the single root source slot edited by one generated-call argument.
///
/// The helper preserves the old local policy: only one ordinary non-variadic
/// data parameter may own a generated argument.  Function-like selector
/// positions immediately followed by `(` are treated as fixed callee-selection
/// context, not editable data leaves.  Ambiguous, variadic, out-of-range, or
/// multi-leaf arguments fail closed so the generated-leaf solver can handle
/// richer literal skeletons without this path greedily rewriting the first
/// leaf.
std::optional<GeneratedCalleeSourceSlot> findEditableParamInGeneratedArgument(
    const RefoldMacroGeneratedCalleeReplayEngine::Dependencies &deps,
    const RefoldModel::MacroDirective &definition,
    ArrayRef<GeneratedCalleeSourceSlot> actuals, size_t begin, size_t end) {
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

    // A function-like macro actual immediately followed by `(` is a generated
    // callee selector inside this argument expression, not the source slot
    // whose value should be edited.  Treat it as fixed proof context and keep
    // looking for the ordinary data argument.
    const bool selectorPosition =
        i + 1 < end &&
        toks[i + 1].kind == RefoldModel::MacroReplacementTokenKind::Literal &&
        toks[i + 1].spelling == "(" &&
        deps.resolveFunctionLikeMacroForReplay(
            StringRef(actuals[*tok.paramIndex].text));
    if (selectorPosition)
      continue;

    const GeneratedCalleeSourceSlot &slot = actuals[*tok.paramIndex];
    if (editable)
      return std::nullopt;
    editable = slot;
  }
  return editable;
}

/// Instantiates one replacement-token argument for the next generated callee.
///
/// This helper owns only generated-argument materialization for one argument
/// range.  It preserves the original fail-closed proof rules for unsupported
/// `__VA_OPT__`, malformed stringification, paste-sensitive skeletons, and
/// variadic forwarding.  The proof flags are reference parameters so they are
/// mutated at the same exact points as the old local lambda.
bool instantiateGeneratedArgument(
    const RefoldMacroGeneratedCalleeReplayEngine::Dependencies &deps,
    const RefoldModel::MacroDirective &definition,
    ArrayRef<GeneratedCalleeSourceSlot> actuals, size_t begin, size_t end,
    bool &usesStringification, bool &usesPaste, bool &usesVariadicForwarding,
    SmallVectorImpl<GeneratedCalleeSourceSlot> &out) {
  const auto &toks = definition.replacementTokens;
  if (begin >= end)
    return false;

  // `__VA_ARGS__` as a complete generated actual distributes the variadic pack
  // positionally into the next generated callee.  Each emitted slot retains its
  // root owner so solved final parameters can still compose replacements back
  // into the original invocation argument.
  if (end == begin + 1 &&
      toks[begin].kind == RefoldModel::MacroReplacementTokenKind::ParamRef &&
      toks[begin].paramIndex &&
      isMacroDirectiveVariadicParam(definition, *toks[begin].paramIndex))
    return appendActualsForParam(definition, actuals, *toks[begin].paramIndex,
                                 deps.lexLang, usesVariadicForwarding, out);

  // Active `__VA_OPT__(, __VA_ARGS__)` in a generated-call argument list
  // contributes additional positional arguments rather than bytes inside the
  // preceding argument.  Accept only the canonical comma-plus-variadic form;
  // anything richer remains outside this proof and fails closed.
  for (size_t i = begin; i < end; ++i) {
    if (toks[i].spelling != "__VA_OPT__")
      continue;
    if (i != begin + 1 || begin >= end ||
        toks[begin].kind != RefoldModel::MacroReplacementTokenKind::ParamRef ||
        !toks[begin].paramIndex || i + 5 != end ||
        toks[i + 1].spelling != "(" || toks[i + 2].spelling != "," ||
        toks[i + 3].kind != RefoldModel::MacroReplacementTokenKind::ParamRef ||
        !toks[i + 3].paramIndex ||
        !isMacroDirectiveVariadicParam(definition,
                                       *toks[i + 3].paramIndex) ||
        toks[i + 4].spelling != ")")
      return false;
    if (!appendActualsForParam(definition, actuals, *toks[begin].paramIndex,
                               deps.lexLang, usesVariadicForwarding, out))
      return false;
    return appendActualsForParam(definition, actuals, *toks[i + 3].paramIndex,
                                 deps.lexLang, usesVariadicForwarding, out);
  }

  std::optional<GeneratedCalleeSourceSlot> editable =
      findEditableParamInGeneratedArgument(deps, definition, actuals, begin,
                                           end);
  if (!editable)
    return false;

  std::string text;
  for (size_t i = begin; i < end; ++i) {
    const auto &tok = toks[i];
    if (tok.spelling == "##") {
      usesPaste = true;
      continue;
    }
    if (tok.spelling == "#") {
      usesStringification = true;
      if (i + 1 >= end ||
          toks[i + 1].kind != RefoldModel::MacroReplacementTokenKind::ParamRef ||
          !toks[i + 1].paramIndex ||
          *toks[i + 1].paramIndex >= definition.defParams.size() ||
          isMacroDirectiveVariadicParam(definition,
                                        *toks[i + 1].paramIndex) ||
          *toks[i + 1].paramIndex >= actuals.size())
        return false;
      // A stringified generated argument is still an expression over the same
      // root source slot.  Materialize the old string-literal spelling for
      // replay, but keep the editable owner as the unstringified source
      // argument so the solved value rewrites `X`, not `#X` or `"X"`.
      text.push_back('"');
      text += actuals[*toks[i + 1].paramIndex].text;
      text.push_back('"');
      ++i;
      continue;
    }
    if (tok.spelling == "__VA_OPT__")
      return false;
    if (tok.kind == RefoldModel::MacroReplacementTokenKind::ParamRef) {
      if (!tok.paramIndex || *tok.paramIndex >= definition.defParams.size() ||
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
}

/// Finds the unique generated-callee call shape in a definition tape.
///
/// This helper preserves the old local policy: selector inversion proves the
/// callee formal, nested generated calls inside the selected call are treated as
/// argument expressions, and multiple competing owner calls fail closed.  The
/// result carries only deterministic evidence needed by the replay-chain step.
std::optional<GeneratedCallShape> findGeneratedCallShape(
    const RefoldMacroGeneratedCalleeReplayEngine::Dependencies &deps,
    const RefoldModel::MacroDirective &definition) {
  const auto &toks = definition.replacementTokens;
  if (toks.empty())
    return std::nullopt;

  // Resolve generated-callee owner ranges with the named selector resolver.
  // The surrounding shape scan owns call ordering and ambiguity checks; the
  // resolver only proves that a candidate callee expression maps to one
  // deterministic root formal or fails closed.
  const SelectorCalleeResolver selectorResolver(deps, definition);

  bool found = false;
  size_t callBegin = toks.size();
  size_t callOpen = toks.size();
  size_t callClose = toks.size();
  uint32_t calleeParamIdx = 0;

  for (size_t open = 1; open < toks.size(); ++open) {
    if (!isReplacementLiteralToken(toks, open, "("))
      continue;

    auto close = findMatchingReplacementParen(toks, open, toks.size());
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
    // expressions, not competing owner calls.  Skip the body after recording
    // the outer call so shapes such as `H(G(X))` remain one generated call
    // whose first actual is the expression `G(X)`.
    open = *close;
  }

  if (!found || calleeParamIdx >= definition.defParams.size())
    return std::nullopt;

  GeneratedCallShape shape;
  shape.calleeParamIdx = calleeParamIdx;
  shape.callBegin = callBegin;
  shape.openParenIndex = callOpen;
  shape.closeParenIndex = callClose;
  std::optional<GeneratedReplayLiteralContext> literalContext =
      collectGeneratedReplayLiteralContext(toks, callBegin, callClose + 1);
  if (!literalContext)
    return std::nullopt;
  shape.prefixLiterals = std::move(literalContext->prefix);
  shape.suffixLiterals = std::move(literalContext->suffix);

  if (!collectTopLevelReplacementArgumentRanges(toks, callOpen, callClose,
                                                shape.argumentRanges))
    return std::nullopt;
  return shape;
}

/// Builds the deterministic replay tape for a generated-callee chain.
///
/// The builder owns only chain stepping: it discovers one generated-call shape
/// at a time, resolves the selected generated callee, instantiates that call's
/// actual slots, and appends prefix/suffix literal context.  It mutates the
/// borrowed `GeneratedReplayChainState` at the same proof points as the former
/// inline loop: alias hops are recorded immediately after callee resolution,
/// proof flags are set by generated-argument instantiation, and depth advances
/// only after a fully admissible generated-call step is accepted.  Returning
/// false represents the same fail-closed exits that previously returned
/// `std::nullopt` from the public builder.
class ReplayTapeBuilder {
public:
  explicit ReplayTapeBuilder(
      const RefoldMacroGeneratedCalleeReplayEngine::Dependencies &deps)
      : deps_(deps) {}

  /// Advances the borrowed replay state through all provable generated calls.
  ///
  /// A missing, malformed, or ambiguous next generated-call shape is not itself
  /// an error here: it terminates the chain exactly as the former loop did and
  /// leaves final admission to the caller's existing generated-callee predicate.
  /// Once a shape selects a next callee, unsupported generated arguments,
  /// unresolved callees, and wrong actual counts fail closed by returning false.
  bool Build(GeneratedReplayChainState &state) const {
    for (size_t depth = 0; depth <= deps_.model.GetMacroDirectives().size();
         ++depth) {
      std::optional<GeneratedCallShape> shape =
          findGeneratedCallShape(deps_, *state.currentDefinition);
      if (!shape)
        break;
      if (shape->calleeParamIdx >= state.currentActuals.size())
        return false;

      uint32_t nextAliasHops = 0;
      const RefoldModel::MacroDirective *nextDefinition =
          deps_.resolveFunctionLikeMacroThroughAliasesWithHops(
              StringRef(state.currentActuals[shape->calleeParamIdx].text),
              &nextAliasHops);
      state.objectAliasHopCount += nextAliasHops;
      if (!nextDefinition || nextDefinition->defParams.empty())
        return false;

      SmallVector<GeneratedCalleeSourceSlot, 8> nextActuals;
      for (const ReplacementTokenRange &argRange : shape->argumentRanges) {
        if (!instantiateGeneratedArgument(
                deps_, *state.currentDefinition, state.currentActuals,
                argRange.begin, argRange.end, state.usesStringification,
                state.usesPaste, state.usesVariadicForwarding, nextActuals))
          return false;
      }

      if (!macroDefinitionAcceptsActualCount(*nextDefinition, nextActuals.size()))
        return false;

      state.replayPrefixLiterals.append(shape->prefixLiterals.begin(),
                                        shape->prefixLiterals.end());
      state.replaySuffixStack.push_back(shape->suffixLiterals);
      state.currentDefinition = nextDefinition;
      state.currentActuals = std::move(nextActuals);
      state.followedGeneratedCall = true;
      ++state.generatedCallDepth;
    }
    return true;
  }

private:
  const RefoldMacroGeneratedCalleeReplayEngine::Dependencies &deps_;
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
/// out-of-range formal references, and `__VA_OPT__` all fail closed.  The
/// stringification and paste proof facts are meaningful only after a successful
/// parse, and are raised only for replay elements that are emitted into the
/// accepted pattern.
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
        if (i + 1 >= end ||
            tokens[i + 1].kind !=
                RefoldModel::MacroReplacementTokenKind::ParamRef ||
            !tokens[i + 1].paramIndex)
          return false;
        const uint32_t paramIdx = *tokens[i + 1].paramIndex;
        if (paramIdx >= oldActuals_.size())
          return false;
        usesStringification_ = true;
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
        usesPaste_ = true;
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


/// Resolves the final generated-callee actual solution back to root arguments.
///
/// Replay-tape construction proves which final callee definition and actuals are
/// reached.  This resolver owns the next deterministic proof obligation: build
/// the final replay pattern, solve old and new expansion surfaces with
/// `FormalActualConstraintSolver`, and compose the resulting solved actual
/// differences back onto the original root invocation arguments.  It deliberately
/// does not construct `MacroPatch` objects or certify proofs; those observable
/// candidate-construction steps stay at the public builder boundary.
class FinalGeneratedCalleeSolutionResolver {
public:
  explicit FinalGeneratedCalleeSolutionResolver(
      const RefoldMacroGeneratedCalleeReplayEngine::Dependencies &deps)
      : deps_(deps) {}

  /// Returns the uniquely solved root-argument replacement set, if provable.
  ///
  /// The method preserves the old local algorithm exactly: final actuals are
  /// assembled in callee-formal order, the replay pattern keeps prefix literals,
  /// the final callee body, and suffix literals in the same order, solver calls
  /// are made old expansion first and new expansion second, and same-root edits
  /// are composed only when each solved obligation can be uniquely inverted.
  std::optional<FinalGeneratedCalleeSolution> Resolve(
      const RefoldModel::MacroInvocation &m, StringRef baseInvocationText,
      ArrayRef<std::pair<size_t, size_t>> invocationArgRanges,
      const std::pair<uint64_t, uint64_t> &wholeCoverATokens,
      const std::pair<size_t, size_t> &bTokenEnvelope,
      GeneratedReplayChainState &replayState) const {
    const bool finalHasVariadic =
        !replayState.currentDefinition->defParams.empty() &&
        replayState.currentDefinition->defParams.back().variadic;
    const size_t finalFixed =
        finalHasVariadic
            ? replayState.currentDefinition->defParams.size() - 1
            : replayState.currentDefinition->defParams.size();

    FinalGeneratedCalleeSolution solution;
    for (size_t i = 0; i < finalFixed; ++i) {
      solution.oldActuals.push_back(replayState.currentActuals[i].text);
      solution.rootSlotByFinalParam.push_back(
          replayState.currentActuals[i].rootArgIdx);
      solution.rootSourceByFinalParam.push_back(
          replayState.currentActuals[i].rootSourceText);
    }
    if (finalHasVariadic) {
      std::string variadicText;
      raw_string_ostream os(variadicText);
      for (size_t i = finalFixed; i < replayState.currentActuals.size(); ++i) {
        if (i != finalFixed)
          os << ", ";
        os << StringRef(replayState.currentActuals[i].text).trim();
      }
      os.flush();
      solution.oldActuals.push_back(std::move(variadicText));
      solution.rootSlotByFinalParam.push_back(
          replayState.currentActuals[finalFixed].rootArgIdx);
      solution.rootSourceByFinalParam.push_back(
          replayState.currentActuals[finalFixed].rootSourceText);
    }
    if (solution.oldActuals.size() !=
            replayState.currentDefinition->defParams.size() ||
        solution.rootSlotByFinalParam.size() != solution.oldActuals.size() ||
        solution.rootSourceByFinalParam.size() != solution.oldActuals.size())
      return std::nullopt;

    std::vector<GeneratedReplayElem> finalPattern;
    const GeneratedCalleeReplayPatternParser finalPatternParser(
        *replayState.currentDefinition,
        ArrayRef<std::string>(solution.oldActuals.data(),
                              solution.oldActuals.size()),
        replayState.usesStringification, replayState.usesPaste);
    if (!finalPatternParser.Parse(
            0, replayState.currentDefinition->replacementTokens.size(),
            finalPattern) ||
        finalPattern.empty())
      return std::nullopt;

    std::vector<GeneratedReplayElem> replayPattern;
    for (const std::string &literal : replayState.replayPrefixLiterals) {
      GeneratedReplayElem elem;
      elem.kind = GeneratedReplayKind::Literal;
      elem.literal = literal;
      replayPattern.push_back(std::move(elem));
    }
    replayPattern.insert(replayPattern.end(), finalPattern.begin(),
                         finalPattern.end());
    for (auto it = replayState.replaySuffixStack.rbegin();
         it != replayState.replaySuffixStack.rend(); ++it) {
      for (const std::string &literal : *it) {
        GeneratedReplayElem elem;
        elem.kind = GeneratedReplayKind::Literal;
        elem.literal = literal;
        replayPattern.push_back(std::move(elem));
      }
    }

    const FormalActualConstraintSolver formalSolver(
        ArrayRef<GeneratedReplayElem>(replayPattern.data(),
                                      replayPattern.size()),
        ArrayRef<std::string>(solution.oldActuals.data(),
                              solution.oldActuals.size()),
        deps_.lexLang);

    StringRef oldExpansion = deps_.sourceMapper
                                 .SliceASource(wholeCoverATokens.first,
                                               wholeCoverATokens.second)
                                 .trim();
    StringRef newExpansion = deps_.sourceMapper
                                 .SliceBSource(bTokenEnvelope.first,
                                               bTokenEnvelope.second)
                                 .trim();
    std::optional<GeneratedSolvedActuals> oldSolved =
        formalSolver.SolveExpansion(oldExpansion);
    std::optional<GeneratedSolvedActuals> newSolved =
        formalSolver.SolveExpansion(newExpansion);
    if (!oldSolved || !newSolved ||
        oldSolved->size() != solution.oldActuals.size() ||
        newSolved->size() != solution.oldActuals.size())
      return std::nullopt;

    for (uint32_t i = 0; i < newSolved->size(); ++i) {
      if (i >= solution.rootSlotByFinalParam.size())
        return std::nullopt;
      const uint32_t rootIdx = solution.rootSlotByFinalParam[i];
      if (rootIdx >= invocationArgRanges.size())
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
          baseInvocationText
              .slice(invocationArgRanges[rootIdx].first,
                     invocationArgRanges[rootIdx].second)
              .trim();
      auto workingIt = solution.workingRootTextByArgIdx.find(rootIdx);
      StringRef source = workingIt != solution.workingRootTextByArgIdx.end()
                             ? StringRef(workingIt->second)
                             : (i < solution.rootSourceByFinalParam.size()
                                    ? StringRef(
                                          solution.rootSourceByFinalParam[i])
                                    : StringRef(solution.oldActuals[i]));

      if (workingIt != solution.workingRootTextByArgIdx.end()) {
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
            originalRoot, StringRef((*oldSolved)[i]),
            StringRef((*newSolved)[i]), deps_.lexLang);
        if (alreadySatisfied && generatedCalleeTextsTokenEquivalent(
                                    StringRef(*alreadySatisfied), source,
                                    deps_.lexLang))
          continue;
      }

      auto rewritten = rewriteSourceActualFromSolvedExpansion(
          source, StringRef((*oldSolved)[i]), StringRef((*newSolved)[i]),
          deps_.lexLang);
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
      solution.workingRootTextByArgIdx[rootIdx] = std::move(*rewritten);
      StringRef finalRoot =
          StringRef(solution.workingRootTextByArgIdx[rootIdx]).trim();
      if (finalRoot != originalRoot)
        solution.replacementsByRootArgIdx[rootIdx] = finalRoot.str();
      else
        solution.replacementsByRootArgIdx.erase(rootIdx);
    }

    if (solution.replacementsByRootArgIdx.empty())
      return std::nullopt;
    return solution;
  }

private:
  const RefoldMacroGeneratedCalleeReplayEngine::Dependencies &deps_;
};

/// Builds and certifies generated-callee replay patches.
///
/// The builder owns only the observable candidate-construction boundary after
/// replay discovery has succeeded.  Normal generated-callee replay still
/// rebuilds the invocation from root-argument replacements, while tuple replay
/// consumes the tuple resolver's already-rebuilt invocation spelling.  Both
/// paths preserve the old proof order exactly: certify the materialized output
/// range, certify the B-token envelope, attach the standard args-only proof,
/// and finally certify generated-callee replay fields.  Any failed rewrite
/// remains a fail-closed miss rather than a weaker fallback candidate.
class GeneratedCalleeProofBuilder {
public:
  explicit GeneratedCalleeProofBuilder(
      const RefoldMacroGeneratedCalleeReplayEngine::Dependencies &deps)
      : deps_(deps) {}

  /// Constructs the generated-callee replay `MacroPatch`, if still provable.
  ///
  /// The method intentionally performs no replay admission or solver work.  It
  /// consumes the deterministic replacement map and borrowed proof flags that
  /// earlier resolvers produced, so patch fields, proof summaries, trace data,
  /// and materialized range certification remain byte-for-byte comparable with
  /// the former inline construction block.
  std::optional<MacroPatch> BuildGeneratedCalleePatch(
      const GeneratedCalleeReplayContext &generatedCalleeCtx,
      const GeneratedReplayChainState &replayState,
      const FinalGeneratedCalleeSolution &finalSolution) const {
    const RefoldModel::MacroInvocation &m = generatedCalleeCtx.invocation;
    InvocationActualRecoveryContext actualRecoveryCtx{
        m, generatedCalleeCtx.baseInvocationText,
        generatedCalleeCtx.invocationArgRanges};

    std::optional<InvocationRewriteWithRange> rewrite =
        deps_.buildInvocationRewriteWithRange(
            actualRecoveryCtx, finalSolution.replacementsByRootArgIdx,
            /*materializedRangeByArgIdx=*/nullptr);
    if (!rewrite)
      return std::nullopt;

    MacroPatch patch{*m.invB, *m.invE, std::move(rewrite->text), m.id};
    deps_.proofCertifier.CertifyInvocationRewriteMaterializedOutputRange(
        patch, rewrite->materializedOutputByteStart,
        rewrite->materializedOutputByteEnd);
    certifyMacroPatchMaterializedBTokenRange(
        patch, static_cast<uint64_t>(generatedCalleeCtx.bTokenEnvelope.first),
        static_cast<uint64_t>(generatedCalleeCtx.bTokenEnvelope.second));
    deps_.proofCertifier.SetArgsOnlyStandardProof(
        patch, m, /*wholeEnvelopeReplayValidated=*/true);
    deps_.proofCertifier.CertifyGeneratedCalleeReplayProof(
        patch, m,
        replayState.currentDefinition ? replayState.currentDefinition->id : 0,
        replayState.generatedCallDepth, replayState.objectAliasHopCount,
        replayState.usesStringification, replayState.usesPaste,
        replayState.usesVariadicForwarding,
        /*decodedStringLiteralEvidenceOnly=*/replayState.usesStringification);
    return patch;
  }

  /// Constructs the tuple-generated replay `MacroPatch`, if still provable.
  ///
  /// Tuple replay has already rebuilt the invocation spelling and proven the
  /// final callee identity.  This method owns only the observable certification
  /// boundary: materialized output range, B-token envelope, standard args-only
  /// proof, and generated-callee proof fields.  Keeping those calls together
  /// preserves proof summaries and prevents replay discovery from mutating
  /// patch state before all tuple obligations have succeeded.
  std::optional<MacroPatch> BuildTupleGeneratedCalleePatch(
      const TupleGeneratedCalleeReplayContext &tupleGeneratedCtx,
      TupleGeneratedReplaySolution tupleSolution) const {
    const RefoldModel::MacroInvocation &m = tupleGeneratedCtx.invocation;

    MacroPatch patch{*m.invB, *m.invE,
                     std::move(tupleSolution.rewrittenInvocation), m.id};
    patch.materialized.outputByteStart = 0;
    patch.materialized.outputByteEnd = patch.replacement.size();
    patch.materialized.hasOutputByteRange = true;
    certifyMacroPatchMaterializedBTokenRange(
        patch, static_cast<uint64_t>(tupleGeneratedCtx.bTokenEnvelope.first),
        static_cast<uint64_t>(tupleGeneratedCtx.bTokenEnvelope.second));
    deps_.proofCertifier.SetArgsOnlyStandardProof(
        patch, m, /*wholeEnvelopeReplayValidated=*/true);
    deps_.proofCertifier.CertifyGeneratedCalleeReplayProof(
        patch, m,
        tupleSolution.calleeDefinition ? tupleSolution.calleeDefinition->id : 0,
        /*generatedCallDepth=*/1, tupleGeneratedCtx.objectAliasHopCount,
        tupleSolution.usesStringification, tupleSolution.usesPaste,
        tupleSolution.usesVariadicForwarding,
        /*decodedStringLiteralEvidenceOnly=*/tupleSolution.usesStringification);
    return patch;
  }

private:
  const RefoldMacroGeneratedCalleeReplayEngine::Dependencies &deps_;
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
/// order.  Tuple-specific stringification and paste facts are meaningful only
/// after a successful parse, and are raised only for replay elements emitted
/// into the accepted tuple pattern.
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
  /// facts are raised only for replay forms emitted into the accepted pattern.
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
        usesPaste_ = true;
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


/// Resolves the tuple-generated replay proof before patch certification.
///
/// The resolver owns the tuple-side algorithm that was formerly inline in
/// `BuildTupleGeneratedCalleeReplayCandidate`: tuple element extraction,
/// generated forwarder shape discovery, direct forwarder/callee proof,
/// old/new generated actual solving, right-to-left tuple edit construction, and
/// overlap rejection.  It deliberately does not construct or certify a
/// `MacroPatch`; the public engine method keeps that proof boundary so candidate
/// ranking, materialized-range certification, and proof summaries remain in the
/// same observable location.
class TupleReplayResolver {
public:
  explicit TupleReplayResolver(
      const RefoldMacroGeneratedCalleeReplayEngine::Dependencies &deps)
      : deps_(deps) {}

  /// Builds the rewritten invocation for one tuple-generated replay candidate.
  ///
  /// Returns nullopt for every unproved shape, ambiguous forwarder/callee proof,
  /// unsupported generated-callee replay element, non-unique solve, conflicting
  /// tuple edit, or no-op rewrite.  The only allowed mutation is the existing
  /// alias-hop accounting through `ctx.objectAliasHopCount`, performed at the
  /// same point as the former inline code after resolving the generated callee.
  std::optional<TupleGeneratedReplaySolution>
  Resolve(const TupleGeneratedCalleeReplayContext &ctx) const {
    StringRef baseInvText = ctx.baseInvocationText;
    const std::pair<uint64_t, uint64_t> *cover = &ctx.wholeCoverATokens;
    const std::pair<size_t, size_t> *bEnv = &ctx.bTokenEnvelope;

    const auto argRange = ctx.invocationArgRanges[ctx.callerArgIdx];
    if (argRange.second < argRange.first ||
        argRange.second > ctx.baseInvocationText.size())
      return std::nullopt;
    StringRef parentTrim = ctx.baseInvocationText
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

    TupleGeneratedForwarderState tupleState;
    tupleState.tupleElementTexts.reserve(tupleElems.size());
    for (const TupleElementSlice &elem : tupleElems) {
      tupleState.tupleElementTexts.push_back(
          tuplePayload.slice(elem.trimBegin, elem.trimEnd).trim().str());
    }
    tupleState.rebuiltPayload = tuplePayload.str();

    const auto &forwarderToks = ctx.forwarderDefinition.replacementTokens;

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
        calleeForwarderParam >= ctx.forwarderDefinition.defParams.size() ||
        calleeForwarderParam >= tupleElems.size())
      return std::nullopt;

    SmallVector<std::string, 8> forwarderPrefixLiterals;
    SmallVector<std::string, 8> forwarderSuffixLiterals;
    if (!appendGeneratedReplayLiteralRange(forwarderToks, 0,
                                           generatedCallBegin,
                                           forwarderPrefixLiterals) ||
        !appendGeneratedReplayLiteralRange(forwarderToks,
                                           generatedCallClose + 1,
                                           forwarderToks.size(),
                                           forwarderSuffixLiterals)) {
      return std::nullopt;
    }

    for (size_t i = generatedCallOpen + 1; i < generatedCallClose; ++i) {
      const auto &tok = forwarderToks[i];
      if (tok.spelling == "#" || tok.spelling == "##" ||
          tok.spelling == "__VA_OPT__")
        return std::nullopt;
      if (tok.kind != RefoldModel::MacroReplacementTokenKind::ParamRef)
        continue;
      if (!tok.paramIndex ||
          *tok.paramIndex >= ctx.forwarderDefinition.defParams.size())
        return std::nullopt;
      if (*tok.paramIndex == calleeForwarderParam)
        return std::nullopt;
      TupleGeneratedArgRef ref;
      ref.forwarderParamIdx = *tok.paramIndex;
      ref.variadicPack = ctx.forwarderDefinition.defParams[*tok.paramIndex]
                             .variadic;
      tupleState.generatedArgRefs.push_back(ref);
    }
    if (tupleState.generatedArgRefs.empty())
      return std::nullopt;

    uint32_t calleeAliasHops = 0;
    const RefoldModel::MacroDirective *calleeDefinition =
        deps_.resolveFunctionLikeMacroThroughAliasesWithHops(
            tupleState.TupleElementText(static_cast<size_t>(calleeForwarderParam)),
            &calleeAliasHops);
    ctx.objectAliasHopCount += calleeAliasHops;
    if (!calleeDefinition || calleeDefinition->defParams.empty())
      return std::nullopt;

    for (const TupleGeneratedArgRef &ref : tupleState.generatedArgRefs) {
      if (ref.variadicPack) {
        if (ref.forwarderParamIdx >= tupleElems.size())
          return std::nullopt;
        for (size_t i = ref.forwarderParamIdx; i < tupleElems.size(); ++i)
          tupleState.oldGeneratedPieces.push_back(
              tupleState.TupleElementText(i).str());
        continue;
      }
      if (ref.forwarderParamIdx >= tupleElems.size())
        return std::nullopt;
      tupleState.oldGeneratedPieces.push_back(
          tupleState.TupleElementText(ref.forwarderParamIdx).str());
    }

    const bool calleeHasVariadic = !calleeDefinition->defParams.empty() &&
                                   calleeDefinition->defParams.back().variadic;
    bool tupleReplayUsesStringification = false;
    bool tupleReplayUsesPaste = false;
    bool tupleReplayUsesVariadicForwarding = false;
    for (const TupleGeneratedArgRef &ref : tupleState.generatedArgRefs)
      tupleReplayUsesVariadicForwarding |= ref.variadicPack;
    const size_t fixedCalleeActuals = calleeHasVariadic
                                          ? calleeDefinition->defParams.size() - 1
                                          : calleeDefinition->defParams.size();
    if ((!calleeHasVariadic &&
         tupleState.oldGeneratedPieces.size() !=
             calleeDefinition->defParams.size()) ||
        (calleeHasVariadic &&
         tupleState.oldGeneratedPieces.size() < fixedCalleeActuals))
      return std::nullopt;

    tupleState.oldActuals.reserve(calleeDefinition->defParams.size());
    for (size_t i = 0; i < fixedCalleeActuals; ++i)
      tupleState.oldActuals.push_back(tupleState.oldGeneratedPieces[i]);
    if (calleeHasVariadic) {
      std::string variadicText;
      raw_string_ostream os(variadicText);
      for (size_t i = fixedCalleeActuals;
           i < tupleState.oldGeneratedPieces.size(); ++i) {
        if (i != fixedCalleeActuals)
          os << ", ";
        os << StringRef(tupleState.oldGeneratedPieces[i]).trim();
      }
      os.flush();
      tupleState.oldActuals.push_back(std::move(variadicText));
    }
    if (tupleState.oldActuals.size() != calleeDefinition->defParams.size())
      return std::nullopt;

    StringRef oldExpansion =
        deps_.sourceMapper.SliceASource(cover->first, cover->second).trim();
    StringRef newExpansion =
        deps_.sourceMapper.SliceBSource(bEnv->first, bEnv->second).trim();

    // Replay the generated callee as a small replacement-list transducer over
    // tuple slots.  A generated callee contributes a sequence of literal
    // tokens, ordinary parameter projections, stringified projections, pasted-
    // token projections, and fixed forwarding-context tokens.  If that sequence
    // explains both the old whole-cover expansion and the new B-side expansion
    // uniquely, then the solved parameter values can be translated back to
    // positional tuple edits.
    std::vector<TupleCalleeReplayElem> calleePattern;
    const TupleGeneratedCalleeReplayPatternParser tuplePatternParser(
        *calleeDefinition,
        ArrayRef<std::string>(tupleState.oldActuals.data(),
                              tupleState.oldActuals.size()),
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
        ArrayRef<std::string>(tupleState.oldActuals.data(),
                              tupleState.oldActuals.size()),
        deps_.lexLang);

    std::optional<TupleSolvedActuals> oldSolved =
        tupleReplayResolver.SolveExpansion(oldExpansion);
    if (!oldSolved || oldSolved->size() != tupleState.oldActuals.size())
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
    if (!newSolved || newSolved->size() != tupleState.oldActuals.size())
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

    size_t pieceCursor = 0;
    for (const TupleGeneratedArgRef &ref : tupleState.generatedArgRefs) {
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
        tupleState.edits.push_back(TupleGeneratedEdit{firstElem.trimBegin,
                                                      lastElem.trimEnd,
                                                      std::move(text)});
        continue;
      }
      if (pieceCursor >= newGeneratedPieces.size() ||
          ref.forwarderParamIdx >= tupleElems.size())
        return std::nullopt;
      const TupleElementSlice &elem = tupleElems[ref.forwarderParamIdx];
      StringRef oldText = pieceCursor < oldSolved->size()
                              ? StringRef((*oldSolved)[pieceCursor]).trim()
                              : (pieceCursor < tupleState.oldActuals.size()
                                     ? StringRef(
                                           tupleState.oldActuals[pieceCursor])
                                           .trim()
                                     : StringRef(tupleState.oldGeneratedPieces[
                                                     pieceCursor])
                                           .trim());
      StringRef newText = StringRef(newGeneratedPieces[pieceCursor]).trim();
      if (newText != tuplePayload.slice(elem.trimBegin, elem.trimEnd).trim()) {
        auto rewrittenElem = rewriteTupleElementFromSolvedExpansion(
            tupleState.TupleElementText(ref.forwarderParamIdx), oldText, newText,
            deps_.lexLang);
        if (!rewrittenElem)
          return std::nullopt;
        tupleState.edits.push_back(TupleGeneratedEdit{elem.trimBegin,
                                                      elem.trimEnd,
                                                      std::move(*rewrittenElem)});
      }
      ++pieceCursor;
    }
    if (pieceCursor != newGeneratedPieces.size() || tupleState.edits.empty())
      return std::nullopt;

    llvm::sort(tupleState.edits, [](const TupleGeneratedEdit &lhs,
                                    const TupleGeneratedEdit &rhs) {
      if (lhs.begin != rhs.begin)
        return lhs.begin > rhs.begin;
      return lhs.end > rhs.end;
    });

    size_t previousBegin = std::numeric_limits<size_t>::max();
    for (const TupleGeneratedEdit &edit : tupleState.edits) {
      if (edit.end < edit.begin || edit.end > tupleState.rebuiltPayload.size())
        return std::nullopt;
      if (previousBegin != std::numeric_limits<size_t>::max() &&
          edit.end > previousBegin)
        return std::nullopt;
      previousBegin = edit.begin;
      tupleState.rebuiltPayload = stringutils::replaceRange(
          tupleState.rebuiltPayload, edit.begin, edit.end, edit.text);
    }

    std::string rewrittenArg =
        ("(" + StringRef(tupleState.rebuiltPayload).trim().str() + ")");
    std::string rewrittenInv = stringutils::replaceRange(
        baseInvText.str(), argRange.first, argRange.second, rewrittenArg);
    if (StringRef(rewrittenInv).trim() == baseInvText.trim())
      return std::nullopt;

    TupleGeneratedReplaySolution solution;
    solution.rewrittenInvocation = std::move(rewrittenInv);
    solution.calleeDefinition = calleeDefinition;
    solution.usesStringification = tupleReplayUsesStringification;
    solution.usesPaste = tupleReplayUsesPaste;
    solution.usesVariadicForwarding = tupleReplayUsesVariadicForwarding;
    return solution;
  }

private:
  const RefoldMacroGeneratedCalleeReplayEngine::Dependencies &deps_;
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

std::optional<TerminalGeneratedCalleeReplaySolution>
RefoldMacroGeneratedCalleeReplayEngine::SolveTerminalGeneratedCalleeReplay(
    const TerminalGeneratedCalleeReplayRequest &ctx) const {
  if (ctx.wholeCoverATokens.first >= ctx.wholeCoverATokens.second ||
      ctx.bTokenEnvelope.first >= ctx.bTokenEnvelope.second)
    return std::nullopt;
  if (!ctx.terminalDefinition.functionLike || ctx.oldActuals.empty() ||
      ctx.terminalDefinition.defParams.size() != ctx.oldActuals.size())
    return std::nullopt;
  if (!macroDefinitionAcceptsActualCount(ctx.terminalDefinition,
                                         ctx.oldActuals.size()))
    return std::nullopt;

  bool usesStringification = false;
  bool usesPaste = false;
  std::vector<GeneratedReplayElem> replayPattern;
  const GeneratedCalleeReplayPatternParser replayPatternParser(
      ctx.terminalDefinition, ctx.oldActuals, usesStringification, usesPaste);
  if (!replayPatternParser.Parse(
          0, ctx.terminalDefinition.replacementTokens.size(), replayPattern) ||
      replayPattern.empty())
    return std::nullopt;

  const FormalActualConstraintSolver formalSolver(
      ArrayRef<GeneratedReplayElem>(replayPattern.data(),
                                    replayPattern.size()),
      ctx.oldActuals, deps_.lexLang);

  StringRef oldExpansion = deps_.sourceMapper
                               .SliceASource(ctx.wholeCoverATokens.first,
                                             ctx.wholeCoverATokens.second)
                               .trim();
  StringRef newExpansion = deps_.sourceMapper
                               .SliceBSource(ctx.bTokenEnvelope.first,
                                             ctx.bTokenEnvelope.second)
                               .trim();

  std::optional<GeneratedSolvedActuals> oldSolved =
      formalSolver.SolveExpansion(oldExpansion);
  std::optional<GeneratedSolvedActuals> newSolved =
      formalSolver.SolveExpansion(newExpansion);
  if (!oldSolved || !newSolved || oldSolved->size() != ctx.oldActuals.size() ||
      newSolved->size() != ctx.oldActuals.size())
    return std::nullopt;

  TerminalGeneratedCalleeReplaySolution solution;
  solution.oldSolvedActuals.assign(oldSolved->begin(), oldSolved->end());
  solution.newSolvedActuals.assign(newSolved->begin(), newSolved->end());
  solution.usesStringification = usesStringification;
  solution.usesPaste = usesPaste;
  return solution;
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
  GeneratedReplayChainState replayState(generatedCalleeCtx);

  // Replay tape construction owns only chain stepping.  Final admission and
  // proof construction are delegated after solving so candidate ranking stays
  // anchored at this public builder boundary.
  if (!ReplayTapeBuilder(deps_).Build(replayState))
    return std::nullopt;

  if (!replayState.followedGeneratedCall || !replayState.currentDefinition ||
      replayState.currentActuals.empty() ||
      !macroDefinitionAcceptsActualCount(*replayState.currentDefinition,
                                         replayState.currentActuals.size()))
    return std::nullopt;

  std::optional<FinalGeneratedCalleeSolution> finalSolution =
      FinalGeneratedCalleeSolutionResolver(deps_).Resolve(
          m, baseInvText, invArgRanges, *cover, *bEnv, replayState);
  if (!finalSolution)
    return std::nullopt;

  return GeneratedCalleeProofBuilder(deps_).BuildGeneratedCalleePatch(
      generatedCalleeCtx, replayState, *finalSolution);
}

std::optional<MacroPatch> RefoldMacroGeneratedCalleeReplayEngine::
    BuildTupleGeneratedCalleeReplayCandidate(
        const TupleGeneratedCalleeReplayContext &tupleGeneratedCtx) const {
  std::optional<TupleGeneratedReplaySolution> tupleSolution =
      TupleReplayResolver(deps_).Resolve(tupleGeneratedCtx);
  if (!tupleSolution)
    return std::nullopt;

  return GeneratedCalleeProofBuilder(deps_).BuildTupleGeneratedCalleePatch(
      tupleGeneratedCtx, std::move(*tupleSolution));
}

} // namespace refold
} // namespace clang
