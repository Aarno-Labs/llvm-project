//===--- RefoldMacroPlannerHelpers.h --------------------------*- C++ -*-===//
//
// Small inline helpers shared by RefoldMacroPatchPlanner and macro-domain
// services such as the paste-argument builder, definition-tape solver,
// args-only template solver, and occurrence-proof validator.
//
// This header owns tiny value carriers and lexical helper predicates that are
// intentionally shared at namespace scope.  Larger algorithms remain in their
// owning services so planner orchestration, replay construction, and proof
// validation stay separated by responsibility.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDMACROPLANNERHELPERS_H
#define LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDMACROPLANNERHELPERS_H

#include "core/RefoldModel.h"
#include "edit/RefoldPatchTypes.h"
#include "macro/RefoldArgTextRecovery.h"
#include "proof/RefoldOwnerStateTypes.h"
#include "source/DiffAlgorithms.h"
#include "source/RefoldToken.h"

#include "clang/Basic/LangOptions.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>

namespace clang {
namespace refold {

/// Carrier returned by `BuildInvocationRewriteWithRange`.
///
/// `text` is the complete rewritten invocation spelling.  The materialized
/// output byte range is measured inside `text` and identifies the subrange that
/// realizes the edited B-side output.
struct InvocationRewriteWithRange {
  /// Complete rewritten invocation spelling.
  std::string text;
  /// Beginning of the materialized output range inside `text`.
  uint64_t materializedOutputByteStart = 0;
  /// End of the materialized output range inside `text`.
  uint64_t materializedOutputByteEnd = 0;
};

/// Borrowed invocation-actual recovery inputs shared by args-only rewrite
/// helpers and macro-domain replay engines.
///
/// The context does not own the invocation text or parsed argument ranges; it
/// names the exact callsite spelling surface used by replay helpers that must
/// rebuild an invocation without changing caller-owned storage.
struct InvocationActualRecoveryContext {
  /// Macro invocation whose actual spelling is being recovered.
  const RefoldModel::MacroInvocation &invocation;
  /// Complete source spelling of the invocation callsite.
  llvm::StringRef baseInvocationText;
  /// Formal-content byte ranges inside `baseInvocationText`.
  llvm::ArrayRef<std::pair<size_t, size_t>> invocationArgRanges;
};

/// One parsed formal-actual content range inside the invocation spelling.
/// The recovery service still exposes ranges as plain byte pairs; this
/// carrier names the meaning of those two offsets at API boundaries without
/// changing the underlying storage used by existing ArrayRef-based helpers.
struct ActualContentRange {
  /// Beginning of the actual-content byte range in invocation spelling.
  size_t begin = 0;
  /// End of the actual-content byte range in invocation spelling.
  size_t end = 0;
};

/// Complete parsed actual layout for one invocation spelling.  Owns the
/// recovered formal-content ranges for the duration of one args-only
/// planning attempt; downstream replay helpers borrow the pair-backed view.
struct InvocationActualLayout {
  /// Formal-content byte ranges for each recovered actual.
  std::vector<std::pair<size_t, size_t>> contentRanges;

  bool empty() const { return contentRanges.empty(); }

  llvm::ArrayRef<std::pair<size_t, size_t>> rangePairs() const {
    return contentRanges;
  }

  ActualContentRange rangeAt(size_t index) const {
    const auto &range = contentRanges[index];
    return ActualContentRange{range.first, range.second};
  }
};

/// Shared read-only state for the args-only planning pipeline.
///
/// Replay helpers borrow the recovered invocation layout through this carrier
/// so the entry point can orchestrate definition-tape replay, paste-aware
/// replay, and ordinary formal replay while keeping per-call state local to the
/// planning attempt.
struct ArgsOnlyPlanningContext {
  /// Root invocation being considered for args-only replay.
  const RefoldModel::MacroInvocation &invocation;
  /// Token-level hunk being replayed through the invocation actuals.
  const diffutils::Hunk &hunk;
  /// Complete source spelling of the root invocation.
  llvm::StringRef baseInvocationText;
  /// Recovered formal-content ranges for the invocation actuals.
  const InvocationActualLayout &actualLayout;
};

/// Explicit reuse-admission state for same-span macro patches.
///
/// The planner may learn about an existing patch through two routes: the
/// caller-owned patch map, or the caller-coalesced same-pass context passed
/// alongside the request.  This carrier keeps those reuse facts in one named
/// bundle so helper methods can reason about reuse without capturing scattered
/// local pointers and flags, while still borrowing all patch storage from the
/// caller.
struct MacroPatchReuseAdmissionContext {
  /// Invocation whose physical span is being checked for reuse.
  const RefoldModel::MacroInvocation &invocation;
  /// Owner assigned to the candidate patch being considered.
  const Owner &currentPatchOwner;
  /// Include owner id when `currentPatchOwner` is include-owned.
  std::optional<uint64_t> ownerIncludeId;
  /// Beginning of the physical invocation byte span in the owner file.
  uint64_t invocationStart = 0;
  /// End of the physical invocation byte span in the owner file.
  uint64_t invocationEnd = 0;

