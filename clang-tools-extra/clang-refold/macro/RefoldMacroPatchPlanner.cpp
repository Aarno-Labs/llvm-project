//===--- RefoldMacroPatchPlanner.cpp ---------------------------*- C++ -*-===//
//
// Macro invocation patch planning service for clang-refold.
//
// This translation unit owns macro patch planning.  The large historical
// algorithms are kept structurally intact here, but their state now comes from
// explicit planner dependencies instead of a monolithic-engine back-reference.
// Helper methods and carrier structs stay here only where they still belong
// to macro patch orchestration.
//
//===----------------------------------------------------------------------===//

#include "macro/RefoldMacroPatchPlanner.h"
#include "core/RefoldLog.h"
#include "edit/RefoldBInsertionLedger.h"
#include "core/RefoldOwnerClassifier.h"
#include "line-control/RefoldLineObserverLayout.h"
#include "macro/RefoldArgTextRecovery.h"
#include "macro/RefoldMacroReplay.h"
#include "macro/RefoldMacroStateProof.h"
#include "macro/RefoldMacroTextUtils.h"
#include "proof/RefoldOwnerStateProof.h"
#include "proof/RefoldProofLattice.h"
#include "source/TokenTextHelpers.h"
#include "util/StringUtils.h"

#include "clang/Basic/SourceLocation.h"
#include "clang/Basic/TokenKinds.h"
#include "clang/Lex/Lexer.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/STLFunctionalExtras.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/Support/FormatVariadic.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <functional>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

using namespace llvm;

namespace clang {
namespace refold {

RefoldMacroPatchPlanner::RefoldMacroPatchPlanner(Dependencies deps)
    : deps_(std::move(deps)) {
  assert(deps_.model && "macro planner requires a model");
  assert(deps_.abTokHunks && "macro planner requires token hunks");
  assert(deps_.bInsertionLedger &&
         "macro planner requires B insertion claim ledger");
  assert(deps_.argTextRecovery && "macro planner requires arg recovery");
  assert(deps_.lexLang && "macro planner requires lexer options");
  assert(deps_.lineDirs && "macro planner requires line directives");
  assert(deps_.macroTopology && "macro planner requires macro topology");
  assert(deps_.pathIdentity && "macro planner requires path identity");
  assert(deps_.sourceMapper && "macro planner requires source mapper");
  assert(deps_.ownerClassifier &&
         "macro planner requires owner/TU classifier");
  assert(deps_.macroStateProof && "macro planner requires macro-state proof");
  assert(deps_.ownerStateProof && "macro planner requires owner-state proof");
  assert(deps_.proofLattice && "macro planner requires proof lattice");
}

//===----------------------------------------------------------------------===//
// Borrowed proof service accessors
//===----------------------------------------------------------------------===//

RefoldMacroStateProof &RefoldMacroPatchPlanner::GetMacroStateProof() const {
  assert(deps_.macroStateProof &&
         "macro planner requires macro-state proof");
  return *deps_.macroStateProof;
}

RefoldOwnerStateProof &RefoldMacroPatchPlanner::GetOwnerStateProof() const {
  assert(deps_.ownerStateProof &&
         "macro planner requires owner-state proof");
  return *deps_.ownerStateProof;
}

RefoldProofLattice &RefoldMacroPatchPlanner::GetProofLattice() const {
  assert(deps_.proofLattice && "macro planner requires proof lattice");
  return *deps_.proofLattice;
}

//===----------------------------------------------------------------------===//
// Macro proof helper façades
//===----------------------------------------------------------------------===//
//
// The planner still exposes a small compatibility surface for call sites that
// already ask it for argument-layout and whole-cover information.  The actual
// proof work below is delegated to named macro-domain services, which keeps
// occurrence replay, actual-layout recovery, paste spelling, and whole-cover
// proof code out of the engine and out of the planner's main algorithm body.


RefoldMacroOccurrenceReplay RefoldMacroPatchPlanner::OccurrenceReplay() const {
  return RefoldMacroOccurrenceReplay({deps_.aToks, deps_.bTokOff,
                                      deps_.macroTopology,
                                      deps_.sourceMapper, deps_.strict});
}

RefoldMacroActualLayout RefoldMacroPatchPlanner::ActualLayout() const {
  return RefoldMacroActualLayout({deps_.lexLang});
}

bool RefoldMacroPatchPlanner::MacroArgReplacementMatchesAllOccurrencesInB(
    const RefoldModel::MacroInvocation &m, uint32_t argIdx, StringRef baseArg,
    StringRef newArg, ArrayRef<diffutils::Hunk> tokenHunks) const {
  return OccurrenceReplay().MacroArgReplacementMatchesAllOccurrencesInB(
      m, argIdx, baseArg, newArg, tokenHunks);
}

bool RefoldMacroPatchPlanner::
MacroArgReplacementMatchesAllOccurrencesInBIgnorePaste(
    const RefoldModel::MacroInvocation &m, uint32_t argIdx, StringRef baseArg,
    StringRef newArg, ArrayRef<diffutils::Hunk> tokenHunks) const {
  return OccurrenceReplay().MacroArgReplacementMatchesAllOccurrencesInBIgnorePaste(
      m, argIdx, baseArg, newArg, tokenHunks);
}

bool RefoldMacroPatchPlanner::
MacroArgReplacementMatchesAllOccurrencesInBIgnorePasteSemanticProof(
    const RefoldModel::MacroInvocation &m, uint32_t argIdx, StringRef baseArg,
    StringRef newArg, ArrayRef<diffutils::Hunk> tokenHunks) const {
  return OccurrenceReplay()
      .MacroArgReplacementMatchesAllOccurrencesInBIgnorePasteSemanticProof(
          m, argIdx, baseArg, newArg, tokenHunks);
}

std::optional<std::vector<std::pair<size_t, size_t>>>
RefoldMacroPatchPlanner::GetMacroInvocationFormalArgContentRanges(
    const RefoldModel::MacroInvocation &m, StringRef invText) const {
  return ActualLayout().GetMacroInvocationFormalArgContentRanges(m, invText);
}

std::optional<std::pair<uint64_t, uint64_t>>
RefoldMacroPatchPlanner::GetWholeCoverATokRange(
    const RefoldModel::MacroInvocation &m) const {
  return RefoldMacroWholeCoverProof::GetWholeCoverATokRange(m);
}


namespace {
// Include-directive source-spelling helpers moved to
// RefoldIncludeMaterializer.cpp with the include-materialization
// routines that use them.

/// Emit every byte cut point outside comments, literals, and nested delimiter
/// groups.
static void enumerateTopLevelBalancedCutPointsWithLexer(
    StringRef text, const LangOptions &lang,
    function_ref<void(unsigned)> emitCut);
// Include materialization path/load helpers now live with the include
// materialization implementation.

/// True iff the producer proved the invocation callee comes from a literal
/// macro name rather than from a formal/argument-derived callee position.
inline bool hasLiteralMacroCalleeOrigin(
    const RefoldModel::MacroInvocation &mi) {
  return mi.calleeOrigin.kind == MacroCalleeOriginKind::LiteralMacroName;
}

/// Decode the restricted string-literal payloads that macro stringification
/// proofs can invert without changing the existing fail-closed policy.
static std::optional<std::string>
decodeSimpleStringLiteralToken(StringRef spelling) {
  size_t quote = spelling.find('"');
  if (quote == StringRef::npos)
    return std::nullopt;
  size_t endQuote = spelling.rfind('"');
  if (endQuote == StringRef::npos || endQuote <= quote)
    return std::nullopt;
  StringRef body = spelling.slice(quote + 1, endQuote);
  std::string out;
  out.reserve(body.size());
  for (size_t i = 0; i < body.size(); ++i) {
    if (body[i] != '\\') {
      out.push_back(body[i]);
      continue;
    }
    if (++i >= body.size())
      return std::nullopt;
    // This generated-callee proof only inverts the ordinary escapes that
    // stringification introduces for spelling preservation. Numeric and
    // line-continuation escapes are left to the existing realization paths.
    switch (body[i]) {
    case '\\':
    case '"':
      out.push_back(body[i]);
      break;
    case 'n':
      out.push_back('\n');
      break;
    case 't':
      out.push_back('\t');
      break;
    default:
      return std::nullopt;
    }
  }
  return out;
}

/// Return the unique trimmed occurrence of \p needle in \p haystack.
static std::optional<std::pair<size_t, size_t>>
findUniqueTrimmedSubstring(StringRef haystack, StringRef needle) {
  needle = needle.trim();
  if (needle.empty())
    return std::nullopt;
  size_t pos = haystack.find(needle);
  if (pos == StringRef::npos)
    return std::nullopt;
  if (haystack.find(needle, pos + 1) != StringRef::npos)
    return std::nullopt;
  return std::make_pair(pos, pos + needle.size());
}

/// Build the minimal token-edit envelope spanning two pure-insertion frontiers.
static diffutils::Hunk buildCombinedInsertionEnvelope(
    const diffutils::Hunk &left, const diffutils::Hunk &right) {
  diffutils::Hunk env;
  env.aStart = std::min(left.aStart, right.aStart);
  env.aEnd = std::max(left.aStart, right.aStart);
  env.bStart = std::min(left.bStart, right.bStart);
  env.bEnd = std::max(left.bEnd, right.bEnd);
  return env;
}

/// Remove identical A/B token spelling at both edges of a candidate hunk.
static diffutils::Hunk trimCommonEdgeTokens(diffutils::Hunk hunk,
                                            ArrayRef<PPTok> aToks,
                                            ArrayRef<PPTok> bToks) {
  while (hunk.aStart < hunk.aEnd && hunk.bStart < hunk.bEnd) {
    size_t aIdx = static_cast<size_t>(hunk.aStart);
    size_t bIdx = static_cast<size_t>(hunk.bStart);
    if (aIdx >= aToks.size() || bIdx >= bToks.size())
      break;
    if (aToks[aIdx].spelling != bToks[bIdx].spelling)
      break;
    ++hunk.aStart;
    ++hunk.bStart;
  }
  while (hunk.aEnd > hunk.aStart && hunk.bEnd > hunk.bStart) {
    size_t aIdx = static_cast<size_t>(hunk.aEnd - 1);
    size_t bIdx = static_cast<size_t>(hunk.bEnd - 1);
    if (aIdx >= aToks.size() || bIdx >= bToks.size())
      break;
    if (aToks[aIdx].spelling != bToks[bIdx].spelling)
      break;
    --hunk.aEnd;
    --hunk.bEnd;
  }
  return hunk;
}

/// Copy text to a string while removing line breaks that may be included by
/// token-range slice helpers at pasted-token frontiers.
static std::string stripNewlines(StringRef text) {
  std::string result = text.str();
  result.erase(std::remove(result.begin(), result.end(), '\n'), result.end());
  return result;
}

/// Return true when a function-like macro formal is variadic.
static bool isMacroInvocationVariadicFormal(
    const RefoldModel::MacroInvocation &mi, size_t formalIdx) {
  return formalIdx < mi.defParams.size() && mi.defParams[formalIdx].variadic;
}

/// Return true when a macro directive parameter is variadic.
static bool isMacroDirectiveVariadicParam(
    const RefoldModel::MacroDirective &definition, uint32_t paramIdx) {
  return paramIdx < definition.defParams.size() &&
         definition.defParams[paramIdx].variadic;
}

/// Detect a comma that would split a non-variadic macro actual.
///
/// This helper deliberately forwards to the raw-lexer implementation so the
/// extracted call sites keep the same macro-argument collection semantics:
/// only parentheses protect commas; brackets and braces do not.
static bool replacementIntroducesTopLevelComma(StringRef text,
                                               const LangOptions &lang) {
  return refoldMacroActualHasTopLevelComma(text, lang);
}

/// Equality for producer argument-span records after canonical sorting.
static bool samePPArgSpan(const RefoldModel::PPArgSpan &lhs,
                          const RefoldModel::PPArgSpan &rhs) {
  return lhs.begin == rhs.begin && lhs.end == rhs.end &&
         lhs.argIdx == rhs.argIdx && lhs.kind == rhs.kind &&
         lhs.byteBegin == rhs.byteBegin && lhs.byteEnd == rhs.byteEnd &&
         lhs.ppByteBegin == rhs.ppByteBegin &&
         lhs.ppByteEnd == rhs.ppByteEnd;
}

/// Equality for token diff hunks.  Kept as a named helper so synthetic hunk
/// deduplication uses the same relation everywhere in the macro planner.
static bool sameTokenHunk(const diffutils::Hunk &lhs,
                          const diffutils::Hunk &rhs) {
  return lhs.aStart == rhs.aStart && lhs.aEnd == rhs.aEnd &&
         lhs.bStart == rhs.bStart && lhs.bEnd == rhs.bEnd;
}

/// Canonical ordering for token hunks before deterministic deduplication.
static bool tokenHunkLess(const diffutils::Hunk &lhs,
                          const diffutils::Hunk &rhs) {
  if (lhs.aStart != rhs.aStart)
    return lhs.aStart < rhs.aStart;
  if (lhs.aEnd != rhs.aEnd)
    return lhs.aEnd < rhs.aEnd;
  if (lhs.bStart != rhs.bStart)
    return lhs.bStart < rhs.bStart;
  return lhs.bEnd < rhs.bEnd;
}

/// Prefer spans that are more local in PP output; fall back to token width.
static uint64_t ppArgSpanProofWidth(const RefoldModel::PPArgSpan &span) {
  if (span.ppByteBegin && span.ppByteEnd && *span.ppByteEnd > *span.ppByteBegin)
    return uint64_t(*span.ppByteEnd - *span.ppByteBegin);
  if (span.end > span.begin)
    return uint64_t(span.end - span.begin);
  return ~uint64_t(0);
}

/// Count non-overlapping occurrences of needle in text.
static uint64_t countSubstringOccurrences(StringRef text, StringRef needle) {
  if (needle.empty())
    return 0;
  uint64_t count = 0;
  for (size_t pos = 0; (pos = text.find(needle, pos)) != StringRef::npos;
       pos += needle.size())
    ++count;
  return count;
}

/// Return the non-empty changed middles after trimming the longest common
/// prefix and suffix from two spellings.
static std::optional<std::pair<std::string, std::string>>
changedMiddleSubstrings(StringRef oldValue, StringRef newValue) {
  size_t prefix = 0;
  while (prefix < oldValue.size() && prefix < newValue.size() &&
         oldValue[prefix] == newValue[prefix])
    ++prefix;
  size_t suffix = 0;
  while (suffix + prefix < oldValue.size() &&
         suffix + prefix < newValue.size() &&
         oldValue[oldValue.size() - suffix - 1] ==
             newValue[newValue.size() - suffix - 1])
    ++suffix;
  StringRef oldMiddle = oldValue.slice(prefix, oldValue.size() - suffix);
  StringRef newMiddle = newValue.slice(prefix, newValue.size() - suffix);
  if (oldMiddle.empty() || newMiddle.empty())
    return std::nullopt;
  return std::make_pair(oldMiddle.str(), newMiddle.str());
}

/// Merge the materialized B-token envelope from one macro patch into another.
static void unionMacroPatchMaterializedBTokenRange(MacroPatch &dst,
                                                   const MacroPatch &src) {
  if (!src.hasMaterializedBTokenRange)
    return;
  if (!dst.hasMaterializedBTokenRange) {
    dst.hasMaterializedBTokenRange = true;
    dst.materializedBTokStart = src.materializedBTokStart;
    dst.materializedBTokEnd = src.materializedBTokEnd;
    return;
  }
  dst.materializedBTokStart =
      std::min(dst.materializedBTokStart, src.materializedBTokStart);
  dst.materializedBTokEnd =
      std::max(dst.materializedBTokEnd, src.materializedBTokEnd);
}

/// Compare token-spelling vectors without relying on a concrete container type.
static bool tokenSpellingVectorsEqual(ArrayRef<std::string> lhs,
                                      ArrayRef<std::string> rhs) {
  if (lhs.size() != rhs.size())
    return false;
  for (size_t i = 0; i < lhs.size(); ++i)
    if (lhs[i] != rhs[i])
      return false;
  return true;
}

/// Return true when a token hunk is wholly covered by one of the supplied
/// producer PP spans.
static bool hunkWithinPPSpans(const diffutils::Hunk &hunk,
                              ArrayRef<RefoldModel::PPSpan> spans) {
  for (const auto &span : spans)
    if (span.begin <= hunk.aStart && hunk.aEnd <= span.end &&
        span.begin < span.end)
      return true;
  return false;
}

/// Raw delimiter-balance summary used only by local recovery checks that have
/// already established that approximate byte-level balancing is sufficient.
struct DelimiterBalance {
  int paren = 0;
  int bracket = 0;
  int brace = 0;
};

static DelimiterBalance computeDelimiterBalance(StringRef text) {
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

/// Canonically order producer argument spans by token range and formal index.
static bool ppArgSpanLessByTokenRangeAndArg(
    const RefoldModel::PPArgSpan &lhs, const RefoldModel::PPArgSpan &rhs) {
  if (lhs.begin != rhs.begin)
    return lhs.begin < rhs.begin;
  if (lhs.end != rhs.end)
    return lhs.end < rhs.end;
  return lhs.argIdx < rhs.argIdx;
}

/// Canonically order producer argument spans by all identity-bearing fields.
static bool ppArgSpanLessByFullIdentity(const RefoldModel::PPArgSpan &lhs,
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

/// Sort argument-span pointers by ascending pasted-token byte contribution.
static bool ppArgSpanPtrLessByByteBegin(const RefoldModel::PPArgSpan *lhs,
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
static bool ppArgSpanGreaterByByteBegin(const RefoldModel::PPArgSpan &lhs,
                                        const RefoldModel::PPArgSpan &rhs) {
  if (!lhs.byteBegin)
    return false;
  if (!rhs.byteBegin)
    return true;
  if (*lhs.byteBegin != *rhs.byteBegin)
    return *lhs.byteBegin > *rhs.byteBegin;
  return lhs.byteEnd.value_or(0) > rhs.byteEnd.value_or(0);
}

/// Return true when a definition parameter is variadic.
static bool macroDefParamIsVariadic(const RefoldModel::MacroDefParam &param) {
  return param.variadic;
}

/// Return true when a macro definition accepts a written actual count.
static bool macroDefinitionAcceptsActualCount(
    const RefoldModel::MacroDirective &definition, size_t count) {
  const bool hasVariadic = !definition.defParams.empty() &&
                           definition.defParams.back().variadic;
  const size_t fixedCount = hasVariadic ? definition.defParams.size() - 1
                                        : definition.defParams.size();
  return hasVariadic ? count >= fixedCount : count == fixedCount;
}

/// Order tuple argument references by caller spelling position.
static bool tupleArgRefLessByCallerByteBegin(
    const RefoldModel::TupleArgRef &lhs,
    const RefoldModel::TupleArgRef &rhs) {
  return lhs.callerByteBegin < rhs.callerByteBegin;
}

/// Sort invocation argument references from right to left for text rewriting.
static bool invArgRefGreaterByByteRange(const RefoldModel::InvArgRef &lhs,
                                        const RefoldModel::InvArgRef &rhs) {
  if (lhs.byteBegin != rhs.byteBegin)
    return lhs.byteBegin > rhs.byteBegin;
  return lhs.byteEnd > rhs.byteEnd;
}

/// Canonically order producer PP spans by token range.
static bool ppSpanLessByTokenRange(const RefoldModel::PPSpan &lhs,
                                   const RefoldModel::PPSpan &rhs) {
  if (lhs.begin != rhs.begin)
    return lhs.begin < rhs.begin;
  return lhs.end < rhs.end;
}

/// Stamp the materialized B-token envelope on a macro patch.
static void stampMacroPatchMaterializedBTokenRange(MacroPatch &patch,
                                                   uint64_t bTokBegin,
                                                   uint64_t bTokEnd) {
  patch.hasMaterializedBTokenRange = true;
  patch.materializedBTokStart = bTokBegin;
  patch.materializedBTokEnd = bTokEnd;
}

/// Deduplicate equivalent standard argument spans after deterministic sorting.
static void
canonicalizeStandardArgSpans(SmallVectorImpl<RefoldModel::PPArgSpan> &spans) {
  llvm::sort(spans, ppArgSpanLessByFullIdentity);
  spans.erase(std::unique(spans.begin(), spans.end(), samePPArgSpan),
              spans.end());
}


/// Normalized replacement-list node used by the definition-tape replay solver.
///
/// Keeping this carrier at file scope makes replay-tree helpers nameable without
/// changing the solver's fail-closed matching semantics.  Literal nodes consume
/// fixed replacement-list tokens, parameter nodes choose a B-token interval for
/// one formal, and VA_OPT nodes contain the recursively parsed optional payload.
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

/// Boundary-lexed token carrier for leaf-level generated-callee text checks.
struct LeafTok {
  std::string spelling;
};

/// Boundary-lexed token carrier for parent-tuple generated-callee replay.
struct ParentTupleCalleeReplayTok {
  std::string spelling;
  size_t begin = 0;
  size_t end = 0;
};

/// Boundary-lexed token with byte offsets inside a replay text slice.
///
/// Several generated-callee and tuple-replay solvers use the same lightweight
/// spelling/begin/end carrier.  Keeping the type at file scope lets those
/// token-comparison helpers become named operations without changing the
/// solvers' local search state.
struct ReplayTok {
  std::string spelling;
  size_t begin = 0;
  size_t end = 0;
};

/// A token interval in A-space used by fixed-body and argument-dependent
/// replay guards.
struct TokenInterval {
  uint64_t begin = 0;
  uint64_t end = 0;
};

/// One element in the whole-envelope replay tiling: either fixed replacement
/// body surface or argument-dependent output surface.
struct WholeEnvelopeReplayElem {
  bool isArgumentDependent = false;
  uint64_t aBegin = 0;
  uint64_t aEnd = 0;
};

/// Child invocation projection used when a current-level formal slot contains
/// a nested macro call that must be replayed through old/new expansion text.
struct CurrentLevelChildSlotRewrite {
  size_t relBegin = 0;
  size_t relEnd = 0;
  std::string oldExpansion;
  std::string newExpansion;
  std::string newSyntax;
};

/// Replacement-list shape for one deterministic generated call step.
///
/// The argument ranges remain replacement-token ranges so the existing solver
/// can invert constructors such as `(X)`, `pre_##X`, active `__VA_OPT__` tails,
/// and nested generated-call expressions without flattening source identity.
struct GeneratedCalleeCallShape {
  uint32_t calleeParamIdx = 0;
  SmallVector<std::pair<size_t, size_t>, 8> argTokenRanges;
  SmallVector<std::string, 8> prefixLiterals;
  SmallVector<std::string, 8> suffixLiterals;
};

/// Return whether a patch is a structure-preserving callsite rewrite for the
/// current invocation root.  Expanded/materialized replacements intentionally
/// fail this pure predicate and are tracked separately by the reuse context.
static bool macroPatchIsCallsiteForInvocation(
    const MacroPatch &patch, const RefoldModel::MacroInvocation &invocation) {
  return patch.proof.preservesInvocationStructure &&
         patch.proof.proofRootMacroId == invocation.id &&
         RefoldLineObserverLayout::InvocationSpanMatchesCallsitePrefix(
             patch.replacement, invocation);
}

/// Deterministic ordering for equivalent definition-tape replay solutions.
///
/// This preserves the original canonical representative policy: lower
/// non-empty deviation wins, then stronger preservation of unused/empty/zero
/// token surfaces, then lexicographic invocation spelling.
static bool definitionTapeScoredSolutionLess(
    const DefinitionTapeScoredSolution &lhs,
    const DefinitionTapeScoredSolution &rhs) {
  if (lhs.nonEmptyDeviation != rhs.nonEmptyDeviation)
    return lhs.nonEmptyDeviation < rhs.nonEmptyDeviation;
  if (lhs.unusedFormalPreservedCount != rhs.unusedFormalPreservedCount)
    return lhs.unusedFormalPreservedCount > rhs.unusedFormalPreservedCount;
  if (lhs.emptySlotTokenCount != rhs.emptySlotTokenCount)
    return lhs.emptySlotTokenCount > rhs.emptySlotTokenCount;
  if (lhs.zeroTokenAssignedFormalCount != rhs.zeroTokenAssignedFormalCount)
    return lhs.zeroTokenAssignedFormalCount >
           rhs.zeroTokenAssignedFormalCount;
  if (lhs.vaOptIncludedCount != rhs.vaOptIncludedCount)
    return lhs.vaOptIncludedCount > rhs.vaOptIncludedCount;
  return lhs.rewritten < rhs.rewritten;
}

/// Compare a leaf-token sequence against an expected spelling vector.
static bool leafTokenSpellingsEqual(ArrayRef<LeafTok> toks,
                                    ArrayRef<std::string> expected) {
  if (toks.size() != expected.size())
    return false;
  for (size_t i = 0; i < toks.size(); ++i)
    if (toks[i].spelling != expected[i])
      return false;
  return true;
}

/// Compare a parent-tuple callee replay token subrange against expected
/// spellings.  The parent-tuple solver uses its own replay-token carrier, so
/// this helper is deliberately typed to that carrier instead of the generic
/// generated-callee replay token used by other local solvers.
static bool replayTokenRangeSpellingsEqual(
    ArrayRef<ParentTupleCalleeReplayTok> toks, size_t begin,
    ArrayRef<std::string> expected) {
  if (begin + expected.size() > toks.size())
    return false;
  for (size_t i = 0; i < expected.size(); ++i)
    if (toks[begin + i].spelling != expected[i])
      return false;
  return true;
}

/// Canonical ordering for token intervals.
static bool tokenIntervalLess(const TokenInterval &lhs,
                              const TokenInterval &rhs) {
  if (lhs.begin != rhs.begin)
    return lhs.begin < rhs.begin;
  return lhs.end < rhs.end;
}

/// Append a non-empty token interval.
static void addNonEmptyTokenInterval(SmallVectorImpl<TokenInterval> &out,
                                     uint64_t begin, uint64_t end) {
  if (begin < end)
    out.push_back({begin, end});
}

/// Recover the spelled text of one invocation argument from the producer-side
/// callsite surface recorded on a macro invocation.
///
/// The producer stores invocation argument byte ranges in TU-relative byte
/// coordinates. The consumer-side `invText` string, however, is a local slice
/// covering only the invocation text itself. This helper therefore rebases the
/// recorded argument byte range through `invB` before slicing `invText`.
///
/// Returns `std::nullopt` when the producer did not record a usable invocation
/// text/range pair for `argIdx`, or when the recorded range does not rebase
/// cleanly into the local invocation surface. Callers must treat failure as a
/// proof failure and remain fail-closed.
static std::optional<StringRef>
tryGetInvocationArgText(const RefoldModel::MacroInvocation &mi,
                        unsigned argIdx) {
  if (!mi.invText)
    return std::nullopt;
  if (argIdx >= mi.invArgRanges.size())
    return std::nullopt;

  const auto &range = mi.invArgRanges[argIdx];
  if (!range.first || !range.second)
    return std::nullopt;

  uint64_t byteBegin = *range.first;
  uint64_t byteEnd = *range.second;
  if (byteEnd < byteBegin)
    return std::nullopt;

  if (mi.invB) {
    if (byteBegin < *mi.invB || byteEnd < *mi.invB)
      return std::nullopt;
    byteBegin -= *mi.invB;
    byteEnd -= *mi.invB;
  }

  if (byteEnd > mi.invText->size() || byteBegin > byteEnd)
    return std::nullopt;

  return StringRef(*mi.invText)
      .slice(static_cast<size_t>(byteBegin), static_cast<size_t>(byteEnd));
}

/// Return true only for the narrowly proved higher-order callee-closure case:
///
///   * the callee of a descendant invocation comes from exactly one caller
///     formal slot in `parent`
///   * the spelled invocation text for that slot is exactly the parent formal
///     name itself (for example `F` in `APPLY(F, X)`)
///   * the slot forwards through exactly one `argRef`
///   * the slot has no tuple forwarding witnesses
///   * `argDeps` agrees with that single forwarded caller slot
///
/// This is intentionally narrower than "general higher-order callee closure".
/// We only admit the whole-formal forwarding shape that is explicitly proven by
/// the current producer contract. Any richer shape must continue to fail
/// closed until the producer emits stronger callee-slice provenance.
static bool isWholeFormalCallerForwardSlot(
    const RefoldModel::MacroInvocation &parent, uint32_t slot) {
  if (slot >= parent.defParams.size())
    return false;

  auto templateText = tryGetInvocationArgText(parent, slot);
  if (!templateText)
    return false;
  if (templateText->trim() != parent.defParams[slot].name)
    return false;

  if (slot >= parent.argRefs.size())
    return false;
  ArrayRef<RefoldModel::InvArgRef> refs(parent.argRefs[slot]);
  if (refs.size() != 1)
    return false;

  if (slot < parent.argTupleRefs.size() && !parent.argTupleRefs[slot].empty())
    return false;

  if (slot >= parent.argDeps.size())
    return false;
  ArrayRef<uint32_t> deps(parent.argDeps[slot]);
  if (deps.size() != 1 || deps.front() != refs.front().callerParamIndex)
    return false;

  return true;
}

/// Format a list of uint32_t values for trace diagnostics.
static std::string formatUInt32List(ArrayRef<uint32_t> values) {
  std::string out;
  raw_string_ostream os(out);
  os << "[";
  for (size_t i = 0; i < values.size(); ++i) {
    if (i)
      os << ", ";
    os << values[i];
  }
  os << "]";
  return os.str();
}

/// One character-diff hunk derived while composing compatible string rewrites.
struct CompatibleStringRewriteHunk {
  uint64_t oldBegin = 0;
  uint64_t oldEnd = 0;
  std::string repl;
};

/// True iff two composed string-rewrite hunks describe the exact same edit.
static bool sameCompatibleStringRewriteHunk(
    const CompatibleStringRewriteHunk &lhs,
    const CompatibleStringRewriteHunk &rhs) {
  return lhs.oldBegin == rhs.oldBegin && lhs.oldEnd == rhs.oldEnd &&
         lhs.repl == rhs.repl;
}

/// True iff two string-rewrite hunks overlap in the original base string.
static bool compatibleStringRewriteHunksOverlap(
    const CompatibleStringRewriteHunk &lhs,
    const CompatibleStringRewriteHunk &rhs) {
  return lhs.oldBegin < rhs.oldEnd && rhs.oldBegin < lhs.oldEnd;
}

/// Compare a string-rewrite hunk's original start against a lower-bound key.
static bool compatibleStringRewriteHunkBeginsBefore(
    const CompatibleStringRewriteHunk &hunk, uint64_t pos) {
  return hunk.oldBegin < pos;
}

/// Merge several independently-proven replacements against the same base text.
///
/// Each replacement is diffed against \p baseOld at character granularity.
/// Identical hunks are deduplicated, disjoint hunks are composed, and any
/// conflicting overlap fails closed. This is the shared callsite/formal
/// merge policy used by the macro patch admission paths.
static std::optional<std::string>
mergeCompatibleStringReplacements(StringRef baseOld,
                                  ArrayRef<StringRef> replacements) {
  std::vector<CompatibleStringRewriteHunk> merged;
  std::vector<StringRef> aRefs = stringutils::splitChars(baseOld);

  for (StringRef replText : replacements) {
    if (replText == baseOld)
      continue;

    std::vector<StringRef> bRefs = stringutils::splitChars(replText);
    auto steps = diffutils::diff(aRefs, bRefs);
    auto hunks = diffutils::coalesce(steps);

    for (const auto &hunk : hunks) {
      CompatibleStringRewriteHunk piece{
          hunk.aStart, hunk.aEnd,
          replText
              .slice(static_cast<size_t>(hunk.bStart),
                     static_cast<size_t>(hunk.bEnd))
              .str()};

      auto it = std::lower_bound(merged.begin(), merged.end(), piece.oldBegin,
                                 compatibleStringRewriteHunkBeginsBefore);

      if (it != merged.begin()) {
        const auto &prev = *std::prev(it);
        if (sameCompatibleStringRewriteHunk(prev, piece))
          continue;
        if (compatibleStringRewriteHunksOverlap(prev, piece))
          return std::nullopt;
      }
      if (it != merged.end()) {
        if (sameCompatibleStringRewriteHunk(*it, piece))
          continue;
        if (compatibleStringRewriteHunksOverlap(*it, piece))
          return std::nullopt;
      }

      merged.insert(it, std::move(piece));
    }
  }

  std::string out = baseOld.str();
  for (auto it = merged.rbegin(); it != merged.rend(); ++it)
    out.replace(static_cast<size_t>(it->oldBegin),
                static_cast<size_t>(it->oldEnd - it->oldBegin), it->repl);
  return out;
}

/// Collect the uint32_t keys of an associative container in deterministic
/// ascending order.
template <typename MapLike>
static SmallVector<uint32_t, 8>
collectSortedUInt32Keys(const MapLike &mapLike) {
  SmallVector<uint32_t, 8> keys;
  keys.reserve(mapLike.size());
  for (const auto &kv : mapLike)
    keys.push_back(kv.first);
  llvm::sort(keys);
  return keys;
}

/// Collect the unique formal indices participating in a pasted token surface.
///
/// Producer metadata may mention the same formal more than once within one
/// pasted product, so callers that reason about support ledgers first need a
/// deduplicated arg-index view.
static SmallVector<uint32_t, 8>
collectSortedUniquePasteArgIdxs(ArrayRef<RefoldModel::PPArgSpan> pasteSpans) {
  SmallVector<uint32_t, 8> argIdxs;
  for (const auto &ps : pasteSpans) {
    if (!llvm::is_contained(argIdxs, ps.argIdx))
      argIdxs.push_back(ps.argIdx);
  }
  llvm::sort(argIdxs);
  return argIdxs;
}

/// Return the sorted subset of `required` that is not present in `provided`.
static SmallVector<uint32_t, 8>
computeSortedMissingUInt32s(ArrayRef<uint32_t> required,
                            ArrayRef<uint32_t> provided) {
  SmallVector<uint32_t, 8> missing;
  for (uint32_t value : required) {
    if (!llvm::is_contained(provided, value))
      missing.push_back(value);
  }
  llvm::sort(missing);
  return missing;
}

/// Return true when the given invocation argument contributes to any pasted
/// token emitted by the invocation.
static bool invocationArgTouchesPaste(const RefoldModel::MacroInvocation &mi,
                                      uint32_t argIdx) {
  for (const auto &ps : mi.pasteSpans) {
    if (ps.argIdx == argIdx)
      return true;
  }
  return false;
}

/// Return true when any arg index in `argIdxs` participates in a pasted token.
static bool anyInvocationArgTouchesPaste(const RefoldModel::MacroInvocation &mi,
                                         ArrayRef<uint32_t> argIdxs) {
  for (uint32_t argIdx : argIdxs) {
    if (invocationArgTouchesPaste(mi, argIdx))
      return true;
  }
  return false;
}

/// Return true when every touched paste arg in `argIdxs` belongs to the
/// supplied allow-list set. Non-paste arguments are ignored.
template <typename SetLike>
static bool allTouchedPasteArgsAreContained(
    const RefoldModel::MacroInvocation &mi, ArrayRef<uint32_t> argIdxs,
    const SetLike &allowedArgIdxs) {
  for (uint32_t argIdx : argIdxs) {
    if (invocationArgTouchesPaste(mi, argIdx) && !allowedArgIdxs.count(argIdx))
      return false;
  }
  return true;
}

/// Total ordering for paste-span pointer groups.
///
/// Some call sites already require concrete byte ranges and some are still
/// screening for missing ranges. Centralizing the ordering keeps all of those
/// paths consistent without changing their local preconditions.
static bool pasteSpanPtrLessByByteRange(const RefoldModel::PPArgSpan *a,
                                        const RefoldModel::PPArgSpan *b) {
  if (a->byteBegin != b->byteBegin)
    return a->byteBegin < b->byteBegin;
  if (a->byteEnd != b->byteEnd)
    return a->byteEnd < b->byteEnd;
  if (a->begin != b->begin)
    return a->begin < b->begin;
  if (a->end != b->end)
    return a->end < b->end;
  return a->argIdx < b->argIdx;
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
static PasteRunInvertibilityCertificate prependDerivedSegment(
    PasteRunInvertibilityCertificate cert, StringRef seg) {
  if (cert.kind != PasteRunInvertibilityKind::Unique)
    return cert;

  cert.derivedSegs.insert(cert.derivedSegs.begin(), seg.str());
  return cert;
}

/// Merge two paste-run inversion attempts, preserving uniqueness only when
/// all successful witnesses agree on exactly the same derived segments.
static PasteRunInvertibilityCertificate mergePasteRunCertificates(
    PasteRunInvertibilityCertificate lhs,
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
static PasteRunInvertibilityCertificate
buildAdjacentPasteRunInvertibilityCertificate(
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
                      bool prevChanged)
      -> PasteRunInvertibilityCertificate {
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
      PasteRunInvertibilityCertificate unchanged =
          prependDerivedSegment(
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
static PasteReplaySegmentationResult mergePasteReplayResults(
    PasteReplaySegmentationResult lhs,
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
static const RefoldModel::PasteToken *
findPasteTokenWitnessForSpan(const RefoldModel::MacroInvocation &m,
                             const RefoldModel::PPArgSpan &span,
                             StringRef aTokSpelling) {
  if (m.pasteTokens.empty())
    return nullptr;

  SmallVector<std::pair<uint64_t, uint64_t>, 8> tokenOrder;
  for (const auto &ps : m.pasteSpans) {
    std::pair<uint64_t, uint64_t> key{ps.begin, ps.end};
    if (llvm::find(tokenOrder, key) == tokenOrder.end())
      tokenOrder.push_back(key);
  }

  auto it = llvm::find(tokenOrder,
                       std::pair<uint64_t, uint64_t>{span.begin, span.end});
  if (it == tokenOrder.end())
    return nullptr;
  const size_t index =
      static_cast<size_t>(std::distance(tokenOrder.begin(), it));
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
static PasteReplaySegmentationResult segmentArgRunByReplayWidths(
    StringRef bRun, ArrayRef<RefoldModel::PastePart> parts) {
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
static PasteReplaySegmentationResult segmentPastedTokenByReplayWitness(
    StringRef bTokSpelling, const RefoldModel::PasteToken &witness) {
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
    // unit. This avoids independently guessing byte cuts between adjacent pasted
    // arguments.
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

namespace {
/// Byte slice for a single top-level element inside a comma-separated tuple.
///
/// `begin`/`end` cover the full half-open byte range for the element inside the
/// caller argument text. `trimBegin`/`trimEnd` shrink that range to the
/// non-whitespace payload used for exact old/new text comparisons.
struct TupleElementSlice {
  size_t begin = 0;
  size_t end = 0;
  size_t trimBegin = 0;
  size_t trimEnd = 0;
};

/// Build a tuple-element slice and reject elements whose trimmed payload is
/// empty. Empty elements would make old/new tuple matching ambiguous.
static bool computeTrimmedTupleElement(StringRef text, size_t begin, size_t end,
                                       TupleElementSlice &out) {
  out.begin = begin;
  out.end = end;
  out.trimBegin = begin;
  out.trimEnd = end;
  std::tie(out.trimBegin, out.trimEnd) =
      stringutils::trimWsRange(text, begin, end);
  return out.trimBegin != out.trimEnd;
}

/// Emit every byte boundary in the half-open range `(begin,end]`.
///
/// The caller decides that the entire byte span is safe; this helper preserves
/// the legacy byte-granularity cut contract.
static void emitByteCutRange(size_t begin, size_t end,
                             function_ref<void(unsigned)> emitCut) {
  for (size_t cut = begin + 1; cut <= end; ++cut)
    emitCut(static_cast<unsigned>(cut));
}


/// True for string and character literal tokens whose interior bytes must not
/// be inspected for delimiters or cut points.
static bool isOpaqueLiteralToken(tok::TokenKind kind) {
  switch (kind) {
  case tok::string_literal:
  case tok::wide_string_literal:
  case tok::utf8_string_literal:
  case tok::utf16_string_literal:
  case tok::utf32_string_literal:
  case tok::char_constant:
  case tok::wide_char_constant:
  case tok::utf8_char_constant:
  case tok::utf16_char_constant:
  case tok::utf32_char_constant:
    return true;
  default:
    return false;
  }
}

/// Return the byte offset immediately after a complete comment token.
///
/// Raw lexing keeps comments as opaque tokens, but this routine verifies the
/// textual terminator before exposing a post-comment cut point. Unterminated
/// comments produce nullopt and therefore no internal/post-comment cut.
static std::optional<size_t> commentCutEnd(StringRef text, size_t begin,
                                           size_t end) {
  StringRef comment = text.slice(begin, end);
  if (comment.starts_with("//")) {
    const size_t newline = text.find('\n', begin);
    if (newline == StringRef::npos)
      return std::nullopt;
    return newline + 1;
  }

  if (comment.starts_with("/*")) {
    const size_t close = text.find("*/", begin + 2);
    if (close == StringRef::npos)
      return std::nullopt;
    return close + 2;
  }

  return end;
}

/// Update delimiter nesting for raw tokens that contribute to top-level comma
/// and cut-point decisions. Unbalanced closers are saturated at zero to match
/// the previous conservative scanner behavior.
static void updateTopLevelDelimiterDepth(tok::TokenKind kind, int &parenDepth,
                                         int &bracketDepth, int &braceDepth) {
  switch (kind) {
  case tok::l_paren:
    ++parenDepth;
    break;
  case tok::r_paren:
    if (parenDepth > 0)
      --parenDepth;
    break;
  case tok::l_square:
    ++bracketDepth;
    break;
  case tok::r_square:
    if (bracketDepth > 0)
      --bracketDepth;
    break;
  case tok::l_brace:
    ++braceDepth;
    break;
  case tok::r_brace:
    if (braceDepth > 0)
      --braceDepth;
    break;
  default:
    break;
  }
}

/// True when no tracked delimiter family is currently nested.
static bool isAtTopLevel(int parenDepth, int bracketDepth, int braceDepth) {
  return parenDepth == 0 && bracketDepth == 0 && braceDepth == 0;
}

/// Enumerate byte cut-points that are balanced with respect to top-level
/// delimiters in `text`.
///
/// The old scanner emitted every safe byte boundary outside comments, literals,
/// and nested delimiter groups. This lexer-backed implementation preserves that
/// contract: tokens decide what text is opaque, while byte-range emission keeps
/// the previous cut-point granularity for whitespace, identifiers, and ordinary
/// punctuation.
static void enumerateTopLevelBalancedCutPointsWithLexer(
    StringRef text, const LangOptions &lang,
    function_ref<void(unsigned)> emitCut) {
  emitCut(0u);
  if (text.empty())
    return;

  const SourceLocation baseLoc = SourceLocation::getFromRawEncoding(1);
  std::string lexBuf = text.str();
  lexBuf.push_back('\0');
  const char *bufStart = lexBuf.data();
  const char *bufEnd = bufStart + text.size();
  Lexer lexer(baseLoc, lang, bufStart, bufStart, bufEnd);
  lexer.SetCommentRetentionState(true);

  int parenDepth = 0;
  int bracketDepth = 0;
  int braceDepth = 0;
  size_t covered = 0;
  Token token;

  while (true) {
    lexer.LexFromRawLexer(token);
    if (token.is(tok::eof))
      break;

    const size_t tokenBegin = std::min(refoldTokenOffsetFromBase(token, baseLoc),
                                       text.size());
    const size_t tokenEnd = std::min(refoldTokenEndOffsetFromBase(token, baseLoc),
                                     text.size());

    // Whitespace and other bytes skipped by raw lexing remain valid cut points
    // only when the current delimiter state is top-level.
    if (covered < tokenBegin &&
        isAtTopLevel(parenDepth, bracketDepth, braceDepth))
      emitByteCutRange(covered, tokenBegin, emitCut);

    if (token.is(tok::comment)) {
      // Treat the whole comment as opaque. A cut may occur after a complete
      // comment terminator, but never inside the comment body.
      covered = tokenEnd;
      std::optional<size_t> cutEnd = commentCutEnd(text, tokenBegin, tokenEnd);
      if (cutEnd) {
        covered = std::min(*cutEnd, text.size());
        if (isAtTopLevel(parenDepth, bracketDepth, braceDepth))
          emitCut(static_cast<unsigned>(covered));
      }
      continue;
    }

    if (isOpaqueLiteralToken(token.getKind())) {
      // String/character literal contents are opaque for both delimiter balance
      // and cut enumeration; expose only the post-literal boundary.
      covered = tokenEnd;
      if (isAtTopLevel(parenDepth, bracketDepth, braceDepth))
        emitCut(static_cast<unsigned>(tokenEnd));
      continue;
    }

    updateTopLevelDelimiterDepth(token.getKind(), parenDepth, bracketDepth,
                                 braceDepth);
    covered = tokenEnd;

    // For ordinary tokens, preserve the legacy byte-granularity behavior: every
    // boundary inside the token spelling is a candidate as long as no delimiter
    // nesting remains open after consuming the token.
    if (isAtTopLevel(parenDepth, bracketDepth, braceDepth))
      emitByteCutRange(tokenBegin, tokenEnd, emitCut);
  }

  if (covered < text.size() &&
      isAtTopLevel(parenDepth, bracketDepth, braceDepth))
    emitByteCutRange(covered, text.size(), emitCut);
}

/// Split a caller tuple into top-level comma-separated elements using Clang's
/// raw lexer instead of ad hoc character scanning.
///
/// This is intentionally token-based: literals and comments arrive as single
/// tokens, so only delimiter depth (`()`, `[]`, `{}`) needs to be tracked when
/// deciding whether a comma separates tuple elements.
static bool splitTopLevelTupleElementsWithLexer(
    StringRef text, const LangOptions &lang,
    SmallVectorImpl<TupleElementSlice> &out) {
  out.clear();

  // Empty text cannot represent a well-formed caller tuple payload for the
  // current matching path. Return false rather than manufacturing one empty
  // element, because callers use failure to reject tuple-based replay.
  if (text.empty())
    return false;

  // Build a null-terminated buffer for Clang's raw lexer. The lexer is bounded
  // by `bufEnd`, so the sentinel is present for lexer safety/convenience but is
  // not part of the logical input range.
  const SourceLocation baseLoc = SourceLocation::getFromRawEncoding(1);
  std::string lexBuf = text.str();
  lexBuf.push_back('\0');
  const char *bufStart = lexBuf.data();
  const char *bufEnd = bufStart + text.size();
  Lexer lexer(baseLoc, lang, bufStart, bufStart, bufEnd);

  // `elementBegin` is the raw byte offset where the current tuple element
  // starts in `text`. It advances to the byte immediately after each top-level
  // comma.
  size_t elementBegin = 0;

  // Track nesting only for syntactic delimiter tokens. Strings, character
  // literals, and comments are emitted by the raw lexer as opaque tokens, so
  // commas inside them cannot be mistaken for tuple separators.
  int parenDepth = 0;
  int bracketDepth = 0;
  int braceDepth = 0;
  Token token;

  while (true) {
    lexer.LexFromRawLexer(token);
    if (token.is(tok::eof))
      break;

    // Comments do not contribute delimiters or tuple separators. Keeping them
    // opaque also prevents commas inside comments from splitting the tuple.
    if (token.is(tok::comment))
      continue;

    // Convert token locations back into offsets relative to `text`. The fake
    // base location gives stable raw encodings for this temporary lexer buffer.
    const size_t tokBegin =
        token.getLocation().getRawEncoding() - baseLoc.getRawEncoding();
    const size_t tokEnd = tokBegin + token.getLength();

    switch (token.getKind()) {
    case tok::l_paren:
      ++parenDepth;
      break;
    case tok::r_paren:
      // Clamp unmatched closers at zero. This helper is only trying to find
      // safe top-level separators; malformed balance is rejected later by the
      // surrounding replay/validation logic rather than diagnosed here.
      if (parenDepth > 0)
        --parenDepth;
      break;
    case tok::l_square:
      ++bracketDepth;
      break;
    case tok::r_square:
      if (bracketDepth > 0)
        --bracketDepth;
      break;
    case tok::l_brace:
      ++braceDepth;
      break;
    case tok::r_brace:
      if (braceDepth > 0)
        --braceDepth;
      break;
    case tok::comma:
      // Only a comma at delimiter depth zero separates caller-tuple elements.
      // Nested commas belong to subexpressions such as calls, subscripts,
      // braced initializers, or macro argument payloads.
      if (parenDepth == 0 && bracketDepth == 0 && braceDepth == 0) {
        // A depth-zero comma closes the current tuple element. Trim it now so
        // later tuple matching compares payloads, not caller formatting.
        TupleElementSlice elem;
        if (!computeTrimmedTupleElement(text, elementBegin, tokBegin, elem))
          return false;
        out.push_back(elem);

        // The next element begins immediately after the separating comma. Any
        // surrounding whitespace is preserved in the source offsets but ignored
        // by the trimmed slice computed for matching.
        elementBegin = tokEnd;
      }
      break;
    default:
      // All other tokens are payload for the current element.
      break;
    }
  }

  // Flush the final element after the last comma, or the only element if no
  // top-level comma was seen.
  TupleElementSlice elem;
  if (!computeTrimmedTupleElement(text, elementBegin, text.size(), elem))
    return false;
  out.push_back(elem);

  return true;
}

} // namespace

std::optional<RefoldMacroPatchPlanner::InvocationActualLayout>
RefoldMacroPatchPlanner::RecoverInvocationActuals(
    const RefoldModel::MacroInvocation &invocation,
    StringRef baseInvocationText) const {
  // Args-only replay starts with data recovery, not candidate construction.
  // Keep recovery fail-closed and limited to the historical availability
  // checks: a concrete invocation byte span, a literal callee origin, parsed
  // formal-content ranges, and the named actual-recovery precondition.
  if (!invocation.invB || !invocation.invE)
    return std::nullopt;

  if (!hasLiteralMacroCalleeOrigin(invocation))
    return std::nullopt;

  auto rangesOpt =
      GetMacroInvocationFormalArgContentRanges(invocation,
                                                     baseInvocationText);
  if (!rangesOpt)
    return std::nullopt;

  InvocationActualLayout layout;
  layout.contentRanges = std::move(*rangesOpt);

  InvocationActualRecoveryContext actualRecoveryCtx{
      invocation, baseInvocationText, layout.rangePairs()};
  if (!InvocationActualsAreRecoverable(actualRecoveryCtx))
    return std::nullopt;

  return layout;
}

bool RefoldMacroPatchPlanner::InvocationActualsAreRecoverable(
    const InvocationActualRecoveryContext &ctx) const {
  // The source-spelling rewrite path is only meaningful for an invocation
  // whose physical callsite byte range exists.  The caller already obtained
  // the formal-content ranges from the recovery service; this method gives
  // that data-availability precondition a named fail-closed boundary.
  if (!ctx.invocation.invB || !ctx.invocation.invE)
    return false;

  // Do not validate every recovered argument range here: the rewrite builder
  // validates only the argument indices it is asked to replace.
  // Keeping that check in BuildInvocationRewriteWithRange preserves the exact
  // fail-closed timing for paths that never touch a particular actual.
  (void)ctx.baseInvocationText;
  (void)ctx.invocationArgRanges;
  return true;
}

std::optional<RefoldMacroPatchPlanner::InvocationRewriteWithRange>
RefoldMacroPatchPlanner::BuildInvocationRewriteWithRange(
    const InvocationActualRecoveryContext &ctx,
    const DenseMap<uint32_t, std::string> &replByArgIdx,
    const DenseMap<uint32_t, std::pair<uint64_t, uint64_t>>
        *materializedRangeByArgIdx) const {
  // Rebuild the invocation by replacing formal-content ranges right-to-left in
  // source order while carrying the exact replacement-relative materialized
  // output interval.  All state required for recovery is supplied explicitly
  // through InvocationActualRecoveryContext.
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

  llvm::sort(keys, [&invocationArgRanges](const uint32_t lhs,
                                          const uint32_t rhs) -> bool {
    const auto lhsRange = invocationArgRanges[lhs];
    const auto rhsRange = invocationArgRanges[rhs];
    if (lhsRange.first != rhsRange.first)
      return lhsRange.first < rhsRange.first;
    return lhs < rhs;
  });

  InvocationRewriteWithRange out;
  out.text.reserve(baseInvocationText.size());

  uint64_t cursor = 0;
  std::optional<uint64_t> mappedBegin;
  std::optional<uint64_t> mappedEnd;
  for (uint32_t argIdx : keys) {
    const auto rawRange = invocationArgRanges[argIdx];
    const ActualContentRange r{rawRange.first, rawRange.second};
    if (r.end < r.begin || r.end > baseInvocationText.size() ||
        r.begin < cursor)
      return std::nullopt;

    out.text += baseInvocationText.slice(cursor, r.begin).str();

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
    mappedEnd = mappedEnd ? std::max(*mappedEnd, materializedEnd)
                          : materializedEnd;

    cursor = r.end;
  }

  out.text += baseInvocationText.substr(cursor).str();
  if (!mappedBegin || !mappedEnd)
    return std::nullopt;

  out.materializedOutputByteStart = *mappedBegin;
  out.materializedOutputByteEnd = *mappedEnd;
  return out;
}

void RefoldMacroPatchPlanner::StampInvocationRewriteMaterializedOutputRange(
    MacroPatch &patch, const InvocationRewriteWithRange &rewrite) const {
  patch.hasMaterializedOutputByteRange = true;
  patch.materializedOutputByteStart = rewrite.materializedOutputByteStart;
  patch.materializedOutputByteEnd = rewrite.materializedOutputByteEnd;
}

/// Origin tag carried beside the final macro candidate so diagnostics and
/// post-selection stamping can distinguish replay, reuse, and realization.
enum class RefoldMacroPatchPlanner::FinalMacroCandidateOrigin : uint8_t {
  DirectArgsOnly,
  DagRootReplay,
  ReuseExistingCallsiteNoOp,
  ReuseExistingCallsiteSkipWholeCover,
  ReuseExistingExpanded,
  WholeCoverRealization,
};

/// Final selector carrier pairing the concrete macro patch with its theorem
/// lattice candidate and path origin.
struct RefoldMacroPatchPlanner::FinalMacroCandidate {
  MacroPatch patch;
  MacroSelectionCandidate selectionCandidate;
  FinalMacroCandidateOrigin origin =
      FinalMacroCandidateOrigin::WholeCoverRealization;
};

/// Explicit state bundle for final macro-candidate admission.
///
/// The whole-cover planner discovers candidates through several independent
/// proof paths, then routes all of them through one proof-lattice selector.
/// This context owns none of those candidates; it only names the invocation,
/// hunk, source spelling, selector policy, replay-stability context, and
/// caller-owned candidate vector that the final-admission helpers need.
struct RefoldMacroPatchPlanner::FinalMacroCandidateAdmissionContext {
  const RefoldModel::MacroInvocation &invocation;
  const diffutils::Hunk &hunk;
  StringRef baseInvocationText;
  SmallVectorImpl<FinalMacroCandidate> &candidates;
  const MacroSubtreeReplayValidationContext *replayStabilityCtx = nullptr;
  bool allowNonTopLevelMacroSelectorFailure = false;

  bool hasDirectArgsOnlyCandidate = false;
  bool hasDagRootReplayCandidate = false;
  bool hasExistingCallsiteCandidate = false;
  bool hasExistingExpandedCandidate = false;
  bool hasWholeCoverCandidate = false;
};

void RefoldMacroPatchPlanner::NoteFinalMacroCandidateOrigin(
    FinalMacroCandidateAdmissionContext &ctx,
    FinalMacroCandidateOrigin origin) const {
  switch (origin) {
  case FinalMacroCandidateOrigin::DirectArgsOnly:
    ctx.hasDirectArgsOnlyCandidate = true;
    break;
  case FinalMacroCandidateOrigin::DagRootReplay:
    ctx.hasDagRootReplayCandidate = true;
    break;
  case FinalMacroCandidateOrigin::ReuseExistingCallsiteNoOp:
  case FinalMacroCandidateOrigin::ReuseExistingCallsiteSkipWholeCover:
    ctx.hasExistingCallsiteCandidate = true;
    break;
  case FinalMacroCandidateOrigin::ReuseExistingExpanded:
    ctx.hasExistingExpandedCandidate = true;
    break;
  case FinalMacroCandidateOrigin::WholeCoverRealization:
    ctx.hasWholeCoverCandidate = true;
    break;
  }
}

void RefoldMacroPatchPlanner::AddFinalMacroCandidate(
    FinalMacroCandidateAdmissionContext &ctx,
    FinalMacroCandidate candidate) const {
  // Keep the replay-context stability gate at the final-admission boundary.
  // Structure-preserving replay candidates are discovered by several paths,
  // but every path must pass this same gate before entering the theorem
  // selector so unstable callsite replay cannot suppress realization.
  if (!ctx.replayStabilityCtx ||
      !MacroCandidateReplayIsStableForFinalSelection(*ctx.replayStabilityCtx,
                                                     candidate.patch))
    return;

  candidate.selectionCandidate =
      GetProofLattice().BuildMacroSelectionCandidate(
          candidate.patch, ctx.allowNonTopLevelMacroSelectorFailure);
  ctx.candidates.push_back(std::move(candidate));
  NoteFinalMacroCandidateOrigin(ctx, ctx.candidates.back().origin);
}

bool RefoldMacroPatchPlanner::TheoremLatticeStructureCandidateDominatesRealization(
    const FinalMacroCandidateAdmissionContext &ctx,
    const MacroPatch &realizationPatch) const {
  const RefoldProofLattice &lattice = GetProofLattice();
  AcceptedResultCandidate realizationCandidate =
      lattice.BuildAcceptedMacroCandidate(realizationPatch);
  if (!lattice.IsSelectableAcceptedResultCandidate(realizationCandidate))
    return false;

  for (const FinalMacroCandidate &candidate : ctx.candidates) {
    if (!candidate.patch.proof.preservesInvocationStructure ||
        candidate.patch.proof.proofRootMacroId != ctx.invocation.id)
      continue;
    if (!lattice.IsSelectableAcceptedResultCandidate(
            candidate.selectionCandidate.selectorCandidate))
      continue;
    if (lattice.LatticePrefers(
            candidate.selectionCandidate.selectorCandidate.proofSummary,
            realizationCandidate.proofSummary) &&
        !lattice.LatticePrefers(
            realizationCandidate.proofSummary,
            candidate.selectionCandidate.selectorCandidate.proofSummary))
      return true;
  }
  return false;
}

std::optional<SelectedMacroSelectionCandidate>
RefoldMacroPatchPlanner::SelectPreferredFinalMacroCandidate(
    const FinalMacroCandidateAdmissionContext &ctx) const {
  // The proof lattice consumes a dense selector vector, while the selected
  // index must continue to refer to the caller-owned final-candidate vector.
  // Build the temporary projection in the same order and do not mutate
  // ctx.candidates here.
  SmallVector<MacroSelectionCandidate, 5> selectionCandidates;
  selectionCandidates.reserve(ctx.candidates.size());
  for (const FinalMacroCandidate &candidate : ctx.candidates)
    selectionCandidates.push_back(candidate.selectionCandidate);
  return GetProofLattice().SelectPreferredMacroSelectionCandidate(
      selectionCandidates);
}

void RefoldMacroPatchPlanner::StampSelectedFinalMacroCandidate(
    const FinalMacroCandidateAdmissionContext &ctx,
    const SelectedMacroSelectionCandidate &selectedCandidate,
    MacroPatch &selectedPatch) const {
  if (selectedCandidate.candidate.emittedCandidate) {
    GetProofLattice().StampSelectedMacroPatchCandidate(
        selectedPatch, *selectedCandidate.candidate.emittedCandidate,
        "macro/final-selector");
    return;
  }

  // A selector-only macro proof may choose the concrete spelling, but it is
  // not an emitted accepted artifact.  Leave the patch unstamped so the final
  // emission-bucket gate must either construct a theorem-normalized emitted
  // carrier or fail closed under the strict/theorem no-legacy audit.
  REFOLD_LOG_TRACE("macro/proof",
        "selected macro candidate has no emitted accepted carrier: inv "
        "id={0} name={1} proofKind={2} selectorOnly={3}",
        ctx.invocation.id, ctx.invocation.name, selectedPatch.proof.kind,
        selectedCandidate.candidate.selectorOnly ? 1 : 0);
}

/// Explicit proof-stamping state for whole-cover realization admission.
///
/// The plan already contains the exact A-cover, B-token envelope, containment
/// proof bit, clipping flags, and owner-realization evidence required by the
/// proof lattice.  Keeping this as a small carrier avoids a giant parameter
/// list while preserving the existing whole-cover stamping behavior exactly.
struct RefoldMacroPatchPlanner::WholeCoverAdmissionContext {
  WholeCoverProofInputs proofInputs;
};

void RefoldMacroPatchPlanner::AttachWholeCoverProofCarrier(
    const WholeCoverAdmissionContext &ctx, MacroPatch &patch) const {
  GetProofLattice().StampMacroWholeCoverRealizationPatch(
      patch, ctx.proofInputs.plan, ctx.proofInputs.invocation);
}

void RefoldMacroPatchPlanner::StampWholeCoverAcceptedCandidate(
    const WholeCoverAdmissionContext &ctx, MacroPatch &patch) const {
  // Whole-cover realization has a single historical stamping operation: it
  // records the materialized B-token envelope, whole-cover diagnostics, proof
  // kind, proof root, and owner-realization witness together.  Keep that
  // operation atomic by routing through the named proof-carrier helper.
  AttachWholeCoverProofCarrier(ctx, patch);
}

void RefoldMacroPatchPlanner::StampReusedMacroPatchAcceptedCandidate(
    const MacroPatchReuseAdmissionContext &ctx, MacroPatch &patch) const {
  (void)ctx;
  (void)patch;
  // Reuse must not manufacture a fresh theorem carrier.  The selected patch is
  // an already accepted same-span patch, so its proof kind, proof-root id,
  // materialized range, and owner certificate are intentionally preserved.
}


// Final replay-stability gates are named methods so every macro candidate is
// admitted through explicit planner operations rather than through stateful
// local closures in the whole-cover entry point.
bool RefoldMacroPatchPlanner::CallsiteReplayObservesActiveHeaderMacroState(
    const RefoldModel::MacroInvocation &m, const MacroPatch &patch) const {
  // Only structure-preserving callsite replay can be unstable in this
  // way.  Expanded/whole-cover candidates do not replay the callsite as a
  // macro invocation, and TU-spelled invocations are handled by the TU
  // macro-state repair paths rather than include-owner replay.
  if (!patch.proof.preservesInvocationStructure ||
      patch.proof.proofRootMacroId != m.id)
    return false;
  if (!RefoldLineObserverLayout::InvocationSpanMatchesCallsitePrefix(patch.replacement, m))
    return false;
  if (!m.ownerIncludeId || !m.invFile)
    return false;
  if ((*deps_.pathIdentity).PathsEqual(*m.invFile, (*deps_.model).GetSourcePath()))
    return false;

  // Decide whether this structure-preserving candidate would observe a
  // specific active header directive if replayed at the current callsite.
  // Use the shared macro-state observation proof so this header replay
  // gate remains aligned with TU carry, include materialization, and
  // unresolved-expansion fallback.

  // Exact source interval for a macro-state directive in the header
  // containing this invocation.  The interval is used only for ordering
  // and active-state reconstruction in this admission check; movement is
  // performed later by the materialization path.
  using HeaderDirectivePiece = MacroStateDirectiveLineInterval;

  // Lazily read the header bytes so directive intervals can be validated
  // against the real file.  If the file cannot be read, this proof does
  // not guess; it simply declines to suppress the candidate here and lets
  // downstream validation/fallback handle the uncertainty.
  std::optional<std::string> headerBytesStorage;
  auto getHeaderBytes = [&]() -> std::optional<StringRef> {
    if (headerBytesStorage)
      return StringRef(*headerBytesStorage);
    auto bufOrErr =
        MemoryBuffer::getFile((*deps_.lineDirs).ToAbsolutePath(m.invFile->str()));
    if (!bufOrErr)
      return std::nullopt;
    const MemoryBuffer &mb = **bufOrErr;
    headerBytesStorage.emplace(mb.getBufferStart(), mb.getBufferEnd());
    return StringRef(*headerBytesStorage);
  };

  // Reconstruct the full directive interval and verify that it belongs to
  // the same include owner and header file as the callsite. The shared
  // helper owns macro-name-anchor reconstruction and exact-text
  // validation against the lazily-read header bytes.
  auto directiveInterval = [&](const RefoldModel::MacroDirective &directive)
      -> std::optional<HeaderDirectivePiece> {
    std::optional<StringRef> headerBytes = getHeaderBytes();
    if (!headerBytes)
      return std::nullopt;
    return GetMacroStateProof().RecoverMacroStateDirectiveLineInterval(
        directive, *m.invFile, *headerBytes, m.ownerIncludeId);
  };

  // Determine whether `definition` is the active macro definition for
  // `macroName` immediately before the preserved callsite.  A later #undef
  // or #define for the same name cancels this definition for replay
  // stability purposes.
  auto activeDefinitionAtPatch =
      [&](const RefoldModel::MacroDirective &definition,
          StringRef macroName) {
        const RefoldModel::MacroDirective *active = nullptr;
        uint64_t activeEnd = 0;
        for (const auto &candidate : (*deps_.model).GetMacroDirectives()) {
          std::optional<HeaderDirectivePiece> piece =
              directiveInterval(candidate);
          if (!piece || piece->end > patch.invStart)
            continue;
          if (StringRef(piece->name) != macroName)
            continue;
          if (!active || piece->end > activeEnd ||
              (piece->end == activeEnd && candidate.id > active->id)) {
            active = &candidate;
            activeEnd = piece->end;
          }
        }
        return active == &definition && definition.subkind == "#define";
      };

  // If any active header-owned definition would be observed by the
  // replacement, this structure-preserving candidate is inadmissible.  It
  // is not enough that the rewritten text is token-equivalent somewhere;
  // it must be token-equivalent under the macro state at its final replay
  // position.
  for (const auto &directive : (*deps_.model).GetMacroDirectives()) {
    std::optional<HeaderDirectivePiece> piece = directiveInterval(directive);
    if (!piece || piece->end > patch.invStart)
      continue;
    if (!activeDefinitionAtPatch(directive, piece->name))
      continue;
    if (ReplacementObservesDirective(directive, piece->name, patch.replacement)) {
      REFOLD_LOG_TRACE("macro/proof",
            "suppress structure-preserving macro replay: inv id={0} "
            "name={1} replacement observes active header macro-state "
            "directive #{2} '{3}' before callsite",
            m.id, m.name, directive.id, piece->name);
      return true;
    }
  }
  return false;
}

bool RefoldMacroPatchPlanner::ArgsOnlyWholeEnvelopeCandidateHasLiteralBodyReplay(
    const RefoldModel::MacroInvocation &m, const MacroPatch &patch) const {
  auto isArgsOnlyInvocationPreservingProof = [&]() {
    switch (patch.proof.kind) {
    case MacroPatchProofKind::ArgsOnlyStandard:
    case MacroPatchProofKind::ArgsOnlyPasteSingle:
    case MacroPatchProofKind::ArgsOnlyPasteMulti:
    case MacroPatchProofKind::ArgsOnlyPurePasteOnly:
    case MacroPatchProofKind::ArgsOnlyPairedPureInsertion:
      return true;
    case MacroPatchProofKind::PasteDerivedCalleeSelector:
    case MacroPatchProofKind::DagSubtreeRoot:
    case MacroPatchProofKind::CallChainSuffix:
    case MacroPatchProofKind::CounterLiteral:
    case MacroPatchProofKind::WholeCoverRealization:
    case MacroPatchProofKind::Unknown:
      return false;
    }
    return false;
  };

  if (!isArgsOnlyInvocationPreservingProof() ||
      !patch.proof.preservesInvocationStructure ||
      patch.proof.proofRootMacroId != m.id)
    return true;
  if (!patch.hasMaterializedBTokenRange)
    return true;

  // Complete replay solvers have already checked the replacement-list
  // tape against the whole edited B envelope.  Do not second-guess those
  // proofs with a direct literal-body cursor walk: repeated formals,
  // empty actual slots, VA_OPT transitions, and higher-order generated
  // callees can all produce changed downstream body tokens while still
  // preserving the root invocation soundly.  The literal-body audit below
  // is only for local formal/operator rewrites that stamp a whole-envelope
  // range without such a complete replay witness.
  if (patch.proof.wholeEnvelopeReplay &&
      patch.proof.wholeEnvelopeReplay->replayValidated &&
      patch.proof.wholeEnvelopeReplay->rootMacroId ==
          patch.proof.proofRootMacroId)
    return true;

  const std::optional<std::pair<uint64_t, uint64_t>> cover =
      GetWholeCoverATokRange(m);
  if (!cover || cover->first >= cover->second)
    return true;

  const std::optional<std::pair<size_t, size_t>> wholeB =
      (*deps_.sourceMapper).MapATokRangeAToBTokenEnvelopePreserveBoundaryInsertions(cover->first,
                                                              cover->second);
  if (!wholeB || wholeB->first >= wholeB->second)
    return true;

  // The materialized whole-cover envelope intentionally preserves
  // zero-width insertions that are anchored exactly at either edge of the
  // macro expansion.  Those boundary insertions are not part of the
  // replacement-list replay obligation: they are emitted by the ordinary
  // boundary edit path next to the preserved invocation.  Use the
  // boundary-trimmed envelope for the literal body/formal/operator replay
  // audit, while still requiring the candidate to have claimed the wider
  // whole-cover envelope before this guard applies.
  std::pair<size_t, size_t> replayB = *wholeB;
  if (std::optional<std::pair<size_t, size_t>> trimmedB =
          (*deps_.sourceMapper).MapATokRangeAToBTokenEnvelope(cover->first, cover->second)) {
    if (wholeB->first <= trimmedB->first &&
        trimmedB->first <= trimmedB->second &&
        trimmedB->second <= wholeB->second)
      replayB = *trimmedB;
  }

  // This guard is only about whole-envelope claims.  Narrow args-only
  // materializations are checked by their local formal/paste/stringify
  // proof paths and do not compete as complete expansion replays here.
  if (patch.materializedBTokStart != static_cast<uint64_t>(wholeB->first) ||
      patch.materializedBTokEnd != static_cast<uint64_t>(wholeB->second))
    return true;

  // Use file-scope carriers for the whole-envelope replay tiling so the
  // literal-body audit helpers can be extracted without local type leakage.
  using ReplayElem = WholeEnvelopeReplayElem;

  SmallVector<TokenInterval, 32> argumentDependentIntervals;
  auto addArgumentDependentInterval = [&](uint64_t begin, uint64_t end) {
    if (begin >= end)
      return;
    argumentDependentIntervals.push_back({begin, end});
  };

  // Standard substitutions, stringification results, and pasted tokens
  // are all argument-dependent output surfaces: changing the invocation
  // actual can legitimately change those tokens.  Everything else in the
  // macro's replacement-list cover is fixed body surface and must replay
  // literally in B before a structure-preserving candidate may claim the
  // whole expansion envelope.
  for (const auto &as : m.argSpans) {
    if (as.kind == PPArgSpanKind::Standard)
      addArgumentDependentInterval(as.begin, as.end);
  }
  for (const auto &span : m.stringifySpans)
    addArgumentDependentInterval(span.begin, span.end);
  for (const auto &span : m.pasteSpans)
    addArgumentDependentInterval(span.begin, span.end);

  llvm::sort(argumentDependentIntervals, tokenIntervalLess);
  SmallVector<TokenInterval, 32> mergedArgumentIntervals;
  for (const TokenInterval &raw : argumentDependentIntervals) {
    if (raw.begin < cover->first || raw.end > cover->second ||
        raw.end < raw.begin) {
      REFOLD_LOG_TRACE("macro/proof",
            "suppress structure-preserving macro replay: inv id={0} "
            "name={1} argument-dependent surface escapes whole cover: "
            "surface=[{2},{3}) cover=[{4},{5}) text='{6}'",
            m.id, m.name, raw.begin, raw.end, cover->first, cover->second,
            stringutils::showWsWithClip(patch.replacement, 220));
      return false;
    }

    if (!mergedArgumentIntervals.empty() &&
        raw.begin <= mergedArgumentIntervals.back().end) {
      mergedArgumentIntervals.back().end =
          std::max(mergedArgumentIntervals.back().end, raw.end);
      continue;
    }
    mergedArgumentIntervals.push_back(raw);
  }

  SmallVector<TokenInterval, 32> fixedIntervals;
  auto appendFixedPiecesOutsideArguments = [&](TokenInterval body) {
    if (body.begin >= body.end)
      return true;
    if (body.begin < cover->first || body.end > cover->second ||
        body.end < body.begin)
      return false;

    uint64_t cursor = body.begin;
    for (const TokenInterval &arg : mergedArgumentIntervals) {
      if (arg.end <= cursor)
        continue;
      if (arg.begin >= body.end)
        break;
      if (arg.begin > cursor)
        fixedIntervals.push_back(
            {cursor, std::min<uint64_t>(arg.begin, body.end)});
      cursor = std::max(cursor, std::min<uint64_t>(arg.end, body.end));
    }
    if (cursor < body.end)
      fixedIntervals.push_back({cursor, body.end});
    return true;
  };

  for (const auto &bs : m.bodySpans) {
    if (!appendFixedPiecesOutsideArguments({bs.begin, bs.end})) {
      REFOLD_LOG_TRACE("macro/proof",
            "suppress structure-preserving macro replay: inv id={0} "
            "name={1} body span escapes whole cover: body=[{2},{3}) "
            "cover=[{4},{5}) text='{6}'",
            m.id, m.name, bs.begin, bs.end, cover->first, cover->second,
            stringutils::showWsWithClip(patch.replacement, 220));
      return false;
    }
  }

  SmallVector<ReplayElem, 64> elems;
  for (const TokenInterval &fixed : fixedIntervals)
    elems.push_back({false, fixed.begin, fixed.end});
  for (const TokenInterval &arg : mergedArgumentIntervals)
    elems.push_back({true, arg.begin, arg.end});

  if (elems.empty()) {
    REFOLD_LOG_TRACE("macro/proof",
          "suppress structure-preserving macro replay: inv id={0} "
          "name={1} whole-envelope replay has no body or argument "
          "surface to discharge: cover=[{2},{3}) text='{4}'",
          m.id, m.name, cover->first, cover->second,
          stringutils::showWsWithClip(patch.replacement, 220));
    return false;
  }

  llvm::sort(elems, [](const ReplayElem &lhs, const ReplayElem &rhs) {
    if (lhs.aBegin != rhs.aBegin)
      return lhs.aBegin < rhs.aBegin;
    if (lhs.aEnd != rhs.aEnd)
      return lhs.aEnd < rhs.aEnd;
    return lhs.isArgumentDependent < rhs.isArgumentDependent;
  });

  auto mapArgumentDependentSurfaceToReplayBEnvelope =
      [&](const ReplayElem &elem)
      -> std::optional<std::pair<size_t, size_t>> {
    std::optional<std::pair<size_t, size_t>> mapped =
        (*deps_.sourceMapper).MapATokRangeAToBTokenEnvelopePreserveBoundaryInsertions(elem.aBegin,
                                                                elem.aEnd);
    if (!mapped)
      return std::nullopt;
    if (mapped->first > mapped->second || mapped->second > replayB.second)
      return std::nullopt;
    return mapped;
  };

  uint64_t aCursor = cover->first;
  size_t bCursor = replayB.first;
  for (const ReplayElem &elem : elems) {
    if (elem.aBegin != aCursor || elem.aEnd < elem.aBegin ||
        elem.aEnd > cover->second) {
      REFOLD_LOG_TRACE("macro/proof",
            "suppress structure-preserving macro replay: inv id={0} "
            "name={1} whole-envelope replay is not an exact A tiling: "
            "elem=[{2},{3}) cursor={4} cover=[{5},{6}) text='{7}'",
            m.id, m.name, elem.aBegin, elem.aEnd, aCursor, cover->first,
            cover->second,
            stringutils::showWsWithClip(patch.replacement, 220));
      return false;
    }
    aCursor = elem.aEnd;

    if (elem.isArgumentDependent) {
      std::optional<std::pair<size_t, size_t>> argB =
          mapArgumentDependentSurfaceToReplayBEnvelope(elem);
      if (!argB) {
        REFOLD_LOG_TRACE("macro/proof",
              "suppress structure-preserving macro replay: inv id={0} "
              "name={1} argument-dependent surface has no B replay "
              "envelope: A=[{2},{3}) replayB=[{4},{5}) wholeB=[{6},{7}) "
              "text='{8}'",
              m.id, m.name, elem.aBegin, elem.aEnd, replayB.first,
              replayB.second, wholeB->first, wholeB->second,
              stringutils::showWsWithClip(patch.replacement, 220));
        return false;
      }
      if (argB->first != bCursor || argB->second < argB->first ||
          argB->second > replayB.second) {
        REFOLD_LOG_TRACE("macro/proof",
              "suppress structure-preserving macro replay: inv id={0} "
              "name={1} argument-dependent surface does not align with "
              "the B replay cursor: A=[{2},{3}) B=[{4},{5}) "
              "cursor={6} replayB=[{7},{8}) wholeB=[{9},{10}) text='{11}'",
              m.id, m.name, elem.aBegin, elem.aEnd, argB->first, argB->second,
              bCursor, replayB.first, replayB.second, wholeB->first,
              wholeB->second,
              stringutils::showWsWithClip(patch.replacement, 220));
        return false;
      }
      bCursor = argB->second;
      continue;
    }

    const size_t len = static_cast<size_t>(elem.aEnd - elem.aBegin);
    if (bCursor + len > replayB.second) {
      REFOLD_LOG_TRACE("macro/proof",
            "suppress structure-preserving macro replay: inv id={0} "
            "name={1} fixed body span overruns replay B envelope: "
            "A=[{2},{3}) cursor={4} len={5} replayB=[{6},{7}) "
            "wholeB=[{8},{9}) text='{10}'",
            m.id, m.name, elem.aBegin, elem.aEnd, bCursor, len, replayB.first,
            replayB.second, wholeB->first, wholeB->second,
            stringutils::showWsWithClip(patch.replacement, 220));
      return false;
    }

    bool fixedMatches = true;
    for (size_t i = 0; i < len; ++i) {
      if (deps_.aToks[static_cast<size_t>(elem.aBegin) + i].spelling !=
          deps_.bToks[bCursor + i].spelling) {
        fixedMatches = false;
        break;
      }
    }
    if (!fixedMatches) {
      REFOLD_LOG_TRACE("macro/proof",
            "suppress structure-preserving macro replay: inv id={0} "
            "name={1} fixed replacement-list body changed inside a "
            "claimed whole B envelope: A=[{2},{3}) B=[{4},{5}) "
            "Atext='{6}' Btext='{7}' replacement='{8}'",
            m.id, m.name, elem.aBegin, elem.aEnd, bCursor, bCursor + len,
            stringutils::showWsWithClip((*deps_.sourceMapper).SliceASource(elem.aBegin, elem.aEnd),
                                        120),
            stringutils::showWsWithClip((*deps_.sourceMapper).SliceBSource(bCursor, bCursor + len),
                                        120),
            stringutils::showWsWithClip(patch.replacement, 220));
      return false;
    }
    bCursor += len;
  }

  if (aCursor != cover->second || bCursor != replayB.second) {
    REFOLD_LOG_TRACE("macro/proof",
          "suppress structure-preserving macro replay: inv id={0} "
          "name={1} whole-envelope replay did not consume exact cover: "
          "Acur={2} Aend={3} Bcur={4} Bend={5} wholeB=[{6},{7}) "
          "text='{8}'",
          m.id, m.name, aCursor, cover->second, bCursor, replayB.second,
          wholeB->first, wholeB->second,
          stringutils::showWsWithClip(patch.replacement, 220));
    return false;
  }

  return true;
}

bool RefoldMacroPatchPlanner::RootPreservingCandidateHasLiteralFixedRootBodyReplay(
    const RefoldModel::MacroInvocation &m, StringRef baseInvText,
    const MacroPatch &patch) const {
      if (!patch.proof.preservesInvocationStructure ||
          patch.proof.proofRootMacroId != m.id)
        return true;
      if (patch.proof.kind == MacroPatchProofKind::WholeCoverRealization)
        return true;

      // Most whole-envelope witnesses are selector/template summaries: they
      // can prove an argument or generated-callee surface without proving
      // every fixed root-body token that a preserved invocation will
      // regenerate.  The definition-tape replay solver is stronger: it
      // replays the recorded replacement list itself against B, including
      // empty variadic slots and __VA_OPT__ branch choices.  Do not
      // second-guess that complete transducer proof with per-token A->B
      // mapping, because variadic erasure can legitimately leave stable
      // punctuation without a one-token diff envelope.
      if (patch.proof.wholeEnvelopeReplay &&
          patch.proof.wholeEnvelopeReplay->definitionTapeReplayValidated &&
          patch.proof.wholeEnvelopeReplay->rootMacroId ==
              patch.proof.proofRootMacroId)
        return true;

      const std::optional<std::pair<uint64_t, uint64_t>> cover =
          GetWholeCoverATokRange(m);
      if (!cover || cover->first >= cover->second)
        return true;

      // Reuse the file-scope interval carrier for fixed-root body replay
      // exclusions so normalization helpers can be lifted later.
      auto normalizeIntervals = [&](SmallVectorImpl<TokenInterval> &spans) {
        llvm::sort(spans, tokenIntervalLess);
        SmallVector<TokenInterval, 16> merged;
        for (const TokenInterval &span : spans) {
          if (span.begin >= span.end)
            continue;
          if (!merged.empty() && span.begin <= merged.back().end) {
            merged.back().end = std::max(merged.back().end, span.end);
            continue;
          }
          merged.push_back(span);
        }
        spans.clear();
        spans.append(merged.begin(), merged.end());
      };

      SmallVector<TokenInterval, 32> excludedA;
      for (const auto &span : m.argSpans) {
        if (span.kind == PPArgSpanKind::Standard)
          addNonEmptyTokenInterval(excludedA, span.begin, span.end);
      }
      for (const auto &span : m.stringifySpans)
        addNonEmptyTokenInterval(excludedA, span.begin, span.end);
      for (const auto &span : m.pasteSpans)
        addNonEmptyTokenInterval(excludedA, span.begin, span.end);

      // Producer body spans on a root invocation can include tokens emitted
      // by nested/generated macro calls.  Those tokens are not fixed root
      // body: they are discharged by the descendant proof that the root
      // candidate rewrites or by whole-cover realization.  Exclude every
      // descendant cover before asking whether the remaining root-owned body
      // text is still literal.
      DenseMap<uint64_t, const RefoldModel::MacroInvocation *> invById;
      for (const auto &candidate : (*deps_.model).GetMacroInvocations())
        invById[candidate.id] = &candidate;
      SmallVector<std::pair<size_t, size_t>, 1> rootBodyReplayArgRanges;
      const MacroSubtreeReplayValidationContext rootBodyReplaySubtreeCtx{
          m, baseInvText, rootBodyReplayArgRanges, invById};

      for (const auto &candidate : (*deps_.model).GetMacroInvocations()) {
        if (candidate.id == m.id || !candidate.cover.IsValid() ||
            candidate.cover.begin >= candidate.cover.end ||
            !CandidateBelongsToValidatedSubtree(rootBodyReplaySubtreeCtx,
                                                 candidate))
          continue;
        const uint64_t begin =
            std::max<uint64_t>(candidate.cover.begin, cover->first);
        const uint64_t end =
            std::min<uint64_t>(candidate.cover.end, cover->second);
        addNonEmptyTokenInterval(excludedA, begin, end);
      }

      normalizeIntervals(excludedA);

      auto appendFixedPiecesOutsideExcludedSurfaces =
          [&](SmallVectorImpl<TokenInterval> &fixed,
              TokenInterval body) -> bool {
        if (body.begin >= body.end)
          return true;
        if (body.begin < cover->first || body.end > cover->second ||
            body.end < body.begin)
          return false;

        uint64_t cursor = body.begin;
        for (const TokenInterval &excluded : excludedA) {
          if (excluded.end <= cursor)
            continue;
          if (excluded.begin >= body.end)
            break;
          if (excluded.begin > cursor)
            fixed.push_back(
                {cursor, std::min<uint64_t>(excluded.begin, body.end)});
          cursor =
              std::max(cursor, std::min<uint64_t>(excluded.end, body.end));
        }
        if (cursor < body.end)
          fixed.push_back({cursor, body.end});
        return true;
      };

      SmallVector<TokenInterval, 32> fixedBodyA;
      for (const auto &span : m.bodySpans) {
        const uint64_t begin = std::max<uint64_t>(span.begin, cover->first);
        const uint64_t end = std::min<uint64_t>(span.end, cover->second);
        if (!appendFixedPiecesOutsideExcludedSurfaces(fixedBodyA,
                                                      {begin, end})) {
          REFOLD_LOG_TRACE("macro/proof",
                "suppress structure-preserving macro replay: inv id={0} "
                "name={1} fixed root body span escapes whole cover: "
                "body=[{2},{3}) cover=[{4},{5}) replacement='{6}'",
                m.id, m.name, span.begin, span.end, cover->first,
                cover->second,
                stringutils::showWsWithClip(patch.replacement, 220));
          return false;
        }
      }

      for (const TokenInterval &fixed : fixedBodyA) {
        for (uint64_t aTok = fixed.begin; aTok < fixed.end; ++aTok) {
          if (static_cast<size_t>(aTok) >= deps_.aToks.size())
            return false;

          std::optional<std::pair<size_t, size_t>> bTok =
              (*deps_.sourceMapper).MapATokRangeAToBTokenEnvelope(aTok, aTok + 1);
          if (!bTok || bTok->first >= bTok->second) {
            REFOLD_LOG_TRACE("macro/proof",
                  "suppress structure-preserving macro replay: inv id={0} "
                  "name={1} fixed root body token has no B replay "
                  "envelope: A=[{2},{3}) Atext='{4}' replacement='{5}'",
                  m.id, m.name, aTok, aTok + 1,
                  stringutils::showWsWithClip((*deps_.sourceMapper).SliceASource(aTok, aTok + 1),
                                              120),
                  stringutils::showWsWithClip(patch.replacement, 220));
            return false;
          }

          if (bTok->second != bTok->first + 1 ||
              bTok->first >= deps_.bToks.size() ||
              deps_.aToks[static_cast<size_t>(aTok)].spelling !=
                  deps_.bToks[bTok->first].spelling) {
            REFOLD_LOG_TRACE("macro/proof",
                  "suppress structure-preserving macro replay: inv id={0} "
                  "name={1} fixed root body changed while preserving the "
                  "root invocation: A=[{2},{3}) B=[{4},{5}) Atext='{6}' "
                  "Btext='{7}' replacement='{8}'",
                  m.id, m.name, aTok, aTok + 1, bTok->first, bTok->second,
                  stringutils::showWsWithClip((*deps_.sourceMapper).SliceASource(aTok, aTok + 1),
                                              120),
                  stringutils::showWsWithClip(
                      (*deps_.sourceMapper).SliceBSource(bTok->first, bTok->second), 120),
                  stringutils::showWsWithClip(patch.replacement, 220));
            return false;
          }
        }
      }

      return true;
}

bool RefoldMacroPatchPlanner::MacroCandidateReplayIsStableForFinalSelection(
    const MacroSubtreeReplayValidationContext &ctx,
    const MacroPatch &patch) const {
  const RefoldModel::MacroInvocation &m = ctx.rootInvocation;
  return StructurePreservingCallsiteHasStableFormalSyntax(patch, m) &&
         ArgsOnlyWholeEnvelopeCandidateHasLiteralBodyReplay(m, patch) &&
         RootPreservingCandidateHasLiteralFixedRootBodyReplay(
             m, ctx.rootInvocationText, patch) &&
         ClaimedWholeEnvelopeIsReplaySafe(ctx, patch) &&
         !CallsiteReplayObservesActiveHeaderMacroState(m, patch);
}

MacroPatch RefoldMacroPatchPlanner::MaterializeWholeCoverPatch(
    const WholeCoverCandidate &candidate) const {
  // Whole-cover realization construction is intentionally narrow: the caller
  // supplies the already-proven plan and invocation span, and this method only
  // builds the emitted patch and attaches the same proof carrier used by the
  // historical in-line selector.
  MacroPatch patch{candidate.invocationStart, candidate.invocationEnd,
                   candidate.plan.clippedText, candidate.invocation.id};
  const WholeCoverAdmissionContext wholeCoverAdmissionCtx{
      WholeCoverProofInputs{candidate.invocation, candidate.plan}};
  StampWholeCoverAcceptedCandidate(wholeCoverAdmissionCtx, patch);
  return patch;
}

std::optional<MacroPatch> RefoldMacroPatchPlanner::SelectWholeCoverPatch(
    const WholeCoverFinalSelectionContext &ctx) const {
  // Final whole-cover-family selection is intentionally after all candidate
  // builders have run.  This method only admits the already-discovered
  // candidates in the historical order and delegates ranking to the existing
  // proof lattice; it does not discover new fallback paths.
  SmallVector<FinalMacroCandidate, 5> finalMacroCandidates;
  FinalMacroCandidateAdmissionContext finalAdmissionCtx{
      ctx.invocation, ctx.effectiveHunk, ctx.baseInvocationText,
      finalMacroCandidates, &ctx.replayStabilityCtx,
      ctx.allowNonTopLevelMacroSelectorFailure};

  if (ctx.argsOnlyCandidate &&
      MacroCandidateReplayIsStableForFinalSelection(
          ctx.replayStabilityCtx, *ctx.argsOnlyCandidate)) {
    AddFinalMacroCandidate(
        finalAdmissionCtx,
        FinalMacroCandidate{*ctx.argsOnlyCandidate, MacroSelectionCandidate{},
                            FinalMacroCandidateOrigin::DirectArgsOnly});
  }

  if (ctx.dagRootCandidate &&
      MacroCandidateReplayIsStableForFinalSelection(
          ctx.replayStabilityCtx, *ctx.dagRootCandidate)) {
    AddFinalMacroCandidate(
        finalAdmissionCtx,
        FinalMacroCandidate{*ctx.dagRootCandidate, MacroSelectionCandidate{},
                            FinalMacroCandidateOrigin::DagRootReplay});
  }

  if (ctx.canReuseExistingCallsiteNoOp && ctx.existingPatch &&
      MacroCandidateReplayIsStableForFinalSelection(
          ctx.replayStabilityCtx, *ctx.existingPatch)) {
    AddFinalMacroCandidate(
        finalAdmissionCtx,
        FinalMacroCandidate{*ctx.existingPatch, MacroSelectionCandidate{},
                            FinalMacroCandidateOrigin::
                                ReuseExistingCallsiteNoOp});
  }

  if (ctx.canReuseExistingCallsiteSkipWholeCover && ctx.existingPatch &&
      MacroCandidateReplayIsStableForFinalSelection(
          ctx.replayStabilityCtx, *ctx.existingPatch)) {
    AddFinalMacroCandidate(
        finalAdmissionCtx,
        FinalMacroCandidate{*ctx.existingPatch, MacroSelectionCandidate{},
                            FinalMacroCandidateOrigin::
                                ReuseExistingCallsiteSkipWholeCover});
  }

  if (ctx.canReuseExistingExpanded && ctx.existingExpandedPatch &&
      !TheoremLatticeStructureCandidateDominatesRealization(
          finalAdmissionCtx, *ctx.existingExpandedPatch)) {
    // Reuse of an already-expanded same-owner patch is a realization carrier.
    // Structure-vs-realization preference is expressed through the shared
    // lattice selector instead of a path-local origin comparison.
    AddFinalMacroCandidate(
        finalAdmissionCtx,
        FinalMacroCandidate{*ctx.existingExpandedPatch, MacroSelectionCandidate{},
                            FinalMacroCandidateOrigin::ReuseExistingExpanded});
  }

  if (ctx.wholeCoverPlan) {
    // Whole-cover realization is added as another final candidate unless a
    // selectable structure-preserving candidate for the same root already wins
    // the named lattice tie-breaker above.
    const WholeCoverCandidate wholeCoverCandidate{
        ctx.invocation, ctx.invocationStart, ctx.invocationEnd,
        *ctx.wholeCoverPlan};
    MacroPatch patch = MaterializeWholeCoverPatch(wholeCoverCandidate);
    if (!TheoremLatticeStructureCandidateDominatesRealization(
            finalAdmissionCtx, patch))
      AddFinalMacroCandidate(
          finalAdmissionCtx,
          FinalMacroCandidate{patch, MacroSelectionCandidate{},
                              FinalMacroCandidateOrigin::WholeCoverRealization});
  }

  if (finalAdmissionCtx.candidates.empty())
    return std::nullopt;

  // The macro selector ranks MacroSelectionCandidate objects, not emitted
  // AcceptedResultCandidate objects.  This keeps selector-only nested macro
  // proofs out of the theorem-facing accepted-result selector while preserving
  // the parallel final-admission candidate array for recovering the selected
  // patch.
  const std::optional<SelectedMacroSelectionCandidate> selectedCandidate =
      SelectPreferredFinalMacroCandidate(finalAdmissionCtx);
  if (!selectedCandidate) {
    REFOLD_LOG_TRACE(
        "macro/proof",
        "no final macro candidate survived proof-discharge gating: inv id={0} "
        "name={1} candidates={2} allowNestedSelectorOnly={3}",
        ctx.invocation.id, ctx.invocation.name,
        finalAdmissionCtx.candidates.size(),
        finalAdmissionCtx.allowNonTopLevelMacroSelectorFailure ? 1 : 0);
    return std::nullopt;
  }

  const FinalMacroCandidate &selected =
      finalAdmissionCtx.candidates[selectedCandidate->index];

  switch (selected.origin) {
  case FinalMacroCandidateOrigin::DirectArgsOnly:
    break;

  case FinalMacroCandidateOrigin::DagRootReplay:
    break;

  case FinalMacroCandidateOrigin::ReuseExistingCallsiteNoOp:
    if (selected.patch.subtreeCertBacked)
      REFOLD_LOG_TRACE("macro/proof",
            "subtree continuity probe: reused subtree-backed callsite patch "
            "without a fresh subtree winner in this pass inv id={0} name={1}",
            ctx.invocation.id, ctx.invocation.name);
    break;

  case FinalMacroCandidateOrigin::ReuseExistingCallsiteSkipWholeCover:
    if (selected.patch.subtreeCertBacked) {
      REFOLD_LOG_TRACE("macro/proof",
            "subtree continuity probe: reused subtree-backed callsite patch "
            "from skip-whole-cover path without a fresh subtree winner inv "
            "id={0} "
            "name={1}",
            ctx.invocation.id, ctx.invocation.name);
    }
    break;

  case FinalMacroCandidateOrigin::ReuseExistingExpanded:
    break;

  case FinalMacroCandidateOrigin::WholeCoverRealization:
    break;
  }

  MacroPatch selectedPatch = selected.patch;
  switch (selected.origin) {
  case FinalMacroCandidateOrigin::ReuseExistingCallsiteNoOp:
  case FinalMacroCandidateOrigin::ReuseExistingCallsiteSkipWholeCover:
  case FinalMacroCandidateOrigin::ReuseExistingExpanded:
    StampReusedMacroPatchAcceptedCandidate(ctx.reuseAdmissionCtx, selectedPatch);
    break;
  case FinalMacroCandidateOrigin::DirectArgsOnly:
  case FinalMacroCandidateOrigin::DagRootReplay:
  case FinalMacroCandidateOrigin::WholeCoverRealization:
    break;
  }
  StampSelectedFinalMacroCandidate(
      finalAdmissionCtx, *selectedCandidate, selectedPatch);
  return selectedPatch;
}

/// Explicit reuse-admission state for same-span macro patches.
///
/// The planner may learn about an existing patch through two routes: the
/// caller-owned patch map, or the caller-coalesced same-pass context passed
/// alongside the request.  This private carrier keeps those reuse facts in one
/// named bundle so helper methods can reason about reuse without capturing
/// scattered local pointers and flags, while still borrowing all patch storage
/// from the caller.
struct RefoldMacroPatchPlanner::MacroPatchReuseAdmissionContext {
  const RefoldModel::MacroInvocation &invocation;
  const Owner &currentPatchOwner;
  std::optional<uint64_t> ownerIncludeId;
  uint64_t invocationStart = 0;
  uint64_t invocationEnd = 0;

  const MacroPatch *existingPatch = nullptr;
  const MacroPatch *existingExpandedPatch = nullptr;
  bool existingIsCallsite = false;
  bool existingCameFromCallerContext = false;
  bool existingExpandedCameFromCallerContext = false;
};

RefoldMacroPatchPlanner::MacroPatchReuseAdmissionContext
RefoldMacroPatchPlanner::RecoverWholeCoverReuseContext(
    const RefoldModel::MacroInvocation &invocation,
    const Owner &currentPatchOwner, uint64_t invocationStart,
    uint64_t invocationEnd,
    const DenseMap<std::optional<uint64_t>, DenseMap<uint64_t, MacroPatch>>
        &patchMap,
    ExistingMacroPatchContext existingContext) const {
  // Reuse discovery runs before whole-cover candidate assembly because
  // same-span reuse must see both the caller-owned patch map and the already
  // coalesced same-pass context. The resulting carrier borrows those accepted
  // patches; it does not mutate map ownership or change the deterministic
  // representative choice.
  MacroPatchReuseAdmissionContext ctx{invocation, currentPatchOwner,
                                      invocation.ownerIncludeId,
                                      invocationStart, invocationEnd};
  CollectExistingMacroPatchReuseFromMap(ctx, patchMap);
  AdmitCallerExistingMacroPatchContext(ctx, existingContext);
  return ctx;
}

bool RefoldMacroPatchPlanner::ExistingPatchMatchesReuseSite(
    const MacroPatchReuseAdmissionContext &ctx,
    const MacroPatch &patch) const {
  return patch.invStart == ctx.invocationStart &&
         patch.invEnd == ctx.invocationEnd &&
         MacroPatchOwnerMatches(patch, ctx.currentPatchOwner);
}

bool RefoldMacroPatchPlanner::ExistingPatchPreservesCurrentInvocation(
    const MacroPatchReuseAdmissionContext &ctx,
    const MacroPatch &patch) const {
  return macroPatchIsCallsiteForInvocation(patch, ctx.invocation);
}

void RefoldMacroPatchPlanner::CollectExistingMacroPatchReuseFromMap(
    MacroPatchReuseAdmissionContext &ctx,
    const DenseMap<std::optional<uint64_t>, DenseMap<uint64_t, MacroPatch>>
        &patchMap) const {
  auto ownerIt = patchMap.find(ctx.ownerIncludeId);
  if (ownerIt == patchMap.end())
    return;

  std::optional<uint64_t> bestNonCallsiteId;
  std::optional<uint64_t> bestCallsiteId;

  for (const auto &kv : ownerIt->second) {
    const uint64_t id = kv.first;
    const MacroPatch &patch = kv.second;

    // Only same physical bytes under the same owner certificate are reusable.
    if (!ExistingPatchMatchesReuseSite(ctx, patch))
      continue;

    if (!ExistingPatchPreservesCurrentInvocation(ctx, patch)) {
      if (!bestNonCallsiteId || id < *bestNonCallsiteId)
        bestNonCallsiteId = id;
    } else {
      if (!bestCallsiteId || id < *bestCallsiteId)
        bestCallsiteId = id;
    }
  }

  if (bestNonCallsiteId) {
    auto it = ownerIt->second.find(*bestNonCallsiteId);
    if (it != ownerIt->second.end())
      ctx.existingExpandedPatch = &it->second;
  }

  if (bestCallsiteId) {
    auto it = ownerIt->second.find(*bestCallsiteId);
    if (it != ownerIt->second.end()) {
      ctx.existingPatch = &it->second;
      ctx.existingIsCallsite =
          ExistingPatchPreservesCurrentInvocation(ctx, it->second);
    }
  }
}

void RefoldMacroPatchPlanner::AdmitCallerExistingMacroPatchContext(
    MacroPatchReuseAdmissionContext &ctx,
    RefoldMacroPatchPlanner::ExistingMacroPatchContext existingContext) const {
  if (ctx.existingPatch || ctx.existingExpandedPatch || !existingContext.patch)
    return;

  const MacroPatch &patch = *existingContext.patch;
  if (!ExistingPatchMatchesReuseSite(ctx, patch))
    return;

  const bool contextIsCallsite =
      existingContext.isCallsite &&
      ExistingPatchPreservesCurrentInvocation(ctx, patch);
  if (contextIsCallsite) {
    ctx.existingPatch = &patch;
    ctx.existingIsCallsite = true;
    ctx.existingCameFromCallerContext = true;
  } else {
    ctx.existingExpandedPatch = &patch;
    ctx.existingExpandedCameFromCallerContext = true;
  }
}



std::optional<MacroPatch>
RefoldMacroPatchPlanner::TryPairedPureInsertionRootArgsOnlyPatch(
    const WholeCoverArgsOnlyCandidateContext &ctx) const {
  const RefoldModel::MacroInvocation &m = ctx.invocation;
  const diffutils::Hunk &hEff = ctx.effectiveHunk;
  ArrayRef<RefoldModel::PPArgSpan> argLikeSpans = ctx.argLikeSpans;
  StringRef baseInvText = ctx.baseInvocationText;

  // This path is only for pure insertions in B. Non-insertion edits already
  // carry an A-side range and should use the ordinary args-only path.
  if (hEff.aStart != hEff.aEnd || hEff.bStart >= hEff.bEnd)
    return std::nullopt;
  if (argLikeSpans.empty())
    return std::nullopt;

  // Paste spans need paste-specific replay/invertibility logic. Do not try
  // to explain paste edits by pairing generic insertion frontiers.
  if (!m.pasteSpans.empty())
    return std::nullopt;

  // If the current hunk is already fully contained in an argument span, then
  // it is not the split-frontier case this recovery path is meant for.
  SmallVector<char, 16> curTouched(argLikeSpans.size(), 0);
  if ((*deps_.sourceMapper).HunkFullyWithinArgSpans(hEff, argLikeSpans,
                                                    curTouched))
    return std::nullopt;

  // Require a real callsite-shaped invocation spelling. This recovery
  // produces a structure-preserving invocation rewrite, not an already-
  // expanded payload.
  StringRef invSpanText =
      !baseInvText.empty()
          ? baseInvText
          : (m.invText ? StringRef(*m.invText) : StringRef(""));
  if (!RefoldLineObserverLayout::InvocationSpanMatchesCallsitePrefix(invSpanText,
                                                                     m))
    return std::nullopt;

  // Candidate ownership is tested against ordinary and stringify argument
  // occurrences. Paste occurrences were rejected above.
  std::vector<RefoldModel::PPArgSpan> occs;
  append_range(occs, m.argSpans);
  append_range(occs, m.stringifySpans);
  if (occs.empty())
    return std::nullopt;

  for (const auto &partner : (*deps_.abTokHunks)) {
    // Pair only with another pure B insertion. Replacement/deletion hunks are
    // outside this split-insertion recovery proof.
    if (partner.aStart != partner.aEnd || partner.bStart >= partner.bEnd)
      continue;

    // Do not pair the hunk with itself.
    if (partner.aStart == hEff.aStart && partner.bStart == hEff.bStart &&
        partner.bEnd == hEff.bEnd)
      continue;

    // Both insertion frontiers must live inside this macro invocation cover.
    if (!(m.cover.begin <= partner.aStart && partner.aEnd <= m.cover.end))
      continue;

    auto isCanonicalLeader = [&](const diffutils::Hunk &lhs,
                                 const diffutils::Hunk &rhs) {
      // Each pair is considered once. The lower A frontier leads; B
      // coordinates provide deterministic tie-breakers for same-gap insertions.
      if (lhs.aStart != rhs.aStart)
        return lhs.aStart < rhs.aStart;
      if (lhs.bStart != rhs.bStart)
        return lhs.bStart < rhs.bStart;
      return lhs.bEnd < rhs.bEnd;
    };
    if (!isCanonicalLeader(hEff, partner))
      continue;

    const diffutils::Hunk env = buildCombinedInsertionEnvelope(hEff, partner);

    // Remove unchanged matching edge tokens so the synthetic envelope exposes
    // only the edited core between the paired insertion frontiers.
    const diffutils::Hunk envTrim =
        trimCommonEdgeTokens(env, deps_.aToks, deps_.bToks);

    // The trimmed synthetic envelope must be fully explainable by argument
    // occurrences. Otherwise the paired insertions are not an args-only edit.
    std::vector<char> touchedOcc(occs.size(), 0);
    if (!(*deps_.sourceMapper).HunkFullyWithinArgSpans(envTrim, occs,
                                                       touchedOcc))
      continue;

    // Require the envelope to touch exactly one formal argument. If it spans
    // multiple formals, there is no single invocation argument replacement to
    // delegate to the standard args-only builder.
    SmallVector<uint32_t, 4> touchedArgs;
    for (size_t i = 0; i < occs.size(); ++i) {
      if (!touchedOcc[i])
        continue;
      if (!llvm::is_contained(touchedArgs, occs[i].argIdx))
        touchedArgs.push_back(occs[i].argIdx);
    }
    if (touchedArgs.size() != 1)
      continue;

    // Once the paired insertions have been converted into a proof-compatible
    // argument envelope, reuse the ordinary args-only builder and validation.
    auto patch = BuildMacroInvocationPatchArgsOnly(m, envTrim, baseInvText);
    if (!patch)
      continue;

    GetProofLattice().SetMacroPatchProof(
        *patch,
        GetProofLattice().MakeMacroPatchProof(
            MacroPatchProofKind::ArgsOnlyPairedPureInsertion,
            /*preservesInvocationStructure=*/true, m.id));
    return patch;
  }

  return std::nullopt;
}

RefoldMacroPatchPlanner::WholeCoverArgsOnlyCandidateResult
RefoldMacroPatchPlanner::BuildWholeCoverArgsOnlyCandidate(
    const WholeCoverArgsOnlyCandidateContext &ctx) const {
  WholeCoverArgsOnlyCandidateResult result;
  const RefoldModel::MacroInvocation &m = ctx.invocation;
  const diffutils::Hunk &hEff = ctx.effectiveHunk;
  ArrayRef<RefoldModel::PPArgSpan> argLikeSpans = ctx.argLikeSpans;
  StringRef baseInvText = ctx.baseInvocationText;
  const MacroPatchReuseAdmissionContext &reuseAdmission = ctx.reuseAdmission;

  SmallVector<char, 16> argTouched(argLikeSpans.size(), 0);
  StringRef invSpanText =
      !baseInvText.empty()
          ? baseInvText
          : (m.invText ? StringRef(*m.invText) : StringRef(""));
  result.rootHasDirectArgLikeSurface =
      !argLikeSpans.empty() &&
      (*deps_.sourceMapper).HunkFullyWithinArgSpans(hEff, argLikeSpans,
                                                    argTouched) &&
      RefoldLineObserverLayout::InvocationSpanMatchesCallsitePrefix(invSpanText,
                                                                    m);

  if (result.rootHasDirectArgLikeSurface) {
    // First try to patch arguments in-place. If that cannot satisfy the edit,
    // DAG lifting may still preserve deeper nested structure, so keep the
    // candidate instead of returning immediately.
    result.argsOnlyCandidate =
        BuildMacroInvocationPatchArgsOnly(m, hEff, baseInvText);
    if (!result.argsOnlyCandidate) {
      // If an existing callsite patch already satisfies this trimmed hunk,
      // defer reuse until DAG chaining has had a chance to compete.
      result.reuseExistingCallsitePatch =
          reuseAdmission.existingPatch && reuseAdmission.existingIsCallsite &&
          !baseInvText.empty() &&
          reuseAdmission.existingPatch->proof.preservesInvocationStructure &&
          reuseAdmission.existingPatch->proof.proofRootMacroId == m.id;
    }
    return result;
  }

  if (RefoldLineObserverLayout::InvocationSpanMatchesCallsitePrefix(invSpanText,
                                                                    m)) {
    // The definition-replay proof inside the args-only builder can handle edits
    // whose token hunk spans both argument substitutions and macro-body tokens,
    // most importantly __VA_OPT__ erasure/exposure.
    result.argsOnlyCandidate =
        BuildMacroInvocationPatchArgsOnly(m, hEff, baseInvText);
    if (!result.argsOnlyCandidate && !argLikeSpans.empty())
      result.argsOnlyCandidate =
          TryPairedPureInsertionRootArgsOnlyPatch(ctx);
  }

  return result;
}



bool RefoldMacroPatchPlanner::GeneratedCalleeReplayPreservesEnvelope(
    const GeneratedCalleeReplayContext &ctx) const {
  return ctx.wholeCoverATokens.first < ctx.wholeCoverATokens.second &&
         ctx.bTokenEnvelope.first < ctx.bTokenEnvelope.second;
}

bool RefoldMacroPatchPlanner::GeneratedCalleeReplayIsAdmissible(
    const GeneratedCalleeReplayContext &ctx) const {
  return ctx.followedGeneratedCall && ctx.currentDefinition &&
         !ctx.currentActuals.empty() &&
         macroDefinitionAcceptsActualCount(*ctx.currentDefinition,
                                           ctx.currentActuals.size());
}

std::optional<MacroPatch>
RefoldMacroPatchPlanner::BuildGeneratedCalleeReplayCandidate(
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
  const std::pair<size_t, size_t> *bEnv =
      &generatedCalleeCtx.bTokenEnvelope;
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
            if (tok.kind ==
                RefoldModel::MacroReplacementTokenKind::Literal) {
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

  // Resolve the replacement-list expression that supplies the generated
  // callee to the current macro formal that owns the callee spelling.  The
  // direct case is the usual `F(...)` shape.  The selector case covers a
  // producer-proven, deterministic function-like selector such as
  // `SELECT(F)(x)`, where `SELECT(f)` replays to exactly one of its formals.
  // This is still a structural proof over replacement-list tokens: the
  // selector macro must be function-like, arity-compatible, and its whole
  // replacement list must be the selected formal.  No text-keyed guessing is
  // used to decide which root argument owns the generated callee.
  std::function<std::optional<uint32_t>(size_t, size_t, unsigned)>
      resolveCalleeParamInRange =
          [&](size_t begin, size_t end,
              unsigned depth) -> std::optional<uint32_t> {
    if (begin >= end || end > toks.size() ||
        depth > (*deps_.model).GetMacroDirectives().size())
      return std::nullopt;

    if (end == begin + 1 &&
        toks[begin].kind ==
            RefoldModel::MacroReplacementTokenKind::ParamRef &&
        toks[begin].paramIndex &&
        *toks[begin].paramIndex < definition.defParams.size())
      return *toks[begin].paramIndex;

    if (begin + 3 > end ||
        toks[begin].kind !=
            RefoldModel::MacroReplacementTokenKind::Literal ||
        !isLiteralToken(begin + 1, "("))
      return std::nullopt;

    auto selectorClose = findMatchingParen(begin + 1, end);
    if (!selectorClose || *selectorClose + 1 != end)
      return std::nullopt;

    const RefoldModel::MacroDirective *selectorDefinition =
        ResolveFunctionLikeMacroForReplay(StringRef(toks[begin].spelling));
    if (!selectorDefinition || selectorDefinition->defParams.empty())
      return std::nullopt;

    SmallVector<std::pair<size_t, size_t>, 8> selectorArgs;
    if (!collectTopLevelArgRanges(begin + 1, *selectorClose,
                                  selectorArgs) ||
        !macroDefinitionAcceptsActualCount(*selectorDefinition,
                                      selectorArgs.size()))
      return std::nullopt;

    // A generated-callee selector is deterministic only when the selector's
    // replacement tape is exactly one non-variadic formal.  Richer selector
    // expressions can be added later as their own replay proof, but they do
    // not belong in this single-owner callee-slot inversion.
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

    const auto selectedArg = selectorArgs[*selected.paramIndex];
    return resolveCalleeParamInRange(selectedArg.first, selectedArg.second,
                                     depth + 1);
  };

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
      auto resolved = resolveCalleeParamInRange(begin, open, 0);
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


auto splitVariadicPackSourceSlot = [&](const GeneratedCalleeSourceSlot &slot,
                                       SmallVectorImpl<GeneratedCalleeSourceSlot> &out) {
  SmallVector<RefoldLexBoundaryToken, 32> toks;
  refoldLexBoundaryTokens(StringRef(slot.text), (*deps_.lexLang), toks);

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
    pieces.push_back({static_cast<size_t>(elem.data() - slot.text.data()),
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

  // A variadic formal can be forwarded into a fixed-arity generated callee.
  // The producer records the root variadic tail as one invocation argument
  // range (`foo, bar, baz`), but substituting `__VA_ARGS__` into a
  // generated call exposes those comma-separated elements as positional
  // actuals. Split only at commas that macro argument collection would see:
  // nested parentheses protect commas, while brackets/braces intentionally
  // do not. Each piece keeps the whole root variadic source as its rewrite
  // owner so multiple solved final parameters can be composed back into one
  // root argument replacement.
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
        toks[i + 1].kind ==
            RefoldModel::MacroReplacementTokenKind::Literal &&
        toks[i + 1].spelling == "(" &&
        ResolveFunctionLikeMacroForReplay(
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
            !isMacroDirectiveVariadicParam(definition, *toks[i + 3].paramIndex) ||
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
              isMacroDirectiveVariadicParam(definition, *toks[i + 1].paramIndex) ||
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

for (size_t depth = 0; depth <= (*deps_.model).GetMacroDirectives().size();
     ++depth) {
  auto shape = findGeneratedCallShape(*generatedCalleeCtx.currentDefinition);
  if (!shape)
    break;
  if (shape->calleeParamIdx >= generatedCalleeCtx.currentActuals.size())
    return std::nullopt;

  uint32_t nextAliasHops = 0;
  const RefoldModel::MacroDirective *nextDefinition =
      ResolveFunctionLikeMacroThroughAliasesWithHops(
          StringRef(generatedCalleeCtx.currentActuals[shape->calleeParamIdx].text),
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

const bool finalHasVariadic = !generatedCalleeCtx.currentDefinition->defParams.empty() &&
                              generatedCalleeCtx.currentDefinition->defParams.back().variadic;
const size_t finalFixed = finalHasVariadic
                              ? generatedCalleeCtx.currentDefinition->defParams.size() - 1
                              : generatedCalleeCtx.currentDefinition->defParams.size();

SmallVector<std::string, 8> oldActuals;
SmallVector<uint32_t, 8> rootSlotByFinalParam;
SmallVector<std::string, 8> rootSourceByFinalParam;
for (size_t i = 0; i < finalFixed; ++i) {
  oldActuals.push_back(generatedCalleeCtx.currentActuals[i].text);
  rootSlotByFinalParam.push_back(generatedCalleeCtx.currentActuals[i].rootArgIdx);
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
if (oldActuals.size() != generatedCalleeCtx.currentDefinition->defParams.size() ||
    rootSlotByFinalParam.size() != oldActuals.size() ||
    rootSourceByFinalParam.size() != oldActuals.size())
  return std::nullopt;

auto lexReplayTokens = [&](StringRef text,
                           SmallVectorImpl<ReplayTok> &out) {
  out.clear();
  SmallVector<RefoldLexBoundaryToken, 32> toks;
  refoldLexBoundaryTokens(text, (*deps_.lexLang), toks);
  for (const RefoldLexBoundaryToken &tok : toks)
    out.push_back(ReplayTok{tok.spelling, tok.begin, tok.end});
};

auto tokenSpellingsForText = [&](StringRef text) {
  SmallVector<ReplayTok, 16> toks;
  lexReplayTokens(text, toks);
  SmallVector<std::string, 16> out;
  for (const ReplayTok &tok : toks)
    out.push_back(tok.spelling);
  return out;
};

auto textsTokenEquivalent = [&](StringRef lhs, StringRef rhs) {
  SmallVector<std::string, 16> lhsToks = tokenSpellingsForText(lhs);
  SmallVector<std::string, 16> rhsToks = tokenSpellingsForText(rhs);
  if (lhsToks.size() != rhsToks.size())
    return false;
  for (size_t i = 0; i < lhsToks.size(); ++i)
    if (lhsToks[i] != rhsToks[i])
      return false;
  return true;
};
enum class ReplayKind { Literal, Param, Stringify, Paste };
struct PastePiece {
  bool isParam = false;
  uint32_t paramIdx = 0;
  std::string literal;
};
struct ReplayElem {
  ReplayKind kind = ReplayKind::Literal;
  std::string literal;
  uint32_t paramIdx = 0;
  std::vector<PastePiece> pastePieces;
};

auto pastePieceFromReplacementToken =
    [&](const RefoldModel::MacroReplacementToken &tok,
        PastePiece &piece) -> bool {
  if (tok.kind == RefoldModel::MacroReplacementTokenKind::ParamRef) {
    if (!tok.paramIndex || *tok.paramIndex >= oldActuals.size())
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
};

std::function<bool(size_t, size_t, std::vector<ReplayElem> &)>
    parseReplayRange;
parseReplayRange = [&](size_t begin, size_t end,
                       std::vector<ReplayElem> &out) {
  const auto &tokens = generatedCalleeCtx.currentDefinition->replacementTokens;
  for (size_t i = begin; i < end;) {
    const auto &tok = tokens[i];
    if (tok.spelling == "#") {
      generatedCalleeCtx.usesStringification = true;
      if (i + 1 >= end ||
          tokens[i + 1].kind !=
              RefoldModel::MacroReplacementTokenKind::ParamRef ||
          !tokens[i + 1].paramIndex)
        return false;
      const uint32_t paramIdx = *tokens[i + 1].paramIndex;
      if (paramIdx >= oldActuals.size())
        return false;
      ReplayElem elem;
      elem.kind = ReplayKind::Stringify;
      elem.paramIdx = paramIdx;
      out.push_back(std::move(elem));
      i += 2;
      continue;
    }

    if (i + 1 < end && tokens[i + 1].spelling == "##") {
      generatedCalleeCtx.usesPaste = true;
      ReplayElem elem;
      elem.kind = ReplayKind::Paste;
      PastePiece first;
      if (!pastePieceFromReplacementToken(tok, first))
        return false;
      elem.pastePieces.push_back(std::move(first));
      i += 2;
      while (true) {
        if (i >= end)
          return false;
        PastePiece next;
        if (!pastePieceFromReplacementToken(tokens[i], next))
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

    if (tok.kind == RefoldModel::MacroReplacementTokenKind::ParamRef) {
      if (!tok.paramIndex || *tok.paramIndex >= oldActuals.size())
        return false;
      ReplayElem elem;
      elem.kind = ReplayKind::Param;
      elem.paramIdx = *tok.paramIndex;
      out.push_back(std::move(elem));
      ++i;
      continue;
    }
    if (tok.spelling == "##" || tok.spelling == "__VA_OPT__")
      return false;
    ReplayElem elem;
    elem.kind = ReplayKind::Literal;
    elem.literal = tok.spelling.str();
    out.push_back(std::move(elem));
    ++i;
  }
  return true;
};

std::vector<ReplayElem> finalPattern;
if (!parseReplayRange(0, generatedCalleeCtx.currentDefinition->replacementTokens.size(),
                      finalPattern) ||
    finalPattern.empty())
  return std::nullopt;

std::vector<ReplayElem> replayPattern;
for (const std::string &literal : generatedCalleeCtx.replayPrefixLiterals) {
  ReplayElem elem;
  elem.kind = ReplayKind::Literal;
  elem.literal = literal;
  replayPattern.push_back(std::move(elem));
}
replayPattern.insert(replayPattern.end(), finalPattern.begin(),
                     finalPattern.end());
for (auto it = generatedCalleeCtx.replaySuffixStack.rbegin();
     it != generatedCalleeCtx.replaySuffixStack.rend(); ++it) {
  for (const std::string &literal : *it) {
    ReplayElem elem;
    elem.kind = ReplayKind::Literal;
    elem.literal = literal;
    replayPattern.push_back(std::move(elem));
  }
}

using SolvedActuals = SmallVector<std::string, 8>;
auto assignSolvedActual = [&](SolvedActuals &actuals, uint32_t paramIdx,
                              StringRef value) -> bool {
  if (paramIdx >= actuals.size())
    return false;
  if (!textsTokenEquivalent(actuals[paramIdx], oldActuals[paramIdx]) &&
      !textsTokenEquivalent(actuals[paramIdx], value))
    return false;
  if (textsTokenEquivalent(actuals[paramIdx], oldActuals[paramIdx])) {
    actuals[paramIdx] = value.trim().str();
    return true;
  }
  return textsTokenEquivalent(actuals[paramIdx], value);
};

auto solvePasteToken =
    [&](ArrayRef<PastePiece> pieces, StringRef spelling,
        const SolvedActuals &seed) -> SmallVector<SolvedActuals, 4> {
  SmallVector<SolvedActuals, 4> solutions;
  const bool hasLiteralAnchor =
      llvm::any_of(pieces, [](const PastePiece &piece) {
        return !piece.isParam && !piece.literal.empty();
      });
  if (!hasLiteralAnchor) {
    SolvedActuals cur = seed;
    size_t cursor = 0;
    for (const PastePiece &piece : pieces) {
      if (!piece.isParam) {
        if (!spelling.substr(cursor).starts_with(piece.literal))
          return solutions;
        cursor += piece.literal.size();
        continue;
      }
      if (piece.paramIdx >= oldActuals.size())
        return solutions;
      const size_t width =
          StringRef(oldActuals[piece.paramIdx]).trim().size();
      if (cursor + width > spelling.size())
        return solutions;
      if (!assignSolvedActual(cur, piece.paramIdx,
                              spelling.slice(cursor, cursor + width))) {
        return solutions;
      }
      cursor += width;
    }
    if (cursor == spelling.size())
      solutions.push_back(std::move(cur));
    return solutions;
  }

  std::function<void(size_t, size_t, SolvedActuals &)> dfsPaste;
  dfsPaste = [&](size_t pieceIdx, size_t cursor, SolvedActuals &cur) {
    if (solutions.size() > 1)
      return;
    if (pieceIdx == pieces.size()) {
      if (cursor == spelling.size())
        solutions.push_back(cur);
      return;
    }
    const PastePiece &piece = pieces[pieceIdx];
    if (!piece.isParam) {
      if (spelling.substr(cursor).starts_with(piece.literal))
        dfsPaste(pieceIdx + 1, cursor + piece.literal.size(), cur);
      return;
    }
    for (size_t end = cursor; end <= spelling.size(); ++end) {
      SolvedActuals next = cur;
      if (!assignSolvedActual(next, piece.paramIdx,
                              spelling.slice(cursor, end))) {
        continue;
      }
      dfsPaste(pieceIdx + 1, end, next);
      if (solutions.size() > 1)
        return;
    }
  };
  SolvedActuals start = seed;
  dfsPaste(0, 0, start);
  return solutions;
};

auto solveExpansion =
    [&](StringRef expansion) -> std::optional<SolvedActuals> {
  SmallVector<ReplayTok, 32> toks;
  lexReplayTokens(expansion, toks);
  SmallVector<SolvedActuals, 4> solutions;
  SolvedActuals seed;
  for (const std::string &actual : oldActuals)
    seed.push_back(actual);

  std::function<void(ArrayRef<ReplayElem>, size_t, SolvedActuals &)> dfs;
  dfs = [&](ArrayRef<ReplayElem> elems, size_t tokPos, SolvedActuals &cur) {
    if (solutions.size() > 1)
      return;
    if (elems.empty()) {
      if (tokPos == toks.size())
        solutions.push_back(cur);
      return;
    }
    const ReplayElem &elem = elems.front();
    ArrayRef<ReplayElem> rest = elems.drop_front();
    switch (elem.kind) {
    case ReplayKind::Literal:
      if (tokPos < toks.size() && toks[tokPos].spelling == elem.literal)
        dfs(rest, tokPos + 1, cur);
      return;
    case ReplayKind::Param: {
      for (size_t end = tokPos; end <= toks.size(); ++end) {
        StringRef value;
        if (end > tokPos) {
          const size_t byteBegin = toks[tokPos].begin;
          const size_t byteEnd = toks[end - 1].end;
          value = expansion.slice(byteBegin, byteEnd);
        }
        SolvedActuals next = cur;
        if (!assignSolvedActual(next, elem.paramIdx, value))
          continue;
        dfs(rest, end, next);
        if (solutions.size() > 1)
          return;
      }
      return;
    }
    case ReplayKind::Stringify: {
      if (tokPos >= toks.size())
        return;
      std::optional<std::string> content =
          decodeSimpleStringLiteralToken(toks[tokPos].spelling);
      if (!content)
        return;
      SolvedActuals next = cur;
      if (!assignSolvedActual(next, elem.paramIdx, StringRef(*content)))
        return;
      dfs(rest, tokPos + 1, next);
      return;
    }
    case ReplayKind::Paste: {
      if (tokPos >= toks.size())
        return;
      SmallVector<SolvedActuals, 4> pasteSolutions =
          solvePasteToken(elem.pastePieces, toks[tokPos].spelling, cur);
      for (SolvedActuals &pasteSol : pasteSolutions) {
        dfs(rest, tokPos + 1, pasteSol);
        if (solutions.size() > 1)
          return;
      }
      return;
    }
    }
  };

  dfs(replayPattern, 0, seed);
  if (solutions.size() != 1)
    return std::nullopt;
  return solutions.front();
};

StringRef oldExpansion = (*deps_.sourceMapper).SliceASource(cover->first, cover->second).trim();
StringRef newExpansion = (*deps_.sourceMapper).SliceBSource(bEnv->first, bEnv->second).trim();
std::optional<SolvedActuals> oldSolved = solveExpansion(oldExpansion);
std::optional<SolvedActuals> newSolved = solveExpansion(newExpansion);
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
  refoldLexBoundaryTokens(source, (*deps_.lexLang), sourceToks);
  refoldLexBoundaryTokens(oldText, (*deps_.lexLang), oldToks);
  refoldLexBoundaryTokens(newText, (*deps_.lexLang), newToks);
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
    return stringutils::replaceRange(source.str(), pos,
                                     pos + oldText.size(), newText);
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
            source.str(), middlePos, middlePos + oldMiddle.size(),
            newMiddle);
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
  if (textsTokenEquivalent(StringRef((*oldSolved)[i]),
                           StringRef((*newSolved)[i])))
    continue;

  if (!isMacroInvocationVariadicFormal(m, rootIdx) &&
      StringRef((*newSolved)[i]).trim().empty())
    return std::nullopt;

  StringRef originalRoot =
      baseInvText.slice(invArgRanges[rootIdx].first,
                        invArgRanges[rootIdx].second).trim();
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
        originalRoot, StringRef((*oldSolved)[i]),
        StringRef((*newSolved)[i]));
    if (alreadySatisfied &&
        textsTokenEquivalent(StringRef(*alreadySatisfied), source))
      continue;
  }

  auto rewritten = rewriteSourceActualFromSolvedExpansion(
      source, StringRef((*oldSolved)[i]), StringRef((*newSolved)[i]));
  if (!rewritten)
    return std::nullopt;
  if (!isMacroInvocationVariadicFormal(m, rootIdx) && replacementIntroducesTopLevelComma(*rewritten, (*deps_.lexLang)))
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
    BuildInvocationRewriteWithRange(actualRecoveryCtx, replByRootArgIdx);
if (!rewrite)
  return std::nullopt;

MacroPatch patch{*m.invB, *m.invE, std::move(rewrite->text), m.id};
StampInvocationRewriteMaterializedOutputRange(patch, *rewrite);
stampMacroPatchMaterializedBTokenRange(patch,
                                       static_cast<uint64_t>(bEnv->first),
                                       static_cast<uint64_t>(bEnv->second));
SetArgsOnlyStandardProof(patch, m, /*wholeEnvelopeReplayValidated=*/true);
StampGeneratedCalleeReplayProof(
    patch, m, generatedCalleeCtx.currentDefinition ? generatedCalleeCtx.currentDefinition->id : 0,
    generatedCalleeCtx.generatedCallDepth,
    generatedCalleeCtx.objectAliasHopCount,
    generatedCalleeCtx.usesStringification, generatedCalleeCtx.usesPaste,
    generatedCalleeCtx.usesVariadicForwarding,
    /*decodedStringLiteralEvidenceOnly=*/
    generatedCalleeCtx.usesStringification);
return patch;

}

std::optional<MacroPatch>
RefoldMacroPatchPlanner::BuildGeneratedLeafReplayCandidate(
    const GeneratedLeafReplayContext &generatedLeafCtx) const {
  const RefoldModel::MacroInvocation &m = generatedLeafCtx.invocation;
  StringRef baseInvText = generatedLeafCtx.baseInvocationText;
  ArrayRef<std::pair<size_t, size_t>> invArgRanges =
      generatedLeafCtx.invocationArgRanges;
  const std::pair<size_t, size_t> *bEnv =
      &generatedLeafCtx.bTokenEnvelope;
  const RefoldModel::MacroDirective *rootDefinition =
      &generatedLeafCtx.rootDefinition;
  InvocationActualRecoveryContext actualRecoveryCtx{m, baseInvText,
                                                     invArgRanges};

// Use the file-scope leaf-token carrier so leaf token-equivalence helpers
// can be lifted without depending on a local token type.
auto lexLeafTokens = [&](StringRef text, SmallVectorImpl<LeafTok> &out) {
  out.clear();
  SmallVector<RefoldLexBoundaryToken, 32> toks;
  refoldLexBoundaryTokens(text, (*deps_.lexLang), toks);
  for (const RefoldLexBoundaryToken &tok : toks)
    out.push_back(LeafTok{tok.spelling});
};
auto leafValueForToken = [&](StringRef spelling) -> std::string {
  if (std::optional<std::string> decoded =
          decodeSimpleStringLiteralToken(spelling))
    return *decoded;
  return spelling.str();
};

auto leafTextsTokenEquivalent = [&](StringRef lhs, StringRef rhs) {
  SmallVector<LeafTok, 16> lhsToks;
  SmallVector<LeafTok, 16> rhsToks;
  lexLeafTokens(lhs, lhsToks);
  lexLeafTokens(rhs, rhsToks);
  if (lhsToks.size() != rhsToks.size())
    return false;
  for (size_t i = 0; i < lhsToks.size(); ++i)
    if (lhsToks[i].spelling != rhsToks[i].spelling)
      return false;
  return true;
};

SmallVector<LeafTok, 32> oldToks;
SmallVector<LeafTok, 32> newToks;
lexLeafTokens(generatedLeafCtx.oldExpansion, oldToks);
lexLeafTokens(generatedLeafCtx.newExpansion, newToks);
if (oldToks.empty() || newToks.empty())
  return std::nullopt;

std::optional<std::pair<std::string, std::string>> leafRewrite;
bool scalarLeafRewriteConflict = false;
if (oldToks.size() == newToks.size()) {
  for (size_t i = 0; i < oldToks.size(); ++i) {
    if (oldToks[i].spelling == newToks[i].spelling)
      continue;
    std::optional<std::pair<std::string, std::string>> changed =
        changedMiddleSubstrings(
            StringRef(leafValueForToken(oldToks[i].spelling)),
            StringRef(leafValueForToken(newToks[i].spelling)));
    if (!changed) {
      scalarLeafRewriteConflict = true;
      leafRewrite.reset();
      break;
    }
    if (leafRewrite) {
      if (leafRewrite->first != changed->first ||
          leafRewrite->second != changed->second) {
        scalarLeafRewriteConflict = true;
        leafRewrite.reset();
        break;
      }
      continue;
    }
    leafRewrite = std::move(changed);
  }

  if (leafRewrite && !scalarLeafRewriteConflict) {
    // A generated-leaf rewrite edits one root invocation argument.  Such an
    // edit is admissible only if the whole owner expansion is consistent
    // with that single source change.  If the same solved old leaf remains
    // unchanged somewhere else in the owner cover, then rewriting the root
    // argument would also rewrite that occurrence.  Rejecting here prevents
    // cases such as REGISTER_COMMAND(open), where paste/stringify/call uses
    // become OPEN but one raw `open` occurrence intentionally stays open.
    for (size_t i = 0; i < oldToks.size(); ++i) {
      if (oldToks[i].spelling != newToks[i].spelling)
        continue;
      std::string stableValue = leafValueForToken(oldToks[i].spelling);
      if (StringRef(stableValue) == StringRef(leafRewrite->first))
        return std::nullopt;
    }
  }
}


auto tryGeneratedSelectorActualRewrite =
    [&]() -> std::optional<MacroPatch> {
  if (oldToks.empty() || newToks.empty())
    return std::nullopt;

  auto appendLexedArgumentTokens = [&](StringRef text,
                                       SmallVectorImpl<std::string> &out) {
    SmallVector<RefoldLexBoundaryToken, 16> toks;
    refoldLexBoundaryTokens(text, (*deps_.lexLang), toks);
    for (const RefoldLexBoundaryToken &tok : toks)
      out.push_back(tok.spelling);
  };

  auto stringifyArgumentForReplay = [&](StringRef text) -> std::string {
    SmallVector<RefoldLexBoundaryToken, 16> toks;
    refoldLexBoundaryTokens(text, (*deps_.lexLang), toks);
    std::string body;
    for (const RefoldLexBoundaryToken &tok : toks) {
      if (!body.empty())
        body.push_back(' ');
      body += tok.spelling;
    }
    std::string out = "\"";
    for (char c : body) {
      if (c == '\\' || c == '"')
        out.push_back('\\');
      out.push_back(c);
    }
    out.push_back('"');
    return out;
  };

  auto replayFunctionLikeToSpellings =
      [&](const RefoldModel::MacroDirective &definition,
          ArrayRef<StringRef> actuals)
      -> std::optional<SmallVector<std::string, 16>> {
    if (definition.defParams.size() != actuals.size())
      return std::nullopt;
    SmallVector<std::string, 16> out;
    const auto &toks = definition.replacementTokens;
    for (size_t i = 0; i < toks.size();) {
      const auto &tok = toks[i];
      if (tok.spelling == "#") {
        if (i + 1 >= toks.size() ||
            toks[i + 1].kind !=
                RefoldModel::MacroReplacementTokenKind::ParamRef ||
            !toks[i + 1].paramIndex ||
            *toks[i + 1].paramIndex >= actuals.size())
          return std::nullopt;
        out.push_back(
            stringifyArgumentForReplay(actuals[*toks[i + 1].paramIndex]));
        i += 2;
        continue;
      }
      if (i + 1 < toks.size() && toks[i + 1].spelling == "##") {
        std::string pasted;
        auto appendPastePiece =
            [&](const RefoldModel::MacroReplacementToken &piece) {
              if (piece.kind ==
                  RefoldModel::MacroReplacementTokenKind::ParamRef) {
                if (!piece.paramIndex ||
                    *piece.paramIndex >= actuals.size())
                  return false;
                pasted += actuals[*piece.paramIndex].trim().str();
                return true;
              }
              if (piece.spelling == "#" || piece.spelling == "##" ||
                  piece.spelling == "__VA_OPT__")
                return false;
              pasted += piece.spelling.str();
              return true;
            };
        if (!appendPastePiece(tok))
          return std::nullopt;
        i += 2;
        while (true) {
          if (i >= toks.size() || !appendPastePiece(toks[i]))
            return std::nullopt;
          ++i;
          if (i >= toks.size() || toks[i].spelling != "##")
            break;
          ++i;
        }
        out.push_back(std::move(pasted));
        continue;
      }
      if (tok.kind == RefoldModel::MacroReplacementTokenKind::ParamRef) {
        if (!tok.paramIndex || *tok.paramIndex >= actuals.size())
          return std::nullopt;
        appendLexedArgumentTokens(actuals[*tok.paramIndex], out);
        ++i;
        continue;
      }
      if (tok.spelling == "##" || tok.spelling == "__VA_OPT__")
        return std::nullopt;
      out.push_back(tok.spelling.str());
      ++i;
    }
    return out;
  };


  struct SelectorCandidate {
    std::string replacement;
    uint32_t rootArgIdx = 0;
    size_t newSelectorSize = 0;
  };
  SmallVector<SelectorCandidate, 4> candidates;

  for (uint32_t selectorIdx = 0; selectorIdx < invArgRanges.size();
       ++selectorIdx) {
    auto selectorRange = invArgRanges[selectorIdx];
    if (selectorRange.second < selectorRange.first ||
        selectorRange.second > baseInvText.size())
      return std::nullopt;
    StringRef selectorText =
        baseInvText.slice(selectorRange.first, selectorRange.second).trim();
    const RefoldModel::MacroDirective *oldSelectorDef =
        ResolveFunctionLikeMacroForReplay(selectorText);
    if (!oldSelectorDef || oldSelectorDef->defParams.empty())
      continue;
    if (selectorIdx + oldSelectorDef->defParams.size() >=
        invArgRanges.size() + 1)
      continue;

    SmallVector<StringRef, 8> actuals;
    for (size_t i = 0; i < oldSelectorDef->defParams.size(); ++i) {
      const auto r = invArgRanges[selectorIdx + 1 + i];
      if (r.second < r.first || r.second > baseInvText.size())
        return std::nullopt;
      actuals.push_back(baseInvText.slice(r.first, r.second).trim());
    }
    std::optional<SmallVector<std::string, 16>> oldReplay =
        replayFunctionLikeToSpellings(*oldSelectorDef, actuals);
    if (!oldReplay || !leafTokenSpellingsEqual(oldToks, *oldReplay))
      continue;

    const bool oldSelectorWasAlias =
        IsObjectLikeSingleTokenAlias(selectorText);
    for (const RefoldModel::MacroDirective &sourceDirective :
         (*deps_.model).GetMacroDirectives()) {
      if (sourceDirective.subkind != "#define" ||
          sourceDirective.name.empty() ||
          sourceDirective.name == selectorText)
        continue;

      const bool candidateIsAlias =
          !sourceDirective.functionLike &&
          sourceDirective.replacementTokens.size() == 1 &&
          sourceDirective.replacementTokens[0].kind ==
              RefoldModel::MacroReplacementTokenKind::Literal;
      if (oldSelectorWasAlias != candidateIsAlias)
        continue;
      if (!oldSelectorWasAlias && !sourceDirective.functionLike)
        continue;

      const RefoldModel::MacroDirective *candidateDef =
          ResolveFunctionLikeMacroForReplay(sourceDirective.name);
      if (!candidateDef || candidateDef == oldSelectorDef ||
          candidateDef->defParams.size() != actuals.size())
        continue;
      std::optional<SmallVector<std::string, 16>> candidateReplay =
          replayFunctionLikeToSpellings(*candidateDef, actuals);
      if (!candidateReplay ||
          !leafTokenSpellingsEqual(newToks, *candidateReplay))
        continue;

      std::string replacement = baseInvText.str();
      replacement.replace(selectorRange.first,
                          selectorRange.second - selectorRange.first,
                          sourceDirective.name.str());
      candidates.push_back(SelectorCandidate{std::move(replacement),
                                             selectorIdx,
                                             sourceDirective.name.size()});
    }
  }

  if (candidates.empty())
    return std::nullopt;
  llvm::sort(candidates, [](const SelectorCandidate &lhs,
                            const SelectorCandidate &rhs) {
    return lhs.replacement < rhs.replacement;
  });
  for (const SelectorCandidate &candidate : candidates)
    if (candidate.replacement != candidates.front().replacement)
      return std::nullopt;

  MacroPatch patch{*m.invB, *m.invE, candidates.front().replacement, m.id};
  patch.hasMaterializedOutputByteRange = true;
  patch.materializedOutputByteStart =
      invArgRanges[candidates.front().rootArgIdx].first;
  patch.materializedOutputByteEnd = patch.materializedOutputByteStart +
                                    candidates.front().newSelectorSize;
  stampMacroPatchMaterializedBTokenRange(
      patch, static_cast<uint64_t>(generatedLeafCtx.bTokenEnvelope.first),
      static_cast<uint64_t>(generatedLeafCtx.bTokenEnvelope.second));
  SetArgsOnlyStandardProof(patch, m, /*wholeEnvelopeReplayValidated=*/true);
  return patch;
};

if (auto selectorPatch = tryGeneratedSelectorActualRewrite())
  return selectorPatch;

auto trailingVariadicIsStringifiedByAnyRootCallee = [&]() {
  for (uint32_t argIdx = 0; argIdx < invArgRanges.size(); ++argIdx) {
    auto r = invArgRanges[argIdx];
    if (r.second < r.first || r.second > baseInvText.size())
      return false;
    const RefoldModel::MacroDirective *callee =
        ResolveFunctionLikeMacroForReplay(
            baseInvText.slice(r.first, r.second).trim());
    if (!callee || callee->defParams.empty() ||
        !callee->defParams.back().variadic)
      continue;
    const uint32_t variadicIdx =
        static_cast<uint32_t>(callee->defParams.size() - 1);
    for (size_t i = 0; i + 1 < callee->replacementTokens.size(); ++i) {
      if (callee->replacementTokens[i].spelling == "#" &&
          callee->replacementTokens[i + 1].kind ==
              RefoldModel::MacroReplacementTokenKind::ParamRef &&
          callee->replacementTokens[i + 1].paramIndex &&
          *callee->replacementTokens[i + 1].paramIndex == variadicIdx)
        return true;
    }
  }
  return false;
};

// `__VA_OPT__` activation can add a new variadic root argument even though
// no old root argument text exists to replace.  Accept only the canonical
// case where the old expansion is a prefix of the new expansion and the
// inserted tail contains exactly one data token after fixed punctuation;
// the new token is inserted as the missing variadic actual.
const bool rootHasTrailingVariadic =
    !rootDefinition->defParams.empty() &&
    rootDefinition->defParams.back().variadic;
const bool omittedVariadicAbsent =
    rootHasTrailingVariadic &&
    invArgRanges.size() + 1 == rootDefinition->defParams.size();
const bool omittedVariadicEmptyFormal =
    rootHasTrailingVariadic &&
    invArgRanges.size() == rootDefinition->defParams.size() &&
    !invArgRanges.empty() &&
    invArgRanges.back().first == invArgRanges.back().second;

if (!leafRewrite && oldToks.size() < newToks.size() &&
    (omittedVariadicAbsent || omittedVariadicEmptyFormal)) {
  bool oldPrefixMatches = true;
  for (size_t i = 0; i < oldToks.size(); ++i) {
    if (oldToks[i].spelling != newToks[i].spelling) {
      oldPrefixMatches = false;
      break;
    }
  }
  if (oldPrefixMatches) {
    std::string insertedActual;
    bool sawInsertedData = false;
    const bool stringifiedTail =
        trailingVariadicIsStringifiedByAnyRootCallee();
    for (size_t i = oldToks.size(); i < newToks.size(); ++i) {
      StringRef spelling(newToks[i].spelling);
      const std::string decoded = leafValueForToken(spelling);
      // Leading `__VA_OPT__` separators are replacement-list context, not
      // source text to insert.  Once actual data has started, commas are
      // ordinary variadic-pack separators and must be preserved so multi-
      // actual activations become `, 2, 3` at the root callsite rather than
      // a single collapsed token.
      if (!sawInsertedData &&
          (spelling == ":" || spelling == "," ||
           StringRef(decoded) == ":" || StringRef(decoded) == ","))
        continue;
      if (stringifiedTail) {
        if (sawInsertedData)
          return std::nullopt;
        std::optional<std::string> decodedInserted =
            decodeSimpleStringLiteralToken(spelling);
        if (!decodedInserted)
          return std::nullopt;
        insertedActual = std::move(*decodedInserted);
        sawInsertedData = true;
        continue;
      }
      if (spelling == "," || StringRef(decoded) == ",") {
        if (insertedActual.empty() || insertedActual.back() == ' ')
          insertedActual += ",";
        else
          insertedActual += ",";
        insertedActual += " ";
      } else {
        if (!insertedActual.empty() && insertedActual.back() != ' ' &&
            insertedActual.back() != ',')
          insertedActual += " ";
        insertedActual += spelling.str();
      }
      sawInsertedData = true;
    }
    if (sawInsertedData && !StringRef(insertedActual).trim().empty()) {
      size_t close = baseInvText.rfind(')');
      if (close == StringRef::npos)
        return std::nullopt;
      std::string rewritten = baseInvText.slice(0, close).str();
      rewritten += ", ";
      rewritten += StringRef(insertedActual).trim();
      rewritten += baseInvText.substr(close).str();

      MacroPatch patch{*m.invB, *m.invE, std::move(rewritten), m.id};
      patch.hasMaterializedOutputByteRange = true;
      patch.materializedOutputByteStart = close;
      patch.materializedOutputByteEnd =
          close + 2 + StringRef(insertedActual).trim().size();
      stampMacroPatchMaterializedBTokenRange(
          patch, static_cast<uint64_t>(bEnv->first),
          static_cast<uint64_t>(bEnv->second));
      SetArgsOnlyStandardProof(patch, m,
                               /*wholeEnvelopeReplayValidated=*/true);
      return patch;
    }
  }
}

auto removeTrailingVariadicActual = [&]() -> std::optional<MacroPatch> {
  if (!rootHasTrailingVariadic || invArgRanges.size() < 2)
    return std::nullopt;
  const uint32_t lastIdx = static_cast<uint32_t>(invArgRanges.size() - 1);
  const auto prev = invArgRanges[lastIdx - 1];
  const auto last = invArgRanges[lastIdx];
  if (last.second < last.first || last.second > baseInvText.size() ||
      last.first == last.second)
    return std::nullopt;

  // Deactivating a trailing `__VA_OPT__` tail removes the variadic root
  // actual, not the callee selector or the fixed argument before it.  The
  // formal range list gives us the exact separator-to-end span for the last
  // argument: start at the previous argument's end so the separating comma
  // and whitespace disappear together with the old variadic payload.
  std::string rewritten = baseInvText.slice(0, prev.second).str();
  rewritten += baseInvText.substr(last.second).str();
  MacroPatch patch{*m.invB, *m.invE, std::move(rewritten), m.id};
  patch.hasMaterializedOutputByteRange = true;
  patch.materializedOutputByteStart = prev.second;
  patch.materializedOutputByteEnd = prev.second;
  stampMacroPatchMaterializedBTokenRange(
      patch, static_cast<uint64_t>(generatedLeafCtx.bTokenEnvelope.first),
      static_cast<uint64_t>(generatedLeafCtx.bTokenEnvelope.second));
  SetArgsOnlyStandardProof(patch, m, /*wholeEnvelopeReplayValidated=*/true);
  return patch;
};

if (!leafRewrite && oldToks.size() > newToks.size() &&
    rootHasTrailingVariadic) {
  bool newPrefixMatches = true;
  for (size_t i = 0; i < newToks.size(); ++i) {
    if (oldToks[i].spelling != newToks[i].spelling) {
      newPrefixMatches = false;
      break;
    }
  }
  if (newPrefixMatches) {
    bool onlyVaOptTailRemoved = true;
    for (size_t i = newToks.size(); i < oldToks.size(); ++i) {
      StringRef spelling(oldToks[i].spelling);
      const std::string decoded = leafValueForToken(spelling);
      if (spelling == ":" || spelling == "," || StringRef(decoded) == ":" ||
          StringRef(decoded) == ",")
        continue;
      // The remaining removed token is the old variadic data token.  There
      // must be exactly one such token for this narrow deactivation proof;
      // richer pack edits belong in the full generated-argument DAG proof.
      if (i + 1 != oldToks.size())
        onlyVaOptTailRemoved = false;
    }
    if (onlyVaOptTailRemoved) {
      if (auto patch = removeTrailingVariadicActual())
        return patch;
    }
  }
}

auto tryMultiLeafPatternRewrite = [&]() -> std::optional<MacroPatch> {
  if (oldToks.size() != newToks.size())
    return std::nullopt;

  DenseMap<uint32_t, std::string> replByArgIdx;
  for (size_t tokIdx = 0; tokIdx < oldToks.size(); ++tokIdx) {
    if (oldToks[tokIdx].spelling == newToks[tokIdx].spelling)
      continue;

    std::string oldValueStorage =
        leafValueForToken(oldToks[tokIdx].spelling);
    std::string newValueStorage =
        leafValueForToken(newToks[tokIdx].spelling);
    StringRef oldValue(oldValueStorage);
    StringRef newValue(newValueStorage);
    struct ArgOccurrence {
      uint32_t argIdx = 0;
      size_t begin = 0;
      size_t end = 0;
      std::string oldText;

      // The multi-leaf matcher can compare in two different domains:
      // ordinary source spelling, or the decoded payload of a source string
      // literal.  The latter is probe evidence only.  If it produces a new
      // source actual, the solved payload must be re-encoded as a string
      // literal instead of emitted as a raw identifier/token sequence.
      bool matchedDecodedSourceStringLiteral = false;
      std::string sourceText;
    };
    SmallVector<ArgOccurrence, 8> occs;
    std::optional<uint32_t> emptyArgIdx;
    for (uint32_t argIdx = 0; argIdx < invArgRanges.size(); ++argIdx) {
      auto r = invArgRanges[argIdx];
      if (r.second < r.first || r.second > baseInvText.size())
        return std::nullopt;
      StringRef argText = baseInvText.slice(r.first, r.second).trim();
      if (ResolveFunctionLikeMacroForReplay(argText))
        continue;
      if (argText.empty()) {
        if (emptyArgIdx)
          return std::nullopt;
        emptyArgIdx = argIdx;
        continue;
      }
      struct Probe {
        StringRef text;
        bool decodedSourceStringLiteral = false;
      };
      SmallVector<Probe, 2> probes;
      probes.push_back(Probe{argText, false});

      // Keep the decoded spelling alive for the full probe loop.  More
      // importantly, tag it as decoded evidence so a later source rewrite
      // re-quotes the solved payload.  Decoding makes `"beta"` comparable
      // to the generated leaf value `beta`, but it does not prove that the
      // source actual may be rewritten to the raw identifier `gamma`.
      std::optional<std::string> decodedArgStorage =
          decodeSimpleStringLiteralToken(argText);
      if (decodedArgStorage)
        probes.push_back(Probe{StringRef(*decodedArgStorage), true});

      for (Probe probe : probes) {
        if (probe.text.empty())
          continue;
        size_t pos = oldValue.find(probe.text);
        if (pos == StringRef::npos)
          continue;
        if (oldValue.find(probe.text, pos + probe.text.size()) !=
            StringRef::npos)
          return std::nullopt;
        ArgOccurrence occ;
        occ.argIdx = argIdx;
        occ.begin = pos;
        occ.end = pos + probe.text.size();
        occ.oldText = probe.text.str();
        occ.matchedDecodedSourceStringLiteral =
            probe.decodedSourceStringLiteral;
        occ.sourceText = argText.str();
        occs.push_back(std::move(occ));
        break;
      }
    }
    if (emptyArgIdx) {
      // Empty actuals are real macro arguments even though they have no
      // spelling to find in the old expansion.  Model the single empty leaf
      // as a zero-width occurrence at the end of the generated value; the
      // skeleton matcher below then proves whether B supplies a unique
      // suffix for that empty source slot or erases an old non-empty slot.
      ArgOccurrence occ;
      occ.argIdx = *emptyArgIdx;
      occ.begin = oldValue.size();
      occ.end = oldValue.size();
      occ.oldText = "";
      occs.push_back(std::move(occ));
    }
    if (occs.empty())
      return std::nullopt;
    llvm::sort(occs,
               [](const ArgOccurrence &lhs, const ArgOccurrence &rhs) {
                 if (lhs.begin != rhs.begin)
                   return lhs.begin < rhs.begin;
                 return lhs.argIdx < rhs.argIdx;
               });
    for (size_t i = 1; i < occs.size(); ++i)
      if (occs[i].begin < occs[i - 1].end)
        return std::nullopt;

    auto rewriteSolvedLeafAsSource =
        [&](const ArgOccurrence &occ,
            StringRef solved) -> std::optional<std::string> {
      if (!occ.matchedDecodedSourceStringLiteral)
        return solved.str();

      StringRef source = StringRef(occ.sourceText).trim();
      const size_t quote = source.find('"');
      const size_t endQuote = source.rfind('"');
      if (quote == StringRef::npos || endQuote == StringRef::npos ||
          endQuote <= quote)
        return std::nullopt;

      // Preserve the source literal prefix (`L`, `u8`, etc.) and any suffix
      // spelling, but replace the decoded payload with a freshly quoted C
      // string literal.  This keeps decoded-payload matching from silently
      // changing an ordinary forwarded string literal into an identifier.
      std::string rewritten = source.slice(0, quote).str();
      rewritten += stringutils::quoteCStringLiteral(solved);
      rewritten += source.substr(endQuote + 1).str();
      return rewritten;
    };

    // Match the literal skeleton around the old root-argument leaves
    // against the new expansion value.  The leaves themselves may change
    // length, so only the fixed literal gaps are used as anchors.
    size_t newCursor = 0;
    for (size_t i = 0; i < occs.size(); ++i) {
      StringRef prefix =
          oldValue.slice(i == 0 ? 0 : occs[i - 1].end, occs[i].begin);
      if (!newValue.substr(newCursor).starts_with(prefix))
        return std::nullopt;
      newCursor += prefix.size();
      StringRef nextLiteral = oldValue.slice(
          occs[i].end,
          i + 1 < occs.size() ? occs[i + 1].begin : oldValue.size());
      size_t nextPos = StringRef::npos;
      if (nextLiteral.empty()) {
        if (i + 1 < occs.size() && occs[i + 1].begin == occs[i].end) {
          // Adjacent generated leaves come from a paste-like expression
          // with no literal separator (`A##B`, or `A##<empty>`).  With no
          // anchor between leaves, use the same deterministic contribution
          // rule as unanchored paste inversion: every leaf before the next
          // literal keeps its old contribution width, and the final leaf in
          // the run consumes the remaining segment.  This prevents greedy
          // scalar rewrites such as `alpha,beta -> gammabeta,<empty>` while
          // still failing closed if the old-width cut is impossible.
          nextPos = newCursor + occs[i].oldText.size();
          if (nextPos > newValue.size())
            return std::nullopt;
        } else {
          nextPos = newValue.size();
        }
      } else {
        nextPos = newValue.find(nextLiteral, newCursor);
        if (nextPos == StringRef::npos)
          return std::nullopt;
        if (newValue.find(nextLiteral, nextPos + 1) != StringRef::npos)
          return std::nullopt;
      }
      StringRef solved = newValue.slice(newCursor, nextPos).trim();
      std::optional<std::string> sourceSolved =
          rewriteSolvedLeafAsSource(occs[i], solved);
      if (!sourceSolved)
        return std::nullopt;
      StringRef sourceSolvedRef(*sourceSolved);
      auto existing = replByArgIdx.find(occs[i].argIdx);
      if (existing != replByArgIdx.end()) {
        if (!leafTextsTokenEquivalent(StringRef(existing->second),
                                      sourceSolvedRef))
          return std::nullopt;
      } else {
        if (!isMacroInvocationVariadicFormal(m, occs[i].argIdx) &&
            replacementIntroducesTopLevelComma(sourceSolvedRef, (*deps_.lexLang)))
          return std::nullopt;
        replByArgIdx[occs[i].argIdx] = std::move(*sourceSolved);
      }
      newCursor = nextPos;
    }

    // The loop above leaves `newCursor` at the start of the literal
    // suffix following the last editable leaf.  Intermediate separators are
    // consumed as the next leaf's prefix, but the final suffix has no next
    // iteration to consume it.  Consume that trailing literal context here
    // so decorated multi-leaf expressions such as
    // `pre_##A##_mid_##B##_suf` are matched as one skeleton rather than
    // rejected after solving the last source slot.
    if (!occs.empty()) {
      StringRef trailingLiteral = oldValue.drop_front(occs.back().end);
      if (!newValue.substr(newCursor).starts_with(trailingLiteral))
        return std::nullopt;
      newCursor += trailingLiteral.size();
    }
    if (newCursor != newValue.size())
      return std::nullopt;
  }

  if (replByArgIdx.empty())
    return std::nullopt;
  std::optional<InvocationRewriteWithRange> rewrite =
      BuildInvocationRewriteWithRange(actualRecoveryCtx, replByArgIdx);
  if (!rewrite)
    return std::nullopt;
  MacroPatch patch{*generatedLeafCtx.invocation.invB,
                   *generatedLeafCtx.invocation.invE,
                   std::move(rewrite->text),
                   generatedLeafCtx.invocation.id};
  StampInvocationRewriteMaterializedOutputRange(patch, *rewrite);
  stampMacroPatchMaterializedBTokenRange(
      patch, static_cast<uint64_t>(generatedLeafCtx.bTokenEnvelope.first),
      static_cast<uint64_t>(generatedLeafCtx.bTokenEnvelope.second));
  SetArgsOnlyStandardProof(patch, m, /*wholeEnvelopeReplayValidated=*/true);
  return patch;
};

if (auto patch = tryMultiLeafPatternRewrite())
  return patch;

if (!leafRewrite)
  return std::nullopt;

const std::string &oldLeaf = leafRewrite->first;
const std::string &newLeaf = leafRewrite->second;
std::optional<uint32_t> targetArgIdx;
std::optional<std::string> targetReplacement;
for (uint32_t argIdx = 0;
       argIdx < generatedLeafCtx.invocationArgRanges.size(); ++argIdx) {
  auto r = generatedLeafCtx.invocationArgRanges[argIdx];
  if (r.second < r.first || r.second > generatedLeafCtx.baseInvocationText.size())
    return std::nullopt;
  StringRef argText =
      generatedLeafCtx.baseInvocationText.slice(r.first, r.second);
  size_t pos = argText.find(oldLeaf);
  if (pos == StringRef::npos)
    continue;
  if (argText.find(oldLeaf, pos + oldLeaf.size()) != StringRef::npos)
    return std::nullopt;
  if (targetArgIdx)
    return std::nullopt;
  std::string rewrittenArg = stringutils::replaceRange(
      argText.str(), pos, pos + oldLeaf.size(), StringRef(newLeaf));
  if (!isMacroInvocationVariadicFormal(m, argIdx) && replacementIntroducesTopLevelComma(rewrittenArg, (*deps_.lexLang)))
    return std::nullopt;
  targetArgIdx = argIdx;
  targetReplacement = std::move(rewrittenArg);
}
if (!targetArgIdx || !targetReplacement)
  return std::nullopt;

// A generated-leaf rewrite is a source edit to the root invocation
// argument.  Therefore every current-level occurrence of that root formal
// that still observes the solved old leaf in B would also be rewritten by
// the proposed source change.  Reject that shape instead of preserving the
// parent callsite: the edited leaf must be materialized or proved by the
// DAG path that can account for all child/parent observations together.
auto stableRootOccurrenceStillObservesOldLeaf =
    [&](const RefoldModel::PPArgSpan &span, StringRef observerKind) {
      if (span.argIdx != *targetArgIdx || span.begin >= span.end)
        return false;

      StringRef aText = (*deps_.sourceMapper).SliceASource(span.begin, span.end).trim();
      if (aText.find(StringRef(oldLeaf)) == StringRef::npos)
        return false;

      std::optional<std::pair<size_t, size_t>> bEnv =
          (*deps_.sourceMapper).MapAToBTokenEnvelopeByPPArgSpan(span);
      if (!bEnv || bEnv->second < bEnv->first) {
        return true;
      }

      StringRef bText = (*deps_.sourceMapper).SliceBSource(bEnv->first, bEnv->second).trim();
      if (bText.find(StringRef(oldLeaf)) == StringRef::npos)
        return false;

      return true;
    };

for (const RefoldModel::PPArgSpan &span : generatedLeafCtx.invocation.argSpans) {
  if (stableRootOccurrenceStillObservesOldLeaf(span, "standard"))
    return std::nullopt;
}
for (const RefoldModel::PPArgSpan &span : generatedLeafCtx.invocation.stringifySpans) {
  if (stableRootOccurrenceStillObservesOldLeaf(span, "stringify"))
    return std::nullopt;
}

DenseMap<uint32_t, std::string> replByArgIdx;
replByArgIdx[*targetArgIdx] = *targetReplacement;
std::optional<InvocationRewriteWithRange> rewrite =
    BuildInvocationRewriteWithRange(actualRecoveryCtx, replByArgIdx);
if (!rewrite)
  return std::nullopt;

MacroPatch patch{*generatedLeafCtx.invocation.invB,
                   *generatedLeafCtx.invocation.invE,
                   std::move(rewrite->text),
                   generatedLeafCtx.invocation.id};
StampInvocationRewriteMaterializedOutputRange(patch, *rewrite);
stampMacroPatchMaterializedBTokenRange(patch,
                                       static_cast<uint64_t>(generatedLeafCtx.bTokenEnvelope.first),
                                       static_cast<uint64_t>(generatedLeafCtx.bTokenEnvelope.second));
SetArgsOnlyStandardProof(patch, m, /*wholeEnvelopeReplayValidated=*/true);
return patch;

}

std::optional<MacroPatch>
RefoldMacroPatchPlanner::BuildTupleGeneratedCalleeReplayCandidate(
    const TupleGeneratedCalleeReplayContext &tupleGeneratedCtx) const {
  const RefoldModel::MacroInvocation &m = tupleGeneratedCtx.invocation;
  StringRef baseInvText = tupleGeneratedCtx.baseInvocationText;
  const std::pair<uint64_t, uint64_t> *cover =
      &tupleGeneratedCtx.wholeCoverATokens;
  const std::pair<size_t, size_t> *bEnv =
      &tupleGeneratedCtx.bTokenEnvelope;

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
  if (!splitTopLevelTupleElementsWithLexer(tuplePayload, (*deps_.lexLang),
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

  const auto &forwarderToks = tupleGeneratedCtx.forwarderDefinition.replacementTokens;

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
      calleeForwarderParam >= tupleGeneratedCtx.forwarderDefinition.defParams.size() ||
      calleeForwarderParam >= tupleElems.size())
    return std::nullopt;

  SmallVector<std::string, 8> forwarderPrefixLiterals;
  SmallVector<std::string, 8> forwarderSuffixLiterals;
  auto collectForwarderLiteralContext =
      [&](size_t begin, size_t end, SmallVectorImpl<std::string> &out) {
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
        *tok.paramIndex >= tupleGeneratedCtx.forwarderDefinition.defParams.size())
      return std::nullopt;
    if (*tok.paramIndex == calleeForwarderParam)
      return std::nullopt;
    GeneratedArgRef ref;
    ref.forwarderParamIdx = *tok.paramIndex;
    ref.variadicPack =
        tupleGeneratedCtx.forwarderDefinition.defParams[*tok.paramIndex].variadic;
    generatedArgs.push_back(ref);
  }
  if (generatedArgs.empty())
    return std::nullopt;

  uint32_t calleeAliasHops = 0;
  const RefoldModel::MacroDirective *calleeDefinition =
      ResolveFunctionLikeMacroThroughAliasesWithHops(
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
  const size_t fixedCalleeActuals =
      calleeHasVariadic ? calleeDefinition->defParams.size() - 1
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

  StringRef oldExpansion = (*deps_.sourceMapper).SliceASource(cover->first, cover->second).trim();
  StringRef newExpansion = (*deps_.sourceMapper).SliceBSource(bEnv->first, bEnv->second).trim();

  // Replay the generated callee as a small replacement-list transducer over
  // tuple slots.  A generated callee contributes a sequence of literal
  // tokens, ordinary parameter projections, stringified projections, pasted-
  // token projections, and fixed forwarding-context tokens.  If that sequence
  // explains both the old whole-cover expansion and the new B-side expansion
  // uniquely, then the solved parameter values can be translated back to
  // positional tuple edits.
  auto lexReplayTokens = [&](StringRef text,
                             SmallVectorImpl<ReplayTok> &out) {
    out.clear();
    SmallVector<RefoldLexBoundaryToken, 32> toks;
    refoldLexBoundaryTokens(text, (*deps_.lexLang), toks);
    for (const RefoldLexBoundaryToken &tok : toks)
      out.push_back(ReplayTok{tok.spelling, tok.begin, tok.end});
  };

  auto tokenSpellingsForText = [&](StringRef text) {
    SmallVector<ReplayTok, 16> toks;
    lexReplayTokens(text, toks);
    SmallVector<std::string, 16> out;
    for (const ReplayTok &tok : toks)
      out.push_back(tok.spelling);
    return out;
  };

  auto textsTokenEquivalent = [&](StringRef lhs, StringRef rhs) {
    SmallVector<std::string, 16> lhsToks = tokenSpellingsForText(lhs);
    SmallVector<std::string, 16> rhsToks = tokenSpellingsForText(rhs);
    if (lhsToks.size() != rhsToks.size())
      return false;
    for (size_t i = 0; i < lhsToks.size(); ++i)
      if (lhsToks[i] != rhsToks[i])
        return false;
    return true;
  };

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
    refoldLexBoundaryTokens(source, (*deps_.lexLang), sourceToks);
    refoldLexBoundaryTokens(oldText, (*deps_.lexLang), oldToks);
    refoldLexBoundaryTokens(newText, (*deps_.lexLang), newToks);
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
  enum class CalleeReplayKind { Literal, Param, Stringify, Paste, VaOpt };
  struct CalleePastePiece {
    bool isParam = false;
    uint32_t paramIdx = 0;
    std::string literal;
  };
  struct CalleeReplayElem {
    CalleeReplayKind kind = CalleeReplayKind::Literal;
    std::string literal;
    uint32_t paramIdx = 0;
    std::vector<CalleeReplayElem> children;
    std::vector<CalleePastePiece> pastePieces;
  };

  auto pastePieceFromReplacementToken =
      [&](const RefoldModel::MacroReplacementToken &tok,
          CalleePastePiece &piece) -> bool {
    if (tok.kind == RefoldModel::MacroReplacementTokenKind::ParamRef) {
      if (!tok.paramIndex || *tok.paramIndex >= oldActuals.size())
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
  };

  std::function<bool(size_t, size_t, std::vector<CalleeReplayElem> &)>
      parseCalleeReplayRange;
  parseCalleeReplayRange = [&](size_t begin, size_t end,
                               std::vector<CalleeReplayElem> &out) {
    for (size_t i = begin; i < end;) {
      const auto &tok = calleeDefinition->replacementTokens[i];
      if (tok.spelling == "#") {
        if (i + 1 >= end ||
            calleeDefinition->replacementTokens[i + 1].kind !=
                RefoldModel::MacroReplacementTokenKind::ParamRef ||
            !calleeDefinition->replacementTokens[i + 1].paramIndex)
          return false;
        const uint32_t paramIdx =
            *calleeDefinition->replacementTokens[i + 1].paramIndex;
        if (paramIdx >= oldActuals.size())
          return false;
        tupleReplayUsesStringification = true;
        CalleeReplayElem elem;
        elem.kind = CalleeReplayKind::Stringify;
        elem.paramIdx = paramIdx;
        out.push_back(std::move(elem));
        i += 2;
        continue;
      }

      if (i + 1 < end &&
          calleeDefinition->replacementTokens[i + 1].spelling == "##") {
        tupleReplayUsesPaste = true;
        CalleeReplayElem elem;
        elem.kind = CalleeReplayKind::Paste;
        CalleePastePiece first;
        if (!pastePieceFromReplacementToken(tok, first))
          return false;
        elem.pastePieces.push_back(std::move(first));
        i += 2;
        while (true) {
          if (i >= end)
            return false;
          CalleePastePiece next;
          if (!pastePieceFromReplacementToken(
                  calleeDefinition->replacementTokens[i], next))
            return false;
          elem.pastePieces.push_back(std::move(next));
          ++i;
          if (i >= end ||
              calleeDefinition->replacementTokens[i].spelling != "##")
            break;
          ++i;
        }
        out.push_back(std::move(elem));
        continue;
      }

      if (tok.kind == RefoldModel::MacroReplacementTokenKind::ParamRef) {
        if (!tok.paramIndex || *tok.paramIndex >= oldActuals.size())
          return false;
        CalleeReplayElem elem;
        elem.kind = CalleeReplayKind::Param;
        elem.paramIdx = *tok.paramIndex;
        out.push_back(std::move(elem));
        ++i;
        continue;
      }
      if (tok.spelling == "##")
        return false;
      if (tok.spelling == "__VA_OPT__")
        return false;
      CalleeReplayElem elem;
      elem.kind = CalleeReplayKind::Literal;
      elem.literal = tok.spelling.str();
      out.push_back(std::move(elem));
      ++i;
    }
    return true;
  };

  std::vector<CalleeReplayElem> calleePattern;
  if (!parseCalleeReplayRange(0, calleeDefinition->replacementTokens.size(),
                              calleePattern) ||
      calleePattern.empty())
    return std::nullopt;

  std::vector<CalleeReplayElem> replayPattern;
  replayPattern.reserve(forwarderPrefixLiterals.size() +
                        calleePattern.size() +
                        forwarderSuffixLiterals.size());
  for (const std::string &literal : forwarderPrefixLiterals) {
    CalleeReplayElem elem;
    elem.kind = CalleeReplayKind::Literal;
    elem.literal = literal;
    replayPattern.push_back(std::move(elem));
  }
  replayPattern.insert(replayPattern.end(), calleePattern.begin(),
                       calleePattern.end());
  for (const std::string &literal : forwarderSuffixLiterals) {
    CalleeReplayElem elem;
    elem.kind = CalleeReplayKind::Literal;
    elem.literal = literal;
    replayPattern.push_back(std::move(elem));
  }

  using SolvedActuals = SmallVector<std::string, 8>;
  auto assignSolvedActual = [&](SolvedActuals &actuals, uint32_t paramIdx,
                                StringRef value) -> bool {
    if (paramIdx >= actuals.size())
      return false;
    if (!textsTokenEquivalent(actuals[paramIdx], oldActuals[paramIdx]) &&
        !textsTokenEquivalent(actuals[paramIdx], value))
      return false;
    if (textsTokenEquivalent(actuals[paramIdx], oldActuals[paramIdx])) {
      actuals[paramIdx] = value.trim().str();
      return true;
    }
    return textsTokenEquivalent(actuals[paramIdx], value);
  };

  auto solvePasteToken =
      [&](ArrayRef<CalleePastePiece> pieces, StringRef spelling,
          const SolvedActuals &seed) -> SmallVector<SolvedActuals, 4> {
    SmallVector<SolvedActuals, 4> solutions;

    const bool hasLiteralAnchor =
        llvm::any_of(pieces, [](const CalleePastePiece &piece) {
          return !piece.isParam && !piece.literal.empty();
        });

    // If a paste expression has no fixed literal anchor (`A ## B`), arbitrary
    // substring search would invent cuts for length-changing rewrites.  The
    // only deterministic inverse available in that shape is the original
    // contribution width recorded by the tuple slots.  Anchored paste forms
    // (`pre_ ## X ## _suf`, `A ## _ ## B`) use the generic DFS below instead.
    if (!hasLiteralAnchor) {
      SolvedActuals cur = seed;
      size_t cursor = 0;
      for (const CalleePastePiece &piece : pieces) {
        if (!piece.isParam) {
          if (!spelling.substr(cursor).starts_with(piece.literal))
            return solutions;
          cursor += piece.literal.size();
          continue;
        }
        if (piece.paramIdx >= oldActuals.size())
          return solutions;
        const size_t width =
            StringRef(oldActuals[piece.paramIdx]).trim().size();
        if (cursor + width > spelling.size())
          return solutions;
        if (!assignSolvedActual(cur, piece.paramIdx,
                                spelling.slice(cursor, cursor + width)))
          return solutions;
        cursor += width;
      }
      if (cursor == spelling.size())
        solutions.push_back(std::move(cur));
      return solutions;
    }

    std::function<void(size_t, size_t, SolvedActuals &)> dfsPaste;
    dfsPaste = [&](size_t pieceIdx, size_t cursor, SolvedActuals &cur) {
      if (solutions.size() > 1)
        return;
      if (pieceIdx == pieces.size()) {
        if (cursor == spelling.size())
          solutions.push_back(cur);
        return;
      }
      const CalleePastePiece &piece = pieces[pieceIdx];
      if (!piece.isParam) {
        if (spelling.substr(cursor).starts_with(piece.literal))
          dfsPaste(pieceIdx + 1, cursor + piece.literal.size(), cur);
        return;
      }

      for (size_t end = cursor; end <= spelling.size(); ++end) {
        StringRef slice = spelling.slice(cursor, end);
        SolvedActuals next = cur;
        if (!assignSolvedActual(next, piece.paramIdx, slice))
          continue;
        dfsPaste(pieceIdx + 1, end, next);
        if (solutions.size() > 1)
          return;
      }
    };
    SolvedActuals start = seed;
    dfsPaste(0, 0, start);
    return solutions;
  };

  auto solveExpansion =
      [&](StringRef expansion) -> std::optional<SolvedActuals> {
    SmallVector<ReplayTok, 32> toks;
    lexReplayTokens(expansion, toks);
    SmallVector<SolvedActuals, 4> solutions;
    SolvedActuals seed;
    seed.reserve(oldActuals.size());
    for (const std::string &actual : oldActuals)
      seed.push_back(actual);

    std::function<void(ArrayRef<CalleeReplayElem>, size_t, SolvedActuals &)>
        dfs;
    dfs = [&](ArrayRef<CalleeReplayElem> elems, size_t tokPos,
              SolvedActuals &cur) {
      if (solutions.size() > 1)
        return;
      if (elems.empty()) {
        if (tokPos == toks.size())
          solutions.push_back(cur);
        return;
      }
      const CalleeReplayElem &elem = elems.front();
      ArrayRef<CalleeReplayElem> rest = elems.drop_front();
      switch (elem.kind) {
      case CalleeReplayKind::Literal:
        if (tokPos < toks.size() && toks[tokPos].spelling == elem.literal)
          dfs(rest, tokPos + 1, cur);
        return;
      case CalleeReplayKind::Param: {
        for (size_t end = tokPos; end <= toks.size(); ++end) {
          StringRef value;
          if (end > tokPos) {
            const size_t byteBegin = toks[tokPos].begin;
            const size_t byteEnd = toks[end - 1].end;
            value = expansion.slice(byteBegin, byteEnd);
          }
          SolvedActuals next = cur;
          if (!assignSolvedActual(next, elem.paramIdx, value))
            continue;
          dfs(rest, end, next);
          if (solutions.size() > 1)
            return;
        }
        return;
      }
      case CalleeReplayKind::Stringify: {
        if (tokPos >= toks.size())
          return;
        std::optional<std::string> content =
            decodeSimpleStringLiteralToken(toks[tokPos].spelling);
        if (!content)
          return;
        SolvedActuals next = cur;
        if (!assignSolvedActual(next, elem.paramIdx, StringRef(*content)))
          return;
        dfs(rest, tokPos + 1, next);
        return;
      }
      case CalleeReplayKind::Paste: {
        if (tokPos >= toks.size())
          return;
        SmallVector<SolvedActuals, 4> pasteSolutions =
            solvePasteToken(elem.pastePieces, toks[tokPos].spelling, cur);
        for (SolvedActuals &pasteSol : pasteSolutions) {
          dfs(rest, tokPos + 1, pasteSol);
          if (solutions.size() > 1)
            return;
        }
        return;
      }
      case CalleeReplayKind::VaOpt:
        return;
      }
    };

    dfs(replayPattern, 0, seed);
    if (solutions.size() != 1)
      return std::nullopt;
    return solutions.front();
  };

  std::optional<SolvedActuals> oldSolved = solveExpansion(oldExpansion);
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

  std::optional<SolvedActuals> newSolved = solveExpansion(newExpansion);
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
      edits.push_back(TupleEdit{firstElem.trimBegin, lastElem.trimEnd,
                                std::move(text)});
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
      edits.push_back(TupleEdit{elem.trimBegin, elem.trimEnd,
                                std::move(*rewrittenElem)});
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
  patch.materializedOutputByteStart = 0;
  patch.materializedOutputByteEnd = patch.replacement.size();
  patch.hasMaterializedOutputByteRange = true;
  stampMacroPatchMaterializedBTokenRange(patch,
                                         static_cast<uint64_t>(bEnv->first),
                                         static_cast<uint64_t>(bEnv->second));
  SetArgsOnlyStandardProof(patch, m, /*wholeEnvelopeReplayValidated=*/true);
  StampGeneratedCalleeReplayProof(
      patch, m, calleeDefinition ? calleeDefinition->id : 0,
      /*generatedCallDepth=*/1, tupleGeneratedCtx.objectAliasHopCount,
      tupleReplayUsesStringification, tupleReplayUsesPaste,
      tupleReplayUsesVariadicForwarding,
      /*decodedStringLiteralEvidenceOnly=*/tupleReplayUsesStringification);
  return patch;
}

// Stamp the B-token envelope corresponding to an invocation's whole A-side
// macro cover.  The helper depends only on planner services plus its explicit
// invocation/patch arguments.
bool RefoldMacroPatchPlanner::StampMacroPatchWholeExpansionBRange(
    const RefoldModel::MacroInvocation &m, MacroPatch &patch) const {
  std::optional<std::pair<uint64_t, uint64_t>> cover =
      GetWholeCoverATokRange(m);
  if (!cover)
    return false;

  std::optional<std::pair<size_t, size_t>> bEnv =
      (*deps_.sourceMapper).MapATokRangeAToBTokenEnvelopePreserveBoundaryInsertions(
          cover->first, cover->second);
  if (!bEnv || bEnv->first >= bEnv->second)
    return false;

  patch.hasMaterializedBTokenRange = true;
  patch.materializedBTokStart = static_cast<uint64_t>(bEnv->first);
  patch.materializedBTokEnd = static_cast<uint64_t>(bEnv->second);
  return true;
}

void RefoldMacroPatchPlanner::SetArgsOnlyStandardProof(
    MacroPatch &patch, const RefoldModel::MacroInvocation &m,
    bool wholeEnvelopeReplayValidated,
    bool definitionTapeReplayValidated) const {
  MacroPatchProof proof = GetProofLattice().MakeMacroPatchProof(
      MacroPatchProofKind::ArgsOnlyStandard,
      /*preservesInvocationStructure=*/true, m.id);
  if (wholeEnvelopeReplayValidated) {
    WholeEnvelopeReplayWitness witness;
    witness.rootMacroId = m.id;
    witness.replayValidated = true;
    witness.definitionTapeReplayValidated = definitionTapeReplayValidated;
    proof.wholeEnvelopeReplay = witness;
  }
  GetProofLattice().SetMacroPatchProof(patch, std::move(proof));
}

void RefoldMacroPatchPlanner::StampGeneratedCalleeReplayProof(
    MacroPatch &patch, const RefoldModel::MacroInvocation &m,
    uint64_t finalDirectiveId, uint32_t generatedCallDepth,
    uint32_t objectAliasHops, bool usesStringification, bool usesPaste,
    bool usesVariadicForwarding,
    bool decodedStringLiteralEvidenceOnly) const {
  // This only enriches the already-validated whole-envelope replay.  It does
  // not reclassify the macro proof or change selector behavior.
  MacroPatchProof proof = patch.proof;
  GeneratedCalleeReplayWitness witness;
  witness.rootMacroId = m.id;
  witness.finalDirectiveId = finalDirectiveId;
  witness.generatedCallDepth = generatedCallDepth;
  witness.objectAliasHops = objectAliasHops;
  witness.calleeChainDeterministic = true;
  witness.replacementReplayValidated = true;
  witness.solvedActualsMappedToRoot = true;
  witness.usesForwarding = true;
  witness.usesStringification = usesStringification;
  witness.usesPaste = usesPaste;
  witness.usesVariadicForwarding = usesVariadicForwarding;
  witness.usesObjectAlias = objectAliasHops != 0;
  witness.decodedStringLiteralEvidenceOnly =
      decodedStringLiteralEvidenceOnly || usesStringification;
  proof.generatedCalleeReplay = std::move(witness);
  GetProofLattice().SetMacroPatchProof(patch, std::move(proof));
}

const RefoldModel::MacroDirective *
RefoldMacroPatchPlanner::GetDefinitionDirectiveForInvocation(
    const RefoldModel::MacroInvocation &m) const {
  if (!m.definitionDirectiveId)
    return nullptr;
  for (const RefoldModel::MacroDirective &directive :
       (*deps_.model).GetMacroDirectives()) {
    if (directive.id == *m.definitionDirectiveId)
      return &directive;
  }
  return nullptr;
}

bool RefoldMacroPatchPlanner::MatchLiteralAToken(
    uint64_t tok, llvm::StringRef spelling) const {
  return tok < deps_.aToks.size() &&
         deps_.aToks[static_cast<size_t>(tok)].spelling == spelling;
}

bool RefoldMacroPatchPlanner::CurrentLevelInvocationIsInSubtreeOf(
    const RefoldModel::MacroInvocation &macro, uint64_t rootId) const {
  uint64_t currentId = macro.id;
  SmallVector<uint64_t, 8> seen;
  while (true) {
    if (currentId == rootId)
      return true;
    if (std::find(seen.begin(), seen.end(), currentId) != seen.end())
      return false;
    seen.push_back(currentId);

    const RefoldModel::MacroInvocation *current =
        (*deps_.macroTopology).FindMacroInvocationById(currentId);
    if (!current || !current->callerMacroId)
      return false;
    currentId = *current->callerMacroId;
  }
}

bool RefoldMacroPatchPlanner::CurrentLevelSubtreeContainsCounterInvocation(
    uint64_t rootId) const {
  if (!(*deps_.macroTopology).FindMacroInvocationById(rootId))
    return false;
  for (const RefoldModel::MacroInvocation &macro :
       (*deps_.model).GetMacroInvocations()) {
    if (macro.name != "__COUNTER__")
      continue;
    if (CurrentLevelInvocationIsInSubtreeOf(macro, rootId))
      return true;
  }
  return false;
}

std::optional<unsigned>
RefoldMacroPatchPlanner::CandidateDepthInValidatedSubtree(
    const MacroSubtreeReplayValidationContext &ctx,
    const RefoldModel::MacroInvocation &candidate) const {
  // Candidates must be strict descendants of the validated root.  The root
  // invocation itself is not a
  // descendant candidate for this path.  The returned depth is also the
  // original DAG leaf tie-breaker, so it must be computed from the same
  // fail-closed ancestry walk as the membership test.
  unsigned depth = 0;
  std::optional<uint64_t> parent = candidate.callerMacroId;
  while (parent) {
    ++depth;
    if (*parent == ctx.rootInvocation.id)
      return depth;
    auto it = ctx.invocationById.find(*parent);
    if (it == ctx.invocationById.end())
      return std::nullopt;
    parent = it->second->callerMacroId;
  }
  return std::nullopt;
}

bool RefoldMacroPatchPlanner::CandidateBelongsToValidatedSubtree(
    const MacroSubtreeReplayValidationContext &ctx,
    const RefoldModel::MacroInvocation &candidate) const {
  return CandidateDepthInValidatedSubtree(ctx, candidate).has_value();
}

bool RefoldMacroPatchPlanner::SubtreePathHasProvableCalleeClosure(
    const MacroSubtreeReplayValidationContext &ctx,
    const RefoldModel::MacroInvocation &candidate) const {
  // A descendant is replayable through the validated root only when every
  // generated-callee hop is justified either by a literal callee spelling or by
  // the already-proven whole-formal caller-forwarding rule.  Unsupported or
  // broken ancestry remains a fail-closed rejection.
  const RefoldModel::MacroInvocation *cur = &candidate;
  for (;;) {
    if (!hasLiteralMacroCalleeOrigin(*cur)) {
      if (cur->calleeOrigin.kind != MacroCalleeOriginKind::CallerParam ||
          !cur->callerMacroId ||
          cur->calleeOrigin.callerParamIndices.size() != 1)
        return false;

      auto parentIt = ctx.invocationById.find(*cur->callerMacroId);
      if (parentIt == ctx.invocationById.end())
        return false;

      const uint32_t slot = cur->calleeOrigin.callerParamIndices.front();
      if (!isWholeFormalCallerForwardSlot(*parentIt->second, slot))
        return false;
    }

    if (cur->id == ctx.rootInvocation.id)
      return true;
    if (!cur->callerMacroId)
      return false;
    auto it = ctx.invocationById.find(*cur->callerMacroId);
    if (it == ctx.invocationById.end())
      return false;
    cur = it->second;
  }
}

bool RefoldMacroPatchPlanner::SubtreeReplayDoesNotContradictSiblingSurface(
    const MacroSubtreeReplayValidationContext &ctx,
    const MacroPatch &patch) const {
  const RefoldModel::MacroInvocation &root = ctx.rootInvocation;

  // Only local DAG subtree-root proofs need this owner-closure check.  Other
  // proof kinds are either already owner-wide or are handled by their own final
  // replay-stability gates.
  if (patch.proof.kind != MacroPatchProofKind::DagSubtreeRoot ||
      !patch.subtreeCertBacked ||
      patch.proof.proofRootMacroId != root.id ||
      !patch.proof.preservesInvocationStructure)
    return true;
  if (!RefoldLineObserverLayout::InvocationSpanMatchesCallsitePrefix(
          patch.replacement, root))
    return true;

  auto rootRangesOpt =
      GetMacroInvocationFormalArgContentRanges(root, patch.replacement);
  if (!rootRangesOpt)
    return true;

  SmallVector<std::string, 8> rootActuals;
  rootActuals.reserve(rootRangesOpt->size());
  for (const auto &range : *rootRangesOpt)
    rootActuals.push_back(
        StringRef(patch.replacement).slice(range.first, range.second).str());

  auto hasNonForwardedSyntaxOutsideRefs =
      [&](StringRef argText, ArrayRef<RefoldModel::InvArgRef> refs,
          size_t argAbsBegin) {
        SmallVector<std::pair<size_t, size_t>, 4> ranges;
        for (const RefoldModel::InvArgRef &ref : refs) {
          if (ref.byteBegin < argAbsBegin || ref.byteEnd < ref.byteBegin)
            continue;
          const size_t relBegin =
              static_cast<size_t>(ref.byteBegin - argAbsBegin);
          const size_t relEnd = static_cast<size_t>(ref.byteEnd - argAbsBegin);
          if (relEnd > argText.size())
            continue;
          ranges.push_back({relBegin, relEnd});
        }
        llvm::sort(ranges);

        size_t cursor = 0;
        for (const auto &range : ranges) {
          if (range.first > cursor &&
              !argText.slice(cursor, range.first).trim().empty())
            return true;
          cursor = std::max(cursor, range.second);
        }
        return cursor < argText.size() &&
               !argText.drop_front(cursor).trim().empty();
      };

  auto replayStringifyArgumentThroughRoot =
      [&](const RefoldModel::MacroInvocation &inv,
          uint32_t argIdx) -> std::optional<std::string> {
    if (!DirectlyStringifiesFormal(inv, argIdx))
      return std::nullopt;
    if (!inv.invText || argIdx >= inv.argRefs.size())
      return std::nullopt;
    auto rangesOpt =
        GetMacroInvocationFormalArgContentRanges(inv, *inv.invText);
    if (!rangesOpt || argIdx >= rangesOpt->size())
      return std::nullopt;

    const auto argRange = (*rangesOpt)[argIdx];
    StringRef invText = *inv.invText;
    if (argRange.second > invText.size() || argRange.second < argRange.first)
      return std::nullopt;

    StringRef oldArgText = invText.slice(argRange.first, argRange.second);
    const auto &refs = inv.argRefs[argIdx];
    if (!hasNonForwardedSyntaxOutsideRefs(oldArgText, refs, argRange.first))
      return std::nullopt;

    std::string replayed = oldArgText.str();
    SmallVector<RefoldModel::InvArgRef, 4> sortedRefs;
    sortedRefs.append(refs.begin(), refs.end());
    llvm::sort(sortedRefs, invArgRefGreaterByByteRange);

    for (const RefoldModel::InvArgRef &ref : sortedRefs) {
      if (ref.callerParamIndex >= rootActuals.size())
        return std::nullopt;
      if (ref.byteBegin < argRange.first || ref.byteEnd < ref.byteBegin)
        return std::nullopt;
      const size_t relBegin =
          static_cast<size_t>(ref.byteBegin - argRange.first);
      const size_t relEnd = static_cast<size_t>(ref.byteEnd - argRange.first);
      if (relEnd > replayed.size())
        return std::nullopt;
      replayed.replace(relBegin, relEnd - relBegin,
                       rootActuals[ref.callerParamIndex]);
    }

    return stringutils::canonicalizeStringifyInversePayload(replayed);
  };

  for (const RefoldModel::MacroInvocation &inv :
       (*deps_.model).GetMacroInvocations()) {
    if ((*deps_.macroTopology).GetRootMacroId(inv.id) != root.id)
      continue;
    for (const RefoldModel::PPArgSpan &span : inv.stringifySpans) {
      if (!HunkTouchesASpan(span.begin, span.end))
        continue;

      std::optional<std::pair<size_t, size_t>> bEnv =
          (*deps_.sourceMapper)
              .MapATokRangeAToBTokenEnvelopePreserveBoundaryInsertions(
                  span.begin, span.end);
      if (!bEnv || bEnv->second <= bEnv->first)
        continue;

      std::optional<std::string> bPayload =
          (*deps_.argTextRecovery)
              .UnstringifyLiteralToArgText(
                  (*deps_.sourceMapper).SliceBSource(bEnv->first, bEnv->second),
                  /*allowTopLevelComma=*/true);
      if (!bPayload)
        continue;
      std::optional<std::string> canonicalB =
          stringutils::canonicalizeStringifyInversePayload(*bPayload);
      if (!canonicalB)
        continue;

      std::optional<std::string> replayed =
          replayStringifyArgumentThroughRoot(inv, span.argIdx);
      if (!replayed)
        continue;

      if (*replayed != *canonicalB) {
        REFOLD_LOG_TRACE(
            "macro/proof",
            "suppress DAG subtree root replay: sibling direct stringify "
            "surface would change under candidate root invocation inv id={0} "
            "name={1} stringifyMacro={2} stringifyName={3} span=[{4},{5}) "
            "replayed='{6}' bPayload='{7}' replacement='{8}'",
            root.id, root.name, inv.id, inv.name, span.begin, span.end,
            stringutils::showWsWithClip(*replayed, 160),
            stringutils::showWsWithClip(*canonicalB, 160),
            stringutils::showWsWithClip(patch.replacement, 220));
        return false;
      }
    }
  }

  return true;
}

bool RefoldMacroPatchPlanner::ClaimedWholeEnvelopeIsReplaySafe(
    const MacroSubtreeReplayValidationContext &ctx,
    const MacroPatch &patch) const {
  // A claimed whole-envelope replay backed only by a local DAG subtree proof is
  // admissible only if the replay remains closed over sibling observer
  // surfaces.  Keep this wrapper separate so future proof-stamping extraction
  // can add more whole-envelope gates without changing final-candidate order.
  return SubtreeReplayDoesNotContradictSiblingSurface(ctx, patch);
}

const RefoldModel::MacroDirective *
RefoldMacroPatchPlanner::ResolveFunctionLikeMacroThroughAliasesWithHops(
    llvm::StringRef startName, uint32_t *aliasHops) const {
  if (aliasHops)
    *aliasHops = 0;
  if (startName.empty())
    return nullptr;
  SmallVector<std::string, 8> seen;
  std::string current = startName.trim().str();
  for (size_t depth = 0; depth <= (*deps_.model).GetMacroDirectives().size();
       ++depth) {
    if (llvm::is_contained(seen, current))
      return nullptr;
    seen.push_back(current);

    const RefoldModel::MacroDirective *functionLike = nullptr;
    const RefoldModel::MacroDirective *alias = nullptr;
    for (const RefoldModel::MacroDirective &directive :
         (*deps_.model).GetMacroDirectives()) {
      if (directive.subkind != "#define" ||
          directive.name != llvm::StringRef(current))
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
    if (aliasHops)
      ++*aliasHops;
    current = alias->replacementTokens[0].spelling.str();
  }
  return nullptr;
}

const RefoldModel::MacroDirective *
RefoldMacroPatchPlanner::ResolveFunctionLikeMacroForReplay(
    llvm::StringRef startName) const {
  return ResolveFunctionLikeMacroThroughAliasesWithHops(startName, nullptr);
}

bool RefoldMacroPatchPlanner::IsObjectLikeSingleTokenAlias(
    llvm::StringRef name) const {
  for (const RefoldModel::MacroDirective &directive :
       (*deps_.model).GetMacroDirectives()) {
    if (directive.subkind != "#define" || directive.name != name ||
        directive.functionLike)
      continue;
    if (directive.replacementTokens.size() == 1 &&
        directive.replacementTokens[0].kind ==
            RefoldModel::MacroReplacementTokenKind::Literal)
      return true;
  }
  return false;
}

bool RefoldMacroPatchPlanner::TokenSpellingsEqualToA(
    llvm::ArrayRef<std::string> expected, uint64_t beginTok,
    uint64_t endTok) const {
  if (endTok < beginTok || endTok > deps_.aToks.size() ||
      endTok - beginTok != expected.size())
    return false;
  for (size_t i = 0; i < expected.size(); ++i) {
    if (deps_.aToks[static_cast<size_t>(beginTok) + i].spelling !=
        llvm::StringRef(expected[i]))
      return false;
  }
  return true;
}

bool RefoldMacroPatchPlanner::TokenSpellingsEqualToB(
    llvm::ArrayRef<std::string> expected, uint64_t beginTok,
    uint64_t endTok) const {
  if (endTok < beginTok || endTok > deps_.bToks.size() ||
      endTok - beginTok != expected.size())
    return false;
  for (size_t i = 0; i < expected.size(); ++i) {
    if (deps_.bToks[static_cast<size_t>(beginTok) + i].spelling !=
        llvm::StringRef(expected[i]))
      return false;
  }
  return true;
}

bool RefoldMacroPatchPlanner::HunkTouchesASpan(
    uint64_t begin, uint64_t end) const {
  for (const diffutils::Hunk &hunk : (*deps_.abTokHunks)) {
    if (hunk.aStart < end && begin < hunk.aEnd)
      return true;
  }
  return false;
}

bool RefoldMacroPatchPlanner::ReplacementObservesDirective(
    const RefoldModel::MacroDirective &directive, llvm::StringRef macroName,
    llvm::StringRef replacement) const {
  return GetMacroStateProof()
      .FirstMacroStateObservationOffsetInText(directive, macroName,
                                              replacement)
      .has_value();
}

bool RefoldMacroPatchPlanner::DirectlyStringifiesFormal(
    const RefoldModel::MacroInvocation &inv, uint32_t argIdx) const {
  const RefoldModel::MacroDirective *definition =
      GetDefinitionDirectiveForInvocation(inv);
  if (!definition || argIdx >= definition->defParams.size())
    return false;

  for (size_t i = 0; i + 1 < definition->replacementTokens.size(); ++i) {
    const auto &hash = definition->replacementTokens[i];
    const auto &formal = definition->replacementTokens[i + 1];
    if (hash.spelling == "#" &&
        formal.kind == RefoldModel::MacroReplacementTokenKind::ParamRef &&
        formal.paramIndex && *formal.paramIndex == argIdx)
      return true;
  }
  return false;
}

bool RefoldMacroPatchPlanner::IsParenthesizedTuple(
    llvm::StringRef arg) const {
  arg = arg.trim();
  if (!arg.starts_with("(") || !arg.ends_with(")") || arg.size() < 2)
    return false;
  SmallVector<TupleElementSlice, 8> elems;
  return splitTopLevelTupleElementsWithLexer(arg.drop_front().drop_back(),
                                            (*deps_.lexLang), elems) &&
         elems.size() >= 2;
}

bool RefoldMacroPatchPlanner::StructurePreservingCallsiteHasStableFormalSyntax(
    const MacroPatch &patch, const RefoldModel::MacroInvocation &m) const {
  // Invocation-preserving macro patches are replayed by the preprocessor as
  // function-like macro callsites.  Therefore the replacement text must itself
  // be a complete, well-formed invocation of this macro.
  if (!patch.proof.preservesInvocationStructure ||
      patch.proof.proofRootMacroId != m.id)
    return true;
  if (m.subkind != "func")
    return true;
  if (!RefoldLineObserverLayout::InvocationSpanMatchesCallsitePrefix(
          patch.replacement, m)) {
    REFOLD_LOG_TRACE("macro/proof",
          "suppress structure-preserving macro replay: inv id={0} "
          "name={1} replacement no longer has a matching callsite "
          "prefix",
          m.id, m.name);
    return false;
  }

  auto parsedActuals =
      RefoldArgTextRecovery::LexMacroInvocationActualContentRanges(
          patch.replacement, (*deps_.lexLang));
  if (!parsedActuals) {
    REFOLD_LOG_TRACE("macro/proof",
          "suppress structure-preserving macro replay: inv id={0} "
          "name={1} replacement is not a complete macro invocation: "
          "'{2}'",
          m.id, m.name,
          stringutils::showWsWithClip(patch.replacement, 220));
    return false;
  }

  const size_t formalN = m.defParams.size();
  size_t actualN = parsedActuals->size();
  if (actualN == 0 && formalN != 0) {
    // `M()` is zero actuals for a zero-parameter macro, but for a macro with
    // parameters it is one empty actual followed by any omitted variadic tail.
    actualN = 1;
  }

  bool trailingFormalsAreVariadic = true;
  for (size_t i = actualN; i < formalN; ++i) {
    if (!isMacroInvocationVariadicFormal(m, i)) {
      trailingFormalsAreVariadic = false;
      break;
    }
  }

  bool arityCompatible = false;
  if (actualN == formalN) {
    arityCompatible = true;
  } else if (formalN != 0 && actualN > formalN) {
    // Surplus actuals are valid only for a final variadic formal; they
    // collectively form the variadic tail.
    arityCompatible = isMacroInvocationVariadicFormal(m, formalN - 1);
  } else if (actualN < formalN) {
    // Missing actuals are valid only for omitted trailing variadic formals,
    // e.g. `M(x)` for `M(x, ...)`.
    arityCompatible = trailingFormalsAreVariadic;
  }

  if (!arityCompatible) {
    REFOLD_LOG_TRACE("macro/proof",
          "suppress structure-preserving macro replay: inv id={0} "
          "name={1} replacement actual/formal arity is invalid: "
          "actuals={2} formals={3} text='{4}'",
          m.id, m.name, static_cast<unsigned>(actualN),
          static_cast<unsigned>(formalN),
          stringutils::showWsWithClip(patch.replacement, 220));
    return false;
  }

  return true;
}


// Paste-aware macro-argument helpers moved with the args-only planner.
// They still read mapping/model state through the temporary engine dependency,
// but the proof policy and derivation logic are now owned by this service.

bool RefoldMacroPatchPlanner::HunkTouchesAnyPasteToken(
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

std::optional<PasteArgEdit>
RefoldMacroPatchPlanner::DerivePasteArgEdit(const RefoldModel::MacroInvocation &m,
                                 const diffutils::Hunk &h) const {
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
  auto bEnvOpt = (*deps_.sourceMapper).MapAToBTokenEnvelopeByPPArgSpan(*tokenSpan);
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
  StringRef aTokRaw = (*deps_.sourceMapper).SliceASource(tokenSpan->begin, tokenSpan->end);
  StringRef bTokRaw = (*deps_.sourceMapper).SliceBSource(bEnvOpt->first, bEnvOpt->second);

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
RefoldMacroPatchPlanner::DerivePasteArgEdits(const RefoldModel::MacroInvocation &m,
                                  const diffutils::Hunk &h) const {
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

  auto bEnvOpt = (*deps_.sourceMapper).MapAToBTokenEnvelopeByPPArgSpan(*tokenSpan);
  if (!bEnvOpt || bEnvOpt->second <= bEnvOpt->first)
    return std::nullopt;

  // Paste edits are only representable as args-only when the A-span maps to
  // exactly one B token.
  if (bEnvOpt->second - bEnvOpt->first != 1)
    return std::nullopt;

  StringRef aTokRaw = (*deps_.sourceMapper).SliceASource(tokenSpan->begin, tokenSpan->end);
  StringRef bTokRaw = (*deps_.sourceMapper).SliceBSource(bEnvOpt->first, bEnvOpt->second);

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
      tokenSpan->begin < deps_.aToks.size() && bEnvOpt->first < deps_.bToks.size()) {
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
RefoldMacroPatchPlanner::SegmentPastedTokenArgsByFixedSlices(
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

StringRef RefoldMacroPatchPlanner::DeriveNewPasteSegmentFromSpellingReplacement(
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

bool RefoldMacroPatchPlanner::PasteArgReplacementsMatchAllPasteTokensInB(
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
  if (m.invText)
    origFormalRanges = GetMacroInvocationFormalArgContentRanges(m, *m.invText);

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

  // Format a diagnostic summary of child invocations that consume any missing
  // root arguments through `argDeps`. This is logging-only context for nested
  // delegation failures: it reports which child/formal dependencies are
  // relevant and whether each child covers the token interval being diagnosed.

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
    auto bEnv = (*deps_.sourceMapper).MapATokRangeAToBTokenEnvelope(beginTok, endTok);
    if (!bEnv || bEnv->second != bEnv->first + 1)
      return false;

    // Extract the pasted token text as produced in A and B. We strip newlines
    // defensively since slice helpers may include trailing '\n' depending on
    // how token ranges were formed.
    std::string aTok = stripNewlines((*deps_.sourceMapper).SliceASource(beginTok, endTok));
    std::string bTok = stripNewlines((*deps_.sourceMapper).SliceBSource(bEnv->first, bEnv->second));

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

std::optional<std::vector<RefoldModel::PPArgSpan>>
RefoldMacroPatchPlanner::GetCurrentLevelStandardArgSpans(
    const ArgsOnlyTemplateReplayContext &ctx) const {
  const RefoldModel::MacroInvocation &invocation = ctx.invocation;
  auto coverOpt = GetWholeCoverATokRange(invocation);
  if (ctx.invocationArgRanges.empty() || !coverOpt ||
      coverOpt->first >= coverOpt->second)
    return std::nullopt;

  auto surface =
      GetCurrentLevelTemplateSurfaceForInvocation(invocation,
                                                  ctx.invocationArgRanges);
  if (!surface || surface->coverBegin != coverOpt->first ||
      surface->coverEnd != coverOpt->second)
    return std::nullopt;

  return std::move(surface->standardSpans);
}

std::optional<RefoldMacroPatchPlanner::CurrentLevelTemplateSurface>
RefoldMacroPatchPlanner::GetCurrentLevelTemplateSurfaceForInvocation(
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
RefoldMacroPatchPlanner::GetCurrentLevelStandardArgSpansForInvocation(
    const RefoldModel::MacroInvocation &invocation,
    ArrayRef<std::pair<size_t, size_t>> formalRanges) const {
  auto surface =
      GetCurrentLevelTemplateSurfaceForInvocation(invocation, formalRanges);
  if (!surface)
    return std::nullopt;
  return std::move(surface->standardSpans);
}

std::optional<std::pair<std::string, std::string>>
RefoldMacroPatchPlanner::GetCurrentLevelExpansionTextForInvocation(
    const RefoldModel::MacroInvocation &invocation) const {
  if (!invocation.invText)
    return std::nullopt;
  auto formalRangesOpt =
      GetMacroInvocationFormalArgContentRanges(invocation,
                                                     *invocation.invText);
  if (!formalRangesOpt)
    return std::nullopt;

  auto surface =
      GetCurrentLevelTemplateSurfaceForInvocation(invocation, *formalRangesOpt);
  if (!surface || surface->coverBegin >= surface->coverEnd)
    return std::nullopt;

  auto bEnv =
      (*deps_.sourceMapper).MapATokRangeAToBTokenEnvelopePreserveBoundaryInsertions(
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

bool RefoldMacroPatchPlanner::ArgsOnlyTemplateReplayPreservesEnvelope(
    const RefoldModel::MacroInvocation &invocation,
    const CurrentLevelTemplateSurface &surface) const {
  std::optional<std::pair<size_t, size_t>> wholeBEnv =
      (*deps_.sourceMapper).MapATokRangeAToBTokenEnvelopePreserveBoundaryInsertions(
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
      elemBEnv =
          (*deps_.sourceMapper).MapATokRangeAToBTokenEnvelopePreserveBoundaryInsertions(
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

void RefoldMacroPatchPlanner::AttachArgsOnlyProofCarrier(
    const ArgsOnlyTemplateReplayContext &ctx, MacroPatch &patch,
    bool wholeEnvelopeReplayValidated, bool definitionTapeReplayValidated) const {
  SetArgsOnlyStandardProof(patch, ctx.invocation, wholeEnvelopeReplayValidated,
                           definitionTapeReplayValidated);
}

void RefoldMacroPatchPlanner::StampArgsOnlyAcceptedCandidate(
    const ArgsOnlyTemplateReplayContext &ctx, MacroPatch &patch,
    const InvocationRewriteWithRange &rewrite,
    std::pair<size_t, size_t> bTokenEnvelope) const {
  (void)ctx;
  patch.hasMaterializedOutputByteRange = true;
  patch.materializedOutputByteStart = rewrite.materializedOutputByteStart;
  patch.materializedOutputByteEnd = rewrite.materializedOutputByteEnd;
  stampMacroPatchMaterializedBTokenRange(
      patch, static_cast<uint64_t>(bTokenEnvelope.first),
      static_cast<uint64_t>(bTokenEnvelope.second));
}

std::optional<MacroPatch>
RefoldMacroPatchPlanner::TryTemplateSolvedArgsOnlyPatch(
    const ArgsOnlyTemplateReplayContext &ctx) const {
  const InvocationActualRecoveryContext actualRecoveryCtx{
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

  auto cover = GetWholeCoverATokRange(templateInvocation);
  if (!cover) {
    return std::nullopt;
  }

  // Recursively rebuild a macro invocation's source spelling from its
  // current-level template proof.  The recursion is used only to preserve
  // nested lexical child invocations whose old and new local expansion
  // surfaces can be proven to bridge the parent argument.
  std::function<std::optional<std::string>(const RefoldModel::MacroInvocation &,
                                           unsigned)>
      buildInvocationSyntaxFromCurrentLevelTemplate;

  buildInvocationSyntaxFromCurrentLevelTemplate =
      [&](const RefoldModel::MacroInvocation &inv,
          unsigned depth) -> std::optional<std::string> {
    // The recursion follows recorded child invocation edges.  A path deeper
    // than the number of recorded invocations implies a cycle or stale
    // metadata, so use that structural bound instead of a fixed depth cap.
    if (depth > (*deps_.model).GetMacroInvocations().size() || !inv.invText ||
        !inv.invB || !inv.invE || !inv.stringifySpans.empty() ||
        !inv.pasteSpans.empty())
      return std::nullopt;

    auto formalRangesOpt =
        GetMacroInvocationFormalArgContentRanges(inv, *inv.invText);
    if (!formalRangesOpt)
      return std::nullopt;
    const auto &formalRanges = *formalRangesOpt;

    auto standardSpansOpt =
        GetCurrentLevelStandardArgSpansForInvocation(inv, formalRanges);
    if (!standardSpansOpt)
      return std::nullopt;
    const auto &standardSpans = *standardSpansOpt;
    if (standardSpans.size() != formalRanges.size())
      return std::nullopt;

    // Accumulate replacements by parsed formal slot.  Each replacement is
    // derived from one standard span's old expansion and its mapped B-side
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
      auto bEnv = (*deps_.sourceMapper).MapAToBTokenEnvelopeByPPArgSpan(sp);
      if (!bEnv)
        return std::nullopt;
      StringRef newExpansion =
          (*deps_.sourceMapper).SliceBSource(bEnv->first, bEnv->second).trim();
      if (oldExpansion.empty() || newExpansion.empty())
        return std::nullopt;

      std::optional<std::string> replacement;
      if (oldExpansion == baseTrim) {
        replacement = newExpansion.str();
      } else if (auto loc =
                     findUniqueTrimmedSubstring(baseTrim, oldExpansion)) {
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
        // `callerMacroId` is honored when present, but absence of that edge
        // is not enough to accept a child; the file/range containment and the
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
              GetCurrentLevelExpansionTextForInvocation(child);
          if (!childExpansion)
            continue;
          auto childNewSyntax =
              buildInvocationSyntaxFromCurrentLevelTemplate(child, depth + 1);
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
      if (!allowComma &&
          refoldMacroActualHasTopLevelComma(StringRef(*replacement), (*deps_.lexLang)))
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
    return StringRef(rewritten).trim().str();
  };

  // Prefer the current-level template proof when it can rewrite the whole
  // invocation.  It captures the brace/bracket comma-split cases before the
  // older hunk-local machinery has a chance to accept a partial patch.
  if (auto currentLevelRewrite = buildInvocationSyntaxFromCurrentLevelTemplate(
          templateInvocation, 0)) {
    if (StringRef(*currentLevelRewrite).trim() !=
        templateBaseInvocationText.trim()) {
      bool counterWholeEnvelopeReplayValidated = false;
      if (CurrentLevelSubtreeContainsCounterInvocation(templateInvocation.id)) {
        auto currentLevelSurface = GetCurrentLevelTemplateSurfaceForInvocation(
            templateInvocation, templateInvocationArgRanges);
        counterWholeEnvelopeReplayValidated =
            currentLevelSurface &&
            ArgsOnlyTemplateReplayPreservesEnvelope(templateInvocation,
                                                    *currentLevelSurface);
      }

      MacroPatch patch{*templateInvocation.invB, *templateInvocation.invE,
                       std::move(*currentLevelRewrite), templateInvocation.id};
      patch.materializedOutputByteStart = 0;
      patch.materializedOutputByteEnd = patch.replacement.size();
      patch.hasMaterializedOutputByteRange = true;
      StampMacroPatchWholeExpansionBRange(templateInvocation, patch);
      AttachArgsOnlyProofCarrier(ctx, patch, /*wholeEnvelopeReplayValidated=*/
                                 counterWholeEnvelopeReplayValidated);
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

  auto bodyMatchesAt = [&](const ArgsOnlyTemplateElem &elem,
                           size_t bPos) -> bool {
    const size_t len = static_cast<size_t>(elem.aEnd - elem.aBegin);
    if (bPos + len > bEnv->second)
      return false;
    for (size_t i = 0; i < len; ++i) {
      if (deps_.aToks[static_cast<size_t>(elem.aBegin) + i].spelling !=
          deps_.bToks[bPos + i].spelling)
        return false;
    }
    return true;
  };

  const std::pair<size_t, size_t> unset = {std::numeric_limits<size_t>::max(),
                                           std::numeric_limits<size_t>::max()};
  std::vector<std::pair<size_t, size_t>> curAssign(occurrenceCount, unset);
  std::vector<std::vector<std::pair<size_t, size_t>>> solutions;

  // Enumerate all bounded assignments of B-token intervals to argument
  // occurrences while requiring fixed body spans to match literally. The cap
  // keeps this a small proof search rather than an unbounded parser.
  std::function<void(size_t, size_t)> dfs = [&](size_t elemIdx, size_t bPos) {
    if (solutions.size() > 16)
      return;
    if (elemIdx == elems.size()) {
      if (bPos == bEnv->second)
        solutions.push_back(curAssign);
      return;
    }

    const ArgsOnlyTemplateElem &elem = elems[elemIdx];
    if (!elem.isArg) {
      const size_t len = static_cast<size_t>(elem.aEnd - elem.aBegin);
      if (bodyMatchesAt(elem, bPos))
        dfs(elemIdx + 1, bPos + len);
      return;
    }

    for (size_t end = bPos + 1; end <= bEnv->second; ++end) {
      curAssign[elem.occurrenceOrdinal] = {bPos, end};
      dfs(elemIdx + 1, end);
      curAssign[elem.occurrenceOrdinal] = unset;
      if (solutions.size() > 16)
        return;
    }
  };

  dfs(0, bEnv->first);
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
      -> std::optional<InvocationRewriteWithRange> {
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

    return BuildInvocationRewriteWithRange(actualRecoveryCtx, replByArg);
  };

  // Multiple token-template assignments are acceptable only when they all
  // reconstruct the same invocation spelling. Otherwise the expansion surface
  // is underdetermined and this proof path fails closed.
  std::optional<InvocationRewriteWithRange> uniqueRewrite;
  for (const auto &sol : solutions) {
    std::optional<InvocationRewriteWithRange> candidate =
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
  StampArgsOnlyAcceptedCandidate(ctx, patch, *uniqueRewrite, *bEnv);
  AttachArgsOnlyProofCarrier(ctx, patch, /*wholeEnvelopeReplayValidated=*/true);
  return patch;
}


std::optional<MacroPatch>
RefoldMacroPatchPlanner::TryDefinitionTapeReplayArgsOnlyPatch(
    const ArgsOnlyPlanningContext &ctx) const {
  const RefoldModel::MacroInvocation &m = ctx.invocation;
  const diffutils::Hunk &h = ctx.hunk;
  StringRef baseInvText = ctx.baseInvocationText;
  ArrayRef<std::pair<size_t, size_t>> invArgRanges =
      ctx.actualLayout.rangePairs();
  const ArgsOnlyTemplateReplayContext argsOnlyTemplateCtx{m, baseInvText,
                                                          invArgRanges};

  const RefoldModel::MacroDirective *definition =
      GetDefinitionDirectiveForInvocation(m);
  if (!definition || definition->subkind != "#define" ||
      !definition->functionLike || definition->name != m.name ||
      definition->defParams.size() != m.defParams.size() ||
      definition->replacementTokens.empty() || !m.cover.IsValid() ||
      !m.stringifySpans.empty() || !m.pasteSpans.empty())
    return std::nullopt;

  auto cover = GetWholeCoverATokRange(m);
  if (!cover || cover->first >= cover->second)
    return std::nullopt;

  // Use the file-scope replacement-list node carrier so the replay solver's
  // semantic helpers can be lifted without first reintroducing local types.
  using ReplayElem = DefinitionTapeReplayElem;

  // Parse the producer's replacement-token tape into ReplayElem nodes.
  // Stringification and token-paste are rejected here because they transform
  // argument spelling before it reaches the PP output; those cases require
  // the dedicated stringify/paste proof paths rather than raw token replay.
  std::function<bool(size_t, size_t, std::vector<ReplayElem> &)>
      parseReplayRange;
  parseReplayRange = [&](size_t begin, size_t end,
                         std::vector<ReplayElem> &out) -> bool {
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

  // Use the file-scope A-occurrence carrier so replay matching helpers have
  // a stable parameter type when they are lifted out of this function.
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
      matchAReplay;
  matchAReplay = [&](ArrayRef<ReplayElem> elems, uint64_t &cursor,
                     size_t &spanIdx, std::vector<ReplayAOcc> &occs) -> bool {
    for (const ReplayElem &elem : elems) {
      if (spanIdx < standardSpans.size() &&
          standardSpans[spanIdx].begin < cursor)
        return false;

      switch (elem.kind) {
      case ReplayElem::Kind::Literal:
        if (!MatchLiteralAToken(cursor, elem.spelling))
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
    bEnv = (*deps_.sourceMapper).MapAByteRangeToBTokenEnvelope(
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
          skippedPrefixWidth > 0 && skippedPrefixWidth <= fixedLiteralPrefix.size();

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

      for (size_t i = 0; canReanchorAtPostInsertionFrontier &&
                         i < skippedPrefixWidth; ++i) {
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
          m.id, m.name, h.aStart, h.aEnd, h.bStart, h.bEnd,
          *m.invPPByteBegin, *m.invPPByteEnd, bEnv->first, bEnv->second);
    }
  } else {
    bEnv = (*deps_.sourceMapper).MapATokRangeAToBTokenEnvelopePreserveBoundaryInsertions(
        cover->first, cover->second);
  }
  if (!bEnv || bEnv->first > bEnv->second ||
      bEnv->second > deps_.bToks.size())
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
          (*deps_.sourceMapper).SliceBSource(sol.ranges[argIdx].first, sol.ranges[argIdx].second)
              .trim();
      StringRef newText = (*deps_.sourceMapper).SliceBSource(range.first, range.second).trim();
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
      const bool variadicFormal = elem.argIdx < m.defParams.size() &&
                                  m.defParams[elem.argIdx].variadic;
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
  // keeps all valid replay witnesses, partitions them by semantic
  // definition-tape obligations, and applies canonical preference only after
  // the surviving class set is known.  This is deliberately different from a
  // score-first solver: score/spelling may pick a representative, but cannot
  // prove that two replay witnesses are equivalent.

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

  // Use the file-scope score carrier so the deterministic replay-ranking
  // comparator can be lifted without depending on a function-local type.
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
        repl = (*deps_.sourceMapper).SliceBSource(sol.ranges[i].first, sol.ranges[i].second)
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

      if (StringRef(repl).trim() ==
          baseInvText.slice(r.first, r.second).trim())
        continue;
      if (i < m.defParams.size() && !m.defParams[i].variadic &&
          !repl.empty() && replacementIntroducesTopLevelComma(repl, (*deps_.lexLang)))
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
      rewritten = stringutils::replaceRange(rewritten, edit.begin, edit.end,
                                            edit.repl);
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
          i > 0 && toks[i - 1].kind ==
                       RefoldModel::MacroReplacementTokenKind::Literal &&
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
    state.literalComma = state.nonEmpty && replacementIntroducesTopLevelComma(text, (*deps_.lexLang));
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
        assigned ? (*deps_.sourceMapper).SliceBSource(sol.ranges[idx].first, sol.ranges[idx].second)
                       .trim()
                       .str()
                 : std::string();
    const bool literalComma =
        !resultText.empty() && replacementIntroducesTopLevelComma(StringRef(resultText), (*deps_.lexLang));
    const bool vaOptResultActive = hasVaOpt && vaOptIncludedCount != 0;
    const bool vaOptOriginallyActive =
        hasVaOpt && originalVariadicState.nonEmpty;
    const bool commaInserted =
        !originalVariadicState.nonEmpty && resultNonEmpty;
    const bool commaDeleted =
        originalVariadicState.nonEmpty && !resultNonEmpty;
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

  auto makeVariadicCommaWitnessForSolution = [&](const ReplaySolution &sol,
                                                 unsigned vaOptIncludedCount)
      -> std::optional<VariadicCommaWitness> {
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
          (*deps_.sourceMapper).SliceBSource(sol.ranges[idx].first, sol.ranges[idx].second)
              .trim()
              .str();
      witness.literalCommaInActual |= replacementIntroducesTopLevelComma(StringRef(text), (*deps_.lexLang));
    }
    witness.commaInserted =
        !witness.originalNonEmpty && witness.resultNonEmpty;
    witness.commaDeleted =
        witness.originalNonEmpty && !witness.resultNonEmpty;
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
    witness.vaOptNodeCount =
        static_cast<uint32_t>(tapeProfile.vaOptNodeCount);
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
      key += ":" + variadicStateSignatureForSolution(
                       scored.sol, scored.vaOptIncludedCount);
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

      const unsigned oldN = i < oldTokenCountByFormal.size()
                                ? oldTokenCountByFormal[i]
                                : 0;
      const unsigned newN = static_cast<unsigned>(sol.ranges[i].second -
                                                  sol.ranges[i].first);
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

  if (GetProofLattice().ShouldEmitProofLog()) {
    GetProofLattice().TraceWitnessAmbiguity("MacroActualDefinitionTapeReplay",
                          solutions.size(), validSolutions.size(),
                          equivalenceClasses.size());
  }

  // makes the variadic definition-tape partition authoritative when
  // the refined variadic/VA_OPT key leaves exactly one semantic class.  If a
  // variadic replay still exposes multiple non-equivalent classes, keep the
  // legacy deterministic representative for now rather than guessing through
  // this local gate; the global resolver decides how such cross-class
  // fallbacks compose with weaker proof families.
  const bool definitionTapeEquivalenceAuthoritative =
      (!hasVariadicFormal && !hasVaOpt) || equivalenceClasses.size() == 1;
  if (definitionTapeEquivalenceAuthoritative &&
      equivalenceClasses.size() != 1) {
    if (GetProofLattice().ShouldEmitProofLog()) {
      RefoldWitness witness;
      witness.family = WitnessProofFamily::DefinitionTapeReplay;
      witness.owner = llvm::formatv("macro#{0}", m.id).str();
      witness.detail = llvm::formatv(
                           "definition={0} b=[{1},{2}) classes={3}",
                           definition->id, bEnv->first, bEnv->second,
                           equivalenceClasses.size())
                           .str();
      GetProofLattice().TraceWitnessRejected(
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
  // proof range remains the mapped expansion envelope stamped below.
  patch.materializedOutputByteStart = 0;
  patch.materializedOutputByteEnd = patch.replacement.size();
  patch.hasMaterializedOutputByteRange = true;
  stampMacroPatchMaterializedBTokenRange(
      patch, static_cast<uint64_t>(bEnv->first),
      static_cast<uint64_t>(bEnv->second));
  AttachArgsOnlyProofCarrier(argsOnlyTemplateCtx, patch,
                               /*wholeEnvelopeReplayValidated=*/true,
                               /*definitionTapeReplayValidated=*/true);
  if (auto variadicWitness = makeVariadicCommaWitnessForSolution(
          best->sol, static_cast<unsigned>(best->vaOptIncludedCount))) {
    MacroPatchProof proof = patch.proof;
    proof.variadicCommaReplay = std::move(*variadicWitness);
    GetProofLattice().SetMacroPatchProof(patch, std::move(proof));
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
    GetProofLattice().SetMacroPatchProof(patch, std::move(proof));
  }
  return patch;

}

RefoldMacroPatchPlanner::ArgsOnlyPatchAttempt
RefoldMacroPatchPlanner::BuildPasteAwareArgsOnlyPatch(
    const ArgsOnlyPlanningContext &ctx) const {
  const RefoldModel::MacroInvocation &m = ctx.invocation;
  const diffutils::Hunk &h = ctx.hunk;
  StringRef baseInvText = ctx.baseInvocationText;
  ArrayRef<std::pair<size_t, size_t>> invArgRanges =
      ctx.actualLayout.rangePairs();
  const diffutils::Hunk tokenHunksCurrent[] = {h};
  const InvocationActualRecoveryContext actualRecoveryCtx{m, baseInvText,
                                                           invArgRanges};

  // Fast path for token-paste edits. A single pasted token can embed multiple
  // argument contributions (e.g., X##_##Y##_##Z), so a single edit hunk may
  // change multiple arg segments inside that token (e.g., a_b_c -> d_e_f). In
  // that case we attempt to derive per-arg segment replacements and splice them
  // into the invocation spelling.
  if (!HunkTouchesAnyPasteToken(m, h))
    return ArgsOnlyPatchAttempt::ContinueSearchResult();

  auto edits = DerivePasteArgEdits(m, h);
  if (edits && !edits->empty()) {
    DenseMap<uint32_t, std::string> replByArgIdx;
    for (const auto &pae : *edits) {
      uint32_t argIdx = pae.argIdx;
      if (static_cast<size_t>(argIdx) >= invArgRanges.size())
        return ArgsOnlyPatchAttempt::RejectResult();

      // A single argument may contribute multiple segments to the same
      // pasted token (e.g. X##_..._##X). We merge repeated argIdx
      // conservatively after deriving the candidate replacement below.
      auto range = invArgRanges[argIdx];
      StringRef baseArgText =
          baseInvText.substr(range.first, range.second - range.first);

      // Prefer an exact source-slice splice when the replay witness
      // provides one. Otherwise retain the existing conservative boundary
      // splice for legacy paste-span metadata.
      std::string newArg =
          (pae.argByteBegin && pae.argByteEnd)
              ? RefoldMacroPasteSpelling::SplicePasteSegmentIntoSpellingArgExact(
                    baseArgText, *pae.argByteBegin, *pae.argByteEnd,
                    pae.oldSeg, pae.newSeg)
              : RefoldMacroPasteSpelling::SplicePasteSegmentIntoSpellingArg(baseArgText, pae.oldSeg,
                                                  pae.newSeg);
      if (newArg.empty()) {
        // Deleting an entire argument (making it empty) is legal. Accept this
        // only when the paste-span covered the whole argument spelling.
        if (!(StringRef(pae.newSeg).trim().empty() &&
              baseArgText.trim() == StringRef(pae.oldSeg).trim()))
          return ArgsOnlyPatchAttempt::RejectResult();
      }

      // If the argument is not the variadic formal, replacing it with a
      // text that introduces a top-level comma would change the macro
      // invocation's argument list.
      if (!isMacroInvocationVariadicFormal(m, argIdx) && replacementIntroducesTopLevelComma(newArg, (*deps_.lexLang)))
        return ArgsOnlyPatchAttempt::RejectResult();

      auto existing = replByArgIdx.find(argIdx);
      if (existing != replByArgIdx.end()) {
        if (existing->second != newArg)
          return ArgsOnlyPatchAttempt::RejectResult();
        continue;
      }

      // Per-arg safety gate: validate standard + stringify occurrences for
      // this arg.
      //
      // NOTE: For multi-span paste edits where the pasted token length may
      // change, per-arg paste-span validation cannot be done reliably in
      // isolation. We validate paste tokens as a *group* below via
      // pasteArgReplacementsMatchAllPasteTokensInB(...).
      if (!MacroArgReplacementMatchesAllOccurrencesInBIgnorePaste(
              m, argIdx, baseArgText, newArg, tokenHunksCurrent)) {
        return ArgsOnlyPatchAttempt::RejectResult();
      }

      replByArgIdx[argIdx] = std::move(newArg);
    }

    if (!replByArgIdx.empty()) {
      // Combined safety gate: applying all derived replacements must
      // reconstruct every pasted token occurrence exactly as seen in B.
      if (!PasteArgReplacementsMatchAllPasteTokensInB(
              m, baseInvText, invArgRanges, replByArgIdx)) {
        return ArgsOnlyPatchAttempt::RejectResult();
      }

      std::optional<InvocationRewriteWithRange> rewrite =
          BuildInvocationRewriteWithRange(actualRecoveryCtx, replByArgIdx);
      if (!rewrite)
        return ArgsOnlyPatchAttempt::RejectResult();

      {
        MacroPatch patch{*m.invB, *m.invE, std::move(rewrite->text), m.id};
        StampInvocationRewriteMaterializedOutputRange(patch, *rewrite);
        // Paste replay validates the rewritten callsite against every pasted
        // token occurrence in the expansion.  For the edit map, therefore,
        // the B-side materialization is the whole expansion cover that the
        // source argument rewrite regenerates, not merely the first changed
        // pasted-token hunk.
        StampMacroPatchWholeExpansionBRange(m, patch);
        GetProofLattice().SetMacroPatchProof(
            patch,
            GetProofLattice().MakeMacroPatchProof(MacroPatchProofKind::ArgsOnlyPasteMulti,
                                /*preservesInvocationStructure=*/true, m.id));
        // The builder already proved this rewrite by replaying the rewritten
        // invocation arguments against every pasted token occurrence in B.
        // Carry that proof source onto the accepted patch for converted
        // selector-site discharge.
        patch.pasteReplayValidated = true;
        GetProofLattice().SyncMacroPatchProofSummary(patch);
        return ArgsOnlyPatchAttempt::AcceptedResult(std::move(patch));
      }
    }
  }

  // Single-segment paste edit (existing behavior)
  //
  // This handles the common case where only one pasted segment changes (e.g.
  // X##_##Y, changing just X). The multi-span derivation above requires token
  // lengths to remain stable; when they do not, we fall back to deriving a
  // single segment edit from the token-level diff.
  auto pae = DerivePasteArgEdit(m, h);
  if (pae) {
    uint32_t argIdx = pae->argIdx;

    // HARD FAILURE: If we derived a paste edit but the index is invalid,
    // we must exit, not fall through.
    if (static_cast<size_t>(argIdx) >= invArgRanges.size())
      return ArgsOnlyPatchAttempt::RejectResult();

    auto r = invArgRanges[argIdx];
    StringRef baseArgText = baseInvText.substr(r.first, r.second - r.first);
    std::string newArg =
        (pae->argByteBegin && pae->argByteEnd)
            ? RefoldMacroPasteSpelling::SplicePasteSegmentIntoSpellingArgExact(
                  baseArgText, *pae->argByteBegin, *pae->argByteEnd,
                  pae->oldSeg, pae->newSeg)
            : RefoldMacroPasteSpelling::SplicePasteSegmentIntoSpellingArg(baseArgText, pae->oldSeg,
                                                pae->newSeg);
    if (newArg.empty()) {
      // Deleting an entire argument (making it empty) is legal. Accept this
      // only when the paste-span covered the whole argument spelling.
      if (!(StringRef(pae->newSeg).trim().empty() &&
            baseArgText.trim() == StringRef(pae->oldSeg).trim()))
        return ArgsOnlyPatchAttempt::RejectResult();
    }

    if (!isMacroInvocationVariadicFormal(m, argIdx) && replacementIntroducesTopLevelComma(newArg, (*deps_.lexLang)))
      return ArgsOnlyPatchAttempt::RejectResult();

    // Safety gate: for single-segment paste edits we can directly validate
    // all occurrences, including paste-span occurrences, against the B
    // stream.
    if (!MacroArgReplacementMatchesAllOccurrencesInB(
            m, argIdx, baseArgText, newArg, tokenHunksCurrent)) {
      return ArgsOnlyPatchAttempt::RejectResult();
    }

    DenseMap<uint32_t, std::string> singleReplByArgIdx;
    singleReplByArgIdx[argIdx] = std::move(newArg);
    std::optional<InvocationRewriteWithRange> rewrite =
        BuildInvocationRewriteWithRange(actualRecoveryCtx,
                                        singleReplByArgIdx);
    if (!rewrite)
      return ArgsOnlyPatchAttempt::RejectResult();

    {
      MacroPatch patch{*m.invB, *m.invE, std::move(rewrite->text), m.id};
      StampInvocationRewriteMaterializedOutputRange(patch, *rewrite);
      // As with the multi-paste path, the source argument rewrite is a
      // compact representation of the macro's replayed expansion surface.
      // Keep the B-side map anchored to that whole expansion envelope.
      StampMacroPatchWholeExpansionBRange(m, patch);
      GetProofLattice().SetMacroPatchProof(
          patch,
          GetProofLattice().MakeMacroPatchProof(MacroPatchProofKind::ArgsOnlyPasteSingle,
                              /*preservesInvocationStructure=*/true, m.id));
      // Single-segment paste rewrites are admitted only after direct replay
      // validation against all touched occurrences in B. Record that proof
      // source explicitly for converted selector-site discharge.
      patch.pasteReplayValidated = true;
      GetProofLattice().SyncMacroPatchProofSummary(patch);
      return ArgsOnlyPatchAttempt::AcceptedResult(std::move(patch));
    }
  }

  // If we touched paste but could not safely derive a paste splice patch,
  // fall through to the standard (non-paste) args-only policy below.

  return ArgsOnlyPatchAttempt::ContinueSearchResult();
}

std::optional<MacroPatch>
RefoldMacroPatchPlanner::BuildStandardArgsOnlyPatch(
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
  // from the B slices.
  // The immutable producer #define is resolved through a named planner helper.
  bool replayedStandardArgSpanFormalIndices = false;
  auto getDefinitionReplayedStandardArgSpans = [&]() {
    std::vector<RefoldModel::PPArgSpan> out = m.argSpans;

    // Producer arg indices can be ambiguous when an actual contains a comma
    // that is not protected by parentheses, e.g. `M(arr[1, 2], 3)`.  Clang's
    // source range for the first written argument may cover the bracketed text,
    // while macro replacement still substitutes the comma-separated pieces into
    // successive formals.  Repair only the fully provable case: the definition
    // replacement-list tape and the recorded standard spans must replay the
    // macro's complete A-side cover exactly, with one non-empty standard span
    // per replacement-list parameter reference.
    const RefoldModel::MacroDirective *definition = GetDefinitionDirectiveForInvocation(m);
    if (!definition || definition->subkind != "#define" ||
        !definition->functionLike || definition->name != m.name ||
        definition->defParams.size() != m.defParams.size() || out.empty() ||
        !m.stringifySpans.empty() || !m.pasteSpans.empty() ||
        !m.cover.IsValid() || m.cover.end > deps_.aToks.size())
      return out;

    // Extract the formal-reference order from the macro replacement-list tape.
    // Stringify and paste are excluded because their spelling/segmentation
    // rules are not ordinary standard-argument substitution.
    SmallVector<uint32_t, 8> formalSeq;
    formalSeq.reserve(definition->replacementTokens.size());
    for (const RefoldModel::MacroReplacementToken &token :
         definition->replacementTokens) {
      if (token.spelling == "#" || token.spelling == "##")
        return out;
      if (token.kind != RefoldModel::MacroReplacementTokenKind::ParamRef)
        continue;
      if (!token.paramIndex || *token.paramIndex >= m.defParams.size())
        return out;
      formalSeq.push_back(*token.paramIndex);
    }

    if (formalSeq.size() != out.size())
      return out;

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
        if (tok >= m.cover.end || tok >= deps_.aToks.size() ||
            deps_.aToks[static_cast<size_t>(tok)].spelling != repTok.spelling)
          return m.argSpans;
        ++tok;
        break;
      case RefoldModel::MacroReplacementTokenKind::ParamRef: {
        if (argSpanIdx >= out.size())
          return m.argSpans;
        const RefoldModel::PPArgSpan &sp = out[argSpanIdx];
        if (sp.kind != PPArgSpanKind::Standard || sp.begin != tok ||
            sp.begin >= sp.end || sp.end > m.cover.end ||
            sp.end > deps_.aToks.size())
          return m.argSpans;
        tok = sp.end;
        ++argSpanIdx;
        break;
      }
      }
    }

    if (tok != m.cover.end || argSpanIdx != out.size())
      return m.argSpans;

    bool changed = false;
    for (size_t i = 0; i < out.size(); ++i) {
      if (out[i].argIdx != formalSeq[i])
        changed = true;
      out[i].argIdx = formalSeq[i];
    }
    if (changed) {
      replayedStandardArgSpanFormalIndices = true;
    }
    return out;
  };

  // Prefer the direct current-level invocation parse; fall back to definition
  // replay only for older producer shapes where the replacement-list tape
  // proves the same reindexing.
  std::vector<RefoldModel::PPArgSpan> standardArgSpans;
  if (auto currentLevelSpans = GetCurrentLevelStandardArgSpans(argsOnlyTemplateCtx)) {
    standardArgSpans = std::move(*currentLevelSpans);
  } else {
    standardArgSpans = getDefinitionReplayedStandardArgSpans();
  }

  std::vector<RefoldModel::PPArgSpan> occs;
  append_range(occs, standardArgSpans);
  append_range(occs, m.stringifySpans);

  std::vector<char> occIsStringify;
  occIsStringify.resize(occs.size());
  std::fill_n(occIsStringify.begin(), standardArgSpans.size(), false);
  std::fill_n(occIsStringify.begin() + standardArgSpans.size(),
              m.stringifySpans.size(), true);

  // Pure paste-only invocations have no STANDARD or STRINGIFY evidence, so the
  // normal args-only path bottoms out at occs.empty(). They are still
  // invertible when the touched pasted token can be segmented back into unique
  // per-argument replacements and those replacements reconstruct every pasted
  // token occurrence in B.
  if (occs.empty() && !m.pasteSpans.empty()) {

    // First derive per-argument paste edits from the current hunk. The
    // derivation proves that the edited pasted-token spelling can be
    // mapped back to argument segments rather than arbitrary token substrings.
    auto edits = DerivePasteArgEdits(m, hArgs);
    if (!edits || edits->empty()) {
      return std::nullopt;
    }

    // Merge all derived paste edits into one replacement spelling per
    // invocation argument. Multiple pasted-token occurrences may refer to the
    // same formal, but they must all demand the same final argument spelling.
    DenseMap<uint32_t, std::string> replByArgIdx;
    for (const auto &pae : *edits) {
      const uint32_t argIdx = pae.argIdx;
      if (static_cast<size_t>(argIdx) >= invArgRanges.size()) {
        return std::nullopt;
      }

      // Reconstruct the full invocation-argument spelling by replacing the
      // derived old paste segment with the derived new paste segment. Prefer
      // the exact byte-window splice when the paste witness identifies the
      // segment boundaries inside the argument.
      auto range = invArgRanges[argIdx];
      StringRef baseArgText =
          baseInvText.substr(range.first, range.second - range.first);
      std::string newArg = (pae.argByteBegin && pae.argByteEnd)
                               ? RefoldMacroPasteSpelling::SplicePasteSegmentIntoSpellingArgExact(
                                     baseArgText, *pae.argByteBegin,
                                     *pae.argByteEnd, pae.oldSeg, pae.newSeg)
                               : RefoldMacroPasteSpelling::SplicePasteSegmentIntoSpellingArg(
                                     baseArgText, pae.oldSeg, pae.newSeg);

      // An empty splice result normally means the old segment could not be
      // found or replaced safely. The one accepted empty-result case is a true
      // no-op where the new segment is empty and the original argument was
      // exactly the old segment after trimming.
      if (newArg.empty()) {
        if (!(StringRef(pae.newSeg).trim().empty() &&
              baseArgText.trim() == StringRef(pae.oldSeg).trim())) {
          return std::nullopt;
        }
      }

      // A non-variadic macro formal cannot be rewritten to text containing a
      // top-level comma, because that would change call-site arity.
      if (!isMacroInvocationVariadicFormal(m, argIdx) && replacementIntroducesTopLevelComma(newArg, (*deps_.lexLang))) {
        return std::nullopt;
      }

      // If the same formal was observed through multiple pasted tokens, require
      // every occurrence to reconstruct the exact same replacement argument.
      auto existing = replByArgIdx.find(argIdx);
      if (existing != replByArgIdx.end()) {
        if (existing->second != newArg) {
          return std::nullopt;
        }
        continue;
      }

      replByArgIdx[argIdx] = std::move(newArg);
    }

    // No argument changed after merging, so there is no invocation rewrite to
    // propose from this fallback.
    if (replByArgIdx.empty()) {
      return std::nullopt;
    }

    // Validate the merged argument replacements globally against every pasted
    // token occurrence in B. This prevents accepting a rewrite that explains
    // only the touched token while breaking another paste occurrence from the
    // same invocation.
    if (!PasteArgReplacementsMatchAllPasteTokensInB(
            m, baseInvText, invArgRanges, replByArgIdx)) {
      return std::nullopt;
    }

    std::optional<InvocationRewriteWithRange> rewrite =
        BuildInvocationRewriteWithRange(actualRecoveryCtx, replByArgIdx);
    if (!rewrite)
      return std::nullopt;

    {
      MacroPatch patch{*m.invB, *m.invE, std::move(rewrite->text), m.id};
      StampInvocationRewriteMaterializedOutputRange(patch, *rewrite);
      // Pure-paste-only replay has no standard/stringify occurrence to define
      // a smaller B-side surface.  The proved replay unit is the full expansion
      // cover reconstructed from the rewritten invocation arguments.
      StampMacroPatchWholeExpansionBRange(m, patch);
      GetProofLattice().SetMacroPatchProof(
          patch,
          GetProofLattice().MakeMacroPatchProof(MacroPatchProofKind::ArgsOnlyPurePasteOnly,
                              /*preservesInvocationStructure=*/true, m.id));
      // Pure-paste-only rewrites have no standard or stringify occurrences to
      // lean on, so successful all-paste replay is the decisive proof source.
      patch.pasteReplayValidated = true;
      GetProofLattice().SyncMacroPatchProofSummary(patch);
      return patch;
    }
  }

  // Higher-order generated-callee replay for ordinary macro actual slots.
  //
  // The tuple-specific proof below handles wrappers such as `WRAP((STR, x))`,
  // where the source slots to repair are tuple elements inside one formal.  The
  // same owner-level invariant also applies when those slots are ordinary
  // invocation actuals:
  //
  //   #define APPLY(F, G, X) F(G, X)
  //   #define FWD(G, X) G(X)
  //   #define STR(x) #x
  //   APPLY(FWD, STR, alpha)
  //
  // The root expansion surface belongs to APPLY, but the edited token may be
  // exposed only after following one or more generated calls (`APPLY -> FWD ->
  // STR`).  This proof follows that generated-call chain through replacement
  // lists, replays the final callee replacement-list as a token transducer, and
  // maps the solved final callee actuals back to the original APPLY actual
  // ranges.  It is owner-polymorphic with respect to the source slots: no tuple
  // syntax is assumed, and every accepted edit is positional rather than
  // text-keyed.
  {
    // Higher-order generated-callee replay keeps the fail-closed discovery
    // order, then hands the explicit 6B context to the named builder.
    if (m.definitionDirectiveId && m.invB && m.invE &&
        m.stringifySpans.empty() && m.pasteSpans.empty()) {
      auto cover = GetWholeCoverATokRange(m);
      if (cover && cover->first < cover->second) {
        auto bEnv = (*deps_.sourceMapper).MapATokRangeAToBTokenEnvelope(
            cover->first, cover->second);
        if (!bEnv || bEnv->first >= bEnv->second)
          bEnv = (*deps_.sourceMapper)
                     .MapATokRangeAToBTokenEnvelopePreserveBoundaryInsertions(
                         cover->first, cover->second);
        if (bEnv && bEnv->first < bEnv->second) {
          const RefoldModel::MacroDirective *rootDefinition = nullptr;
          for (const RefoldModel::MacroDirective &directive :
               (*deps_.model).GetMacroDirectives()) {
            if (directive.id == *m.definitionDirectiveId) {
              rootDefinition = &directive;
              break;
            }
          }
          if (rootDefinition && rootDefinition->subkind == "#define" &&
              rootDefinition->functionLike && !rootDefinition->defParams.empty()) {
            SmallVector<GeneratedCalleeSourceSlot, 8> currentActuals;
            currentActuals.reserve(invArgRanges.size());
            bool actualsRecoverable = true;
            for (uint32_t i = 0; i < invArgRanges.size(); ++i) {
              const auto r = invArgRanges[i];
              if (r.second < r.first || r.second > baseInvText.size()) {
                actualsRecoverable = false;
                break;
              }
              GeneratedCalleeSourceSlot slot;
              slot.text = baseInvText.slice(r.first, r.second).trim().str();
              slot.rootSourceText = slot.text;
              slot.rootArgIdx = i;
              currentActuals.push_back(std::move(slot));
            }

            if (actualsRecoverable &&
                macroDefinitionAcceptsActualCount(*rootDefinition,
                                                  currentActuals.size())) {
              SmallVector<std::string, 8> replayPrefixLiterals;
              SmallVector<SmallVector<std::string, 8>, 8> replaySuffixStack;
              const RefoldModel::MacroDirective *currentDefinition =
                  rootDefinition;
              bool followedGeneratedCall = false;
              uint32_t generatedCallDepth = 0;
              uint32_t objectAliasHopCount = 0;
              bool generatedReplayUsesStringification = false;
              bool generatedReplayUsesPaste = false;
              bool generatedReplayUsesVariadicForwarding = false;

              GeneratedCalleeReplayContext generatedCalleeCtx{
                  m,
                  baseInvText,
                  invArgRanges,
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

              if (auto higherOrderGeneratedPatch =
                      BuildGeneratedCalleeReplayCandidate(generatedCalleeCtx))
                return higherOrderGeneratedPatch;
            }
          }
        }
      }
    }
  }

  // Last-resort higher-order leaf replay for generated-call owners whose
  // argument expressions have not yet been lowered into the full transducer
  // graph above.  This is still a proof, not a preference: it is considered
  // only for a root macro whose replacement list generates a call through a
  // formal callee, and it accepts only a unique old->new leaf substitution that
  // can be mapped back into exactly one root invocation argument.  This covers
  // owner surfaces such as `F(G, A, B) -> G(A ## B)`, `G(#X)`, and repeated
  // generated calls `G(X) G(X)` without choosing among ambiguous edits.
  {
    // Generated-leaf replay keeps the old discovery gates in place and delegates
    // only the solved replay/admission body to the named helper.
    if (m.definitionDirectiveId && m.invB && m.invE) {
      const RefoldModel::MacroDirective *rootDefinition = nullptr;
      for (const RefoldModel::MacroDirective &directive :
           (*deps_.model).GetMacroDirectives()) {
        if (directive.id == *m.definitionDirectiveId) {
          rootDefinition = &directive;
          break;
        }
      }
      if (rootDefinition && rootDefinition->subkind == "#define" &&
          rootDefinition->functionLike && !rootDefinition->defParams.empty()) {
        bool hasGeneratedCall = false;
        const auto &rootToks = rootDefinition->replacementTokens;
        for (size_t i = 0; i + 1 < rootToks.size(); ++i) {
          if (rootToks[i].kind ==
                  RefoldModel::MacroReplacementTokenKind::ParamRef &&
              rootToks[i].paramIndex &&
              rootToks[i + 1].kind ==
                  RefoldModel::MacroReplacementTokenKind::Literal &&
              rootToks[i + 1].spelling == "(") {
            hasGeneratedCall = true;
            break;
          }
        }

        auto cover = GetWholeCoverATokRange(m);
        if (hasGeneratedCall && cover && cover->first < cover->second) {
          auto bEnv = (*deps_.sourceMapper).MapATokRangeAToBTokenEnvelope(
              cover->first, cover->second);
          if (!bEnv || bEnv->first >= bEnv->second)
            bEnv = (*deps_.sourceMapper)
                       .MapATokRangeAToBTokenEnvelopePreserveBoundaryInsertions(
                           cover->first, cover->second);
          if (bEnv && bEnv->first < bEnv->second) {
            StringRef oldExpansion =
                (*deps_.sourceMapper).SliceASource(cover->first, cover->second)
                    .trim();
            StringRef newExpansion =
                (*deps_.sourceMapper).SliceBSource(bEnv->first, bEnv->second)
                    .trim();

            if (oldExpansion == newExpansion && h.isInsertOnly() &&
                h.aStart == cover->second && h.bStart == bEnv->second &&
                h.bStart < h.bEnd) {
              bEnv->second = static_cast<size_t>(h.bEnd);
              newExpansion = (*deps_.sourceMapper)
                                 .SliceBSource(bEnv->first, bEnv->second)
                                 .trim();
            }

            bool rootHasGeneratedSelectorDescendant = false;
            for (const RefoldModel::MacroInvocation &candidate :
                 (*deps_.model).GetMacroInvocations()) {
              const RefoldModel::MacroInvocation *cur = &candidate;
              bool isDescendant = false;
              for (size_t depth = 0;
                   cur && depth <= (*deps_.model).GetMacroInvocations().size();
                   ++depth) {
                if (cur->id == m.id) {
                  isDescendant = true;
                  break;
                }
                if (!cur->callerMacroId)
                  break;
                cur = (*deps_.macroTopology)
                          .FindMacroInvocationById(*cur->callerMacroId);
              }
              if (!isDescendant)
                continue;
              if (candidate.calleeOrigin.kind ==
                      MacroCalleeOriginKind::CallerParam &&
                  !candidate.calleeOrigin.callerParamIndices.empty()) {
                rootHasGeneratedSelectorDescendant = true;
                break;
              }
            }

            if (h.isInsertOnly() && rootHasGeneratedSelectorDescendant &&
                (h.aStart == cover->first || h.aStart == cover->second)) {
              while (bEnv->first > 0 &&
                     bEnv->first - 1 <
                         deps_.bInsertionLedger->BTokToInsertionId().size()) {
                int32_t insId =
                    deps_.bInsertionLedger->BTokToInsertionId()[bEnv->first - 1];
                if (insId < 0)
                  break;
                const BInsertionProv &ins =
                    deps_.bInsertionLedger
                        ->Insertions()[static_cast<size_t>(insId)];
                if (ins.claim == BInsertionClaim::Standalone ||
                    ins.aGap != cover->first || ins.b1 != bEnv->first)
                  break;
                bEnv->first = ins.b0;
              }
              while (bEnv->second <
                     deps_.bInsertionLedger->BTokToInsertionId().size()) {
                int32_t insId =
                    deps_.bInsertionLedger->BTokToInsertionId()[bEnv->second];
                if (insId < 0)
                  break;
                const BInsertionProv &ins =
                    deps_.bInsertionLedger
                        ->Insertions()[static_cast<size_t>(insId)];
                if (ins.claim == BInsertionClaim::Standalone ||
                    ins.aGap != cover->second || ins.b0 != bEnv->second)
                  break;
                bEnv->second = ins.b1;
              }
              newExpansion = (*deps_.sourceMapper)
                                 .SliceBSource(bEnv->first, bEnv->second)
                                 .trim();
            }

            if (!oldExpansion.empty() && !newExpansion.empty() &&
                oldExpansion != newExpansion) {
              GeneratedLeafReplayContext generatedLeafCtx{
                  m, h, baseInvText, invArgRanges, *cover, *bEnv,
                  *rootDefinition, oldExpansion, newExpansion};
              if (auto higherOrderLeafPatch =
                      BuildGeneratedLeafReplayCandidate(generatedLeafCtx))
                return higherOrderLeafPatch;
            }
          }
        }
      }
    }
  }

  // Some tuple-generated callees expose the edited token only through a
  // nested child macro surface, so the root wrapper can have no standard
  // PPArgSpan/stringify occurrence at all.  That happens for shapes such as
  //
  //   #define WRAP(PAIR) CALL PAIR
  //   #define CALL(F, X) F(X)
  //   #define STR(x) #x
  //   WRAP((STR, alpha))
  //
  // where the root WRAP cover is the string literal produced by STR, not a
  // direct argument span of WRAP.  Try a narrow positional tuple proof before
  // giving up on an empty occurrence set: the wrapper must forward exactly one
  // tuple formal into a generated callee call, and the callee replacement list
  // must be invertible for the observed whole-cover A/B expansion.
  {
    // Tuple-generated callee replay remains after ordinary generated-leaf replay
    // in the same ranking position; only the body is now a named helper.
    if (m.definitionDirectiveId && m.invB && m.invE &&
        m.stringifySpans.empty() && m.pasteSpans.empty()) {
      auto cover = GetWholeCoverATokRange(m);
      if (cover && cover->first < cover->second) {
        auto bEnv = (*deps_.sourceMapper).MapATokRangeAToBTokenEnvelope(
            cover->first, cover->second);
        if (!bEnv || bEnv->first >= bEnv->second)
          bEnv = (*deps_.sourceMapper)
                     .MapATokRangeAToBTokenEnvelopePreserveBoundaryInsertions(
                         cover->first, cover->second);
        if (bEnv && bEnv->first < bEnv->second) {
          const RefoldModel::MacroDirective *rootDefinition = nullptr;
          for (const RefoldModel::MacroDirective &directive :
               (*deps_.model).GetMacroDirectives()) {
            if (directive.id == *m.definitionDirectiveId) {
              rootDefinition = &directive;
              break;
            }
          }
          if (rootDefinition && rootDefinition->subkind == "#define" &&
              rootDefinition->functionLike &&
              rootDefinition->replacementTokens.size() == 2) {
            const auto &rootTok0 = rootDefinition->replacementTokens[0];
            const auto &rootTok1 = rootDefinition->replacementTokens[1];
            if (rootTok0.kind == RefoldModel::MacroReplacementTokenKind::Literal &&
                rootTok1.kind == RefoldModel::MacroReplacementTokenKind::ParamRef &&
                rootTok1.paramIndex && *rootTok1.paramIndex < invArgRanges.size()) {
              const uint32_t callerArgIdx = *rootTok1.paramIndex;
              uint32_t tupleObjectAliasHopCount = 0;
              uint32_t forwarderAliasHops = 0;
              const RefoldModel::MacroDirective *forwarderDefinition =
                  ResolveFunctionLikeMacroThroughAliasesWithHops(
                      rootTok0.spelling, &forwarderAliasHops);
              tupleObjectAliasHopCount += forwarderAliasHops;
              if (forwarderDefinition && !forwarderDefinition->defParams.empty()) {
                TupleGeneratedCalleeReplayContext tupleGeneratedCtx{
                    m, baseInvText, invArgRanges, *cover, *bEnv,
                    *rootDefinition, *forwarderDefinition, callerArgIdx,
                    tupleObjectAliasHopCount};
                if (auto tupleGeneratedPatch =
                        BuildTupleGeneratedCalleeReplayCandidate(tupleGeneratedCtx))
                  return tupleGeneratedPatch;
              }
            }
          }
        }
      }
    }
  }

  if (occs.empty())
    return std::nullopt;

  std::vector<char> touchedOcc(occs.size(), 0);
  if (!(*deps_.sourceMapper).HunkFullyWithinArgSpans(hArgs, occs, touchedOcc)) {
    return std::nullopt;
  }

  // Convert touched occurrence spans into touched formal arguments. The
  // args-only path is valid only for edits fully contained inside recorded
  // argument occurrences; anything outside those spans must fail closed.
  unsigned occFormalCount = static_cast<unsigned>(invArgRanges.size());
  for (const auto &sp : occs)
    occFormalCount = std::max(occFormalCount, (unsigned)sp.argIdx + 1);

  std::vector<char> touched(occFormalCount, 0);
  for (size_t i = 0; i < occs.size(); ++i) {
    if (!touchedOcc[i])
      continue;
    const auto &sp = occs[i];
    if (sp.argIdx >= touched.size())
      return std::nullopt;
    touched[sp.argIdx] = 1;
  }

  // `touched` is now indexed by formal argument, not occurrence. Later checks
  // use it to decide which invocation arguments need replacement and which must
  // remain unchanged.

  auto hunkTouchesFormalOccurrence =
      [&](const diffutils::Hunk &cand,
          const RefoldModel::PPArgSpan &sp) -> bool {
    // For replacements/deletions, touching is ordinary A-range overlap. For
    // pure insertions, the hunk has no A width, so require the existing
    // argument-span ownership helper to prove that the B insertion belongs to
    // this occurrence.
    if (cand.aStart == cand.aEnd) {
      auto bEnv = (*deps_.sourceMapper).MapAToBTokenEnvelopeByPPArgSpan(sp);
      if (!bEnv)
        return false;
      return OccurrenceReplay().GetOwnedPureInsertionBRangeForArgSpan(sp, occs, *bEnv, cand)
          .has_value();
    }

    return cand.aStart < sp.end && cand.aEnd > sp.begin;
  };

  // A provenance-only LCS can split one logical macro-argument rewrite into
  // several pure-insertion islands around repeated punctuation. When the
  // current seed is a pure insertion, widen the set of touched formals to every
  // occurrence in this same invocation cover that is independently touched by
  // another token hunk. This does not move hunk boundaries and does not inspect
  // neighboring token spellings; it only lets the existing replay validator see
  // the complete split edit before deciding whether an args-only rewrite is
  // actually proven.
  if (hArgs.aStart == hArgs.aEnd && hArgs.bStart < hArgs.bEnd) {
    for (const diffutils::Hunk &cand : (*deps_.abTokHunks)) {
      if (cand.aStart < m.cover.begin || cand.aEnd > m.cover.end)
        continue;
      if (cand.bStart >= cand.bEnd)
        continue;

      for (const auto &sp : occs) {
        if (sp.argIdx >= touched.size())
          return std::nullopt;
        if (hunkTouchesFormalOccurrence(cand, sp))
          touched[sp.argIdx] = 1;
      }
    }
  }

  auto hunkTouchesTouchedFormal = [&](const diffutils::Hunk &cand) -> bool {
    // Keep only hunks that intersect an occurrence of a formal already touched
    // by the primary args-only hunk. This lets the rewrite validate all edits
    // to the same formal argument, not just the hunk that triggered this path.
    for (const auto &sp : occs) {
      if (sp.argIdx >= touched.size() || !touched[sp.argIdx])
        continue;
      if (hunkTouchesFormalOccurrence(cand, sp))
        return true;
    }
    return false;
  };

  SmallVector<diffutils::Hunk, 8> tokenHunksForTouchedFormals;
  tokenHunksForTouchedFormals.push_back(hArgs);

  // Add sibling token hunks that also touch the same formal arguments. The
  // eventual argument replacement must explain the complete set of token edits
  // for those formals, otherwise we could accept a partial rewrite.
  for (const auto &cand : (*deps_.abTokHunks)) {
    if (cand.aStart == hArgs.aStart && cand.aEnd == hArgs.aEnd &&
        cand.bStart == hArgs.bStart && cand.bEnd == hArgs.bEnd)
      continue;
    if (!hunkTouchesTouchedFormal(cand))
      continue;
    tokenHunksForTouchedFormals.push_back(cand);
  }

  auto maybeAddSyntheticTouchedFormalEnvelope =
      [&](const RefoldModel::PPArgSpan &sp, const diffutils::Hunk &anchor) {
        // This synthesis is only for insertion-frontier hunks. Non-insertion
        // hunks already carry an A-side interval and do not need
        // reconstruction.
        if (anchor.aStart != anchor.aEnd)
          return;
        if (anchor.aStart < m.cover.begin || anchor.aEnd > m.cover.end)
          return;

        for (const auto &partner : (*deps_.abTokHunks)) {
          if (sameTokenHunk(anchor, partner))
            continue;

          // Pair the anchor only with another insertion frontier from the same
          // macro cover. Their combined envelope may reveal the true touched
          // argument span after common edge tokens are trimmed.
          if (partner.aStart != partner.aEnd)
            continue;
          if (partner.aStart < m.cover.begin || partner.aEnd > m.cover.end)
            continue;

          const diffutils::Hunk env =
              buildCombinedInsertionEnvelope(anchor, partner);
          const diffutils::Hunk envTrim = trimCommonEdgeTokens(env, deps_.aToks, deps_.bToks);

          // The trimmed synthetic envelope must expose a real A-side token
          // range.
          if (envTrim.aStart >= envTrim.aEnd)
            continue;

          // The exposed range must be fully contained in the exact occurrence
          // currently being considered.
          if (!(sp.begin <= envTrim.aStart && envTrim.aEnd <= sp.end))
            continue;

          SmallVector<char, 8> envTouched(occs.size(), 0);
          if (!(*deps_.sourceMapper).HunkFullyWithinArgSpans(envTrim, occs, envTouched))
            continue;

          bool touchesThisExactOccurrence = false;
          for (size_t occIdx = 0; occIdx < occs.size(); ++occIdx) {
            if (!envTouched[occIdx])
              continue;

            // Fail closed if the synthetic envelope touches any other formal or
            // any other occurrence. It is valid only as an explanation for
            // this exact argument occurrence.
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
          tokenHunksForTouchedFormals.push_back(envTrim);
        }
      };

  // Iterate over a snapshot of the currently known hunks. Synthetic-envelope
  // discovery may append to `tokenHunksForTouchedFormals`, so the seed copy
  // avoids recursively pairing newly synthesized hunks in the same pass.
  SmallVector<diffutils::Hunk, 8> seedTokenHunks(
      tokenHunksForTouchedFormals.begin(), tokenHunksForTouchedFormals.end());

  for (size_t occIdx = 0; occIdx < occs.size(); ++occIdx) {
    const auto &sp = occs[occIdx];
    if (sp.argIdx >= touched.size() || !touched[sp.argIdx])
      continue;

    for (const auto &cand : seedTokenHunks) {
      // Only zero-width A-side insertion frontiers can participate in synthetic
      // envelope construction. Non-insertion hunks already expose their A
      // range.
      if (cand.aStart != cand.aEnd)
        continue;
      maybeAddSyntheticTouchedFormalEnvelope(sp, cand);
    }
  }

  // Normalize the hunk set after adding synthetic envelopes. This prevents the
  // same physical edit from being observed multiple times through equivalent
  // primary/synthetic paths.
  llvm::sort(tokenHunksForTouchedFormals, tokenHunkLess);
  tokenHunksForTouchedFormals.erase(
      std::unique(tokenHunksForTouchedFormals.begin(),
                  tokenHunksForTouchedFormals.end(), sameTokenHunk),
      tokenHunksForTouchedFormals.end());

  // From this point on, treat the normalized vector as the authoritative token
  // hunk set for the touched formal arguments.
  ArrayRef<diffutils::Hunk> tokenHunks(tokenHunksForTouchedFormals);


  struct OccObservation {
    // The original spelling contributed by one occurrence of a formal argument.
    StringRef oldText;

    // The rewritten spelling inferred for that same occurrence from the token
    // hunk set.
    std::string newText;

    // Optional byte range inside newText that corresponds exactly to B-only
    // insertion payload owned by this occurrence. When absent, the whole
    // rewritten argument remains the conservative materialized output range.
    std::optional<std::pair<uint64_t, uint64_t>> materializedNewTextRange;
  };

  // Try to rebuild a caller tuple argument from occurrence observations that
  // were seen through a child macro invocation. This accepts only two certified
  // forwarding shapes: direct tuple-ref metadata, or a variadic identity-
  // forward wrapper where the child preserves the caller tuple as one full-
  // width argument.
  auto tryTupleForwardedCallerTupleRewrite =
      [&](uint32_t callerArgIdx, StringRef baseArgText,
          ArrayRef<OccObservation> occObservations,
          std::string &outNewArg) -> bool {


    /// Tuple rewrite modes are ordered from strongest proof to weakest.
    ///
    /// DirectTupleRefs uses producer-supplied tuple element byte ranges in
    /// the normalized child invocation. VariadicIdentityForward is the
    /// fallback for variadic forwarding wrappers whose immediate child
    /// keeps the caller's variadic tuple intact as a single full-width
    /// forwarded argument (for example `__VA_ARGS__`).
    enum class TupleRewriteMode {
      None,
      DirectTupleRefs,
      VariadicIdentityForward,
    };

    StringRef parentTrim = baseArgText.trim();

    // There is no caller tuple to rewrite if the parent argument is empty.
    if (parentTrim.empty())
      return false;

    auto getNormalizedArgText =
        [&](const RefoldModel::MacroInvocation &inv,
            uint32_t argIdx) -> std::optional<StringRef> {
      // Return a child argument slice from normalized invocation text. This is
      // used by tuple-ref mode because tuple refs are expressed over normalized
      // child argument text/ranges.
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
    };

    auto getInvocationArgText =
        [&](const RefoldModel::MacroInvocation &inv,
            uint32_t argIdx) -> std::optional<StringRef> {
      // Return a child argument slice from the raw invocation spelling. This
      // is used by identity-forward mode, where the proof comes from raw
      // arg-ref byte coverage rather than tuple-ref metadata.
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
    };

    /// Resolve a macro name through a deterministic object-like alias chain to
    /// a unique function-like definition.
    ///
    /// Tuple-generated-callee proofs use this for two different
    /// source-preserving cases: the root wrapper can name the forwarding macro
    /// through an alias
    /// (`CALL_ALIAS PAIR`), and the tuple's callee element can itself be an
    /// alias chain (`FSEL1 -> FSEL2 -> ADD_ONE`).  The alias is used only as
    /// proof evidence; the tuple source spelling is never replaced by the
    /// resolved name.  Each hop must be a unique object-like #define with
    /// exactly one literal replacement token, and the walk is bounded by the
    /// directive table so cycles or ambiguous macro-state histories fail
    /// closed.
    auto resolveFunctionLikeMacroThroughObjectAliases =
        [&](StringRef startName) -> const RefoldModel::MacroDirective * {
      if (startName.empty())
        return nullptr;

      SmallVector<std::string, 8> seen;
      std::string current = startName.str();
      for (size_t depth = 0; depth <= (*deps_.model).GetMacroDirectives().size();
           ++depth) {
        if (llvm::is_contained(seen, current))
          return nullptr;
        seen.push_back(current);

        const RefoldModel::MacroDirective *functionLike = nullptr;
        const RefoldModel::MacroDirective *alias = nullptr;
        for (const RefoldModel::MacroDirective &directive :
             (*deps_.model).GetMacroDirectives()) {
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
    };

    auto tryParentTupleGeneratedCalleeRewrite = [&]() -> bool {
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
      if (!m.definitionDirectiveId)
        return false;

      const RefoldModel::MacroDirective *rootDefinition = nullptr;
      for (const RefoldModel::MacroDirective &directive :
           (*deps_.model).GetMacroDirectives()) {
        if (directive.id == *m.definitionDirectiveId) {
          rootDefinition = &directive;
          break;
        }
      }
      if (!rootDefinition || rootDefinition->subkind != "#define" ||
          !rootDefinition->functionLike)
        return false;

      // This fallback proves the common tuple-wrapper shape directly from the
      // parent definition: the wrapper replacement is a literal forwarding
      // macro followed by exactly the caller tuple formal, e.g. `CALL PAIR`.
      // Keeping the shape this narrow prevents the proof from guessing about
      // arbitrary wrapper bodies.
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
          resolveFunctionLikeMacroThroughObjectAliases(forwarderName);
      if (!forwarderDefinition || forwarderDefinition->defParams.empty())
        return false;

      // The forwarded caller argument must be a real parenthesized tuple.  The
      // byte ranges returned by the splitter are relative to the tuple payload
      // between the outer parentheses; edits are later applied to that payload
      // and wrapped back in the original tuple parens.
      if (!parentTrim.starts_with("(") || !parentTrim.ends_with(")") ||
          parentTrim.size() < 2)
        return false;
      StringRef tuplePayload = parentTrim.drop_front().drop_back();
      SmallVector<TupleElementSlice, 8> tupleElems;
      if (!splitTopLevelTupleElementsWithLexer(tuplePayload, (*deps_.lexLang),
                                               tupleElems))
        return false;
      if (tupleElems.size() < 2)
        return false;

      // A generated-argument reference maps one forwarder formal to the callee
      // actual list.  For a fixed formal this is a one-tuple-element mapping;
      // for a variadic forwarder formal it represents the entire remaining
      // tuple tail, which is why later edits must be positional rather than
      // keyed by old text.
      struct GeneratedArgRef {
        uint32_t forwarderParamIdx = 0;
        bool variadicPack = false;
      };

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
      // function-like macro definition below, while the remaining formals
      // become positional tuple-edit targets.
      SmallVector<GeneratedArgRef, 8> generatedArgs;
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
        GeneratedArgRef ref;
        ref.forwarderParamIdx = *tok.paramIndex;
        ref.variadicPack =
            forwarderDefinition->defParams[*tok.paramIndex].variadic;
        generatedArgs.push_back(ref);
      }
      if (generatedArgs.empty())
        return false;

      auto tupleElementText = [&](size_t elemIdx) -> StringRef {
        const TupleElementSlice &elem = tupleElems[elemIdx];
        return tuplePayload.slice(elem.trimBegin, elem.trimEnd).trim();
      };

      auto resolveFunctionLikeCallee =
          [&](StringRef calleeSpelling) -> const RefoldModel::MacroDirective * {
        // Resolve the tuple's callee element through the same exact alias proof
        // used for the forwarding macro.  This admits chains such as
        // `FSEL1 -> FSEL2 -> ADD_ONE` while preserving the original tuple
        // spelling (`FSEL1`) in the reconstructed source.
        return resolveFunctionLikeMacroThroughObjectAliases(calleeSpelling);
      };

      const StringRef calleeSourceText =
          tupleElementText(static_cast<size_t>(calleeForwarderParam));
      const RefoldModel::MacroDirective *calleeDefinition =
          resolveFunctionLikeCallee(calleeSourceText);
      if (!calleeDefinition || calleeDefinition->defParams.empty())
        return false;

      // Read the generated actuals from the original tuple spelling.  This is
      // positional, so duplicate spellings like `(ADD2, 10, 10)` remain
      // distinguishable by tuple slot even though their text is identical.
      SmallVector<StringRef, 8> oldGeneratedActualPieces;
      for (const GeneratedArgRef &ref : generatedArgs) {
        if (ref.variadicPack) {
          if (ref.forwarderParamIdx >= tupleElems.size())
            return false;
          for (size_t i = ref.forwarderParamIdx; i < tupleElems.size(); ++i)
            oldGeneratedActualPieces.push_back(tupleElementText(i));
          continue;
        }
        if (ref.forwarderParamIdx >= tupleElems.size())
          return false;
        oldGeneratedActualPieces.push_back(
            tupleElementText(ref.forwarderParamIdx));
      }

      const bool calleeHasVariadic =
          !calleeDefinition->defParams.empty() &&
          calleeDefinition->defParams.back().variadic;
      const size_t fixedCalleeActuals =
          calleeHasVariadic ? calleeDefinition->defParams.size() - 1
                            : calleeDefinition->defParams.size();
      if ((!calleeHasVariadic && oldGeneratedActualPieces.size() !=
                                     calleeDefinition->defParams.size()) ||
          (calleeHasVariadic &&
           oldGeneratedActualPieces.size() < fixedCalleeActuals))
        return false;

      // Repackage the tuple pieces as the actual list of the generated callee.
      // For a variadic callee, all generated tail pieces are joined into the
      // one variadic formal spelling; the inverse mapping back to tuple
      // elements is performed after solving the new expansion.
      SmallVector<std::string, 8> oldActuals;
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

      // Token view used for callee replay.  The spelling sequence proves the
      // replacement-list grammar; the byte offsets let a solved token interval
      // be converted back to exact text for a tuple slot.
      auto lexReplayTokens = [&](StringRef text,
                                 SmallVectorImpl<ReplayTok> &out) {
        out.clear();
        SmallVector<RefoldLexBoundaryToken, 32> toks;
        refoldLexBoundaryTokens(text, (*deps_.lexLang), toks);
        for (const RefoldLexBoundaryToken &tok : toks)
          out.push_back(ReplayTok{tok.spelling, tok.begin, tok.end});
      };

      auto tokenSpellingsForText = [&](StringRef text) {
        SmallVector<ReplayTok, 16> toks;
        lexReplayTokens(text, toks);
        SmallVector<std::string, 16> out;
        for (const ReplayTok &tok : toks)
          out.push_back(tok.spelling);
        return out;
      };

      // Normalized callee replacement-list tree.  This mirrors ReplayElem above
      // but is local to the generated callee: literals must match exactly,
      // params become solved callee actual ranges, and VA_OPT recursively
      // models the erased/exposed payload.
      enum class CalleeReplayKind { Literal, Param, VaOpt, Stringify, Paste };
      struct CalleePastePiece {
        bool isParam = false;
        uint32_t paramIdx = 0;
        std::string literal;
      };
      struct CalleeReplayElem {
        CalleeReplayKind kind = CalleeReplayKind::Literal;
        std::string literal;
        uint32_t paramIdx = 0;
        std::vector<CalleeReplayElem> children;
        std::vector<CalleePastePiece> pastePieces;
      };

      // Parse the callee replacement list into a replay tree.  Ordinary
      // literal/param/VA_OPT replay handles generated callees like
      // `ADD_ONE(x)`, while the Stringify/Paste nodes below cover the two
      // non-injective macro operators only when the observed old and new
      // expansion text make the inverse mapping unique.  The operators remain
      // local to this generated callee; the final tuple edit is still
      // positional over the caller tuple.
      auto pastePieceFromReplacementToken =
          [&](const RefoldModel::MacroReplacementToken &tok,
              CalleePastePiece &piece) -> bool {
        if (tok.kind == RefoldModel::MacroReplacementTokenKind::ParamRef) {
          if (!tok.paramIndex || *tok.paramIndex >= oldActuals.size())
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
      };

      std::function<bool(size_t, size_t, std::vector<CalleeReplayElem> &)>
          parseCalleeReplayRange;
      parseCalleeReplayRange = [&](size_t begin, size_t end,
                                   std::vector<CalleeReplayElem> &out) {
        for (size_t i = begin; i < end;) {
          const auto &tok = calleeDefinition->replacementTokens[i];
          if (tok.spelling == "#") {
            if (i + 1 >= end ||
                calleeDefinition->replacementTokens[i + 1].kind !=
                    RefoldModel::MacroReplacementTokenKind::ParamRef ||
                !calleeDefinition->replacementTokens[i + 1].paramIndex)
              return false;
            const uint32_t paramIdx =
                *calleeDefinition->replacementTokens[i + 1].paramIndex;
            if (paramIdx >= oldActuals.size())
              return false;
            CalleeReplayElem elem;
            elem.kind = CalleeReplayKind::Stringify;
            elem.paramIdx = paramIdx;
            out.push_back(std::move(elem));
            i += 2;
            continue;
          }

          if (i + 1 < end &&
              calleeDefinition->replacementTokens[i + 1].spelling == "##") {
            CalleeReplayElem elem;
            elem.kind = CalleeReplayKind::Paste;
            CalleePastePiece first;
            if (!pastePieceFromReplacementToken(tok, first))
              return false;
            elem.pastePieces.push_back(std::move(first));
            i += 2;
            while (true) {
              if (i >= end)
                return false;
              CalleePastePiece next;
              if (!pastePieceFromReplacementToken(
                      calleeDefinition->replacementTokens[i], next))
                return false;
              elem.pastePieces.push_back(std::move(next));
              ++i;
              if (i >= end ||
                  calleeDefinition->replacementTokens[i].spelling != "##")
                break;
              ++i;
            }
            out.push_back(std::move(elem));
            continue;
          }

          if (tok.kind == RefoldModel::MacroReplacementTokenKind::ParamRef) {
            if (!tok.paramIndex || *tok.paramIndex >= oldActuals.size())
              return false;
            CalleeReplayElem elem;
            elem.kind = CalleeReplayKind::Param;
            elem.paramIdx = *tok.paramIndex;
            out.push_back(std::move(elem));
            ++i;
            continue;
          }
          if (tok.spelling == "##")
            return false;
          if (tok.spelling == "__VA_OPT__") {
            if (i + 1 >= end ||
                calleeDefinition->replacementTokens[i + 1].kind !=
                    RefoldModel::MacroReplacementTokenKind::Literal ||
                calleeDefinition->replacementTokens[i + 1].spelling != "(")
              return false;
            unsigned depth = 1;
            size_t j = i + 2;
            for (; j < end; ++j) {
              const auto &inner = calleeDefinition->replacementTokens[j];
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
            CalleeReplayElem elem;
            elem.kind = CalleeReplayKind::VaOpt;
            if (!parseCalleeReplayRange(i + 2, j, elem.children))
              return false;
            out.push_back(std::move(elem));
            i = j + 1;
            continue;
          }
          CalleeReplayElem elem;
          elem.kind = CalleeReplayKind::Literal;
          elem.literal = tok.spelling.str();
          out.push_back(std::move(elem));
          ++i;
        }
        return true;
      };

      std::vector<CalleeReplayElem> calleePattern;
      if (!parseCalleeReplayRange(0, calleeDefinition->replacementTokens.size(),
                                  calleePattern) ||
          calleePattern.empty())
        return false;

      SmallVector<SmallVector<std::string, 16>, 8> oldActualTokSpellings;
      for (const std::string &actual : oldActuals)
        oldActualTokSpellings.push_back(tokenSpellingsForText(actual));
      auto singleTokenSpelling =
          [&](StringRef text) -> std::optional<std::string> {
        SmallVector<ReplayTok, 4> toks;
        lexReplayTokens(text, toks);
        if (toks.size() != 1)
          return std::nullopt;
        return toks.front().spelling;
      };

      auto rewriteTupleElementFromSolvedExpansion =
          [&](size_t elemIdx, StringRef oldExpansion,
              StringRef newExpansion) -> std::optional<std::string> {
        StringRef source = tupleElementText(elemIdx);
        oldExpansion = oldExpansion.trim();
        newExpansion = newExpansion.trim();
        if (source == oldExpansion)
          return newExpansion.str();
        if (auto loc =
                findUniqueTrimmedSubstring(source, oldExpansion))
          return stringutils::replaceRange(source.str(), loc->first,
                                           loc->second, newExpansion);
        return std::nullopt;
      };

      // Verify that the old generated expansion is actually explained by the
      // tuple-derived callee actuals.  This is the A-side proof for the tuple
      // bridge; without it, solving the new text alone could rewrite an
      // unrelated whole-argument expansion that merely happens to look similar.
      std::function<void(ArrayRef<CalleeReplayElem>, ArrayRef<ReplayTok>,
                         size_t, SmallVectorImpl<size_t> &)>
          matchOldEnds;
      matchOldEnds = [&](ArrayRef<CalleeReplayElem> elems,
                         ArrayRef<ReplayTok> toks, size_t pos,
                         SmallVectorImpl<size_t> &ends) {
        if (elems.empty()) {
          ends.push_back(pos);
          return;
        }
        const CalleeReplayElem &elem = elems.front();
        ArrayRef<CalleeReplayElem> rest = elems.drop_front();
        switch (elem.kind) {
        case CalleeReplayKind::Literal:
          if (pos < toks.size() && toks[pos].spelling == elem.literal)
            matchOldEnds(rest, toks, pos + 1, ends);
          return;
        case CalleeReplayKind::Param: {
          const auto &expected = oldActualTokSpellings[elem.paramIdx];
          if (pos + expected.size() > toks.size())
            return;
          for (size_t i = 0; i < expected.size(); ++i)
            if (toks[pos + i].spelling != expected[i])
              return;
          matchOldEnds(rest, toks, pos + expected.size(), ends);
          return;
        }
        case CalleeReplayKind::Stringify: {
          if (pos >= toks.size())
            return;
          std::optional<std::string> content =
              decodeSimpleStringLiteralToken(toks[pos].spelling);
          if (!content)
            return;
          StringRef oldActual = StringRef(oldActuals[elem.paramIdx]).trim();
          if (StringRef(*content).trim() != oldActual) {
            auto loc = findUniqueTrimmedSubstring(oldActual, StringRef(*content));
            if (!loc)
              return;
          }
          matchOldEnds(rest, toks, pos + 1, ends);
          return;
        }
        case CalleeReplayKind::Paste: {
          if (pos >= toks.size())
            return;
          std::string expected;
          for (const CalleePastePiece &piece : elem.pastePieces) {
            if (piece.isParam)
              expected += StringRef(oldActuals[piece.paramIdx]).trim().str();
            else
              expected += piece.literal;
          }
          if (toks[pos].spelling != expected)
            return;
          matchOldEnds(rest, toks, pos + 1, ends);
          return;
        }
        case CalleeReplayKind::VaOpt: {
          matchOldEnds(rest, toks, pos, ends);
          SmallVector<size_t, 4> childEnds;
          matchOldEnds(elem.children, toks, pos, childEnds);
          for (size_t childEnd : childEnds)
            if (childEnd != pos)
              matchOldEnds(rest, toks, childEnd, ends);
          return;
        }
        }
      };

      auto matchOldExpansion = [&](StringRef oldExpansion) {
        SmallVector<ReplayTok, 32> toks;
        lexReplayTokens(oldExpansion, toks);
        SmallVector<size_t, 4> ends;
        matchOldEnds(calleePattern, toks, 0, ends);
        return llvm::is_contained(ends, toks.size());
      };

      auto solveStringifyOrPasteNewExpansion = [&](StringRef newExpansion)
          -> std::optional<SmallVector<std::string, 8>> {
        if (calleePattern.size() != 1)
          return std::nullopt;
        const CalleeReplayElem &elem = calleePattern.front();
        SmallVector<std::string, 8> actuals;
        actuals.resize(calleeDefinition->defParams.size());
        for (size_t i = 0; i < oldActuals.size(); ++i)
          actuals[i] = oldActuals[i];

        if (elem.kind == CalleeReplayKind::Stringify) {
          std::optional<std::string> token = singleTokenSpelling(newExpansion);
          if (!token)
            return std::nullopt;
          std::optional<std::string> content =
              decodeSimpleStringLiteralToken(*token);
          if (!content)
            return std::nullopt;
          actuals[elem.paramIdx] = std::move(*content);
          return actuals;
        }

        if (elem.kind != CalleeReplayKind::Paste)
          return std::nullopt;
        std::optional<std::string> pasted = singleTokenSpelling(newExpansion);
        if (!pasted)
          return std::nullopt;

        size_t cursor = 0;
        SmallVector<std::optional<std::string>, 8> assigned;
        assigned.resize(calleeDefinition->defParams.size());
        for (const CalleePastePiece &piece : elem.pastePieces) {
          if (piece.isParam) {
            const size_t width =
                StringRef(oldActuals[piece.paramIdx]).trim().size();
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
      };

      auto solveNewExpansion = [&](StringRef newExpansion)
          -> std::optional<SmallVector<std::string, 8>> {
        // Invert the callee replacement-list grammar over the B-side expansion
        // and recover the new callee actual text.  Repeated formal references
        // must bind to identical token spellings, and the result is accepted
        // only when there is exactly one solution.  This is what makes
        // duplicate tuple values safe: slots are solved by replay position, not
        // by text.
        SmallVector<ReplayTok, 32> toks;
        lexReplayTokens(newExpansion, toks);
        SmallVector<std::optional<std::pair<size_t, size_t>>, 8> assigned;
        assigned.resize(calleeDefinition->defParams.size());
        SmallVector<SmallVector<std::string, 8>, 4> solutions;

        std::function<void(
            ArrayRef<CalleeReplayElem>, size_t,
            SmallVector<std::optional<std::pair<size_t, size_t>>, 8> &)>
            dfs;
        dfs = [&](ArrayRef<CalleeReplayElem> elems, size_t tokPos,
                  SmallVector<std::optional<std::pair<size_t, size_t>>, 8>
                      &curAssigned) {
          if (solutions.size() > 1)
            return;
          if (elems.empty()) {
            if (tokPos != toks.size())
              return;
            SmallVector<std::string, 8> actuals;
            for (const auto &range : curAssigned) {
              if (!range)
                return;
              if (range->first == range->second) {
                actuals.push_back(std::string());
                continue;
              }
              const size_t byteBegin = toks[range->first].begin;
              const size_t byteEnd = toks[range->second - 1].end;
              actuals.push_back(newExpansion.slice(byteBegin, byteEnd).str());
            }
            solutions.push_back(std::move(actuals));
            return;
          }

          const CalleeReplayElem &elem = elems.front();
          ArrayRef<CalleeReplayElem> rest = elems.drop_front();
          switch (elem.kind) {
          case CalleeReplayKind::Literal:
            if (tokPos < toks.size() && toks[tokPos].spelling == elem.literal)
              dfs(rest, tokPos + 1, curAssigned);
            return;
          case CalleeReplayKind::Param: {
            if (elem.paramIdx >= curAssigned.size())
              return;
            if (curAssigned[elem.paramIdx]) {
              const auto range = *curAssigned[elem.paramIdx];
              const size_t width = range.second - range.first;
              if (tokPos + width <= toks.size()) {
                bool same = true;
                for (size_t i = 0; i < width; ++i) {
                  if (toks[range.first + i].spelling !=
                      toks[tokPos + i].spelling) {
                    same = false;
                    break;
                  }
                }
                if (same)
                  dfs(rest, tokPos + width, curAssigned);
              }
              return;
            }
            for (size_t end = tokPos; end <= toks.size(); ++end) {
              curAssigned[elem.paramIdx] = std::make_pair(tokPos, end);
              dfs(rest, end, curAssigned);
              curAssigned[elem.paramIdx].reset();
              if (solutions.size() > 1)
                return;
            }
            return;
          }
          case CalleeReplayKind::Stringify:
          case CalleeReplayKind::Paste:
            return;
          case CalleeReplayKind::VaOpt: {
            dfs(rest, tokPos, curAssigned);
            SmallVector<std::optional<std::pair<size_t, size_t>>, 8>
                withPayload = curAssigned;
            std::function<void(
                ArrayRef<CalleeReplayElem>, size_t,
                SmallVector<std::optional<std::pair<size_t, size_t>>, 8> &)>
                dfsChild;
            dfsChild = [&](ArrayRef<CalleeReplayElem> childElems,
                           size_t childTokPos,
                           SmallVector<std::optional<std::pair<size_t, size_t>>,
                                       8> &childAssigned) {
              if (childElems.empty()) {
                if (childTokPos != tokPos)
                  dfs(rest, childTokPos, childAssigned);
                return;
              }
              const CalleeReplayElem &child = childElems.front();
              ArrayRef<CalleeReplayElem> childRest = childElems.drop_front();
              switch (child.kind) {
              case CalleeReplayKind::Literal:
                if (childTokPos < toks.size() &&
                    toks[childTokPos].spelling == child.literal)
                  dfsChild(childRest, childTokPos + 1, childAssigned);
                return;
              case CalleeReplayKind::Param: {
                if (child.paramIdx >= childAssigned.size())
                  return;
                if (childAssigned[child.paramIdx]) {
                  const auto range = *childAssigned[child.paramIdx];
                  const size_t width = range.second - range.first;
                  if (childTokPos + width <= toks.size()) {
                    bool same = true;
                    for (size_t i = 0; i < width; ++i) {
                      if (toks[range.first + i].spelling !=
                          toks[childTokPos + i].spelling) {
                        same = false;
                        break;
                      }
                    }
                    if (same)
                      dfsChild(childRest, childTokPos + width, childAssigned);
                  }
                  return;
                }
                for (size_t end = childTokPos; end <= toks.size(); ++end) {
                  childAssigned[child.paramIdx] =
                      std::make_pair(childTokPos, end);
                  dfsChild(childRest, end, childAssigned);
                  childAssigned[child.paramIdx].reset();
                  if (solutions.size() > 1)
                    return;
                }
                return;
              }
              case CalleeReplayKind::Stringify:
              case CalleeReplayKind::Paste:
              case CalleeReplayKind::VaOpt:
                return;
              }
            };
            dfsChild(elem.children, tokPos, withPayload);
            return;
          }
          }
        };

        dfs(calleePattern, 0, assigned);
        if (solutions.size() != 1)
          return std::nullopt;
        return solutions.front();
      };

      // Every observed occurrence of the parent argument must agree on the same
      // solved generated-callee actuals.  If one occurrence cannot be explained
      // by the old tuple-derived call, or if two occurrences imply different B
      // actuals, the tuple bridge is unproved.
      std::optional<SmallVector<std::string, 8>> mergedSolvedActuals;
      for (const OccObservation &obs : occObservations) {
        if (!matchOldExpansion(obs.oldText))
          continue;
        auto solved = solveStringifyOrPasteNewExpansion(obs.newText);
        if (!solved)
          solved = solveNewExpansion(obs.newText);
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

      // For ordinary param replay, the source tuple element and the old
      // generated actual are the same spelling.  Stringification is different:
      // the B-side expansion is a string literal whose payload corresponds to
      // the generated actual after forwarding/prescan, while the tuple slot may
      // still contain a structural spelling such as `ID(alpha)`.  Keep a
      // separate old-expansion projection for tuple editing so `"alpha" ->
      // "beta"` can become `ID(alpha) -> ID(beta)` instead of replacing the
      // whole slot with `beta`.
      SmallVector<std::string, 8> oldGeneratedPiecesForRewrite;
      for (StringRef piece : oldGeneratedActualPieces)
        oldGeneratedPiecesForRewrite.push_back(piece.trim().str());
      if (calleePattern.size() == 1 &&
          calleePattern.front().kind == CalleeReplayKind::Stringify) {
        const uint32_t paramIdx = calleePattern.front().paramIdx;
        if (paramIdx < oldGeneratedPiecesForRewrite.size()) {
          for (const OccObservation &obs : occObservations) {
            std::optional<std::string> token = singleTokenSpelling(obs.oldText);
            if (!token)
              continue;
            std::optional<std::string> content =
                decodeSimpleStringLiteralToken(*token);
            if (!content)
              continue;
            StringRef sourcePiece =
                StringRef(oldGeneratedPiecesForRewrite[paramIdx]);
            if (sourcePiece == StringRef(*content) ||
                findUniqueTrimmedSubstring(sourcePiece,
                                                         StringRef(*content))) {
              oldGeneratedPiecesForRewrite[paramIdx] = std::move(*content);
              break;
            }
          }
        }
      }

      // Split the solved callee actuals back into the generated tuple pieces.
      // A non-empty variadic tail appends more tuple elements; an empty tail
      // deletes the old variadic tail slice during the tuple-edit pass below.
      SmallVector<std::string, 8> newGeneratedPieces;
      for (size_t i = 0; i < fixedCalleeActuals; ++i)
        newGeneratedPieces.push_back((*mergedSolvedActuals)[i]);
      if (calleeHasVariadic) {
        StringRef tail = StringRef((*mergedSolvedActuals).back()).trim();
        if (!tail.empty())
          newGeneratedPieces.push_back(tail.str());
      }

      // Translate the solved callee actuals back to the tuple elements consumed
      // by the forwarding macro.  Non-variadic forwarding parameters map to one
      // tuple element each.  A variadic forwarding parameter maps to the whole
      // remaining tuple tail, so insertion/removal of callee variadic actuals
      // is represented by replacing that tail slice.
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
          const TupleElementSlice &firstElem =
              tupleElems[ref.forwarderParamIdx];
          const TupleElementSlice &lastElem = tupleElems.back();
          edits.push_back(TupleEdit{firstElem.trimBegin, lastElem.trimEnd,
                                    std::move(text)});
          continue;
        }
        if (pieceCursor >= newGeneratedPieces.size() ||
            ref.forwarderParamIdx >= tupleElems.size())
          return false;
        const TupleElementSlice &elem = tupleElems[ref.forwarderParamIdx];
        StringRef oldText =
            pieceCursor < oldGeneratedPiecesForRewrite.size()
                ? StringRef(oldGeneratedPiecesForRewrite[pieceCursor]).trim()
                : StringRef(oldGeneratedActualPieces[pieceCursor]).trim();
        StringRef newText = StringRef(newGeneratedPieces[pieceCursor]).trim();
        if (newText !=
            tuplePayload.slice(elem.trimBegin, elem.trimEnd).trim()) {
          std::optional<std::string> rewrittenElem =
              rewriteTupleElementFromSolvedExpansion(ref.forwarderParamIdx,
                                                     oldText, newText);
          if (!rewrittenElem)
            return false;
          edits.push_back(TupleEdit{elem.trimBegin, elem.trimEnd,
                                    std::move(*rewrittenElem)});
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
    };

    if (tryParentTupleGeneratedCalleeRewrite())
      return true;

    const RefoldModel::MacroInvocation *tupleChild = nullptr;
    TupleRewriteMode rewriteMode = TupleRewriteMode::None;
    SmallVector<std::pair<uint32_t, StringRef>, 8> childArgs;
    SmallVector<RefoldModel::TupleArgRef, 8> childTupleRefs;
    std::optional<uint32_t> identityForwardChildArgIdx;

    // Search direct children of the current macro invocation for exactly one
    // forwarding witness. Multiple usable children would make the caller tuple
    // rewrite ambiguous, so the code fails closed if more than one is found.
    for (const auto &cand : (*deps_.model).GetMacroInvocations()) {
      if (!cand.callerMacroId || *cand.callerMacroId != m.id)
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

          auto oldArgText = getNormalizedArgText(cand, childArgIdx);
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
          // Accept exactly one child as the tuple-ref witness. A second
          // witness would give two possible reconstructions of the same caller
          // argument.
          if (tupleChild)
            return false;
          tupleChild = &cand;
          rewriteMode = TupleRewriteMode::DirectTupleRefs;
          childArgs = std::move(localChildArgs);
          childTupleRefs = std::move(localTupleRefs);
          continue;
        }
      }

      if (!isMacroInvocationVariadicFormal(m, callerArgIdx) || !cand.invText || !cand.invB ||
          cand.invArgRanges.empty() || cand.argRefs.empty())
        continue;

      // Variadic forwarding wrappers may not carry tuple-specific metadata.
      // Accept a second certified shape where one child argument is a
      // full-width identity forward of the caller variadic formal. That
      // proves the caller tuple survives unchanged at the child hop, so we
      // can safely rebuild it element-by-element from the occurrence
      // observations.
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
        // bounds. Full-width trimmed coverage is the identity-forward proof:
        // the child argument is exactly the caller argument, modulo surrounding
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

        auto oldArgText = getInvocationArgText(cand, childArgIdx);
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

    // No direct child proved either tuple-ref forwarding or identity
    // forwarding.
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
        auto it = newTextByOld.find(obs.oldText);
        if (it == newTextByOld.end()) {
          newTextByOld[obs.oldText] = obs.newText;
          continue;
        }
        if (it->second != obs.newText)
          return false;
      }

      // A tuple-forwarding child can use one tuple element as a callee and a
      // different tuple element as that callee's argument, e.g.
      // `WRAP((ADD_ONE, 10))` -> `CALL(ADD_ONE, 10)` -> `ADD_ONE(10)`.
      // In that shape the only expansion occurrence visible at the WRAP level
      // is the full callee expansion `((10) + 1)`, so the direct tuple-ref map
      // above has no key for the tuple element `10`.  Recover that missing
      // element rewrite by proving the child replacement-list constructs a
      // function-like invocation from tuple-ref formals and then replaying the
      // callee's own replacement-token tape against the observed old/new
      // expansion text.
      auto deriveTupleElementRewritesThroughForwardedCallee = [&]() -> bool {
        if (!tupleChild || !tupleChild->definitionDirectiveId)
          return true;

        const RefoldModel::MacroDirective *childDefinition = nullptr;
        for (const RefoldModel::MacroDirective &directive :
             (*deps_.model).GetMacroDirectives()) {
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
        // with the callee and each argument coming from direct tuple refs. This
        // is a syntactic proof of a generated call, not a name-based heuristic.
        const auto &repToks = childDefinition->replacementTokens;
        if (repToks.size() < 4)
          return true;
        if (repToks[0].kind !=
                RefoldModel::MacroReplacementTokenKind::ParamRef ||
            !repToks[0].paramIndex ||
            *repToks[0].paramIndex >= childArgs.size())
          return true;
        if (repToks[1].kind !=
                RefoldModel::MacroReplacementTokenKind::Literal ||
            repToks[1].spelling != "(")
          return true;
        if (repToks.back().kind !=
                RefoldModel::MacroReplacementTokenKind::Literal ||
            repToks.back().spelling != ")")
          return true;

        const uint32_t calleeChildArgIdx = *repToks[0].paramIndex;
        std::optional<StringRef> calleeName;
        for (const auto &arg : childArgs) {
          if (arg.first == calleeChildArgIdx) {
            calleeName = arg.second.trim();
            break;
          }
        }
        if (!calleeName || calleeName->empty())
          return true;

        const RefoldModel::MacroDirective *calleeDefinition = nullptr;
        for (const RefoldModel::MacroDirective &directive :
             (*deps_.model).GetMacroDirectives()) {
          if (directive.subkind == "#define" && directive.functionLike &&
              directive.name == *calleeName) {
            if (calleeDefinition)
              return false;
            calleeDefinition = &directive;
          }
        }
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

        auto childArgText =
            [&](uint32_t childArgIdx) -> std::optional<StringRef> {
          for (const auto &arg : childArgs)
            if (arg.first == childArgIdx)
              return arg.second.trim();
          return std::nullopt;
        };

        SmallVector<std::string, 4> oldActuals;
        oldActuals.reserve(forwardedChildArgs.size());
        for (uint32_t childArgIdx : forwardedChildArgs) {
          auto text = childArgText(childArgIdx);
          if (!text)
            return true;
          oldActuals.push_back(text->str());
        }

        // Use the file-scope token carrier for this parent-tuple callee replay
        // proof so token-range helpers can later become named methods.
        using ReplayTok = ParentTupleCalleeReplayTok;
        auto lexReplayTokens = [&](StringRef text,
                                   SmallVectorImpl<ReplayTok> &out) {
          out.clear();
          SmallVector<RefoldLexBoundaryToken, 16> toks;
          refoldLexBoundaryTokens(text, (*deps_.lexLang), toks);
          for (const RefoldLexBoundaryToken &tok : toks)
            out.push_back(ReplayTok{tok.spelling, tok.begin, tok.end});
        };

        auto tokenSpellingsForText = [&](StringRef text) {
          SmallVector<ReplayTok, 8> toks;
          lexReplayTokens(text, toks);
          SmallVector<std::string, 8> out;
          for (const ReplayTok &tok : toks)
            out.push_back(tok.spelling);
          return out;
        };

        struct CalleeReplayElem {
          bool isParam = false;
          std::string literal;
          uint32_t paramIdx = 0;
        };
        SmallVector<CalleeReplayElem, 16> calleePattern;
        for (const RefoldModel::MacroReplacementToken &tok :
             calleeDefinition->replacementTokens) {
          if (tok.spelling == "#" || tok.spelling == "##" ||
              tok.spelling == "__VA_OPT__")
            return true;
          CalleeReplayElem elem;
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
          oldActualTokSpellings.push_back(tokenSpellingsForText(actual));

        auto matchOldExpansion = [&](StringRef oldExpansion) {
          SmallVector<ReplayTok, 16> toks;
          lexReplayTokens(oldExpansion, toks);
          size_t pos = 0;
          for (const CalleeReplayElem &elem : calleePattern) {
            if (!elem.isParam) {
              if (pos >= toks.size() || toks[pos].spelling != elem.literal)
                return false;
              ++pos;
              continue;
            }
            const auto &expected = oldActualTokSpellings[elem.paramIdx];
            if (!replayTokenRangeSpellingsEqual(toks, pos, expected))
              return false;
            pos += expected.size();
          }
          return pos == toks.size();
        };

        auto solveNewExpansion = [&](StringRef newExpansion)
            -> std::optional<SmallVector<std::string, 4>> {
          SmallVector<ReplayTok, 16> toks;
          lexReplayTokens(newExpansion, toks);
          SmallVector<std::optional<std::pair<size_t, size_t>>, 4> assigned;
          assigned.resize(calleeDefinition->defParams.size());
          SmallVector<SmallVector<std::string, 4>, 4> solutions;

          std::function<void(size_t, size_t)> dfs = [&](size_t elemIdx,
                                                        size_t tokPos) {
            if (solutions.size() > 1)
              return;
            if (elemIdx == calleePattern.size()) {
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
                actuals.push_back(newExpansion.slice(byteBegin, byteEnd).str());
              }
              solutions.push_back(std::move(actuals));
              return;
            }

            const CalleeReplayElem &elem = calleePattern[elemIdx];
            if (!elem.isParam) {
              if (tokPos < toks.size() && toks[tokPos].spelling == elem.literal)
                dfs(elemIdx + 1, tokPos + 1);
              return;
            }

            if (elem.paramIdx >= assigned.size())
              return;
            if (assigned[elem.paramIdx]) {
              const auto range = *assigned[elem.paramIdx];
              const size_t width = range.second - range.first;
              if (tokPos + width <= toks.size()) {
                bool same = true;
                for (size_t i = 0; i < width; ++i) {
                  if (toks[range.first + i].spelling !=
                      toks[tokPos + i].spelling) {
                    same = false;
                    break;
                  }
                }
                if (same)
                  dfs(elemIdx + 1, tokPos + width);
              }
              return;
            }

            for (size_t end = tokPos; end <= toks.size(); ++end) {
              assigned[elem.paramIdx] = std::make_pair(tokPos, end);
              dfs(elemIdx + 1, end);
              assigned[elem.paramIdx].reset();
              if (solutions.size() > 1)
                return;
            }
          };

          dfs(0, 0);
          if (solutions.size() != 1)
            return std::nullopt;
          return solutions.front();
        };

        for (const OccObservation &obs : occObservations) {
          if (!matchOldExpansion(obs.oldText))
            continue;
          auto solvedActuals = solveNewExpansion(obs.newText);
          if (!solvedActuals)
            return false;
          if (solvedActuals->size() != forwardedChildArgs.size())
            return false;

          for (size_t i = 0; i < forwardedChildArgs.size(); ++i) {
            auto oldText = childArgText(forwardedChildArgs[i]);
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
      };

      if (!deriveTupleElementRewritesThroughForwardedCallee())
        return false;

      rebuilt = parentTrim.str();

      // Replace parent tuple slices from right to left so tuple-ref byte
      // offsets remain valid while editing `rebuilt`.
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
      if (!splitTopLevelTupleElementsWithLexer(parentTrim, (*deps_.lexLang),
                                               tupleElems))
        return false;

      // Identity-forward rewrites are positional: the immediate child keeps
      // the caller variadic tuple intact, so each observed occurrence must
      // correspond to exactly one top-level tuple element in order.
      if (tupleElems.size() != occObservations.size())
        return false;

      rebuilt = parentTrim.str();
      for (size_t i = tupleElems.size(); i > 0; --i) {
        const auto &elem = tupleElems[i - 1];
        StringRef oldElemText =
            parentTrim.slice(elem.trimBegin, elem.trimEnd).trim();

        // Replacements apply from right to left so earlier byte offsets
        // stay valid while we splice into the rebuilt caller tuple.
        if (oldElemText != occObservations[i - 1].oldText.trim())
          return false;

        rebuilt =
            stringutils::replaceRange(rebuilt, elem.trimBegin, elem.trimEnd,
                                      occObservations[i - 1].newText);
        if (occObservations[i - 1].newText != oldElemText)
          changed = true;
      }

      if (!changed)
        return false;

    } else {
      return false;
    }

    // Return the rebuilt caller argument, normalized to the same trimmed
    // spelling convention used throughout this tuple-forwarding path.
    outNewArg = StringRef(rebuilt).trim().str();

    return true;
  };

  // If the rewritten B slice introduces extra unmatched opening delimiters
  // relative to the old occurrence text, extend the right edge over matching
  // unchanged closer tokens. This keeps the observation envelope balanced when
  // the diff split left the closers just outside the initial hunk.
  auto maybeExtendRightBoundaryClosers =
      [&](const RefoldModel::PPArgSpan &sp, std::pair<size_t, size_t> env,
          StringRef oldText) -> std::pair<size_t, size_t> {
    StringRef curText = (*deps_.sourceMapper).SliceBSource(env.first, env.second).trim();
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

    // Walk forward only through identical A/B closer tokens immediately after
    // the occurrence. This preserves semantics: widening is allowed only over
    // text that already matches on both sides and exactly satisfies the missing
    // delimiter balance.
    while ((needParen > 0 || needBracket > 0 || needBrace > 0) &&
           aPos < deps_.aToks.size() && bPos < deps_.bToks.size()) {
      StringRef aTok = deps_.aToks[static_cast<size_t>(aPos)].spelling;
      StringRef bTok = deps_.bToks[bPos].spelling;
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
  };

  // Validate a repaired formal replacement against every occurrence whose
  // argIdx was obtained by current-level/declaration replay.
  //
  // This mirrors the normal all-occurrences check, but uses the repaired
  // `standardArgSpans` collection so stale producer indices cannot validate a
  // partial rewrite of only the first occurrence.
  auto standardArgReplacementMatchesAllReplayedOccurrencesInB =
      [&](uint32_t argIdx, StringRef newArg,
          ArrayRef<diffutils::Hunk> tokenHunks) -> bool {
    const uint64_t maxTok = deps_.bTokOff.empty()
                                ? 0ULL
                                : static_cast<uint64_t>(deps_.bTokOff.size() - 1);
    const StringRef expected = newArg.trim();
    bool sawOccurrence = false;

    for (const RefoldModel::PPArgSpan &s : standardArgSpans) {
      if (s.argIdx != argIdx || s.kind != PPArgSpanKind::Standard)
        continue;
      sawOccurrence = true;

      auto bEnv = (*deps_.sourceMapper).MapAToBTokenEnvelopeByPPArgSpan(s);
      if (!bEnv || bEnv->second < bEnv->first)
        return false;

      // Grow the mapped B envelope with any owned insertions or overlapping
      // hunks for this occurrence before comparing the materialized text.
      size_t lo = bEnv->first;
      size_t hi = bEnv->second;
      for (const diffutils::Hunk &hk : tokenHunks) {
        if (auto owned = OccurrenceReplay().GetOwnedPureInsertionBRangeForArgSpan(
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

      StringRef oldText = (*deps_.sourceMapper).SliceASource(s.begin, s.end).trim();
      auto grownEnv = maybeExtendRightBoundaryClosers(s, {lo, hi}, oldText);
      StringRef actual = (*deps_.sourceMapper).SliceBSource(grownEnv.first, grownEnv.second).trim();
      if (actual != expected)
        return false;
    }

    return sawOccurrence;
  };

  // Compute argument replacements implied by each touched occurrence. Multiple
  // occurrences of the same argIdx must imply the exact same replacement,
  // otherwise the macro cannot be refolded args-only.
  DenseMap<uint32_t, std::string> replByArgIdx;
  DenseMap<uint32_t, std::pair<uint64_t, uint64_t>>
      materializedRangeByArgIdx;
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

    SmallVector<OccObservation, 8> occObservations;
    std::optional<std::string> unifiedNewArg;
    std::optional<std::pair<uint64_t, uint64_t>>
        unifiedMaterializedNewTextRange;
    bool sawUntrackedMaterializedNewTextRange = false;
    bool needTupleForwarding = false;

    for (size_t i = 0; i < occs.size(); ++i) {
      const auto &sp = occs[i];
      if (sp.argIdx != argIdx)
        continue;

      // Start with the B-token envelope corresponding to this A-side argument
      // occurrence. If the direct PP-arg mapping is unavailable, fall back to
      // the triggering hunk's B range so the path can still fail/validate
      // locally.
      auto bEnv = (*deps_.sourceMapper).MapAToBTokenEnvelopeByPPArgSpan(sp);
      if (!bEnv) {
        if (h.bStart >= h.bEnd)
          return std::nullopt;
        bEnv = {static_cast<size_t>(h.bStart), static_cast<size_t>(h.bEnd)};
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
          if (auto owned = OccurrenceReplay().GetOwnedPureInsertionBRangeForArgSpan(
                  sp, occs, *bEnv, candH)) {
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

        if (e0 != bEnv->first || e1 != bEnv->second) {
          bEnv = std::make_pair(e0, e1);
        }
      }

      // `oldText` is the original occurrence spelling. `bEnv` is the candidate
      // B spelling that should replace this occurrence after all relevant hunks
      // for the same formal have been incorporated.
      StringRef oldText = (*deps_.sourceMapper).SliceASource(sp.begin, sp.end).trim();

      // For ordinary argument occurrences, allow a narrow right-edge repair
      // over unchanged closer tokens when the diff split leaves balancing
      // delimiters just outside the initial B envelope.
      if (sp.kind == PPArgSpanKind::Standard) {
        auto grownEnv = maybeExtendRightBoundaryClosers(sp, *bEnv, oldText);
        if (grownEnv.second != bEnv->second) {
          bEnv = grownEnv;
        }
      }

      StringRef bSlice = (*deps_.sourceMapper).SliceBSource(bEnv->first, bEnv->second).trim();
      std::string newArg = bSlice.str();

      std::optional<std::pair<uint64_t, uint64_t>>
          materializedNewTextRange;
      if (!occIsStringify[i] && ownedInsertionBRange &&
          !contributedNonInsertionHunk) {
        if (auto insertedBytes = deps_.sourceMapper->BTokenRangeToByteRange(
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
              materializedNewTextRange = std::make_pair(
                  insertedBytes->first - sliceBegin,
                  insertedBytes->second - sliceBegin);
            }
          }
        }
      }

      if (occIsStringify[i]) {
        // Stringify occurrences expose a string literal in the expansion, not
        // the raw argument spelling. Invert the literal back to argument text,
        // then require canonicalization to be stable so ambiguous escapes fail
        // closed.
        auto un = (*deps_.argTextRecovery).UnstringifyLiteralToArgText(
            bSlice, isMacroInvocationVariadicFormal(m, argIdx));
        if (!un)
          return std::nullopt;

        auto canon = stringutils::canonicalizeStringifyInversePayload(*un);
        if (!canon || StringRef(*canon).trim() != StringRef(*un).trim()) {
          return std::nullopt;
        }

        newArg = std::move(*canon);

        // Normalize the old side into the same unstringified representation so
        // the later occurrence-consistency checks compare argument text to
        // argument text.
        auto oldUn = (*deps_.argTextRecovery).UnstringifyLiteralToArgText(oldText, true);
        if (oldUn)
          oldText = StringRef(*oldUn).trim();
      }

      if (!occIsStringify[i] && !m.pasteSpans.empty()) {
        bool argHasPaste = false;
        for (const auto &ps : m.pasteSpans) {
          if (ps.argIdx == argIdx) {
            argHasPaste = true;
            break;
          }
        }

        if (argHasPaste) {
          StringRef aSlice = (*deps_.sourceMapper).SliceASource(sp.begin, sp.end).trim();
          if (!aSlice.empty()) {
            size_t pos = baseArgText.find(aSlice);
            if (pos != StringRef::npos) {
              // Paste/lift case: the occurrence may represent only the pasted
              // segment inside a larger call-site argument. Replace that
              // original segment inside the full base argument rather than
              // replacing the whole argument with the pasted-token slice.
              std::string cand = baseArgText.substr(0, pos).str() +
                                 bSlice.str() +
                                 baseArgText.substr(pos + aSlice.size()).str();
              newArg = StringRef(cand).trim().str();
              materializedNewTextRange = std::nullopt;
            } else {
              // The pasted occurrence could not be located inside the original
              // call-site argument, so leave `newArg` as the direct B slice and
              // let the later consistency/validation checks decide whether it
              // is usable.
            }
          }
        }
      }

      // Record this occurrence-level old/new observation. If all observations
      // for this formal agree on one replacement, the args-only path can
      // rewrite the formal directly; disagreement triggers the tuple-forwarding
      // fallback.
      occObservations.push_back(
          OccObservation{oldText, newArg, materializedNewTextRange});
      if (!materializedNewTextRange) {
        sawUntrackedMaterializedNewTextRange = true;
      } else if (unifiedMaterializedNewTextRange) {
        unifiedMaterializedNewTextRange->first = std::min(
            unifiedMaterializedNewTextRange->first,
            materializedNewTextRange->first);
        unifiedMaterializedNewTextRange->second = std::max(
            unifiedMaterializedNewTextRange->second,
            materializedNewTextRange->second);
      } else {
        unifiedMaterializedNewTextRange = *materializedNewTextRange;
      }

      if (!unifiedNewArg)
        unifiedNewArg = newArg;
      else if (*unifiedNewArg != newArg)
        needTupleForwarding = true;
    }

    std::string finalNewArg;
    bool tupleForwarded = false;

    if (needTupleForwarding) {
      // Occurrence observations for this formal did not collapse to one uniform
      // replacement. Try the narrower tuple-forwarding proof before rejecting
      // the args-only rewrite outright.
      if (!tryTupleForwardedCallerTupleRewrite(argIdx, baseArgText,
                                               occObservations, finalNewArg)) {
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
      if (tryTupleForwardedCallerTupleRewrite(argIdx, baseArgText,
                                              occObservations, finalNewArg)) {
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
    if (!isMacroInvocationVariadicFormal(m, argIdx) && replacementIntroducesTopLevelComma(finalNewArg, (*deps_.lexLang)))
      return std::nullopt;

    auto tupleSliceConsistencyMatchesAllOccurrencesInB = [&]() -> bool {
      // Tuple-forwarded rewrites are slice-based: each old tuple element/slice
      // must consistently map to exactly one new spelling across all
      // observations.
      llvm::StringMap<std::string> newTextByOld;
      for (const auto &obs : occObservations) {
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

      // Validate the slice map against every standard occurrence of this formal
      // in B, not only the occurrence that originally triggered the rewrite.
      for (const auto &s : standardArgSpans) {
        if (s.argIdx != argIdx || s.kind != PPArgSpanKind::Standard)
          continue;

        StringRef oldSlice = (*deps_.sourceMapper).SliceASource(s.begin, s.end).trim();
        auto expectedIt = newTextByOld.find(oldSlice);
        if (expectedIt == newTextByOld.end())
          return false;

        auto bEnv = (*deps_.sourceMapper).MapAToBTokenEnvelopeByPPArgSpan(s);
        if (!bEnv)
          return false;

        size_t lo = bEnv->first;
        size_t hi = bEnv->second;

        // Reconstruct the same widened B envelope used during observation
        // collection, incorporating owned insertions and overlapping token
        // hunks for this occurrence.
        for (const auto &hk : tokenHunks) {
          if (auto owned = OccurrenceReplay().GetOwnedPureInsertionBRangeForArgSpan(
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
              s, std::make_pair(lo, hi), oldSlice);
          lo = grownEnv.first;
          hi = grownEnv.second;
        }

        StringRef tokText = (*deps_.sourceMapper).SliceBSource(lo, hi).trim();
        if (tokText != StringRef(expectedIt->second).trim())
          return false;
      }

      return true;
    };

    const bool matchesAllOccurrences =
        tupleForwarded
            ? tupleSliceConsistencyMatchesAllOccurrencesInB()
        : replayedStandardArgSpanFormalIndices
            ? standardArgReplacementMatchesAllReplayedOccurrencesInB(
                  argIdx, finalNewArg, tokenHunks)
            : MacroArgReplacementMatchesAllOccurrencesInB(
                  m, argIdx, baseArgText, finalNewArg, tokenHunks);

    if (!matchesAllOccurrences) {
      // The candidate replacement explained the local observations but failed
      // the global occurrence check. Before returning, gather tuple-specific
      // diagnostics when child tuple metadata exists for this argument.
      bool hasTupleChildForArg = false;
      for (const auto &cand : (*deps_.model).GetMacroInvocations()) {
        if (!cand.callerMacroId || *cand.callerMacroId != m.id)
          continue;
        if (cand.argTupleRefs.empty())
          continue;

        for (const auto &refs : cand.argTupleRefs) {
          for (const auto &ref : refs) {
            if (ref.callerParamIndex == argIdx) {
              hasTupleChildForArg = true;
              break;
            }
          }
          if (hasTupleChildForArg)
            break;
        }
        if (hasTupleChildForArg)
          break;
      }


      if (hasTupleChildForArg) {

        for (const auto &s : standardArgSpans) {
          if (s.argIdx != argIdx || s.kind != PPArgSpanKind::Standard)
            continue;

          auto bEnv = (*deps_.sourceMapper).MapAToBTokenEnvelopeByPPArgSpan(s);
          if (!bEnv) {
            continue;
          }

          size_t lo = bEnv->first;
          size_t hi = bEnv->second;
          SmallVector<std::string, 8> hunkEffects;

          // Rebuild and log the exact envelope-extension reasoning for this
          // occurrence: owned insertion hunks, overlapping hunks, or ignored
          // hunks.
          for (const auto &hk : tokenHunks) {
            if (auto owned = OccurrenceReplay().GetOwnedPureInsertionBRangeForArgSpan(
                    s, standardArgSpans, *bEnv, hk)) {
              hunkEffects.push_back(
                  formatv("owned {0} -> [{1},{2}) '{3}'", hk, owned->first,
                          owned->second,
                          stringutils::showWsWithClip(
                              (*deps_.sourceMapper).SliceBSource(owned->first, owned->second), 80))
                      .str());
              lo = std::min(lo, owned->first);
              hi = std::max(hi, owned->second);
              continue;
            }

            bool touches = false;
            if (hk.aStart != hk.aEnd)
              touches = (hk.aStart < s.end && hk.aEnd > s.begin);

            if (touches && hk.bStart < hk.bEnd) {
              hunkEffects.push_back(
                  formatv("overlap {0} -> [{1},{2}) '{3}'", hk,
                          (uint64_t)hk.bStart, (uint64_t)hk.bEnd,
                          stringutils::showWsWithClip(
                              (*deps_.sourceMapper).SliceBSource(hk.bStart, hk.bEnd), 80))
                      .str());
              lo = static_cast<size_t>(std::min<uint64_t>(lo, hk.bStart));
              hi = static_cast<size_t>(std::max<uint64_t>(hi, hk.bEnd));
            } else {
              // This hunk did not contribute to the B envelope for this
              // occurrence; keep it in the diagnostic output so missing/extra
              // hunk effects are visible when debugging tuple-forward failures.
              hunkEffects.push_back(formatv("ignored {0}", hk).str());
            }
          }

        }
      }

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

    if (finalMaterializedRange &&
        finalMaterializedRange->second <= finalNewArg.size()) {
      materializedRangeByArgIdx[argIdx] = *finalMaterializedRange;
    }
    replByArgIdx[argIdx] = std::move(finalNewArg);
  }

  // If nothing required replacement, there is no meaningful args-only patch to
  // emit.
  if (replByArgIdx.empty()) {
    return std::nullopt;
  }

  std::optional<InvocationRewriteWithRange> rewrite =
      BuildInvocationRewriteWithRange(actualRecoveryCtx, replByArgIdx,
                                      &materializedRangeByArgIdx);
  if (!rewrite)
    return std::nullopt;

  {
    MacroPatch patch{*m.invB, *m.invE, std::move(rewrite->text), m.id};
    StampInvocationRewriteMaterializedOutputRange(patch, *rewrite);
    // The materialized output byte range may remain narrowed to an inserted
    // payload inside one argument, but the target-PP proof for an
    // invocation-preserving macro repair is the B-side expansion envelope of
    // the whole macro owner.  Keeping the output byte range narrow is useful
    // for source-spelling edits; leaving the B-token range unstamped would make
    // append/pure-insertion repairs look theorem-incomplete even after the
    // occurrence replay above proved that the rewritten invocation regenerates
    // the edited expansion.
    StampMacroPatchWholeExpansionBRange(m, patch);
    SetArgsOnlyStandardProof(patch, m, /*wholeEnvelopeReplayValidated=*/false);
    return patch;
  }
}

std::optional<MacroPatch>
RefoldMacroPatchPlanner::BuildMacroInvocationPatchArgsOnly(
    const RefoldModel::MacroInvocation &m, const diffutils::Hunk &h,
    StringRef baseInvText) const {
  std::optional<InvocationActualLayout> recoveredActualLayout =
      RecoverInvocationActuals(m, baseInvText);
  if (!recoveredActualLayout)
    return std::nullopt;

  const ArgsOnlyPlanningContext planningCtx{m, h, baseInvText,
                                            *recoveredActualLayout};
  const ArrayRef<std::pair<size_t, size_t>> invArgRanges =
      recoveredActualLayout->rangePairs();
  const ArgsOnlyTemplateReplayContext argsOnlyTemplateCtx{m, baseInvText,
                                                          invArgRanges};

  // Definition-tape replay proves surfaces that ordinary non-empty occurrence
  // replay cannot see: empty actuals, zero-token formal slots, and __VA_OPT__
  // branch flips.  A miss is non-terminal, matching the former local lambda.
  if (auto replayPatch = TryDefinitionTapeReplayArgsOnlyPatch(planningCtx))
    return replayPatch;

  // The ordinary current-level template solver has higher priority than the
  // later paste/standard fallback phases, preserving the historical admission
  // order inside the monolithic planner.
  if (auto templatePatch = TryTemplateSolvedArgsOnlyPatch(argsOnlyTemplateCtx))
    return templatePatch;

  // Paste-aware replay has two non-success outcomes: continue when the hunk was
  // not accepted by a paste-specialized proof, or reject when a touched paste
  // surface was shown to be invalid and the old code failed closed.
  ArgsOnlyPatchAttempt pasteAttempt = BuildPasteAwareArgsOnlyPatch(planningCtx);
  if (pasteAttempt.disposition == ArgsOnlyPatchAttempt::Disposition::Accepted)
    return std::move(pasteAttempt.patch);
  if (pasteAttempt.disposition == ArgsOnlyPatchAttempt::Disposition::Reject)
    return std::nullopt;

  return BuildStandardArgsOnlyPatch(planningCtx);
}


std::optional<WholeCoverPlan> RefoldMacroPatchPlanner::ComputeWholeCoverPlan(
    const RefoldModel::MacroInvocation &m) const {
  auto range = GetWholeCoverATokRange(m);
  if (!range)
    return std::nullopt;

  WholeCoverPlan plan;
  plan.covLoA = range->first;
  plan.covHiA = range->second;

  // Function-like macros with no formal parameters can have a useful body-span
  // range that is narrower than the producer's invocation cover. Record that
  // distinction so diagnostics can explain why the whole-cover domain came from
  // body material rather than the raw cover.
  plan.usedBodyRange =
      (m.subkind == "func" && m.defParams.empty() && !m.bodySpans.empty() &&
       (plan.covLoA != m.cover.begin || plan.covHiA != m.cover.end));

  // Whole-cover replay is admissible only when the selected A range is
  // explained by this invocation's own detailed macro provenance.  The old
  // nested/coarse-span containment fallback is gone: descendant spans
  // may justify a structure-preserving DAG lift, but they do not by themselves
  // prove that this callsite can be replaced by realized whole-cover output.
  plan.selfContained = RefoldMacroWholeCoverProof::MacroWholeCoverIsSelfContained(m);
  if (!plan.selfContained) {
    return std::nullopt;
  }

  // Map the accepted A-token cover into B while preserving boundary insertions.
  // This gives the raw B envelope that the whole-cover candidate will replay.
  auto bEnv = (*deps_.sourceMapper).MapATokRangeAToBTokenEnvelopePreserveBoundaryInsertions(
      plan.covLoA, plan.covHiA);
  if (!bEnv)
    return std::nullopt;

  plan.rawBTokStart = bEnv->first;
  plan.rawBTokEnd = bEnv->second;
  plan.bTokStart = plan.rawBTokStart;
  plan.bTokEnd = plan.rawBTokEnd;
  if (plan.bTokEnd <= plan.bTokStart)
    return std::nullopt;

  // If the mapped B envelope starts one token too far to the right, pull it
  // left when the immediately preceding B token matches the first A cover
  // token. This repairs boundary-placement drift without choosing by lexical
  // neighbor preference: the token must exactly be the cover boundary token.
  if (plan.covLoA < deps_.aToks.size() && plan.bTokStart < deps_.bToks.size()) {
    StringRef want = deps_.aToks[static_cast<size_t>(plan.covLoA)].spelling;
    if (!want.empty()) {
      if (deps_.bToks[plan.bTokStart].spelling != want && plan.bTokStart > 0 &&
          deps_.bToks[plan.bTokStart - 1].spelling == want) {
        plan.bTokStart--;
        plan.adjustedLeft = true;
      }
    }
  }

  // Symmetrically, if the mapped B envelope includes one token too far to the
  // right, contract it when the previous B token matches the last A cover
  // token. This keeps the replacement envelope aligned with the macro-owned
  // cover.
  if (plan.covHiA > 0 && (plan.covHiA - 1) < deps_.aToks.size() &&
      plan.bTokEnd > 0 && (plan.bTokEnd - 1) < deps_.bToks.size()) {
    StringRef want = deps_.aToks[static_cast<size_t>(plan.covHiA - 1)].spelling;
    if (!want.empty()) {
      // Do not contract a whole-cover replacement across a trailing B comment.
      // Comments are source trivia, not macro-body delimiter tokens; clipping
      // one out here splits an otherwise line-local insertion and forces a
      // spurious #line resynchronization before the comment text.
      const bool rightIsComment = deps_.bToks[plan.bTokEnd - 1].kind == "comment";
      if (!rightIsComment && deps_.bToks[plan.bTokEnd - 1].spelling != want &&
          plan.bTokEnd >= 2 && deps_.bToks[plan.bTokEnd - 2].spelling == want) {
        plan.bTokEnd--;
        plan.adjustedRight = true;
      }
    }
  }

  if (plan.bTokEnd <= plan.bTokStart)
    return std::nullopt;

  // Clip away B text that is already claimed by stronger/narrower accepted
  // material before using the whole-cover text. The raw vs. clipped comparison
  // records whether this candidate had to yield to existing claims.
  std::string unclipped =
      (*deps_.sourceMapper).SliceBSource(plan.bTokStart, plan.bTokEnd).str();
  std::string clipped = deps_.bInsertionLedger->SliceBSourceClippedAgainstClaims(
      plan.bTokStart, plan.bTokEnd);
  plan.claimsClipped = (unclipped != clipped);
  plan.clippedText = StringRef(clipped).trim().str();

  return plan;
}

bool RefoldMacroPatchPlanner::WholeCoverPatchMatchesPlan(const MacroPatch &patch,
                                              const WholeCoverPlan &plan,
                                              uint64_t rootMacroId) const {
  if (patch.proof.kind != MacroPatchProofKind::WholeCoverRealization ||
      patch.proof.preservesInvocationStructure ||
      patch.proof.proofRootMacroId != rootMacroId)
    return false;
  return patch.wholeCoverUsedBodyRange == plan.usedBodyRange &&
         patch.wholeCoverSelfContained == plan.selfContained &&
         patch.wholeCoverAdjustedLeft == plan.adjustedLeft &&
         patch.wholeCoverAdjustedRight == plan.adjustedRight &&
         patch.wholeCoverClaimsClipped == plan.claimsClipped &&
         patch.wholeCoverALo == plan.covLoA &&
         patch.wholeCoverAHi == plan.covHiA &&
         patch.wholeCoverBRawLo == plan.rawBTokStart &&
         patch.wholeCoverBRawHi == plan.rawBTokEnd &&
         patch.wholeCoverBAdjLo == plan.bTokStart &&
         patch.wholeCoverBAdjHi == plan.bTokEnd;
}

Owner
RefoldMacroPatchPlanner::NormalizeHunkOwnerForPatch(StringRef tuPath,
                                         const diffutils::Hunk &h) const {
  Owner owner =
      deps_.ownerClassifier->ClassifyOwnerWithSegments(tuPath, h);
  const bool mapsToTU =
      deps_.ownerClassifier->HunkMapsToTU(h.aStart, h.aEnd, tuPath);
  if (mapsToTU)
    return Owner::TU(owner.condArmId);
  if (owner.kind == OwnerKind::Include && owner.includeId)
    return Owner::Include(*owner.includeId, owner.condArmId);
  return Owner::Unknown();
}

bool RefoldMacroPatchPlanner::MacroPatchOwnerMatches(const MacroPatch &patch,
                                          const Owner &owner) const {
  // Only compare against a concrete, single-owner certificate. Mixed-owner
  // patches cannot be treated as belonging to one TU/include/conditional owner.
  if (!patch.ownerCertPresent || patch.ownerMixedWitness)
    return false;
  if (!owner.IsTU() && !owner.IsInclude())
    return false;

  const uint8_t wantKind = owner.IsTU() ? 1 : 2;
  if (patch.ownerKindCode != wantKind)
    return false;

  // Match the serialized owner certificate exactly: owner kind, include ID, and
  // optional conditional-arm identity must all agree.
  const uint64_t wantInclude = owner.includeId.value_or(0);
  if (patch.ownerIncludeIdCert != wantInclude)
    return false;

  if (patch.ownerHasCondArmCert != owner.condArmId.has_value())
    return false;
  if (patch.ownerHasCondArmCert &&
      patch.ownerCondArmIdCert != owner.condArmId.value())
    return false;

  return true;
}

void RefoldMacroPatchPlanner::CarryMacroPatchOwnerCertificate(
    MacroPatch &dst, const MacroPatch &src) const {
  dst.ownerCertPresent = src.ownerCertPresent;
  dst.ownerMixedWitness = src.ownerMixedWitness;
  dst.ownerKindCode = src.ownerKindCode;
  dst.ownerIncludeIdCert = src.ownerIncludeIdCert;
  dst.ownerHasCondArmCert = src.ownerHasCondArmCert;
  dst.ownerCondArmIdCert = src.ownerCondArmIdCert;
  dst.ownerWitnessCount = src.ownerWitnessCount;
}

void RefoldMacroPatchPlanner::StampMacroPatchOwnerWitness(
    MacroPatch &patch, const Owner &owner) const {
  // Only TU/include owners are representable in the compact legacy patch
  // certificate. More specific owners (macro directives, pragma islands,
  // line-control islands) must be discharged by their own proof class rather
  // than being lossy-serialized as kind code 0.
  if (!owner.IsTU() && !owner.IsInclude())
    return;

  // Serialize the owner into the compact certificate fields stored on
  // MacroPatch. This lets later merge/selection checks compare owner witnesses
  // without retaining the full Owner object.
  const uint8_t kindCode = owner.IsTU() ? 1 : 2;
  const uint64_t includeId = owner.includeId.value_or(0);
  const bool hasCondArm = owner.condArmId.has_value();
  const uint64_t condArmId = hasCondArm ? *owner.condArmId : 0;

  // First concrete witness initializes the certificate.
  if (!patch.ownerCertPresent) {
    patch.ownerCertPresent = true;
    patch.ownerMixedWitness = false;
    patch.ownerKindCode = kindCode;
    patch.ownerIncludeIdCert = includeId;
    patch.ownerHasCondArmCert = hasCondArm;
    patch.ownerCondArmIdCert = condArmId;
    patch.ownerWitnessCount = 1;
    return;
  }

  // Additional witnesses must match the original certificate exactly. If any
  // differ, preserve the witness count but mark the patch as mixed-owner so it
  // cannot later be treated as a single-owner rewrite.
  ++patch.ownerWitnessCount;
  if (patch.ownerKindCode != kindCode ||
      patch.ownerIncludeIdCert != includeId ||
      patch.ownerHasCondArmCert != hasCondArm ||
      (hasCondArm && patch.ownerCondArmIdCert != condArmId)) {
    patch.ownerMixedWitness = true;
  }
}

// Whole-cover realization is admitted only by RefoldMacroWholeCoverProof::MacroWholeCoverIsSelfContained(),
// which proves the selected A-token cover directly from the invocation's own
// detailed provenance spans before stamping the shared OwnerRealizationProof. A
// nested child whose cover needs caller/descendant aggregation must instead be
// handled by structure-preserving DAG lifting or by a wider owner realization;
// coarse descendant span aggregation is not a separate proof class.
std::optional<MacroPatch>
RefoldMacroPatchPlanner::TryCounterLiteralWholeCoverPatch(
    const RefoldModel::MacroInvocation &m, const diffutils::Hunk &h,
    uint64_t invocationStart, uint64_t invocationEnd) const {
  // The only robust representation of an edited (or forced) __COUNTER__
  // expansion is to replace the invocation spelling with the B-side literal
  // token(s) for this specific occurrence. Do NOT whole-cover expand using the
  // macro cover, which may span multiple occurrences when a header is included
  // multiple times.
  if (m.name != "__COUNTER__")
    return std::nullopt;

  std::optional<std::string> repl;
  std::optional<std::pair<uint64_t, uint64_t>> counterBTokenRange;
  if (h.bEnd > h.bStart) {
    counterBTokenRange = std::make_pair(static_cast<uint64_t>(h.bStart),
                                        static_cast<uint64_t>(h.bEnd));
    repl = (*deps_.sourceMapper)
               .SliceBSource(static_cast<size_t>(h.bStart),
                             static_cast<size_t>(h.bEnd))
               .trim()
               .str();
  } else {
    auto bEnv =
        (*deps_.sourceMapper).MapATokRangeAToBTokenEnvelope(h.aStart, h.aEnd);
    if (bEnv && bEnv->second > bEnv->first) {
      counterBTokenRange =
          std::make_pair(static_cast<uint64_t>(bEnv->first),
                         static_cast<uint64_t>(bEnv->second));
      repl =
          (*deps_.sourceMapper).SliceBSource(bEnv->first, bEnv->second).trim().str();
    }
  }
  if (!repl)
    return std::nullopt;

  CounterEventIdentity event = (*deps_.macroTopology).BuildCounterEventIdentity(
      m, /*occurrenceOrdinal=*/0, h.aStart, h.aEnd, m.ownerIncludeId);
  event.expectedBValue = *repl;
  const OwnerStateBoundary boundary =
      GetOwnerStateProof().CounterStateBoundaryForEvent(event);
  const std::string detail =
      llvm::formatv("literalized direct __COUNTER__ invocation #{0} "
                    "A=[{1},{2})",
                    m.id, h.aStart, h.aEnd)
          .str();
  SuffixStabilityWitness counterWitness =
      GetOwnerStateProof().BuildStateTransitionWitness(
          SuffixStabilityWitnessKind::Literalization,
          OwnerStateComponent::Counter, boundary,
          llvm::formatv(
              "{0}; {1}", detail,
              GetOwnerStateProof().FormatCounterEventForWitness(event))
              .str());
  (void)GetOwnerStateProof().CheckStateTransitionAcrossEditBoundary(
      boundary, OwnerStateComponent::Counter, StateMutationKind::Literalized,
      counterWitness, "counter", detail, /*requireKnownObserver=*/false);

  MacroPatch patch{invocationStart, invocationEnd, std::move(*repl), m.id};
  if (counterBTokenRange &&
      counterBTokenRange->first <= counterBTokenRange->second &&
      counterBTokenRange->second <= deps_.bToks.size()) {
    patch.hasMaterializedBTokenRange = true;
    patch.materializedBTokStart = counterBTokenRange->first;
    patch.materializedBTokEnd = counterBTokenRange->second;
  }
  MacroPatchProof proof = GetProofLattice().MakeMacroPatchProof(
      MacroPatchProofKind::CounterLiteral,
      /*preservesInvocationStructure=*/false, m.id);
  CounterStateWitness counterState;
  counterState.hasCounterEvents = true;
  counterState.counterOrderKnown = true;
  counterState.suffixStateStable = true;
  counterState.literalizationStable = true;
  counterState.counterConsumptionCount = 1;
  counterState.counterMutationCount = 1;
  if (counterWitness.kind != SuffixStabilityWitnessKind::None)
    counterState.preservedSuffixObserverCount = 1;
  counterState.hasExpectedBValues = true;
  counterState.expectedBValueCount = 1;
  counterState.consumptionSignature =
      GetOwnerStateProof().FormatCounterEventForWitness(event);
  counterState.orderSignature =
      llvm::formatv("ordinal={0}:macro={1}:A=[{2},{3})",
                    event.occurrenceOrdinal, event.macroInvocationId,
                    event.aTokenBegin, event.aTokenEnd)
          .str();
  counterState.suffixValueSignature =
      llvm::formatv(
          "expected={0}",
          RefoldProofLattice::FormatWitnessTraceHash(*event.expectedBValue))
          .str();
  counterState.suffixObserverSignature =
      llvm::formatv("literalization:{0}", counterWitness.kind).str();
  proof.suffixStability = std::move(counterWitness);
  proof.counterState = std::move(counterState);
  GetProofLattice().SetMacroPatchProof(patch, std::move(proof));
  return patch;
}

std::optional<MacroPatch>
RefoldMacroPatchPlanner::BuildMacroInvocationPatchWholeCover(
    const RefoldModel::MacroInvocation &m, const diffutils::Hunk &h,
    StringRef baseInvText,
    const DenseMap<std::optional<uint64_t>, DenseMap<uint64_t, MacroPatch>>
        &patchMap) const {
  return BuildMacroInvocationPatchWholeCover(
      m, h, baseInvText, patchMap, ExistingMacroPatchContext{});
}

std::optional<MacroPatch>
RefoldMacroPatchPlanner::BuildMacroInvocationPatchWholeCover(
    const RefoldModel::MacroInvocation &m, const diffutils::Hunk &h,
    StringRef baseInvText,
    const DenseMap<std::optional<uint64_t>, DenseMap<uint64_t, MacroPatch>>
        &patchMap,
    RefoldMacroPatchPlanner::ExistingMacroPatchContext existingContext) const {
  // Strategy (in priority order):
  //   1) Prefer an args-only rewrite of the invocation spelling when the edit
  //      is fully contained within argument-like spans.
  //   2) For function-like macros, attempt conservative DAG-chained args-only
  //      lifting from a nested callee invocation back to this callsite.
  //   3) Admit whole-cover realization only through the explicit
  //      RefoldMacroWholeCoverProof::MacroWholeCoverIsSelfContained() + OwnerRealizationProof path.
  // The invocation byte span in the owning file must be known.
  const auto invStart = m.invB;
  const auto invEnd = m.invE;
  if (!invStart || !invEnd || *invEnd < *invStart)
    return std::nullopt;

  // Special-case: __COUNTER__.
  if (auto counterPatch =
          TryCounterLiteralWholeCoverPatch(m, h, *invStart, *invEnd))
    return counterPatch;

  const Owner currentPatchOwner =
      NormalizeHunkOwnerForPatch((*deps_.model).GetSourcePath(), h);

  // Set when two different concrete subtree-backed witnesses for the same
  // root disagree on an overlapping expected-root formal rewrite. Once this is
  // set for the current pass, we suppress same-root structure-preserving reuse
  // and let whole-cover realization compete for the root instead of
  // collapsing incompatible witnesses into one root replay.
  bool conflictingConcreteSubtreeWitnessForcesWholeCover = false;

  // Existing-patch reuse is computed before the rest of candidate assembly.
  // Keep local aliases for the rest of this large planner so this remains
  // behavior-preserving and does not change candidate discovery order.
  MacroPatchReuseAdmissionContext reuseAdmissionCtx =
      RecoverWholeCoverReuseContext(m, currentPatchOwner, *invStart, *invEnd,
                                    patchMap, existingContext);
  const MacroPatch *&existingPatch = reuseAdmissionCtx.existingPatch;
  const MacroPatch *&existingExpandedPatch =
      reuseAdmissionCtx.existingExpandedPatch;
  bool &existingIsCallsite = reuseAdmissionCtx.existingIsCallsite;

  const diffutils::Hunk hEff = trimCommonEdgeTokens(h, deps_.aToks, deps_.bToks);

  // 1) Prefer args-only patching when safe and fully validated. Treat normal
  //    arg spans, stringify spans, and paste spans as "argument-like"
  //    occurrences.

  auto isVariadicFormalInInvocation =
      [&](const RefoldModel::MacroInvocation &inv, uint32_t idx) -> bool {
    return idx < inv.defParams.size() && inv.defParams[idx].variadic;
  };

  // Important arbitration rule: a direct root args-only patch is not returned
  // immediately. We let the DAG-chained certificate path run afterwards and
  // prefer it when it can produce a unique validated callsite rewrite, because
  // that path can preserve deeper nested macro structure that a direct root
  // argument rewrite would flatten.
  std::optional<MacroPatch> argsOnlyCandidate;

  // Keep the preferred DAG root replay alive until the shared final macro
  // selector runs. Carry this candidate through the same final arbitration as
  // the other accepted macro outcomes instead of returning it immediately from
  // the local DAG competition block.
  std::optional<MacroPatch> dagRootCandidate;
  bool reuseExistingCallsitePatch = false;
  bool existingCallsitePatchAbsorbedByDirectCandidate = false;

  SmallVector<RefoldModel::PPArgSpan, 16> argLikeSpans;
  argLikeSpans.append(m.argSpans.begin(), m.argSpans.end());
  argLikeSpans.append(m.stringifySpans.begin(), m.stringifySpans.end());
  argLikeSpans.append(m.pasteSpans.begin(), m.pasteSpans.end());

  // Direct root args-only recovery computes both the root candidate and the
  // direct-root surface fact used later to decide whether unsupported
  // descendants force fallback.
  const WholeCoverArgsOnlyCandidateContext argsOnlyPhaseCtx{
      m, hEff, baseInvText, argLikeSpans, reuseAdmissionCtx};
  const WholeCoverArgsOnlyCandidateResult argsOnlyPhase =
      BuildWholeCoverArgsOnlyCandidate(argsOnlyPhaseCtx);
  argsOnlyCandidate = argsOnlyPhase.argsOnlyCandidate;
  reuseExistingCallsitePatch = argsOnlyPhase.reuseExistingCallsitePatch;
  const bool rootHasDirectArgLikeSurface =
      argsOnlyPhase.rootHasDirectArgLikeSurface;

  // 1b) Conservative DAG chaining: if the edited A-span lies within this
  //     invocation's cover but not within one of its direct argument-like
  //     spans, attempt to lift the edit from a nested callee invocation back
  //     to this callsite's arguments.
  //
  // Policy:
  //   * Every intermediate hop must be proven by arg_refs/template inversion.
  //   * A hop is allowed whenever the child argument text admits a unique
  //     inverse through arg_refs back to the contributing caller formals.
  //   * If the inverse is ambiguous, unsupported, or does not match the
  //     observed text, lifting fails and we conservatively keep the subtree
  //     expanded.
  if (m.subkind == "func" && hasLiteralMacroCalleeOrigin(m)) {
    bool directRootPreservationInadmissible = false;
    auto tryDAGChainedArgsOnly = [&]() -> std::optional<MacroPatch> {
      // --- Root invocation parsing preconditions -----------------------------
      //
      // We can only "lift" edits back into the current root invocation (m) if
      // we can reliably parse *its current spelling* into formal argument byte
      // ranges. This must be the callsite text (not some expanded body text).
      StringRef invSpanText =
          !baseInvText.empty()
              ? baseInvText
              : (m.invText ? StringRef(*m.invText) : StringRef(""));

      if (!RefoldLineObserverLayout::InvocationSpanMatchesCallsitePrefix(invSpanText, m)) {
        return std::nullopt;
      }

      // Parse the byte ranges of each *formal argument* within invSpanText.
      // This is the target surface we will rewrite if lifting succeeds.
      auto invArgRangesOpt =
          GetMacroInvocationFormalArgContentRanges(m, invSpanText);
      if (!invArgRangesOpt) {
        return std::nullopt;
      }
      const auto &invArgRanges = *invArgRangesOpt;
      const size_t numArgs = invArgRanges.size();

      // --- DAG traversal lookup structures ----------------------------------
      //
      // We lift edits along caller/callee edges between MacroInvocation items.
      // Build a fast lookup map from invocation id -> invocation* so we can
      // climb parent pointers without repeated O(N) scans.
      DenseMap<uint64_t, const RefoldModel::MacroInvocation *> invById;
      invById.reserve((*deps_.model).GetMacroInvocations().size());
      for (const auto &mi : (*deps_.model).GetMacroInvocations())
        invById[mi.id] = &mi;

      const MacroSubtreeReplayValidationContext subtreeValidationCtx{
          m, invSpanText, invArgRanges, invById};
      const RefoldModel::MacroInvocation &rootInvocation =
          subtreeValidationCtx.rootInvocation;
      StringRef rootInvocationText =
          subtreeValidationCtx.rootInvocationText;
      ArrayRef<std::pair<size_t, size_t>> rootInvocationArgRanges =
          subtreeValidationCtx.rootInvocationArgRanges;

      // Collect all "argument-like" spans for an invocation:
      //   * standard argument spans
      //   * stringify-derived spans
      //   * paste sub-spans
      //
      // These are the only spans we are willing to treat as "editable
      // arguments" when detecting a leaf edit.
      auto gatherArgLike = [&](const RefoldModel::MacroInvocation &mi,
                               SmallVectorImpl<RefoldModel::PPArgSpan> &out) {
        out.clear();
        out.append(mi.argSpans.begin(), mi.argSpans.end());
        out.append(mi.stringifySpans.begin(), mi.stringifySpans.end());
        out.append(mi.pasteSpans.begin(), mi.pasteSpans.end());
      };

      // SpanText is the extracted argument text for a span, plus a reliability
      // bit. Reliability is important for paste spans when the edited B token
      // changes length; we may have to fall back to clamped slicing which is
      // ambiguous.
      struct SpanText {
        std::string text;
        bool reliable; // true iff derived without ambiguous fallback logic
      };

      // Extract the argument text corresponding to a PPArgSpan, either from A
      // (fromB=false) or from B (fromB=true).
      //
      // Special handling:
      //   * Paste spans may refer to a token-internal [byteBegin, byteEnd)
      //   range.
      //     For B-side paste spans, that range may no longer align if the
      //     pasted token changed. We attempt to re-derive the segment using
      //     A-side prefix/suffix preservation; otherwise we clamp and mark
      //     unreliable.
      auto extractSpanText = [&](const RefoldModel::PPArgSpan &sp,
                                 bool fromB) -> std::optional<SpanText> {
        if (sp.end <= sp.begin)
          return std::nullopt;

        // --- A-side extraction: exact bytes from the original pp token stream.
        if (!fromB) {
          StringRef a = (*deps_.sourceMapper).SliceASource(static_cast<size_t>(sp.begin),
                                     static_cast<size_t>(sp.end));
          if ((sp.kind == PPArgSpanKind::Paste ||
             sp.kind == PPArgSpanKind::Stringify) &&
            sp.byteBegin && sp.byteEnd) {
            // Paste spans and wrapped stringify spans can identify a subrange
            // within a single output token.
            if ((sp.end - sp.begin) != 1)
              return std::nullopt;
            const uint64_t bb = *sp.byteBegin;
            const uint64_t be = *sp.byteEnd;
            if (be < bb || be > static_cast<uint64_t>(a.size()))
              return std::nullopt;
            a = a.slice(static_cast<size_t>(bb), static_cast<size_t>(be));
          }
          return SpanText{a.trim().str(), /*reliable=*/true};
        }

        // --- B-side extraction: map the A-span to its B envelope and slice B.
        auto bEnv = (*deps_.sourceMapper).MapAToBTokenEnvelopeByPPArgSpan(sp);
        if (!bEnv)
          return std::nullopt;
        if (bEnv->second <= bEnv->first)
          return std::nullopt;
        StringRef b = (*deps_.sourceMapper).SliceBSource(bEnv->first, bEnv->second);
        bool reliable = true;

        if ((sp.kind == PPArgSpanKind::Paste ||
             sp.kind == PPArgSpanKind::Stringify) &&
            sp.byteBegin && sp.byteEnd) {
          if ((bEnv->second - bEnv->first) != 1)
            return std::nullopt;
          const uint64_t bb = *sp.byteBegin;
          const uint64_t be = *sp.byteEnd;

          // Token-internal byte ranges are computed from the A-side token
          // spelling. If the B-side token changes length (for example L"hello"
          // -> L"goodbye" for wrapped stringify, or any pasted token rewrite),
          // using the raw [bb,be) slice can truncate the changed segment.
          // Re-derive the B-side segment by peeling any unchanged prefix/suffix
          // when possible.
          StringRef aTok = (*deps_.sourceMapper).SliceASource(static_cast<size_t>(sp.begin),
                                        static_cast<size_t>(sp.end));
          if (be < bb || be > static_cast<uint64_t>(aTok.size()))
            return std::nullopt;
          StringRef aPref = aTok.take_front(static_cast<size_t>(bb));
          StringRef aSuff = aTok.drop_front(static_cast<size_t>(be));
          if (b.starts_with(aPref) && b.ends_with(aSuff) &&
              b.size() >= aPref.size() + aSuff.size()) {
            b = b.slice(aPref.size(), b.size() - aSuff.size());
          } else {
            // The normal case: split the rewritten core around the original
            // literal delimiters and require a unique segmentation.
            reliable = false;
            const uint64_t bbC = std::min<uint64_t>(bb, b.size());
            const uint64_t beC = std::min<uint64_t>(be, b.size());
            if (beC < bbC)
              return std::nullopt;
            b = b.slice(static_cast<size_t>(bbC), static_cast<size_t>(beC));
          }
        }
        return SpanText{b.trim().str(), reliable};
      };

      // Normalize text to a canonical "argument text" for comparison/lifting.
      //
      // - Stringify spans: compare unescaped payload (so escaping/quoting
      //   choices don't create spurious diffs).
      // - Paste spans: usually raw token text; however if a stringify
      // participates
      //   in paste (e.g. L## #x), the pasted token includes quotes even though
      //   the callsite argument does not. Only then do we unstringify.
      auto normalizeLiftText =
          [&](const RefoldModel::MacroInvocation *inv,
              const RefoldModel::PPArgSpan &sp, StringRef raw0,
              bool allowTopLevelComma) -> std::optional<std::string> {
        StringRef raw = raw0.trim();

        // For stringify spans, compare against the de-escaped payload so that
        // quote/escape choices do not create false diffs.
        if (sp.kind == PPArgSpanKind::Stringify) {
          auto un = (*deps_.argTextRecovery).UnstringifyLiteralToArgText(
              raw, allowTopLevelComma);
          if (!un)
            return std::nullopt;
          return *un;
        }

        // Paste spans are normally token text (identifiers, numbers, string
        // literals, etc.). However, when a stringified argument participates in
        // token pasting (e.g. L## #x), the pasted token will contain quotes
        // even though the invocation argument does not. Only in that case
        // should we unstringify.
        if (sp.kind == PPArgSpanKind::Paste && sp.byteBegin && sp.byteEnd &&
            raw.find('"') != StringRef::npos) {
          bool invArgHasQuote = false;
          if (inv && inv->invText && inv->invB &&
              sp.argIdx < inv->invArgRanges.size()) {
            const auto &rng = inv->invArgRanges[sp.argIdx];
            if (rng.first && rng.second && *rng.first <= *rng.second &&
                *rng.first >= *inv->invB) {
              const uint64_t relB = *rng.first - *inv->invB;
              const uint64_t relE = *rng.second - *inv->invB;
              if (relE >= relB && relE <= inv->invText->size()) {
                StringRef invArg = StringRef(*inv->invText).slice(relB, relE);
                invArgHasQuote = invArg.find('"') != StringRef::npos;
              }
            }
          }
          if (!invArgHasQuote) {
            auto un = (*deps_.argTextRecovery).UnstringifyLiteralToArgText(raw);
            if (!un)
              return std::nullopt;
            return *un;
          }
        }

        return raw.str();
      };

      // Drop malformed or out-of-bounds argument-like spans before using them
      // for hunk containment checks. Invalid producer spans must not become
      // proof witnesses for args-only rewrite selection.
      auto sanitizeArgLikeSpans =
          [&](SmallVectorImpl<RefoldModel::PPArgSpan> &spans) {
            SmallVector<RefoldModel::PPArgSpan, 8> valid;
            valid.reserve(spans.size());
            const uint64_t maxATokCount = static_cast<uint64_t>(deps_.aToks.size());
            for (const auto &sp : spans) {
              if (sp.end <= sp.begin)
                continue;
              if (sp.begin >= maxATokCount || sp.end > maxATokCount)
                continue;
              valid.push_back(sp);
            }
            spans.assign(valid.begin(), valid.end());
          };

      // Return true when the hunk's A-side interval is fully covered by macro
      // body spans, recording which body spans it touches. Zero-width insertion
      // hunks are accepted only when they sit exactly on a body-span boundary.
      [[maybe_unused]] auto hunkWithinBodySpans =
          [&](const diffutils::Hunk &hh,
              ArrayRef<RefoldModel::PPSpan> bodySpans,
              SmallVectorImpl<uint32_t> &touchedBodyIdxs) -> bool {
        touchedBodyIdxs.clear();

        const uint64_t a0 = hh.aStart;
        const uint64_t a1 = hh.aEnd;

        if (a0 == a1) {
          // Pure insertions have no A tokens to test for containment. Treat
          // them as body-owned only when the insertion frontier coincides with
          // a recorded body span boundary.
          for (size_t i = 0; i < bodySpans.size(); ++i) {
            const auto &s = bodySpans[i];
            if (a0 == s.begin || a0 == s.end) {
              touchedBodyIdxs.push_back(static_cast<uint32_t>(i));
              return true;
            }
          }
          return false;
        }

        bool any = false;
        for (uint64_t a = a0; a < a1; ++a) {
          bool inSome = false;

          // Every A token in the hunk must be covered by at least one body
          // span. The touched span list is deduplicated because overlapping
          // body spans may cover the same token.
          for (size_t i = 0; i < bodySpans.size(); ++i) {
            const auto &s = bodySpans[i];
            if (a >= s.begin && a < s.end) {
              if (!llvm::is_contained(touchedBodyIdxs,
                                      static_cast<uint32_t>(i)))
                touchedBodyIdxs.push_back(static_cast<uint32_t>(i));
              inSome = true;
              any = true;
            }
          }

          // A single uncovered token means the hunk is not fully body-local.
          if (!inSome)
            return false;
        }

        return any;
      };

      // --- Leaf candidates touched by this hunk ------------------------------
      //
      // We search for descendant invocations whose *argument-like spans* are
      // fully covered by the hunk and exhibit an A->B text difference.
      //
      // We prefer deeper leaves (closest to the actual edited text), because
      // lifting from a deeper leaf tends to be more local and less ambiguous.
      struct LeafCandidate {
        const RefoldModel::MacroInvocation *inv;
        unsigned depth;             // distance from leaf to root (m): 1..N
        uint64_t smallestSpanBytes; // tie-breaker: prefer more local spans
        SmallVector<RefoldModel::PPArgSpan, 8> argLike;
        SmallVector<char, 8> touched;
      };

      struct SplitInsertionRootCandidate {
        MacroPatch patch;
        SmallVector<uint32_t, 8> deferOccurrenceArgIdxs;
      };

      SmallVector<SplitInsertionRootCandidate, 8>
          splitInsertionRootCandidates;

      if (hEff.aStart == hEff.aEnd && !(*deps_.abTokHunks).empty()) {
        // Collect sibling pure-insertion hunks inside the same macro cover.
        // Those partner insertions are later used to synthesize a wider
        // envelope for split insertion edits that are not explainable from the
        // current hunk alone.
        SmallVector<diffutils::Hunk, 8> partnerInsertions;
        for (const auto &hh : (*deps_.abTokHunks)) {
          // Only pure insertions can serve as the second frontier of a
          // split-insertion envelope.
          if (hh.aStart != hh.aEnd)
            continue;
          if (hh.aStart < m.cover.begin || hh.aEnd > m.cover.end)
            continue;

          // Do not pair the hunk with itself.
          if (hh.aStart == hEff.aStart && hh.bStart == hEff.bStart &&
              hh.bEnd == hEff.bEnd)
            continue;

          partnerInsertions.push_back(hh);
        }

        for (const auto &partner : partnerInsertions) {
          // Combine the current insertion with its partner to see whether the
          // pair exposes an argument-local edit envelope. The trimmed envelope is
          // the proof candidate passed to the args-only builder.
          const diffutils::Hunk env =
              buildCombinedInsertionEnvelope(hEff, partner);
          const diffutils::Hunk envTrim = trimCommonEdgeTokens(env, deps_.aToks, deps_.bToks);

          SmallVector<char, 16> envTrimRootTouched(argLikeSpans.size(), 0);
          const bool envTrimRootWithinArgLike =
              !argLikeSpans.empty() &&
              (*deps_.sourceMapper).HunkFullyWithinArgSpans(envTrim, argLikeSpans,
                                      envTrimRootTouched);

          std::optional<MacroPatch> pairRootPatch;
          if (!argLikeSpans.empty() && envTrimRootWithinArgLike &&
              RefoldLineObserverLayout::InvocationSpanMatchesCallsitePrefix(invSpanText, m)) {
            // Once the paired insertion envelope trims down to a valid
            // argument-local hunk, delegate to the standard args-only builder
            // for the actual rewrite and validation.
            pairRootPatch =
                BuildMacroInvocationPatchArgsOnly(m, envTrim, baseInvText);
          }

          if (pairRootPatch) {
            SplitInsertionRootCandidate candidate;
            // Preserve the already-constructed full root-callsite replacement
            // so it can be validated and merged later through the normal DAG
            // root candidate path.
            candidate.patch = std::move(*pairRootPatch);

            // Collect the root formal argument indices touched by the trimmed
            // combined insertion envelope. We intentionally store formal arg
            // indices here, not raw arg-like span indices, because a single
            // formal may appear multiple times at the root callsite.
            for (size_t idx = 0; idx < envTrimRootTouched.size(); ++idx) {
              // Ignore untouched spans and any defensive out-of-range cases.
              if (!envTrimRootTouched[idx] || idx >= argLikeSpans.size())
                continue;
              const uint32_t argIdx = argLikeSpans[idx].argIdx;
              if (argIdx >= numArgs)
                continue;

              // Defer occurrence-level consistency checks for each touched root
              // formal exactly once. The later DAG validation step will use
              // this set to avoid rejecting the reconstructed root patch before
              // its final root-formal replay is available.
              if (!llvm::is_contained(candidate.deferOccurrenceArgIdxs, argIdx))
                candidate.deferOccurrenceArgIdxs.push_back(argIdx);
            }

            // Keep the deferred formal set stable and deterministic so later
            // validation and tracing do not depend on discovery order.
            llvm::sort(candidate.deferOccurrenceArgIdxs);
            splitInsertionRootCandidates.push_back(std::move(candidate));
          }
        }
      }

      SmallVector<LeafCandidate, 8> leafCands;
      directRootPreservationInadmissible = false;

      for (const auto &cand : (*deps_.model).GetMacroInvocations()) {
        // Only consider invocations that are strict descendants of the
        // validated root by producer-recorded caller ancestry.  Keep the
        // computed depth because leaf ordering uses the original distance-to-
        // root tie-breaker.
        std::optional<unsigned> candidateDepth =
            CandidateDepthInValidatedSubtree(subtreeValidationCtx, cand);
        if (!candidateDepth)
          continue;
        const bool hunkWithinCandCover = cand.cover.begin <= h.aStart &&
                                         h.aEnd <= cand.cover.end &&
                                         cand.cover.begin < cand.cover.end;

        SmallVector<RefoldModel::PPArgSpan, 8> candArgLikeRaw;
        gatherArgLike(cand, candArgLikeRaw);
        SmallVector<RefoldModel::PPArgSpan, 8> candArgLike = candArgLikeRaw;
        sanitizeArgLikeSpans(candArgLike);

        if (!SubtreePathHasProvableCalleeClosure(subtreeValidationCtx, cand)) {
          // Unsupported descendant structure only blocks direct root replay
          // when the edit cannot already be represented by one of the root's
          // own argument-like spans. If the root has a direct args-only proof
          // surface, keep that candidate alive and let DAG lifting compete
          // normally instead of forcing whole-cover expansion.
          if (hunkWithinCandCover && !rootHasDirectArgLikeSurface)
            directRootPreservationInadmissible = true;
          continue;
        }

        // Candidate must have argument-like spans; otherwise there's nothing
        // concrete to map an edit to.
        if (candArgLike.empty())
          continue;

        // Determine how many formals this invocation "effectively" has, because
        // spans might reference argIdx beyond invArgRanges size.
        unsigned candFormalCount = (unsigned)cand.invArgRanges.size();
        for (const auto &sp : candArgLike)
          candFormalCount = std::max(candFormalCount, (unsigned)sp.argIdx + 1);

        // Mark which concrete arg-like span occurrences are touched by the
        // hunk, then compress that to a per-formal touched set. The helper
        // expects one flag per span occurrence, while the later DAG logic
        // reasons per formal arg index.
        SmallVector<char, 8> candTouchedBySpan(candArgLike.size(), 0);
        if (!(*deps_.sourceMapper).HunkFullyWithinArgSpans(h, candArgLike, candTouchedBySpan)) {
          continue;
        }

        SmallVector<char, 8> candTouched(candFormalCount, 0);
        for (size_t si = 0; si < candArgLike.size(); ++si) {
          if (!candTouchedBySpan[si])
            continue;
          const auto &sp = candArgLike[si];
          if (sp.argIdx < candTouched.size())
            candTouched[sp.argIdx] = 1;
        }

        bool anyTouched = false;
        for (char t : candTouched)
          if (t) {
            anyTouched = true;
            break;
          }
        if (!anyTouched) {
          continue;
        }

        // Now verify there's an actual A->B difference within at least one
        // touched arg-like span (otherwise lifting would be a no-op).
        bool anyDiff = false;
        uint64_t bestSpan = ~uint64_t(0);
        for (const auto &sp : candArgLike) {
          if (sp.argIdx >= candTouched.size() || !candTouched[sp.argIdx])
            continue;
          bestSpan = std::min(bestSpan, ppArgSpanProofWidth(sp));

          auto aTxt = extractSpanText(sp, /*fromB=*/false);
          auto bTxt = extractSpanText(sp, /*fromB=*/true);
          if (!aTxt || !bTxt)
            continue;

          if (bTxt->reliable) {
            // Compare normalized old/new argument text. For variadic formals,
            // allow top-level commas when unstringifying.
            auto aLift = normalizeLiftText(&cand, sp, aTxt->text,
                                           /*allowTopLevelComma=*/true);
            bool allowComma = sp.argIdx < cand.defParams.size() &&
                              cand.defParams[sp.argIdx].variadic;
            auto bLift = normalizeLiftText(&cand, sp, bTxt->text,
                                           /*allowTopLevelComma=*/allowComma);
            if (aLift && bLift && *aLift != *bLift)
              anyDiff = true;
          } else if (sp.kind == PPArgSpanKind::Paste && sp.byteBegin &&
                     sp.byteEnd) {
            // Paste-subrange extraction may be unreliable after edits (token
            // has changed). We still treat this as a potential edit; later we
            // only accept it if token-level splitting is uniquely determined.
            anyDiff = true;
          }
        }

        if (!anyDiff) {
          continue;
        }

        // Candidate leaf accepted: store its arg-like spans and which formals
        // are touched, plus depth and a locality tie-breaker.
        leafCands.push_back(LeafCandidate{&cand, *candidateDepth, bestSpan,
                                          std::move(candArgLike),
                                          std::move(candTouched)});
      }

      // Order leaves from most promising to least:
      //   (1) deepest first (closest to the actual edit)
      //   (2) smaller span first (more local pp coverage)
      llvm::sort(leafCands, [](const LeafCandidate &a, const LeafCandidate &b) {
        if (a.depth != b.depth)
          return a.depth > b.depth; // deepest first
        return a.smallestSpanBytes < b.smallestSpanBytes;
      });

      // Diagnostic: if DAG chaining is considered, report the number of
      // leaf candidates that could potentially be lifted back to this root.


      // Cache the root invocation's *current* argument texts (trimmed). These
      // are used for final-hop two-parent splitting and for validation.
      DenseMap<uint32_t, StringRef> rootArgText;
      for (uint32_t i = 0; i < numArgs; ++i) {
        rootArgText[i] =
            invSpanText.slice(invArgRanges[i].first, invArgRanges[i].second)
                .trim();
      }

      // --- Leaf edit lifting to the root invocation --------------------------
      //
      // Generalized single-parent lifting uses the recorded arg_refs slices to
      // invert a nested callee argument back to the caller's raw invocation
      // argument text. In other words, we do not merely ask "which caller
      // formal does this depend on?"; we reconstruct the child argument's
      // template, substitute abstract caller-formal variables into that
      // template, and require the observed old/new texts to match that
      // template uniquely.
      //
      // This is strictly more general than the earlier direct-pass-through
      // check. It accepts wrapper forms such as "((x), 10)" whenever the edit
      // changes only the caller-derived part, but it still rejects any hop that
      // cannot be proven by arg_refs (mixed body-owned edits, ambiguous
      // repeated-variable splits, missing provenance, etc.).
      //
      // The template inverse is now expressed as an explicit invertibility
      // certificate:
      //   * Unique       -> each contributing caller formal has exactly one
      //                     derived replacement string.
      //   * NoMatch      -> the observed text does not fit the original
      //                     arg_refs/literal template at all.
      //   * Ambiguous    -> multiple distinct inverses exist.
      //   * Unsupported  -> the template shape is outside the conservative
      //                     solver bounds.
      struct LocalArgRef {
        uint32_t callerParamIndex;
        uint32_t begin;
        uint32_t end;
      };

      struct ArgRefTemplate {
        std::string argText;
        SmallVector<LocalArgRef, 4> refs;
        SmallVector<uint32_t, 2> distinctCallerParams;
      };

      // Return the trimmed raw invocation argument text for `argIdx` using the
      // invocation spelling's formal slots.  Producer source ranges can
      // describe a larger C syntactic surface (for example `arr[1, 2]`) even
      // when the preprocessor splits that text across multiple macro formals,
      // so wrapper reconstruction must use the locally parsed macro-argument
      // slots here.
      auto getInvocationArgText =
          [&](const RefoldModel::MacroInvocation &inv,
              uint32_t argIdx) -> std::optional<StringRef> {
        if (!inv.invText)
          return std::nullopt;
        auto rangesOpt =
            GetMacroInvocationFormalArgContentRanges(inv, *inv.invText);
        if (!rangesOpt || argIdx >= rangesOpt->size())
          return std::nullopt;
        const auto &rng = (*rangesOpt)[argIdx];
        if (rng.second < rng.first || rng.second > inv.invText->size())
          return std::nullopt;
        return StringRef(*inv.invText)
            .slice((size_t)rng.first, (size_t)rng.second)
            .trim();
      };

      // Build an argument-local template for one child invocation argument by
      // replacing producer arg-ref byte ranges with caller-parameter
      // placeholders. The returned template is trimmed, ordered, and
      // non-overlapping so the later invertibility solver can match literal
      // text and forwarded caller slices without reasoning about raw invocation
      // offsets.
      auto buildArgRefTemplate =
          [&](const RefoldModel::MacroInvocation &inv,
              uint32_t argIdx) -> std::optional<ArgRefTemplate> {
        if (!inv.invText || !inv.invB)
          return std::nullopt;
        if (argIdx >= inv.invArgRanges.size() || argIdx >= inv.argRefs.size())
          return std::nullopt;

        const auto &rng = inv.invArgRanges[argIdx];
        if (!rng.first || !rng.second || *rng.second < *rng.first ||
            *rng.first < *inv.invB)
          return std::nullopt;

        // Convert the producer's absolute argument range into invocation-local
        // coordinates so it can slice `inv.invText`.
        const uint64_t relB = *rng.first - *inv.invB;
        const uint64_t relE = *rng.second - *inv.invB;
        if (relE < relB || relE > inv.invText->size())
          return std::nullopt;

        StringRef rawArg =
            StringRef(*inv.invText).slice((size_t)relB, (size_t)relE);

        // Work in trimmed argument-local coordinates so literal segments and
        // arg-ref placeholders describe the same surface text that will be
        // compared.
        size_t trimLead = 0;
        size_t trimEnd = rawArg.size();
        std::tie(trimLead, trimEnd) =
            stringutils::trimWsRange(rawArg, 0, rawArg.size());

        ArgRefTemplate out;
        out.argText = rawArg.slice(trimLead, trimEnd).str();

        SmallVector<LocalArgRef, 4> refs;
        refs.reserve(inv.argRefs[argIdx].size());

        // Rebase producer byte ranges from invocation-relative coordinates into
        // the trimmed argument surface used by the invertibility solver.
        for (const auto &ref : inv.argRefs[argIdx]) {
          if (ref.byteEnd < ref.byteBegin)
            return std::nullopt;

          // The arg-ref must lie inside this child argument's raw invocation
          // range.
          if (ref.byteBegin < relB || ref.byteEnd > relE)
            return std::nullopt;

          const uint64_t localBAbs = ref.byteBegin - relB;
          const uint64_t localEAbs = ref.byteEnd - relB;
          if (localEAbs < localBAbs || localEAbs > rawArg.size())
            return std::nullopt;

          // After trimming, placeholders must still be wholly inside the
          // retained argument surface. Refs in discarded leading/trailing
          // whitespace would not have a stable local coordinate.
          if (localBAbs < trimLead || localEAbs > trimEnd)
            return std::nullopt;

          refs.push_back(LocalArgRef{ref.callerParamIndex,
                                     (uint32_t)(localBAbs - trimLead),
                                     (uint32_t)(localEAbs - trimLead)});
        }

        // Sort placeholders into source order. Caller parameter is only a
        // stable final tie-breaker for duplicate coordinates.
        llvm::sort(refs, [](const LocalArgRef &a, const LocalArgRef &b) {
          if (a.begin != b.begin)
            return a.begin < b.begin;
          if (a.end != b.end)
            return a.end < b.end;
          return a.callerParamIndex < b.callerParamIndex;
        });

        // The template solver assumes non-overlapping placeholders in source
        // order; reject overlapping arg-ref evidence rather than guessing.
        uint32_t prevEnd = 0;
        bool first = true;
        for (const auto &ref : refs) {
          if (ref.end < ref.begin || ref.end > out.argText.size())
            return std::nullopt;
          if (!first && ref.begin < prevEnd)
            return std::nullopt;

          prevEnd = ref.end;
          first = false;

          // Keep the distinct caller parameters referenced by this template so
          // the caller can quickly tell which parent arguments participate in
          // the match.
          if (llvm::find(out.distinctCallerParams, ref.callerParamIndex) ==
              out.distinctCallerParams.end())
            out.distinctCallerParams.push_back(ref.callerParamIndex);
        }

        out.refs = std::move(refs);
        return out;
      };

      // Compare two caller-parameter index lists as sets, ignoring order and
      // duplicate entries.
      auto sameIndexSet = [&](ArrayRef<uint32_t> a,
                              ArrayRef<uint32_t> b) -> bool {
        SmallVector<uint32_t, 4> sa(a.begin(), a.end());
        SmallVector<uint32_t, 4> sb(b.begin(), b.end());
        llvm::sort(sa);
        llvm::sort(sb);
        sa.erase(std::unique(sa.begin(), sa.end()), sa.end());
        sb.erase(std::unique(sb.begin(), sb.end()), sb.end());
        return sa == sb;
      };

      enum class ArgRefInvertibilityKind {
        Unique,
        NoMatch,
        Ambiguous,
        Unsupported,
      };

      struct ArgRefInvertibilityCertificate {
        ArgRefInvertibilityKind kind = ArgRefInvertibilityKind::Unsupported;
        DenseMap<uint32_t, std::string> derivedTextByCallerParam;
      };

      // Prove whether `observed` is a unique realization of an argument-ref
      // template. The template is treated as fixed literal text plus
      // caller-parameter placeholders; repeated placeholders must receive
      // identical text. The result is accepted only when exactly one
      // assignment maps caller parameters to observed substrings.
      auto buildArgRefInvertibilityCertificate =
          [&](const ArgRefTemplate &tpl,
              StringRef observed) -> ArgRefInvertibilityCertificate {
        ArgRefInvertibilityCertificate cert;

        constexpr size_t maxDistinctCallerParams = 8;

        // Empty templates do not prove forwarding, and very wide templates are
        // kept out of this local DFS to avoid turning malformed metadata into
        // an expensive search problem.
        if (tpl.refs.empty() || tpl.distinctCallerParams.empty() ||
            tpl.distinctCallerParams.size() > maxDistinctCallerParams)
          return cert;

        // Decompose the template into:
        //
        //   literal[0], var[0], literal[1], var[1], ..., literal[n]
        //
        // `varOrdinals` indexes into `tpl.distinctCallerParams`, so repeated
        // refs to the same caller parameter share one assignment slot.
        SmallVector<StringRef, 8> literals;
        SmallVector<unsigned, 8> varOrdinals;
        literals.reserve(tpl.refs.size() + 1);
        varOrdinals.reserve(tpl.refs.size());

        size_t curPos = 0;
        for (const auto &ref : tpl.refs) {
          if (ref.begin < curPos || ref.end < ref.begin ||
              ref.end > tpl.argText.size()) {
            cert.kind = ArgRefInvertibilityKind::NoMatch;
            return cert;
          }

          literals.push_back(StringRef(tpl.argText).slice(curPos, ref.begin));

          auto it = llvm::find(tpl.distinctCallerParams, ref.callerParamIndex);
          if (it == tpl.distinctCallerParams.end()) {
            cert.kind = ArgRefInvertibilityKind::NoMatch;
            return cert;
          }

          varOrdinals.push_back(
              (unsigned)std::distance(tpl.distinctCallerParams.begin(), it));
          curPos = ref.end;
        }
        literals.push_back(StringRef(tpl.argText).drop_front(curPos));

        StringRef obs = observed.trim();

        // `assigns[i]` is the candidate observed text for
        // `tpl.distinctCallerParams[i]`. It remains empty until the DFS first
        // reaches that caller parameter placeholder.
        SmallVector<std::optional<StringRef>, maxDistinctCallerParams> assigns(
            tpl.distinctCallerParams.size());

        // Keep at most enough distinct solutions to distinguish Unique from
        // Ambiguous. Duplicate assignment vectors can arise through equivalent
        // split paths and are ignored.
        SmallVector<SmallVector<std::string, maxDistinctCallerParams>, 2>
            solutions;

        auto addSolution = [&](ArrayRef<std::optional<StringRef>> aLocal) {
          SmallVector<std::string, maxDistinctCallerParams> sLocal;
          sLocal.reserve(tpl.distinctCallerParams.size());
          for (size_t i = 0; i < tpl.distinctCallerParams.size(); ++i)
            sLocal.push_back(aLocal[i] ? aLocal[i]->str() : std::string());

          for (const auto &existing : solutions)
            if (existing == sLocal)
              return;

          solutions.push_back(std::move(sLocal));
        };

        auto dfs = [&](auto &&self, size_t refIdx, size_t obsPos) -> void {
          // One solution is acceptable; a second distinct solution is enough to
          // prove ambiguity, so stop exploring once ambiguity is known.
          if (solutions.size() > 1)
            return;

          // Each placeholder is preceded by a fixed literal. The observed text
          // must match that literal exactly at the current position before the
          // variable can consume anything.
          const StringRef lit = literals[refIdx];
          if (obsPos > obs.size() || !obs.drop_front(obsPos).starts_with(lit))
            return;
          obsPos += lit.size();

          // All placeholders consumed. This path is a solution only if it also
          // consumed the full observed text, including the trailing literal.
          if (refIdx == varOrdinals.size()) {
            if (obsPos == obs.size())
              addSolution(assigns);
            return;
          }

          const unsigned varOrd = varOrdinals[refIdx];
          if (varOrd >= assigns.size())
            return;

          if (assigns[varOrd]) {
            // Repeated references to the same caller parameter must consume the
            // exact same observed text as the first occurrence.
            const StringRef val = *assigns[varOrd];
            if (obsPos <= obs.size() && obs.drop_front(obsPos).starts_with(val))
              self(self, refIdx + 1, obsPos + val.size());
            return;
          }

          // Bound the candidate length by the fixed literals and already-bound
          // variables that must still fit in the remaining observed text.
          // Unbound later variables may be empty, so they do not add to this
          // lower bound.
          size_t minRemain = 0;
          for (size_t j = refIdx + 1; j < literals.size(); ++j)
            minRemain += literals[j].size();
          for (size_t j = refIdx + 1; j < varOrdinals.size(); ++j) {
            const unsigned laterOrd = varOrdinals[j];
            if (laterOrd < assigns.size() && assigns[laterOrd])
              minRemain += assigns[laterOrd]->size();
          }
          if (obsPos + minRemain > obs.size())
            return;

          const size_t maxLen = obs.size() - obsPos - minRemain;
          const StringRef rest = obs.drop_front(obsPos);
          const StringRef nextLit = literals[refIdx + 1];

          auto tryLen = [&](size_t len) {
            assigns[varOrd] = rest.take_front(len);
            self(self, refIdx + 1, obsPos + len);
            assigns[varOrd] = std::nullopt;
          };

          if (!nextLit.empty()) {
            // When the next literal is known, only split at occurrences of that
            // literal. This avoids enumerating equivalent impossible lengths.
            for (size_t searchPos = 0;; ++searchPos) {
              const size_t pos = rest.find(nextLit, searchPos);
              if (pos == StringRef::npos || pos > maxLen)
                break;
              tryLen(pos);
              if (solutions.size() > 1)
                return;
            }
          } else {
            // With no following literal delimiter, every remaining length is a
            // possible assignment; uniqueness below decides whether this is
            // safe.
            for (size_t len = 0; len <= maxLen; ++len) {
              tryLen(len);
              if (solutions.size() > 1)
                return;
            }
          }
        };

        dfs(dfs, 0, 0);

        // Exactly one assignment vector is required. Zero solutions means this
        // observed text does not realize the template; multiple means
        // ambiguous.
        if (solutions.empty()) {
          cert.kind = ArgRefInvertibilityKind::NoMatch;
          return cert;
        }
        if (solutions.size() > 1) {
          cert.kind = ArgRefInvertibilityKind::Ambiguous;
          return cert;
        }

        cert.kind = ArgRefInvertibilityKind::Unique;
        for (unsigned i = 0; i < tpl.distinctCallerParams.size(); ++i)
          cert.derivedTextByCallerParam[tpl.distinctCallerParams[i]] =
              solutions[0][i];
        return cert;
      };

      struct TrimmedArgInfo {
        std::string text;
        uint64_t absTrimBegin = 0;
        uint64_t absTrimEnd = 0;
      };

      // Return the trimmed spelling and absolute byte extent of one invocation
      // argument. The extent is derived from the invocation's locally parsed
      // macro-argument slots, not directly from producer source ranges, so commas
      // inside braces/brackets are assigned to the same formals the preprocessor
      // actually uses.
      auto getTrimmedInvocationArgInfo =
          [&](const RefoldModel::MacroInvocation &inv,
              uint32_t argIdx) -> std::optional<TrimmedArgInfo> {
        if (!inv.invText || !inv.invB)
          return std::nullopt;
        auto rangesOpt =
            GetMacroInvocationFormalArgContentRanges(inv, *inv.invText);
        if (!rangesOpt || argIdx >= rangesOpt->size())
          return std::nullopt;

        const auto &rng = (*rangesOpt)[argIdx];
        if (rng.second < rng.first || rng.second > inv.invText->size())
          return std::nullopt;

        StringRef raw = StringRef(*inv.invText)
                            .slice((size_t)rng.first, (size_t)rng.second);

        size_t trimLead = 0;
        size_t trimEnd = raw.size();
        std::tie(trimLead, trimEnd) =
            stringutils::trimWsRange(raw, 0, raw.size());

        TrimmedArgInfo out;
        out.text = raw.slice(trimLead, trimEnd).str();
        out.absTrimBegin = *inv.invB + rng.first + trimLead;
        out.absTrimEnd = *inv.invB + rng.first + trimEnd;
        return out;
      };

      // Return the comparable A-side expansion text for an invocation cover.
      // When a parameterless function-like wrapper has a producer-recorded body
      // span narrower than its full cover, use that body payload instead of the
      // wider invocation cover.
      auto getInvocationCoverAText =
          [&](const RefoldModel::MacroInvocation &inv)
          -> std::optional<std::string> {
        uint64_t covLoA = inv.cover.begin;
        uint64_t covHiA = inv.cover.end;
        if (inv.subkind == "func" && inv.defParams.empty() &&
            !inv.bodySpans.empty()) {
          // Object-like/function-like-without-params wrappers can have a cover
          // wider than the replacement body. Prefer the concrete body span when
          // the producer recorded one, because that is the comparable payload.
          uint64_t lo = std::numeric_limits<uint64_t>::max();
          uint64_t hi = 0;
          for (const auto &s : inv.bodySpans) {
            if (s.begin < s.end) {
              lo = std::min(lo, s.begin);
              hi = std::max(hi, s.end);
            }
          }
          if (lo != std::numeric_limits<uint64_t>::max() && lo < hi) {
            covLoA = lo;
            covHiA = hi;
          }
        }
        if (covLoA >= covHiA)
          return std::nullopt;
        return (*deps_.sourceMapper).SliceASource(covLoA, covHiA).trim().str();
      };

      // Return true when `pos` is a plausible split point in refold text. The
      // check rejects cuts through the middle of an identifier-like token.
      auto isLikelyTokenBoundaryInRefoldText = [&](StringRef s,
                                                   size_t pos) -> bool {
        if (pos == 0 || pos >= s.size())
          return true;
        return !(stringutils::isIdentPart(s[pos - 1]) &&
                 stringutils::isIdentPart(s[pos]));
      };

      // Return true if the entire fragment is balanced at top level according
      // to the lexer-backed cut-point enumerator.
      auto isBalancedRefoldFragment = [&](StringRef s) -> bool {
        bool balancedAtEnd = false;
        enumerateTopLevelBalancedCutPointsWithLexer(s, (*deps_.lexLang),
                                                    [&](unsigned cut) {
                                                      if (cut == s.size())
                                                        balancedAtEnd = true;
                                                    });
        return balancedAtEnd;
      };

      // Enumerate top-level, balanced occurrences of `needle` in `haystack` up
      // to `maxPos`, only emitting matches that begin at a plausible token
      // boundary.
      auto enumerateTopLevelLiteralMatchesInRefoldText = [&](StringRef haystack,
                                                             StringRef needle,
                                                             size_t maxPos,
                                                             auto &&emitMatch) {
        if (needle.empty())
          return;
        enumerateTopLevelBalancedCutPointsWithLexer(
            haystack, (*deps_.lexLang), [&](unsigned cut) {
              const size_t pos = static_cast<size_t>(cut);
              if (pos > maxPos)
                return;
              if (!isLikelyTokenBoundaryInRefoldText(haystack, pos))
                return;
              if (haystack.drop_front(pos).starts_with(needle))
                emitMatch(pos);
            });
      };

      // Rebuild an invocation spelling by replacing selected formal-argument
      // slots with proven replacement text. All edits are validated in
      // invocation-local coordinates first, then applied right-to-left so
      // original byte ranges remain stable.
      auto buildRewrittenInvocationSyntax =
          [&](const RefoldModel::MacroInvocation &inv,
              const DenseMap<uint32_t, std::string> &replByFormal)
          -> std::optional<std::string> {
        if (!inv.invText || !inv.invB)
          return std::nullopt;
        auto rangesOpt = GetMacroInvocationFormalArgContentRanges(inv, *inv.invText);
        if (!rangesOpt)
          return std::nullopt;

        // Apply formal-slot edits to the invocation spelling right-to-left so
        // original byte offsets remain stable while edits are installed.
        struct LocalEdit {
          uint64_t begin = 0;
          uint64_t end = 0;
          std::string repl;
        };

        SmallVector<LocalEdit, 8> edits;
        edits.reserve(replByFormal.size());
        for (const auto &kvLocal : replByFormal) {
          // Validate each replacement against the original formal slot before
          // editing the invocation surface. Non-variadic slots cannot receive a
          // top-level comma because that would change call arity.
          const uint32_t argIdx = kvLocal.first;
          if (argIdx >= rangesOpt->size())
            return std::nullopt;
          const auto &rng = (*rangesOpt)[argIdx];
          if (rng.second < rng.first || rng.second > inv.invText->size())
            return std::nullopt;

          const uint64_t relB = rng.first;
          const uint64_t relE = rng.second;

          StringRef newArg = StringRef(kvLocal.second).trim();
          const bool allowComma =
              argIdx < inv.defParams.size() && inv.defParams[argIdx].variadic;
          if (!allowComma && refoldMacroActualHasTopLevelComma(newArg, (*deps_.lexLang)))
            return std::nullopt;

          edits.push_back(LocalEdit{relB, relE, newArg.str()});
        }

        llvm::sort(edits, [](const LocalEdit &a, const LocalEdit &b) {
          return a.begin > b.begin;
        });

        // Apply from right to left so earlier byte offsets remain valid.
        std::string rewritten = inv.invText->str();
        for (const auto &edit : edits)
          rewritten = stringutils::replaceRange(rewritten, edit.begin, edit.end,
                                                edit.repl);
        return StringRef(rewritten).trim().str();
      };

      // Return the normalized expansion spellings that can legitimately
      // represent this invocation when matching wrapper observations. Besides
      // the raw A/B expansion surface, include equivalent unstringified and
      // wide-string literal forms when those interpretations are valid.
      auto expansionTextCandidates =
          [&](const RefoldModel::MacroInvocation &inv,
              bool fromB) -> SmallVector<std::string, 4> {
        SmallVector<std::string, 4> out;
        std::optional<std::string> base =
            fromB ? GetProofLattice().BuildWholeCoverReplacementText(inv)
                  : getInvocationCoverAText(inv);
        if (!base)
          return out;

        auto addUnique = [&](StringRef s) {
          std::string cand = s.trim().str();
          if (cand.empty())
            return;
          if (llvm::find(out, cand) == out.end())
            out.push_back(std::move(cand));
        };

        auto tryAddUnstringified = [&](StringRef raw) {
          auto un =
              (*deps_.argTextRecovery).UnstringifyLiteralToArgText(
                  raw, /*allowTopLevelComma=*/true);
          if (!un)
            return;
          addUnique(*un);
        };

        auto tryAddWideLiteral = [&](StringRef raw) {
          StringRef t = raw.trim();
          if (!stringutils::looksLikeStringLiteralToken(t))
            return;

          // Preserve existing wide/prefixed string literals as-is; only add a
          // widened form when the observed literal is an ordinary string.
          if (t.starts_with("L\"") || t.starts_with("u\"") ||
              t.starts_with("U\"") || t.starts_with("u8\""))
            return;

          addUnique((Twine("L") + t).str());
        };

        // Compare wrapper observations against several equivalent surfaces: the
        // expansion spelling, a valid unstringified logical input, and an
        // ordinary string literal promoted to wide literal form.
        addUnique(*base);
        tryAddUnstringified(*base);
        tryAddWideLiteral(*base);
        return out;
      };

      enum class WrapperChainKind {
        Exact,
        StringLiteral,
        WideStringLiteral,
      };

      enum class WrapperObservedSource {
        ChildExpansion,
        ChildRawInvocation,
      };

      struct WrapperChainCertificate {
        WrapperChainKind kind = WrapperChainKind::Exact;
        WrapperObservedSource source = WrapperObservedSource::ChildExpansion;
        std::string observedOldText;
        std::string logicalInputText;
      };

      struct LexicalChildPlaceholder {
        uint64_t relBegin = 0;
        uint64_t relEnd = 0;
        const RefoldModel::MacroInvocation *child = nullptr;
        SmallVector<WrapperChainCertificate, 4> observedForms;
        SmallVector<std::string, 4> newExpansionCandidates;
        std::string rawInvocationText;
      };

      // Find direct lexical child macro invocations spelled inside one trimmed
      // parent argument. Each returned placeholder records the child's byte
      // range relative to that parent argument plus the observed wrapper forms
      // that can represent the child text during wrapper-chain reconstruction.
      auto getTopLevelLexicalChildrenInArg =
          [&](const RefoldModel::MacroInvocation &parent, uint32_t parentFormal)
          -> SmallVector<LexicalChildPlaceholder, 4> {
        SmallVector<LexicalChildPlaceholder, 8> cands;

        // Child positions are reported relative to the trimmed parent argument,
        // so we need both the trimmed argument extent and the source file
        // containing it.
        auto argInfo = getTrimmedInvocationArgInfo(parent, parentFormal);
        if (!argInfo || !parent.invFile)
          return SmallVector<LexicalChildPlaceholder, 4>{};

        for (const auto &cand : (*deps_.model).GetMacroInvocations()) {
          // A lexical child must be a distinct invocation spelled in the same
          // file and wholly inside the parent argument's trimmed absolute byte
          // range.
          if (cand.id == parent.id || !cand.invFile || !cand.invB || !cand.invE)
            continue;
          if (*cand.invFile != *parent.invFile)
            continue;
          if (*cand.invB < argInfo->absTrimBegin ||
              *cand.invE > argInfo->absTrimEnd || *cand.invE <= *cand.invB)
            continue;

          // Build old/new expansion surfaces for the child. Old surfaces
          // explain what the parent argument originally contained; new surfaces
          // are candidate child replacements used later if this placeholder is
          // rewritten.
          auto olds = expansionTextCandidates(cand, /*fromB=*/false);
          auto news = expansionTextCandidates(cand, /*fromB=*/true);

          SmallVector<WrapperChainCertificate, 4> forms;

          auto addObservedForm = [&](WrapperChainKind kind,
                                     WrapperObservedSource source,
                                     StringRef text, StringRef logicalInput) {
            std::string observed = text.trim().str();
            std::string logical = logicalInput.trim().str();
            if (observed.empty() || logical.empty())
              return;

            // String-literal wrapper forms compare against the logical unquoted
            // input, so require the inverse stringify payload to be canonical
            // before using it as a certificate.
            if (kind == WrapperChainKind::StringLiteral ||
                kind == WrapperChainKind::WideStringLiteral) {
              auto canon =
                  stringutils::canonicalizeStringifyInversePayload(logical);
              if (!canon ||
                  StringRef(*canon).trim() != StringRef(logical).trim())
                return;
              logical = std::move(*canon);
            }

            // Deduplicate equivalent certificates; the same observed/logical
            // pair can be reached from multiple expansion-surface candidates.
            for (const auto &existing : forms) {
              if (existing.kind == kind &&
                  existing.observedOldText == observed &&
                  existing.logicalInputText == logical)
                return;
            }

            forms.push_back(WrapperChainCertificate{
                kind, source, std::move(observed), std::move(logical)});
          };

          // The child may appear in the parent argument as its expansion text,
          // or as a stringized/wide-stringized wrapper around that expansion
          // text.
          for (StringRef oldText : olds) {
            StringRef trimmed = oldText.trim();
            addObservedForm(WrapperChainKind::Exact,
                            WrapperObservedSource::ChildExpansion, trimmed,
                            trimmed);
            addObservedForm(WrapperChainKind::StringLiteral,
                            WrapperObservedSource::ChildExpansion,
                            stringutils::quoteCStringLiteral(trimmed), trimmed);
            addObservedForm(
                WrapperChainKind::WideStringLiteral,
                WrapperObservedSource::ChildExpansion,
                (Twine("L") + stringutils::quoteCStringLiteral(trimmed)).str(),
                trimmed);
          }

          // Also accept the raw child invocation spelling as an observed form.
          // This covers wrappers that forward or stringify the child call
          // syntax itself rather than the child's expansion result.
          if (cand.invText) {
            const std::string rawInvocation =
                StringRef(*cand.invText).trim().str();
            if (!rawInvocation.empty()) {
              addObservedForm(WrapperChainKind::Exact,
                              WrapperObservedSource::ChildRawInvocation,
                              rawInvocation, rawInvocation);
              addObservedForm(WrapperChainKind::StringLiteral,
                              WrapperObservedSource::ChildRawInvocation,
                              stringutils::quoteCStringLiteral(rawInvocation),
                              rawInvocation);
              addObservedForm(
                  WrapperChainKind::WideStringLiteral,
                  WrapperObservedSource::ChildRawInvocation,
                  (Twine("L") + stringutils::quoteCStringLiteral(rawInvocation))
                      .str(),
                  rawInvocation);
            }
          }

          // Without at least one observed form, this child cannot be matched
          // back to a concrete surface inside the parent argument.
          if (forms.empty())
            continue;

          LexicalChildPlaceholder ph;
          ph.relBegin = *cand.invB - argInfo->absTrimBegin;
          ph.relEnd = *cand.invE - argInfo->absTrimBegin;
          ph.child = &cand;
          ph.observedForms = std::move(forms);
          ph.newExpansionCandidates = std::move(news);
          if (cand.invText)
            ph.rawInvocationText = StringRef(*cand.invText).trim().str();

          cands.push_back(std::move(ph));
        }

        // Sort by source order, with wider candidates first for identical
        // starts so outer placeholders dominate nested placeholders during
        // top-level filtering.
        llvm::sort(cands, [](const LexicalChildPlaceholder &a,
                             const LexicalChildPlaceholder &b) {
          if (a.relBegin != b.relBegin)
            return a.relBegin < b.relBegin;
          return a.relEnd > b.relEnd;
        });

        SmallVector<LexicalChildPlaceholder, 4> top;
        for (const auto &cand : cands) {
          // Keep only top-level child placeholders. Nested or overlapping child
          // invocations are represented by their outermost placeholder here,
          // because wrapper-chain reconstruction needs a non-overlapping
          // decomposition of the parent argument surface.
          bool contained = false;
          for (const auto &sel : top) {
            if (cand.relBegin >= sel.relBegin && cand.relEnd <= sel.relEnd) {
              contained = true;
              break;
            }

            // Treat partial overlap as non-top-level too. Overlapping
            // placeholders do not define a deterministic left-to-right rewrite
            // surface.
            if (!(cand.relEnd <= sel.relBegin || cand.relBegin >= sel.relEnd)) {
              contained = true;
              break;
            }
          }

          if (!contained)
            top.push_back(cand);
        }

        return top;
      };

      enum class ArgInvertibilityKind {
        LiteralOnly,
        TemplateWithChildren,
      };

      struct ArgInvertibilityCertificate {
        ArgInvertibilityKind kind = ArgInvertibilityKind::LiteralOnly;
        std::string rawArgText;
        SmallVector<std::string, 8> literals;
        SmallVector<LexicalChildPlaceholder, 4> slots;
        SmallVector<unsigned, 4> chosenObservedFormIdx;
      };

      // Build a certificate proving how the parent formal's original argument
      // text produced `observedOld0`. Literal-only arguments must match
      // exactly; arguments containing lexical child invocations are converted
      // into a literal/slot template, and each child slot must match one
      // semantically unique observed wrapper form.
      auto buildArgInvertibilityCertificate =
          [&](const RefoldModel::MacroInvocation &parent, uint32_t parentFormal,
              StringRef observedOld0)
          -> std::optional<ArgInvertibilityCertificate> {
        auto argInfo = getTrimmedInvocationArgInfo(parent, parentFormal);
        if (!argInfo)
          return std::nullopt;

        StringRef rawArg = StringRef(argInfo->text).trim();
        StringRef observedOld = observedOld0.trim();

        ArgInvertibilityCertificate cert;
        cert.rawArgText = rawArg.str();

        // Discover child invocations spelled directly inside this parent
        // argument. These become template slots; the text between them remains
        // fixed literal material.
        auto placeholders =
            getTopLevelLexicalChildrenInArg(parent, parentFormal);

        if (placeholders.empty()) {
          // No child slots means the argument is just literal surface text. It
          // is invertible only when the observed old expansion equals that text
          // exactly.
          if (observedOld != rawArg)
            return std::nullopt;
          cert.kind = ArgInvertibilityKind::LiteralOnly;
          cert.literals.push_back(rawArg.str());
          return cert;
        }

        cert.kind = ArgInvertibilityKind::TemplateWithChildren;
        cert.slots = placeholders;

        // Decompose the raw argument into alternating fixed literals and child
        // slots:
        //
        //   literal[0], slot[0], literal[1], slot[1], ..., literal[n]
        //
        // Slot ranges are relative to the trimmed parent argument.
        uint64_t curPos = 0;
        for (const auto &ph : placeholders) {
          if (ph.relBegin < curPos || ph.relEnd < ph.relBegin ||
              ph.relEnd > rawArg.size())
            return std::nullopt;

          cert.literals.push_back(
              rawArg.slice((size_t)curPos, (size_t)ph.relBegin).str());
          curPos = ph.relEnd;
        }
        cert.literals.push_back(rawArg.drop_front((size_t)curPos).str());

        SmallVector<unsigned, 4> chosenOld;
        SmallVector<SmallVector<unsigned, 4>, 2> oldSolutions;

        // Match the observed old expansion against the literal/slot template
        // and record which observed wrapper form each child slot used.
        auto matchOld = [&](auto &&self, size_t idx, size_t pos) -> void {
          // Stop after finding more than one solution; the later equivalence
          // check only needs to distinguish unique/semantically-equivalent from
          // ambiguous.
          if (oldSolutions.size() > 1)
            return;

          // Each slot is preceded by a fixed literal fragment that must match
          // exactly at the current observed position.
          const StringRef lit = cert.literals[idx];
          if (pos > observedOld.size() ||
              !observedOld.drop_front(pos).starts_with(lit))
            return;
          pos += lit.size();

          if (idx == cert.slots.size()) {
            // All slots consumed. This is a complete match only if the
            // trailing literal also consumed the rest of the observed old text.
            if (pos == observedOld.size())
              oldSolutions.push_back(chosenOld);
            return;
          }

          // Try every certified old surface for this child slot. A slot may
          // match as the child expansion, raw child invocation, string literal
          // wrapper, etc.; the selected form is recorded so the rewrite can
          // preserve the same wrapper shape later.
          for (unsigned choice = 0;
               choice < cert.slots[idx].observedForms.size(); ++choice) {
            StringRef phOld =
                cert.slots[idx].observedForms[choice].observedOldText;
            if (observedOld.drop_front(pos).starts_with(phOld)) {
              chosenOld.push_back(choice);
              self(self, idx + 1, pos + phOld.size());
              chosenOld.pop_back();
            }
          }
        };

        matchOld(matchOld, 0, 0);
        if (oldSolutions.empty())
          return std::nullopt;

        auto semanticallyEquivalentOldSolutions =
            [&](const SmallVectorImpl<unsigned> &a,
                const SmallVectorImpl<unsigned> &b) -> bool {
          if (a.size() != b.size())
            return false;

          for (size_t i = 0; i < a.size(); ++i) {
            if (i >= cert.slots.size() ||
                a[i] >= cert.slots[i].observedForms.size() ||
                b[i] >= cert.slots[i].observedForms.size()) {
              return false;
            }

            const auto &fa = cert.slots[i].observedForms[a[i]];
            const auto &fb = cert.slots[i].observedForms[b[i]];

            // Multiple textual matches are acceptable only when they select the
            // same wrapper semantics and the same logical child input.
            // Otherwise the old observation is ambiguous and cannot drive a
            // deterministic rewrite.
            if (fa.kind != fb.kind || fa.source != fb.source ||
                StringRef(fa.logicalInputText).trim() !=
                    StringRef(fb.logicalInputText).trim()) {
              return false;
            }
          }

          return true;
        };

        // Accept multiple syntactic matches only when they are semantically
        // identical for every slot. This avoids rejecting harmless duplicate
        // surfaces while still failing closed on genuinely different wrapper
        // interpretations.
        for (size_t i = 1; i < oldSolutions.size(); ++i) {
          if (!semanticallyEquivalentOldSolutions(oldSolutions[0],
                                                  oldSolutions[i])) {
            return std::nullopt;
          }
        }

        cert.chosenObservedFormIdx = oldSolutions[0];

        return cert;
      };

      enum class SlotRewriteDecisionKind {
        PreferredChildSyntax,
        PreserveRawInvocation,
        PassthroughFlatten,
      };

      struct SlotRewriteDecision {
        SlotRewriteDecisionKind kind =
            SlotRewriteDecisionKind::PassthroughFlatten;
        std::string observedText;
        std::string rebuiltText;

        bool operator==(const SlotRewriteDecision &other) const {
          return kind == other.kind && observedText == other.observedText &&
                 rebuiltText == other.rebuiltText;
        }
      };

      enum class SlotSemanticRewriteCertificateKind {
        Unique,
        Invalid,
      };

      struct SlotSemanticRewriteCertificate {
        SlotSemanticRewriteCertificateKind kind =
            SlotSemanticRewriteCertificateKind::Invalid;
        SlotRewriteDecision decision;
        WrapperChainKind wrapperKind = WrapperChainKind::Exact;
        WrapperObservedSource wrapperSource =
            WrapperObservedSource::ChildExpansion;
        std::string logicalInputText;

        bool operator==(const SlotSemanticRewriteCertificate &other) const {
          return kind == other.kind && decision == other.decision &&
                 wrapperKind == other.wrapperKind &&
                 wrapperSource == other.wrapperSource &&
                 logicalInputText == other.logicalInputText;
        }
      };

      enum class ArgSemanticRewriteCertificateKind {
        NoChange,
        Unique,
        Invalid,
      };

      enum class ArgSemanticRewriteFailure {
        None,
        MissingStructuralTemplate,
      };

      struct ArgSemanticRewriteCertificate {
        ArgSemanticRewriteCertificateKind kind =
            ArgSemanticRewriteCertificateKind::Invalid;
        ArgSemanticRewriteFailure failure = ArgSemanticRewriteFailure::None;
        std::string observedNewText;
        std::string rawArgOldText;
        std::string rawArgNewText;
        SmallVector<SlotRewriteDecision, 8> slotDecisions;
        SmallVector<SlotSemanticRewriteCertificate, 8> slotCertificates;
        std::string detail;
      };

      enum class SemanticInteractionKind {
        Plain,
        ChildSyntax,
        RawInvocation,
        Stringify,
        WideStringify,
        Paste,
        ChildSyntaxPaste,
        RawInvocationPaste,
        StringifyPaste,
        WideStringifyPaste,
        Mixed,
      };

      enum class SemanticInteractionFailure {
        None,
        NonCanonicalLogicalInput,
      };

      struct SemanticInteractionCertificate {
        bool valid = true;
        SemanticInteractionKind kind = SemanticInteractionKind::Plain;
        SemanticInteractionFailure failure = SemanticInteractionFailure::None;
        const RefoldModel::MacroInvocation *inv = nullptr;
        uint32_t argIdx = 0;
        bool touchesPaste = false;
        bool usesPreferredChildSyntax = false;
        bool usesRawInvocationPreservation = false;
        bool usesPassthroughFlatten = false;
        bool usesStringify = false;
        bool usesWideStringify = false;
        bool usesRawChildInvocationLogicalInput = false;
        SmallVector<SlotSemanticRewriteCertificate, 8> slotCertificates;
        SmallVector<std::string, 8> canonicalLogicalInputs;
        std::string detail;
      };

      struct SemanticInteractionSignature {
        bool touchesPaste = false;
        bool usesPreferredChildSyntax = false;
        bool usesRawInvocationPreservation = false;
        bool usesPassthroughFlatten = false;
        bool usesStringify = false;
        bool usesWideStringify = false;
        bool usesRawChildInvocationLogicalInput = false;
        SmallVector<std::string, 8> canonicalLogicalInputs;

        bool operator==(const SemanticInteractionSignature &other) const {
          return touchesPaste == other.touchesPaste &&
                 usesPreferredChildSyntax == other.usesPreferredChildSyntax &&
                 usesRawInvocationPreservation ==
                     other.usesRawInvocationPreservation &&
                 usesPassthroughFlatten == other.usesPassthroughFlatten &&
                 usesStringify == other.usesStringify &&
                 usesWideStringify == other.usesWideStringify &&
                 usesRawChildInvocationLogicalInput ==
                     other.usesRawChildInvocationLogicalInput &&
                 canonicalLogicalInputs == other.canonicalLogicalInputs;
        }
      };

      enum class FormalInteractionConsistencyFailure {
        None,
        DivergentSemanticEvidence,
      };

      struct FormalInteractionConsistencyCertificate {
        bool valid = true;
        FormalInteractionConsistencyFailure failure =
            FormalInteractionConsistencyFailure::None;
        const RefoldModel::MacroInvocation *inv = nullptr;
        uint32_t argIdx = 0;
        SemanticInteractionSignature signature;
        SmallVector<SemanticInteractionCertificate, 2> interactions;
        std::string detail;
      };

      enum class SubtreeInteractionConsistencyFailure {
        None,
        DivergentFormalSemantics,
      };

      struct SubtreeInteractionConsistencyCertificate {
        bool valid = true;
        SubtreeInteractionConsistencyFailure failure =
            SubtreeInteractionConsistencyFailure::None;
        SmallVector<FormalInteractionConsistencyCertificate, 16>
            formalConsistencies;
        std::string detail;
      };

      // Build a semantic rewrite certificate for one parent argument by
      // replaying the old child-slot decomposition against the new observed
      // text. The solver first tries to preserve child invocation syntax when
      // a slot can still be explained semantically; only then does it allow
      // passthrough flattening of the observed slot text.
      auto buildArgSemanticRewriteCertificate =
          [&](const ArgInvertibilityCertificate &cert, StringRef observedNew0,
              const DenseMap<uint64_t, std::string> *preferredChildSyntax)
          -> ArgSemanticRewriteCertificate {
        ArgSemanticRewriteCertificate argCert;
        StringRef observedNew = observedNew0.trim();
        argCert.observedNewText = observedNew.str();
        argCert.rawArgOldText = StringRef(cert.rawArgText).trim().str();

        // Literal-only arguments have no child slots to preserve. The new
        // observed text is therefore the new argument spelling directly.
        if (cert.kind == ArgInvertibilityKind::LiteralOnly) {
          argCert.rawArgNewText = observedNew.str();
          argCert.kind = (StringRef(argCert.rawArgNewText).trim() ==
                          StringRef(argCert.rawArgOldText).trim())
                             ? ArgSemanticRewriteCertificateKind::NoChange
                             : ArgSemanticRewriteCertificateKind::Unique;
          return argCert;
        }

        SmallVector<SlotRewriteDecision, 8> newParts;
        SmallVector<SlotSemanticRewriteCertificate, 8> newPartCertificates;
        SmallVector<SmallVector<SlotRewriteDecision, 8>, 2> newSolutions;
        SmallVector<SmallVector<SlotSemanticRewriteCertificate, 8>, 2>
            newSolutionCertificates;

        auto pieceMatchesWrapperCertificate =
            [&](const WrapperChainCertificate &wrapper,
                StringRef piece0) -> bool {
          // First check only the surface shape required by the wrapper selected
          // during old-text inversion: exact text, string literal, or wide
          // string literal.
          StringRef piece = piece0.trim();
          switch (wrapper.kind) {
          case WrapperChainKind::Exact:
            return true;
          case WrapperChainKind::StringLiteral:
            return stringutils::looksLikeStringLiteralToken(piece);
          case WrapperChainKind::WideStringLiteral:
            return piece.starts_with("L\"") ||
                   (piece.starts_with("L") &&
                    stringutils::looksLikeStringLiteralToken(
                        piece.drop_front(1)));
          }
          llvm_unreachable("invalid WrapperChainKind");
        };

        auto literalDecodesToCanonicalLogicalInput =
            [&](StringRef piece0, StringRef expected0) -> bool {
          // Stringified children are compared through the canonical inverse so
          // equivalent escaped/whitespace-normalized payloads collapse
          // together.
          auto decoded =
              (*deps_.argTextRecovery).UnstringifyLiteralToArgText(
                  piece0, /*allowTopLevelComma=*/true);
          if (!decoded)
            return false;

          auto canonDecoded =
              stringutils::canonicalizeStringifyInversePayload(*decoded);
          auto canonExpected =
              stringutils::canonicalizeStringifyInversePayload(expected0);
          if (!canonDecoded || !canonExpected)
            return false;

          // Require both sides to already be in canonical form before comparing
          // them. Otherwise a non-canonical literal spelling could be accepted
          // as if it were a unique logical child input.
          return StringRef(*canonDecoded).trim() ==
                     StringRef(*decoded).trim() &&
                 StringRef(*canonExpected).trim() == expected0.trim() &&
                 *canonDecoded == *canonExpected;
        };

        auto pieceMatchesWrapperLogicalInput =
            [&](const WrapperChainCertificate &wrapper,
                StringRef piece0) -> bool {
          // Validate both the wrapper surface and the logical child input it
          // denotes.
          if (!pieceMatchesWrapperCertificate(wrapper, piece0))
            return false;

          const StringRef logical = StringRef(wrapper.logicalInputText).trim();
          switch (wrapper.kind) {
          case WrapperChainKind::Exact:
            return piece0.trim() == logical;
          case WrapperChainKind::StringLiteral:
          case WrapperChainKind::WideStringLiteral:
            return literalDecodesToCanonicalLogicalInput(piece0, logical);
          }
          llvm_unreachable("invalid WrapperChainKind");
        };

        auto pieceMatchesAnyTrimmedCandidate =
            [&](StringRef piece0,
                const SmallVectorImpl<std::string> &candidates) -> bool {
          StringRef piece = piece0.trim();
          for (const auto &cand : candidates)
            if (piece == StringRef(cand).trim())
              return true;
          return false;
        };

        auto collectSlotSemanticRewriteCertificates =
            [&](const LexicalChildPlaceholder &slot,
                const WrapperChainCertificate &wrapper, StringRef piece0)
            -> SmallVector<SlotSemanticRewriteCertificate, 4> {
          SmallVector<SlotSemanticRewriteCertificate, 4> out;
          const StringRef piece = piece0.trim();

          auto addUnique = [&](SlotRewriteDecisionKind kind,
                               StringRef rebuilt0) {
            SlotSemanticRewriteCertificate cert;
            cert.kind = SlotSemanticRewriteCertificateKind::Unique;
            cert.decision.kind = kind;
            cert.decision.observedText = piece.str();
            cert.decision.rebuiltText = rebuilt0.str();
            cert.wrapperKind = wrapper.kind;
            cert.wrapperSource = wrapper.source;
            cert.logicalInputText = wrapper.logicalInputText;
            for (const auto &existing : out)
              if (existing == cert)
                return;
            out.push_back(std::move(cert));
          };

          if (preferredChildSyntax && slot.child) {
            // Prefer preserving a certified child invocation spelling when it
            // is semantically compatible with the piece observed in the new
            // text.
            auto it = preferredChildSyntax->find(slot.child->id);
            if (it != preferredChildSyntax->end() &&
                pieceMatchesWrapperCertificate(wrapper, piece)) {
              bool compatible = false;
              const StringRef preferredSyntax = StringRef(it->second).trim();

              switch (wrapper.source) {
              case WrapperObservedSource::ChildRawInvocation:
                // The old slot matched the raw child invocation surface.
                // Preserving child syntax is valid only if the new piece
                // denotes that preferred invocation syntax under the same
                // wrapper form.
                switch (wrapper.kind) {
                case WrapperChainKind::Exact:
                  compatible = piece == preferredSyntax;
                  break;
                case WrapperChainKind::StringLiteral:
                case WrapperChainKind::WideStringLiteral:
                  compatible = literalDecodesToCanonicalLogicalInput(
                      piece, preferredSyntax);
                  break;
                }
                break;

              case WrapperObservedSource::ChildExpansion:
                // The old slot matched the child's expansion surface. The
                // preferred child syntax is compatible only if the observed new
                // piece is still a valid new expansion candidate for that
                // child.
                switch (wrapper.kind) {
                case WrapperChainKind::Exact:
                  compatible = pieceMatchesAnyTrimmedCandidate(
                      piece, slot.newExpansionCandidates);
                  break;
                case WrapperChainKind::StringLiteral:
                case WrapperChainKind::WideStringLiteral:
                  compatible = llvm::any_of(
                      slot.newExpansionCandidates,
                      [&](const std::string &cand) {
                        return literalDecodesToCanonicalLogicalInput(piece,
                                                                     cand);
                      });
                  break;
                }
                break;
              }

              if (compatible)
                addUnique(SlotRewriteDecisionKind::PreferredChildSyntax,
                          preferredSyntax);
            }
          }

          if (!slot.rawInvocationText.empty() && !slot.observedForms.empty()) {
            // If no preferred syntax is available, retaining the original raw
            // child invocation is still valid when the new observed piece
            // realizes the same logical input under a certified wrapper form.
            for (const auto &form : slot.observedForms) {
              if (!pieceMatchesWrapperLogicalInput(form, piece))
                continue;
              addUnique(SlotRewriteDecisionKind::PreserveRawInvocation,
                        slot.rawInvocationText);
            }
          }

          return out;
        };

        auto addNewSolution =
            [&](const SmallVectorImpl<SlotRewriteDecision> &parts,
                const SmallVectorImpl<SlotSemanticRewriteCertificate>
                    &slotCertificates) {
              // Deduplicate equivalent decision vectors. Different traversal
              // paths can sometimes reconstruct the same slot decisions.
              SmallVector<SlotRewriteDecision, 8> copy(parts.begin(),
                                                       parts.end());
              for (const auto &existing : newSolutions)
                if (existing == copy)
                  return;

              newSolutions.push_back(std::move(copy));
              newSolutionCertificates.emplace_back(slotCertificates.begin(),
                                                   slotCertificates.end());
            };

        auto rebuildFromSolution =
            [&](const SmallVectorImpl<SlotRewriteDecision> &sol)
            -> std::string {
          // Reassemble the parent argument from fixed literals and the rebuilt
          // text selected for each child slot.
          std::string rebuilt;
          for (size_t i = 0; i < cert.slots.size(); ++i) {
            rebuilt += cert.literals[i];
            rebuilt += sol[i].rebuiltText;
          }
          rebuilt += cert.literals.back();
          return rebuilt;
        };

        auto solutionsCollapseToSameRebuilt = [&]() -> bool {
          // Multiple slot-level explanations are acceptable only if they
          // produce the exact same rebuilt parent argument text.
          if (newSolutions.empty())
            return false;
          std::string rebuilt = rebuildFromSolution(newSolutions[0]);
          for (size_t i = 1; i < newSolutions.size(); ++i)
            if (rebuildFromSolution(newSolutions[i]) != rebuilt)
              return false;
          return true;
        };

        auto solveNew = [&](auto &&self, size_t idx, size_t pos) -> void {
          // Once two distinct solutions are present, uniqueness has already
          // failed unless they later collapse to the same rebuilt argument.
          if (newSolutions.size() > 1)
            return;

          // Each slot is preceded by the fixed literal captured from the old
          // argument template. The new observed text must preserve those
          // literal boundaries.
          const StringRef lit = cert.literals[idx];
          if (pos > observedNew.size() ||
              !observedNew.drop_front(pos).starts_with(lit))
            return;
          pos += lit.size();

          if (idx == cert.slots.size()) {
            // All child slots were consumed; accept only full consumption of
            // the new observed text, including the trailing literal.
            if (pos == observedNew.size())
              addNewSolution(newParts, newPartCertificates);
            return;
          }

          const auto &slot = cert.slots[idx];

          // Reuse the old inversion's selected wrapper form for this slot. This
          // keeps the new reconstruction from silently changing a child from
          // raw invocation to expansion, or from exact text to stringized text.
          const WrapperChainCertificate wrapper =
              (idx < cert.chosenObservedFormIdx.size() &&
               cert.chosenObservedFormIdx[idx] < slot.observedForms.size())
                  ? slot.observedForms[cert.chosenObservedFormIdx[idx]]
                  : WrapperChainCertificate{};

          // The remaining fixed literals must fit after this slot piece. This
          // bounds the maximum slot length before we enumerate candidate
          // pieces.
          size_t minRemain = 0;
          for (size_t j = idx + 1; j < cert.literals.size(); ++j)
            minRemain += cert.literals[j].size();
          if (pos + minRemain > observedNew.size())
            return;

          const size_t maxLen = observedNew.size() - pos - minRemain;
          const StringRef rest = observedNew.drop_front(pos);
          const StringRef nextLit = cert.literals[idx + 1];

          auto enumerateSlotPieces = [&](auto &&emitPiece) {
            // Candidate slot pieces must be balanced refold fragments and must
            // end at token-like boundaries so reconstruction cannot split an
            // identifier or literal spelling accidentally.
            if (!nextLit.empty()) {
              // When the next fixed literal is known, only consider top-level
              // occurrences of that literal as the slot endpoint.
              enumerateTopLevelLiteralMatchesInRefoldText(
                  rest, nextLit, maxLen, [&](size_t found) {
                    StringRef piece = rest.take_front(found);
                    if (!isBalancedRefoldFragment(piece))
                      return;
                    emitPiece(piece);
                    if (newSolutions.size() > 1)
                      return;
                  });
            } else {
              // Without a following literal, every balanced top-level cut point
              // is a possible slot endpoint. The uniqueness checks below
              // decide whether any such split is acceptable.
              enumerateTopLevelBalancedCutPointsWithLexer(
                  rest, (*deps_.lexLang), [&](unsigned cut) {
                    const size_t len = static_cast<size_t>(cut);
                    if (len > maxLen)
                      return;
                    if (!isLikelyTokenBoundaryInRefoldText(rest, len) ||
                        !isBalancedRefoldFragment(rest.take_front(len)))
                      return;
                    emitPiece(rest.take_front(len));
                    if (newSolutions.size() > 1)
                      return;
                  });
            }
          };

          bool triedSemanticPreserve = false;

          // First try structure-preserving slot rewrites. Only if those do not
          // yield a unique rebuilt spelling do we consider flattening fallback.
          enumerateSlotPieces([&](StringRef piece) {
            auto semanticCerts =
                collectSlotSemanticRewriteCertificates(slot, wrapper, piece);
            if (semanticCerts.empty())
              return;

            triedSemanticPreserve = true;
            for (const auto &semanticCert : semanticCerts) {
              newParts.push_back(semanticCert.decision);
              newPartCertificates.push_back(semanticCert);
              self(self, idx + 1, pos + piece.size());
              newPartCertificates.pop_back();
              newParts.pop_back();

              if (newSolutions.size() > 1)
                return;
            }
          });

          // If semantic preservation already produced one or more solutions
          // that all rebuild to the same parent argument, do not explore the
          // weaker flattening fallback.
          if (triedSemanticPreserve && solutionsCollapseToSameRebuilt())
            return;

          enumerateSlotPieces([&](StringRef piece) {
            // Passthrough flattening is the explicit fallback: it keeps the new
            // observed text but records that no child syntax was preserved.
            SlotSemanticRewriteCertificate fallbackCert;
            fallbackCert.kind = SlotSemanticRewriteCertificateKind::Unique;
            fallbackCert.decision =
                SlotRewriteDecision{SlotRewriteDecisionKind::PassthroughFlatten,
                                    piece.str(), piece.str()};
            fallbackCert.wrapperKind = wrapper.kind;
            fallbackCert.wrapperSource = wrapper.source;
            fallbackCert.logicalInputText = wrapper.logicalInputText;

            newParts.push_back(fallbackCert.decision);
            newPartCertificates.push_back(fallbackCert);
            self(self, idx + 1, pos + piece.size());
            newPartCertificates.pop_back();
            newParts.pop_back();
          });
        };

        solveNew(solveNew, 0, 0);

        if (newSolutions.empty()) {
          argCert.detail = "new arg did not admit any structurally valid slot "
                           "reconstruction";
          return argCert;
        }

        // Different slot decisions are still acceptable if they reconstruct the
        // same final argument text. Different final text means the rewrite is
        // ambiguous.
        std::string rebuilt = rebuildFromSolution(newSolutions[0]);
        for (size_t i = 1; i < newSolutions.size(); ++i) {
          if (rebuildFromSolution(newSolutions[i]) != rebuilt) {
            argCert.detail = "new arg admitted multiple non-equivalent "
                             "structural reconstructions";
            return argCert;
          }
        }

        argCert.slotDecisions.assign(newSolutions[0].begin(),
                                     newSolutions[0].end());
        if (!newSolutionCertificates.empty()) {
          argCert.slotCertificates.assign(newSolutionCertificates[0].begin(),
                                          newSolutionCertificates[0].end());
        }

        argCert.rawArgNewText = rebuilt;
        argCert.kind = (StringRef(argCert.rawArgNewText).trim() ==
                        StringRef(argCert.rawArgOldText).trim())
                           ? ArgSemanticRewriteCertificateKind::NoChange
                           : ArgSemanticRewriteCertificateKind::Unique;
        return argCert;
      };

      // Build the full old-to-new argument rewrite certificate for one observed
      // parent formal. First prove that the old observed text maps uniquely
      // back to the parent's argument structure, then replay that structure
      // against the new observed text to derive the rewritten argument.
      auto buildObservedArgRewriteCertificate =
          [&](const RefoldModel::MacroInvocation &parent, uint32_t parentFormal,
              StringRef observedOld0, StringRef observedNew0,
              const DenseMap<uint64_t, std::string> *preferredChildSyntax)
          -> ArgSemanticRewriteCertificate {
        auto invertibilityCert = buildArgInvertibilityCertificate(
            parent, parentFormal, observedOld0);
        if (!invertibilityCert) {
          ArgSemanticRewriteCertificate argCert;
          argCert.failure =
              ArgSemanticRewriteFailure::MissingStructuralTemplate;
          argCert.detail = "observed old arg text did not match a unique "
                           "structural template";
          return argCert;
        }
        return buildArgSemanticRewriteCertificate(
            *invertibilityCert, observedNew0, preferredChildSyntax);
      };

      struct FormalTextPair {
        std::string oldText;
        std::string newText;
      };

      struct ObservedFormalConstraint {
        std::string oldText;
        std::string newText;
      };

      auto formatFormalTextPairMap =
          [&](const DenseMap<uint32_t, FormalTextPair> &formals) {
            SmallVector<uint32_t, 8> argIdxs;
            argIdxs.reserve(formals.size());
            for (const auto &kvLocal : formals)
              argIdxs.push_back(kvLocal.first);
            llvm::sort(argIdxs);

            std::string out;
            raw_string_ostream os(out);
            os << "{";
            for (size_t i = 0; i < argIdxs.size(); ++i) {
              if (i)
                os << ", ";
              const uint32_t argIdx = argIdxs[i];
              const auto it = formals.find(argIdx);
              os << argIdx << ":'"
                 << stringutils::showWsWithClip(it->second.oldText, 80)
                 << "'->'"
                 << stringutils::showWsWithClip(it->second.newText, 80) << "'";
            }
            os << "}";
            return os.str();
          };

      enum class FormalRewriteCertificateKind {
        NoChange,
        Unique,
        Invalid,
      };

      enum class FormalRewriteFailure {
        None,
        MissingArgumentText,
        MissingStructuralTemplate,
        RawRewriteNotCertifiable,
        MergeConflict,
        ArityChange,
        OccurrenceMismatch,
        InteractionConflict,
      };

      enum class RawFormalValidationFailure {
        None,
        ArityChange,
        OccurrenceMismatch,
      };

      struct RawFormalValidationCertificate {
        bool valid = false;
        RawFormalValidationFailure failure = RawFormalValidationFailure::None;
        const RefoldModel::MacroInvocation *inv = nullptr;
        uint32_t argIdx = 0;
        std::string oldText;
        std::string newText;
        std::string detail;
      };

      enum class PasteRewriteValidationFailure {
        None,
        MissingInvocationText,
        MissingArgumentRanges,
        PasteMismatch,
      };

      struct PasteRewriteValidationCertificate {
        bool required = false;
        bool valid = true;
        bool deferred = false;
        const RefoldModel::MacroInvocation *inv = nullptr;
        DenseMap<uint32_t, std::string> replacementByArgIdx;
        PasteRewriteValidationFailure failure =
            PasteRewriteValidationFailure::None;
        std::string detail;
      };

      struct FormalRewriteCertificate {
        FormalRewriteCertificateKind kind =
            FormalRewriteCertificateKind::Invalid;
        FormalRewriteFailure failure = FormalRewriteFailure::None;
        const RefoldModel::MacroInvocation *inv = nullptr;
        uint32_t argIdx = 0;
        std::string oldText;
        std::string newText;
        SmallVector<FormalTextPair, 2> candidateRewrites;
        SmallVector<ArgSemanticRewriteCertificate, 2> argRewriteCertificates;
        SmallVector<SemanticInteractionCertificate, 2> interactionCertificates;
        FormalInteractionConsistencyCertificate interactionConsistency;
        RawFormalValidationCertificate validation;
        std::string detail;
      };

      SmallVector<diffutils::Hunk, 1> tokenHunksForCheck;
      tokenHunksForCheck.push_back(h);
      ArrayRef<diffutils::Hunk> tokenHunksAR(tokenHunksForCheck);

      std::function<std::optional<std::string>(StringRef,
                                               ArrayRef<FormalTextPair>)>
          mergeCompatibleFormalRewrites;

      // Validate a proposed raw formal-argument replacement before it is used
      // to rebuild an invocation. This enforces arity safety and, unless
      // explicitly deferred, checks that the same replacement explains every
      // occurrence of the formal in B.
      auto buildRawFormalValidationCertificate =
          [&](const RefoldModel::MacroInvocation &inv, uint32_t argIdx,
              StringRef oldText0, StringRef newText0, StringRef traceStage,
              bool skipOccurrenceConsistency =
                  false) -> RawFormalValidationCertificate {
        RawFormalValidationCertificate cert;
        cert.inv = &inv;
        cert.argIdx = argIdx;
        cert.oldText = oldText0.trim().str();
        cert.newText = newText0.trim().str();

        const StringRef oldText = StringRef(cert.oldText).trim();
        const StringRef newText = StringRef(cert.newText).trim();

        // No text change means the replacement is trivially valid; there is no
        // need to run arity or occurrence checks.
        if (oldText == newText) {
          cert.valid = true;
          return cert;
        }

        // Non-variadic formals cannot receive a top-level comma, because that
        // would change the macro call's argument structure rather than only
        // replacing this formal's payload.
        if (!isVariadicFormalInInvocation(inv, argIdx) &&
            refoldMacroActualHasTopLevelComma(newText, (*deps_.lexLang))) {
          cert.failure = RawFormalValidationFailure::ArityChange;
          cert.detail = formatv("{0}: inv id={1} name={2} argIdx={3} arity "
                                "safety failed",
                                traceStage, inv.id, inv.name, argIdx)
                            .str();
          return cert;
        }

        // Some semantic rewrite paths validate occurrence consistency through a
        // stronger structural certificate. In those cases, this raw-text
        // validator only performs local arity/surface checks and records the
        // deferral.
        if (skipOccurrenceConsistency) {
          cert.valid = true;
          cert.detail =
              formatv("{0}: inv id={1} name={2} argIdx={3} occurrence "
                      "consistency deferred to semantic certificate "
                      "pipeline",
                      traceStage, inv.id, inv.name, argIdx)
                  .str();
          return cert;
        }

        // Require the proposed old->new formal rewrite to explain all non-paste
        // occurrences of this formal in the B stream. Paste-specific semantic
        // proof is intentionally ignored here because it is handled by
        // dedicated paste replay paths.
        if (!MacroArgReplacementMatchesAllOccurrencesInBIgnorePasteSemanticProof(
                inv, argIdx, oldText, newText, tokenHunksAR)) {
          cert.failure = RawFormalValidationFailure::OccurrenceMismatch;
          cert.detail =
              formatv("{0}: inv id={1} name={2} argIdx={3} occurrence "
                      "consistency failed",
                      traceStage, inv.id, inv.name, argIdx)
                  .str();
          return cert;
        }

        cert.valid = true;
        return cert;
      };

      // Validate paste-token consistency for a proposed set of
      // invocation-argument replacements. This check is required only when at
      // least one replaced formal contributes to a paste span; in that case the
      // rebuilt arguments must replay every pasted token occurrence in B, not
      // just the occurrence that triggered the rewrite.
      auto buildPasteRewriteValidationCertificate =
          [&](const RefoldModel::MacroInvocation &inv,
              const DenseMap<uint32_t, std::string> &replacementByArgIdx,
              StringRef traceStage,
              std::optional<StringRef> callsiteTextOverride = std::nullopt,
              ArrayRef<std::pair<size_t, size_t>> callsiteArgRangesOverride =
                  ArrayRef<std::pair<size_t, size_t>>())
          -> PasteRewriteValidationCertificate {
        PasteRewriteValidationCertificate cert;
        cert.inv = &inv;
        cert.replacementByArgIdx = replacementByArgIdx;

        // First determine whether this certificate is even needed. If none of
        // the replaced formals participate in paste spans, paste replay cannot
        // constrain the candidate rewrite.
        for (const auto &kv : replacementByArgIdx) {
          const uint32_t argIdx = kv.first;
          for (const auto &ps : inv.pasteSpans) {
            if (ps.argIdx == argIdx) {
              cert.required = true;
              break;
            }
          }
          if (cert.required)
            break;
        }

        if (!cert.required)
          return cert;

        StringRef callsiteText;
        ArrayRef<std::pair<size_t, size_t>> callsiteArgRanges;
        std::optional<SmallVector<std::pair<size_t, size_t>, 8>> ownedArgRanges;

        if (callsiteTextOverride) {
          // Some callers validate against a freshly rewritten invocation
          // surface. In that mode, the caller must also provide argument ranges
          // relative to the override text; the producer's original ranges no
          // longer apply.
          callsiteText = *callsiteTextOverride;
          if (callsiteArgRangesOverride.empty()) {
            cert.valid = false;
            cert.failure = PasteRewriteValidationFailure::MissingArgumentRanges;
            cert.detail =
                formatv("{0}: paste consistency unavailable: inv id={1} "
                        "name={2} arg ranges unavailable",
                        traceStage, inv.id, inv.name)
                    .str();
            return cert;
          }
          callsiteArgRanges = callsiteArgRangesOverride;
        } else {
          // Default mode validates against the invocation spelling recorded by
          // the producer and derives one argument range per formal from that
          // spelling.
          if (!inv.invText) {
            cert.valid = false;
            cert.failure = PasteRewriteValidationFailure::MissingInvocationText;
            cert.detail =
                formatv("{0}: paste consistency unavailable: inv id={1} "
                        "name={2} hasInvText=0",
                        traceStage, inv.id, inv.name)
                    .str();
            return cert;
          }

          callsiteText = StringRef(*inv.invText);
          auto invArgRangesOpt =
              GetMacroInvocationFormalArgContentRanges(inv, callsiteText);
          if (!invArgRangesOpt) {
            cert.valid = false;
            cert.failure = PasteRewriteValidationFailure::MissingArgumentRanges;
            cert.detail =
                formatv("{0}: paste consistency unavailable: inv id={1} "
                        "name={2} arg ranges unavailable",
                        traceStage, inv.id, inv.name)
                    .str();
            return cert;
          }

          // Keep the derived ranges alive while exposing them through ArrayRef
          // below.
          ownedArgRanges.emplace(invArgRangesOpt->begin(),
                                 invArgRangesOpt->end());
          callsiteArgRanges = *ownedArgRanges;
        }

        // The decisive paste check: after applying all proposed argument
        // replacements, every paste token produced by this invocation must
        // match the corresponding B-side pasted token spelling.
        if (!PasteArgReplacementsMatchAllPasteTokensInB(
                inv, callsiteText, callsiteArgRanges,
                cert.replacementByArgIdx)) {
          cert.valid = false;
          cert.failure = PasteRewriteValidationFailure::PasteMismatch;
          cert.detail =
              formatv("{0}: paste-token consistency failed: inv id={1} "
                      "name={2} touchedArgs={3}",
                      traceStage, inv.id, inv.name,
                      cert.replacementByArgIdx.size())
                  .str();
          return cert;
        }

        return cert;
      };

      // Summarize the semantic features involved in one argument rewrite. This
      // certificate records whether the rewrite preserved child syntax,
      // flattened a child slot, passed through stringify/wide-stringify
      // wrappers, and/or touched paste spans, then classifies the combined
      // interaction for later validation and diagnostics.
      auto buildSemanticInteractionCertificate =
          [&](const RefoldModel::MacroInvocation &inv, uint32_t argIdx,
              const ArgSemanticRewriteCertificate &argCert,
              StringRef traceStage) -> SemanticInteractionCertificate {
        SemanticInteractionCertificate cert;
        cert.inv = &inv;
        cert.argIdx = argIdx;
        cert.slotCertificates.assign(argCert.slotCertificates.begin(),
                                     argCert.slotCertificates.end());

        auto addLogicalInput = [&](StringRef logical0) {
          // Keep a unique list of canonical logical child inputs represented by
          // the slot certificates. This is diagnostic/proof metadata, not
          // replacement text.
          const std::string normalized = logical0.trim().str();
          if (normalized.empty())
            return;
          for (const auto &existing : cert.canonicalLogicalInputs)
            if (existing == normalized)
              return;
          cert.canonicalLogicalInputs.push_back(normalized);
        };

        // Paste participation is a property of the invocation/formal as a
        // whole, not of any one wrapper slot.
        cert.touchesPaste = invocationArgTouchesPaste(inv, argIdx);

        for (const auto &slotCert : argCert.slotCertificates) {
          // Record how each child slot was rebuilt: preferred new child syntax,
          // preserved raw child invocation spelling, or flattened observed
          // text.
          switch (slotCert.decision.kind) {
          case SlotRewriteDecisionKind::PreferredChildSyntax:
            cert.usesPreferredChildSyntax = true;
            break;
          case SlotRewriteDecisionKind::PreserveRawInvocation:
            cert.usesRawInvocationPreservation = true;
            break;
          case SlotRewriteDecisionKind::PassthroughFlatten:
            cert.usesPassthroughFlatten = true;
            break;
          }

          if (slotCert.wrapperSource ==
              WrapperObservedSource::ChildRawInvocation)
            cert.usesRawChildInvocationLogicalInput = true;

          // Exact wrappers contribute their logical input directly. Stringified
          // wrappers must first prove that their inverse payload is canonical,
          // so later paste/stringify interaction checks do not depend on
          // ambiguous escape spellings.
          switch (slotCert.wrapperKind) {
          case WrapperChainKind::Exact:
            addLogicalInput(slotCert.logicalInputText);
            break;
          case WrapperChainKind::StringLiteral:
          case WrapperChainKind::WideStringLiteral: {
            cert.usesStringify = true;
            if (slotCert.wrapperKind == WrapperChainKind::WideStringLiteral)
              cert.usesWideStringify = true;

            auto canon = stringutils::canonicalizeStringifyInversePayload(
                slotCert.logicalInputText);
            if (!canon || StringRef(*canon).trim() !=
                              StringRef(slotCert.logicalInputText).trim()) {
              cert.valid = false;
              cert.failure =
                  SemanticInteractionFailure::NonCanonicalLogicalInput;
              cert.detail = formatv("{0}: inv id={1} name={2} argIdx={3} "
                                    "interaction canonical logical payload "
                                    "failed",
                                    traceStage, inv.id, inv.name, argIdx)
                                .str();
              return cert;
            }

            addLogicalInput(*canon);
            break;
          }
          }
        }

        const bool hasChildSyntax =
            cert.usesPreferredChildSyntax || cert.usesRawInvocationPreservation;
        const bool hasStringify = cert.usesStringify;
        const bool hasPaste = cert.touchesPaste;

        // Classify the interaction from most constrained combinations to
        // simpler single-feature cases. Paste+stringify and paste+child-syntax
        // combinations are more specific than plain paste/stringify because
        // they require additional cross-feature validation.
        if (hasPaste && hasStringify && cert.usesWideStringify) {
          cert.kind = SemanticInteractionKind::WideStringifyPaste;
        } else if (hasPaste && hasStringify) {
          cert.kind = SemanticInteractionKind::StringifyPaste;
        } else if (hasPaste && cert.usesRawInvocationPreservation) {
          cert.kind = SemanticInteractionKind::RawInvocationPaste;
        } else if (hasPaste && cert.usesPreferredChildSyntax) {
          cert.kind = SemanticInteractionKind::ChildSyntaxPaste;
        } else if (hasPaste) {
          cert.kind = SemanticInteractionKind::Paste;
        } else if (hasStringify && cert.usesWideStringify) {
          cert.kind = SemanticInteractionKind::WideStringify;
        } else if (hasStringify) {
          cert.kind = SemanticInteractionKind::Stringify;
        } else if (cert.usesRawInvocationPreservation) {
          cert.kind = SemanticInteractionKind::RawInvocation;
        } else if (cert.usesPreferredChildSyntax) {
          cert.kind = SemanticInteractionKind::ChildSyntax;
        } else {
          cert.kind = SemanticInteractionKind::Plain;
        }

        // If all three major mechanisms interact, keep the coarser Mixed
        // category so downstream code does not accidentally treat it as one of
        // the simpler two-feature cases.
        if (hasChildSyntax && hasStringify && hasPaste)
          cert.kind = SemanticInteractionKind::Mixed;

        cert.detail =
            formatv("semantic interaction: inv id={0} name={1} argIdx={2}"
                    " kind={3} slots={4} logicalInputs={5} paste={6} "
                    "rawChildInput={7}",
                    inv.id, inv.name, argIdx, static_cast<unsigned>(cert.kind),
                    cert.slotCertificates.size(),
                    cert.canonicalLogicalInputs.size(),
                    cert.touchesPaste ? 1 : 0,
                    cert.usesRawChildInvocationLogicalInput ? 1 : 0)
                .str();
        return cert;
      };

      // Build the normalized comparison key for a semantic interaction
      // certificate. The signature keeps only the feature flags and
      // deduplicated logical inputs needed to compare interaction shapes across
      // candidate rewrites.
      auto buildSemanticInteractionSignature =
          [&](const SemanticInteractionCertificate &interaction)
          -> SemanticInteractionSignature {
        SemanticInteractionSignature sig;
        sig.touchesPaste = interaction.touchesPaste;
        sig.usesPreferredChildSyntax = interaction.usesPreferredChildSyntax;
        sig.usesRawInvocationPreservation =
            interaction.usesRawInvocationPreservation;
        sig.usesPassthroughFlatten = interaction.usesPassthroughFlatten;
        sig.usesStringify = interaction.usesStringify;
        sig.usesWideStringify = interaction.usesWideStringify;
        sig.usesRawChildInvocationLogicalInput =
            interaction.usesRawChildInvocationLogicalInput;
        sig.canonicalLogicalInputs.assign(
            interaction.canonicalLogicalInputs.begin(),
            interaction.canonicalLogicalInputs.end());
        llvm::sort(sig.canonicalLogicalInputs);
        sig.canonicalLogicalInputs.erase(
            std::unique(sig.canonicalLogicalInputs.begin(),
                        sig.canonicalLogicalInputs.end()),
            sig.canonicalLogicalInputs.end());
        return sig;
      };

      // Verify that every observation for the same formal argument carries the
      // same semantic interaction shape. This prevents one occurrence from
      // being justified as, for example, paste+stringify while another
      // occurrence of the same formal is justified through a different
      // child-syntax or flattening path.
      auto buildFormalInteractionConsistencyCertificate =
          [&](const RefoldModel::MacroInvocation &inv, uint32_t argIdx,
              ArrayRef<SemanticInteractionCertificate> interactions,
              StringRef traceStage) -> FormalInteractionConsistencyCertificate {
        FormalInteractionConsistencyCertificate cert;
        cert.inv = &inv;
        cert.argIdx = argIdx;
        cert.interactions.assign(interactions.begin(), interactions.end());

        // No observations means there is no conflicting semantic evidence for
        // this formal. Treat that as a vacuous success.
        if (interactions.empty()) {
          cert.detail = formatv("{0}: inv id={1} name={2} argIdx={3} semantic "
                                "interaction convergence vacuously satisfied",
                                traceStage, inv.id, inv.name, argIdx)
                            .str();
          return cert;
        }

        // Use the first observation as the required interaction signature, then
        // demand exact signature equality for all remaining observations of the
        // same formal.
        cert.signature =
            buildSemanticInteractionSignature(interactions.front());
        for (size_t i = 1; i < interactions.size(); ++i) {
          auto sig = buildSemanticInteractionSignature(interactions[i]);
          if (!(sig == cert.signature)) {
            cert.valid = false;
            cert.failure =
                FormalInteractionConsistencyFailure::DivergentSemanticEvidence;
            cert.detail =
                formatv("{0}: inv id={1} name={2} argIdx={3} semantic "
                        "interaction evidence diverged across "
                        "observations",
                        traceStage, inv.id, inv.name, argIdx)
                    .str();
            return cert;
          }
        }

        cert.detail =
            formatv("{0}: inv id={1} name={2} argIdx={3} semantic "
                    "interaction convergence satisfied "
                    "logicalInputs={4} paste={5} stringify={6} wide={7} "
                    "rawInvocation={8} childSyntax={9} passthrough={10}",
                    traceStage, inv.id, inv.name, argIdx,
                    cert.signature.canonicalLogicalInputs.size(),
                    cert.signature.touchesPaste ? 1 : 0,
                    cert.signature.usesStringify ? 1 : 0,
                    cert.signature.usesWideStringify ? 1 : 0,
                    cert.signature.usesRawInvocationPreservation ? 1 : 0,
                    cert.signature.usesPreferredChildSyntax ? 1 : 0,
                    cert.signature.usesPassthroughFlatten ? 1 : 0)
                .str();
        return cert;
      };

      // Build the certified rewrite for one formal argument from all observed
      // old->new constraints collected for that formal. Each observation is
      // first structurally certified, the semantic interaction shapes must
      // converge, and the resulting candidate rewrites must merge into one
      // arity-safe formal replacement.
      auto buildObservedFormalRewriteCertificate =
          [&](const RefoldModel::MacroInvocation &inv, uint32_t argIdx,
              ArrayRef<ObservedFormalConstraint> observedConstraints,
              const DenseMap<uint64_t, std::string> *preferredChildSyntax,
              StringRef traceStage) -> FormalRewriteCertificate {
        FormalRewriteCertificate cert;
        cert.inv = &inv;
        cert.argIdx = argIdx;

        // Start from the original call-site argument spelling. All candidate
        // rewrites for this formal are merged relative to this same old text.
        auto argText = getInvocationArgText(inv, argIdx);
        if (!argText) {
          cert.failure = FormalRewriteFailure::MissingArgumentText;
          cert.detail =
              formatv("{0}: inv id={1} name={2} argIdx={3} text unavailable",
                      traceStage, inv.id, inv.name, argIdx)
                  .str();
          return cert;
        }

        const StringRef oldTrim = argText->trim();

        for (const auto &constraint : observedConstraints) {
          // Convert each observed expansion-level old/new pair into an
          // argument-level semantic rewrite certificate. This is where
          // child-slot/template evidence is used to map observed text back to
          // the parent formal.
          auto argRewriteCert = buildObservedArgRewriteCertificate(
              inv, argIdx, constraint.oldText, constraint.newText,
              preferredChildSyntax);
          if (argRewriteCert.kind ==
              ArgSemanticRewriteCertificateKind::Invalid) {
            cert.failure =
                argRewriteCert.failure ==
                        ArgSemanticRewriteFailure::MissingStructuralTemplate
                    ? FormalRewriteFailure::MissingStructuralTemplate
                    : FormalRewriteFailure::RawRewriteNotCertifiable;
            cert.detail = formatv("{0}: inv id={1} name={2} argIdx={3} raw "
                                  "rewrite not certifiable ({4})",
                                  traceStage, inv.id, inv.name, argIdx,
                                  argRewriteCert.detail)
                              .str();
            return cert;
          }

          // Classify the semantic mechanisms involved in this observation
          // (child-syntax preservation, raw invocation preservation, stringify,
          // paste, flattening). Later all observations for the same formal
          // must agree on this interaction shape.
          auto interactionCert = buildSemanticInteractionCertificate(
              inv, argIdx, argRewriteCert, traceStage);
          if (!interactionCert.valid) {
            cert.failure = FormalRewriteFailure::RawRewriteNotCertifiable;
            cert.detail = interactionCert.detail;
            return cert;
          }

          cert.argRewriteCertificates.push_back(argRewriteCert);
          cert.interactionCertificates.push_back(interactionCert);

          // Store the concrete old->new formal-text rewrite proposed by this
          // observation. The merge step below will require all observations to
          // be compatible with one final replacement.
          cert.candidateRewrites.push_back(FormalTextPair{
              oldTrim.str(),
              StringRef(argRewriteCert.rawArgNewText).trim().str()});
        }

        // All observations of this formal must use the same semantic proof
        // shape. A mix such as one occurrence requiring paste+stringify and
        // another requiring plain child syntax is not treated as one coherent
        // formal rewrite.
        cert.interactionConsistency =
            buildFormalInteractionConsistencyCertificate(
                inv, argIdx, cert.interactionCertificates, traceStage);
        if (!cert.interactionConsistency.valid) {
          cert.failure = FormalRewriteFailure::InteractionConflict;
          cert.detail = cert.interactionConsistency.detail;
          return cert;
        }

        // Collapse all observation-level candidate rewrites into one
        // replacement for the formal. Conflicting replacements fail closed
        // instead of picking one.
        auto merged =
            mergeCompatibleFormalRewrites(oldTrim, cert.candidateRewrites);
        if (!merged) {
          cert.failure = FormalRewriteFailure::MergeConflict;
          cert.detail =
              formatv("{0}: inv id={1} name={2} argIdx={3} rewrite merge "
                      "conflicted",
                      traceStage, inv.id, inv.name, argIdx)
                  .str();
          return cert;
        }

        const StringRef mergedTrim = StringRef(*merged).trim();
        cert.oldText = oldTrim.str();
        cert.newText = mergedTrim.str();

        // A fully certified rewrite can still collapse to no change after
        // merging.
        if (mergedTrim == oldTrim) {
          cert.kind = FormalRewriteCertificateKind::NoChange;
          cert.detail =
              formatv("{0}: inv id={1} name={2} argIdx={3} certified "
                      "rewrite collapsed to no-change old='{4}' new='{5}'"
                      " argRewriteCerts={6} interactionCerts={7}",
                      traceStage, inv.id, inv.name, argIdx, oldTrim, mergedTrim,
                      cert.argRewriteCertificates.size(),
                      cert.interactionCertificates.size())
                  .str();
          return cert;
        }

        // If a stronger semantic pipeline already validated the occurrence
        // behavior for child syntax, stringify, or paste interactions, the
        // raw-text occurrence check is deferred to that certificate. Plain
        // rewrites still use the direct all-occurrences validation.
        const bool deferOccurrenceConsistency =
            cert.interactionConsistency.signature.usesPreferredChildSyntax ||
            cert.interactionConsistency.signature
                .usesRawInvocationPreservation ||
            cert.interactionConsistency.signature.usesStringify ||
            cert.interactionConsistency.signature.usesWideStringify ||
            cert.interactionConsistency.signature.touchesPaste;

        cert.validation = buildRawFormalValidationCertificate(
            inv, argIdx, oldTrim, mergedTrim, traceStage,
            deferOccurrenceConsistency);
        if (!cert.validation.valid) {
          switch (cert.validation.failure) {
          case RawFormalValidationFailure::ArityChange:
            cert.failure = FormalRewriteFailure::ArityChange;
            break;
          case RawFormalValidationFailure::OccurrenceMismatch:
            cert.failure = FormalRewriteFailure::OccurrenceMismatch;
            break;
          case RawFormalValidationFailure::None:
            cert.failure = FormalRewriteFailure::None;
            break;
          }
          cert.detail = cert.validation.detail;
          return cert;
        }

        cert.kind = FormalRewriteCertificateKind::Unique;
        return cert;
      };

      // Bridge a certified child invocation rewrite back into the parent
      // call-site argument that lexically contains that child. This succeeds
      // only when the child appears as one unambiguous top-level placeholder
      // inside exactly one parent formal, producing a single parent formal
      // old->new text pair.
      auto tryLexicalChildBridge =
          [&](const RefoldModel::MacroInvocation &parent,
              const RefoldModel::MacroInvocation &child,
              const std::string &rewrittenChildSyntax)
          -> std::optional<DenseMap<uint32_t, FormalTextPair>> {
        DenseMap<uint32_t, FormalTextPair> out;

        // Empty child syntax cannot produce a meaningful parent-argument
        // rewrite.
        if (rewrittenChildSyntax.empty())
          return std::nullopt;

        std::optional<uint32_t> matchedFormal;
        std::optional<LexicalChildPlaceholder> matchedSlot;

        // Search every parent formal for a top-level lexical child placeholder
        // corresponding to the rewritten child invocation.
        for (uint32_t parentFormal = 0;
             parentFormal < parent.invArgRanges.size(); ++parentFormal) {
          auto argInfo = getTrimmedInvocationArgInfo(parent, parentFormal);
          if (!argInfo)
            continue;

          auto slots = getTopLevelLexicalChildrenInArg(parent, parentFormal);
          for (const auto &slot : slots) {
            if (!slot.child || slot.child->id != child.id)
              continue;

            // The bridge must be unambiguous. If the same child can be
            // associated with multiple parent formals/slots, do not choose one
            // heuristically.
            if (matchedFormal)
              return std::nullopt;

            matchedFormal = parentFormal;
            matchedSlot = slot;
          }
        }

        if (!matchedFormal || !matchedSlot)
          return std::nullopt;

        auto argInfo = getTrimmedInvocationArgInfo(parent, *matchedFormal);
        if (!argInfo)
          return std::nullopt;

        // Revalidate the placeholder bounds against the trimmed parent argument
        // text before using them as replacement byte offsets.
        if (matchedSlot->relEnd < matchedSlot->relBegin ||
            matchedSlot->relEnd > argInfo->text.size())
          return std::nullopt;

        // Replace only the child placeholder inside the parent argument,
        // preserving the surrounding literal caller text.
        std::string rewrittenArg = stringutils::replaceRange(
            argInfo->text, matchedSlot->relBegin, matchedSlot->relEnd,
            rewrittenChildSyntax);

        out[*matchedFormal] =
            FormalTextPair{StringRef(argInfo->text).trim().str(),
                           StringRef(rewrittenArg).trim().str()};
        return out;
      };

      // Merge multiple certified rewrites for the same formal argument into one
      // replacement. Every rewrite must agree on the same original formal text;
      // compatible replacements are then reconciled by the shared string
      // replacement merger.
      mergeCompatibleFormalRewrites =
          [&](StringRef baseOld0,
              ArrayRef<FormalTextPair> rewrites) -> std::optional<std::string> {
        const StringRef baseOld = baseOld0.trim();
        std::vector<std::string> replacementStorage;
        replacementStorage.reserve(rewrites.size());

        for (const auto &rewrite : rewrites) {
          if (StringRef(rewrite.oldText).trim() != baseOld)
            return std::nullopt;
          replacementStorage.push_back(
              StringRef(rewrite.newText).trim().str());
        }

        SmallVector<StringRef, 8> replacementRefs;
        replacementRefs.reserve(replacementStorage.size());
        for (const std::string &replacement : replacementStorage)
          replacementRefs.push_back(StringRef(replacement));

        return mergeCompatibleStringReplacements(baseOld, replacementRefs);
      };

      enum class InvocationRewriteCertificateKind {
        NoChange,
        Unique,
        Invalid,
      };

      enum class InvocationRewriteFailure {
        None,
        ArityChange,
        OccurrenceMismatch,
        MissingInvocationText,
        MissingArgumentRanges,
        PasteMismatch,
      };

      struct CertifiedFormalRewrite {
        uint32_t argIdx = 0;
        std::string oldText;
        std::string newText;
      };

      struct InvocationRewriteCertificate {
        InvocationRewriteCertificateKind kind =
            InvocationRewriteCertificateKind::Invalid;
        InvocationRewriteFailure failure = InvocationRewriteFailure::None;
        const RefoldModel::MacroInvocation *inv = nullptr;
        SmallVector<CertifiedFormalRewrite, 4> rewrites;
        DenseMap<uint32_t, std::string> replacementByArgIdx;
        SmallVector<RawFormalValidationCertificate, 4> formalValidations;
        PasteRewriteValidationCertificate pasteValidation;
        bool touchesPaste = false;
        std::string rewrittenInvocationSyntax;
        std::string detail;
      };

      // Build an invocation-level rewrite certificate from the certified formal
      // rewrites for that invocation. Each formal replacement is validated for
      // arity and occurrence consistency, paste replay is checked once the full
      // replacement set is known, and the certificate records whether the
      // invocation is unchanged or has one unique validated rewrite.
      auto buildInvocationRewriteCertificate =
          [&](const RefoldModel::MacroInvocation &inv,
              const DenseMap<uint32_t, FormalTextPair> &formals,
              StringRef traceStage,
              std::optional<StringRef> callsiteTextOverride = std::nullopt,
              ArrayRef<std::pair<size_t, size_t>> callsiteArgRangesOverride =
                  ArrayRef<std::pair<size_t, size_t>>(),
              ArrayRef<uint32_t> deferOccurrenceArgIdxs =
                  ArrayRef<uint32_t>()) -> InvocationRewriteCertificate {
        InvocationRewriteCertificate cert;
        cert.inv = &inv;

        // DenseMap iteration is intentionally unordered, so process formals in
        // sorted argument order for deterministic validation, diagnostics, and
        // replay state.
        SmallVector<uint32_t, 8> argOrder;
        argOrder.reserve(formals.size());
        for (const auto &kv : formals)
          argOrder.push_back(kv.first);
        llvm::sort(argOrder);

        for (uint32_t argIdx : argOrder) {
          auto it = formals.find(argIdx);
          if (it == formals.end())
            continue;

          StringRef oldText = StringRef(it->second.oldText).trim();
          StringRef newText = StringRef(it->second.newText).trim();

          // Some semantic paths already discharge occurrence consistency for a
          // formal through a stronger structural proof. Those arguments still
          // get arity and surface validation here, but the raw all-occurrences
          // check is deferred.
          const bool deferOccurrenceConsistency =
              llvm::is_contained(deferOccurrenceArgIdxs, argIdx);
          auto validation = buildRawFormalValidationCertificate(
              inv, argIdx, oldText, newText, traceStage,
              deferOccurrenceConsistency);

          cert.formalValidations.push_back(validation);
          if (!validation.valid) {
            switch (validation.failure) {
            case RawFormalValidationFailure::ArityChange:
              cert.failure = InvocationRewriteFailure::ArityChange;
              break;
            case RawFormalValidationFailure::OccurrenceMismatch:
              cert.failure = InvocationRewriteFailure::OccurrenceMismatch;
              break;
            case RawFormalValidationFailure::None:
              cert.failure = InvocationRewriteFailure::None;
              break;
            }
            cert.detail = validation.detail;
            return cert;
          }

          // Carry every validated formal spelling into the replacement map,
          // including no-change formals. Paste replay may need unchanged paste
          // participants in order to reconstruct all pasted tokens.
          cert.replacementByArgIdx[argIdx] = newText.str();
          if (!cert.touchesPaste)
            cert.touchesPaste = invocationArgTouchesPaste(inv, argIdx);

          if (oldText == newText)
            continue;

          cert.rewrites.push_back(
              CertifiedFormalRewrite{argIdx, oldText.str(), newText.str()});
        }

        auto logInvocationSupportLedger = [&](StringRef ledgerState) {
          if (!inTraceMode())
            return;
          // Build the support ledger only when trace logging will consume it:
          // provided formals, actually changed formals, carried formals, and
          // paste-required formals that still lack replacement support.
          auto providedArgIdxs = argOrder;
          auto carriedArgIdxs =
              collectSortedUInt32Keys(cert.replacementByArgIdx);

          SmallVector<uint32_t, 8> changedArgIdxs;
          changedArgIdxs.reserve(cert.rewrites.size());
          for (const auto &rewrite : cert.rewrites)
            changedArgIdxs.push_back(rewrite.argIdx);
          llvm::sort(changedArgIdxs);

          auto requiredPasteArgIdxs =
              collectSortedUniquePasteArgIdxs(inv.pasteSpans);
          auto missingSupportArgIdxs =
              computeSortedMissingUInt32s(requiredPasteArgIdxs, carriedArgIdxs);

          trace("macro/proof",
                "{0}: invocation support ledger {1} inv id={2} name={3} "
                "provided={4} changed={5} carried={6} requiredPaste={7} "
                "missingSupport={8} deferredArgs={9} pasteRequired={10} "
                "pasteValid={11} pasteDeferred={12}",
                traceStage, ledgerState, inv.id, inv.name,
                formatUInt32List(providedArgIdxs),
                formatUInt32List(changedArgIdxs),
                formatUInt32List(carriedArgIdxs),
                formatUInt32List(requiredPasteArgIdxs),
                formatUInt32List(missingSupportArgIdxs),
                formatUInt32List(deferOccurrenceArgIdxs),
                cert.pasteValidation.required ? 1 : 0,
                cert.pasteValidation.valid ? 1 : 0,
                cert.pasteValidation.deferred ? 1 : 0);
        };

        logInvocationSupportLedger("enter");

        // If every validated formal collapsed to its original text, the
        // invocation has no rewrite to materialize. Still return a certificate
        // so callers can report the no-change proof path deterministically.
        if (cert.rewrites.empty()) {
          cert.kind = InvocationRewriteCertificateKind::NoChange;
          logInvocationSupportLedger("no-change");
          return cert;
        }

        // Paste validation must run after all formal replacements are known,
        // because a pasted token may depend on several formals and some of them
        // may be unchanged but still required for replay.
        cert.pasteValidation = buildPasteRewriteValidationCertificate(
            inv, cert.replacementByArgIdx, traceStage, callsiteTextOverride,
            callsiteArgRangesOverride);
        cert.touchesPaste = cert.pasteValidation.required;

        logInvocationSupportLedger("replay");

        if (!cert.pasteValidation.valid) {
          switch (cert.pasteValidation.failure) {
          case PasteRewriteValidationFailure::MissingInvocationText:
            cert.failure = InvocationRewriteFailure::MissingInvocationText;
            break;
          case PasteRewriteValidationFailure::MissingArgumentRanges:
            cert.failure = InvocationRewriteFailure::MissingArgumentRanges;
            break;
          case PasteRewriteValidationFailure::PasteMismatch:
            cert.failure = InvocationRewriteFailure::PasteMismatch;
            break;
          case PasteRewriteValidationFailure::None:
            cert.failure = InvocationRewriteFailure::None;
            break;
          }
          cert.detail = cert.pasteValidation.detail;
          return cert;
        }

        cert.kind = InvocationRewriteCertificateKind::Unique;
        return cert;
      };

      // Build an invocation rewrite certificate for a wrapper placeholder-hop.
      // This mostly delegates to the normal invocation certificate path, but
      // also records the rewritten call-site syntax needed by wrapper
      // reconstruction and permits a narrow paste-validation deferral when that
      // rewritten syntax is available.
      auto buildWrapperPlaceholderHopInvocationCertificate =
          [&](const RefoldModel::MacroInvocation &inv,
              const DenseMap<uint32_t, FormalTextPair> &formals,
              StringRef traceStage,
              std::optional<StringRef> callsiteTextOverride = std::nullopt,
              ArrayRef<std::pair<size_t, size_t>> callsiteArgRangesOverride =
                  ArrayRef<std::pair<size_t, size_t>>(),
              ArrayRef<uint32_t> deferOccurrenceArgIdxs =
                  ArrayRef<uint32_t>()) -> InvocationRewriteCertificate {
        auto cert = buildInvocationRewriteCertificate(
            inv, formals, traceStage, callsiteTextOverride,
            callsiteArgRangesOverride, deferOccurrenceArgIdxs);

        DenseMap<uint32_t, std::string> replByFormal;
        for (const auto &kv : formals) {
          StringRef oldText = StringRef(kv.second.oldText).trim();
          StringRef newText = StringRef(kv.second.newText).trim();
          if (oldText == newText)
            continue;
          replByFormal[kv.first] = newText.str();
        }

        // Materialize the rewritten invocation spelling from only the changed
        // formals. Wrapper-chain reconstruction needs this concrete call-site
        // syntax even when the normal proof certificate later needs paste
        // replay deferral.
        if (!replByFormal.empty()) {
          if (auto rewritten =
                  buildRewrittenInvocationSyntax(inv, replByFormal))
            cert.rewrittenInvocationSyntax = std::move(*rewritten);
        }

        // In the ordinary case, return the normal certificate unchanged. The
        // special deferral below applies only to paste mismatch failures where
        // we successfully produced rewritten invocation syntax for this
        // placeholder-hop.
        if (cert.kind != InvocationRewriteCertificateKind::Invalid ||
            cert.failure != InvocationRewriteFailure::PasteMismatch ||
            cert.rewrittenInvocationSyntax.empty())
          return cert;

        // Placeholder-hop rewriting can temporarily break local paste replay
        // because the decisive validation happens after the parent wrapper
        // incorporates the rewritten child syntax. Mark that paste check as
        // deferred rather than rejected, while keeping the deferral explicit in
        // the certificate detail.
        cert.kind = InvocationRewriteCertificateKind::Unique;
        cert.failure = InvocationRewriteFailure::None;
        cert.pasteValidation.valid = true;
        cert.pasteValidation.deferred = true;
        cert.pasteValidation.failure = PasteRewriteValidationFailure::None;
        cert.detail =
            formatv("{0}: wrapper placeholder-hop paste validation "
                    "deferred: inv id={1} name={2} touchedArgs={3}",
                    traceStage, inv.id, inv.name, cert.rewrites.size())
                .str();
        cert.pasteValidation.detail = cert.detail;
        return cert;
      };

      // Derive root-formal old->new rewrites by comparing the original
      // invocation spelling with a candidate rewritten invocation spelling. The
      // new spelling is reparsed into formal argument ranges so replacements
      // are aligned by formal index rather than by raw byte position.
      auto buildRootFormalRewriteMapFromCallsiteReplacement =
          [&](StringRef baseText, StringRef newText)
          -> std::optional<DenseMap<uint32_t, FormalTextPair>> {
        auto newRangesOpt =
            GetMacroInvocationFormalArgContentRanges(m, newText);
        if (!newRangesOpt || newRangesOpt->size() != invArgRanges.size())
          return std::nullopt;

        DenseMap<uint32_t, FormalTextPair> formals;
        for (uint32_t argIdx = 0; argIdx < invArgRanges.size(); ++argIdx) {
          const auto &oldR = invArgRanges[argIdx];
          const auto &newR = (*newRangesOpt)[argIdx];

          // Both old and newly parsed ranges must be valid slices of their
          // respective invocation spellings before they can be compared as
          // formal arguments.
          if (oldR.first > oldR.second || oldR.second > baseText.size() ||
              newR.first > newR.second || newR.second > newText.size())
            return std::nullopt;

          StringRef oldArg =
              baseText.slice((size_t)oldR.first, (size_t)oldR.second).trim();
          StringRef newArg =
              newText.slice((size_t)newR.first, (size_t)newR.second).trim();

          if (oldArg == newArg)
            continue;

          formals[argIdx] = FormalTextPair{oldArg.str(), newArg.str()};
        }

        return formals;
      };

      enum class ParentConstraintDerivationFailure {
        None,
        MissingArgDeps,
        EmptyArgDeps,
        MissingArgRefs,
        TemplateNotCertifiable,
        InversionNotUnique,
        IncompleteDerivation,
      };

      struct ParentConstraintDerivationCertificate {
        bool valid = false;
        uint32_t childFormal = 0;
        ParentConstraintDerivationFailure failure =
            ParentConstraintDerivationFailure::None;
        SmallVector<std::pair<uint32_t, ObservedFormalConstraint>, 4>
            derivedConstraints;
        std::string detail;
      };

      std::function<ParentConstraintDerivationCertificate(
          const RefoldModel::MacroInvocation &, uint32_t, StringRef, StringRef,
          StringRef)>
          buildParentConstraintDerivationCertificate;

      // Attempt to interpret a group of paste-byte ranges in the coordinate
      // space of the current observed pasted surface.
      //
      // Why this is needed:
      //   For nested paste replay, child paste spans may be recorded in one of
      //   two coordinate systems:
      //
      //   (1) Already-local coordinates:
      //       The span byte ranges are already relative to the current
      //       observed surface we are trying to replay. In that case, we can
      //       use them directly.
      //
      //   (2) Enclosing-token coordinates:
      //       The child spans are still expressed relative to the larger
      //       enclosing pasted token owned by `surfaceOwner`. In that case,
      //       we must rebase them into the local observed surface before we
      //       can derive exact-shape replay constraints.
      //
      // This helper first checks whether every span in `group` already fits
      // within `observedSurface`. If so, it returns those ranges unchanged.
      //
      // Otherwise, it looks for the enclosing paste envelope on
      // `surfaceOwner->pasteSpans` that covers the same emitted token
      // `[tokBegin, tokEnd)`. If that enclosing envelope exists and its total
      // width exactly matches `observedSurface`, then each child span is
      // rebased by subtracting the enclosing base offset.
      //
      // The function returns:
      //   - rebased/local byte ranges on success
      //   - std::nullopt if the group cannot be interpreted unambiguously in
      //     the observed-surface coordinate space
      auto tryRebasePasteGroupToObservedSurface =
          [&](const RefoldModel::MacroInvocation *surfaceOwner,
              ArrayRef<const RefoldModel::PPArgSpan *> group,
              StringRef observedSurface, StringRef traceStage)
          -> std::optional<SmallVector<std::pair<uint64_t, uint64_t>, 4>> {
        SmallVector<std::pair<uint64_t, uint64_t>, 4> rebased;
        rebased.reserve(group.size());
        const uint64_t observedLen = observedSurface.size();

        // Fast path:
        // If every span already has a valid byte range fully inside the
        // current observed surface, then the group is already expressed in the
        // local coordinate space and does not need rebasing.
        bool fitsObservedSurface = true;
        for (const auto *sp : group) {
          if (!sp->byteBegin || !sp->byteEnd || *sp->byteBegin > *sp->byteEnd ||
              *sp->byteEnd > observedLen) {
            fitsObservedSurface = false;
            break;
          }
        }
        if (fitsObservedSurface) {
          for (const auto *sp : group)
            rebased.push_back({*sp->byteBegin, *sp->byteEnd});
          return rebased;
        }

        // If the spans do not already fit the observed surface, we can only
        // recover them if we know which enclosing invocation owns the larger
        // pasted token that these spans were originally measured against.
        if (!surfaceOwner)
          return std::nullopt;

        std::optional<uint64_t> base;
        std::optional<uint64_t> limit;
        const uint64_t tokBegin = group.front()->begin;
        const uint64_t tokEnd = group.front()->end;

        // Find the enclosing paste envelope on the surface owner for the same
        // emitted token `[tokBegin, tokEnd)`.
        //
        // Multiple owner spans may contribute to that token, so we compute the
        // minimal base and maximal limit across all matching owner paste spans.
        // The resulting [base, limit) interval is the full owner-local byte
        // range for the observed pasted surface.
        for (const auto &ownerSp : surfaceOwner->pasteSpans) {
          if (!ownerSp.byteBegin || !ownerSp.byteEnd)
            continue;
          if (ownerSp.begin != tokBegin || ownerSp.end != tokEnd)
            continue;
          base = base ? std::min<uint64_t>(*base, *ownerSp.byteBegin)
                      : *ownerSp.byteBegin;
          limit = limit ? std::max<uint64_t>(*limit, *ownerSp.byteEnd)
                        : *ownerSp.byteEnd;
        }

        // The enclosing owner envelope must:
        //   - exist
        //   - be well-formed
        //   - have width exactly equal to the current observed surface
        //
        // If not, we cannot safely interpret the child spans relative to the
        // local replay surface.
        if (!base || !limit || *limit < *base ||
            (*limit - *base) != observedLen)
          return std::nullopt;

        // Rebase each child span from owner-local/full-token coordinates into
        // observed-surface-local coordinates by subtracting the enclosing base.
        //
        // Each span must lie fully inside the enclosing owner envelope;
        // otherwise the replay would be inconsistent and must be rejected.
        for (const auto *sp : group) {
          if (!sp->byteBegin || !sp->byteEnd)
            return std::nullopt;
          if (*sp->byteBegin < *base || *sp->byteEnd < *sp->byteBegin ||
              *sp->byteEnd > *limit)
            return std::nullopt;
          rebased.push_back({*sp->byteBegin - *base, *sp->byteEnd - *base});
        }

        return rebased;
      };

      // Lift a nested paste-token rewrite back into constraints on the parent
      // invocation. This handles the shape where `curFormal` is itself the raw
      // spelling of a nested child invocation, and `curOld`/`curNew` are the
      // old/new pasted-token surfaces produced by that nested child.
      auto tryBuildNestedPasteChainDerivation =
          [&](const RefoldModel::MacroInvocation &cur, uint32_t curFormal,
              StringRef curOld, StringRef curNew, StringRef traceStage)
          -> std::optional<ParentConstraintDerivationCertificate> {
        auto argText = getInvocationArgText(cur, curFormal);
        if (!argText || argText->empty())
          return std::nullopt;

        ArrayRef<const RefoldModel::MacroInvocation *> children =
            (*deps_.macroTopology).MacroChildrenOf(cur.id);
        if (children.empty())
          return std::nullopt;

        // The current formal must name exactly one nested child invocation by
        // raw invocation spelling. Multiple exact matches would make the
        // paste-chain owner ambiguous.
        const RefoldModel::MacroInvocation *nested = nullptr;
        SmallVector<uint32_t, 4> nestedMatches;
        for (const auto *cand : children) {
          if (!cand || !cand->invText)
            continue;
          if (StringRef(*cand->invText).trim() != argText->trim())
            continue;
          nestedMatches.push_back(cand->id);
          if (nested) {
            REFOLD_LOG_TRACE("macro/dag",
                             "{0}: nested pasted-chain derivation ambiguous "
                             "nested child matches parent child id={1} name={2} "
                             "argIdx={3} matches={4}",
                             traceStage, cur.id, cur.name, curFormal,
                             formatUInt32List(nestedMatches));
            return std::nullopt;
          }
          nested = cand;
        }
        if (!nested) {
          return std::nullopt;
        }

        // The observed old token must correspond to exactly one old expansion
        // surface of the nested invocation. Otherwise the old side does not
        // identify a unique nested paste-token witness.
        SmallVector<std::string, 4> oldExpansionCandidates =
            expansionTextCandidates(*nested, /*fromB=*/false);
        SmallVector<std::string, 4> matchingOldCandidates;
        for (const auto &cand : oldExpansionCandidates) {
          if (StringRef(cand).trim() == curOld.trim())
            matchingOldCandidates.push_back(cand);
        }
        if (matchingOldCandidates.size() != 1)
          return std::nullopt;

        // Group paste spans by the PP token they produced. This derivation is
        // for one pasted token assembled from multiple formal contributions, so
        // require exactly one group below.
        DenseMap<uint64_t, SmallVector<const RefoldModel::PPArgSpan *, 4>>
            pasteGroups;
        for (const auto &sp : nested->pasteSpans) {
          if (!sp.byteBegin || !sp.byteEnd)
            return std::nullopt;
          const uint64_t key = (uint64_t(sp.begin) << 32) | uint64_t(sp.end);
          pasteGroups[key].push_back(&sp);
        }
        if (pasteGroups.size() != 1)
          return std::nullopt;

        auto &group = pasteGroups.begin()->second;
        if (group.size() < 2)
          return std::nullopt;

        llvm::sort(group, pasteSpanPtrLessByByteRange);

        StringRef oldTok = curOld.trim();
        StringRef newTok = curNew.trim();
        if (oldTok.empty() || newTok.empty())
          return std::nullopt;

        // Rebase producer paste byte ranges onto the observed old token
        // surface. The nested child may have been observed through a wrapper,
        // so the producer-local byte windows must be aligned to `oldTok` before
        // using them to split `newTok`.
        auto rebasedGroup = tryRebasePasteGroupToObservedSurface(
            &cur, ArrayRef<const RefoldModel::PPArgSpan *>(group), oldTok,
            traceStage);
        if (!rebasedGroup)
          return std::nullopt;
        for (size_t i = 1; i < rebasedGroup->size(); ++i) {
          if ((*rebasedGroup)[i - 1].second > (*rebasedGroup)[i].first)
            return std::nullopt;
        }

        // Text outside the first/last paste segments is stable boundary text.
        // The new pasted token must preserve it before we try to split the
        // changed core.
        StringRef leading = oldTok.take_front((*rebasedGroup).front().first);
        StringRef trailing = oldTok.drop_front((*rebasedGroup).back().second);
        if (!newTok.starts_with(leading) || !newTok.ends_with(trailing))
          return std::nullopt;

        SmallVector<StringRef, 4> oldSegs;
        SmallVector<StringRef, 4> midBodies;
        oldSegs.reserve(group.size());
        midBodies.reserve(group.size() - 1);
        for (size_t i = 0; i < group.size(); ++i) {
          const auto [segBegin, segEnd] = (*rebasedGroup)[i];
          oldSegs.push_back(oldTok.slice(segBegin, segEnd));
          if (i + 1 < group.size()) {
            StringRef mid = oldTok.slice(segEnd, (*rebasedGroup)[i + 1].first);
            if (mid.empty())
              return std::nullopt;
            midBodies.push_back(mid);
          }
        }

        StringRef core =
            newTok.slice(leading.size(), newTok.size() - trailing.size());

        auto suffixDelimiterNeed = [&](size_t delimIdx) -> uint64_t {
          const StringRef delim = midBodies[delimIdx];
          uint64_t need = 0;
          for (size_t segIdx = delimIdx + 1; segIdx < oldSegs.size(); ++segIdx)
            need += countSubstringOccurrences(oldSegs[segIdx], delim);
          for (size_t later = delimIdx + 1; later < midBodies.size(); ++later)
            if (midBodies[later] == delim)
              ++need;
          return need;
        };

        SmallVector<StringRef, 4> curSegs;
        SmallVector<SmallVector<StringRef, 4>, 2> splitSolutions;
        auto addSplitSolution = [&](const SmallVectorImpl<StringRef> &parts) {
          SmallVector<StringRef, 4> copy(parts.begin(), parts.end());
          for (const auto &existing : splitSolutions)
            if (existing == copy)
              return;
          splitSolutions.push_back(std::move(copy));
        };

        auto splitCore = [&](auto &&self, size_t delimIdx,
                             StringRef rest) -> void {
          if (splitSolutions.size() > 1)
            return;
          if (delimIdx == midBodies.size()) {
            curSegs.push_back(rest);
            addSplitSolution(curSegs);
            curSegs.pop_back();
            return;
          }

          const StringRef delim = midBodies[delimIdx];
          const uint64_t needLeft = countSubstringOccurrences(oldSegs[delimIdx], delim);
          const uint64_t needRight = suffixDelimiterNeed(delimIdx);

          // Try each occurrence of the old delimiter as the next split point.
          // The occurrence-count guards keep delimiter text that originally
          // belonged inside neighboring segments from being consumed as a
          // split.
          for (size_t pos = 0; (pos = rest.find(delim, pos)) != StringRef::npos;
               ++pos) {
            StringRef left = rest.slice(0, pos);
            StringRef tail = rest.drop_front(pos + delim.size());
            if (countSubstringOccurrences(left, delim) < needLeft)
              continue;
            if (countSubstringOccurrences(tail, delim) < needRight)
              continue;
            curSegs.push_back(left);
            self(self, delimIdx + 1, tail);
            curSegs.pop_back();
          }
        };
        splitCore(splitCore, 0, core);

        // Accept only a unique split with one new segment for each old paste
        // contribution. Anything else is ambiguous or structurally incomplete.
        if (splitSolutions.size() != 1 ||
            splitSolutions[0].size() != group.size())
          return std::nullopt;

        ParentConstraintDerivationCertificate cert;
        cert.childFormal = curFormal;
        DenseMap<uint32_t, ObservedFormalConstraint> mergedByParentFormal;

        // Recursively lift each nested paste operand rewrite into constraints
        // on the parent formal(s). If two nested operands derive different
        // constraints for the same parent formal, fail closed.
        for (size_t i = 0; i < group.size(); ++i) {
          const uint32_t nestedFormal = group[i]->argIdx;
          auto nestedCert = buildParentConstraintDerivationCertificate(
              *nested, nestedFormal, oldSegs[i], splitSolutions[0][i],
              traceStage);
          if (!nestedCert.valid)
            return std::nullopt;
          for (const auto &derived : nestedCert.derivedConstraints) {
            auto itExisting = mergedByParentFormal.find(derived.first);
            if (itExisting == mergedByParentFormal.end()) {
              mergedByParentFormal.insert({derived.first, derived.second});
              continue;
            }
            if (itExisting->second.oldText != derived.second.oldText ||
                itExisting->second.newText != derived.second.newText)
              return std::nullopt;
          }
        }

        for (const auto &kv : mergedByParentFormal)
          cert.derivedConstraints.push_back({kv.first, kv.second});
        llvm::sort(cert.derivedConstraints, [](const auto &a, const auto &b) {
          return a.first < b.first;
        });
        if (cert.derivedConstraints.empty())
          return std::nullopt;
        cert.valid = true;
        return cert;
      };

      // Lift a child formal rewrite back to two parent formals when the old
      // child surface has the stable shape `oldA + mid + oldB`. The delimiter
      // `mid` must split the new child surface uniquely, producing one derived
      // rewrite for each parent formal.
      auto tryBuildTwoParentDelimitedDerivation =
          [&](const RefoldModel::MacroInvocation &cur, uint32_t curFormal,
              StringRef curOld, StringRef curNew, StringRef traceStage)
          -> std::optional<ParentConstraintDerivationCertificate> {
        if (!cur.callerMacroId || curFormal >= cur.argDeps.size())
          return std::nullopt;

        auto parentIt = subtreeValidationCtx.invocationById.find(*cur.callerMacroId);
        if (parentIt == subtreeValidationCtx.invocationById.end())
          return std::nullopt;
        const RefoldModel::MacroInvocation *parent = parentIt->second;
        ArrayRef<uint32_t> deps = cur.argDeps[curFormal];

        // This derivation is intentionally limited to a binary dependency:
        // one child formal assembled from exactly two parent formals.
        if (deps.size() != 2)
          return std::nullopt;

        auto oldAOpt = getInvocationArgText(*parent, deps[0]);
        auto oldBOpt = getInvocationArgText(*parent, deps[1]);
        if (!oldAOpt || !oldBOpt)
          return std::nullopt;

        const StringRef oldTok = curOld.trim();
        const StringRef newTok = curNew.trim();
        const StringRef oldA = oldAOpt->trim();
        const StringRef oldB = oldBOpt->trim();
        if (oldTok.empty() || newTok.empty() || oldA.empty() || oldB.empty())
          return std::nullopt;

        // Prove that the old child surface is exactly oldA + mid + oldB. If
        // the two parent texts are not anchored at the edges, there is no stable
        // middle delimiter to reuse when splitting the new child surface.
        if (!oldTok.starts_with(oldA) || !oldTok.ends_with(oldB) ||
            oldTok.size() < oldA.size() + oldB.size()) {
          return std::nullopt;
        }

        StringRef mid = oldTok.slice(oldA.size(), oldTok.size() - oldB.size());
        if (mid.empty())
          return std::nullopt;

        // If oldA or oldB themselves contain the delimiter, a valid split of the
        // new text must leave those delimiter occurrences on the corresponding
        // side. Otherwise the split would steal text that belongs inside a
        // parent argument.
        const uint64_t needA = countSubstringOccurrences(oldA, mid);
        const uint64_t needB = countSubstringOccurrences(oldB, mid);

        SmallVector<std::pair<StringRef, StringRef>, 4> splits;
        for (size_t pos = 0; (pos = newTok.find(mid, pos)) != StringRef::npos;
             ++pos) {
          StringRef newA = newTok.slice(0, pos);
          StringRef newB = newTok.drop_front(pos + mid.size());
          if (countSubstringOccurrences(newA, mid) < needA || countSubstringOccurrences(newB, mid) < needB)
            continue;
          splits.push_back({newA, newB});
        }

        // The delimiter must determine exactly one newA/newB split. No split
        // means the shape was not preserved; multiple splits are ambiguous.
        if (splits.size() != 1)
          return std::nullopt;

        ParentConstraintDerivationCertificate cert;
        cert.childFormal = curFormal;
        cert.valid = true;

        // Lift the unique child split into one observed rewrite constraint for
        // each parent formal.
        cert.derivedConstraints.push_back(
            {deps[0], ObservedFormalConstraint{oldA.str(),
                                               splits[0].first.trim().str()}});
        cert.derivedConstraints.push_back(
            {deps[1], ObservedFormalConstraint{oldB.str(),
                                               splits[0].second.trim().str()}});
        cert.detail = formatv("{0}: child id={1} name={2} argIdx={3} accepted "
                              "via two-parent delimited derivation",
                              traceStage, cur.id, cur.name, curFormal)
                          .str();
        return cert;
      };

      // Derive parent-formal old/new constraints from one observed rewrite of a
      // child formal. The preferred path uses arg-ref metadata to invert the
      // child formal back into caller-parameter pieces; specialized derivations
      // handle narrow paste/delimiter cases before failing over to a lexical
      // bridge requirement.
      buildParentConstraintDerivationCertificate =
          [&](const RefoldModel::MacroInvocation &cur, uint32_t curFormal,
              StringRef curOld, StringRef curNew,
              StringRef traceStage) -> ParentConstraintDerivationCertificate {
        ParentConstraintDerivationCertificate cert;
        cert.childFormal = curFormal;


        // `argDeps` says which parent formals flow into this child formal. If
        // the dependency metadata is missing or empty, arg-ref inversion cannot
        // prove the parent constraints.
        if (curFormal >= cur.argDeps.size()) {
          cert.failure = ParentConstraintDerivationFailure::MissingArgDeps;
          cert.detail = formatv("{0}: child id={1} name={2} argIdx={3} missing "
                                "argDeps entry; lexical bridge required",
                                traceStage, cur.id, cur.name, curFormal)
                            .str();
          return cert;
        }
        ArrayRef<uint32_t> deps = cur.argDeps[curFormal];
        if (deps.empty()) {
          cert.failure = ParentConstraintDerivationFailure::EmptyArgDeps;
          cert.detail = formatv("{0}: child id={1} name={2} argIdx={3} has "
                                "empty argDeps; lexical bridge required",
                                traceStage, cur.id, cur.name, curFormal)
                            .str();
          return cert;
        }

        // `argRefs` gives byte-level placeholders inside the child argument.
        // Without it, we know a dependency exists but cannot split the observed
        // child old/new text back into parent-formal slices.
        if (curFormal >= cur.argRefs.size()) {
          cert.failure = ParentConstraintDerivationFailure::MissingArgRefs;
          cert.detail = formatv("{0}: child id={1} name={2} argIdx={3} missing "
                                "argRefs entry; lexical bridge required",
                                traceStage, cur.id, cur.name, curFormal)
                            .str();
          return cert;
        }

        auto tpl = buildArgRefTemplate(cur, curFormal);
        if (!tpl || tpl->refs.empty() ||
            !sameIndexSet(deps, tpl->distinctCallerParams)) {
          // If the generic arg-ref template is unavailable, try the two narrow
          // structural derivations that can still prove parent constraints
          // without a normal placeholder template.
          if (auto twoParentCert = tryBuildTwoParentDelimitedDerivation(
                  cur, curFormal, curOld, curNew, traceStage)) {
            return *twoParentCert;
          }
          if (auto nestedCert = tryBuildNestedPasteChainDerivation(
                  cur, curFormal, curOld, curNew, traceStage)) {
            return *nestedCert;
          }
          cert.failure =
              ParentConstraintDerivationFailure::TemplateNotCertifiable;
          cert.detail = formatv("{0}: child id={1} name={2} argIdx={3} arg-ref "
                                "template not certifiable; lexical bridge "
                                "required",
                                traceStage, cur.id, cur.name, curFormal)
                            .str();
          return cert;
        }

        // Invert both the old and new observed child text through the same
        // template. Both sides must produce a unique assignment from parent
        // formal -> observed slice, or the lifted parent rewrite is ambiguous.
        auto oldCert = buildArgRefInvertibilityCertificate(*tpl, curOld);
        auto newCert = buildArgRefInvertibilityCertificate(*tpl, curNew);
        if (oldCert.kind != ArgRefInvertibilityKind::Unique ||
            newCert.kind != ArgRefInvertibilityKind::Unique) {
          if (auto twoParentCert = tryBuildTwoParentDelimitedDerivation(
                  cur, curFormal, curOld, curNew, traceStage)) {
            return *twoParentCert;
          }
          if (auto nestedCert = tryBuildNestedPasteChainDerivation(
                  cur, curFormal, curOld, curNew, traceStage)) {
            return *nestedCert;
          }
          cert.failure = ParentConstraintDerivationFailure::InversionNotUnique;
          cert.detail = formatv("{0}: child id={1} name={2} argIdx={3} arg-ref "
                                "inversion not unique; lexical bridge required",
                                traceStage, cur.id, cur.name, curFormal)
                            .str();
          return cert;
        }

        // Pair the old and new assignments for every parent formal referenced
        // by the template. Missing either side means the child observation did
        // not derive a complete parent-level constraint.
        for (uint32_t parentFormal : tpl->distinctCallerParams) {
          auto oldIt = oldCert.derivedTextByCallerParam.find(parentFormal);
          auto newIt = newCert.derivedTextByCallerParam.find(parentFormal);
          if (oldIt == oldCert.derivedTextByCallerParam.end() ||
              newIt == newCert.derivedTextByCallerParam.end()) {
            cert.failure =
                ParentConstraintDerivationFailure::IncompleteDerivation;
            cert.detail =
                formatv("{0}: child id={1} name={2} argIdx={3} "
                        "parent formal derivation incomplete; lexical "
                        "bridge required",
                        traceStage, cur.id, cur.name, curFormal)
                    .str();
            cert.derivedConstraints.clear();
            return cert;
          }
          cert.derivedConstraints.push_back(
              {parentFormal,
               ObservedFormalConstraint{oldIt->second, newIt->second}});
        }

        cert.valid = true;
        return cert;
      };

      enum class StructuredLiftCertificateKind {
        Unique,
        NeedsLexicalBridge,
        Invalid,
      };

      enum class StructuredLiftFailureReason {
        None,
        CurrentInvocationInvalid,
        MissingCallerInvocation,
        RootLexicalBridgeRequired,
        ParentConstraintDerivationFailed,
        ParentFormalInvalid,
        ParentInvocationInvalid,
      };

      struct StructuredLiftCertificate {
        StructuredLiftCertificateKind kind =
            StructuredLiftCertificateKind::Invalid;
        StructuredLiftFailureReason failureReason =
            StructuredLiftFailureReason::None;
        ParentConstraintDerivationFailure derivationFailure =
            ParentConstraintDerivationFailure::None;
        FormalRewriteFailure parentFormalFailure = FormalRewriteFailure::None;
        InvocationRewriteFailure currentInvocationFailure =
            InvocationRewriteFailure::None;
        InvocationRewriteFailure parentInvocationFailure =
            InvocationRewriteFailure::None;
        const RefoldModel::MacroInvocation *nextInv = nullptr;
        DenseMap<uint32_t, FormalTextPair> nextFormals;
        DenseMap<uint32_t, SmallVector<uint32_t, 2>> parentFormalSources;
        DenseSet<uint32_t> bridgedNextFormals;
        InvocationRewriteCertificate currentCert;
        SmallVector<ParentConstraintDerivationCertificate, 4> derivations;
        SmallVector<FormalRewriteCertificate, 4> parentFormalCertificates;
        InvocationRewriteCertificate parentCert;
        std::string rewrittenChildSyntax;
        std::string detail;
      };

      std::function<StructuredLiftCertificate(
          const RefoldModel::MacroInvocation &,
          const DenseMap<uint32_t, FormalTextPair> &)>
          buildStructuredLiftCertificate;

      // Re-root a child-formal rewrite through an exact sibling invocation
      // spelled in the same parent. This handles cases where the current child
      // observed `old -> new`, but the parent-level rewrite is better proven by
      // applying that `new` text to a sibling call whose raw invocation
      // spelling exactly matched the old child surface.
      auto tryBuildExactSiblingRerootLift =
          [&](const RefoldModel::MacroInvocation &parent,
              const RefoldModel::MacroInvocation &cur, uint32_t curFormal,
              StringRef curOld,
              StringRef curNew) -> std::optional<StructuredLiftCertificate> {
        const StringRef oldTrim = curOld.trim();
        const StringRef newTrim = curNew.trim();
        if (oldTrim.empty() || newTrim.empty())
          return std::nullopt;

        const RefoldModel::MacroInvocation *matchedSibling = nullptr;

        // Find the unique sibling invocation under the same parent whose raw
        // invocation spelling exactly matches the old observed child surface.
        // Multiple siblings with the same spelling would make the reroot target
        // ambiguous.
        for (const auto &cand : (*deps_.model).GetMacroInvocations()) {
          if (cand.id == cur.id || !cand.callerMacroId ||
              *cand.callerMacroId != parent.id || !cand.invText)
            continue;
          if (StringRef(*cand.invText).trim() != oldTrim)
            continue;
          if (matchedSibling) {
            return std::nullopt;
          }
          matchedSibling = &cand;
        }

        if (!matchedSibling || matchedSibling->invArgRanges.empty()) {
          return std::nullopt;
        }

        std::optional<StructuredLiftCertificate> uniqueLift;
        std::optional<uint32_t> uniqueSiblingFormal;

        auto sameNextFormals =
            [&](const DenseMap<uint32_t, FormalTextPair> &lhs,
                const DenseMap<uint32_t, FormalTextPair> &rhs) {
              if (lhs.size() != rhs.size())
                return false;
              for (const auto &kvLocal : lhs) {
                auto it = rhs.find(kvLocal.first);
                if (it == rhs.end())
                  return false;
                if (it->second.oldText != kvLocal.second.oldText ||
                    it->second.newText != kvLocal.second.newText)
                  return false;
              }
              return true;
            };

        struct ConcreteExemplarReplayLiftResult {
          enum class State {
            None,
            Unique,
            Ambiguous,
          };

          State state = State::None;
          std::optional<StructuredLiftCertificate> lift;
        };

        auto tryBuildConcreteExemplarReplayLift =
            [&](uint32_t siblingFormal,
                StringRef siblingOldTrim) -> ConcreteExemplarReplayLiftResult {
          SmallVector<std::string, 4> parentActuals;
          for (uint32_t parentFormal = 0;
               parentFormal < parent.invArgRanges.size(); ++parentFormal) {
            if (auto parentArg = getInvocationArgText(parent, parentFormal)) {
              StringRef parentArgTrim = parentArg->trim();
              if (!parentArgTrim.empty() &&
                  !llvm::is_contained(parentActuals, parentArgTrim.str()))
                parentActuals.push_back(parentArgTrim.str());
            }
          }

          ConcreteExemplarReplayLiftResult result;
          for (const auto &exemplar : (*deps_.model).GetMacroInvocations()) {
            if (exemplar.id == matchedSibling->id ||
                exemplar.name != matchedSibling->name ||
                siblingFormal >= exemplar.invArgRanges.size())
              continue;

            auto exemplarOldArg = getInvocationArgText(exemplar, siblingFormal);
            if (!exemplarOldArg)
              continue;
            StringRef exemplarOldTrim = exemplarOldArg->trim();
            if (exemplarOldTrim.empty() ||
                !llvm::is_contained(parentActuals, exemplarOldTrim.str()))
              continue;

            // Use another invocation of the same sibling macro as a concrete
            // exemplar. If replacing the exemplar's old formal with the new
            // observed surface derives exactly one concrete new formal value,
            // replay that value through the matched sibling.
            SmallVector<std::string, 4> projectedConcreteNews;
            for (const auto &oldExpStr :
                 expansionTextCandidates(exemplar, /*fromB=*/false)) {
              StringRef projected =
                  DeriveNewPasteSegmentFromSpellingReplacement(
                      StringRef(oldExpStr).trim(), newTrim, exemplarOldTrim);
              projected = projected.trim();
              if (projected.empty() || projected == exemplarOldTrim)
                continue;
              if (!llvm::is_contained(projectedConcreteNews, projected.str()))
                projectedConcreteNews.push_back(projected.str());
            }
            if (projectedConcreteNews.size() != 1)
              continue;

            const std::string &projectedConcreteNew =
                projectedConcreteNews.front();

            DenseMap<uint32_t, FormalTextPair> exemplarFormals;
            exemplarFormals[siblingFormal] =
                FormalTextPair{exemplarOldTrim.str(), projectedConcreteNew};
            auto exemplarInvCert =
                buildWrapperPlaceholderHopInvocationCertificate(
                    exemplar, exemplarFormals,
                    "DAG per-hop exact sibling reroot concrete exemplar");
            if (exemplarInvCert.kind ==
                InvocationRewriteCertificateKind::Invalid)
              continue;

            DenseMap<uint32_t, FormalTextPair> replayFormals;
            replayFormals[siblingFormal] =
                FormalTextPair{siblingOldTrim.str(), projectedConcreteNew};
            auto replayInvCert =
                buildWrapperPlaceholderHopInvocationCertificate(
                    *matchedSibling, replayFormals,
                    "DAG per-hop exact sibling reroot concrete replay");
            if (replayInvCert.kind == InvocationRewriteCertificateKind::Invalid)
              continue;

            auto projectedLift =
                buildStructuredLiftCertificate(*matchedSibling, replayFormals);
            if (projectedLift.kind != StructuredLiftCertificateKind::Unique ||
                projectedLift.nextInv != &parent)
              continue;

            // Multiple exemplars are acceptable only if they produce the same
            // parent-formal rewrite. Divergent projections make the reroot
            // lift ambiguous.
            if (result.lift) {
              if (!sameNextFormals(result.lift->nextFormals,
                                   projectedLift.nextFormals)) {
                result.state =
                    ConcreteExemplarReplayLiftResult::State::Ambiguous;
                result.lift.reset();
                return result;
              }
              result.state = ConcreteExemplarReplayLiftResult::State::Unique;
              continue;
            }

            result.state = ConcreteExemplarReplayLiftResult::State::Unique;
            result.lift = std::move(projectedLift);
          }

          return result;
        };

        for (uint32_t siblingFormal = 0;
             siblingFormal < matchedSibling->invArgRanges.size();
             ++siblingFormal) {
          auto siblingOldArg =
              getInvocationArgText(*matchedSibling, siblingFormal);
          if (!siblingOldArg) {
            continue;
          }

          const StringRef siblingOldTrim = siblingOldArg->trim();
          if (siblingOldTrim.empty() || siblingOldTrim == newTrim) {
            continue;
          }

          DenseMap<uint32_t, FormalTextPair> siblingFormals;
          siblingFormals[siblingFormal] =
              FormalTextPair{siblingOldTrim.str(), newTrim.str()};
          auto siblingLift =
              buildStructuredLiftCertificate(*matchedSibling, siblingFormals);
          if (siblingLift.kind != StructuredLiftCertificateKind::Unique ||
              siblingLift.nextInv != &parent)
            continue;

          auto siblingLiftHasCertifiedParentFormalEvidence = [&]() {
            for (const auto &derived : siblingLift.nextFormals) {
              const uint32_t parentFormal = derived.first;
              bool certified = false;
              for (const auto &formalCert :
                   siblingLift.parentFormalCertificates) {
                if (formalCert.argIdx != parentFormal)
                  continue;
                if (formalCert.kind != FormalRewriteCertificateKind::Invalid) {
                  certified = true;
                  break;
                }
              }
              if (!certified)
                return false;
            }
            return true;
          };

          if (!siblingLiftHasCertifiedParentFormalEvidence()) {
            auto replayResult = tryBuildConcreteExemplarReplayLift(
                siblingFormal, siblingOldTrim);
            if (replayResult.state ==
                    ConcreteExemplarReplayLiftResult::State::Unique &&
                replayResult.lift) {
              siblingLift = std::move(*replayResult.lift);
            } else if (replayResult.state ==
                           ConcreteExemplarReplayLiftResult::State::None &&
                       siblingLift.parentCert.kind !=
                           InvocationRewriteCertificateKind::Invalid &&
                       siblingLift.parentInvocationFailure ==
                           InvocationRewriteFailure::None) {
              // The direct sibling lift already has a valid parent invocation
              // certificate, and no concrete exemplar contradicted it. Keep
              // the direct lift even though it lacks per-formal evidence.
            } else {
              continue;
            }
          }

          // Accept exactly one sibling formal as the reroot seed. If two
          // sibling formals both lift to the parent, the old->new relationship
          // is ambiguous.
          if (uniqueLift) {
            return std::nullopt;
          }

          uniqueSiblingFormal = siblingFormal;
          uniqueLift = std::move(siblingLift);
        }

        if (!uniqueLift || !uniqueSiblingFormal)
          return std::nullopt;

        return std::move(*uniqueLift);
      };

      /// Build one proof step that lifts a certified rewrite from `cur` to its
      /// caller in the macro-expansion DAG.
      ///
      /// The input `curFormals` describes the rewrite that has already been
      /// proven at the current invocation boundary. This lambda tries to invert
      /// that rewrite through `cur`'s formal dependencies, derive the
      /// equivalent constraints on the parent invocation's formals, and then
      /// prove that the parent invocation can be rebuilt with those rewritten
      /// formals while preserving the parent's placeholder structure.
      ///
      /// The result is intentionally fail-closed:
      ///
      /// * `Unique` means the hop produced one certified parent rewrite.
      /// * `NeedsLexicalBridge` means the structured DAG lift could not be
      ///   proven at this boundary, so the caller must fall back to a lexical
      ///   bridge at `nextInv`.
      /// * `Invalid`/failure fields record the exact proof obligation that
      ///   failed.
      buildStructuredLiftCertificate =
          [&](const RefoldModel::MacroInvocation &cur,
              const DenseMap<uint32_t, FormalTextPair> &curFormals)
          -> StructuredLiftCertificate {
        StructuredLiftCertificate cert;

        // Parent-formal observations are formatted deterministically for trace
        // output because DenseMap iteration order is unstable. A parent formal
        // may accumulate constraints from multiple child formals before the
        // certificate checks whether those observations are mutually consistent.
        //
        // First prove that the current invocation itself can be reconstructed
        // from the already-derived formal rewrites. If this fails, there is no
        // structured child syntax to lift through the parent.
        auto curCert = buildWrapperPlaceholderHopInvocationCertificate(
            cur, curFormals, "DAG per-hop");
        cert.currentCert = curCert;
        if (curCert.kind == InvocationRewriteCertificateKind::Invalid) {
          cert.failureReason =
              StructuredLiftFailureReason::CurrentInvocationInvalid;
          cert.currentInvocationFailure = curCert.failure;
          cert.detail = curCert.detail;
          return cert;
        }

        // Materialize the rewritten child invocation syntax for two purposes:
        // logging and, when needed, as preferred syntax for a parent formal
        // that contains this child invocation as a lexical placeholder.
        DenseMap<uint32_t, std::string> curFormalSyntax;
        for (const auto &kvLocal : curFormals)
          curFormalSyntax[kvLocal.first] = kvLocal.second.newText;
        if (!curCert.rewrittenInvocationSyntax.empty()) {
          cert.rewrittenChildSyntax = curCert.rewrittenInvocationSyntax;
        } else if (auto curSyntax =
                       buildRewrittenInvocationSyntax(cur, curFormalSyntax)) {
          cert.rewrittenChildSyntax = std::move(*curSyntax);
        }

        // A structured lift step normally moves from a child invocation to its
        // caller. If the child has no caller metadata, the only sound next step
        // is to ask the outer algorithm to bridge lexically back to the root.
        const RefoldModel::MacroInvocation *parent = nullptr;
        if (cur.callerMacroId) {
          auto parentIt = subtreeValidationCtx.invocationById.find(*cur.callerMacroId);
          if (parentIt == subtreeValidationCtx.invocationById.end()) {
            cert.failureReason =
                StructuredLiftFailureReason::MissingCallerInvocation;
            cert.detail =
                formatv("DAG per-hop: missing caller invocation: child id={0} "
                        "name={1} callerId={2}",
                        cur.id, cur.name, *cur.callerMacroId)
                    .str();
            return cert;
          }
          parent = parentIt->second;
        } else {
          cert.kind = StructuredLiftCertificateKind::NeedsLexicalBridge;
          cert.failureReason =
              StructuredLiftFailureReason::RootLexicalBridgeRequired;
          cert.nextInv = &m;
          cert.detail = formatv("DAG per-hop: child id={0} name={1} requires "
                                "lexical bridge to root",
                                cur.id, cur.name)
                            .str();
          return cert;
        }

        /// Try to explain an observed rewrite of a pasted surface by replaying
        /// exactly one direct paste-producing child invocation.
        ///
        /// This helper is intentionally narrow. It only succeeds when:
        ///
        /// * the child has exactly one paste product at this hop,
        /// * the pasted operands can be rebased onto the observed surface,
        /// * the rewritten surface has a unique split around the original
        ///   inter-operand delimiters, and
        /// * every recovered operand rewrite can be lifted through the child's
        ///   normal formal-derivation certificate.
        ///
        /// Any ambiguity is rejected because it would amount to inventing an
        /// inverse paste decomposition rather than proving one.
        auto tryDeriveObservedConstraintsFromDirectPasteChild =
            [&](const RefoldModel::MacroInvocation &surfaceOwner,
                const RefoldModel::MacroInvocation &directChild,
                StringRef observedOld0, StringRef observedNew0,
                StringRef traceStage)
            -> std::optional<
                SmallVector<std::pair<uint32_t, ObservedFormalConstraint>, 4>> {
          StringRef observedOld = observedOld0.trim();
          StringRef observedNew = observedNew0.trim();
          if (observedOld.empty() || observedNew.empty())
            return std::nullopt;

          // Group spans by their common pasted result. Exact-shape replay only
          // handles a single pasted product at this hop; multiple independent
          // paste groups would require choosing between distinct replay
          // regions.
          DenseMap<uint64_t, SmallVector<const RefoldModel::PPArgSpan *, 4>>
              pasteGroups;
          for (const auto &sp : directChild.pasteSpans) {
            if (!sp.byteBegin || !sp.byteEnd)
              return std::nullopt;
            const uint64_t key = (uint64_t(sp.begin) << 32) | uint64_t(sp.end);
            pasteGroups[key].push_back(&sp);
          }
          if (pasteGroups.size() != 1)
            return std::nullopt;

          auto &group = pasteGroups.begin()->second;
          if (group.size() < 2)
            return std::nullopt;

          llvm::sort(group, pasteSpanPtrLessByByteRange);

          // Rebase the child's paste operand byte ranges onto the observed
          // surface. This proves that the pasted product being edited is the
          // same concrete surface produced by this direct child.
          auto rebasedGroup = tryRebasePasteGroupToObservedSurface(
              &surfaceOwner, ArrayRef<const RefoldModel::PPArgSpan *>(group),
              observedOld, traceStage);
          if (!rebasedGroup)
            return std::nullopt;
          for (size_t i = 1; i < rebasedGroup->size(); ++i) {
            if ((*rebasedGroup)[i - 1].second > (*rebasedGroup)[i].first)
              return std::nullopt;
          }

          // The edit must preserve the non-pasted prefix/suffix verbatim.
          // Otherwise we are no longer replaying the same direct pasted child.
          StringRef leading =
              observedOld.take_front((*rebasedGroup).front().first);
          StringRef trailing =
              observedOld.drop_front((*rebasedGroup).back().second);
          if (!observedNew.starts_with(leading) ||
              !observedNew.ends_with(trailing))
            return std::nullopt;

          // Extract the original operand surfaces and the literal material that
          // appeared between adjacent operands. The inter-operand material is
          // used below as the only permitted delimiter for splitting the new
          // pasted core.
          SmallVector<StringRef, 4> oldSegs;
          SmallVector<StringRef, 4> midBodies;
          oldSegs.reserve(group.size());
          midBodies.reserve(group.size() - 1);
          for (size_t i = 0; i < group.size(); ++i) {
            const auto [segBegin, segEnd] = (*rebasedGroup)[i];
            oldSegs.push_back(observedOld.slice(segBegin, segEnd));
            if (i + 1 < group.size()) {
              StringRef mid =
                  observedOld.slice(segEnd, (*rebasedGroup)[i + 1].first);
              if (mid.empty())
                return std::nullopt;
              midBodies.push_back(mid);
            }
          }

          StringRef core = observedNew.slice(
              leading.size(), observedNew.size() - trailing.size());

          // Count how many future occurrences of the current delimiter must be
          // reserved to make the remainder splittable. This lets the splitter
          // reject early cuts that would strand a later operand.
          auto suffixDelimiterNeed = [&](size_t delimIdx) -> uint64_t {
            const StringRef delim = midBodies[delimIdx];
            uint64_t need = 0;
            for (size_t segIdx = delimIdx + 1; segIdx < oldSegs.size();
                 ++segIdx)
              need += countSubstringOccurrences(oldSegs[segIdx], delim);
            for (size_t later = delimIdx + 1; later < midBodies.size(); ++later)
              if (midBodies[later] == delim)
                ++need;
            return need;
          };

          SmallVector<StringRef, 4> curSegs;
          SmallVector<SmallVector<StringRef, 4>, 2> splitSolutions;
          auto addSplitSolution = [&](const SmallVectorImpl<StringRef> &parts) {
            SmallVector<StringRef, 4> copy(parts.begin(), parts.end());
            for (const auto &existing : splitSolutions)
              if (existing == copy)
                return;
            splitSolutions.push_back(std::move(copy));
          };

          // Split the rewritten pasted core around the original inter-operand
          // delimiters. We only accept a unique segmentation; if multiple
          // splits work, the inverse-paste explanation is ambiguous and
          // therefore not a valid replay certificate.
          auto splitCore = [&](auto &&self, size_t delimIdx,
                               StringRef rest) -> void {
            if (splitSolutions.size() > 1)
              return;
            if (delimIdx == midBodies.size()) {
              curSegs.push_back(rest);
              addSplitSolution(curSegs);
              curSegs.pop_back();
              return;
            }

            const StringRef delim = midBodies[delimIdx];
            const uint64_t needLeft = countSubstringOccurrences(oldSegs[delimIdx], delim);
            const uint64_t needRight = suffixDelimiterNeed(delimIdx);

            for (size_t pos = 0;
                 (pos = rest.find(delim, pos)) != StringRef::npos; ++pos) {
              StringRef left = rest.slice(0, pos);
              StringRef tail = rest.drop_front(pos + delim.size());
              if (countSubstringOccurrences(left, delim) < needLeft)
                continue;
              if (countSubstringOccurrences(tail, delim) < needRight)
                continue;
              curSegs.push_back(left);
              self(self, delimIdx + 1, tail);
              curSegs.pop_back();
            }
          };
          splitCore(splitCore, 0, core);
          if (splitSolutions.size() != 1 ||
              splitSolutions[0].size() != group.size())
            return std::nullopt;

          // Lift each recovered child-operand rewrite through the child's
          // normal parent-constraint derivation, then merge the resulting
          // parent-formal constraints. Conflicting lifts mean the pasted
          // surface cannot be explained by one consistent replay of the
          // original child.
          DenseMap<uint32_t, ObservedFormalConstraint> mergedByFormal;
          for (size_t i = 0; i < group.size(); ++i) {
            const uint32_t childFormal = group[i]->argIdx;
            auto derived = buildParentConstraintDerivationCertificate(
                directChild, childFormal, oldSegs[i], splitSolutions[0][i],
                traceStage);
            if (!derived.valid)
              return std::nullopt;
            for (const auto &kv : derived.derivedConstraints) {
              auto itExisting = mergedByFormal.find(kv.first);
              if (itExisting == mergedByFormal.end()) {
                mergedByFormal.insert({kv.first, kv.second});
                continue;
              }
              if (itExisting->second.oldText != kv.second.oldText ||
                  itExisting->second.newText != kv.second.newText)
                return std::nullopt;
            }
          }

          // Return a stable, sorted set of derived parent-formal observations.
          SmallVector<std::pair<uint32_t, ObservedFormalConstraint>, 4> out;
          for (const auto &kv : mergedByFormal)
            out.push_back({kv.first, kv.second});
          llvm::sort(out, [](const auto &a, const auto &b) {
            return a.first < b.first;
          });
          if (out.empty())
            return std::nullopt;
          return out;
        };

        /// Rebuild the exact original nested paste shape for `target` when the
        /// observed edit still admits a unique, certificate-backed replay of
        /// that shape.
        ///
        /// This is a preservation path, not a synthesis path. It only reuses
        /// child invocations that already existed in the original source and
        /// only accepts the replay after the usual formal and placeholder
        /// certificates prove that the rebuilt invocation is structurally
        /// valid.
        std::function<std::optional<std::string>(
            const RefoldModel::MacroInvocation &, StringRef, StringRef,
            StringRef)>
            tryBuildExactOriginalShapePasteReplaySyntax;

        tryBuildExactOriginalShapePasteReplaySyntax =
            [&](const RefoldModel::MacroInvocation &target,
                StringRef observedOld0, StringRef observedNew0,
                StringRef traceStage) -> std::optional<std::string> {
          StringRef observedOld = observedOld0.trim();
          StringRef observedNew = observedNew0.trim();
          if (!target.invText)
            return std::nullopt;

          StringRef rawTarget = StringRef(*target.invText).trim();
          if (rawTarget.empty())
            return std::nullopt;
          if (observedOld == observedNew)
            return rawTarget.str();

          ArrayRef<const RefoldModel::MacroInvocation *> children =
              (*deps_.macroTopology).MacroChildrenOf(target.id);
          if (children.empty())
            return std::nullopt;

          const RefoldModel::MacroInvocation *directPasteChild = nullptr;
          std::optional<
              SmallVector<std::pair<uint32_t, ObservedFormalConstraint>, 4>>
              derivedConstraints;
          auto sameDerivedConstraints =
              [&](const SmallVectorImpl<
                      std::pair<uint32_t, ObservedFormalConstraint>> &lhs,
                  const SmallVectorImpl<
                      std::pair<uint32_t, ObservedFormalConstraint>> &rhs)
              -> bool {
            if (lhs.size() != rhs.size())
              return false;
            for (size_t i = 0; i < lhs.size(); ++i) {
              if (lhs[i].first != rhs[i].first)
                return false;
              if (lhs[i].second.oldText != rhs[i].second.oldText ||
                  lhs[i].second.newText != rhs[i].second.newText)
                return false;
            }
            return true;
          };

          // Find the unique direct child whose paste spans can explain the
          // observed rewrite. If more than one child derives different parent
          // constraints, the replay would be ambiguous and must be rejected.
          auto trySelectDirectChildForSurface =
              [&](StringRef surfaceOld, StringRef surfaceNew) -> bool {
            for (const auto *cand : children) {
              if (!cand || cand->pasteSpans.empty())
                continue;
              auto derived = tryDeriveObservedConstraintsFromDirectPasteChild(
                  target, *cand, surfaceOld, surfaceNew, traceStage);
              if (!derived)
                continue;
              if (directPasteChild) {
                if (directPasteChild != cand || !derivedConstraints ||
                    !sameDerivedConstraints(*derivedConstraints, *derived))
                  return false;
                continue;
              }
              directPasteChild = cand;
              derivedConstraints = std::move(derived);
            }
            return true;
          };

          if (!trySelectDirectChildForSurface(observedOld, observedNew))
            return std::nullopt;
          if (!directPasteChild) {
            // Some observed paste surfaces are represented through the quoted
            // spelling produced by stringification. Retry with quoted surfaces,
            // but still require the same unique direct-child proof.
            std::string quotedOld =
                stringutils::quoteCStringLiteral(observedOld);
            std::string quotedNew =
                stringutils::quoteCStringLiteral(observedNew);
            if (!trySelectDirectChildForSurface(quotedOld, quotedNew))
              return std::nullopt;
          }
          if (!directPasteChild || !derivedConstraints) {
            return std::nullopt;
          }

          // Group the derived constraints by the target's formals. Each group
          // is later replayed either by recursively preserving a nested child
          // or by falling back to the normal observed-formal certificate.
          DenseMap<uint32_t, SmallVector<ObservedFormalConstraint, 2>>
              groupedObserved;
          for (const auto &kv : *derivedConstraints)
            groupedObserved[kv.first].push_back(kv.second);

          DenseMap<uint32_t, FormalTextPair> targetFormals;
          for (const auto &kvLocal : groupedObserved) {
            const uint32_t formalIdx = kvLocal.first;
            auto argText = getInvocationArgText(target, formalIdx);
            if (!argText)
              return std::nullopt;
            const StringRef rawOldArg = argText->trim();

            // When the target formal is exactly one nested child invocation,
            // try to preserve that nested child first. This keeps a chain such
            // as JOIN(JOIN(...), ...) instead of collapsing it to the already
            // materialized pasted token.
            if (kvLocal.second.size() == 1) {
              const StringRef segOld =
                  StringRef(kvLocal.second.front().oldText).trim();
              const StringRef segNew =
                  StringRef(kvLocal.second.front().newText).trim();
              auto placeholders =
                  getTopLevelLexicalChildrenInArg(target, formalIdx);
              if (placeholders.size() == 1 && placeholders.front().child &&
                  placeholders.front().relBegin == 0 &&
                  placeholders.front().relEnd == rawOldArg.size()) {
                const RefoldModel::MacroInvocation *nestedChild =
                    placeholders.front().child;
                if (auto nestedSyntax =
                        tryBuildExactOriginalShapePasteReplaySyntax(
                            *nestedChild, segOld, segNew,
                            "DAG per-hop exact original-shape replay")) {
                  targetFormals[formalIdx] =
                      FormalTextPair{rawOldArg.str(), std::move(*nestedSyntax)};
                  continue;
                }
              }
            }

            // Otherwise, certify the formal rewrite in the usual way and let
            // the wrapper-hop certificate rebuild the target invocation around
            // it.
            auto formalCert = buildObservedFormalRewriteCertificate(
                target, formalIdx, kvLocal.second, /*preferredChildSyntax=*/nullptr,
                traceStage);
            if (formalCert.kind == FormalRewriteCertificateKind::Invalid)
              return std::nullopt;
            targetFormals[formalIdx] =
                FormalTextPair{formalCert.oldText, formalCert.newText};
          }

          // Finally, prove that the rewritten formals still fit the target's
          // original placeholder structure. This is the soundness gate for the
          // exact-shape replay at this invocation boundary.
          auto replayCert = buildWrapperPlaceholderHopInvocationCertificate(
              target, targetFormals, traceStage);
          if (replayCert.kind == InvocationRewriteCertificateKind::Invalid)
            return std::nullopt;
          if (!replayCert.rewrittenInvocationSyntax.empty())
            return replayCert.rewrittenInvocationSyntax;

          // If the placeholder certificate did not directly materialize syntax,
          // build it from only the formals that actually changed.
          DenseMap<uint32_t, std::string> replByFormal;
          for (const auto &kvLocal : targetFormals) {
            StringRef oldText = StringRef(kvLocal.second.oldText).trim();
            StringRef newText = StringRef(kvLocal.second.newText).trim();
            if (oldText != newText)
              replByFormal[kvLocal.first] = newText.str();
          }
          return buildRewrittenInvocationSyntax(target, replByFormal);
        };

        /// Handle the common DAG hop where one child formal maps directly to
        /// one parent formal.
        ///
        /// The normal result is a passthrough rewrite of the parent formal. If
        /// the child observed a flattened surface but the parent logical
        /// argument still contains one nested lexical child, this probes the
        /// exact-shape replay path first so that a provable nested paste tree
        /// is preserved rather than replaced by its materialized token text.
        auto tryBuildDirectPassthroughParentFormalRewrite =
            [&](uint32_t curFormal, uint32_t parentFormal,
                StringRef curNewText) -> std::optional<FormalTextPair> {
          if (curFormal >= cur.argDeps.size())
            return std::nullopt;
          ArrayRef<uint32_t> deps = cur.argDeps[curFormal];
          if (deps.size() != 1 || deps[0] != parentFormal)
            return std::nullopt;

          auto tpl = buildArgRefTemplate(cur, curFormal);
          if (!tpl || tpl->refs.size() != 1 ||
              tpl->distinctCallerParams.size() != 1)
            return std::nullopt;

          const auto &ref = tpl->refs[0];
          if (ref.callerParamIndex != parentFormal || ref.begin != 0 ||
              ref.end != StringRef(tpl->argText).trim().size())
            return std::nullopt;

          auto parentArgText = getInvocationArgText(*parent, parentFormal);
          if (!parentArgText)
            return std::nullopt;

          StringRef oldTrim = parentArgText->trim();
          StringRef newTrim = curNewText.trim();
          if (newTrim.empty() || oldTrim == newTrim)
            return std::nullopt;

          // `curFormals` may not carry this formal when the child rewrite was
          // derived through a different certified path, so guard the lookup.
          const auto curFormalIt = curFormals.find(curFormal);
          if (curFormalIt == curFormals.end())
            return std::nullopt;

          const StringRef childObservedOld =
              StringRef(curFormalIt->second.oldText).trim();


          // A mismatch here means the child has already collapsed some nested
          // structure relative to the parent's logical argument. If the parent
          // argument is exactly one lexical child, try to replay that original
          // nested shape instead of committing to the flatter replacement text.
          if (childObservedOld != oldTrim) {
            auto placeholders =
                getTopLevelLexicalChildrenInArg(*parent, parentFormal);
            if (placeholders.size() == 1 && placeholders.front().child &&
                placeholders.front().relBegin == 0 &&
                placeholders.front().relEnd == oldTrim.size()) {
              const RefoldModel::MacroInvocation *nestedChild =
                  placeholders.front().child;
              if (auto replaySyntax =
                      tryBuildExactOriginalShapePasteReplaySyntax(
                          *nestedChild, childObservedOld, newTrim,
                          "DAG per-hop exact original-shape replay")) {
                return FormalTextPair{oldTrim.str(), std::move(*replaySyntax)};
              }
            }
          }

          return FormalTextPair{oldTrim.str(), newTrim.str()};
        };

        // Parent observations are the inverted constraints this hop derives
        // from child-formal rewrites. `parentObservedSources` tracks which
        // child formals contributed each parent observation so that narrowly
        // scoped recovery paths can prove they are not merging unrelated input.
        DenseMap<uint32_t, SmallVector<ObservedFormalConstraint, 2>>
            parentObserved;
        DenseMap<uint32_t, SmallVector<uint32_t, 2>> parentObservedSources;
        SmallVector<uint32_t, 4> unresolvedChildFormals;
        SmallVector<std::string, 4> unresolvedDerivationDetails;

        auto recordObservedParentConstraint =
            [&](uint32_t parentFormal,
                const ObservedFormalConstraint &constraint,
                uint32_t sourceCurFormal) {
              auto &constraints = parentObserved[parentFormal];
              bool seen = false;
              for (const auto &existing : constraints) {
                if (existing.oldText == constraint.oldText &&
                    existing.newText == constraint.newText) {
                  seen = true;
                  break;
                }
              }
              if (!seen)
                constraints.push_back(constraint);

              auto &sources = parentObservedSources[parentFormal];
              if (llvm::find(sources, sourceCurFormal) == sources.end())
                sources.push_back(sourceCurFormal);
            };

        // Invert each child-formal rewrite through the current invocation's
        // formal dependency certificate. Failed inversions are not immediately
        // fatal because a sibling reroot or parent body-space proof may still
        // discharge the same obligation without requiring a lexical bridge.
        for (const auto &kvLocal : curFormals) {
          const uint32_t curFormal = kvLocal.first;
          StringRef curOld = kvLocal.second.oldText;
          StringRef curNew = kvLocal.second.newText;

          auto derivationCert = buildParentConstraintDerivationCertificate(
              cur, curFormal, curOld, curNew, "DAG per-hop");
          cert.derivations.push_back(derivationCert);
          if (!derivationCert.valid) {
            if (parent) {
              // Some failed direct inversions are still exactly explainable by
              // rerooting through a sibling child under the same parent. Accept
              // only if that reroot produces concrete parent-formal rewrites.
              auto siblingLift = tryBuildExactSiblingRerootLift(
                  *parent, cur, curFormal, curOld, curNew);
              if (siblingLift) {
                for (const auto &derived : siblingLift->nextFormals) {
                  const uint32_t parentFormal = derived.first;
                  const ObservedFormalConstraint constraint{
                      derived.second.oldText, derived.second.newText};
                  recordObservedParentConstraint(parentFormal, constraint,
                                                 curFormal);
                }
                continue;
              }
            }

            unresolvedChildFormals.push_back(curFormal);
            unresolvedDerivationDetails.push_back(derivationCert.detail);
            continue;
          }

          for (const auto &derived : derivationCert.derivedConstraints) {
            const uint32_t parentFormal = derived.first;
            const ObservedFormalConstraint &constraint = derived.second;
            recordObservedParentConstraint(parentFormal, constraint, curFormal);
          }
        }

        // Prefer the already-certified child invocation syntax when proving a
        // parent formal that contains this child as a placeholder. This lets
        // the parent certificate preserve the structured child spelling instead
        // of rediscovering or flattening it from observed text alone.
        DenseMap<uint64_t, std::string> preferredChildSyntax;
        if (!cert.rewrittenChildSyntax.empty())
          preferredChildSyntax[cur.id] = cert.rewrittenChildSyntax;


        if (inTraceMode()) {
          // Proof-ledger logging separates the sets that are easy to conflate:
          // observed parent arguments, paste arguments required by the parent,
          // and child formals that still need a non-direct discharge.
          auto observedParentArgIdxs = collectSortedUInt32Keys(parentObserved);
          auto requiredParentPasteArgIdxs =
              collectSortedUniquePasteArgIdxs(parent->pasteSpans);
          trace("macro/proof",
                "DAG per-hop proof ledger observed: child id={0} name={1} "
                "parent id={2} name={3} observedParentArgs={4} "
                "requiredParentPasteArgs={5} unresolvedChildFormals={6}",
                cur.id, cur.name, parent->id, parent->name,
                formatUInt32List(observedParentArgIdxs),
                formatUInt32List(requiredParentPasteArgIdxs),
                formatUInt32List(unresolvedChildFormals));
        }

        // Convert the observed parent-formal constraints into concrete parent
        // formal rewrites. This is the main consistency gate for the parent
        // boundary: conflicting observations, missing structural templates, or
        // unsupported placeholder interactions all fail here.
        DenseMap<uint32_t, FormalTextPair> parentFormals;
        for (const auto &kvLocal : parentObserved) {
          const uint32_t parentFormal = kvLocal.first;
          auto formalCert = buildObservedFormalRewriteCertificate(
              *parent, parentFormal, kvLocal.second,
              preferredChildSyntax.empty() ? nullptr : &preferredChildSyntax,
              "DAG per-hop");
          cert.parentFormalCertificates.push_back(formalCert);
          if (formalCert.kind == FormalRewriteCertificateKind::Invalid) {
            const bool templateMismatch =
                formalCert.failure ==
                FormalRewriteFailure::MissingStructuralTemplate;
            auto srcIt = parentObservedSources.find(parentFormal);
            if (templateMismatch && srcIt != parentObservedSources.end() &&
                srcIt->second.size() == 1) {
              const uint32_t sourceCurFormal = srcIt->second.front();
              auto curIt = curFormals.find(sourceCurFormal);
              if (curIt != curFormals.end()) {
                // A single-source template mismatch may be the direct
                // passthrough case where the child observed a flattened
                // surface. Accept this recovery only when the formal dependency
                // template proves a one-to-one child-formal to parent-formal
                // mapping.
                if (auto flatten = tryBuildDirectPassthroughParentFormalRewrite(
                        sourceCurFormal, parentFormal, curIt->second.newText)) {
                  parentFormals[parentFormal] = std::move(*flatten);
                  continue;
                }
              }
            }
            cert.failureReason =
                StructuredLiftFailureReason::ParentFormalInvalid;
            cert.parentFormalFailure = formalCert.failure;
            cert.detail =
                formatv("{0}; lexical bridge required", formalCert.detail)
                    .str();
            cert.kind = StructuredLiftCertificateKind::NeedsLexicalBridge;
            cert.nextInv = parent;
            return cert;
          }

          parentFormals[parentFormal] =
              FormalTextPair{formalCert.oldText, formalCert.newText};
        }

        if (inTraceMode()) {
          // After formal certification, compare the parent arguments we
          // actually carry with the paste arguments that the parent may need to
          // preserve. This is diagnostic-only: the later body-space gate owns
          // the semantic discharge.
          auto observedParentArgIdxs = collectSortedUInt32Keys(parentObserved);
          auto carriedParentArgIdxs = collectSortedUInt32Keys(parentFormals);
          auto requiredParentPasteArgIdxs =
              collectSortedUniquePasteArgIdxs(parent->pasteSpans);
          auto missingParentSupportArgIdxs = computeSortedMissingUInt32s(
              requiredParentPasteArgIdxs, carriedParentArgIdxs);
          trace("macro/proof",
                "DAG per-hop proof ledger carried: child id={0} name={1} "
                "parent id={2} name={3} observedParentArgs={4} "
                "carriedParentArgs={5} requiredParentPasteArgs={6} "
                "missingSupport={7} unresolvedChildFormals={8}",
                cur.id, cur.name, parent->id, parent->name,
                formatUInt32List(observedParentArgIdxs),
                formatUInt32List(carriedParentArgIdxs),
                formatUInt32List(requiredParentPasteArgIdxs),
                formatUInt32List(missingParentSupportArgIdxs),
                formatUInt32List(unresolvedChildFormals));
        }

        // Unresolved child-formal inversions are allowed only when a parent
        // formal certificate has proven that the parent body itself preserves
        // the relevant child syntax/raw invocation input. In that case the
        // missing direct inversion is discharged by body-space semantics rather
        // than by a guessed parent-formal constraint.
        auto unresolvedDerivationsDischargedByBodySpace = [&]() {
          if (unresolvedChildFormals.empty())
            return true;
          for (const auto &formalCert : cert.parentFormalCertificates) {
            const auto &sig = formalCert.interactionConsistency.signature;
            if (sig.usesPreferredChildSyntax ||
                sig.usesRawInvocationPreservation ||
                sig.usesRawChildInvocationLogicalInput)
              return true;
          }
          return false;
        };

        if (!unresolvedDerivationsDischargedByBodySpace()) {
          cert.failureReason =
              StructuredLiftFailureReason::ParentConstraintDerivationFailed;
          cert.derivationFailure =
              ParentConstraintDerivationFailure::InversionNotUnique;
          cert.detail =
              unresolvedDerivationDetails.empty()
                  ? formatv("DAG per-hop: unresolved parent "
                            "constraint derivation requires "
                            "lexical bridge: child id={0} name={1} "
                            "parent id={2} name={3}",
                            cur.id, cur.name, parent->id, parent->name)
                        .str()
                  : unresolvedDerivationDetails.front();
          cert.kind = StructuredLiftCertificateKind::NeedsLexicalBridge;
          cert.nextInv = parent;
          return cert;
        }

        // Final hop gate: prove that the parent invocation can be reconstructed
        // from the certified parent formal rewrites. A parent formal rewrite is
        // not enough by itself; the full invocation placeholder structure must
        // also remain valid.
        auto parentCert = buildWrapperPlaceholderHopInvocationCertificate(
            *parent, parentFormals, "DAG per-hop");
        cert.parentCert = parentCert;
        if (parentCert.kind == InvocationRewriteCertificateKind::Invalid) {
          cert.failureReason =
              StructuredLiftFailureReason::ParentInvocationInvalid;
          cert.parentInvocationFailure = parentCert.failure;
          cert.detail = parentCert.detail;
          cert.kind = StructuredLiftCertificateKind::NeedsLexicalBridge;
          cert.nextInv = parent;
          return cert;
        }
        if (parentCert.kind == InvocationRewriteCertificateKind::NoChange) {
          cert.detail =
              formatv("DAG per-hop: parent invocation no-change child "
                      "id={0} name={1} parent id={2} name={3} "
                      "parentFormals={4}",
                      cur.id, cur.name, parent->id, parent->name,
                      parentFormals.size())
                  .str();
        } else {
          cert.detail =
              formatv("DAG per-hop: structured hop child id={0} name={1} "
                      "parent id={2} name={3} parentFormals={4}",
                      cur.id, cur.name, parent->id, parent->name,
                      parentFormals.size())
                  .str();
        }


        // The hop is now fully certified: the next outer invocation is the
        // parent, and `nextFormals` carries the rewritten parent formals for
        // the next structured lift step.
        cert.kind = StructuredLiftCertificateKind::Unique;
        cert.nextInv = parent;
        cert.nextFormals = std::move(parentFormals);
        cert.parentFormalSources = std::move(parentObservedSources);
        return cert;
      };

      enum class LiftChainCertificateKind {
        Unique,
        Invalid,
      };

      struct LiftChainCertificate {
        LiftChainCertificateKind kind = LiftChainCertificateKind::Invalid;
        const RefoldModel::MacroInvocation *leaf = nullptr;
        SmallVector<uint32_t, 4> leafArgIdxs;
        DenseMap<uint32_t, FormalTextPair> leafFormals;
        SmallVector<StructuredLiftCertificate, 4> steps;
        bool usedLexicalBridge = false;
        DenseSet<uint32_t> bridgedRootArgIdxs;
        DenseMap<uint32_t, FormalTextPair> rootFormals;
        std::string detail;
      };

      /// Lift a proven leaf-formal rewrite outward through the macro DAG until
      /// it reaches the root invocation `m`.
      ///
      /// The leaf rewrite starts as a set of changed formal arguments on
      /// `leaf`. Each loop iteration tries to move that rewrite from the
      /// current invocation to its caller:
      ///
      /// * first through a structured DAG hop, where formal dependencies and
      ///   placeholder certificates prove the parent-formal rewrite directly;
      /// * otherwise through a lexical child bridge, where the
      ///   already-certified rewritten child syntax is substituted into the
      ///   parent argument text.
      ///
      /// The certificate records every hop, whether any lexical bridge was
      /// required, and which final root formals came from bridge-derived text.
      /// Failure is fail-closed: if any hop cannot be proven or bridged, the
      /// returned certificate remains non-unique and carries the failure
      /// detail.
      auto buildLiftChainCertificate =
          [&](const RefoldModel::MacroInvocation &leaf,
              const DenseMap<uint32_t, FormalTextPair> &leafFormals)
          -> LiftChainCertificate {
        LiftChainCertificate cert;
        cert.leaf = &leaf;

        // Normalize the starting point of the chain to only the leaf formals
        // that actually changed. No-change formals do not need to be lifted and
        // would only add noise to later proof obligations.
        for (const auto &kvLocal : leafFormals) {
          StringRef oldText = StringRef(kvLocal.second.oldText).trim();
          StringRef newText = StringRef(kvLocal.second.newText).trim();
          if (oldText == newText)
            continue;
          cert.leafArgIdxs.push_back(kvLocal.first);
          cert.leafFormals[kvLocal.first] =
              FormalTextPair{oldText.str(), newText.str()};
        }
        llvm::sort(cert.leafArgIdxs);

        // Nothing distinct reached the leaf boundary, so there is no lift chain
        // to build. This is not a proof failure; it is simply a non-candidate.
        if (cert.leafFormals.empty()) {
          cert.detail = formatv("DAG lift chain: leaf id={0} name={1} has no "
                                "distinct leaf formals to lift",
                                leaf.id, leaf.name)
                            .str();
          return cert;
        }

        const RefoldModel::MacroInvocation *cur = &leaf;
        DenseMap<uint32_t, FormalTextPair> curFormals;
        DenseSet<uint32_t> bridgedCurFormals;
        curFormals = cert.leafFormals;

        // Walk from the edited leaf toward the selected root invocation `m`.
        // At each point, `curFormals` is the certified rewrite at the current
        // invocation boundary.
        while (cur->id != m.id) {
          auto step = buildStructuredLiftCertificate(*cur, curFormals);
          cert.steps.push_back(step);

          // A hard invalid step means the hop failed before producing any
          // usable parent boundary. There is no safe bridge target to continue
          // from in this case.
          if (step.kind == StructuredLiftCertificateKind::Invalid) {
            cert.detail = step.detail;
            return cert;
          }

          if (step.kind == StructuredLiftCertificateKind::Unique) {
            if (!step.nextInv) {
              cert.detail = formatv(
                                "DAG lift chain: missing next invocation "
                                "after structured hop child id={0} name={1}",
                                cur->id, cur->name)
                                .str();
              return cert;
            }

            // Propagate bridge provenance through a successful structured hop.
            // If a current formal was bridge-derived, then any parent formal
            // whose certificate depends on that current formal is also marked
            // bridge-derived.
            DenseSet<uint32_t> nextBridgedCurFormals;
            if (!bridgedCurFormals.empty()) {
              for (const auto &kvLocal : step.nextFormals) {
                auto srcIt = step.parentFormalSources.find(kvLocal.first);
                if (srcIt == step.parentFormalSources.end())
                  continue;
                for (uint32_t sourceCurFormal : srcIt->second) {
                  if (bridgedCurFormals.contains(sourceCurFormal)) {
                    nextBridgedCurFormals.insert(kvLocal.first);
                    break;
                  }
                }
              }
            }

            // Advance to the parent using the structured proof result.
            cert.steps.back().bridgedNextFormals = nextBridgedCurFormals;
            cur = step.nextInv;
            curFormals = std::move(step.nextFormals);
            bridgedCurFormals = std::move(nextBridgedCurFormals);
            continue;
          }

          // The structured hop could not be proven, so the step must have
          // supplied both a parent invocation and a certified rewritten child
          // spelling for the lexical bridge path.
          if (!step.nextInv || step.rewrittenChildSyntax.empty()) {
            cert.detail = formatv(
                              "DAG lift chain: lexical bridge unavailable "
                              "child id={0} name={1}",
                              cur->id, cur->name)
                              .str();
            return cert;
          }

          // Bridge by replacing the child occurrence in the parent with the
          // already-certified rewritten child syntax. The bridge must return a
          // concrete parent-formal rewrite map; otherwise the lift cannot
          // continue soundly.
          auto bridged = tryLexicalChildBridge(*step.nextInv, *cur,
                                               step.rewrittenChildSyntax);
          if (!bridged) {
            cert.detail =
                formatv("DAG lift chain: lexical bridge failed "
                        "parent id={0} name={1} child id={2} name={3}",
                        step.nextInv->id, step.nextInv->name, cur->id,
                        cur->name)
                    .str();
            return cert;
          }

          // After a lexical bridge, every produced parent formal is marked as
          // bridge-derived. Later structured hops may propagate that provenance
          // farther outward through their parent-formal source maps.
          cert.usedLexicalBridge = true;
          cur = step.nextInv;
          curFormals = std::move(*bridged);
          bridgedCurFormals.clear();
          for (const auto &kvLocal : curFormals)
            bridgedCurFormals.insert(kvLocal.first);
          cert.steps.back().bridgedNextFormals = bridgedCurFormals;
        }

        // We have reached the root invocation. The current formal rewrite map
        // is now the root-formal rewrite map for the complete lift chain.
        for (const auto &kvLocal : curFormals) {
          cert.rootFormals[kvLocal.first] = kvLocal.second;
          if (bridgedCurFormals.contains(kvLocal.first))
            cert.bridgedRootArgIdxs.insert(kvLocal.first);
        }

        cert.kind = LiftChainCertificateKind::Unique;
        cert.detail = formatv(
                          "DAG lift chain: leaf id={0} name={1} leafArgs={2} "
                          "steps={3} lexicalBridge={4} rootFormals={5}",
                          leaf.id, leaf.name,
                          formatUInt32List(cert.leafArgIdxs), cert.steps.size(),
                          cert.usedLexicalBridge ? 1 : 0,
                          cert.rootFormals.size())
                          .str();
        return cert;
      };

      auto mergeCompatibleRootFormalRewrites =
          [&](StringRef baseOld0, ArrayRef<FormalTextPair> rewrites)
          -> std::optional<std::string> {
        return mergeCompatibleFormalRewrites(baseOld0, rewrites);
      };

      enum class RootFormalMergeCertificateKind {
        NoChange,
        Unique,
        Invalid,
      };

      enum class RootFormalMergeFailure {
        None,
        ArgIndexOutOfBounds,
        InvalidArgRange,
        MergeConflict,
      };

      struct RootFormalMergeCertificate {
        RootFormalMergeCertificateKind kind =
            RootFormalMergeCertificateKind::Invalid;
        RootFormalMergeFailure failure = RootFormalMergeFailure::None;
        uint32_t argIdx = 0;
        SmallVector<FormalTextPair, 2> observedRewrites;
        std::string baseArgText;
        std::string mergedArgText;
        std::string detail;
      };

      /// Merge all independently lifted rewrites for one root formal.
      ///
      /// Multiple leaf lift chains can arrive at the same root argument. This
      /// certificate verifies that the target root argument exists, recovers its
      /// original spelled text from the root invocation span, and then accepts
      /// the merge only if all proposed rewrites are mutually compatible with
      /// that original argument text.
      auto buildRootFormalMergeCertificate =
          [&](uint32_t argIdx, ArrayRef<FormalTextPair> rewrites,
              StringRef traceStage) -> RootFormalMergeCertificate {
        RootFormalMergeCertificate cert;
        cert.argIdx = argIdx;
        cert.observedRewrites.assign(rewrites.begin(), rewrites.end());

        // The merge is defined only for formals that exist on the root
        // invocation. Reject stale or malformed dependency information before
        // slicing the invocation text.
        if (argIdx >= invArgRanges.size()) {
          cert.failure = RootFormalMergeFailure::ArgIndexOutOfBounds;
          cert.detail = formatv(
                            "{0}: root arg index out of bounds root id={1} "
                            "name={2} argIdx={3} numArgs={4}",
                            traceStage, m.id, m.name, argIdx,
                            invArgRanges.size())
                            .str();
          return cert;
        }

        const size_t begin = invArgRanges[argIdx].first;
        const size_t end = invArgRanges[argIdx].second;
        if (begin > end || end > invSpanText.size()) {
          cert.failure = RootFormalMergeFailure::InvalidArgRange;
          cert.detail =
              formatv("{0}: root arg range invalid root id={1} name={2} "
                      "argIdx={3} range=[{4},{5}) spanLen={6}",
                      traceStage, m.id, m.name, argIdx, begin, end,
                      invSpanText.size())
                  .str();
          return cert;
        }

        const StringRef baseArgText = invSpanText.slice(begin, end).trim();
        cert.baseArgText = baseArgText.str();

        // This is the only semantic merge point for root-formal rewrites. The
        // helper must prove the rewrite set is compatible; otherwise competing
        // leaf chains are not allowed to silently overwrite one another.
        auto mergedNewArg =
            mergeCompatibleRootFormalRewrites(baseArgText, rewrites);
        if (!mergedNewArg) {
          cert.failure = RootFormalMergeFailure::MergeConflict;
          cert.detail = formatv(
                            "{0}: root rewrite merge conflicted root id={1} "
                            "name={2} argIdx={3}",
                            traceStage, m.id, m.name, argIdx)
                            .str();
          return cert;
        }

        cert.mergedArgText = StringRef(*mergedNewArg).trim().str();
        cert.kind = cert.baseArgText == cert.mergedArgText
                        ? RootFormalMergeCertificateKind::NoChange
                        : RootFormalMergeCertificateKind::Unique;
        return cert;
      };

      // Small utility structs for accumulating per-formal old/new and then
      // converting them into root-level byte edits in invSpanText.
      struct OldNewText {
        std::string oldText;
        std::string newText;
      };

      enum class SubtreeRewriteCertificateKind {
        NoChange,
        Unique,
        Invalid,
      };

      struct SubtreeInteractionSummaryCertificate {
        bool valid = true;
        bool hasPaste = false;
        bool hasStringify = false;
        bool hasWideStringify = false;
        bool hasRawInvocation = false;
        bool hasPreferredChildSyntax = false;
        bool hasMixedInteractions = false;
        bool hasStringifyPaste = false;
        bool hasWideStringifyPaste = false;
        bool hasRawInvocationPaste = false;
        bool hasChildSyntaxPaste = false;
        SmallVector<SemanticInteractionCertificate, 16> interactions;
        std::string detail;
      };

      enum class SubtreeSemanticAdmissibilityFailure {
        None,
        MixedSemanticInteractions,
        LexicalBridgeWithStructuredSemantics,
        DeferredPasteNotDischarged,
        PasteWithPassthroughFlatten,
      };

      struct SubtreeSemanticAdmissibilityCertificate {
        bool valid = true;
        SubtreeSemanticAdmissibilityFailure failure =
            SubtreeSemanticAdmissibilityFailure::None;
        std::string detail;
      };

      enum class DeferredPasteDischargeFailure {
        None,
        MissingSemanticDischarge,
        MissingAncestorPasteValidation,
      };

      struct DeferredPasteDischargeCertificate {
        bool valid = true;
        DeferredPasteDischargeFailure failure =
            DeferredPasteDischargeFailure::None;
        SmallVector<const InvocationRewriteCertificate *, 4>
            deferredInvocations;
        std::string detail;
      };

      struct SubtreeSemanticCertificate {
        bool valid = false;
        bool usesLexicalBridge = false;
        bool touchesPaste = false;
        bool hasWrapperSemantics = false;
        bool hasStringifySemantics = false;
        bool hasWideStringifySemantics = false;
        bool hasPreferredChildSyntax = false;
        bool hasRawInvocationPreservation = false;
        bool hasPassthroughFlatten = false;
        bool hasBridgeSensitiveStructuredSemantics = false;
        bool hasAcceptedRootPlaceholderReplay = false;
        bool rootReplayFlattensOnlyWholeChildArgs = false;
        const RefoldModel::MacroInvocation *acceptedRootReplayInv = nullptr;
        SmallVector<InvocationRewriteCertificate, 8> invocationCertificates;
        SmallVector<FormalRewriteCertificate, 16> formalCertificates;
        SmallVector<ArgSemanticRewriteCertificate, 16> argCertificates;
        SmallVector<SlotSemanticRewriteCertificate, 32> slotCertificates;
        SmallVector<SemanticInteractionCertificate, 32> interactionCertificates;
        SmallVector<FormalInteractionConsistencyCertificate, 16>
            formalInteractionConsistencies;
        SmallVector<RawFormalValidationCertificate, 16> rawFormalValidations;
        SmallVector<PasteRewriteValidationCertificate, 8> pasteValidations;
        SmallVector<ParentConstraintDerivationCertificate, 16>
            parentDerivations;
        SmallVector<StructuredLiftCertificate, 8> structuredLiftCertificates;
        SmallVector<LiftChainCertificate, 4> liftChains;
        SmallVector<RootFormalMergeCertificate, 4> rootMergeCertificates;
        StringSet<> bridgedFormalKeys;
        StringSet<> bridgedInteractionKeys;
        SubtreeInteractionSummaryCertificate interactionSummary;
        SubtreeInteractionConsistencyCertificate interactionConsistency;
        DeferredPasteDischargeCertificate deferredPasteDischarge;
        SubtreeSemanticAdmissibilityCertificate admissibility;
        std::string detail;
      };

      struct SubtreeRewriteCertificate {
        SubtreeRewriteCertificateKind kind =
            SubtreeRewriteCertificateKind::Invalid;
        const RefoldModel::MacroInvocation *leaf = nullptr;
        const RefoldModel::MacroInvocation *root = nullptr;
        DenseMap<uint32_t, FormalTextPair> leafFormals;
        DenseMap<uint32_t, FormalTextPair> rootFormals;
        SmallVector<uint32_t, 8> deferRootOccurrenceArgIdxs;
        InvocationRewriteCertificate leafCert;
        InvocationRewriteCertificate rootCert;
        SmallVector<LiftChainCertificate, 4> liftCertificates;
        SmallVector<RootFormalMergeCertificate, 4> rootMergeCertificates;
        SubtreeSemanticCertificate semantic;
        std::string detail;
      };

      /// Summarize the semantic interaction modes observed inside a macro
      /// subtree.
      ///
      /// This certificate is deliberately coarse-grained: it does not prove a
      /// rewrite by itself, but records which sensitive macro semantics appear
      /// below this point, such as paste, stringify, raw invocation preservation,
      /// preferred child syntax, and mixed interaction modes. Later selection
      /// and merge logic can then make decisions using explicit interaction
      /// facts instead of re-inspecting every child certificate.
      auto buildSubtreeInteractionSummaryCertificate =
          [&](ArrayRef<SemanticInteractionCertificate> interactions)
          -> SubtreeInteractionSummaryCertificate {
        SubtreeInteractionSummaryCertificate cert;
        cert.interactions.assign(interactions.begin(), interactions.end());

        // Fold each per-interaction certificate into subtree-wide feature bits.
        // The generic booleans record whether a mechanism appears anywhere,
        // while the switch records the important combined modes where that
        // mechanism interacts with paste or with other mixed semantics.
        for (const auto &interaction : interactions) {
          cert.hasPaste |= interaction.touchesPaste;
          cert.hasStringify |= interaction.usesStringify;
          cert.hasWideStringify |= interaction.usesWideStringify;
          cert.hasRawInvocation |= interaction.usesRawInvocationPreservation;
          cert.hasPreferredChildSyntax |= interaction.usesPreferredChildSyntax;
          switch (interaction.kind) {
          case SemanticInteractionKind::Plain:
          case SemanticInteractionKind::ChildSyntax:
          case SemanticInteractionKind::RawInvocation:
          case SemanticInteractionKind::Stringify:
          case SemanticInteractionKind::WideStringify:
          case SemanticInteractionKind::Paste:
            break;
          case SemanticInteractionKind::ChildSyntaxPaste:
            cert.hasChildSyntaxPaste = true;
            break;
          case SemanticInteractionKind::RawInvocationPaste:
            cert.hasRawInvocationPaste = true;
            break;
          case SemanticInteractionKind::StringifyPaste:
            cert.hasStringifyPaste = true;
            break;
          case SemanticInteractionKind::WideStringifyPaste:
            cert.hasStringifyPaste = true;
            cert.hasWideStringifyPaste = true;
            break;
          case SemanticInteractionKind::Mixed:
            cert.hasMixedInteractions = true;
            break;
          }
        }

        cert.detail =
            formatv("subtree interaction summary: interactions={0} "
                    "paste={1} stringify={2} wide={3} rawInvocation={4} "
                    "childSyntax={5} stringifyPaste={6} "
                    "wideStringifyPaste={7} rawInvocationPaste={8} "
                    "childSyntaxPaste={9} mixed={10}",
                    cert.interactions.size(), cert.hasPaste ? 1 : 0,
                    cert.hasStringify ? 1 : 0, cert.hasWideStringify ? 1 : 0,
                    cert.hasRawInvocation ? 1 : 0,
                    cert.hasPreferredChildSyntax ? 1 : 0,
                    cert.hasStringifyPaste ? 1 : 0,
                    cert.hasWideStringifyPaste ? 1 : 0,
                    cert.hasRawInvocationPaste ? 1 : 0,
                    cert.hasChildSyntaxPaste ? 1 : 0,
                    cert.hasMixedInteractions ? 1 : 0)
                .str();
        return cert;
      };

      /// Verify that every formal reached through the subtree has consistent
      /// semantic-interaction evidence across all lift/root paths.
      ///
      /// A single logical formal may be encountered more than once when
      /// multiple child rewrites lift to the same invocation argument. This
      /// certificate allows duplicate observations only when their interaction
      /// signatures are identical. Divergent signatures mean different paths
      /// disagree about whether the formal depends on paste, stringify, raw
      /// invocation preservation, preferred child syntax, or another sensitive
      /// semantic mode.
      auto buildSubtreeInteractionConsistencyCertificate =
          [&](ArrayRef<FormalRewriteCertificate> formalCertificates)
          -> SubtreeInteractionConsistencyCertificate {
        SubtreeInteractionConsistencyCertificate cert;
        StringMap<size_t> keyToIndex;

        // Key by logical formal identity, not by certificate position. The same
        // invocation argument may appear through several lifted rewrite paths.
        auto makeKey = [&](const RefoldModel::MacroInvocation *inv,
                           uint32_t argIdx) -> std::string {
          return formatv("{0}#{1}", inv ? inv->id : 0, argIdx).str();
        };

        for (const auto &formalCert : formalCertificates) {
          cert.formalConsistencies.push_back(formalCert.interactionConsistency);
          const auto &consistency = cert.formalConsistencies.back();

          // A formal-level invalidity is already a failed proof obligation, so
          // the subtree consistency certificate must fail immediately.
          if (!consistency.valid) {
            cert.valid = false;
            cert.failure =
                SubtreeInteractionConsistencyFailure::DivergentFormalSemantics;
            cert.detail = consistency.detail;
            return cert;
          }

          std::string key = makeKey(consistency.inv, consistency.argIdx);
          auto it = keyToIndex.find(key);
          if (it == keyToIndex.end()) {
            keyToIndex[key] = cert.formalConsistencies.size() - 1;
            continue;
          }

          // Duplicate evidence for the same logical formal is acceptable only
          // if the semantic signature is exactly the same. Otherwise two paths
          // are asking the same formal to be interpreted under different macro
          // semantics, which is not a sound merge.
          const auto &existing = cert.formalConsistencies[it->second];
          if (!(existing.signature == consistency.signature)) {
            cert.valid = false;
            cert.failure =
                SubtreeInteractionConsistencyFailure::DivergentFormalSemantics;
            cert.detail = formatv(
                              "subtree semantic interaction consistency "
                              "failed: inv id={0} argIdx={1} formal semantic "
                              "evidence diverged across lift/root paths",
                              consistency.inv ? consistency.inv->id : 0,
                              consistency.argIdx)
                              .str();
            return cert;
          }
        }

        cert.detail = formatv(
                          "subtree interaction consistency: formals={0} "
                          "uniqueFormals={1}",
                          cert.formalConsistencies.size(), keyToIndex.size())
                          .str();
        return cert;
      };

      /// Prove that every deferred paste-validation obligation in the subtree is
      /// discharged by a later, semantically stronger witness.
      ///
      /// Some invocation certificates intentionally defer paste validation when
      /// the local hop cannot fully validate the paste shape in isolation. This
      /// certificate checks that each deferred paste is eventually justified by
      /// at least one accepted outer explanation:
      ///
      /// * a semantic discharge from an ancestor formal certificate,
      /// * a valid paste replay on an ancestor invocation, or
      /// * the accepted root replay, including the lexical-bridge case where
      ///   bridge-derived root formals exactly match the root certificate.
      ///
      /// If none of those witnesses exists, the deferred paste would become an
      /// unproven assumption, so the subtree certificate fails closed.
      auto buildSubtreeDeferredPasteDischargeCertificate =
          [&](const SubtreeSemanticCertificate &semantic,
              const InvocationRewriteCertificate &rootCert)
          -> DeferredPasteDischargeCertificate {
        DeferredPasteDischargeCertificate cert;

        // Walk caller links to determine whether `ancestor` dominates
        // `descendant` in the macro invocation DAG. Missing metadata is treated
        // as non-ancestry rather than guessed.
        auto isAncestorOrSame =
            [&](const RefoldModel::MacroInvocation *ancestor,
                const RefoldModel::MacroInvocation *descendant) -> bool {
          if (!ancestor || !descendant)
            return false;
          const RefoldModel::MacroInvocation *cur = descendant;
          while (cur) {
            if (cur->id == ancestor->id)
              return true;
            if (!cur->callerMacroId)
              break;
            auto it = subtreeValidationCtx.invocationById.find(*cur->callerMacroId);
            if (it == subtreeValidationCtx.invocationById.end())
              break;
            cur = it->second;
          }
          return false;
        };

        // A deferred paste can also be discharged by the accepted root replay
        // when the path from the deferred invocation to the root used a lexical
        // bridge. In that case, require every bridge-derived root formal to
        // appear verbatim in the accepted root rewrite certificate.
        auto rootReplayMatchesLexicalBridgeChain =
            [&](const InvocationRewriteCertificate &deferredInvCert) -> bool {
          if (!deferredInvCert.inv || !rootCert.inv ||
              !rootCert.pasteValidation.valid)
            return false;

          for (const auto &liftCert : semantic.liftChains) {
            if (!liftCert.leaf)
              continue;
            if (!isAncestorOrSame(deferredInvCert.inv, liftCert.leaf))
              continue;
            if (!liftCert.usedLexicalBridge ||
                liftCert.bridgedRootArgIdxs.empty())
              continue;

            bool allBridgedRootFormalsMatched = true;
            for (uint32_t rootArgIdx : liftCert.bridgedRootArgIdxs) {
              auto rootFormalIt = liftCert.rootFormals.find(rootArgIdx);
              if (rootFormalIt == liftCert.rootFormals.end()) {
                allBridgedRootFormalsMatched = false;
                break;
              }

              bool matchedRewrite = false;
              for (const auto &rewrite : rootCert.rewrites) {
                if (rewrite.argIdx != rootArgIdx)
                  continue;
                if (rewrite.oldText == rootFormalIt->second.oldText &&
                    rewrite.newText == rootFormalIt->second.newText) {
                  matchedRewrite = true;
                  break;
                }
              }
              if (!matchedRewrite) {
                allBridgedRootFormalsMatched = false;
                break;
              }
            }

            if (allBridgedRootFormalsMatched)
              return true;
          }
          return false;
        };

        for (const auto &invCert : semantic.invocationCertificates) {
          if (!invCert.pasteValidation.deferred)
            continue;
          cert.deferredInvocations.push_back(&invCert);

          // Semantic discharge means an ancestor formal certificate used a mode
          // strong enough to preserve the paste-sensitive input without relying
          // only on local paste-shape validation.
          bool hasSemanticDischarge = false;
          for (const auto &formalCert : semantic.formalCertificates) {
            if (!isAncestorOrSame(formalCert.inv, invCert.inv))
              continue;
            const auto &sig = formalCert.interactionConsistency.signature;
            if (sig.usesPreferredChildSyntax ||
                sig.usesRawInvocationPreservation || sig.usesStringify ||
                sig.usesWideStringify ||
                sig.usesRawChildInvocationLogicalInput) {
              hasSemanticDischarge = true;
              break;
            }
          }

          // Ancestor replay discharge means some strictly outer invocation has
          // already validated a paste replay that covers this deferred inner
          // obligation.
          bool hasAncestorReplayPath = false;
          for (const auto &candidate : semantic.invocationCertificates) {
            if (!candidate.pasteValidation.valid)
              continue;
            if (!isAncestorOrSame(candidate.inv, invCert.inv))
              continue;
            if (candidate.inv && invCert.inv &&
                candidate.inv->id == invCert.inv->id)
              continue;
            hasAncestorReplayPath = true;
            break;
          }

          // The root replay is a valid discharge either when the deferred
          // invocation is the root itself, or when the bridge-derived root
          // formals prove that the accepted root rewrite consumed the bridged
          // syntax exactly.
          const bool hasAcceptedRootReplayCandidate =
              (invCert.inv && rootCert.inv &&
               invCert.inv->id == rootCert.inv->id &&
               rootCert.pasteValidation.valid) ||
              rootReplayMatchesLexicalBridgeChain(invCert);


          // Every deferred paste must have an explicit discharge witness. Do
          // not allow a deferred local proof to leak into the accepted subtree
          // as an unstated global assumption.
          if (!(hasSemanticDischarge || hasAncestorReplayPath ||
                hasAcceptedRootReplayCandidate)) {
            cert.valid = false;
            cert.failure = hasSemanticDischarge
                               ? DeferredPasteDischargeFailure::
                                     MissingAncestorPasteValidation
                               : DeferredPasteDischargeFailure::
                                     MissingSemanticDischarge;
            cert.detail = formatv(
                              "subtree deferred paste discharge failed: inv "
                              "id={0} name={1} has deferred paste validation "
                              "without ancestor semantic discharge, "
                              "accepted ancestor replay path, or accepted "
                              "root replay candidate",
                              invCert.inv ? invCert.inv->id : 0,
                              invCert.inv ? invCert.inv->name
                                          : StringRef("<none>"))
                              .str();
            return cert;
          }
        }

        cert.detail = formatv(
                          "subtree deferred paste discharge: deferredInvs={0}",
                          cert.deferredInvocations.size())
                          .str();
        return cert;
      };

      /// Decide whether the collected subtree semantics are admissible for a
      /// structure-preserving macro rewrite.
      ///
      /// This is the final semantic gate after the subtree has collected lift
      /// chains, formal interaction summaries, deferred paste obligations, and
      /// root replay information. It does not construct new rewrite evidence;
      /// it only checks that the evidence already collected is strong enough to
      /// accept the subtree without relying on an ambiguous or lossy macro
      /// interpretation.
      ///
      /// The certificate fails closed for three important cases:
      ///
      /// * deferred paste obligations that were never discharged,
      /// * mixed semantic interactions that cannot be represented by one
      ///   structural proof class, and
      /// * bridge-sensitive structured semantics that survived a lexical
      ///   bridge.
      auto buildSubtreeSemanticAdmissibilityCertificate =
          [&](const SubtreeSemanticCertificate &semantic)
          -> SubtreeSemanticAdmissibilityCertificate {
        SubtreeSemanticAdmissibilityCertificate cert;

        // Deferred paste validation is allowed only if a later semantic or
        // ancestor replay witness discharged it. Otherwise the subtree would be
        // accepted with an unresolved paste-shape obligation.
        if (!semantic.deferredPasteDischarge.valid) {
          cert.valid = false;
          cert.failure =
              SubtreeSemanticAdmissibilityFailure::DeferredPasteNotDischarged;
          cert.detail = semantic.deferredPasteDischarge.detail;
          return cert;
        }

        // Mixed interactions are rejected at the subtree level because they
        // indicate that the same accepted subtree would need incompatible
        // semantic interpretations, such as wrapper/stringify/paste behavior
        // that cannot be folded into one sound structural witness.
        if (semantic.interactionSummary.hasMixedInteractions) {
          cert.valid = false;
          cert.failure =
              SubtreeSemanticAdmissibilityFailure::MixedSemanticInteractions;
          cert.detail =
              "subtree semantic admissibility failed: mixed wrapper/"
              "stringify/paste interactions are not structurally admissible";
          return cert;
        }

        // Classify the sensitive semantic features that make a lexical bridge
        // or flattening step dangerous. These summary booleans keep the later
        // admissibility checks readable and ensure every rejection is based on
        // explicit subtree facts.
        const bool hasInteractionScopedPasteSemantics =
            semantic.interactionSummary.hasPaste ||
            semantic.interactionSummary.hasStringifyPaste ||
            semantic.interactionSummary.hasWideStringifyPaste ||
            semantic.interactionSummary.hasRawInvocationPaste ||
            semantic.interactionSummary.hasChildSyntaxPaste;
        const bool hasStructuredSemantics =
            semantic.hasWrapperSemantics || semantic.hasPreferredChildSyntax ||
            semantic.hasRawInvocationPreservation ||
            hasInteractionScopedPasteSemantics;
        const bool hasBridgeSensitiveStructuredSemantics =
            semantic.hasBridgeSensitiveStructuredSemantics;

        // Strict mode must fail closed when a paste-bearing subtree only
        // reaches its parent through passthrough flatten. That rewrite path
        // intentionally drops interior structural boundaries, which makes
        // nested pasted-token edits underdetermined: multiple replay candidates
        // can survive even though they share the same final pasted spelling.
        //
        // One narrow proof class is still admissible: if the accepted root
        // replay itself is a deferred wrapper-placeholder replay whose
        // rewritten syntax is known, and every rewritten root formal
        // corresponds to exactly one whole-child placeholder, then the parent
        // invocation is certified while only the child subtree is flattened. In
        // that case we are not inventing interior child structure; we are
        // preserving only the ancestor syntax that has already been proven
        // replayable.
        const bool allowRootPlaceholderFlattenReplay =
            semantic.hasAcceptedRootPlaceholderReplay &&
            semantic.rootReplayFlattensOnlyWholeChildArgs &&
            semantic.acceptedRootReplayInv &&
            semantic.deferredPasteDischarge.valid &&
            !semantic.deferredPasteDischarge.deferredInvocations.empty() &&
            llvm::all_of(
                semantic.deferredPasteDischarge.deferredInvocations,
                [&](const InvocationRewriteCertificate *invCert) {
                  return invCert && invCert->inv &&
                         invCert->inv->id == semantic.acceptedRootReplayInv->id;
                });
        if (semantic.touchesPaste && semantic.hasPassthroughFlatten &&
            !allowRootPlaceholderFlattenReplay) {
          cert.valid = false;
          cert.failure =
              SubtreeSemanticAdmissibilityFailure::PasteWithPassthroughFlatten;
          cert.detail =
              "subtree semantic admissibility failed: paste-bearing subtree "
              "relies on passthrough flatten and therefore does not have a "
              "unique structure-preserving witness";
          return cert;
        }

        // The only allowed paste+flatten case is the explicit root-placeholder
        // replay exception above. Log it as an admissible exception so that it
        // remains visible in proof traces.

        // A lexical bridge can safely carry plain text, but it must not be the
        // remaining explanation for wrapper/raw-invocation/paste-sensitive
        // structure. If such semantics are still bridge-sensitive here, the
        // subtree has lost the structural witness needed for sound replay.
        if (hasBridgeSensitiveStructuredSemantics) {
          cert.valid = false;
          cert.failure = SubtreeSemanticAdmissibilityFailure::
              LexicalBridgeWithStructuredSemantics;
          cert.detail =
              "subtree semantic admissibility failed: bridge-sensitive "
              "wrapper/raw-invocation/paste subtree semantics remain "
              "inadmissible";
          return cert;
        }

        // All semantic hazards were either absent or explicitly discharged.
        // Record the summary facts used by the admissibility decision.
        cert.detail =
            formatv("subtree semantic admissibility: lexicalBridge={0} "
                    "mixed={1} structuredSemantics={2} "
                    "bridgeSensitiveStructuredSemantics={3} "
                    "bridgedFormals={4} bridgedInteractions={5}",
                    semantic.usesLexicalBridge ? 1 : 0,
                    semantic.interactionSummary.hasMixedInteractions ? 1 : 0,
                    hasStructuredSemantics ? 1 : 0,
                    semantic.hasBridgeSensitiveStructuredSemantics ? 1 : 0,
                    semantic.bridgedFormalKeys.size(),
                    semantic.bridgedInteractionKeys.size())
                .str();
        return cert;
      };

      /// Assemble the complete semantic proof bundle for one accepted macro
      /// subtree rewrite.
      ///
      /// This gathers every proof artifact produced while moving from the leaf
      /// invocation to the root invocation: invocation certificates, lift-chain
      /// steps, parent-constraint derivations, formal/argument/slot rewrite
      /// certificates, root-formal merge certificates, raw-formal validations,
      /// paste validations, and semantic interaction summaries.
      ///
      /// After collection, the bundle is checked in three stages:
      ///
      /// * all repeated formal-interaction evidence must be consistent,
      /// * every deferred paste obligation must be discharged, and
      /// * the combined subtree semantics must be admissible.
      ///
      /// The result is fail-closed. If any semantic gate rejects, the returned
      /// certificate is invalid and carries that gate's detail string.
      auto buildSubtreeSemanticCertificate =
          [&](const InvocationRewriteCertificate &leafCert,
              ArrayRef<LiftChainCertificate> liftCertificates,
              ArrayRef<RootFormalMergeCertificate> rootMergeCertificates,
              const InvocationRewriteCertificate &rootCert)
          -> SubtreeSemanticCertificate {
        SubtreeSemanticCertificate cert;

        // Use a stable logical-formal key for bookkeeping across certificates.
        // The same invocation/formal can be reached through multiple lift paths.
        auto makeFormalKey = [&](const RefoldModel::MacroInvocation *inv,
                                 uint32_t argIdx) -> std::string {
          return formatv("{0}#{1}", inv ? inv->id : 0, argIdx).str();
        };

        // Returns true iff the original root formal is exactly one top-level
        // child placeholder and nothing else. This is the structural predicate
        // for preserving the parent while flattening only that child.
        auto rootFormalIsWholeSingleChildPlaceholder =
            [&](const InvocationRewriteCertificate &invCert,
                uint32_t argIdx) -> bool {
          if (!invCert.inv)
            return false;
          auto rawArg = getInvocationArgText(*invCert.inv, argIdx);
          if (!rawArg)
            return false;
          auto placeholders =
              getTopLevelLexicalChildrenInArg(*invCert.inv, argIdx);
          return placeholders.size() == 1 && placeholders.front().child &&
                 placeholders.front().relBegin == 0 &&
                 placeholders.front().relEnd == rawArg->size();
        };

        // Slot certificates are where wrapper spelling decisions first become
        // visible. Fold them into the subtree feature bits used later by the
        // admissibility gate.
        auto recordSlotCertificate =
            [&](const SlotSemanticRewriteCertificate &slotCert) {
              cert.slotCertificates.push_back(slotCert);
              switch (slotCert.decision.kind) {
              case SlotRewriteDecisionKind::PreferredChildSyntax:
                cert.hasPreferredChildSyntax = true;
                break;
              case SlotRewriteDecisionKind::PreserveRawInvocation:
                cert.hasRawInvocationPreservation = true;
                break;
              case SlotRewriteDecisionKind::PassthroughFlatten:
                cert.hasPassthroughFlatten = true;
                break;
              }

              switch (slotCert.wrapperKind) {
              case WrapperChainKind::Exact:
                break;
              case WrapperChainKind::StringLiteral:
                cert.hasWrapperSemantics = true;
                cert.hasStringifySemantics = true;
                break;
              case WrapperChainKind::WideStringLiteral:
                cert.hasWrapperSemantics = true;
                cert.hasStringifySemantics = true;
                cert.hasWideStringifySemantics = true;
                break;
              }
            };

        // Argument certificates own the slot certificates for one rewritten
        // formal argument. Record both levels so later diagnostics can report
        // the complete proof trail.
        auto recordArgCertificate =
            [&](const ArgSemanticRewriteCertificate &argCert) {
              cert.argCertificates.push_back(argCert);
              for (const auto &slotCert : argCert.slotCertificates)
                recordSlotCertificate(slotCert);
            };

        // Record a formal rewrite certificate and all semantic evidence nested
        // below it. If this formal was produced by a lexical bridge, remember
        // that provenance and mark bridge-sensitive semantics when the formal
        // still depends on paste, stringify, raw invocation, child syntax, or
        // passthrough flattening.
        auto recordFormalCertificate =
            [&](const FormalRewriteCertificate &formalCert,
                bool bridgeSensitive) {
              cert.formalCertificates.push_back(formalCert);
              cert.formalInteractionConsistencies.push_back(
                  formalCert.interactionConsistency);
              if (bridgeSensitive)
                cert.bridgedFormalKeys.insert(
                    makeFormalKey(formalCert.inv, formalCert.argIdx));
              if (formalCert.validation.valid ||
                  formalCert.validation.failure !=
                      RawFormalValidationFailure::None)
                cert.rawFormalValidations.push_back(formalCert.validation);
              for (const auto &interactionCert :
                   formalCert.interactionCertificates) {
                cert.interactionCertificates.push_back(interactionCert);
                if (bridgeSensitive) {
                  cert.bridgedInteractionKeys.insert(makeFormalKey(
                      interactionCert.inv, interactionCert.argIdx));
                  const auto &sig = formalCert.interactionConsistency.signature;
                  if (sig.touchesPaste || sig.usesPreferredChildSyntax ||
                      sig.usesRawInvocationPreservation ||
                      sig.usesPassthroughFlatten || sig.usesStringify ||
                      sig.usesWideStringify ||
                      sig.usesRawChildInvocationLogicalInput)
                    cert.hasBridgeSensitiveStructuredSemantics = true;
                }
              }
              for (const auto &argCert : formalCert.argRewriteCertificates)
                recordArgCertificate(argCert);
            };

        // Invocation certificates contribute raw-formal validations and paste
        // validation state. Any required or failed paste validation makes the
        // subtree paste-sensitive for the final admissibility checks.
        auto recordInvocationCertificate =
            [&](const InvocationRewriteCertificate &invCert) {
              cert.invocationCertificates.push_back(invCert);
              for (const auto &validation : invCert.formalValidations)
                cert.rawFormalValidations.push_back(validation);
              if (invCert.pasteValidation.required ||
                  !invCert.pasteValidation.valid) {
                cert.pasteValidations.push_back(invCert.pasteValidation);
                cert.touchesPaste = true;
              }
            };

        // Seed the bundle with the leaf certificate, then walk every lift chain
        // and record the proof artifacts produced at each structured hop.
        recordInvocationCertificate(leafCert);
        for (const auto &liftCert : liftCertificates) {
          cert.liftChains.push_back(liftCert);
          cert.usesLexicalBridge |= liftCert.usedLexicalBridge;
          for (const auto &step : liftCert.steps) {
            cert.structuredLiftCertificates.push_back(step);
            recordInvocationCertificate(step.currentCert);
            for (const auto &derivation : step.derivations)
              cert.parentDerivations.push_back(derivation);
            for (const auto &formalCert : step.parentFormalCertificates)
              recordFormalCertificate(
                  formalCert,
                  step.bridgedNextFormals.contains(formalCert.argIdx));
            if (step.parentCert.kind !=
                InvocationRewriteCertificateKind::Invalid)
              recordInvocationCertificate(step.parentCert);
          }
        }

        for (const auto &mergeCert : rootMergeCertificates)
          cert.rootMergeCertificates.push_back(mergeCert);

        // The accepted root certificate is part of the same semantic bundle:
        // it may discharge deferred paste obligations or establish the narrow
        // root-placeholder flatten replay exception below.
        recordInvocationCertificate(rootCert);

        // Track whether the accepted root replay qualifies for the narrow
        // "preserve parent / flatten child" proof class. We only admit root
        // replays that are already uniquely certified deferred paste replays
        // with concrete rewritten syntax, and only when every rewritten formal
        // corresponds to one whole child placeholder in the original root.
        if (rootCert.kind == InvocationRewriteCertificateKind::Unique &&
            rootCert.pasteValidation.required &&
            rootCert.pasteValidation.valid &&
            rootCert.pasteValidation.deferred &&
            !rootCert.rewrittenInvocationSyntax.empty() && rootCert.inv) {
          cert.hasAcceptedRootPlaceholderReplay = true;
          cert.acceptedRootReplayInv = rootCert.inv;
          cert.rootReplayFlattensOnlyWholeChildArgs =
              !rootCert.rewrites.empty();
          for (const auto &rewrite : rootCert.rewrites) {
            if (!rootFormalIsWholeSingleChildPlaceholder(rootCert,
                                                         rewrite.argIdx)) {
              cert.rootReplayFlattensOnlyWholeChildArgs = false;
              break;
            }
          }
        }

        // Collapse the collected interaction certificates into subtree-wide
        // feature bits, then verify that repeated evidence for the same logical
        // formal agrees across all lift/root paths.
        cert.interactionSummary = buildSubtreeInteractionSummaryCertificate(
            cert.interactionCertificates);
        cert.interactionConsistency =
            buildSubtreeInteractionConsistencyCertificate(
                cert.formalCertificates);
        if (!cert.interactionConsistency.valid) {
          cert.valid = false;
          cert.detail = cert.interactionConsistency.detail;
          return cert;
        }

        // Deferred paste validations are permitted only if this complete bundle
        // contains an ancestor, root replay, or semantic witness that discharges
        // them.
        cert.deferredPasteDischarge =
            buildSubtreeDeferredPasteDischargeCertificate(cert, rootCert);
        if (!cert.deferredPasteDischarge.valid) {
          cert.valid = false;
          cert.detail = cert.deferredPasteDischarge.detail;
          return cert;
        }

        // Final semantic gate: reject combinations that are individually proven
        // but not jointly admissible, such as bridge-sensitive structured
        // semantics surviving through a lexical bridge.
        cert.admissibility = buildSubtreeSemanticAdmissibilityCertificate(cert);
        if (!cert.admissibility.valid) {
          cert.valid = false;
          cert.detail = cert.admissibility.detail;
          return cert;
        }

        cert.valid = true;
        cert.detail =
            formatv("subtree semantic bundle: invCerts={0} formalCerts={1} "
                    "argCerts={2} slotCerts={3} interactions={4} "
                    "formalConsistency={5} derivations={6} liftSteps={7} "
                    "rootMerges={8} lexicalBridge={9} paste={10} "
                    "wrappers={11} admissible={12} deferred={13} "
                    "bridgedFormals={14} bridgedInteractions={15} "
                    "bridgeSensitiveStructuredSemantics={16}",
                    cert.invocationCertificates.size(),
                    cert.formalCertificates.size(), cert.argCertificates.size(),
                    cert.slotCertificates.size(),
                    cert.interactionCertificates.size(),
                    cert.formalInteractionConsistencies.size(),
                    cert.parentDerivations.size(),
                    cert.structuredLiftCertificates.size(),
                    cert.rootMergeCertificates.size(),
                    cert.usesLexicalBridge ? 1 : 0, cert.touchesPaste ? 1 : 0,
                    cert.hasWrapperSemantics ? 1 : 0,
                    cert.admissibility.valid ? 1 : 0,
                    cert.deferredPasteDischarge.deferredInvocations.size(),
                    cert.bridgedFormalKeys.size(),
                    cert.bridgedInteractionKeys.size(),
                    cert.hasBridgeSensitiveStructuredSemantics ? 1 : 0)
                .str();
        return cert;
      };

      /// Partition the changed leaf formals into independently liftable groups.
      ///
      /// Most changed formals can be lifted independently. Paste changes are the
      /// exception: when multiple formals contribute to the same pasted token,
      /// those formals must be lifted together so the later replay/validation
      /// logic sees the complete paste surface rather than isolated operands.
      ///
      /// This builds an undirected dependency graph over changed leaf formals:
      /// formals are connected when they appear in the same paste product. Each
      /// connected component becomes one lift group.
      auto buildLeafFormalLiftGroups =
          [&](const RefoldModel::MacroInvocation &leaf,
              const DenseMap<uint32_t, FormalTextPair> &leafFormals)
          -> SmallVector<DenseMap<uint32_t, FormalTextPair>, 4> {
        SmallVector<DenseMap<uint32_t, FormalTextPair>, 4> groups;
        if (leafFormals.empty())
          return groups;

        // Use a stable formal order so the resulting groups are deterministic
        // even though the input map is a DenseMap.
        SmallVector<uint32_t, 8> argOrder;
        argOrder.reserve(leafFormals.size());
        for (const auto &kvLocal : leafFormals)
          argOrder.push_back(kvLocal.first);
        llvm::sort(argOrder);

        DenseMap<uint32_t, SmallVector<uint32_t, 4>> adjacency;
        for (uint32_t argIdx : argOrder)
          adjacency[argIdx];

        // Group changed formals by the paste product they contribute to. The
        // key is the final pasted-token span, so all operands of the same paste
        // result land in the same bucket.
        StringMap<SmallVector<uint32_t, 4>> tokenArgs;
        for (const auto &ps : leaf.pasteSpans) {
          auto it = leafFormals.find(ps.argIdx);
          if (it == leafFormals.end())
            continue;

          std::string key = formatv("{0}:{1}", ps.begin, ps.end).str();
          auto &args = tokenArgs[key];
          if (llvm::find(args, ps.argIdx) == args.end())
            args.push_back(ps.argIdx);
        }

        // Add undirected edges between every pair of changed formals that share
        // a paste product. A connected component therefore represents the
        // smallest set of leaf formals that must be replayed together.
        for (const auto &kvLocal : tokenArgs) {
          ArrayRef<uint32_t> args = kvLocal.second;
          if (args.size() < 2)
            continue;
          for (size_t i = 0; i < args.size(); ++i) {
            for (size_t j = i + 1; j < args.size(); ++j) {
              if (llvm::find(adjacency[args[i]], args[j]) ==
                  adjacency[args[i]].end())
                adjacency[args[i]].push_back(args[j]);
              if (llvm::find(adjacency[args[j]], args[i]) ==
                  adjacency[args[j]].end())
                adjacency[args[j]].push_back(args[i]);
            }
          }
        }

        // Emit one formal map per connected component. Isolated changed formals
        // become singleton groups; paste-coupled formals become one shared
        // group so the later lift chain can preserve their joint semantics.
        DenseSet<uint32_t> visited;
        for (uint32_t rootArgIdx : argOrder) {
          if (!visited.insert(rootArgIdx).second)
            continue;

          SmallVector<uint32_t, 8> stack{rootArgIdx};
          DenseMap<uint32_t, FormalTextPair> groupFormals;
          while (!stack.empty()) {
            uint32_t argIdx = stack.pop_back_val();
            auto it = leafFormals.find(argIdx);
            if (it != leafFormals.end())
              groupFormals[argIdx] = it->second;

            auto adjIt = adjacency.find(argIdx);
            if (adjIt == adjacency.end())
              continue;
            for (uint32_t nextArgIdx : adjIt->second) {
              if (visited.insert(nextArgIdx).second)
                stack.push_back(nextArgIdx);
            }
          }

          if (!groupFormals.empty())
            groups.push_back(std::move(groupFormals));
        }

        return groups;
      };

      /// Build a complete rewrite certificate for one edited macro subtree.
      ///
      /// The input `leafEdits` describes edits observed at a leaf invocation.
      /// This routine proves the rewrite in four stages:
      ///
      /// * certify the edited leaf invocation,
      /// * partition leaf formals into independently liftable groups,
      /// * lift each group outward to root formals and merge root rewrites, and
      /// * certify the final root invocation plus the collected subtree
      ///   semantics.
      ///
      /// The returned certificate is `Unique` only if every required semantic
      /// gate is proven.
      /// Otherwise it fails closed with the detail from the first failed proof
      /// obligation.
      auto buildSubtreeRewriteCertificate =
          [&](const RefoldModel::MacroInvocation &leaf,
              const DenseMap<uint32_t, OldNewText> &leafEdits,
              bool deferLeafPasteValidation) -> SubtreeRewriteCertificate {
        SubtreeRewriteCertificate cert;
        cert.leaf = &leaf;
        cert.root = &rootInvocation;

        // Normalize the raw leaf edits into changed formal rewrites. No-change
        // edits are dropped here so later certificates only reason about actual
        // rewrite obligations.
        DenseMap<uint32_t, FormalTextPair> pendingLeafFormals;
        for (const auto &kvLocal : leafEdits) {
          StringRef oldText = StringRef(kvLocal.second.oldText).trim();
          StringRef newText = StringRef(kvLocal.second.newText).trim();
          if (oldText == newText)
            continue;
          pendingLeafFormals[kvLocal.first] =
              FormalTextPair{oldText.str(), newText.str()};
        }

        if (pendingLeafFormals.empty()) {
          cert.kind = SubtreeRewriteCertificateKind::NoChange;
          cert.detail = formatv(
                            "DAG subtree: leaf id={0} name={1} pending leaf "
                            "formals empty",
                            leaf.id, leaf.name)
                            .str();
          return cert;
        }

        // First prove that the edited leaf invocation can be rebuilt from the
        // changed leaf formals. This establishes the inner boundary before any
        // attempt is made to lift the rewrite outward through callers.
        cert.leafCert = buildWrapperPlaceholderHopInvocationCertificate(
            leaf, pendingLeafFormals, "DAG subtree leaf");
        if (cert.leafCert.kind == InvocationRewriteCertificateKind::Invalid) {
          cert.detail = cert.leafCert.detail;
          return cert;
        }
        if (cert.leafCert.kind == InvocationRewriteCertificateKind::NoChange) {
          cert.kind = SubtreeRewriteCertificateKind::NoChange;
          cert.detail =
              formatv("DAG subtree: leaf invocation no-change leaf "
                      "id={0} name={1} pendingLeafFormals={2} detail={3}",
                      leaf.id, leaf.name, pendingLeafFormals.size(),
                      cert.leafCert.detail)
                  .str();
          return cert;
        }

        // Use the leaf certificate's normalized rewrite list as the canonical
        // set of leaf formals to lift. This avoids carrying any raw input edits
        // that the leaf certificate did not accept.
        cert.leafFormals.clear();
        for (const auto &rewrite : cert.leafCert.rewrites) {
          cert.leafFormals[rewrite.argIdx] =
              FormalTextPair{rewrite.oldText, rewrite.newText};
        }

        DenseMap<uint32_t, SmallVector<FormalTextPair, 2>> rootRewrites;
        DenseSet<uint32_t> deferredRootOccurrenceArgIdxSet;

        // Paste-coupled leaf formals must be lifted together, while independent
        // formals can be lifted separately. Each lift group produces zero or
        // more candidate rewrites at the root invocation.
        auto liftGroups = buildLeafFormalLiftGroups(leaf, cert.leafFormals);
        for (const auto &groupLeafFormals : liftGroups) {
          auto liftCert = buildLiftChainCertificate(leaf, groupLeafFormals);
          cert.liftCertificates.push_back(liftCert);
          if (liftCert.kind == LiftChainCertificateKind::Invalid) {
            cert.detail = !liftCert.detail.empty()
                              ? liftCert.detail
                              : formatv("DAG subtree: lift failed root id={0} "
                                        "name={1} leaf id={2} name={3} "
                                        "groupArgs={4}",
                                        rootInvocation.id, rootInvocation.name, leaf.id, leaf.name,
                                        formatUInt32List(liftCert.leafArgIdxs))
                                    .str();
            return cert;
          }

          // Accumulate unique root-formal rewrites from all lift chains. The
          // actual compatibility check is delayed until the root merge
          // certificate so duplicate or overlapping chains are handled in one
          // proof location.
          for (const auto &rk : liftCert.rootFormals) {
            auto &rewrites = rootRewrites[rk.first];
            bool seen = false;
            for (const auto &existing : rewrites) {
              if (existing.oldText == rk.second.oldText &&
                  existing.newText == rk.second.newText) {
                seen = true;
                break;
              }
            }
            if (!seen)
              rewrites.push_back(rk.second);
          }

          // Remember root arguments whose final text came through a lexical
          // bridge. Root replay may need to defer occurrence matching for these
          // arguments until the semantic bundle proves the bridge was consumed
          // exactly.
          for (uint32_t rootArgIdx : liftCert.bridgedRootArgIdxs)
            deferredRootOccurrenceArgIdxSet.insert(rootArgIdx);
        }

        if (rootRewrites.empty()) {
          cert.kind = SubtreeRewriteCertificateKind::NoChange;
          cert.detail =
              formatv("DAG subtree: no lifted root formals root id={0} "
                      "name={1} leaf id={2} name={3} leafCertRewrites={4} "
                      "liftCertificates={5}",
                      rootInvocation.id, rootInvocation.name, leaf.id, leaf.name,
                      cert.leafCert.rewrites.size(),
                      cert.liftCertificates.size())
                  .str();
          return cert;
        }

        // Merge all lifted rewrites per root formal against the original root
        // argument text. This is where independently lifted leaf groups are
        // required to agree before they are allowed to affect the root.
        DenseMap<uint32_t, FormalTextPair> pendingRootFormals;
        for (const auto &kvLocal : rootRewrites) {
          const uint32_t argIdx = kvLocal.first;
          auto mergeCert = buildRootFormalMergeCertificate(
              argIdx, kvLocal.second, "DAG subtree root merge");
          cert.rootMergeCertificates.push_back(mergeCert);
          if (mergeCert.kind == RootFormalMergeCertificateKind::Invalid) {
            cert.detail = mergeCert.detail;
            return cert;
          }
          if (mergeCert.kind == RootFormalMergeCertificateKind::NoChange) {
            pendingRootFormals[argIdx] =
                FormalTextPair{mergeCert.baseArgText, mergeCert.mergedArgText};
            continue;
          }

          pendingRootFormals[argIdx] =
              FormalTextPair{mergeCert.baseArgText, mergeCert.mergedArgText};
        }

        // Convert bridge provenance into the sorted root-argument list consumed
        // by root invocation certification.
        cert.deferRootOccurrenceArgIdxs.clear();
        cert.deferRootOccurrenceArgIdxs.reserve(pendingRootFormals.size());
        for (const auto &kvLocal : pendingRootFormals) {
          if (deferredRootOccurrenceArgIdxSet.contains(kvLocal.first))
            cert.deferRootOccurrenceArgIdxs.push_back(kvLocal.first);
        }
        llvm::sort(cert.deferRootOccurrenceArgIdxs);

        // Prove that the root invocation can be rewritten from the merged root
        // formals. Deferred occurrence arguments tell the root certificate
        // which bridged formal occurrences require semantic discharge later.
        cert.rootCert = buildInvocationRewriteCertificate(
            rootInvocation, pendingRootFormals, "DAG subtree root",
            subtreeValidationCtx.rootInvocationText,
            subtreeValidationCtx.rootInvocationArgRanges,
            cert.deferRootOccurrenceArgIdxs);
        if (cert.rootCert.kind == InvocationRewriteCertificateKind::Invalid) {
          if (cert.rootCert.failure ==
              InvocationRewriteFailure::PasteMismatch) {
            // A plain root rewrite can fail on paste shape even when the root
            // still has a valid wrapper-placeholder replay. Probe that narrower
            // certificate before rejecting the whole subtree.
            auto wrapperProbe = buildWrapperPlaceholderHopInvocationCertificate(
                rootInvocation, pendingRootFormals, "DAG subtree root probe");
            if (wrapperProbe.kind == InvocationRewriteCertificateKind::Unique) {
              cert.rootCert = std::move(wrapperProbe);
            }
          }
          if (cert.rootCert.kind == InvocationRewriteCertificateKind::Invalid) {
            cert.detail = cert.rootCert.detail;
            return cert;
          }
        }
        if (cert.rootCert.kind == InvocationRewriteCertificateKind::NoChange) {
          cert.kind = SubtreeRewriteCertificateKind::NoChange;
          cert.detail =
              formatv("DAG subtree: root invocation no-change root "
                      "id={0} name={1} pendingRootFormals={2} detail={3}",
                      rootInvocation.id, rootInvocation.name, pendingRootFormals.size(),
                      cert.rootCert.detail)
                  .str();
          return cert;
        }

        cert.rootFormals = pendingRootFormals;

        if (inTraceMode()) {
          // Emit a compact proof ledger before the semantic bundle is checked.
          // This makes it easier to distinguish lift/merge/root-cert failures
          // from later subtree semantic admissibility failures.
          SmallVector<uint32_t, 8> rootFormalArgIdxs;
          rootFormalArgIdxs.reserve(cert.rootFormals.size());
          for (const auto &kvLocal : cert.rootFormals)
            rootFormalArgIdxs.push_back(kvLocal.first);
          llvm::sort(rootFormalArgIdxs);
          trace("macro/proof",
                "DAG subtree proof ledger: root id={0} name={1} leaf id={2} "
                "name={3} rootFormals={4} deferredRootArgs={5} liftChains={6} "
                "leafPasteRequired={7} leafPasteDeferred={8} "
                "rootPasteRequired={9} "
                "rootPasteDeferred={10}",
                rootInvocation.id, rootInvocation.name, leaf.id, leaf.name,
                formatUInt32List(rootFormalArgIdxs),
                formatUInt32List(cert.deferRootOccurrenceArgIdxs),
                cert.liftCertificates.size(),
                cert.leafCert.pasteValidation.required ? 1 : 0,
                cert.leafCert.pasteValidation.deferred ? 1 : 0,
                cert.rootCert.pasteValidation.required ? 1 : 0,
                cert.rootCert.pasteValidation.deferred ? 1 : 0);
        }

        // The final semantic certificate checks global consistency conditions
        // that are not local to any single hop: repeated formal evidence,
        // deferred paste discharge, bridge-sensitive semantics, and admissible
        // paste/stringify/wrapper combinations.
        cert.semantic = buildSubtreeSemanticCertificate(
            cert.leafCert, cert.liftCertificates, cert.rootMergeCertificates,
            cert.rootCert);
        if (!cert.semantic.valid) {
          cert.detail = cert.semantic.detail;
          return cert;
        }

        cert.kind = SubtreeRewriteCertificateKind::Unique;
        return cert;
      };

      struct ArgEdit {
        uint64_t begin;
        uint64_t end;
        std::string repl;
      };

      enum class UniformObservedLeafSeedCertificateKind {
        NoChange,
        Unique,
        Invalid,
      };

      enum class UniformObservedLeafSeedFailure {
        None,
        EmptyConstraints,
        DivergentConstraints,
      };

      struct UniformObservedLeafSeedCertificate {
        UniformObservedLeafSeedCertificateKind kind =
            UniformObservedLeafSeedCertificateKind::Invalid;
        UniformObservedLeafSeedFailure failure =
            UniformObservedLeafSeedFailure::None;
        const RefoldModel::MacroInvocation *inv = nullptr;
        uint32_t argIdx = 0;
        std::string oldText;
        std::string newText;
        std::string detail;
      };

      // Nested leaf invocations are recorded in macro-body space, so their
      // invocation text is often placeholder syntax such as STR1(x) or CAT(a,b)
      // rather than source-spelled actual arguments. When all observed leaf
      // constraints for one formal collapse to the same normalized old/new
      // text, certify that exact uniform observed rewrite as the leaf seed and
      // then continue through the structured lift/root-certificate pipeline.
      // This does not accept a root patch by itself; it only certifies the
      // leaf-side semantic rewrite when direct raw leaf-formal certification is
      // unavailable.
      auto buildUniformObservedLeafSeedCertificate =
          [&](const RefoldModel::MacroInvocation &inv, uint32_t argIdx,
              ArrayRef<ObservedFormalConstraint> constraints,
              StringRef traceStage) -> UniformObservedLeafSeedCertificate {
        UniformObservedLeafSeedCertificate cert;
        cert.inv = &inv;
        cert.argIdx = argIdx;

        if (constraints.empty()) {
          cert.failure = UniformObservedLeafSeedFailure::EmptyConstraints;
          cert.detail =
              formatv("{0}: uniform observed leaf seed unavailable: inv "
                      "id={1} name={2} argIdx={3} has no observed "
                      "constraints",
                      traceStage, inv.id, inv.name, argIdx)
                  .str();
          return cert;
        }

        // Uniformity is the proof condition: every observed constraint for this
        // leaf formal must normalize to exactly the same old/new rewrite.
        const StringRef oldTrim = StringRef(constraints[0].oldText).trim();
        const StringRef newTrim = StringRef(constraints[0].newText).trim();
        for (const auto &constraint : constraints) {
          if (StringRef(constraint.oldText).trim() != oldTrim ||
              StringRef(constraint.newText).trim() != newTrim) {
            cert.failure = UniformObservedLeafSeedFailure::DivergentConstraints;
            cert.detail =
                formatv("{0}: uniform observed leaf seed unavailable: "
                        "inv id={1} name={2} argIdx={3} observed "
                        "constraints diverged",
                        traceStage, inv.id, inv.name, argIdx)
                    .str();
            return cert;
          }
        }

        // The resulting seed is only the normalized leaf-side rewrite. Later
        // subtree/lift certificates must still prove how this reaches the root.
        cert.oldText = oldTrim.str();
        cert.newText = newTrim.str();
        if (oldTrim == newTrim) {
          cert.kind = UniformObservedLeafSeedCertificateKind::NoChange;
          cert.detail = formatv("{0}: uniform observed leaf seed collapsed to "
                                "no-change inv id={1} name={2} argIdx={3}",
                                traceStage, inv.id, inv.name, argIdx)
                            .str();
          return cert;
        }

        cert.kind = UniformObservedLeafSeedCertificateKind::Unique;
        cert.detail =
            formatv("{0}: uniform observed leaf seed certified inv id={1} "
                    "name={2} argIdx={3} old='{4}' new='{5}'",
                    traceStage, inv.id, inv.name, argIdx, cert.oldText,
                    cert.newText)
                .str();
        return cert;
      };

      enum class RootPatchConstructionCertificateKind {
        NoChange,
        Unique,
        Invalid,
      };

      enum class RootPatchConstructionFailure {
        None,
        ArgIndexOutOfBounds,
        InvalidArgRange,
        OverlappingEdits,
      };

      struct RootPatchConstructionCertificate {
        RootPatchConstructionCertificateKind kind =
            RootPatchConstructionCertificateKind::Invalid;
        RootPatchConstructionFailure failure =
            RootPatchConstructionFailure::None;
        SmallVector<ArgEdit, 8> edits;
        std::optional<MacroPatch> patch;
        std::string detail;
      };

      enum class DagCandidateAcceptanceFailure {
        None,
        DifferentSpan,
        DifferentBaseText,
        MergeConflict,
        MergedRootValidationFailed,
      };

      struct DagCandidateAcceptanceCertificate {
        bool accepted = false;
        bool merged = false;
        DagCandidateAcceptanceFailure failure =
            DagCandidateAcceptanceFailure::None;
        std::string detail;
      };

      struct DagCandidateValidationMetadata {
        SmallVector<uint32_t, 8> deferOccurrenceArgIdxs;
        DenseMap<uint32_t, FormalTextPair> expectedRootFormals;
        StringMap<SemanticInteractionSignature> bridgeSensitiveFormalSignatures;
        bool hasExpectedRootFormals = false;
        bool hasBridgeSensitiveStructuredSemantics = false;
        bool hasMixedSemanticInteractions = false;
      };

      auto formatBridgeSensitiveFormalSignatureMap =
          [&](const StringMap<SemanticInteractionSignature> &sigs) {
            SmallVector<StringRef, 8> keys;
            keys.reserve(sigs.size());
            for (const auto &kvLocal : sigs)
              keys.push_back(kvLocal.getKey());
            llvm::sort(keys);

            std::string out;
            raw_string_ostream os(out);
            os << "{";
            for (size_t i = 0; i < keys.size(); ++i) {
              if (i)
                os << ", ";
              StringRef key = keys[i];
              const auto it = sigs.find(key);
              os << key << ":(paste=" << (it->second.touchesPaste ? 1 : 0)
                 << ", stringify=" << (it->second.usesStringify ? 1 : 0)
                 << ", wide=" << (it->second.usesWideStringify ? 1 : 0)
                 << ", wrapper=" << (it->second.usesPassthroughFlatten ? 1 : 0)
                 << ", childSyntax="
                 << (it->second.usesPreferredChildSyntax ? 1 : 0) << ", raw="
                 << (it->second.usesRawInvocationPreservation ? 1 : 0) << ")";
            }
            os << "}";
            return os.str();
          };

      // --- Leaf lifting, validation, and uniqueness --------------------------
      //
      // We scan leaf candidates (deepest-first) and attempt to produce a root
      // invocation patch. We accept only if:
      //   * all lifted edits validate against B (occurrence matching), and
      //   * the resulting root patch is unique (no second distinct patch).
      std::optional<MacroPatch> uniquePatch;
      std::optional<std::string> uniquePatchBaseText;
      DagCandidateValidationMetadata uniquePatchValidation;
      unsigned distinctRootPatches = 0;

      /// Mutable DAG-candidate acceptance state shared by the final subtree
      /// merge and validation lambdas.
      ///
      /// The context only borrows the local unique-candidate storage.  It does
      /// not change ownership or selection order; it simply names the proof
      /// state that later helper extraction must receive explicitly.
      struct DagCandidateAcceptanceContext {
        const MacroSubtreeReplayValidationContext &subtreeValidation;
        std::optional<MacroPatch> &uniquePatch;
        std::optional<std::string> &uniquePatchBaseText;
        DagCandidateValidationMetadata &uniquePatchValidation;
        unsigned &distinctRootPatches;
      };

      DagCandidateAcceptanceContext dagCandidateAcceptanceCtx{
          subtreeValidationCtx, uniquePatch, uniquePatchBaseText,
          uniquePatchValidation, distinctRootPatches};

      /// Construct the concrete source patch for an accepted root invocation
      /// rewrite certificate.
      ///
      /// `rootCert` has already proven which root formals should be rewritten.
      /// This lambda turns those formal rewrites into byte-range edits inside
      /// the original root invocation spelling, verifies that the resulting
      /// argument edits are in bounds and non-overlapping, and then
      /// materializes one `MacroPatch` over the full invocation span.
      ///
      /// This is a construction step, not a semantic proof step: it does not
      /// decide whether the rewrite is valid. It only verifies that the
      /// accepted root-certificate rewrites can be represented as a single
      /// well-formed textual patch.
      auto buildRootPatchConstructionCertificate =
          [&](const InvocationRewriteCertificate &rootCert,
              StringRef traceStage) -> RootPatchConstructionCertificate {
        RootPatchConstructionCertificate cert;

        // A root certificate with no effective rewrites does not need a patch.
        if (rootCert.kind == InvocationRewriteCertificateKind::NoChange ||
            rootCert.rewrites.empty()) {
          cert.kind = RootPatchConstructionCertificateKind::NoChange;
          cert.detail =
              formatv("{0}: root patch construction no-op root id={1} "
                      "name={2}",
                      traceStage, rootInvocation.id, rootInvocation.name)
                  .str();
          return cert;
        }

        // Convert each certified root-formal rewrite into an argument-local
        // byte edit within the root invocation text. Range validation happens
        // here because this is the first point where semantic formal indexes
        // become concrete source slices.
        cert.edits.reserve(rootCert.rewrites.size());
        for (const auto &rewrite : rootCert.rewrites) {
          const uint32_t argIdx = rewrite.argIdx;
          if (argIdx >= rootInvocationArgRanges.size()) {
            cert.failure = RootPatchConstructionFailure::ArgIndexOutOfBounds;
            cert.detail =
                formatv("{0}: root patch construction failed root id={1} "
                        "name={2} argIdx={3} out of bounds argCount={4}",
                        traceStage, rootInvocation.id, rootInvocation.name, argIdx, rootInvocationArgRanges.size())
                    .str();
            return cert;
          }

          const uint64_t begin = (uint64_t)rootInvocationArgRanges[argIdx].first;
          const uint64_t end = (uint64_t)rootInvocationArgRanges[argIdx].second;
          if (begin > end || end > (uint64_t)rootInvocationText.size()) {
            cert.failure = RootPatchConstructionFailure::InvalidArgRange;
            cert.detail =
                formatv("{0}: root patch construction failed root id={1} "
                        "name={2} argIdx={3} invalid range=[{4},{5}) "
                        "spanLen={6}",
                        traceStage, rootInvocation.id, rootInvocation.name, argIdx, begin, end,
                        rootInvocationText.size())
                    .str();
            return cert;
          }

          cert.edits.push_back(ArgEdit{begin, end, rewrite.newText});
        }

        // Apply edits in source order and reject overlap. Root formal argument
        // ranges should be disjoint; overlap here indicates malformed range
        // metadata or an invalid patch-construction request.
        llvm::sort(cert.edits, [](const ArgEdit &a, const ArgEdit &b) {
          return a.begin < b.begin;
        });

        uint64_t cur = 0;
        for (const auto &e : cert.edits) {
          if (e.begin < cur || e.end < e.begin) {
            cert.failure = RootPatchConstructionFailure::OverlappingEdits;
            cert.detail =
                formatv("{0}: root patch construction failed root id={1} "
                        "name={2} overlapping edits range=[{3},{4}) "
                        "prevEnd={5}",
                        traceStage, rootInvocation.id, rootInvocation.name, e.begin, e.end, cur)
                    .str();
            return cert;
          }
          cur = e.end;
        }

        // Materialize the replacement for the entire root invocation by copying
        // untouched text between argument edits and substituting each certified
        // new argument spelling at its original argument range. At the same time,
        // remember the output-side subrange occupied by the substituted root
        // argument payloads. The physical patch still covers the full invocation,
        // but the optional edit map should point at the root formal text that
        // actually represents the B-side materialized hunk.
        std::string replText;
        replText.reserve(rootInvocationText.size());
        std::optional<uint64_t> materializedOutputBegin;
        std::optional<uint64_t> materializedOutputEnd;
        cur = 0;
        for (const auto &e : cert.edits) {
          auto mid = rootInvocationText.slice((size_t)cur, (size_t)e.begin);
          replText.append(mid.begin(), mid.end());
          const uint64_t replBegin = static_cast<uint64_t>(replText.size());
          replText.append(e.repl);
          const uint64_t replEnd = static_cast<uint64_t>(replText.size());
          materializedOutputBegin = materializedOutputBegin
                                        ? std::min(*materializedOutputBegin,
                                                   replBegin)
                                        : replBegin;
          materializedOutputEnd = materializedOutputEnd
                                      ? std::max(*materializedOutputEnd, replEnd)
                                      : replEnd;
          cur = e.end;
        }
        auto tail = rootInvocationText.drop_front((size_t)cur);
        replText.append(tail.begin(), tail.end());

        MacroPatch patch{*invStart, *invEnd, std::move(replText), 0};
        if (materializedOutputBegin && materializedOutputEnd) {
          patch.hasMaterializedOutputByteRange = true;
          patch.materializedOutputByteStart = *materializedOutputBegin;
          patch.materializedOutputByteEnd = *materializedOutputEnd;
        }

        // Target-PP identity for a DAG root replay is the union of the exact
        // B-token envelopes for the root formal occurrences that the accepted
        // certificate rewrites.  This is deliberately derived from the
        // producer-recorded PPArgSpan -> B-token mapping, not from the source
        // replacement text, so the resolver can distinguish proof of output
        // identity from a spelling preview.
        std::optional<std::pair<uint64_t, uint64_t>> materializedBTokenRange;
        for (const auto &rewrite : rootCert.rewrites) {
          for (const auto &span : rootInvocation.argSpans) {
            if (span.kind != PPArgSpanKind::Standard ||
                span.argIdx != rewrite.argIdx || span.begin >= span.end)
              continue;

            std::optional<std::pair<size_t, size_t>> bEnv =
                (*deps_.sourceMapper).MapAToBTokenEnvelopeByPPArgSpan(span);
            if (!bEnv || bEnv->second <= bEnv->first ||
                bEnv->second > deps_.bToks.size())
              continue;

            const uint64_t bBegin = static_cast<uint64_t>(bEnv->first);
            const uint64_t bEnd = static_cast<uint64_t>(bEnv->second);
            if (!materializedBTokenRange) {
              materializedBTokenRange = std::make_pair(bBegin, bEnd);
            } else {
              materializedBTokenRange->first =
                  std::min(materializedBTokenRange->first, bBegin);
              materializedBTokenRange->second =
                  std::max(materializedBTokenRange->second, bEnd);
            }
          }
        }
        if (materializedBTokenRange) {
          patch.hasMaterializedBTokenRange = true;
          patch.materializedBTokStart = materializedBTokenRange->first;
          patch.materializedBTokEnd = materializedBTokenRange->second;
        } else if (std::optional<WholeCoverPlan> plan =
                       ComputeWholeCoverPlan(m)) {
          // A DAG subtree-root repair may rewrite a root actual whose
          // final B-side tokens are produced only by descendant generated
          // callee/stringify/paste expansions.  Such roots have no direct
          // Standard PPArgSpan -> B-token envelope for the rewritten formal,
          // but the already-validated subtree/root replay proves that the
          // preserved root invocation materializes this owner-closed macro
          // expansion.  Use the producer-derived whole-cover B envelope as
          // target-PP proof; never derive target identity from replacement
          // source text.
          patch.hasMaterializedBTokenRange = true;
          patch.materializedBTokStart = static_cast<uint64_t>(plan->bTokStart);
          patch.materializedBTokEnd = static_cast<uint64_t>(plan->bTokEnd);
        }

        cert.patch = std::move(patch);
        cert.kind = RootPatchConstructionCertificateKind::Unique;
        cert.detail =
            formatv("{0}: root patch construction succeeded root id={1} "
                    "name={2} edits={3} replLen={4}",
                    traceStage, rootInvocation.id, rootInvocation.name, cert.edits.size(),
                    cert.patch ? cert.patch->replacement.size() : 0)
                .str();
        return cert;
      };

      /// Merge validation metadata from two DAG candidates that are being
      /// composed into one candidate.
      ///
      /// The metadata records proof obligations that must remain consistent
      /// across the composed candidate: bridge-sensitive semantic signatures,
      /// deferred root occurrence arguments, and the expected root-formal
      /// rewrites. The merge succeeds only when duplicate evidence agrees and
      /// independently produced root-formal rewrites are compatible against the
      /// original root argument text.
      auto mergeDagCandidateValidationMetadata =
          [&](const DagCandidateValidationMetadata &lhs,
              const DagCandidateValidationMetadata &rhs)
          -> std::optional<DagCandidateValidationMetadata> {
        DagCandidateValidationMetadata merged;

        // Boolean hazards compose by union: if either side observed a semantic
        // condition that requires later validation, the merged candidate must
        // carry that condition forward.
        merged.hasBridgeSensitiveStructuredSemantics =
            lhs.hasBridgeSensitiveStructuredSemantics ||
            rhs.hasBridgeSensitiveStructuredSemantics;
        merged.hasMixedSemanticInteractions =
            lhs.hasMixedSemanticInteractions ||
            rhs.hasMixedSemanticInteractions;

        // Bridge-sensitive formal signatures are keyed by logical formal. The
        // same formal may appear in both candidates, but only with identical
        // semantic evidence; divergent signatures mean the candidates cannot be
        // soundly composed.
        for (const auto &kvLocal : lhs.bridgeSensitiveFormalSignatures)
          merged.bridgeSensitiveFormalSignatures[kvLocal.getKey()] = kvLocal.getValue();
        for (const auto &kvLocal : rhs.bridgeSensitiveFormalSignatures) {
          auto it = merged.bridgeSensitiveFormalSignatures.find(kvLocal.getKey());
          if (it == merged.bridgeSensitiveFormalSignatures.end()) {
            merged.bridgeSensitiveFormalSignatures[kvLocal.getKey()] = kvLocal.getValue();
            continue;
          }
          if (!(it->second == kvLocal.getValue()))
            return std::nullopt;
        }

        // Deferred occurrence arguments also compose by set union. Keep the
        // resulting vector sorted so downstream diagnostics and comparisons are
        // deterministic.
        auto addDeferredArgIdxs = [&](ArrayRef<uint32_t> argIdxs) {
          for (uint32_t argIdx : argIdxs) {
            if (!llvm::is_contained(merged.deferOccurrenceArgIdxs, argIdx))
              merged.deferOccurrenceArgIdxs.push_back(argIdx);
          }
        };
        addDeferredArgIdxs(lhs.deferOccurrenceArgIdxs);
        addDeferredArgIdxs(rhs.deferOccurrenceArgIdxs);
        llvm::sort(merged.deferOccurrenceArgIdxs);

        if (!lhs.hasExpectedRootFormals && !rhs.hasExpectedRootFormals)
          return merged;

        // Collect expected root-formal rewrites from both candidates by root
        // argument. Compatibility is checked per argument against the original
        // root spelling below.
        DenseMap<uint32_t, SmallVector<FormalTextPair, 2>> rewritesByArg;
        auto collect = [&](const DagCandidateValidationMetadata &meta) {
          if (!meta.hasExpectedRootFormals)
            return;
          for (const auto &kvLocal : meta.expectedRootFormals)
            rewritesByArg[kvLocal.first].push_back(kvLocal.second);
        };
        collect(lhs);
        collect(rhs);

        for (const auto &kvLocal : rewritesByArg) {
          const uint32_t argIdx = kvLocal.first;
          if (argIdx >= rootInvocationArgRanges.size())
            return std::nullopt;

          const size_t begin = rootInvocationArgRanges[argIdx].first;
          const size_t end = rootInvocationArgRanges[argIdx].second;
          if (begin > end || end > rootInvocationText.size())
            return std::nullopt;

          // The merge is anchored to the original root argument text. This
          // prevents two candidates from overwriting each other unless their
          // proposed rewrites are mutually compatible with the same base text.
          const StringRef baseArgText = rootInvocationText.slice(begin, end).trim();
          auto mergedArgText =
              mergeCompatibleFormalRewrites(baseArgText, kvLocal.second);
          if (!mergedArgText)
            return std::nullopt;
          if (StringRef(*mergedArgText).trim() == baseArgText) {
            REFOLD_LOG_TRACE("macro/proof",
                  "merge DAG validation metadata root id={0} name={1} "
                  "argIdx={2} collapsed to base text base='{3}' variants={4}",
                  rootInvocation.id, rootInvocation.name, argIdx,
                  stringutils::showWsWithClip(baseArgText, 120),
                  kvLocal.second.size());
            continue;
          }

          merged.expectedRootFormals[argIdx] = FormalTextPair{
              baseArgText.str(), StringRef(*mergedArgText).trim().str()};
        }

        merged.hasExpectedRootFormals = true;
        return merged;
      };

      struct InvocationHeadShape {
        std::string callee;
        size_t argCount = 0;
      };

      /// Extract the callee spelling and argument count from a text fragment
      /// that must parse as a macro invocation.
      ///
      /// This intentionally records only the invocation head shape, not the
      /// full argument contents. Callers use it to check whether a rewrite
      /// preserves the same outer invocation boundary while allowing the
      /// argument text itself to change.
      auto getInvocationHeadShape =
          [&](StringRef text) -> std::optional<InvocationHeadShape> {
        StringRef trimmed = text.trim();
        auto argRangesOpt =
            RefoldArgTextRecovery::LexMacroInvocationActualContentRanges(trimmed,
                                                                        (*deps_.lexLang));
        if (!argRangesOpt)
          return std::nullopt;

        size_t open = trimmed.find('(');
        if (open == StringRef::npos)
          return std::nullopt;

        StringRef callee = trimmed.take_front(open).trim();
        if (callee.empty())
          return std::nullopt;

        return InvocationHeadShape{callee.str(), argRangesOpt->size()};
      };

      /// Return true when a formal rewrite preserves the same outer macro
      /// invocation head.
      ///
      /// This is a structural guard for rewrites that may change the contents
      /// of an invocation argument but must not silently replace the callee or
      /// alter the arity of the invocation being preserved.
      auto preservesRootInvocationHead =
          [&](const FormalTextPair &rewrite) -> bool {
        auto oldShape = getInvocationHeadShape(rewrite.oldText);
        auto newShape = getInvocationHeadShape(rewrite.newText);
        if (!oldShape || !newShape)
          return false;
        return oldShape->callee == newShape->callee &&
               oldShape->argCount == newShape->argCount;
      };

      /// Count how many nested macro invocation heads are preserved by a
      /// rewrite.
      ///
      /// The score increases by one for the current invocation when the old and
      /// new text have the same callee and arity, then recurses into matching
      /// argument positions to count preserved nested invocation heads. A score
      /// of zero means the fragment does not preserve the outer invocation
      /// shape and therefore cannot contribute structural-preservation credit.
      std::function<unsigned(StringRef, StringRef)>
          countPreservedInvocationHeads =
              [&](StringRef oldText, StringRef newText) -> unsigned {
        auto oldShape = getInvocationHeadShape(oldText);
        auto newShape = getInvocationHeadShape(newText);
        if (!oldShape || !newShape)
          return 0;
        if (oldShape->callee != newShape->callee ||
            oldShape->argCount != newShape->argCount)
          return 0;

        auto oldArgRangesOpt =
            RefoldArgTextRecovery::LexMacroInvocationActualContentRanges(
                oldText.trim(), (*deps_.lexLang));
        auto newArgRangesOpt =
            RefoldArgTextRecovery::LexMacroInvocationActualContentRanges(
                newText.trim(), (*deps_.lexLang));
        if (!oldArgRangesOpt || !newArgRangesOpt ||
            oldArgRangesOpt->size() != newArgRangesOpt->size())
          return 0;

        // The outer invocation head is preserved. Recurse positionally through
        // corresponding argument slices to count any nested invocation heads
        // that are also preserved by the rewrite.
        unsigned score = 1;
        for (size_t i = 0; i < oldArgRangesOpt->size(); ++i) {
          const auto &oldArgRange = (*oldArgRangesOpt)[i];
          const auto &newArgRange = (*newArgRangesOpt)[i];
          score += countPreservedInvocationHeads(
              oldText.trim().slice((size_t)oldArgRange.first,
                                   (size_t)oldArgRange.second),
              newText.trim().slice((size_t)newArgRange.first,
                                   (size_t)newArgRange.second));
        }
        return score;
      };

      /// Choose between two otherwise-compatible structured DAG candidates when
      /// one preserves more root invocation structure than the other.
      ///
      /// Returns:
      ///
      /// * `-1` when the existing candidate should remain preferred,
      /// * `1` when the new candidate should replace it, and
      /// * `0` when this comparison cannot safely distinguish them.
      ///
      /// This is intentionally conservative. It only makes a preference when
      /// both candidates rewrite the same root formals from the same original
      /// text and one candidate has strictly stronger invocation-head
      /// preservation evidence. If the candidates disagree in any non-ordered
      /// way, the caller must treat them as not comparable here.
      auto choosePreferredStructuredDagCandidate =
          [&](const DagCandidateValidationMetadata &existingValidation,
              const DagCandidateValidationMetadata &candidateValidation)
          -> int {
        if (!existingValidation.hasExpectedRootFormals ||
            !candidateValidation.hasExpectedRootFormals)
          return 0;

        if (existingValidation.expectedRootFormals.size() !=
            candidateValidation.expectedRootFormals.size())
          return 0;

        bool existingPreferred = false;
        bool candidatePreferred = false;

        // Compare candidates formal-by-formal. The comparison is only valid if
        // both candidates cover exactly the same root formals and start from
        // the same base argument text for each formal.
        for (const auto &kvLocal : existingValidation.expectedRootFormals) {
          auto it = candidateValidation.expectedRootFormals.find(kvLocal.first);
          if (it == candidateValidation.expectedRootFormals.end())
            return 0;

          const FormalTextPair &existingRewrite = kvLocal.second;
          const FormalTextPair &candidateRewrite = it->second;
          if (StringRef(existingRewrite.oldText).trim() !=
              StringRef(candidateRewrite.oldText).trim())
            return 0;

          const bool existingPreserves =
              preservesRootInvocationHead(existingRewrite);
          const bool candidatePreserves =
              preservesRootInvocationHead(candidateRewrite);
          const unsigned existingStructureScore = countPreservedInvocationHeads(
              existingRewrite.oldText, existingRewrite.newText);
          const unsigned candidateStructureScore =
              countPreservedInvocationHeads(candidateRewrite.oldText,
                                            candidateRewrite.newText);

          // Prefer the candidate that preserves more nested invocation heads.
          // This gives deeper structure preservation priority over the weaker
          // outer-head-only predicate below.
          if (existingStructureScore != candidateStructureScore) {
            if (existingStructureScore > candidateStructureScore)
              existingPreferred = true;
            if (candidateStructureScore > existingStructureScore)
              candidatePreferred = true;
            continue;
          }

          if (existingPreserves == candidatePreserves) {
            if (StringRef(existingRewrite.newText).trim() !=
                StringRef(candidateRewrite.newText).trim())
              return 0;
            continue;
          }

          // If the recursive score ties, use outer root-invocation-head
          // preservation as the final structural preference signal.
          if (existingPreserves)
            existingPreferred = true;
          if (candidatePreserves)
            candidatePreferred = true;
        }

        if (existingPreferred == candidatePreferred)
          return 0;
        return existingPreferred ? -1 : 1;
      };

      struct RootProofValidationCertificate {
        bool valid = false;
        DenseMap<uint32_t, FormalTextPair> replayRootFormals;
        InvocationRewriteCertificate replayInvocationCertificate;
        std::string detail;
      };

      // Wrapper-placeholder replay is used only after ordinary paste replay has
      // failed.  In that mode the candidate text proves that one observed paste
      // chain can be reconstructed, but it does not by itself prove that changing
      // the root formal is safe for every other descendant use of that formal.
      //
      // A root formal may feed several pasted selectors below the same macro
      // invocation.  Some of those pasted tokens are emitted directly and have
      // paste spans; others are immediately consumed as macro names and therefore
      // have only paste-token witnesses plus the expansion span of the selected
      // macro.  If such an untargeted selector would change under replay, the
      // refolded source can preprocess to a different program even though the
      // single edited pasted token was explained.  Reject the deferred wrapper
      // replay unless every descendant output range controlled
      // by each changed root formal is covered by the current token diff.
      // Hidden paste-derived macro selectors are rejected because the current
      // map only observed the old selected macro body.
      auto rootDeferredPasteReplayHasOnlyProvenDependentUses =
          [&](const DenseMap<uint32_t, FormalTextPair> &rootFormals,
              StringRef traceStage, std::string &failureDetail) -> bool {
        struct DependentUse {
          const RefoldModel::MacroInvocation *inv = nullptr;
          uint32_t argIdx = 0;
          const char *kind = "";
          uint64_t begin = 0;
          uint64_t end = 0;
          bool hiddenPasteSelector = false;
        };

        auto tokenRangeTouchesCurrentDiff = [&](uint64_t begin,
                                                uint64_t end) -> bool {
          if (begin >= end)
            return false;
          for (const auto &h : tokenHunksForCheck) {
            if (h.aStart == h.aEnd) {
              // Pure insertions belong to the adjacent dependent surface when
              // the insertion point is exactly inside or on either boundary.
              if (h.aStart >= begin && h.aStart <= end)
                return true;
              continue;
            }
            if (h.aStart < end && h.aEnd > begin)
              return true;
          }
          return false;
        };

        auto addArgSpanUses = [&](SmallVectorImpl<DependentUse> &uses,
                                  const RefoldModel::MacroInvocation &inv,
                                  uint32_t argIdx, const char *kind,
                                  ArrayRef<RefoldModel::PPArgSpan> spans) {
          for (const auto &sp : spans) {
            if (sp.argIdx != argIdx || !sp.IsValid())
              continue;
            uses.push_back(
                DependentUse{&inv, argIdx, kind, sp.begin, sp.end, false});
          }
        };

        auto pasteTokenUsesFormal =
            [&](const RefoldModel::MacroInvocation &inv, uint32_t argIdx) {
          for (const auto &tok : inv.pasteTokens) {
            for (const auto &part : tok.parts) {
              if (part.kind == RefoldModel::PastePartKind::Arg &&
                  part.argIndex && *part.argIndex == argIdx)
                return true;
            }
          }
          return false;
        };

        auto hasFinalPasteSpanForRange =
            [&](const RefoldModel::MacroInvocation &inv, uint32_t argIdx,
                uint64_t begin, uint64_t end) {
          for (const auto &ps : inv.pasteSpans) {
            if (ps.argIdx == argIdx && ps.begin == begin && ps.end == end)
              return true;
          }
          return false;
        };

        auto addPasteTokenUses =
            [&](SmallVectorImpl<DependentUse> &uses,
                const RefoldModel::MacroInvocation &inv, uint32_t argIdx) {
          auto addOne = [&](const RefoldModel::PPSpan &sp) {
            if (!sp.IsValid())
              return;
            const bool hasFinalPasteSpan =
                hasFinalPasteSpanForRange(inv, argIdx, sp.begin, sp.end);
            uses.push_back(DependentUse{&inv, argIdx,
                                        hasFinalPasteSpan
                                            ? "paste-token"
                                            : "hidden-paste-selector",
                                        sp.begin, sp.end, !hasFinalPasteSpan});
          };

          if (!inv.spans.empty()) {
            for (const auto &sp : inv.spans)
              addOne(sp);
          } else {
            for (const auto &sp : inv.bodySpans)
              addOne(sp);
          }
        };

        auto formatDependentUse = [&](const DependentUse &use) {
          return formatv("{{id={0},name={1},arg={2},kind={3},A=[{4},{5}),"
                         "touched={6},hiddenSelector={7}}}",
                         use.inv ? use.inv->id : 0,
                         use.inv ? use.inv->name : StringRef(""), use.argIdx,
                         use.kind, use.begin, use.end,
                         tokenRangeTouchesCurrentDiff(use.begin, use.end) ? 1
                                                                         : 0,
                         use.hiddenPasteSelector ? 1 : 0)
              .str();
        };

        for (const auto &kvLocal : rootFormals) {
          StringRef oldText = StringRef(kvLocal.second.oldText).trim();
          StringRef newText = StringRef(kvLocal.second.newText).trim();
          if (oldText == newText)
            continue;

          SmallVector<DependentUse, 32> uses;
          std::set<std::pair<uint64_t, uint32_t>> visited;
          SmallVector<std::pair<const RefoldModel::MacroInvocation *, uint32_t>,
                      32>
              work;
          work.push_back({&m, kvLocal.first});

          while (!work.empty()) {
            auto [cur, curFormal] = work.pop_back_val();
            if (!cur)
              continue;
            if (!visited.insert({cur->id, curFormal}).second)
              continue;

            // Direct final-output occurrences of this formal must be covered by
            // the same diff that justified the root rewrite.
            addArgSpanUses(uses, *cur, curFormal, "standard", cur->argSpans);
            addArgSpanUses(uses, *cur, curFormal, "stringify",
                           cur->stringifySpans);
            addArgSpanUses(uses, *cur, curFormal, "paste-span",
                           cur->pasteSpans);

            if (pasteTokenUsesFormal(*cur, curFormal)) {
              // A paste token may be consumed immediately as a macro selector.
              // Such hidden selectors have no final paste span, so replaying a
              // different root formal would select a macro body that this map did
              // not observe.  Record them distinctly and reject below rather
              // than pretending the wrapper replay proved their expansion.
              addPasteTokenUses(uses, *cur, curFormal);
            }

            ArrayRef<const RefoldModel::MacroInvocation *> children =
                (*deps_.macroTopology).MacroChildrenOf(cur->id);
            if (children.empty())
              continue;

            for (const auto *child : children) {
              if (!child)
                continue;
              for (uint32_t childFormal = 0;
                   childFormal < child->argDeps.size(); ++childFormal) {
                if (llvm::is_contained(child->argDeps[childFormal],
                                       curFormal))
                  work.push_back({child, childFormal});
              }
            }
          }

          SmallVector<std::string, 16> rejectedUses;
          for (const auto &use : uses) {
            if (use.hiddenPasteSelector ||
                !tokenRangeTouchesCurrentDiff(use.begin, use.end))
              rejectedUses.push_back(formatDependentUse(use));
          }

          if (inTraceMode()) {
            SmallVector<std::string, 16> formattedUses;
            formattedUses.reserve(uses.size());
            for (const auto &use : uses)
              formattedUses.push_back(formatDependentUse(use));
            trace("macro/proof",
                  "{0}: root deferred paste replay dependent-use coverage "
                  "root id={1} name={2} argIdx={3} old='{4}' new='{5}' "
                  "uses={6} rejected={7}",
                  traceStage, rootInvocation.id, rootInvocation.name, kvLocal.first,
                  stringutils::showWsWithClip(oldText, 120),
                  stringutils::showWsWithClip(newText, 120),
                  llvm::join(formattedUses, ", "),
                  llvm::join(rejectedUses, ", "));
          }

          if (!rejectedUses.empty()) {
            failureDetail =
                formatv("{0}: root proof validation rejected deferred paste "
                        "wrapper replay root id={1} name='{2}' argIdx={3} "
                        "because changed root formal has unproven "
                        "dependent output uses: {4}",
                        traceStage, rootInvocation.id, rootInvocation.name, kvLocal.first,
                        llvm::join(rejectedUses, ", "))
                    .str();
            return false;
          }
        }

        return true;
      };

      /// Re-validate a constructed root replacement against the root-invocation
      /// proof machinery.
      ///
      /// This is the final replay check for a DAG candidate after some earlier
      /// caller has proposed replacing `baseText` with `newText`. It derives the
      /// root-formal rewrite map from the concrete callsite replacement, checks
      /// that it matches the expected root-formal proof metadata when provided,
      /// and then rebuilds an invocation rewrite certificate from the replayed
      /// formals.
      ///
      /// The candidate is accepted only if the concrete replacement can be
      /// explained by the same root-formal rewrites that the structured DAG
      /// proof expected. This prevents a textually plausible replacement from
      /// bypassing the formal/root certificate chain.
      auto buildRootProofValidationCertificate =
          [&](StringRef baseText, StringRef newText,
              ArrayRef<uint32_t> deferOccurrenceArgIdxs,
              const DenseMap<uint32_t, FormalTextPair> *expectedRootFormals,
              StringRef traceStage) -> RootProofValidationCertificate {
        RootProofValidationCertificate cert;

        // Identical root text needs no replay proof beyond the no-op witness.
        if (baseText == newText) {
          cert.valid = true;
          cert.detail = formatv("{0}: root proof validation no-op root id={1} "
                                "name='{2}'",
                                traceStage, rootInvocation.id, rootInvocation.name)
                            .str();
          return cert;
        }

        // Recover the formal rewrite map implied by the concrete callsite text.
        // If this fails, the replacement cannot be tied back to root arguments.
        auto replayRootFormals =
            buildRootFormalRewriteMapFromCallsiteReplacement(baseText, newText);
        if (!replayRootFormals) {
          cert.detail =
              formatv("{0}: root proof validation failed root id={1} "
                      "name='{2}' could not derive replay root-formal "
                      "rewrite map baseLen={3} newLen={4}",
                      traceStage, rootInvocation.id, rootInvocation.name, baseText.size(), newText.size())
                  .str();
          return cert;
        }

        if (expectedRootFormals) {
          // Some expected formals are support-only no-change entries used to
          // preserve proof context. They may not appear in the replay-derived
          // changed-formal map, so allow them only when the concrete old/new
          // argument text is unchanged and exactly matches the expected pair.
          auto concreteArgMatchesExpectedUnchanged =
              [&](uint32_t argIdx, const FormalTextPair &expected) -> bool {
            if (argIdx >= rootInvocationArgRanges.size())
              return false;

            auto newRangesOpt =
                GetMacroInvocationFormalArgContentRanges(m, newText);
            if (!newRangesOpt || argIdx >= newRangesOpt->size())
              return false;

            const auto &oldR = rootInvocationArgRanges[argIdx];
            const auto &newR = (*newRangesOpt)[argIdx];
            if (oldR.first > oldR.second || oldR.second > baseText.size() ||
                newR.first > newR.second || newR.second > newText.size())
              return false;

            StringRef concreteOld =
                baseText.slice((size_t)oldR.first, (size_t)oldR.second).trim();
            StringRef concreteNew =
                newText.slice((size_t)newR.first, (size_t)newR.second).trim();
            StringRef expectedOld = StringRef(expected.oldText).trim();
            StringRef expectedNew = StringRef(expected.newText).trim();
            return concreteOld == concreteNew && concreteOld == expectedOld &&
                   concreteNew == expectedNew;
          };

          SmallVector<uint32_t, 8> replayAugmentedSupportOnlyArgIdxs;
          const bool traceRootProofValidation = inTraceMode();
          for (const auto &kvLocal : *expectedRootFormals) {
            if (replayRootFormals->contains(kvLocal.first))
              continue;
            if (StringRef(kvLocal.second.oldText).trim() !=
                StringRef(kvLocal.second.newText).trim())
              continue;
            if (!concreteArgMatchesExpectedUnchanged(kvLocal.first, kvLocal.second))
              continue;
            (*replayRootFormals)[kvLocal.first] = kvLocal.second;
            if (traceRootProofValidation)
              replayAugmentedSupportOnlyArgIdxs.push_back(kvLocal.first);
          }

          if (traceRootProofValidation) {
            llvm::sort(replayAugmentedSupportOnlyArgIdxs);
            trace("macro/proof",
                  "{0}: root proof replay-vs-expected root id={1} name={2} "
                  "replay={3} expected={4} augmentedSupportOnly={5}",
                  traceStage, rootInvocation.id, rootInvocation.name,
                  formatFormalTextPairMap(*replayRootFormals),
                  formatFormalTextPairMap(*expectedRootFormals),
                  formatUInt32List(replayAugmentedSupportOnlyArgIdxs));

            auto newRangesOpt =
                GetMacroInvocationFormalArgContentRanges(m, newText);
            SmallVector<uint32_t, 8> missingExpectedArgIdxs;
            SmallVector<uint32_t, 8> unchangedConcreteMissingArgIdxs;
            SmallVector<uint32_t, 8> supportOnlyMissingArgIdxs;
            SmallVector<uint32_t, 8> mismatchedExpectedArgIdxs;
            SmallVector<uint32_t, 8> unexpectedReplayArgIdxs;

            // Build a detailed mismatch ledger before the hard equality checks.
            // These traces make it clear whether failure came from missing
            // support-only formals, actual rewrite mismatches, or unexpected
            // replay-derived formals.
            for (const auto &kvLocal : *expectedRootFormals) {
              auto it = replayRootFormals->find(kvLocal.first);
              if (it == replayRootFormals->end()) {
                missingExpectedArgIdxs.push_back(kvLocal.first);
                if (kvLocal.second.oldText == kvLocal.second.newText)
                  supportOnlyMissingArgIdxs.push_back(kvLocal.first);

                if (newRangesOpt && kvLocal.first < rootInvocationArgRanges.size() &&
                    kvLocal.first < newRangesOpt->size()) {
                  const auto &oldR = rootInvocationArgRanges[kvLocal.first];
                  const auto &newR = (*newRangesOpt)[kvLocal.first];
                  if (oldR.first <= oldR.second &&
                      oldR.second <= baseText.size() &&
                      newR.first <= newR.second &&
                      newR.second <= newText.size()) {
                    StringRef concreteOld =
                        baseText.slice((size_t)oldR.first, (size_t)oldR.second)
                            .trim();
                    StringRef concreteNew =
                        newText.slice((size_t)newR.first, (size_t)newR.second)
                            .trim();
                    if (concreteOld == concreteNew &&
                        concreteOld == StringRef(kvLocal.second.oldText).trim() &&
                        concreteNew == StringRef(kvLocal.second.newText).trim()) {
                      unchangedConcreteMissingArgIdxs.push_back(kvLocal.first);
                    }
                    trace("macro/proof",
                          "{0}: root proof missing expected arg root id={1} "
                          "name={2} argIdx={3} concreteOld='{4}' "
                          "concreteNew='{5}' expectedOld='{6}' "
                          "expectedNew='{7}'",
                          traceStage, rootInvocation.id, rootInvocation.name, kvLocal.first,
                          stringutils::showWsWithClip(concreteOld, 120),
                          stringutils::showWsWithClip(concreteNew, 120),
                          stringutils::showWsWithClip(kvLocal.second.oldText, 120),
                          stringutils::showWsWithClip(kvLocal.second.newText, 120));
                  }
                }
                continue;
              }

              if (it->second.oldText != kvLocal.second.oldText ||
                  it->second.newText != kvLocal.second.newText) {
                mismatchedExpectedArgIdxs.push_back(kvLocal.first);
                trace("macro/proof",
                      "{0}: root proof mismatched expected arg root id={1} "
                      "name={2} argIdx={3} derivedOld='{4}' derivedNew='{5}' "
                      "expectedOld='{6}' expectedNew='{7}'",
                      traceStage, rootInvocation.id, rootInvocation.name, kvLocal.first,
                      stringutils::showWsWithClip(it->second.oldText, 120),
                      stringutils::showWsWithClip(it->second.newText, 120),
                      stringutils::showWsWithClip(kvLocal.second.oldText, 120),
                      stringutils::showWsWithClip(kvLocal.second.newText, 120));
              }
            }

            for (const auto &kvLocal : *replayRootFormals) {
              if (!expectedRootFormals->contains(kvLocal.first))
                unexpectedReplayArgIdxs.push_back(kvLocal.first);
            }

            llvm::sort(missingExpectedArgIdxs);
            llvm::sort(unchangedConcreteMissingArgIdxs);
            llvm::sort(supportOnlyMissingArgIdxs);
            llvm::sort(mismatchedExpectedArgIdxs);
            llvm::sort(unexpectedReplayArgIdxs);
            trace("macro/proof",
                  "{0}: root proof mismatch analysis root id={1} name={2} "
                  "missingExpectedArgs={3} unchangedConcreteMissingArgs={4} "
                  "supportOnlyMissingArgs={5} mismatchedExpectedArgs={6} "
                  "unexpectedReplayArgs={7}",
                  traceStage, rootInvocation.id, rootInvocation.name,
                  formatUInt32List(missingExpectedArgIdxs),
                  formatUInt32List(unchangedConcreteMissingArgIdxs),
                  formatUInt32List(supportOnlyMissingArgIdxs),
                  formatUInt32List(mismatchedExpectedArgIdxs),
                  formatUInt32List(unexpectedReplayArgIdxs));
          }

          // From this point on, replay and expected metadata must be exactly
          // the same root-formal proof set. The diagnostics above explain any
          // mismatch; these checks enforce the invariant.
          if (replayRootFormals->size() != expectedRootFormals->size()) {
            cert.detail =
                formatv("{0}: root proof validation failed root id={1} "
                        "name='{2}' replay-derived root formal count "
                        "mismatch derived={3} expected={4}",
                        traceStage, rootInvocation.id, rootInvocation.name, replayRootFormals->size(),
                        expectedRootFormals->size())
                    .str();
            return cert;
          }

          for (const auto &kvLocal : *expectedRootFormals) {
            auto it = replayRootFormals->find(kvLocal.first);
            if (it == replayRootFormals->end() ||
                it->second.oldText != kvLocal.second.oldText ||
                it->second.newText != kvLocal.second.newText) {
              cert.detail =
                  formatv("{0}: root proof validation failed root "
                          "id={1} name='{2}' replay-derived root "
                          "formal mismatch argIdx={3} derivedOld='{4}' "
                          "derivedNew='{5}' expectedOld='{6}' "
                          "expectedNew='{7}'",
                          traceStage, rootInvocation.id, rootInvocation.name, kvLocal.first,
                          it == replayRootFormals->end()
                              ? StringRef("")
                              : StringRef(it->second.oldText),
                          it == replayRootFormals->end()
                              ? StringRef("")
                              : StringRef(it->second.newText),
                          kvLocal.second.oldText, kvLocal.second.newText)
                      .str();
              return cert;
            }
          }
        }

        // Re-run the normal root invocation certificate on the replay-derived
        // formals. This ensures the concrete replacement is accepted by the
        // same root proof rules as an ordinary structured root rewrite.
        cert.replayInvocationCertificate = buildInvocationRewriteCertificate(
            m, *replayRootFormals, traceStage, baseText, invArgRanges,
            deferOccurrenceArgIdxs);
        if (cert.replayInvocationCertificate.kind ==
            InvocationRewriteCertificateKind::Invalid) {
          if (cert.replayInvocationCertificate.failure ==
              InvocationRewriteFailure::PasteMismatch) {
            // If the plain replay fails only on paste shape, try the narrower
            // wrapper-placeholder replay. Accept it only when it reconstructs
            // exactly the concrete replacement text being validated.
            auto wrapperReplayCert =
                buildWrapperPlaceholderHopInvocationCertificate(
                    m, *replayRootFormals, traceStage, baseText, invArgRanges,
                    deferOccurrenceArgIdxs);
            if (wrapperReplayCert.kind ==
                    InvocationRewriteCertificateKind::Unique &&
                !wrapperReplayCert.rewrittenInvocationSyntax.empty() &&
                StringRef(wrapperReplayCert.rewrittenInvocationSyntax).trim() ==
                    newText.trim()) {
              std::string deferredPasteDetail;
              if (rootDeferredPasteReplayHasOnlyProvenDependentUses(
                      *replayRootFormals, traceStage, deferredPasteDetail)) {
                REFOLD_LOG_TRACE("macro/proof",
                      "{0}: root proof validation accepted wrapper replay "
                      "candidate root id={1} name={2} syntax='{3}' "
                      "pasteDeferred={4}",
                      traceStage, rootInvocation.id, rootInvocation.name,
                      wrapperReplayCert.rewrittenInvocationSyntax,
                      wrapperReplayCert.pasteValidation.deferred ? 1 : 0);
                cert.replayInvocationCertificate = std::move(wrapperReplayCert);
              } else {
                REFOLD_LOG_TRACE("macro/proof", "{0}", deferredPasteDetail);
                cert.detail = deferredPasteDetail;
              }
            }
          }
        }
        if (cert.replayInvocationCertificate.kind ==
            InvocationRewriteCertificateKind::Invalid) {
          if (cert.detail.empty())
            cert.detail = cert.replayInvocationCertificate.detail;
          return cert;
        }

        cert.replayRootFormals = std::move(*replayRootFormals);
        cert.valid = true;
        cert.detail =
            formatv("{0}: root proof validation succeeded root id={1} "
                    "name='{2}' replayFormals={3} deferredArgs={4}",
                    traceStage, rootInvocation.id, rootInvocation.name, cert.replayRootFormals.size(),
                    deferOccurrenceArgIdxs.size())
                .str();
        return cert;
      };

      /// Validate that a composed DAG candidate is still backed by the expected
      /// root-level proof metadata.
      ///
      /// Candidate composition can merge several subtree/root contributions
      /// into one textual replacement. This lambda rejects semantic metadata
      /// that is globally inadmissible after merging, then replays the concrete
      /// `baseText -> newText` replacement through the root proof validator.
      ///
      /// Returning `true` means the concrete candidate text is explainable by
      /// the merged expected root-formal rewrites and by a valid root
      /// invocation certificate. Returning `false` means the candidate must not
      /// be accepted.
      auto validateDagCandidateProof =
          [&](const DagCandidateValidationMetadata &validation,
              StringRef baseText, StringRef newText,
              StringRef traceStage) -> bool {
        const RefoldModel::MacroInvocation &validationRootInvocation =
            dagCandidateAcceptanceCtx.subtreeValidation.rootInvocation;
        if (inTraceMode()) {
          // Emit a stable proof ledger before any rejection so failed composed
          // candidates can be diagnosed against the expected root-formal set
          // and deferred occurrence arguments.
          SmallVector<uint32_t, 8> expectedRootArgIdxs;
          expectedRootArgIdxs.reserve(validation.expectedRootFormals.size());
          for (const auto &kvLocal : validation.expectedRootFormals)
            expectedRootArgIdxs.push_back(kvLocal.first);
          llvm::sort(expectedRootArgIdxs);
          SmallVector<uint32_t, 8> deferredArgs =
              validation.deferOccurrenceArgIdxs;
          llvm::sort(deferredArgs);
          trace("macro/proof",
                "{0}: DAG candidate proof ledger enter root id={1} name={2} "
                "expectedRootArgs={3} deferredArgs={4} bridgeSensitive={5} "
                "mixed={6}",
                traceStage, validationRootInvocation.id, validationRootInvocation.name, formatUInt32List(expectedRootArgIdxs),
                formatUInt32List(deferredArgs),
                validation.hasBridgeSensitiveStructuredSemantics ? 1 : 0,
                validation.hasMixedSemanticInteractions ? 1 : 0);
        }

        // These semantic hazards are not repaired by root replay. If they
        // survived candidate metadata merging, the composed candidate is
        // inadmissible before any textual validation is attempted.
        if (validation.hasMixedSemanticInteractions) {
          return false;
        }
        if (validation.hasBridgeSensitiveStructuredSemantics) {
          return false;
        }

        // Re-derive the root-formal rewrite map from the concrete candidate
        // text and require it to match the expected merged proof metadata.
        auto proofCert = buildRootProofValidationCertificate(
            baseText, newText, validation.deferOccurrenceArgIdxs,
            validation.hasExpectedRootFormals ? &validation.expectedRootFormals
                                              : nullptr,
            traceStage);
        if (!proofCert.valid)
          return false;
        return true;
      };

      /// Convert a proven subtree rewrite certificate into the validation
      /// metadata carried by a DAG candidate.
      ///
      /// The resulting metadata is used later when multiple DAG candidates are
      /// composed or replay-validated at the root. It preserves the expected
      /// root-formal rewrites, deferred root occurrence arguments, and any
      /// semantic hazards that must remain globally visible after candidate
      /// construction.
      auto buildDagCandidateValidationMetadataFromSubtree =
          [&](const SubtreeRewriteCertificate &subtreeCert)
          -> DagCandidateValidationMetadata {
        DagCandidateValidationMetadata validation;
        validation.hasExpectedRootFormals = true;
        validation.hasBridgeSensitiveStructuredSemantics =
            subtreeCert.semantic.hasBridgeSensitiveStructuredSemantics;
        validation.hasMixedSemanticInteractions =
            subtreeCert.semantic.interactionSummary.hasMixedInteractions;

        // Only bridge-derived formals need their semantic signatures carried
        // into candidate-level metadata. Non-bridged formals have already been
        // checked inside the subtree semantic certificate.
        for (const auto &formalConsistency :
             subtreeCert.semantic.formalInteractionConsistencies) {
          std::string formalKey =
              formatv("{0}#{1}",
                      formalConsistency.inv ? formalConsistency.inv->id : 0,
                      formalConsistency.argIdx)
                  .str();
          if (subtreeCert.semantic.bridgedFormalKeys.contains(formalKey))
            validation.bridgeSensitiveFormalSignatures[formalKey] =
                formalConsistency.signature;
        }

        // These are the root rewrites the concrete DAG candidate must replay
        // exactly during final proof validation.
        for (const auto &kvLocal : subtreeCert.rootFormals)
          validation.expectedRootFormals[kvLocal.first] = kvLocal.second;

        validation.deferOccurrenceArgIdxs.assign(
            subtreeCert.deferRootOccurrenceArgIdxs.begin(),
            subtreeCert.deferRootOccurrenceArgIdxs.end());
        return validation;
      };

      /// Accept a DAG-produced root patch, merge it with the current unique
      /// patch, or reject it when the combined proof metadata no longer
      /// validates.
      ///
      /// The DAG path can discover several candidate patches for the same root
      /// invocation. This lambda maintains the invariant that `uniquePatch`, if
      /// present, is backed by validation metadata that still replays through
      /// the root proof machinery. Equivalent patches merge only their
      /// metadata; compatible distinct patches merge their replacement text and
      /// metadata; structurally comparable rivals may replace/keep the existing
      /// patch based on the structured-preservation preference.
      ///
      /// Every acceptance path validates the concrete replacement against the
      /// merged candidate metadata before updating `uniquePatch`.
      auto acceptOrMergeDAGCandidatePatch =
          [&](MacroPatch candPatch, StringRef baseText, StringRef traceStage,
              const DagCandidateValidationMetadata *candValidation =
                  nullptr) -> DagCandidateAcceptanceCertificate {
        DagCandidateAcceptanceCertificate cert;
        DagCandidateValidationMetadata candidateValidation;
        if (candValidation)
          candidateValidation = *candValidation;

        // First candidate for this root span: validate it directly, stamp the
        // root proof identity onto the patch, and install it as the unique
        // candidate state.
        if (!uniquePatch) {
          if (!validateDagCandidateProof(candidateValidation, baseText,
                                         candPatch.replacement, traceStage)) {
            cert.failure =
                DagCandidateAcceptanceFailure::MergedRootValidationFailed;
            cert.detail =
                formatv("{0}: DAG candidate patch rejected root id={1} "
                        "name={2} semantic validation metadata failed",
                        traceStage, rootInvocation.id, rootInvocation.name)
                    .str();
            return cert;
          }
          if (!candPatch.macroId)
            candPatch.macroId = m.id;
          if (!candPatch.proof.proofRootMacroId) {
            candPatch.proof.proofRootMacroId = m.id;
            GetProofLattice().SyncMacroPatchProofSummary(candPatch);
          }
          uniquePatch = std::move(candPatch);
          uniquePatchBaseText = baseText.str();
          uniquePatchValidation = std::move(candidateValidation);
          distinctRootPatches = 1;
          cert.accepted = true;
          cert.detail =
              formatv("{0}: accepted first DAG candidate root patch "
                      "root id={1} name={2} span=[{3},{4}) replLen={5}",
                      traceStage, rootInvocation.id, rootInvocation.name, uniquePatch->invStart,
                      uniquePatch->invEnd, uniquePatch->replacement.size())
                  .str();
          return cert;
        }

        // All DAG candidates for one accepted root patch must cover the exact
        // same source invocation span. Different spans are not composable here.
        if (uniquePatch->invStart != candPatch.invStart ||
            uniquePatch->invEnd != candPatch.invEnd) {
          cert.failure = DagCandidateAcceptanceFailure::DifferentSpan;
          cert.detail =
              formatv("{0}: DAG candidate patch rejected root id={1} "
                      "name={2} span mismatch existing=[{3},{4}) "
                      "candidate=[{5},{6})",
                      traceStage, rootInvocation.id, rootInvocation.name, uniquePatch->invStart,
                      uniquePatch->invEnd, candPatch.invStart, candPatch.invEnd)
                  .str();
          return cert;
        }

        // Textually equivalent patches are still proof-relevant. Merge their
        // validation metadata and replay-validate the unchanged replacement so
        // equivalent subtrees cannot smuggle incompatible semantic obligations.
        if (uniquePatch->replacement == candPatch.replacement) {
          auto mergedValidation = mergeDagCandidateValidationMetadata(
              uniquePatchValidation, candidateValidation);
          if (uniquePatch->subtreeCertBacked || candPatch.subtreeCertBacked) {
            REFOLD_LOG_TRACE("macro/proof",
                  "DAG equivalent subtree-plan probe: root id={0} name={1} "
                  "stage={2} existingExpRoot={3} candidateExpRoot={4} "
                  "mergedExpRoot(pending) currentDeferredArgs={5} "
                  "candidateDeferredArgs={6}",
                  rootInvocation.id, rootInvocation.name, traceStage,
                  stringutils::showWsWithClip(
                      uniquePatch->subtreeExpectedRootFormalSummary, 160),
                  stringutils::showWsWithClip(
                      candPatch.subtreeExpectedRootFormalSummary, 160),
                  stringutils::showWsWithClip(
                      uniquePatch->subtreeDeferredRootArgSummary, 160),
                  stringutils::showWsWithClip(
                      candPatch.subtreeDeferredRootArgSummary, 160));
          }
          if (!mergedValidation) {
            cert.failure =
                DagCandidateAcceptanceFailure::MergedRootValidationFailed;
            cert.detail =
                formatv("{0}: DAG candidate patch rejected root id={1} "
                        "name={2} equivalent replacement produced "
                        "incompatible root-formal validation metadata",
                        traceStage, rootInvocation.id, rootInvocation.name)
                    .str();
            return cert;
          }

          if (!validateDagCandidateProof(
                  *mergedValidation, *uniquePatchBaseText,
                  uniquePatch->replacement, traceStage)) {
            cert.failure =
                DagCandidateAcceptanceFailure::MergedRootValidationFailed;
            cert.detail =
                formatv("{0}: DAG candidate patch rejected root id={1} "
                        "name={2} equivalent replacement failed merged "
                        "root validation",
                        traceStage, rootInvocation.id, rootInvocation.name)
                    .str();
            return cert;
          }

          if (uniquePatch->subtreeCertBacked || candPatch.subtreeCertBacked) {
            REFOLD_LOG_TRACE(
                "macro/proof",
                "DAG equivalent subtree-plan merged: root id={0} name={1} "
                "stage={2} mergedExpRoot={3} mergedDeferredArgs={4} "
                "mergedBridgeFormals={5}",
                rootInvocation.id, rootInvocation.name, traceStage,
                formatFormalTextPairMap(mergedValidation->expectedRootFormals),
                formatUInt32List(mergedValidation->deferOccurrenceArgIdxs),
                formatBridgeSensitiveFormalSignatureMap(
                    mergedValidation->bridgeSensitiveFormalSignatures));
          }
          unionMacroPatchMaterializedBTokenRange(*uniquePatch, candPatch);
          uniquePatchValidation = std::move(*mergedValidation);
          cert.accepted = true;
          cert.detail =
              formatv("{0}: DAG candidate patch equivalent to existing "
                      "root patch root id={1} name={2} span=[{3},{4})",
                      traceStage, rootInvocation.id, rootInvocation.name, uniquePatch->invStart,
                      uniquePatch->invEnd)
                  .str();
          return cert;
        }

        // Non-equivalent replacements can be merged only if they were produced
        // from the same original root invocation spelling.
        if (!uniquePatchBaseText || *uniquePatchBaseText != baseText) {
          cert.failure = DagCandidateAcceptanceFailure::DifferentBaseText;
          cert.detail =
              formatv("{0}: DAG candidate patch rejected root id={1} "
                      "name={2} base text mismatch baseLenExisting={3} "
                      "baseLenCandidate={4}",
                      traceStage, rootInvocation.id, rootInvocation.name,
                      uniquePatchBaseText ? uniquePatchBaseText->size() : 0,
                      baseText.size())
                  .str();
          return cert;
        }

        // Before attempting textual merge, check whether the candidates are the
        // same proof shape but one preserves strictly more invocation
        // structure. This handles wrapper-preserving vs. flattened alternatives
        // without treating them as arbitrary conflicting text hunks.
        int preferredStructured = choosePreferredStructuredDagCandidate(
            uniquePatchValidation, candidateValidation);
        if (preferredStructured < 0) {
          if (uniquePatch->subtreeCertBacked || candPatch.subtreeCertBacked) {
            REFOLD_LOG_TRACE("macro/proof",
                  "DAG structured subtree-choice kept existing: root id={0} "
                  "name={1} stage={2} existingExpRoot={3} candidateExpRoot={4}",
                  rootInvocation.id, rootInvocation.name, traceStage,
                  stringutils::showWsWithClip(
                      uniquePatch->subtreeExpectedRootFormalSummary, 160),
                  stringutils::showWsWithClip(
                      candPatch.subtreeExpectedRootFormalSummary, 160));
          }
          cert.accepted = true;
          cert.detail =
              formatv("{0}: kept existing structured DAG candidate root "
                      "patch root id={1} name={2} over flatter rival",
                      traceStage, rootInvocation.id, rootInvocation.name)
                  .str();
          return cert;
        }
        if (preferredStructured > 0) {
          if (!candPatch.macroId)
            candPatch.macroId = m.id;
          if (!candPatch.proof.proofRootMacroId) {
            candPatch.proof.proofRootMacroId = m.id;
            GetProofLattice().SyncMacroPatchProofSummary(candPatch);
          }
          if (uniquePatch->subtreeCertBacked || candPatch.subtreeCertBacked) {
            REFOLD_LOG_TRACE(
                "macro/proof",
                "DAG structured subtree-choice replaced existing: root id={0} "
                "name={1} stage={2} existingExpRoot={3} candidateExpRoot={4}",
                rootInvocation.id, rootInvocation.name, traceStage,
                stringutils::showWsWithClip(
                    uniquePatch->subtreeExpectedRootFormalSummary, 160),
                stringutils::showWsWithClip(
                    candPatch.subtreeExpectedRootFormalSummary, 160));
          }
          uniquePatch = std::move(candPatch);
          uniquePatchBaseText = baseText.str();
          uniquePatchValidation = std::move(candidateValidation);
          cert.accepted = true;
          cert.detail =
              formatv("{0}: replaced existing DAG candidate root patch "
                      "root id={1} name={2} with more structured rival",
                      traceStage, rootInvocation.id, rootInvocation.name)
                  .str();
          return cert;
        }

        // Neither candidate dominates structurally, so try ordinary compatible
        // replacement merging. The text merge and the proof-metadata merge must
        // both succeed, and the merged replacement must replay through the root
        // proof validator.
        SmallVector<StringRef, 2> repls;
        repls.push_back(StringRef(uniquePatch->replacement));
        repls.push_back(StringRef(candPatch.replacement));
        auto merged = mergeCompatibleStringReplacements(
            *uniquePatchBaseText, ArrayRef<StringRef>(repls));
        if (!merged) {
          cert.failure = DagCandidateAcceptanceFailure::MergeConflict;
          cert.detail = formatv("{0}: DAG candidate patch rejected root id={1} "
                                "name={2} incompatible replacement hunks",
                                traceStage, rootInvocation.id, rootInvocation.name)
                            .str();
          return cert;
        }

        auto mergedValidation = mergeDagCandidateValidationMetadata(
            uniquePatchValidation, candidateValidation);
        if (uniquePatch->subtreeCertBacked || candPatch.subtreeCertBacked) {
          REFOLD_LOG_TRACE("macro/proof",
                "DAG merge subtree-plan probe: root id={0} name={1} stage={2} "
                "existingExpRoot={3} candidateExpRoot={4}",
                rootInvocation.id, rootInvocation.name, traceStage,
                stringutils::showWsWithClip(
                    uniquePatch->subtreeExpectedRootFormalSummary, 160),
                stringutils::showWsWithClip(
                    candPatch.subtreeExpectedRootFormalSummary, 160));
        }
        if (!mergedValidation) {
          cert.failure =
              DagCandidateAcceptanceFailure::MergedRootValidationFailed;
          cert.detail = formatv("{0}: DAG candidate patch rejected root id={1} "
                                "name={2} merged replacement produced "
                                "incompatible root-formal validation metadata",
                                traceStage, rootInvocation.id, rootInvocation.name)
                            .str();
          return cert;
        }

        if (uniquePatch->subtreeCertBacked || candPatch.subtreeCertBacked) {
          REFOLD_LOG_TRACE("macro/proof",
                "DAG merge subtree-plan merged: root id={0} name={1} stage={2} "
                "mergedExpRoot={3} mergedDeferredArgs={4} "
                "mergedBridgeFormals={5}",
                rootInvocation.id, rootInvocation.name, traceStage,
                formatFormalTextPairMap(mergedValidation->expectedRootFormals),
                formatUInt32List(mergedValidation->deferOccurrenceArgIdxs),
                formatBridgeSensitiveFormalSignatureMap(
                    mergedValidation->bridgeSensitiveFormalSignatures));
        }
        if (!validateDagCandidateProof(*mergedValidation, *uniquePatchBaseText,
                                       StringRef(*merged), traceStage)) {
          cert.failure =
              DagCandidateAcceptanceFailure::MergedRootValidationFailed;
          cert.detail = formatv("{0}: DAG candidate patch rejected root id={1} "
                                "name={2} merged replacement failed root "
                                "validation",
                                traceStage, rootInvocation.id, rootInvocation.name)
                            .str();
          return cert;
        }

        // The merged candidate is now both textually composable and
        // proof-valid. Update the unique patch in place while preserving the
        // accumulated validation metadata for any later candidate.
        uniquePatch->replacement = std::move(*merged);
        uniquePatch->hasMaterializedOutputByteRange = false;
        unionMacroPatchMaterializedBTokenRange(*uniquePatch, candPatch);
        uniquePatchValidation = std::move(*mergedValidation);
        if (!uniquePatch->macroId)
          uniquePatch->macroId = candPatch.macroId;
        ++distinctRootPatches;
        cert.accepted = true;
        cert.merged = true;
        cert.detail =
            formatv("{0}: merged DAG candidate root patch root id={1} "
                    "name={2} distinctRootPatches={3} replLen={4}",
                    traceStage, rootInvocation.id, rootInvocation.name, distinctRootPatches,
                    uniquePatch->replacement.size())
                .str();
        return cert;
      };

      // Replay any root-level split-insertion candidates that were proven while
      // scanning paired pure-insertion envelopes.
      //
      // These candidates already carry a concrete replacement for the full root
      // callsite text (for example, a reconstructed `WRAP(INC, (1) + 3)`), but
      // they still need to pass through the normal DAG candidate validation and
      // merge path so they compete consistently with any other root patches.
      for (const SplitInsertionRootCandidate &candidate :
           splitInsertionRootCandidates) {
        // Re-derive the expected root-formal rewrite map directly from the
        // candidate's replacement text. This gives the DAG validator the same
        // root-formal expectations it would have had if this candidate had been
        // produced through the ordinary replay path.
        auto replayRootFormals =
            buildRootFormalRewriteMapFromCallsiteReplacement(
                invSpanText, StringRef(candidate.patch.replacement));
        if (!replayRootFormals) {
          continue;
        }

        // Validate this split-root candidate against the exact set of root
        // formals implied by the reconstructed replacement. Also defer
        // occurrence-level consistency checks for the touched root formals so
        // the validator can discharge them using the final reconstructed root
        // callsite text rather than rejecting too early.
        DagCandidateValidationMetadata splitValidation;
        splitValidation.hasExpectedRootFormals = true;
        splitValidation.expectedRootFormals = *replayRootFormals;
        splitValidation.deferOccurrenceArgIdxs.assign(
            candidate.deferOccurrenceArgIdxs.begin(),
            candidate.deferOccurrenceArgIdxs.end());

        // Materialize a normal root patch from the queued split candidate.
        // Mark it as already proof-backed: the split-insertion logic has
        // already established that this is a structure-preserving args-only
        // root rewrite.
        MacroPatch splitRootPatch = candidate.patch;
        GetProofLattice().SetMacroPatchProof(splitRootPatch,
                           GetProofLattice().MakeMacroPatchProof(
                               MacroPatchProofKind::ArgsOnlyPairedPureInsertion,
                               /*preservesInvocationStructure=*/true, m.id));

        // Feed the candidate through the shared DAG acceptance/merge logic so
        // it is deduplicated and checked for incompatibility exactly the same
        // way as other DAG-derived root patches.
        auto acceptCert = acceptOrMergeDAGCandidatePatch(
            std::move(splitRootPatch), invSpanText,
            "DAG split insertion root patch", &splitValidation);
        if (!acceptCert.accepted)
          return std::nullopt;
      }

      // Examine each candidate leaf invocation that may explain the edited
      // expansion rooted at `m`.
      //
      // For each leaf, this loop:
      //
      //   1. collects observed per-formal edits from reliable arg-like spans,
      //   2. reconstructs edits from unreliable pasted-token subranges when a
      //      unique segmentation can be proven,
      //   3. certifies those observed edits as leaf-formal rewrites,
      //   4. validates paste-sensitive leaf edits,
      //   5. handles the special chained-call suffix case, and otherwise
      //   6. builds a full bottom-up subtree certificate and tries to accept or
      //      merge the resulting root patch.
      //
      // Each leaf is fail-closed: if any local proof obligation fails, the loop
      // skips that leaf and continues looking for another certifiable witness.
      for (const LeafCandidate &cand : leafCands) {
        const RefoldModel::MacroInvocation &leaf = *cand.inv;

        DenseMap<uint32_t, SmallVector<ObservedFormalConstraint, 2>>
            leafObserved;
        DenseMap<uint32_t, OldNewText> leafEdits;
        DenseMap<uint64_t, SmallVector<const RefoldModel::PPArgSpan *, 4>>
            unreliPaste;
        bool invalid = false;

        // Record one normalized observed old/new constraint for a leaf formal.
        // Duplicate observations are harmless; divergent observations are
        // handled later by the formal certificate builder.
        auto recordLeafObserved = [&](uint32_t argIdx, StringRef oldText,
                                      StringRef newText) -> bool {
          auto &constraints = leafObserved[argIdx];
          for (const auto &existing : constraints) {
            if (existing.oldText == oldText && existing.newText == newText)
              return true;
          }
          constraints.push_back(
              ObservedFormalConstraint{oldText.str(), newText.str()});
          return true;
        };

        // --- Pass 1: collect reliable per-formal edits -----------------------
        //
        // For each touched arg-like span:
        //   * extract A and B text,
        //   * normalize it under the span's stringify/paste context, and
        //   * record the resulting old/new text as an observed formal
        //     constraint.
        //
        // Paste subranges whose B-side extraction is unreliable are deferred to
        // pass 2, where the whole pasted token can be split as one unit.
        for (const RefoldModel::PPArgSpan &sp : cand.argLike) {
          if (sp.argIdx >= cand.touched.size() || !cand.touched[sp.argIdx])
            continue;

          auto aTxt = extractSpanText(sp, /*fromB=*/false);
          auto bTxt = extractSpanText(sp, /*fromB=*/true);
          if (!aTxt || !bTxt)
            continue;

          if (sp.kind == PPArgSpanKind::Paste && sp.byteBegin && sp.byteEnd &&
              !bTxt->reliable) {
            uint64_t key = (uint64_t(sp.begin) << 32) | uint64_t(sp.end);
            unreliPaste[key].push_back(&sp);
            continue;
          }

          if (!bTxt->reliable) {
            invalid = true;
            break;
          }

          auto oldLift = normalizeLiftText(cand.inv, sp, aTxt->text,
                                           /*allowTopLevelComma=*/true);
          bool allowComma = sp.argIdx < cand.inv->defParams.size() &&
                            cand.inv->defParams[sp.argIdx].variadic;
          auto newLift = normalizeLiftText(cand.inv, sp, bTxt->text,
                                           /*allowTopLevelComma=*/allowComma);
          if (!oldLift || !newLift) {
            invalid = true;
            break;
          }

          if (*oldLift == *newLift)
            continue;

          if (!recordLeafObserved(sp.argIdx, *oldLift, *newLift)) {
            invalid = true;
            break;
          }
        }

        if (invalid)
          continue;

        // --- Pass 2: resolve unreliable paste subranges ----------------------
        //
        // When paste subrange extraction is unreliable on B, reconstruct the
        // per-operand B-side text from the full pasted-token envelope. This is
        // accepted only when the edited token has a unique split back into the
        // original operand sequence.
        for (auto &kv : unreliPaste) {
          auto &group = kv.second;
          if (group.size() < 2)
            continue;

          llvm::sort(group, pasteSpanPtrLessByByteRange);

          bool groupOk = true;
          for (const RefoldModel::PPArgSpan *sp : group) {
            if (!sp->byteBegin || !sp->byteEnd) {
              groupOk = false;
              break;
            }
          }
          if (!groupOk) {
            invalid = true;
            break;
          }

          // Extract the whole pasted token in A and B. The individual paste
          // operands may have unreliable B ranges, but the enclosing token must
          // still be extractable.
          RefoldModel::PPArgSpan whole = *group.front();
          whole.kind = PPArgSpanKind::Standard;
          whole.argIdx = 0;
          whole.byteBegin = std::nullopt;
          whole.byteEnd = std::nullopt;

          auto aTok = extractSpanText(whole, /*fromB=*/false);
          auto bTok = extractSpanText(whole, /*fromB=*/true);
          if (!aTok || !bTok || !bTok->reliable) {
            invalid = true;
            break;
          }

          StringRef oldTok = aTok->text;
          StringRef newTok = bTok->text;
          const uint64_t oldLen = oldTok.size();

          // Validate that all operand byte ranges are ordered, non-overlapping,
          // and contained in the original pasted-token text.
          for (size_t i = 0; i < group.size(); ++i) {
            const auto *sp = group[i];
            if (*sp->byteBegin > *sp->byteEnd || *sp->byteEnd > oldLen) {
              groupOk = false;
              break;
            }
            if (i > 0 && *group[i - 1]->byteEnd > *sp->byteBegin) {
              groupOk = false;
              break;
            }
          }
          if (!groupOk) {
            invalid = true;
            break;
          }

          // The text outside the paste operands must be preserved verbatim.
          // Otherwise the edited B token is not just a rewrite of the pasted
          // operand surfaces.
          StringRef leading = oldTok.take_front(*group.front()->byteBegin);
          StringRef trailing = oldTok.drop_front(*group.back()->byteEnd);
          if (!newTok.starts_with(leading) || !newTok.ends_with(trailing)) {
            invalid = true;
            break;
          }

          SmallVector<StringRef, 4> oldSegs;
          SmallVector<StringRef, 4> midBodies;
          oldSegs.reserve(group.size());
          midBodies.reserve(group.size() - 1);
          bool hasEmptyInternalSeparator = false;
          bool hasNonEmptyInternalSeparator = false;
          for (size_t i = 0; i < group.size(); ++i) {
            const auto *sp = group[i];
            oldSegs.push_back(oldTok.slice(*sp->byteBegin, *sp->byteEnd));
            if (i + 1 < group.size()) {
              StringRef mid =
                  oldTok.slice(*sp->byteEnd, *group[i + 1]->byteBegin);
              if (mid.empty()) {
                hasEmptyInternalSeparator = true;
                continue;
              }
              hasNonEmptyInternalSeparator = true;
              midBodies.push_back(mid);
            }
          }
          if (hasEmptyInternalSeparator && hasNonEmptyInternalSeparator) {
            invalid = true;
            break;
          }

          StringRef core =
              newTok.slice(leading.size(), newTok.size() - trailing.size());

          SmallVector<StringRef, 4> curSegs;
          SmallVector<SmallVector<StringRef, 4>, 2> splitSolutions;
          auto addSplitSolution = [&](const SmallVectorImpl<StringRef> &parts) {
            SmallVector<StringRef, 4> copy(parts.begin(), parts.end());
            for (const auto &existing : splitSolutions)
              if (existing == copy)
                return;
            splitSolutions.push_back(std::move(copy));
          };

          if (hasEmptyInternalSeparator) {
            // No literal delimiter survives between adjacent pasted operands.
            // In that case the only sound split witness is an unchanged operand
            // that still appears verbatim in the edited pasted core. Accept the
            // group only when those unchanged anchors induce exactly one
            // segmentation of the rewritten core back into per-operand pieces.
            SmallVector<size_t, 4> anchoredIdxs;
            SmallVector<SmallVector<size_t, 4>, 4> anchorStartsByIdx;
            anchoredIdxs.reserve(group.size());
            anchorStartsByIdx.reserve(group.size());

            for (size_t i = 0; i < oldSegs.size(); ++i) {
              const StringRef anchor = oldSegs[i];
              if (anchor.empty())
                continue;

              SmallVector<size_t, 4> starts;
              for (size_t pos = 0;
                   (pos = core.find(anchor, pos)) != StringRef::npos; ++pos)
                starts.push_back(pos);
              if (starts.empty())
                continue;

              anchoredIdxs.push_back(i);
              anchorStartsByIdx.push_back(std::move(starts));
            }

            if (anchoredIdxs.empty()) {
              invalid = true;
              break;
            }

            SmallVector<size_t, 4> curAnchorStarts;
            auto addZeroDelimiterAnchoredSolution =
                [&](ArrayRef<size_t> anchorStarts) {
                  SmallVector<StringRef, 4> parts(group.size());
                  size_t prevConsumed = 0;

                  for (size_t anchorPos = 0; anchorPos < anchoredIdxs.size();
                       ++anchorPos) {
                    const size_t anchorIdx = anchoredIdxs[anchorPos];
                    const size_t anchorBegin = anchorStarts[anchorPos];
                    const size_t anchorEnd =
                        anchorBegin + oldSegs[anchorIdx].size();
                    if (anchorBegin < prevConsumed || anchorEnd > core.size())
                      return;

                    if (anchorPos == 0) {
                      if (anchorIdx > 1)
                        return;
                      if (anchorIdx == 0) {
                        if (anchorBegin != 0)
                          return;
                      } else {
                        // The first anchor is operand 1, so operand 0 is the
                        // only possible prefix segment before that anchor.
                        parts[0] = core.slice(0, anchorBegin);
                      }
                    } else {
                      const size_t prevAnchorIdx = anchoredIdxs[anchorPos - 1];
                      const size_t gapSegments = anchorIdx - prevAnchorIdx - 1;
                      if (gapSegments > 1)
                        return;
                      if (gapSegments == 1)
                        parts[prevAnchorIdx + 1] =
                            core.slice(prevConsumed, anchorBegin);
                      else if (anchorBegin != prevConsumed)
                        return;
                    }

                    parts[anchorIdx] = oldSegs[anchorIdx];
                    prevConsumed = anchorEnd;
                  }

                  const size_t trailingGapSegments =
                      group.size() - anchoredIdxs.back() - 1;
                  if (trailingGapSegments > 1)
                    return;
                  if (trailingGapSegments == 0) {
                    if (prevConsumed != core.size())
                      return;
                  } else {
                    // There is exactly one unanchored operand after the last
                    // anchor, so it must consume the remaining suffix.
                    parts[anchoredIdxs.back() + 1] =
                        core.drop_front(prevConsumed);
                  }

                  addSplitSolution(parts);
                };

            auto enumerateZeroDelimiterAnchors =
                [&](auto &&self, size_t anchorPos, size_t minStart) -> void {
              if (splitSolutions.size() > 1)
                return;
              if (anchorPos == anchoredIdxs.size()) {
                addZeroDelimiterAnchoredSolution(curAnchorStarts);
                return;
              }

              const size_t anchorIdx = anchoredIdxs[anchorPos];
              const StringRef anchor = oldSegs[anchorIdx];
              for (size_t start : anchorStartsByIdx[anchorPos]) {
                if (start < minStart)
                  continue;
                curAnchorStarts.push_back(start);
                self(self, anchorPos + 1, start + anchor.size());
                curAnchorStarts.pop_back();
              }
            };
            enumerateZeroDelimiterAnchors(enumerateZeroDelimiterAnchors, 0, 0);
          } else {
            // The normal case: split the rewritten core around the original
            // literal delimiters and require a unique segmentation.
            auto suffixDelimiterNeed = [&](size_t delimIdx) -> uint64_t {
              const StringRef delim = midBodies[delimIdx];
              uint64_t need = 0;
              for (size_t segIdx = delimIdx + 1; segIdx < oldSegs.size();
                   ++segIdx)
                need += countSubstringOccurrences(oldSegs[segIdx], delim);
              for (size_t later = delimIdx + 1; later < midBodies.size();
                   ++later)
                if (midBodies[later] == delim)
                  ++need;
              return need;
            };

            auto splitCore = [&](auto &&self, size_t delimIdx,
                                 StringRef rest) -> void {
              if (splitSolutions.size() > 1)
                return;
              if (delimIdx == midBodies.size()) {
                curSegs.push_back(rest);
                addSplitSolution(curSegs);
                curSegs.pop_back();
                return;
              }

              const StringRef delim = midBodies[delimIdx];
              const uint64_t needLeft = countSubstringOccurrences(oldSegs[delimIdx], delim);
              const uint64_t needRight = suffixDelimiterNeed(delimIdx);

              for (size_t pos = 0;
                   (pos = rest.find(delim, pos)) != StringRef::npos; ++pos) {
                StringRef left = rest.slice(0, pos);
                StringRef tail = rest.drop_front(pos + delim.size());
                if (countSubstringOccurrences(left, delim) < needLeft)
                  continue;
                if (countSubstringOccurrences(tail, delim) < needRight)
                  continue;
                curSegs.push_back(left);
                self(self, delimIdx + 1, tail);
                curSegs.pop_back();
              }
            };
            splitCore(splitCore, 0, core);
          }

          if (splitSolutions.size() != 1 ||
              splitSolutions[0].size() != group.size()) {
            invalid = true;
            break;
          }

          auto recordLeafEdit = [&](uint32_t argIdx, StringRef oldText,
                                    StringRef newText) -> bool {
            return recordLeafObserved(argIdx, oldText, newText);
          };

          // Normalize each recovered segment under its original paste-span
          // context and record it as a per-formal observed edit.
          for (size_t i = 0; i < group.size(); ++i) {
            const auto *sp = group[i];
            auto oldSeg = normalizeLiftText(&leaf, *sp, oldSegs[i],
                                            /*allowTopLevelComma=*/true);
            auto newSeg = normalizeLiftText(&leaf, *sp, splitSolutions[0][i],
                                            /*allowTopLevelComma=*/false);
            if (!oldSeg || !newSeg) {
              groupOk = false;
              break;
            }
            if (*oldSeg == *newSeg)
              continue;
            if (!recordLeafEdit(sp->argIdx, *oldSeg, *newSeg)) {
              groupOk = false;
              break;
            }
          }
          if (!groupOk) {
            invalid = true;
            break;
          }
        }

        if (invalid)
          continue;

        // Convert each touched leaf formal's observed old/new expansion text
        // into a certified leaf rewrite before lifting it toward the root.
        // Prefer the normal formal-rewrite certificate. If that fails solely
        // because the leaf lives in macro-body space and its observed text does
        // not match a unique raw structural template, allow one narrower seed:
        // all observed constraints for that formal must collapse to the same
        // normalized old/new text. That seed still must pass the structured
        // lift/root pipeline below.
        DenseSet<uint32_t> observedLeafSeedArgIdxs;
        for (const auto &kvLocal : leafObserved) {
          const uint32_t argIdx = kvLocal.first;
          auto formalCert = buildObservedFormalRewriteCertificate(
              leaf, argIdx, kvLocal.second, /*preferredChildSyntax=*/nullptr,
              "DAG subtree leaf formal");
          if (formalCert.kind == FormalRewriteCertificateKind::Invalid) {
            const bool uniformObservedSeedAllowed =
                formalCert.failure ==
                FormalRewriteFailure::MissingStructuralTemplate;
            if (uniformObservedSeedAllowed) {
              auto observedSeed = buildUniformObservedLeafSeedCertificate(
                  leaf, argIdx, kvLocal.second, "DAG subtree leaf formal");
              if (observedSeed.kind ==
                  UniformObservedLeafSeedCertificateKind::Unique) {
                observedLeafSeedArgIdxs.insert(argIdx);
                leafEdits[argIdx] = OldNewText{std::move(observedSeed.oldText),
                                               std::move(observedSeed.newText)};
                continue;
              }
            }
            invalid = true;
            break;
          }
          if (formalCert.kind == FormalRewriteCertificateKind::NoChange)
            continue;

          leafEdits[argIdx] = OldNewText{std::move(formalCert.oldText),
                                         std::move(formalCert.newText)};
        }

        if (invalid)
          continue;
        if (leafEdits.empty()) {
          continue;
        }

        SmallVector<uint32_t, 8> leafEditArgIdxs;
        leafEditArgIdxs.reserve(leafEdits.size());
        for (const auto &kvLocal : leafEdits)
          leafEditArgIdxs.push_back(kvLocal.first);

        const bool leafTouchesPaste =
            !leaf.pasteSpans.empty() &&
            anyInvocationArgTouchesPaste(leaf, leafEditArgIdxs);
        const bool allTouchedPasteArgsFromObservedLeafSeed =
            leafTouchesPaste &&
            allTouchedPasteArgsAreContained(leaf, leafEditArgIdxs,
                                            observedLeafSeedArgIdxs);

        // Paste-aware leaf certificate: when multiple leaf-formal rewrites
        // participate in the same pasted token, validate them as a group
        // against the B-side pasted-token spellings before attempting to lift
        // them up the caller chain.
        if (!leaf.pasteSpans.empty()) {
          DenseMap<uint32_t, std::string> leafReplByArgIdx;
          for (const auto &kvLocal : leafEdits)
            leafReplByArgIdx[kvLocal.first] = kvLocal.second.newText;

          if (leafTouchesPaste) {
            if (allTouchedPasteArgsFromObservedLeafSeed) {
              // Uniform observed seeds are already derived from the observed
              // pasted surface, so local paste validation is deferred and must
              // be discharged by the later subtree semantic certificate.
            } else if (!leaf.invText) {
              invalid = true;
            } else {
              auto leafRangesOpt = GetMacroInvocationFormalArgContentRanges(
                  leaf, StringRef(*leaf.invText));
              if (!leafRangesOpt) {
                invalid = true;
              } else if (!PasteArgReplacementsMatchAllPasteTokensInB(
                             leaf, StringRef(*leaf.invText), *leafRangesOpt,
                             leafReplByArgIdx)) {
                invalid = true;
              }
            }
          }
        }

        if (invalid)
          continue;

        bool deferLeafPasteValidation = false;
        if (!leaf.pasteSpans.empty())
          deferLeafPasteValidation = allTouchedPasteArgsFromObservedLeafSeed;

        // --- Special-case: chained call suffix arguments --------------------
        //
        // If the root invocation expands to an identifier that is immediately
        // called (e.g. PICK1()(10)), the callee's arguments are spelled in the
        // source as a chained call suffix following the root invocation. In
        // this situation, the leaf edit cannot be lifted to the root via
        // argDeps because the root has no formal parameters. Preserve the call
        // chain by patching the chained suffix argument ranges directly in the
        // invocation file text.
        if (numArgs == 0 && leaf.callerMacroId && *leaf.callerMacroId == m.id &&
            m.invFile && leaf.invFile && *leaf.invFile == *m.invFile) {
          const std::string absPath = (*deps_.lineDirs).ToAbsolutePath(*m.invFile);
          auto bufOrErr = llvm::MemoryBuffer::getFile(absPath);
          if (bufOrErr) {
            StringRef fileText = bufOrErr.get()->getBuffer();

            // Compute the chained call end in the same way as the application
            // Consume any trailing "(...)" groups after the root
            // invocation.
            const uint64_t chainEnd =
                stringutils::extendChainedCallEnd(fileText, *invEnd, "((x)+1)");
            if (chainEnd > *invEnd && chainEnd <= (uint64_t)fileText.size()) {
              // Apply call-chain local edits right-to-left so the byte ranges
              // remain relative to the original invocation spelling.
              struct LocalEdit {
                uint64_t begin; // relative to invStart
                uint64_t end;   // relative to invStart
                std::string repl;
              };

              SmallVector<LocalEdit, 4> localEdits;
              bool ok = true;

              for (auto &kv : leafEdits) {
                const uint32_t argIdx = kv.first;
                if (argIdx >= leaf.invArgRanges.size()) {
                  ok = false;
                  break;
                }

                const auto &rng = leaf.invArgRanges[argIdx];
                if (!rng.first || !rng.second) {
                  ok = false;
                  break;
                }

                const uint64_t bAbs = *rng.first;
                const uint64_t eAbs = *rng.second;
                if (bAbs > eAbs || eAbs > (uint64_t)fileText.size() ||
                    bAbs < *invStart || eAbs > chainEnd) {
                  ok = false;
                  break;
                }

                // Ensure the "old" text actually matches the invocation file at
                // the recorded byte range, so we don't patch unrelated text.
                StringRef oldInFile =
                    fileText.slice((size_t)bAbs, (size_t)eAbs).trim();
                if (oldInFile != StringRef(kv.second.oldText).trim()) {
                  ok = false;
                  break;
                }

                localEdits.push_back(LocalEdit{
                    bAbs - *invStart,
                    eAbs - *invStart,
                    StringRef(kv.second.newText).trim().str(),
                });
              }

              if (ok && !localEdits.empty()) {
                llvm::sort(localEdits,
                           [](const LocalEdit &a, const LocalEdit &b) {
                             return a.begin < b.begin;
                           });

                uint64_t curB = 0;
                for (const auto &e : localEdits) {
                  if (e.begin < curB || e.end < e.begin) {
                    ok = false;
                    break;
                  }
                  curB = e.end;
                }
              }

              if (ok && !localEdits.empty()) {
                std::string replText =
                    fileText.slice((size_t)*invStart, (size_t)chainEnd).str();

                // Apply edits back-to-front to keep byte indices stable.
                for (auto it = localEdits.rbegin(); it != localEdits.rend();
                     ++it) {
                  replText.replace((size_t)it->begin,
                                   (size_t)(it->end - it->begin), it->repl);
                }

                MacroPatch candPatch{*invStart, chainEnd, replText, m.id};
                GetProofLattice().SetMacroPatchProof(
                    candPatch,
                    GetProofLattice().MakeMacroPatchProof(MacroPatchProofKind::CallChainSuffix,
                                        /*preservesInvocationStructure=*/true,
                                        m.id));

                auto acceptCert = acceptOrMergeDAGCandidatePatch(
                    std::move(candPatch),
                    fileText.slice((size_t)*invStart, (size_t)chainEnd),
                    "DAG chained-call suffix patch");
                if (!acceptCert.accepted)
                  return std::nullopt;
                continue;
              }
            }
          }
        }

        // --- Build one explicit subtree certificate --------------------------
        //
        // The leaf rewrite, caller-chain lifting, root-formal merge, and final
        // root validation are now treated as one bottom-up subtree certificate
        // instead of several ad hoc stages.
        auto subtreeCert = buildSubtreeRewriteCertificate(
            leaf, leafEdits, deferLeafPasteValidation);
        if (subtreeCert.kind == SubtreeRewriteCertificateKind::Invalid ||
            subtreeCert.kind == SubtreeRewriteCertificateKind::NoChange) {
          continue;
        }

        auto rootPatchCert = buildRootPatchConstructionCertificate(
            subtreeCert.rootCert, "DAG subtree root patch");
        if (rootPatchCert.kind ==
                RootPatchConstructionCertificateKind::Invalid ||
            rootPatchCert.kind ==
                RootPatchConstructionCertificateKind::NoChange) {
          continue;
        }

        // Replay-validate the constructed root patch against the subtree's
        // expected root-formal metadata before stamping or merging it.
        DagCandidateValidationMetadata subtreeValidation =
            buildDagCandidateValidationMetadataFromSubtree(subtreeCert);
        if (!validateDagCandidateProof(
                subtreeValidation, invSpanText,
                StringRef(rootPatchCert.patch->replacement),
                "DAG subtree root patch")) {
          continue;
        }

        // Stamp the patch with the subtree certificate summary. These fields
        // are audit metadata for the accepted result; the proof itself has
        // already been checked by the subtree/root validation certificates.
        rootPatchCert.patch->macroId = m.id;
        GetProofLattice().SetMacroPatchProof(
            *rootPatchCert.patch,
            GetProofLattice().MakeMacroPatchProof(MacroPatchProofKind::DagSubtreeRoot,
                                /*preservesInvocationStructure=*/true, m.id));
        rootPatchCert.patch->subtreeCertBacked = true;
        rootPatchCert.patch->subtreeLeafMacroId = leaf.id;
        rootPatchCert.patch->subtreeWitnessCount = 1;
        rootPatchCert.patch->subtreeInvocationCertCount = static_cast<uint32_t>(
            subtreeCert.semantic.invocationCertificates.size());
        rootPatchCert.patch->subtreeFormalCertCount = static_cast<uint32_t>(
            subtreeCert.semantic.formalCertificates.size());
        rootPatchCert.patch->subtreeArgCertCount =
            static_cast<uint32_t>(subtreeCert.semantic.argCertificates.size());
        rootPatchCert.patch->subtreeLiftChainCount =
            static_cast<uint32_t>(subtreeCert.semantic.liftChains.size());
        rootPatchCert.patch->subtreeLiftStepCount = static_cast<uint32_t>(
            subtreeCert.semantic.structuredLiftCertificates.size());
        rootPatchCert.patch->subtreeRootMergeCount = static_cast<uint32_t>(
            subtreeCert.semantic.rootMergeCertificates.size());
        rootPatchCert.patch->subtreeUsesLexicalBridge =
            subtreeCert.semantic.usesLexicalBridge;
        rootPatchCert.patch->subtreeTouchesPaste =
            subtreeCert.semantic.touchesPaste;
        rootPatchCert.patch->subtreeHasWrapperSemantics =
            subtreeCert.semantic.hasWrapperSemantics;
        rootPatchCert.patch->subtreeHasStringifySemantics =
            subtreeCert.semantic.hasStringifySemantics;
        rootPatchCert.patch->subtreeHasWideStringifySemantics =
            subtreeCert.semantic.hasWideStringifySemantics;
        rootPatchCert.patch->subtreeHasPreferredChildSyntax =
            subtreeCert.semantic.hasPreferredChildSyntax;
        rootPatchCert.patch->subtreeHasRawInvocationPreservation =
            subtreeCert.semantic.hasRawInvocationPreservation;
        rootPatchCert.patch->subtreeHasPassthroughFlatten =
            subtreeCert.semantic.hasPassthroughFlatten;
        rootPatchCert.patch->subtreeHasBridgeSensitiveStructuredSemantics =
            subtreeCert.semantic.hasBridgeSensitiveStructuredSemantics;
        rootPatchCert.patch->subtreeDeferredPasteDischarged =
            subtreeCert.semantic.deferredPasteDischarge.valid;
        rootPatchCert.patch->subtreeAdmissible =
            subtreeCert.semantic.admissibility.valid;
        rootPatchCert.patch->subtreeExpectedRootFormalCount =
            static_cast<uint32_t>(subtreeValidation.expectedRootFormals.size());
        rootPatchCert.patch->subtreeDeferredRootArgCount =
            static_cast<uint32_t>(
                subtreeValidation.deferOccurrenceArgIdxs.size());
        rootPatchCert.patch->subtreeBridgeSensitiveFormalCount =
            static_cast<uint32_t>(
                subtreeValidation.bridgeSensitiveFormalSignatures.size());
        rootPatchCert.patch->subtreeExpectedRootFormalSummary =
            formatFormalTextPairMap(subtreeValidation.expectedRootFormals);
        rootPatchCert.patch->subtreeDeferredRootArgSummary =
            formatUInt32List(subtreeValidation.deferOccurrenceArgIdxs);
        rootPatchCert.patch->subtreeBridgeSensitiveFormalSummary =
            formatBridgeSensitiveFormalSignatureMap(
                subtreeValidation.bridgeSensitiveFormalSignatures);

        // The subtree certificate is filled after the primary proof stamp so
        // the classifier must see a refreshed MacroPatchProof carrier
        // before this candidate is merged, selected, or emitted.
        GetProofLattice().SyncMacroPatchProofSummary(*rootPatchCert.patch);

        // Finally, merge this subtree-backed root patch with any previously
        // accepted DAG candidate for the same root invocation.
        auto acceptCert = acceptOrMergeDAGCandidatePatch(
            std::move(*rootPatchCert.patch), invSpanText,
            "DAG subtree root patch", &subtreeValidation);
        if (!acceptCert.accepted)
          return std::nullopt;
      }

      return uniquePatch;
    };

    // Call-chain suffix patch: if this hunk's A-side PP tokens map to source
    // bytes in the chained-call suffix immediately following this invocation
    // (e.g. currying-style chains like GET_MATH(ADD)(10)(20)), patch those
    // bytes directly. This preserves the call chain and avoids whole-cover
    // expansion.
    if (hasLiteralMacroCalleeOrigin(m) && m.invFile && m.invB && m.invE) {
      std::string invAbs = (*deps_.lineDirs).ToAbsolutePath(*m.invFile);
      auto bufOrErr = MemoryBuffer::getFile(invAbs);
      if (bufOrErr) {
        std::unique_ptr<MemoryBuffer> buf = std::move(*bufOrErr);
        StringRef invFileText = buf->getBuffer();
        const uint64_t n = invFileText.size();
        const uint64_t invEndAbs = *m.invE;
        if (invEndAbs <= n) {
          // Compute the source extent of the chained-call suffix after the
          // macro invocation. Only tokens that map into this suffix are
          // eligible for the local call-chain patch.
          const uint64_t chainEndAbs = stringutils::extendChainedCallEnd(
              invFileText, invEndAbs, StringRef());
          if (chainEndAbs > invEndAbs) {
            const uint64_t aLen = h.aEnd - h.aStart;
            const uint64_t bLen = h.bEnd - h.bStart;
            if (aLen == bLen && aLen > 0) {
              struct TokEdit {
                uint64_t bAbs;
                uint64_t eAbs;
                std::string repl;
              };
              SmallVector<TokEdit, 8> tokEdits;
              tokEdits.reserve(aLen);
              uint64_t minB = std::numeric_limits<uint64_t>::max();
              uint64_t maxE = 0;
              bool ok = true;

              // Re-map each changed A-side PP token back to its original source
              // byte range. Every token must come from the same invocation file
              // and lie wholly inside the chained-call suffix; otherwise this
              // hunk is not a local suffix rewrite.
              const auto &tokmapByPP = (*deps_.model).GetTokmapByPP();
              for (uint64_t i = 0; i < aLen; ++i) {
                const uint64_t ppIdx = h.aStart + i;
                const uint64_t bTok = h.bStart + i;
                auto it = tokmapByPP.find(ppIdx);
                if (it == tokmapByPP.end()) {
                  ok = false;
                  break;
                }
                const RefoldModel::TokMapEntry &tm = it->second;
                if ((*deps_.lineDirs).ToAbsolutePath(tm.file) != invAbs) {
                  ok = false;
                  break;
                }
                if (tm.b < invEndAbs || tm.e > chainEndAbs) {
                  ok = false;
                  break;
                }
                if (tm.b > tm.e || tm.e > n) {
                  ok = false;
                  break;
                }
                StringRef repl = (*deps_.sourceMapper).SliceBSource(bTok, bTok + 1);
                tokEdits.push_back(TokEdit{tm.b, tm.e, repl.str()});
                minB = std::min(minB, tm.b);
                maxE = std::max(maxE, tm.e);
              }

              if (ok && minB < maxE && maxE <= n) {
                std::string covered = invFileText.slice(minB, maxE).str();

                // Convert absolute source-token edits into offsets relative to
                // the minimal covered suffix slice. The patch will replace only
                // this local slice, not the whole root invocation.
                SmallVector<TextEdit, 8> edits;
                edits.reserve(tokEdits.size());
                for (const auto &te : tokEdits)
                  edits.push_back(TextEdit{te.bAbs - minB,
                                           te.eAbs - minB,
                                           te.repl,
                                           std::nullopt,
                                           std::nullopt,
                                           {},
                                           {},
                                           {}});
                llvm::sort(edits, [](const TextEdit &a, const TextEdit &b) {
                  return a.start < b.start;
                });

                // Apply edits in source order. No line-directive resync is
                // needed because this patch is confined to the chained-call
                // suffix slice and preserves the surrounding invocation text.
                std::string out;
                out.reserve(covered.size());
                uint64_t cur = 0;
                for (const TextEdit &e : edits) {
                  if (e.start < cur || e.end > covered.size()) {
                    ok = false;
                    break;
                  }
                  out.append(covered, cur, e.start - cur);
                  out.append(e.text);
                  cur = e.end;
                }
                if (ok) {
                  out.append(covered, cur, covered.size() - cur);
                  {
                    MacroPatch patch{minB, maxE, std::move(out), m.id};
                    GetProofLattice().SetMacroPatchProof(
                        patch,
                        GetProofLattice().MakeMacroPatchProof(
                            MacroPatchProofKind::CallChainSuffix,
                            /*preservesInvocationStructure=*/true, m.id));
                    return patch;
                  }
                }
              }
            }
          }
        }
      }
    }

    // Prefer the DAG result over a direct root args-only rewrite when both are
    // available: the DAG path has already proved a structure-preserving nested
    // inverse, while the direct root patch only proves expansion equality at
    // this callsite.

    /// Validate the textual merge of a direct root rewrite with a DAG-backed
    /// root rewrite.
    ///
    /// This is a deliberately narrow replay check for the merged replacement:
    /// the merged text must still parse as the same root invocation shape, all
    /// fixed syntax outside the formal argument ranges must remain
    /// byte-for-byte identical, and only argument contents may differ. Unlike
    /// the full DAG proof validator, this does not rebuild subtree semantics;
    /// it only proves that combining the direct and DAG root replacements did
    /// not alter the root invocation envelope.
    auto validateMergedDirectAndDagRootReplacement =
        [&](StringRef baseText, StringRef newText) -> bool {
      if (baseText == newText) {
        return true;
      }

      // Re-parse both invocation spellings so the check is anchored to formal
      // argument ranges, not to arbitrary textual diff hunks.
      auto baseRangesOpt =
          GetMacroInvocationFormalArgContentRanges(m, baseText);
      auto newRangesOpt = GetMacroInvocationFormalArgContentRanges(m, newText);
      if (!baseRangesOpt || !newRangesOpt ||
          newRangesOpt->size() != baseRangesOpt->size()) {
        return false;
      }

      const auto &baseRanges = *baseRangesOpt;
      const auto &newRanges = *newRangesOpt;

      // The merged replacement may rewrite formal argument contents, but it
      // must preserve the fixed invocation spelling around those arguments:
      // callee spelling, parentheses, commas, and any non-argument trivia.
      auto fixedSpansMatch = [&]() -> bool {
        size_t oldCursor = 0;
        size_t newCursor = 0;
        for (size_t argIdx = 0; argIdx < baseRanges.size(); ++argIdx) {
          const auto &oldR = baseRanges[argIdx];
          const auto &newR = newRanges[argIdx];
          if (oldR.first > oldR.second || oldR.second > baseText.size() ||
              newR.first > newR.second || newR.second > newText.size())
            return false;

          if (baseText.slice(oldCursor, oldR.first) !=
              newText.slice(newCursor, newR.first))
            return false;

          oldCursor = oldR.second;
          newCursor = newR.second;
        }
        return baseText.drop_front(oldCursor) == newText.drop_front(newCursor);
      };

      if (!fixedSpansMatch()) {
        return false;
      }

      return true;
    };

    struct LocalFormalTextPair {
      std::string oldText;
      std::string newText;
    };

    // Parse the compact expected-root-formal summary stored in patch audit
    // metadata.
    //
    // The summary is emitted as a small C-like map, for example:
    //
    //   {0:'bill'->'bill', 1:'y'->'z'}
    //
    // This parser is intentionally narrow: it only exists to recover enough
    // structure to compare same-root witness cohorts. Use Clang's raw lexer so
    // punctuation, numeric constants, and quoted payload tokens are recognized
    // consistently with the rest of the refold pipeline instead of relying on
    // ad hoc string scanning.
    auto parseExpectedRootFormalSummary = [&](StringRef summary) {
      DenseMap<uint32_t, LocalFormalTextPair> out;
      StringRef s = summary.trim();
      if (s.empty() || s == "{}")
        return out;

      // Lex the summary from an artificial buffer. The raw source location only
      // needs to be stable enough to recover token slices from `lexBuf`.
      const SourceLocation baseLoc = SourceLocation::getFromRawEncoding(1);
      std::string lexBuf = s.str();
      lexBuf.push_back('\0');
      const char *bufStart = lexBuf.data();
      const char *bufEnd = bufStart + s.size();
      Lexer lexer(baseLoc, (*deps_.lexLang), bufStart, bufStart, bufEnd);

      auto nextNonCommentToken = [&]() {
        Token token;
        while (true) {
          lexer.LexFromRawLexer(token);
          if (!token.is(tok::comment))
            return token;
        }
      };

      // Recover the exact spelling for a token from the artificial lex buffer.
      // This avoids depending on Token internals beyond location and length.
      auto tokenText = [&](const Token &token) -> StringRef {
        const unsigned offset =
            token.getLocation().getRawEncoding() - baseLoc.getRawEncoding();
        return StringRef(bufStart + offset, token.getLength());
      };

      // Summary payloads are encoded as single-quoted token spellings. Accept
      // only char-constant token kinds so malformed summaries fail closed
      // rather than being partially hand-parsed.
      auto parseQuotedPayload =
          [&](const Token &token) -> std::optional<std::string> {
        switch (token.getKind()) {
        case tok::char_constant:
        case tok::wide_char_constant:
        case tok::utf8_char_constant:
        case tok::utf16_char_constant:
        case tok::utf32_char_constant:
          break;
        default:
          return std::nullopt;
        }

        StringRef text = tokenText(token);
        if (text.size() < 2 || text.front() != '\'' || text.back() != '\'')
          return std::nullopt;
        return text.drop_front().drop_back().str();
      };

      Token token = nextNonCommentToken();
      if (!token.is(tok::l_brace))
        return DenseMap<uint32_t, LocalFormalTextPair>{};

      // Parse entries of the form:
      //
      //   <argIdx> : '<oldText>' -> '<newText>'
      //
      // Any unexpected token rejects the whole summary by returning an empty
      // map. The caller treats an unparseable summary as unavailable metadata,
      // not as a partially valid witness.
      while (true) {
        token = nextNonCommentToken();
        if (token.is(tok::r_brace) || token.is(tok::eof))
          break;
        if (!token.is(tok::numeric_constant))
          return DenseMap<uint32_t, LocalFormalTextPair>{};

        uint32_t argIdx = 0;
        if (tokenText(token).getAsInteger(10, argIdx))
          return DenseMap<uint32_t, LocalFormalTextPair>{};

        token = nextNonCommentToken();
        if (!token.is(tok::colon))
          return DenseMap<uint32_t, LocalFormalTextPair>{};

        token = nextNonCommentToken();
        auto oldText = parseQuotedPayload(token);
        if (!oldText)
          return DenseMap<uint32_t, LocalFormalTextPair>{};

        token = nextNonCommentToken();
        if (!token.is(tok::arrow))
          return DenseMap<uint32_t, LocalFormalTextPair>{};

        token = nextNonCommentToken();
        auto newText = parseQuotedPayload(token);
        if (!newText)
          return DenseMap<uint32_t, LocalFormalTextPair>{};

        out[argIdx] =
            LocalFormalTextPair{std::move(*oldText), std::move(*newText)};

        // Entries are comma-separated. A right brace or EOF ends the compact
        // map; any other separator means the audit summary is malformed.
        token = nextNonCommentToken();
        if (token.is(tok::r_brace) || token.is(tok::eof))
          break;
        if (!token.is(tok::comma))
          return DenseMap<uint32_t, LocalFormalTextPair>{};
      }
      return out;
    };

    // Only compare witnesses that are already at the same concrete discharge
    // level. Deferred bridge-backed and passthrough-backed candidates are valid
    // subtree witnesses too, but they intentionally represent the same root at
    // a different abstraction level and must not be treated as conflicting
    // concrete cohorts.
    auto isConcreteSubtreeWitnessCohort = [&](const MacroPatch &patch) {
      if (!patch.subtreeCertBacked)
        return false;
      if (patch.subtreeUsesLexicalBridge)
        return false;
      if (patch.subtreeHasPassthroughFlatten)
        return false;
      if (patch.subtreeDeferredRootArgCount != 0)
        return false;
      return true;
    };

    // Two different subtree-backed leaves for the same root only force
    // whole-cover realization when they disagree on an overlapping concrete
    // root formal rewrite at the same concrete discharge level. This keeps safe
    // deferred wrapper/stringify cohorts from being misclassified as conflicts
    // merely because they preserve the same root through different bridge or
    // deferred-discharge evidence.
    auto conflictingConcreteSubtreeWitnesses =
        [&](const MacroPatch &existing, const MacroPatch &candidate) {
          if (!existing.subtreeCertBacked || !candidate.subtreeCertBacked)
            return false;
          if (existing.proof.proofRootMacroId != m.id ||
              candidate.proof.proofRootMacroId != m.id)
            return false;
          if (!existing.subtreeLeafMacroId || !candidate.subtreeLeafMacroId)
            return false;
          if (existing.subtreeLeafMacroId == candidate.subtreeLeafMacroId)
            return false;
          if (!isConcreteSubtreeWitnessCohort(existing) ||
              !isConcreteSubtreeWitnessCohort(candidate))
            return false;

          const auto existingFormals = parseExpectedRootFormalSummary(
              existing.subtreeExpectedRootFormalSummary);
          const auto candidateFormals = parseExpectedRootFormalSummary(
              candidate.subtreeExpectedRootFormalSummary);
          for (const auto &kvLocal : existingFormals) {
            auto it = candidateFormals.find(kvLocal.first);
            if (it == candidateFormals.end())
              continue;
            if (kvLocal.second.oldText != it->second.oldText ||
                kvLocal.second.newText != it->second.newText)
              return true;
          }
          return false;
        };

    // Try to compose the current root candidate with an already accepted
    // structure-preserving callsite patch for the same root invocation.
    //
    // This is a continuity check between two proof paths that target the same
    // source span. Before merging replacement text, it rejects concrete subtree
    // witness conflicts so a later same-root candidate cannot silently
    // overwrite an earlier subtree-backed witness. If the witnesses are
    // compatible, the merged text is replay-validated against the root
    // invocation envelope before updating the candidate in place.
    auto mergeCurrentRootWithExistingCallsitePatch =
        [&](MacroPatch &candidate, StringRef label) -> void {
      // Only merge against an existing structure-preserving callsite patch that
      // belongs to this same proof root. Other existing patches are handled by
      // the normal conflict/selection logic outside this helper.
      if (!existingPatch || !existingIsCallsite || baseInvText.empty() ||
          !existingPatch->proof.preservesInvocationStructure ||
          existingPatch->proof.proofRootMacroId != m.id)
        return;
      if (candidate.invStart != existingPatch->invStart ||
          candidate.invEnd != existingPatch->invEnd)
        return;
      if (candidate.replacement == existingPatch->replacement)
        return;

      // Concrete subtree witnesses for the same root are not allowed to
      // disagree. Treat that as an explicit whole-cover trigger instead of
      // merging text and losing witness continuity.
      if (conflictingConcreteSubtreeWitnesses(*existingPatch, candidate)) {
        conflictingConcreteSubtreeWitnessForcesWholeCover = true;
        return;
      }

      SmallVector<StringRef, 2> repls;
      repls.push_back(StringRef(candidate.replacement));
      repls.push_back(StringRef(existingPatch->replacement));
      auto merged = mergeCompatibleStringReplacements(
          baseInvText, ArrayRef<StringRef>(repls));
      if (!merged || !validateMergedDirectAndDagRootReplacement(
                         baseInvText, StringRef(*merged))) {
        return;
      }


      // The merged replacement has passed both text compatibility and root
      // replay validation, so update only the candidate. The caller remains
      // responsible for final acceptance/selection of that candidate.
      candidate.replacement = std::move(*merged);
      candidate.hasMaterializedOutputByteRange = false;
      if (!candidate.macroId)
        candidate.macroId = existingPatch->macroId;
    };

    /// Build a structure-preserving root patch for paste-derived selector
    /// substitution: a descendant macro callee was produced by token pasting,
    /// the edited surface changes body-owned tokens of the selected callee, and
    /// another *already-active* macro reachable through the same paste
    /// expression exactly explains B under the same non-selector arguments.
    ///
    /// This is intentionally not a macro-definition rewrite.  The only emitted
    /// source edit is a root invocation argument replacement, such as
    /// `DISPATCH(ONE, 10)` -> `DISPATCH(TWO, 10)`.  The proof is fail-closed:
    /// there must be exactly one active alternate macro target, the original
    /// target must reproduce the A cover using the same local expansion model,
    /// the alternate target must reproduce the B cover, and the pasted-name
    /// witness must identify one unique root selector argument to rewrite.
    auto tryPasteDerivedCalleeSelectorSubstitution =
        [&]() -> std::optional<MacroPatch> {
      StringRef invSpanText =
          !baseInvText.empty()
              ? baseInvText
              : (m.invText ? StringRef(*m.invText) : StringRef(""));
      if (m.subkind != "func" ||
          !RefoldLineObserverLayout::InvocationSpanMatchesCallsitePrefix(invSpanText, m) || !m.invB ||
          !m.invE)
        return std::nullopt;

      auto rootArgRangesOpt =
          GetMacroInvocationFormalArgContentRanges(m, invSpanText);
      if (!rootArgRangesOpt)
        return std::nullopt;
      const auto &rootArgRanges = *rootArgRangesOpt;

      using TokenSpellings = SmallVector<std::string, 16>;
      using ArgumentTokenSpellings = SmallVector<TokenSpellings, 4>;

      struct ProducerFunctionMacroDefinition {
        const RefoldModel::MacroDirective *directive = nullptr;
        StringRef name;
      };

      auto producerFunctionDefinitionFromDirective =
          [&](const RefoldModel::MacroDirective &directive)
          -> std::optional<ProducerFunctionMacroDefinition> {
        if (directive.subkind != "#define" || directive.name.empty())
          return std::nullopt;

        ProducerFunctionMacroDefinition definition;
        definition.directive = &directive;
        definition.name = directive.name;
        return definition;
      };

      // Resolve the macro state at a specific producer item id.  The selector
      // substitution may only reuse a definition that was active during the
      // original expansion.  It never edits a #define, revives an inactive
      // definition, or synthesizes a replacement-list model in the consumer.
      auto activeFunctionDefinitionBefore = [&](uint64_t beforeItemId,
                                                StringRef name)
          -> std::optional<ProducerFunctionMacroDefinition> {
        const RefoldModel::MacroDirective *active = nullptr;
        for (const auto &directive : (*deps_.model).GetMacroDirectives()) {
          if (directive.id >= beforeItemId)
            continue;
          if (directive.subkind != "#define" && directive.subkind != "#undef")
            continue;
          // MacroDirective::name is producer-owned macro-state proof data.
          // The selector proof must not rediscover #define/#undef names from
          // raw directive text; if the model name does not match, this
          // directive is not a state transition for the requested macro.
          if (directive.name != name)
            continue;
          if (!active || directive.id > active->id)
            active = &directive;
        }
        if (!active || active->subkind != "#define")
          return std::nullopt;
        return producerFunctionDefinitionFromDirective(*active);
      };



      // Replay a producer-recorded replacement-list tape.  This replaces the
      // old consumer-side re-lexing of #define bodies: the producer has already
      // classified each replacement token as either fixed literal spelling or a
      // formal-parameter reference.  The consumer merely substitutes recovered
      // argument token spellings and compares the resulting token sequence
      // against A/B.  '#' and '##' are still rejected because this selector
      // proof does not model stringification or paste inside the alternate
      // callee body.
      auto replayProducerFunctionMacroTokens =
          [&](const ProducerFunctionMacroDefinition &definition,
              ArrayRef<TokenSpellings> actualArgs)
          -> std::optional<TokenSpellings> {
        const RefoldModel::MacroDirective &directive = *definition.directive;
        if (directive.defParams.size() != actualArgs.size())
          return std::nullopt;

        TokenSpellings replayed;
        for (const RefoldModel::MacroReplacementToken &token :
             directive.replacementTokens) {
          if (token.spelling == "#" || token.spelling == "##")
            return std::nullopt;
          switch (token.kind) {
          case RefoldModel::MacroReplacementTokenKind::Literal:
            replayed.push_back(token.spelling.str());
            break;
          case RefoldModel::MacroReplacementTokenKind::ParamRef:
            if (!token.paramIndex || *token.paramIndex >= actualArgs.size())
              return std::nullopt;
            replayed.append(actualArgs[*token.paramIndex].begin(),
                            actualArgs[*token.paramIndex].end());
            break;
          }
        }
        return replayed;
      };

      // Recover the selected callee's ordinary argument token sequence from the
      // A-side PP cover by subtracting producer-recorded body spans.  This
      // keeps the replay proof entirely token-based: every non-body occurrence
      // of the single formal must have the same A-token spelling sequence, or
      // the proof has no unique non-selector argument vector and rejects.
      auto coverMinusBodySingleArg =
          [&](const RefoldModel::MacroInvocation &leaf)
          -> std::optional<ArgumentTokenSpellings> {
        if (leaf.defParams.size() != 1 || !leaf.cover.IsValid() ||
            leaf.cover.begin >= leaf.cover.end ||
            leaf.cover.end > deps_.aToks.size())
          return std::nullopt;

        SmallVector<RefoldModel::PPSpan, 4> body;
        body.append(leaf.bodySpans.begin(), leaf.bodySpans.end());
        llvm::sort(body, ppSpanLessByTokenRange);

        auto collectGapTokens =
            [&](uint64_t beginTok,
                uint64_t endTok) -> std::optional<TokenSpellings> {
          if (endTok < beginTok || endTok > deps_.aToks.size())
            return std::nullopt;
          TokenSpellings out;
          for (uint64_t tok = beginTok; tok < endTok; ++tok)
            out.push_back(deps_.aToks[static_cast<size_t>(tok)].spelling);
          return out;
        };

        uint64_t cursor = leaf.cover.begin;
        std::optional<TokenSpellings> argTokens;
        for (const auto &sp : body) {
          if (sp.end <= leaf.cover.begin || sp.begin >= leaf.cover.end)
            continue;
          const uint64_t clippedBegin =
              std::max<uint64_t>(sp.begin, leaf.cover.begin);
          const uint64_t clippedEnd =
              std::min<uint64_t>(sp.end, leaf.cover.end);
          if (cursor < clippedBegin) {
            std::optional<TokenSpellings> gap =
                collectGapTokens(cursor, clippedBegin);
            if (!gap)
              return std::nullopt;
            if (!gap->empty()) {
              if (argTokens && !tokenSpellingVectorsEqual(*argTokens, *gap))
                return std::nullopt;
              argTokens = std::move(*gap);
            }
          }
          cursor = std::max<uint64_t>(cursor, clippedEnd);
        }
        if (cursor < leaf.cover.end) {
          std::optional<TokenSpellings> gap =
              collectGapTokens(cursor, leaf.cover.end);
          if (!gap)
            return std::nullopt;
          if (!gap->empty()) {
            if (argTokens && !tokenSpellingVectorsEqual(*argTokens, *gap))
              return std::nullopt;
            argTokens = std::move(*gap);
          }
        }
        if (!argTokens)
          return std::nullopt;

        ArgumentTokenSpellings out;
        out.push_back(std::move(*argTokens));
        return out;
      };

      // Build an invocation index so provenance walks can climb from the
      // descendant callee back to the root invocation deterministically by
      // producer-recorded caller ids.
      DenseMap<uint64_t, const RefoldModel::MacroInvocation *> invById;
      for (const auto &mi : (*deps_.model).GetMacroInvocations())
        invById[mi.id] = &mi;

      auto depthToRoot = [&](const RefoldModel::MacroInvocation &cand)
          -> std::optional<unsigned> {
        unsigned d = 0;
        std::optional<uint64_t> p = cand.callerMacroId;
        while (p) {
          ++d;
          if (*p == m.id)
            return d;
          auto it = invById.find(*p);
          if (it == invById.end())
            break;
          p = it->second->callerMacroId;
        }
        return std::nullopt;
      };

      // A selector witness identifies the exact root invocation byte slice that
      // supplied the paste-derived callee selector, plus the replacement
      // spelling that would select a candidate active macro definition.  Byte
      // offsets are relative to the root invocation text used for the emitted
      // MacroPatch.
      struct SelectorRewriteWitness {
        uint32_t rootArgIdx = 0;
        uint64_t selectorByteBegin = 0;
        uint64_t selectorByteEnd = 0;
        std::string oldSelector;
        std::string newSelector;
      };

      // Invert the producer-recorded callee-origin tape against a candidate
      // macro name.  Literal parts must match exactly.  Exactly one
      // caller_arg_slice part may supply the selector, and it must name this
      // root invocation and a concrete byte slice of one root argument.  If the
      // split against the next literal anchor is ambiguous, the proof rejects.
      auto deriveSelectorRewriteForCandidateName =
          [&](const RefoldModel::MacroCalleeOrigin &origin,
              StringRef candidateName)
          -> std::optional<SelectorRewriteWitness> {
        if (origin.kind != MacroCalleeOriginKind::Paste || !origin.spelling ||
            *origin.spelling == candidateName || origin.parts.empty() ||
            candidateName.empty())
          return std::nullopt;

        size_t candidatePos = 0;
        std::optional<SelectorRewriteWitness> selector;
        for (size_t i = 0; i < origin.parts.size(); ++i) {
          const RefoldModel::CalleeOriginPart &part = origin.parts[i];
          if (part.kind == RefoldModel::CalleeOriginPartKind::Literal) {
            if (!candidateName.substr(candidatePos).starts_with(part.spelling))
              return std::nullopt;
            candidatePos += part.spelling.size();
            continue;
          }

          if (part.kind != RefoldModel::CalleeOriginPartKind::CallerArgSlice ||
              !part.rootMacroId || *part.rootMacroId != m.id ||
              !part.rootParamIndex || !part.byteBegin || !part.byteEnd ||
              *part.rootParamIndex >= rootArgRanges.size())
            return std::nullopt;
          if (selector)
            return std::nullopt;

          const auto &argRange = rootArgRanges[*part.rootParamIndex];
          const uint64_t argLen = argRange.second - argRange.first;
          if (*part.byteEnd < *part.byteBegin || *part.byteEnd > argLen)
            return std::nullopt;

          StringRef rootSlice = invSpanText.slice(
              argRange.first + *part.byteBegin, argRange.first + *part.byteEnd);
          if (rootSlice != part.spelling)
            return std::nullopt;

          StringRef nextLiteral;
          for (size_t j = i + 1; j < origin.parts.size(); ++j) {
            if (origin.parts[j].kind ==
                RefoldModel::CalleeOriginPartKind::Literal) {
              nextLiteral = origin.parts[j].spelling;
              break;
            }
            if (origin.parts[j].kind ==
                RefoldModel::CalleeOriginPartKind::CallerArgSlice)
              return std::nullopt;
          }

          StringRef replacementPart;
          if (nextLiteral.empty()) {
            replacementPart = candidateName.drop_front(candidatePos);
            candidatePos = candidateName.size();
          } else {
            size_t found = candidateName.find(nextLiteral, candidatePos);
            if (found == StringRef::npos)
              return std::nullopt;
            if (candidateName.find(nextLiteral, found + 1) != StringRef::npos)
              return std::nullopt;
            replacementPart = candidateName.slice(candidatePos, found);
            candidatePos = found;
          }

          if (replacementPart.empty() || replacementPart == part.spelling)
            return std::nullopt;

          SelectorRewriteWitness out;
          out.rootArgIdx = *part.rootParamIndex;
          out.selectorByteBegin = argRange.first + *part.byteBegin;
          out.selectorByteEnd = argRange.first + *part.byteEnd;
          out.oldSelector = part.spelling.str();
          out.newSelector = replacementPart.str();
          selector = std::move(out);
        }

        if (candidatePos != candidateName.size() || !selector)
          return std::nullopt;
        return selector;
      };

      // Each candidate records one complete explanation of B: which descendant
      // callee was edited, which active alternate macro definition explains the
      // B-side cover, and what root invocation text would select that macro.
      struct SelectorCandidate {
        uint64_t leafId = 0;
        uint64_t candidateDirectiveId = 0;
        uint32_t rootArgIdx = 0;
        uint64_t selectorByteBegin = 0;
        uint64_t selectorByteEnd = 0;
        uint64_t bTokStart = 0;
        uint64_t bTokEnd = 0;
        uint64_t newSelectorSize = 0;
        std::string replacement;
      };
      SmallVector<SelectorCandidate, 4> candidates;

      // Search descendants of the current root for the selected callee whose
      // body-owned tokens contain the edited A hunk.  This prevents selector
      // substitution from firing on unrelated paste tokens in the same root
      // DAG.
      for (const auto &leaf : (*deps_.model).GetMacroInvocations()) {
        if (leaf.id == m.id || leaf.subkind != "func")
          continue;
        std::optional<unsigned> depth = depthToRoot(leaf);
        if (!depth || *depth == 0)
          continue;
        if (!leaf.cover.IsValid() ||
            !(leaf.cover.begin <= hEff.aStart && hEff.aEnd <= leaf.cover.end) ||
            !hunkWithinPPSpans(hEff, leaf.bodySpans))
          continue;

        // First prove that the original selected callee definition explains
        // the A-side cover under the recovered non-selector arguments.  Without
        // this baseline equality, replacing the selector would be relating B to
        // a model that did not actually produce A.
        std::optional<ProducerFunctionMacroDefinition> currentDef =
            activeFunctionDefinitionBefore(leaf.id, leaf.name);
        if (!currentDef ||
            (leaf.definitionDirectiveId &&
             currentDef->directive->id != *leaf.definitionDirectiveId))
          continue;
        std::optional<ArgumentTokenSpellings> actualArgs =
            coverMinusBodySingleArg(leaf);
        if (!actualArgs)
          continue;

        std::optional<TokenSpellings> currentExpansion =
            replayProducerFunctionMacroTokens(*currentDef, *actualArgs);
        if (!currentExpansion ||
            !TokenSpellingsEqualToA(*currentExpansion, leaf.cover.begin,
                                    leaf.cover.end))
          continue;

        // The alternate macro must explain exactly the B token envelope mapped
        // from the selected callee's original PP cover.  The selector proof
        // does not widen the edit or borrow neighboring B tokens.
        std::optional<std::pair<size_t, size_t>> bEnv =
            (*deps_.sourceMapper).MapATokRangeAToBTokenEnvelope(leaf.cover.begin, leaf.cover.end);
        if (!bEnv || bEnv->second <= bEnv->first)
          continue;

        if (leaf.calleeOrigin.kind != MacroCalleeOriginKind::Paste ||
            !leaf.calleeOrigin.spelling ||
            *leaf.calleeOrigin.spelling != leaf.name ||
            leaf.calleeOrigin.parts.empty())
          continue;

        // Enumerate existing macro definitions as possible selector targets.
        // This is a finite namespace proof over definitions already present in
        // the source.  Each candidate must be active at the root expansion
        // point and must replay through producer-recorded replacement tokens.
        for (const auto &directive : (*deps_.model).GetMacroDirectives()) {
          std::optional<ProducerFunctionMacroDefinition> candidateDef =
              producerFunctionDefinitionFromDirective(directive);
          if (!candidateDef || candidateDef->name == leaf.name)
            continue;
          std::optional<ProducerFunctionMacroDefinition> activeCandidate =
              activeFunctionDefinitionBefore(m.id, candidateDef->name);
          if (!activeCandidate ||
              activeCandidate->directive->id != candidateDef->directive->id)
            continue;
          if (candidateDef->directive->defParams.size() !=
              currentDef->directive->defParams.size())
            continue;

          std::optional<SelectorRewriteWitness> selector =
              deriveSelectorRewriteForCandidateName(leaf.calleeOrigin,
                                                    candidateDef->name);
          if (!selector)
            continue;

          std::optional<TokenSpellings> candidateExpansion =
              replayProducerFunctionMacroTokens(*candidateDef, *actualArgs);
          if (!candidateExpansion ||
              !TokenSpellingsEqualToB(*candidateExpansion,
                                      static_cast<uint64_t>(bEnv->first),
                                      static_cast<uint64_t>(bEnv->second)))
            continue;

          // Build the only source edit admitted by this proof: replace the
          // producer-proven selector slice inside the root invocation and leave
          // the rest of the callsite unchanged.  The normal root replay
          // validator still checks that the resulting invocation text is
          // well-formed.
          std::string replacement = invSpanText.str();
          replacement.replace(selector->selectorByteBegin,
                              selector->selectorByteEnd -
                                  selector->selectorByteBegin,
                              selector->newSelector);
          if (!validateMergedDirectAndDagRootReplacement(invSpanText,
                                                         replacement))
            continue;

          candidates.push_back(SelectorCandidate{
              leaf.id, candidateDef->directive->id, selector->rootArgIdx,
              selector->selectorByteBegin, selector->selectorByteEnd,
              static_cast<uint64_t>(bEnv->first),
              static_cast<uint64_t>(bEnv->second),
              static_cast<uint64_t>(selector->newSelector.size()),
              std::move(replacement)});
        }
      }

      if (candidates.empty())
        return std::nullopt;

      // Sort before uniqueness checking so diagnostics and tie handling are
      // deterministic.  The proof accepts multiple witnesses only when they all
      // lead to the exact same root replacement text; distinct selector
      // rewrites are treated as ambiguous and rejected.
      llvm::sort(candidates, [](const SelectorCandidate &lhs,
                                const SelectorCandidate &rhs) {
        if (lhs.replacement != rhs.replacement)
          return lhs.replacement < rhs.replacement;
        if (lhs.leafId != rhs.leafId)
          return lhs.leafId < rhs.leafId;
        if (lhs.candidateDirectiveId != rhs.candidateDirectiveId)
          return lhs.candidateDirectiveId < rhs.candidateDirectiveId;
        if (lhs.rootArgIdx != rhs.rootArgIdx)
          return lhs.rootArgIdx < rhs.rootArgIdx;
        if (lhs.selectorByteBegin != rhs.selectorByteBegin)
          return lhs.selectorByteBegin < rhs.selectorByteBegin;
        if (lhs.selectorByteEnd != rhs.selectorByteEnd)
          return lhs.selectorByteEnd < rhs.selectorByteEnd;
        if (lhs.bTokStart != rhs.bTokStart)
          return lhs.bTokStart < rhs.bTokStart;
        return lhs.bTokEnd < rhs.bTokEnd;
      });

      const std::string &chosenReplacement = candidates.front().replacement;
      for (const SelectorCandidate &candidate : candidates)
        if (candidate.replacement != chosenReplacement) {
          return std::nullopt;
        }

      // Stamp the accepted selector substitution as a structure-preserving root
      // macro patch.  The materialized B token range records the descendant
      // expansion that this selector explains, while the output byte range
      // points at the rewritten root argument inside the replacement callsite
      // text.
      MacroPatch patch{*m.invB, *m.invE, chosenReplacement, m.id};
      patch.hasMaterializedBTokenRange = true;
      patch.materializedBTokStart = candidates.front().bTokStart;
      patch.materializedBTokEnd = candidates.front().bTokEnd;
      patch.hasMaterializedOutputByteRange = true;
      patch.materializedOutputByteStart = candidates.front().selectorByteBegin;
      patch.materializedOutputByteEnd = candidates.front().selectorByteBegin +
                                        candidates.front().newSelectorSize;
      GetProofLattice().SetMacroPatchProof(
          patch,
          GetProofLattice().MakeMacroPatchProof(MacroPatchProofKind::PasteDerivedCalleeSelector,
                              /*preservesInvocationStructure=*/true, m.id));
      return patch;
    };

    // Keep the DAG root replay as a final-selection candidate instead of
    // returning it immediately.
    //
    // `tryDAGChainedArgsOnly()` can produce a structure-preserving root patch
    // that competes with the direct args-only root replay for the same
    // invocation span. When both candidates exist, this block first resolves
    // that local same-root competition using replay validation plus the proof
    // lattice. The winning DAG candidate is then carried into the common final
    // selector, where it can still compete against whole-cover realization and
    // any reusable already-tracked callsite patch.
    auto dag = tryDAGChainedArgsOnly();
    if (dag) {
      bool preferDirectRootCandidate = false;
      if (argsOnlyCandidate && dag->invStart == argsOnlyCandidate->invStart &&
          dag->invEnd == argsOnlyCandidate->invEnd &&
          dag->replacement != argsOnlyCandidate->replacement &&
          !baseInvText.empty()) {
        // Both candidates target the same root invocation but produce different
        // text. Validate each replacement against the root invocation envelope
        // before asking the proof lattice to choose between their proof
        // classes.
        const bool directValid = validateMergedDirectAndDagRootReplacement(
            baseInvText, StringRef(argsOnlyCandidate->replacement));
        const bool dagValid = validateMergedDirectAndDagRootReplacement(
            baseInvText, StringRef(dag->replacement));

        // The normalized lattice comparator is authoritative for same-root
        // root-level competition. Once both candidates are individually valid,
        // choose the stronger compatible proof class by the explicit lattice
        // law rather than by an ad hoc direct-vs-DAG heuristic.
        if (directValid && dagValid) {
          const bool preferDirect = GetProofLattice().LatticePrefers(
              argsOnlyCandidate->proofSummary, dag->proofSummary);
          const bool preferDag = GetProofLattice().LatticePrefers(
              dag->proofSummary, argsOnlyCandidate->proofSummary);
          preferDirectRootCandidate = preferDirect || !preferDag;
        }
      } else if (argsOnlyCandidate &&
                 dag->replacement != argsOnlyCandidate->replacement) {
        // The candidates are not a clean same-span root competition, but they
        // still differ textually. Keep the trace explicit because the DAG path
        // will be staged below unless the direct candidate won above.
      }

      if (!preferDirectRootCandidate) {
        // Before staging the DAG patch, give it a chance to compose with an
        // existing structure-preserving callsite patch for the same root span.
        // A concrete subtree witness conflict suppresses the DAG candidate and
        // forces the later whole-cover path instead.
        mergeCurrentRootWithExistingCallsitePatch(*dag,
                                                  "dag/direct root rewrite");
        if (!conflictingConcreteSubtreeWitnessForcesWholeCover) {
          // The DAG candidate has already won the local same-root competition
          // against the direct args-only replay. Preserve that decision by
          // carrying only the DAG root candidate into the final selector; the
          // shared selector still arbitrates it against whole-cover and any
          // reusable already-tracked patch for the same invocation span.
          dagRootCandidate = *dag;
          argsOnlyCandidate.reset();
        } else {
          argsOnlyCandidate.reset();
          reuseExistingCallsitePatch = false;
        }
      }
    }

    // Try selector substitution only after ordinary DAG replay declined to
    // produce a root candidate.  When accepted, it enters the same final macro
    // candidate path as other DAG-root proofs so existing conflict handling and
    // lattice selection remain authoritative.
    if (!dagRootCandidate) {
      if (std::optional<MacroPatch> selectorPatch =
              tryPasteDerivedCalleeSelectorSubstitution()) {
        mergeCurrentRootWithExistingCallsitePatch(
            *selectorPatch, "paste-derived callee selector substitution");
        if (!conflictingConcreteSubtreeWitnessForcesWholeCover) {
          dagRootCandidate = std::move(*selectorPatch);
          argsOnlyCandidate.reset();
        }
      }
    }

    if (directRootPreservationInadmissible) {
      // `directRootPreservationInadmissible` means that the ordinary direct
      // argument-span proof did not see the edited descendant surface.  That is
      // not enough to discard a stronger direct candidate that was built by a
      // whole-cover owner proof, such as higher-order generated-callee replay:
      // that proof deliberately explains the descendant edit through the root
      // invocation's replacement-list grammar and records the B-token envelope
      // it materializes.  Suppress only candidates that do not discharge the
      // current hunk with such an owner-level witness.
      const bool directCandidateDischargesDescendantHunk =
          argsOnlyCandidate &&
          argsOnlyCandidate->proof.preservesInvocationStructure &&
          argsOnlyCandidate->proof.proofRootMacroId == m.id &&
          argsOnlyCandidate->hasMaterializedBTokenRange &&
          argsOnlyCandidate->materializedBTokStart <= h.bStart &&
          h.bEnd <= argsOnlyCandidate->materializedBTokEnd;

      if (!directCandidateDischargesDescendantHunk) {
        argsOnlyCandidate.reset();
        reuseExistingCallsitePatch = false;
      }
    }

    // Try to compose a direct root args-only candidate with an existing
    // structure-preserving callsite patch for the same root invocation.
    //
    // This is the direct-candidate counterpart to the DAG/callsite merge path:
    // it allows compatible same-root patches to combine, but rejects or
    // deprioritizes the direct replay when the merged text cannot be validated
    // against the root invocation envelope or when the proof lattice prefers
    // the existing structure-preserving witness. This prevents a direct
    // args-only replay from silently overriding an already accepted same-root
    // callsite witness.
    if (argsOnlyCandidate && existingPatch && existingIsCallsite &&
        existingPatch->proof.preservesInvocationStructure &&
        existingPatch->proof.proofRootMacroId == m.id && !baseInvText.empty() &&
        argsOnlyCandidate->invStart == existingPatch->invStart &&
        argsOnlyCandidate->invEnd == existingPatch->invEnd &&
        argsOnlyCandidate->replacement != existingPatch->replacement) {

      auto replacementCollapsesParenthesizedTupleFormal =
          [&](const MacroPatch &preservingPatch,
              const MacroPatch &candidatePatch) -> bool {
        auto baseRanges =
            GetMacroInvocationFormalArgContentRanges(m, baseInvText);
        auto preservingRanges = GetMacroInvocationFormalArgContentRanges(
            m, StringRef(preservingPatch.replacement));
        auto candidateRanges = GetMacroInvocationFormalArgContentRanges(
            m, StringRef(candidatePatch.replacement));
        if (!baseRanges || !preservingRanges || !candidateRanges ||
            preservingRanges->size() != baseRanges->size() ||
            candidateRanges->size() != baseRanges->size())
          return false;

        for (size_t i = 0; i < baseRanges->size(); ++i) {
          const StringRef baseArg = baseInvText.slice((*baseRanges)[i].first,
                                                      (*baseRanges)[i].second);
          if (!IsParenthesizedTuple(baseArg))
            continue;

          const StringRef preservingArg =
              StringRef(preservingPatch.replacement)
                  .slice((*preservingRanges)[i].first,
                         (*preservingRanges)[i].second);
          const StringRef candidateArg =
              StringRef(candidatePatch.replacement)
                  .slice((*candidateRanges)[i].first,
                         (*candidateRanges)[i].second);
          if (IsParenthesizedTuple(preservingArg) &&
              !IsParenthesizedTuple(candidateArg))
            return true;
        }
        return false;
      };

      // If an earlier structure-preserving patch already materializes a B-token
      // envelope that covers this hunk, do not automatically freeze it.  The
      // covered-envelope fact proves that both patches talk about the same B
      // surface, but it does not prove that the older patch has already updated
      // every source-level formal affected by this hunk.  Direct stringify and
      // paste repairs such as `FOO(billy, bob) -> FOO(billy, corgan)` must
      // still be allowed to replace a stale same-root patch.
      //
      // The one case where the older patch really is the stronger owner proof
      // is the tuple/generated-callee collapse we introduced this guard for:
      // the original argument is a parenthesized tuple, the existing patch
      // preserves that tuple, and the later direct args-only candidate replaces
      // the tuple formal with its generated expansion text.  In that situation
      // merging or selecting the direct candidate would lose source structure
      // that has already been proved by the tuple owner.
      if (existingPatch->hasMaterializedBTokenRange &&
          existingPatch->materializedBTokStart <= h.bStart &&
          h.bEnd <= existingPatch->materializedBTokEnd &&
          replacementCollapsesParenthesizedTupleFormal(*existingPatch,
                                                       *argsOnlyCandidate)) {
        argsOnlyCandidate.reset();
        reuseExistingCallsitePatch = true;
      } else {
        // First try ordinary compatible text merging. Even when the two patches
        // touch the same root span, the merge is accepted only if the resulting
        // replacement still preserves the root invocation envelope.
        SmallVector<StringRef, 2> repls;
        repls.push_back(StringRef(argsOnlyCandidate->replacement));
        repls.push_back(StringRef(existingPatch->replacement));
        auto merged = mergeCompatibleStringReplacements(
            baseInvText, ArrayRef<StringRef>(repls));
        if (merged && validateMergedDirectAndDagRootReplacement(
                          baseInvText, StringRef(*merged))) {
          argsOnlyCandidate->replacement = std::move(*merged);
          argsOnlyCandidate->hasMaterializedOutputByteRange = false;
          if (!argsOnlyCandidate->macroId)
            argsOnlyCandidate->macroId = existingPatch->macroId;

          // The merged direct replay is the unique source-level candidate for
          // this callsite after it absorbs the existing same-root patch.
          // Keeping the stale pre-merge patch in the final selector would
          // manufacture a second, non-equivalent witness class for bytes that
          // are already covered by the merged invocation repair.
          reuseExistingCallsitePatch = false;
          existingCallsitePatchAbsorbedByDirectCandidate = true;
        } else {
          // If the patches cannot be merged, check whether the direct candidate
          // is independently valid. An invalid direct replay is discarded so
          // the existing callsite patch remains available to the final
          // selector.
          const bool directValid = validateMergedDirectAndDagRootReplacement(
              baseInvText, StringRef(argsOnlyCandidate->replacement));
          if (!directValid) {
            argsOnlyCandidate.reset();
            reuseExistingCallsitePatch = true;
          } else {
            // Both candidates are individually viable but not merge-compatible.
            // Defer to the proof lattice rather than letting the direct replay
            // win merely because it was produced in this local path.
            const bool preferDirect = GetProofLattice().LatticePrefers(
                argsOnlyCandidate->proofSummary, existingPatch->proofSummary);
            const bool preferExisting = GetProofLattice().LatticePrefers(
                existingPatch->proofSummary, argsOnlyCandidate->proofSummary);
            if (preferExisting && !preferDirect) {
              argsOnlyCandidate.reset();
              reuseExistingCallsitePatch = true;
            }
          }
        }
      }
    }
  }

  // Route every macro-level candidate through the common final selector.
  //
  // This prevents nested structure-preserving artifacts from bypassing the
  // normal macro candidate competition. The only local exception is for
  // non-top-level construction sites: a nested macro carrier may be allowed to
  // fail only the top-level proof-root requirement while still participating in
  // selector-only competition. If such an artifact is selected, it must be
  // restamped onto an emission-discharged carrier before any byte edit is
  // emitted.
  const bool allowNonTopLevelMacroSelectorFailure =
      (*deps_.macroTopology).GetRootMacroId(m.id) != m.id;

  // Reuse of an existing callsite patch is split into two cases:
  //
  // * `canReuseExistingCallsiteNoOp` means the current path has already decided
  //   to reuse the existing patch directly.
  // * `canReuseExistingCallsiteSkipWholeCover` means an existing
  //   structure-preserving callsite patch is strong enough to compete in the
  //   final selector without forcing a whole-cover plan.
  const bool canReuseExistingCallsiteNoOp =
      reuseExistingCallsitePatch &&
      !conflictingConcreteSubtreeWitnessForcesWholeCover && existingPatch;
  const bool canReuseExistingCallsiteSkipWholeCover =
      !canReuseExistingCallsiteNoOp &&
      !existingCallsitePatchAbsorbedByDirectCandidate &&
      !conflictingConcreteSubtreeWitnessForcesWholeCover && existingPatch &&
      existingIsCallsite && existingPatch->proof.preservesInvocationStructure &&
      existingPatch->proof.proofRootMacroId == m.id && !baseInvText.empty() &&
      RefoldLineObserverLayout::InvocationSpanMatchesCallsitePrefix(baseInvText, m);

  std::optional<WholeCoverPlan> wholeCoverPlan;
  bool canReuseExistingExpanded = false;

  // Expanded, non-structure-preserving patches are reusable only when they are
  // already proven for this same macro owner/root and still match the current
  // whole-cover plan. `__COUNTER__` is handled separately because its literal
  // realization proof is not a normal whole-cover realization.
  if (existingExpandedPatch &&
      !existingExpandedPatch->proof.preservesInvocationStructure &&
      MacroPatchOwnerMatches(*existingExpandedPatch, currentPatchOwner)) {
    if (existingExpandedPatch->proof.proofRootMacroId == m.id) {
      if (existingExpandedPatch->proof.kind ==
              MacroPatchProofKind::CounterLiteral &&
          m.name == "__COUNTER__") {
        canReuseExistingExpanded = true;
      } else if (existingExpandedPatch->proof.kind ==
                 MacroPatchProofKind::WholeCoverRealization) {
        wholeCoverPlan = ComputeWholeCoverPlan(m);
        if (wholeCoverPlan)
          canReuseExistingExpanded = WholeCoverPatchMatchesPlan(
              *existingExpandedPatch, *wholeCoverPlan, m.id);
      }
    }
  }

  // Ensure the whole-cover plan is available for later selector logic,
  // even when no existing expanded patch was eligible for reuse.
  if (!wholeCoverPlan)
    wholeCoverPlan = ComputeWholeCoverPlan(m);


  // Rebuild the root ancestry index used by the final DAG/subtree stability
  // gate.  The gate only borrows this local storage and does not affect final
  // candidate ordering or proof ranking.
  DenseMap<uint64_t, const RefoldModel::MacroInvocation *> finalSubtreeInvById;
  finalSubtreeInvById.reserve((*deps_.model).GetMacroInvocations().size());
  for (const RefoldModel::MacroInvocation &inv :
       (*deps_.model).GetMacroInvocations())
    finalSubtreeInvById[inv.id] = &inv;
  SmallVector<std::pair<size_t, size_t>, 1> finalSubtreeRootArgRanges;
  const MacroSubtreeReplayValidationContext finalSubtreeValidationCtx{
      m, baseInvText, finalSubtreeRootArgRanges, finalSubtreeInvById};

  // Final candidate admission and proof-lattice selection are centralized
  // after candidate discovery. The recursive DAG/certificate solver state
  // remains local, while final admission order and stamping behavior are
  // handled by the shared selector below.
  WholeCoverFinalSelectionContext finalSelectionCtx{
      m,
      hEff,
      baseInvText,
      *invStart,
      *invEnd,
      argsOnlyCandidate,
      dagRootCandidate,
      canReuseExistingCallsiteNoOp,
      canReuseExistingCallsiteSkipWholeCover,
      canReuseExistingExpanded,
      existingPatch,
      existingExpandedPatch,
      wholeCoverPlan,
      reuseAdmissionCtx,
      finalSubtreeValidationCtx,
      allowNonTopLevelMacroSelectorFailure};
  return SelectWholeCoverPatch(finalSelectionCtx);
}


} // namespace refold
} // namespace clang