  /// Existing patch for the physical callsite span, if known.
  const MacroPatch *existingPatch = nullptr;
  /// Existing patch for an expanded/reused equivalent span, if known.
  const MacroPatch *existingExpandedPatch = nullptr;
  /// True when `existingPatch` names the physical callsite patch.
  bool existingIsCallsite = false;
  /// True when `existingPatch` was provided by the caller context rather than
  /// by the planner's patch map.
  bool existingCameFromCallerContext = false;
  /// True when `existingExpandedPatch` was provided by the caller context.
  bool existingExpandedCameFromCallerContext = false;
};

/// Certify the materialized B-token envelope on a macro patch.
inline void certifyMacroPatchMaterializedBTokenRange(MacroPatch &patch,
                                                     uint64_t bTokBegin,
                                                     uint64_t bTokEnd) {
  patch.materialized.hasBTokenRange = true;
  patch.materialized.bTokStart = bTokBegin;
  patch.materialized.bTokEnd = bTokEnd;
}

/// Return true when a function-like macro formal is variadic.
inline bool
isMacroInvocationVariadicFormal(const RefoldModel::MacroInvocation &mi,
                                size_t formalIdx) {
  return formalIdx < mi.defParams.size() && mi.defParams[formalIdx].variadic;
}

/// Canonically order producer argument spans by token range and formal index.
inline bool ppArgSpanLessByTokenRangeAndArg(const RefoldModel::PPArgSpan &lhs,
                                            const RefoldModel::PPArgSpan &rhs) {
  if (lhs.begin != rhs.begin)
    return lhs.begin < rhs.begin;
  if (lhs.end != rhs.end)
    return lhs.end < rhs.end;
  return lhs.argIdx < rhs.argIdx;
}

/// Detect a comma that would split a non-variadic macro actual. This helper
/// forwards to the raw-lexer implementation so all macro-domain call sites keep
/// the same macro-argument collection semantics (only parentheses protect
/// commas; brackets and braces do not).
inline bool replacementIntroducesTopLevelComma(llvm::StringRef text,
                                               const clang::LangOptions &lang) {
  return refoldMacroActualHasTopLevelComma(text, lang);
}

/// Return the unique trimmed occurrence of `needle` in `haystack`, if any.
inline std::optional<std::pair<size_t, size_t>>
findUniqueTrimmedSubstring(llvm::StringRef haystack, llvm::StringRef needle) {
  needle = needle.trim();
  if (needle.empty())
    return std::nullopt;
  size_t pos = haystack.find(needle);
  if (pos == llvm::StringRef::npos)
    return std::nullopt;
  if (haystack.find(needle, pos + 1) != llvm::StringRef::npos)
    return std::nullopt;
  return std::make_pair(pos, pos + needle.size());
}

/// Decode the restricted string-literal payloads that macro stringification
/// proofs can invert without changing the existing fail-closed policy.
/// Numeric and line-continuation escapes are intentionally rejected.
inline std::optional<std::string>
decodeSimpleStringLiteralToken(llvm::StringRef spelling) {
  size_t quote = spelling.find('"');
  if (quote == llvm::StringRef::npos)
    return std::nullopt;
  size_t endQuote = spelling.rfind('"');
  if (endQuote == llvm::StringRef::npos || endQuote <= quote)
    return std::nullopt;
  llvm::StringRef body = spelling.slice(quote + 1, endQuote);
  std::string out;
  out.reserve(body.size());
  for (size_t i = 0; i < body.size(); ++i) {
    if (body[i] != '\\') {
      out.push_back(body[i]);
      continue;
    }
    if (++i >= body.size())
      return std::nullopt;
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

/// True iff the producer proved the invocation callee comes from a literal
/// macro name rather than from a formal/argument-derived callee position.
inline bool
hasLiteralMacroCalleeOrigin(const RefoldModel::MacroInvocation &mi) {
  return mi.calleeOrigin.kind == MacroCalleeOriginKind::LiteralMacroName;
}

/// Return true when a macro directive parameter is variadic.
inline bool
isMacroDirectiveVariadicParam(const RefoldModel::MacroDirective &definition,
                              uint32_t paramIdx) {
  return paramIdx < definition.defParams.size() &&
         definition.defParams[paramIdx].variadic;
}

/// Return the index of the `)` closing a `__VA_OPT__(...)` payload, or
/// `std::nullopt` when the operator is not followed by a balanced payload
/// within `[vaOptIdx, end)`.
///
/// `vaOptIdx` indexes the `__VA_OPT__` token itself in
/// `definition.replacementTokens`; the payload therefore starts at
/// `vaOptIdx + 2`.  Only literal tokens can open or close the payload, so a
/// parameter reference spelled `(` cannot alter the nesting depth.
///
/// Every generated-callee replay family shares this scan, so a malformed or
/// unterminated payload rejects that family's proof identically rather than
/// being read as an operator by one parser and as ordinary text by another.
inline std::optional<size_t>
findVaOptPayloadClose(const RefoldModel::MacroDirective &definition,
                      size_t vaOptIdx, size_t end) {
  if (vaOptIdx + 1 >= end ||
      definition.replacementTokens[vaOptIdx + 1].kind !=
          RefoldModel::MacroReplacementTokenKind::Literal ||
      definition.replacementTokens[vaOptIdx + 1].spelling != "(")
    return std::nullopt;

  unsigned depth = 1;
  for (size_t close = vaOptIdx + 2; close < end; ++close) {
    const auto &inner = definition.replacementTokens[close];
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

/// Return true when a macro definition accepts a written actual count.
inline bool
macroDefinitionAcceptsActualCount(const RefoldModel::MacroDirective &definition,
                                  size_t count) {
  const bool hasVariadic =
      !definition.defParams.empty() && definition.defParams.back().variadic;
  const size_t fixedCount = hasVariadic ? definition.defParams.size() - 1
                                        : definition.defParams.size();
  return hasVariadic ? count >= fixedCount : count == fixedCount;
}

/// Return the macro definition directive for an invocation, or nullptr when
/// the directive identity is unavailable.
///
/// Macro-domain services share this canonical lookup instead of each holding a
/// back-reference to the planner.
inline const RefoldModel::MacroDirective *
getDefinitionDirectiveForInvocation(const RefoldModel &model,
                                    const RefoldModel::MacroInvocation &m) {
  if (!m.definitionDirectiveId)
    return nullptr;
  return model.GetMacroDirectiveById(*m.definitionDirectiveId);
}

/// Resolve \p startName through a deterministic object-like alias chain to the
/// unique function-like `#define` that it ultimately names.
///
/// Every hop must be a unique object-like `#define` whose replacement list is
/// exactly one literal token, and the terminal definition must be the unique
/// function-like `#define` for its name.  A name carrying two function-like
/// definitions, or two single-literal object-like definitions, is ambiguous
/// macro state and resolves to nullptr rather than to an arbitrary pick.  The
/// walk is bounded both by an explicit visited-name set and by the directive
/// table size, so alias cycles terminate without a result.
///
/// When \p aliasHops is non-null it receives the number of object-like hops
/// taken before the function-like definition was reached.
///
/// Only the per-name directive bucket is scanned.  Bucket order matches the
/// producer record order of GetMacroDirectives(), so the ambiguity decisions
/// above observe exactly the candidate sequence a filtered full scan would.
inline const RefoldModel::MacroDirective *
resolveFunctionLikeMacroThroughObjectAliases(const RefoldModel &model,
                                             llvm::StringRef startName,
                                             uint32_t *aliasHops = nullptr) {
  if (aliasHops)
    *aliasHops = 0;
  if (startName.empty())
    return nullptr;

  // Alias chains are short in practice; a linear probe over the visited names
  // avoids both hashing and the per-hop string allocation the previous
  // SmallVector<std::string> form paid.  Every name viewed here is either the
  // caller's argument or a producer-owned replacement-token spelling, so the
  // referenced bytes outlive this walk.
  llvm::SmallVector<llvm::StringRef, 8> seen;
  llvm::StringRef current = startName;
  for (size_t depth = 0; depth <= model.GetMacroDirectives().size(); ++depth) {
    if (llvm::is_contained(seen, current))
      return nullptr;
    seen.push_back(current);

    const RefoldModel::MacroDirective *functionLike = nullptr;
    const RefoldModel::MacroDirective *alias = nullptr;
    for (const RefoldModel::MacroDirective *directive :
         model.GetMacroDirectivesByName(current)) {
      if (!directive->IsDefine())
        continue;
      if (directive->functionLike) {
        if (functionLike)
          return nullptr;
        functionLike = directive;
        continue;
      }
      if (directive->replacementTokens.size() == 1 &&
          directive->replacementTokens[0].kind ==
              RefoldModel::MacroReplacementTokenKind::Literal) {
        if (alias)
          return nullptr;
        alias = directive;
      }
    }

    if (functionLike)
      return functionLike;
    if (!alias)
      return nullptr;
    if (aliasHops)
      ++*aliasHops;
    current = alias->replacementTokens[0].spelling;
  }
  return nullptr;
}

/// Return true when \p name has at least one object-like `#define` whose
/// replacement list is exactly one literal token.
///
/// This is the admission test for a single hop of the alias chain walked by
/// resolveFunctionLikeMacroThroughObjectAliases; it reports only that such a
/// definition exists, not that the hop is unambiguous.
inline bool isObjectLikeSingleTokenAliasName(const RefoldModel &model,
                                             llvm::StringRef name) {
  for (const RefoldModel::MacroDirective *directive :
       model.GetMacroDirectivesByName(name)) {
    if (!directive->IsObjectLikeDefine())
      continue;
    if (directive->replacementTokens.size() == 1 &&
        directive->replacementTokens[0].kind ==
            RefoldModel::MacroReplacementTokenKind::Literal)
      return true;
  }
  return false;
}

/// Build the minimal token-edit envelope spanning two pure-insertion frontiers.
inline diffutils::Hunk
buildCombinedInsertionEnvelope(const diffutils::Hunk &left,
                               const diffutils::Hunk &right) {
  diffutils::Hunk env;
  env.aStart = std::min(left.aStart, right.aStart);
  env.aEnd = std::max(left.aStart, right.aStart);
  env.bStart = std::min(left.bStart, right.bStart);
  env.bEnd = std::max(left.bEnd, right.bEnd);
  return env;
}

/// Remove identical A/B token spelling at both edges of a candidate hunk.
inline diffutils::Hunk trimCommonEdgeTokens(diffutils::Hunk hunk,
                                            llvm::ArrayRef<PPTok> aToks,
                                            llvm::ArrayRef<PPTok> bToks) {
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

/// Return true when the given invocation argument contributes to any pasted
/// token emitted by the invocation.
inline bool invocationArgTouchesPaste(const RefoldModel::MacroInvocation &mi,
                                      uint32_t argIdx) {
  for (const auto &ps : mi.pasteSpans) {
    if (ps.argIdx == argIdx)
      return true;
  }
  return false;
}

/// Collect the uint32_t keys of an associative container in deterministic
/// ascending order.
template <typename MapLike>
inline llvm::SmallVector<uint32_t, 8>
collectSortedUInt32Keys(const MapLike &mapLike) {
  llvm::SmallVector<uint32_t, 8> keys;
  keys.reserve(mapLike.size());
  for (const auto &kv : mapLike)
    keys.push_back(kv.first);
  llvm::sort(keys);
  return keys;
}

} // namespace refold
} // namespace clang

#endif // LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDMACROPLANNERHELPERS_H
