//===--- RefoldNeutralityProof.cpp ------------------------------*- C++ -*-===//
//
// Source-neutrality proof subsystem — implementation.
//
// This file owns:
//   * the structural proof algorithms for zero-token macro neutrality
//     templates and their lexical helpers, and
//   * the TU/header policy adapter that wires them into RefoldEngine
//     callers.
//
// External callers see only the public surface declared in
// RefoldNeutralityProof.h.  Everything else lives in the anonymous
// namespace so the single-TU inlining behavior is preserved and the header
// does not drag a raw-lexer/state-directive dependency into every caller.
//
//===----------------------------------------------------------------------===//

#include "proof/RefoldNeutralityProof.h"

#include "macro/RefoldMacroStateProof.h"
#include "proof/RefoldOwnerStateProof.h"
#include "util/RefoldPathIdentity.h"
#include "util/StringUtils.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/ScopeExit.h"

#include <algorithm>
#include <cstddef>
#include <string>
#include <utility>

using namespace llvm;

namespace clang {
namespace refold {

namespace {

//===----------------------------------------------------------------------===//
// Zero-token macro-neutrality scanner and forwarding proof.
//===----------------------------------------------------------------------===//

/// Return true when the recorded invocation contributed material PP-token
/// output at any expansion surface.
///
/// Zero-token source-gap proofs may only consume macro calls that produced no
/// ordinary cover tokens, stringify spans, or paste spans.  Keeping this test
/// shared prevents the TU and header materialization paths from diverging on
/// what counts as source-bearing macro output.
bool refoldMacroInvocationHasMaterializedPPTokens(
    const RefoldModel::MacroInvocation &m) {
  if (m.cover.IsValid())
    return true;
  for (const auto &span : m.stringifySpans)
    if (span.IsValid())
      return true;
  for (const auto &span : m.pasteSpans)
    if (span.IsValid())
      return true;
  return false;
}

/// Source surface being checked by a zero-token macro-neutrality proof.
///
/// Definition-site checks slice the producer-recorded #define directive text;
/// invocation-argument checks slice the caller-provided materialized callsite
/// bytes.  The explicit origin prevents path equality alone from choosing
/// between two byte spaces that may share one physical file.
enum class RefoldZeroTokenRangeSurface {
  DefinitionReplacementList,
  InvocationArgument
};

/// Return the byte offset immediately after the balanced __VA_OPT__ payload.
///
/// \p openParen must name the opening parenthesis following `__VA_OPT__` in a
/// macro replacement list.  The scanner recognizes balanced parentheses while
/// skipping string/character literals and block comments.  Newlines, line
/// comments, unterminated literals/comments, and unbalanced parentheses reject
/// the payload because the caller's zero-token proof is intentionally lexical
/// and fail-closed.
std::optional<size_t> refoldParseVaOptPayloadEnd(StringRef replacementText,
                                                 size_t openParen) {
  if (openParen >= replacementText.size() || replacementText[openParen] != '(')
    return std::nullopt;

  unsigned depth = 1;
  for (size_t pos = openParen + 1; pos < replacementText.size();) {
    const char ch = replacementText[pos];
    if (ch == '\n' || ch == '\r')
      return std::nullopt;

    if (ch == '"' || ch == '\'') {
      const char quote = ch;
      ++pos;
      bool closed = false;
      while (pos < replacementText.size()) {
        const char litCh = replacementText[pos++];
        if (litCh == '\n' || litCh == '\r')
          return std::nullopt;
        if (litCh == '\\') {
          if (pos >= replacementText.size())
            return std::nullopt;
          ++pos;
          continue;
        }
        if (litCh == quote) {
          closed = true;
          break;
        }
      }
      if (!closed)
        return std::nullopt;
      continue;
    }

    if (pos + 1 < replacementText.size() && ch == '/') {
      if (replacementText[pos + 1] == '*') {
        pos += 2;
        bool closed = false;
        while (pos + 1 < replacementText.size()) {
          if (replacementText[pos] == '*' && replacementText[pos + 1] == '/') {
            pos += 2;
            closed = true;
            break;
          }
          ++pos;
        }
        if (!closed)
          return std::nullopt;
        continue;
      }
      if (replacementText[pos + 1] == '/')
        return std::nullopt;
    }

    if (ch == '(') {
      ++depth;
      ++pos;
      continue;
    }
    if (ch == ')') {
      --depth;
      if (depth == 0)
        return pos + 1;
      ++pos;
      continue;
    }
    ++pos;
  }
  return std::nullopt;
}

/// Advance \p pos over replacement-list trivia until a non-trivia byte or
/// \p limit is reached.
///
/// This intentionally mirrors the local replacement-list trivia scanner:
/// horizontal and newline whitespace are trivia, block comments must be
/// complete before \p limit, and a line comment is trivia only when it
/// extends exactly to the requested limit.  Returning false means the
/// replacement fragment cannot be proved neutral by this lexical scanner.
bool refoldSkipReplacementTriviaUntil(StringRef text, size_t &pos,
                                      size_t limit) {
  if (limit > text.size())
    return false;
  while (pos < limit) {
    if (stringutils::isWs(text[pos])) {
      ++pos;
      continue;
    }

    StringRef tail = text.substr(pos, limit - pos);
    if (tail.starts_with("/*")) {
      const size_t commentEnd = text.find("*/", pos + 2);
      if (commentEnd == StringRef::npos || commentEnd + 2 > limit)
        return false;
      pos = commentEnd + 2;
      continue;
    }
    if (tail.starts_with("//")) {
      const size_t commentEnd = text.find('\n', pos + 2);
      if (commentEnd != StringRef::npos && commentEnd < limit)
        return false;
      pos = limit;
      continue;
    }
    return true;
  }
  return true;
}

/// Consume a token-paste chain only in the placemarker domain.
///
/// A paste chain is neutral only when every operand is a formal parameter and
/// every corresponding invocation argument has already discharged the caller's
/// token-empty argument proof.  This admits forms such as
/// `#define CAT(a,b) a ## b` with `CAT(,)`, while still rejecting literals,
/// non-empty operands, unrecorded identifiers, and paste chains that
/// synthesize real PP-token material.
bool refoldConsumeNeutralPlacemarkerPasteChain(
    StringRef text, size_t &pos, size_t end, unsigned firstParam,
    const DenseMap<StringRef, unsigned> &paramIndexByName,
    function_ref<bool(unsigned)> invocationArgumentIsNeutral,
    SmallVectorImpl<unsigned> &usedParams) {
  size_t cursor = pos;
  if (!refoldSkipReplacementTriviaUntil(text, cursor, end))
    return false;
  if (cursor + 1 >= end || text[cursor] != '#' || text[cursor + 1] != '#')
    return false;

  if (!invocationArgumentIsNeutral(firstParam))
    return false;
  usedParams.push_back(firstParam);

  while (cursor + 1 < end && text[cursor] == '#' && text[cursor + 1] == '#') {
    cursor += 2;
    if (!refoldSkipReplacementTriviaUntil(text, cursor, end))
      return false;
    if (cursor >= end || !stringutils::isIdentStart(text[cursor]))
      return false;

    const size_t operandBegin = cursor++;
    while (cursor < end && stringutils::isIdentPart(text[cursor]))
      ++cursor;
    StringRef operand = text.slice(operandBegin, cursor);

    auto paramIt = paramIndexByName.find(operand);
    if (paramIt == paramIndexByName.end())
      return false;
    if (!invocationArgumentIsNeutral(paramIt->second))
      return false;
    usedParams.push_back(paramIt->second);

    if (!refoldSkipReplacementTriviaUntil(text, cursor, end))
      return false;
  }

  pos = cursor;
  return true;
}

/// Source interval occupied by a nested macro invocation that may tile part of
/// a zero-token neutrality proof.  The carrier is intentionally just the
/// producer-recorded source byte interval plus the child invocation pointer;
/// callers still decide which surface those bytes belong to and how the child
/// proves neutral.
struct RefoldNeutralMacroChildPiece {
  uint64_t begin = 0;
  uint64_t end = 0;
  const RefoldModel::MacroInvocation *child = nullptr;
};

/// Slice one of the two byte surfaces used by zero-token macro-neutrality
/// checks.
///
/// This centralizes only the common bounds mechanics.  The caller still proves
/// which physical path names the requested surface before passing the boolean
/// match facts, so path identity remains in the owning proof domain.
template <typename ReplacementList>
std::optional<StringRef> refoldSliceZeroTokenRangeSurfaceText(
    const ReplacementList &replacement, RefoldZeroTokenRangeSurface surface,
    uint64_t sliceBegin, uint64_t sliceEnd, StringRef invocationSurfaceText,
    bool rangeNamesReplacementDirective, bool rangeNamesInvocationSurface) {
  if (sliceEnd < sliceBegin)
    return std::nullopt;

  switch (surface) {
  case RefoldZeroTokenRangeSurface::DefinitionReplacementList: {
    if (!rangeNamesReplacementDirective)
      return std::nullopt;
    const uint64_t textBegin = replacement.fileBase;
    const uint64_t textEnd =
        replacement.fileBase + replacement.directive->text.size();
    if (sliceBegin < textBegin || sliceEnd > textEnd)
      return std::nullopt;
    return replacement.directive->text.slice(
        static_cast<size_t>(sliceBegin - replacement.fileBase),
        static_cast<size_t>(sliceEnd - replacement.fileBase));
  }
  case RefoldZeroTokenRangeSurface::InvocationArgument:
    if (!rangeNamesInvocationSurface || sliceEnd > invocationSurfaceText.size())
      return std::nullopt;
    return invocationSurfaceText.slice(sliceBegin, sliceEnd);
  }

  return std::nullopt;
}

/// Sort source-interval child pieces by begin/end/id so overlap checks are
/// deterministic across TU and header neutrality proofs.
void refoldSortNeutralChildPieces(
    SmallVectorImpl<RefoldNeutralMacroChildPiece> &children) {
  llvm::sort(children, [](const auto &lhs, const auto &rhs) {
    if (lhs.begin != rhs.begin)
      return lhs.begin < rhs.begin;
    if (lhs.end != rhs.end)
      return lhs.end < rhs.end;
    return lhs.child->id < rhs.child->id;
  });
}

/// Prove that a source interval is tiled by neutral nested macro children.
///
/// This is only the shared mechanics: collect direct nested invocations on the
/// requested file, sort them deterministically, reject overlaps, and require
/// every gap/suffix plus every child to satisfy caller-provided proofs.
/// Surface slicing, range-file membership, trivia classification, and
/// recursion remain local.
template <typename MacroInvocations, typename ChildBelongsToRange,
          typename SliceTrivia, typename IsNeutralGapTrivia,
          typename ChildIsNeutral>
bool refoldSourceRangeIsTiledByNeutralNestedMacros(
    const MacroInvocations &macroInvocations, uint64_t parentMacroId,
    uint64_t rangeBegin, uint64_t rangeEnd,
    ChildBelongsToRange &&childBelongsToRange, SliceTrivia &&sliceTrivia,
    IsNeutralGapTrivia &&isNeutralGapTrivia, ChildIsNeutral &&childIsNeutral) {
  if (rangeBegin > rangeEnd)
    return false;

  SmallVector<RefoldNeutralMacroChildPiece, 8> children;
  for (const auto &candidate : macroInvocations) {
    if (!candidate.callerMacroId || *candidate.callerMacroId != parentMacroId)
      continue;

    if (!childBelongsToRange(candidate))
      continue;
    if (!candidate.invB || !candidate.invE ||
        *candidate.invB >= *candidate.invE)
      continue;
    if (*candidate.invB < rangeBegin || rangeEnd < *candidate.invE)
      continue;
    if (!sliceTrivia(*candidate.invB, *candidate.invE))
      continue;

    children.push_back({*candidate.invB, *candidate.invE, &candidate});
  }

  refoldSortNeutralChildPieces(children);

  uint64_t cursor = rangeBegin;
  for (const RefoldNeutralMacroChildPiece &piece : children) {
    if (piece.begin < cursor)
      return false;

    std::optional<StringRef> gap = sliceTrivia(cursor, piece.begin);
    if (!gap || !isNeutralGapTrivia(*gap))
      return false;

    if (!childIsNeutral(*piece.child))
      return false;

    cursor = piece.end;
  }

  std::optional<StringRef> suffix = sliceTrivia(cursor, rangeEnd);
  return suffix && isNeutralGapTrivia(*suffix);
}

/// Prove that a requested zero-token byte range is tiled by neutral nested
/// macro invocations on either the definition replacement-list surface or the
/// invocation-argument surface.
///
/// The helper centralizes the byte-surface dispatch that was duplicated
/// between TU fallback and header materialization.  Callers still provide the
/// concrete invocation surface bytes/path, path-equivalence predicate,
/// neutral-trivia predicate, and recursive child proof, so this does not
/// change which ranges are considered replayable in either domain.
template <typename ReplacementList, typename MacroInvocations,
          typename PathEqual, typename IsNeutralTrivia, typename ChildIsNeutral>
bool refoldZeroTokenRangeIsTiledByNeutralNestedMacros(
    const MacroInvocations &macroInvocations,
    const RefoldModel::MacroInvocation &parent,
    const ReplacementList &replacement, uint64_t begin, uint64_t end,
    StringRef rangeFile, RefoldZeroTokenRangeSurface surface,
    StringRef invocationSurfaceText, StringRef invocationSurfacePath,
    PathEqual &&pathEqual, IsNeutralTrivia &&isNeutralTrivia,
    ChildIsNeutral &&childIsNeutral) {
  auto sliceSurfaceText = [&](uint64_t sliceBegin,
                              uint64_t sliceEnd) -> std::optional<StringRef> {
    return refoldSliceZeroTokenRangeSurfaceText(
        replacement, surface, sliceBegin, sliceEnd, invocationSurfaceText,
        pathEqual(rangeFile, replacement.directive->sitePath),
        pathEqual(rangeFile, invocationSurfacePath));
  };

  return refoldSourceRangeIsTiledByNeutralNestedMacros(
      macroInvocations, parent.id, begin, end,
      [&](const RefoldModel::MacroInvocation &candidate) {
        return candidate.invFile && !candidate.invFile->empty() &&
               pathEqual(*candidate.invFile, rangeFile);
      },
      sliceSurfaceText, isNeutralTrivia, childIsNeutral);
}

/// Prove the narrow object-like zero-token wrapper rule.
///
/// Object-like wrappers have no formal-argument substitution surface.  A
/// non-empty replacement list is source-neutral only when every non-trivia
/// byte in the defining directive's replacement-list interval is occupied by
/// a direct nested macro invocation, and every such child recursively proves
/// the same zero-token neutrality predicate.  Any malformed direct child,
/// child on a different source file, child outside the replacement interval,
/// overlap, or non-trivia gap rejects fail-closed.
///
/// The helper owns only this deterministic tiling rule.  It receives the
/// producer-recorded replacement-list bounds/text plus caller-supplied
/// direct-child membership, trivia, and recursive-child proofs; it does not
/// inspect RefoldEngine state or decide whether a TU/header proof should be
/// accepted.
template <typename MacroInvocations, typename ChildBelongsToReplacementFile,
          typename IsNeutralTrivia, typename ChildIsNeutral>
bool refoldObjectLikeNestedWrapperIsNeutral(
    const MacroInvocations &macroInvocations, uint64_t parentMacroId,
    uint64_t replacementFileBegin, uint64_t replacementFileEnd,
    uint64_t replacementFileBase, StringRef replacementDirectiveText,
    ChildBelongsToReplacementFile &&childBelongsToReplacementFile,
    IsNeutralTrivia &&isNeutralTrivia, ChildIsNeutral &&childIsNeutral) {
  if (replacementFileBegin > replacementFileEnd)
    return false;

  SmallVector<RefoldNeutralMacroChildPiece, 8> children;
  for (const auto &candidate : macroInvocations) {
    if (!candidate.callerMacroId || *candidate.callerMacroId != parentMacroId)
      continue;

    // Direct children of the wrapper are the only source intervals allowed to
    // account for non-trivia replacement-list bytes.  If the map records a
    // direct child that cannot participate in this exact definition-site
    // tiling, reject the wrapper rather than ignoring contradictory provenance.
    if (!childBelongsToReplacementFile(candidate))
      return false;
    if (!candidate.invB || !candidate.invE ||
        *candidate.invB >= *candidate.invE)
      return false;
    if (*candidate.invB < replacementFileBegin ||
        replacementFileEnd < *candidate.invE)
      return false;

    children.push_back({*candidate.invB, *candidate.invE, &candidate});
  }

  if (children.empty())
    return false;

  refoldSortNeutralChildPieces(children);

  uint64_t cursor = replacementFileBegin;
  for (const RefoldNeutralMacroChildPiece &piece : children) {
    if (piece.begin < cursor)
      return false;

    const size_t gapBegin = static_cast<size_t>(cursor - replacementFileBase);
    const size_t gapEnd =
        static_cast<size_t>(piece.begin - replacementFileBase);
    if (!isNeutralTrivia(replacementDirectiveText.slice(gapBegin, gapEnd)))
      return false;

    if (!childIsNeutral(*piece.child))
      return false;

    cursor = piece.end;
  }

  const size_t suffixBegin = static_cast<size_t>(cursor - replacementFileBase);
  return isNeutralTrivia(replacementDirectiveText.slice(
      suffixBegin, replacementDirectiveText.size()));
}

/// Walk a function-like macro replacement-list fragment in the zero-token
/// forwarding domain.
///
/// This helper factors only the lexical/control-flow skeleton shared by TU
/// fallback and header materialization.  It deliberately delegates every
/// surface-specific proof obligation to caller-supplied callbacks: tiling by
/// nested zero-token children, direct nested-child lookup, variadic-tail
/// neutrality, and invocation-argument neutrality.  Therefore this routine
/// does not broaden the proof domain; it merely keeps both callers using the
/// same fail-closed replacement-list scanner.
template <typename FragmentTilingProof, typename NeutralNestedChildProof,
          typename VariadicTailProof, typename ArgumentNeutralityProof>
bool refoldReplacementFragmentIsNeutral(
    StringRef text, size_t begin, size_t end,
    const DenseMap<StringRef, unsigned> &paramIndexByName,
    SmallVectorImpl<unsigned> &usedParams,
    FragmentTilingProof &fragmentIsTiledByNeutralChildren,
    NeutralNestedChildProof &neutralNestedChildEndAt,
    VariadicTailProof &proveVariadicTailNeutral,
    ArgumentNeutralityProof &invocationArgumentIsNeutral) {
  if (begin > end || end > text.size())
    return false;

  // A fragment made entirely of nested recorded zero-token macro invocations
  // plus trivia is neutral regardless of whether it is the whole replacement
  // list or the selected payload of __VA_OPT__.  The caller owns the actual
  // nested-child proof for its source surface.
  if (fragmentIsTiledByNeutralChildren(begin, end))
    return true;

  size_t pos = begin;
  while (pos < end) {
    if (!refoldSkipReplacementTriviaUntil(text, pos, end))
      return false;
    if (pos >= end)
      break;

    if (std::optional<size_t> childEnd = neutralNestedChildEndAt(pos, end)) {
      pos = *childEnd;
      continue;
    }

    if (!stringutils::isIdentStart(text[pos]))
      return false;
    const size_t identBegin = pos++;
    while (pos < end && stringutils::isIdentPart(text[pos]))
      ++pos;
    StringRef ident = text.slice(identBegin, pos);

    if (ident == "__VA_OPT__") {
      if (!refoldSkipReplacementTriviaUntil(text, pos, end))
        return false;
      std::optional<size_t> afterVaOpt = refoldParseVaOptPayloadEnd(text, pos);
      if (!afterVaOpt || *afterVaOpt > end)
        return false;

      const size_t payloadBegin = pos + 1;
      const size_t payloadEnd = *afterVaOpt - 1;

      // __VA_OPT__ is neutral in either of two proof-bounded cases:
      //   * the variadic tail is proved token-empty, so the payload is erased
      //     and need not be inspected; or
      //   * the payload itself is proved zero-token, so exposing it also
      //     contributes no PP tokens.
      // This branch-independent rule covers both erased forwarding forms and
      // non-erased payloads such as __VA_OPT__(EMPTY), without evaluating
      // arbitrary payload semantics.
      if (!proveVariadicTailNeutral() &&
          !refoldReplacementFragmentIsNeutral(
              text, payloadBegin, payloadEnd, paramIndexByName, usedParams,
              fragmentIsTiledByNeutralChildren, neutralNestedChildEndAt,
              proveVariadicTailNeutral, invocationArgumentIsNeutral))
        return false;

      pos = *afterVaOpt;
      continue;
    }

    // Outside __VA_OPT__, the only non-trivia replacement-list identifiers
    // admitted by the function-like wrapper proof are formal parameter names.
    // Their invocation argument ranges are checked once after the fragment
    // walk completes.
    auto it = paramIndexByName.find(ident);
    if (it == paramIndexByName.end())
      return false;

    // Token pasting can still be source-neutral in the placemarker domain.
    // The shared helper admits only paste chains whose operands are formals
    // with token-empty invocation arguments; any paste that could synthesize
    // real PP-token material remains outside this proof.
    if (refoldConsumeNeutralPlacemarkerPasteChain(
            text, pos, end, it->second, paramIndexByName,
            invocationArgumentIsNeutral, usedParams))
      continue;

    usedParams.push_back(it->second);
  }

  return true;
}

/// Find a direct nested macro invocation that starts exactly at a replacement
/// list byte offset and recursively proves zero-token neutrality.
///
/// This helper owns the duplicate-ownership rule shared by TU fallback and
/// header materialization: if two direct children claim the same local
/// replacement-list spelling, the proof rejects instead of choosing a child
/// by iteration order.  Callers still provide the path-equality predicate and
/// the recursive child-neutrality proof so no filesystem or engine policy
/// leaks into this shared lexical helper.
template <typename MacroInvocations, typename PathEqual,
          typename ChildIsNeutral>
std::optional<size_t> refoldNeutralNestedReplacementChildEndAt(
    const MacroInvocations &macroInvocations,
    const RefoldModel::MacroInvocation &parent, uint64_t replacementFileBase,
    StringRef replacementDirectiveText, StringRef replacementDirectivePath,
    size_t pos, size_t limit, PathEqual &&pathEqual,
    ChildIsNeutral &&childIsNeutral) {
  if (limit > replacementDirectiveText.size())
    return std::nullopt;

  const uint64_t filePos = replacementFileBase + static_cast<uint64_t>(pos);
  const uint64_t fileLimit = replacementFileBase + static_cast<uint64_t>(limit);

  const RefoldModel::MacroInvocation *matched = nullptr;
  for (const auto &candidate : macroInvocations) {
    if (!candidate.callerMacroId || *candidate.callerMacroId != parent.id)
      continue;
    if (!candidate.invFile || candidate.invFile->empty() ||
        !pathEqual(*candidate.invFile, replacementDirectivePath))
      continue;
    if (!candidate.invB || !candidate.invE ||
        *candidate.invB >= *candidate.invE)
      continue;
    if (*candidate.invB != filePos || *candidate.invE > fileLimit)
      continue;

    if (matched)
      return std::nullopt;
    matched = &candidate;
  }

  if (!matched)
    return std::nullopt;
  if (!childIsNeutral(*matched))
    return std::nullopt;
  return static_cast<size_t>(*matched->invE - replacementFileBase);
}

/// Prove the shared function-like zero-token forwarding rule.
///
/// TU fallback and header materialization have different byte surfaces and
/// recursive domains, but once those are supplied as callbacks the forwarding
/// rule is identical: every replacement-list formal that survives the lexical
/// walk must have an invocation argument tiled by neutral nested macro calls;
/// variadic formals and __VA_OPT__ are admitted only through that same
/// token-empty argument proof.  The helper deliberately returns only a
/// boolean and does not report why a proof failed, preserving the existing
/// fail-closed behavior of both callers.
template <typename ReplacementList, typename FragmentTilingProof,
          typename NeutralNestedChildProof,
          typename InvocationArgumentNeutralityProof>
bool refoldFunctionLikeZeroTokenForwardingIsNeutral(
    const RefoldModel::MacroInvocation &m, const ReplacementList &replacement,
    FragmentTilingProof &&fragmentIsTiledByNeutralChildren,
    NeutralNestedChildProof &&neutralNestedChildEndAt,
    InvocationArgumentNeutralityProof &&invocationArgumentIsNeutral) {
  if (m.defParams.empty() || m.invArgRanges.size() < m.defParams.size())
    return false;

  DenseMap<StringRef, unsigned> paramIndexByName;
  std::optional<unsigned> variadicParamIndex;
  for (unsigned i = 0, e = m.defParams.size(); i != e; ++i) {
    paramIndexByName.try_emplace(m.defParams[i].name, i);
    if (m.defParams[i].variadic) {
      if (variadicParamIndex)
        return false;
      variadicParamIndex = i;
    }
  }

  std::optional<bool> variadicTailIsNeutral;
  auto proveVariadicTailNeutral = [&]() -> bool {
    if (variadicTailIsNeutral)
      return *variadicTailIsNeutral;
    if (!variadicParamIndex || *variadicParamIndex >= m.invArgRanges.size()) {
      variadicTailIsNeutral = false;
      return false;
    }
    variadicTailIsNeutral = invocationArgumentIsNeutral(*variadicParamIndex);
    return *variadicTailIsNeutral;
  };

  SmallVector<unsigned, 4> usedParams;
  StringRef text = replacement.directive->text;
  if (!refoldReplacementFragmentIsNeutral(
          text, replacement.replacementTextBegin, text.size(), paramIndexByName,
          usedParams, fragmentIsTiledByNeutralChildren, neutralNestedChildEndAt,
          proveVariadicTailNeutral, invocationArgumentIsNeutral))
    return false;

  llvm::sort(usedParams);
  usedParams.erase(std::unique(usedParams.begin(), usedParams.end()),
                   usedParams.end());

  for (unsigned argIdx : usedParams)
    if (!invocationArgumentIsNeutral(argIdx))
      return false;

  return true;
}

/// Prove the complete recursive zero-token macro-neutrality rule.
///
/// TU fallback and header materialization use the same structural proof for a
/// zero-token macro invocation: the invocation must emit no material PP
/// tokens, its defining replacement list must be recoverable, recursive child
/// proofs must form an acyclic source tiling, and function-like forwarding
/// must only forward token-empty arguments.  The remaining differences are
/// true policy inputs supplied by the caller: the invocation source surface,
/// path equality, neutral-trivia classification, replacement-list recovery,
/// and whether the caller admits the whole-definition-list tiling shortcut
/// before subkind-specific wrapper checks.
template <typename MacroInvocations, typename RecoverReplacement,
          typename PathEqual, typename IsNeutralTrivia, typename ChildIsNeutral>
bool refoldMacroInvocationIsSourceNeutralZeroToken(
    const MacroInvocations &macroInvocations,
    const RefoldModel::MacroInvocation &m, DenseSet<uint64_t> &visiting,
    RecoverReplacement &&recoverReplacement, StringRef invocationSurfaceText,
    StringRef invocationSurfacePath, PathEqual &&pathEqual,
    IsNeutralTrivia &&isNeutralTrivia,
    bool allowWholeDefinitionListTilingBeforeSubkindCheck,
    ChildIsNeutral &&childIsNeutral) {
  if (refoldMacroInvocationHasMaterializedPPTokens(m))
    return false;

  // Reject recursive/cyclic macro invocation graphs fail-closed.  The proof
  // is structural recursion over producer-recorded child invocations, so
  // revisiting the same invocation would mean the map cannot establish a
  // finite neutral source tiling.
  if (!visiting.insert(m.id).second)
    return false;

  auto finish = [&](bool result) {
    visiting.erase(m.id);
    return result;
  };

  auto replacement = recoverReplacement(m);
  if (!replacement)
    return finish(false);

  StringRef replacementText =
      replacement->directive->text.substr(replacement->replacementTextBegin);

  // A literal empty replacement list is source-neutral by construction.
  if (isNeutralTrivia(replacementText))
    return finish(true);

  auto rangeIsTiledByNeutralNestedMacros =
      [&](uint64_t begin, uint64_t end, StringRef rangeFile,
          RefoldZeroTokenRangeSurface surface) -> bool {
    return refoldZeroTokenRangeIsTiledByNeutralNestedMacros(
        macroInvocations, m, *replacement, begin, end, rangeFile, surface,
        invocationSurfaceText, invocationSurfacePath, pathEqual,
        isNeutralTrivia, [&](const RefoldModel::MacroInvocation &child) {
          return childIsNeutral(child, visiting);
        });
  };

  // Header materialization accepts a whole replacement-list tiling before
  // subkind-specific checks.  TU fallback leaves this disabled so this shared
  // helper keeps both proof domains explicit.
  if (allowWholeDefinitionListTilingBeforeSubkindCheck &&
      rangeIsTiledByNeutralNestedMacros(
          replacement->fileBegin, replacement->fileEnd,
          replacement->directive->sitePath,
          RefoldZeroTokenRangeSurface::DefinitionReplacementList))
    return finish(true);

  if (m.subkind == "func") {
    auto invocationArgumentRangeIsNeutral = [&](unsigned argIdx) -> bool {
      const auto &range = m.invArgRanges[argIdx];
      if (!range.first || !range.second || *range.first > *range.second)
        return false;
      if (!m.invFile || m.invFile->empty())
        return false;
      return rangeIsTiledByNeutralNestedMacros(
          *range.first, *range.second, *m.invFile,
          RefoldZeroTokenRangeSurface::InvocationArgument);
    };

    StringRef text = replacement->directive->text;
    auto replacementFragmentIsTiledByNeutralChildren = [&](size_t begin,
                                                           size_t end) -> bool {
      if (begin > end || end > text.size())
        return false;
      return rangeIsTiledByNeutralNestedMacros(
          replacement->fileBase + static_cast<uint64_t>(begin),
          replacement->fileBase + static_cast<uint64_t>(end),
          replacement->directive->sitePath,
          RefoldZeroTokenRangeSurface::DefinitionReplacementList);
    };

    auto neutralNestedReplacementChildEndAt =
        [&](size_t pos, size_t limit) -> std::optional<size_t> {
      return refoldNeutralNestedReplacementChildEndAt(
          macroInvocations, m, replacement->fileBase, text,
          replacement->directive->sitePath, pos, limit, pathEqual,
          [&](const RefoldModel::MacroInvocation &child) {
            return childIsNeutral(child, visiting);
          });
    };

    return finish(refoldFunctionLikeZeroTokenForwardingIsNeutral(
        m, *replacement, replacementFragmentIsTiledByNeutralChildren,
        neutralNestedReplacementChildEndAt, invocationArgumentRangeIsNeutral));
  }

  if (m.subkind != "obj")
    return finish(false);

  // Object-like wrappers are neutral only when their definition-site
  // replacement list is tiled entirely by direct nested zero-token macro
  // invocations plus trivia.
  return finish(refoldObjectLikeNestedWrapperIsNeutral(
      macroInvocations, m.id, replacement->fileBegin, replacement->fileEnd,
      replacement->fileBase, replacement->directive->text,
      [&](const RefoldModel::MacroInvocation &candidate) {
        return candidate.invFile && !candidate.invFile->empty() &&
               pathEqual(*candidate.invFile, replacement->directive->sitePath);
      },
      isNeutralTrivia,
      [&](const RefoldModel::MacroInvocation &child) {
        return childIsNeutral(child, visiting);
      }));
}

//===----------------------------------------------------------------------===//
// Balanced diagnostic-pragma island helpers.
//===----------------------------------------------------------------------===//

/// Return the normalized spelling used to compare pragma-state islands
/// across source and B replay surfaces.
std::optional<std::string>
canonicalDiagnosticPragmaStateDirectiveText(StringRef text,
                                            const LangOptions &lang) {
  std::optional<ParsedDiagnosticPragmaStateDirective> parsed =
      parseDiagnosticPragmaStateDirective(text, lang);
  if (!parsed)
    return std::nullopt;

  std::string out;
  out += "#pragma ";
  out += parsed->namespaceName;
  out += " diagnostic ";
  out += parsed->actionName;
  if (parsed->action == DiagnosticPragmaStateAction::Setting) {
    out += " ";
    out += parsed->optionSpelling;
  }
  out += "\n";
  return out;
}

/// Canonicalize \p text iff it is exactly one locally balanced diagnostic
/// pragma-state island plus trivia.
///
/// This function is the replay-side counterpart to
/// collectBalancedDiagnosticPragmaStateIslands().  The source collector
/// proves that an island has identity net state; this helper proves that a B
/// replay surface already contains the same state island, so emission must
/// not append the original source island a second time.
std::optional<std::string>
canonicalBalancedDiagnosticPragmaStateIslandText(StringRef text,
                                                 const LangOptions &lang) {
  std::string canonical;
  std::optional<StringRef> namespaceName;
  unsigned depth = 0;
  bool sawDirective = false;

  size_t cursor = 0;
  while (cursor < text.size()) {
    size_t lineEnd = cursor;
    while (lineEnd < text.size() && text[lineEnd] != '\n')
      ++lineEnd;
    const size_t next = lineEnd < text.size() ? lineEnd + 1 : lineEnd;
    StringRef line = text.slice(cursor, next);

    if (!stringutils::lineStartsWithDirectiveKeyword(line, "pragma")) {
      if (!sourceTextIsOnlyWhitespaceAndCompleteComments(line, lang))
        return std::nullopt;
      cursor = next;
      continue;
    }

    std::optional<ParsedDiagnosticPragmaStateDirective> parsed =
        parseDiagnosticPragmaStateDirective(line, lang);
    if (!parsed)
      return std::nullopt;
    if (namespaceName && *namespaceName != parsed->namespaceName)
      return std::nullopt;
    namespaceName = parsed->namespaceName;

    switch (parsed->action) {
    case DiagnosticPragmaStateAction::Push:
      ++depth;
      break;
    case DiagnosticPragmaStateAction::Pop:
      if (depth == 0)
        return std::nullopt;
      --depth;
      break;
    case DiagnosticPragmaStateAction::Setting:
      if (depth == 0)
        return std::nullopt;
      break;
    }

    std::optional<std::string> directiveCanonical =
        canonicalDiagnosticPragmaStateDirectiveText(line, lang);
    if (!directiveCanonical)
      return std::nullopt;
    canonical += *directiveCanonical;
    sawDirective = true;
    cursor = next;
  }

  if (!sawDirective || depth != 0)
    return std::nullopt;
  return canonical;
}

//===----------------------------------------------------------------------===//
// Conditional-island neutrality proof.
//===----------------------------------------------------------------------===//

/// Owner-specific hooks for the shared neutral conditional-island proof.
///
/// The proof itself is owner-polymorphic: TU gaps, pure include-closure
/// gaps, and header-owned gaps have the same first-order invariant, but
/// differ in how model records are associated with the current owner
/// surface and how neutral include/macro artifacts are discharged.
struct NeutralConditionalIslandPolicy {
  StringRef sourceText;
  bool requireGroupBeginAtLineStart = false;
  NeutralConditionalArmSpanMode armSpanMode =
      NeutralConditionalArmSpanMode::SelectedArmsOnly;

  function_ref<bool(const RefoldModel::CondGroup &)> groupBelongs;
  function_ref<bool(const RefoldModel::IncludeItem &)> includeBelongs;
  function_ref<bool(const RefoldModel::IncludeItem &)> includeIsNeutral;
  function_ref<bool(const RefoldModel::PragmaDirective &)> pragmaBelongs;
  function_ref<bool(const RefoldModel::MacroInvocation &)> macroBelongs;
  function_ref<bool(const RefoldModel::MacroInvocation &)> macroIsNeutral;
};

/// Return true iff \p arm has A-side PP material that would make a
/// preserved conditional island source-bearing under \p mode.
bool neutralConditionalArmHasMaterializedTokens(
    const RefoldModel::CondArm &arm, NeutralConditionalArmSpanMode mode) {
  if (mode == NeutralConditionalArmSpanMode::SelectedArmsOnly && !arm.selected)
    return false;
  return arm.span && arm.span->IsValid() && arm.span->begin < arm.span->end;
}

/// Recursive implementation for neutral conditional-island proof.
///
/// A complete conditional group may be preserved as neutral source iff it is
/// wholly inside the source gap, has no materialized PP tokens under the
/// caller-selected arm policy, and every recorded arm-body artifact is either
/// owned by a recursively neutral nested conditional island or independently
/// discharges the appropriate neutral macro/include proof.
bool conditionalGroupIsNeutralIslandImpl(
    const RefoldModel &model, const RefoldModel::CondGroup &group,
    uint64_t gapBegin, uint64_t gapEnd,
    const NeutralConditionalIslandPolicy &policy,
    SmallVectorImpl<uint64_t> &recursionStack) {
  if (!policy.groupBelongs(group))
    return false;
  if (group.groupB >= group.groupE || group.groupE > policy.sourceText.size())
    return false;
  if (group.groupB < gapBegin || gapEnd < group.groupE)
    return false;
  if (policy.requireGroupBeginAtLineStart &&
      !stringutils::beginsLineAfterWs(policy.sourceText, group.groupB))
    return false;

  for (uint64_t activeId : recursionStack)
    if (activeId == group.id)
      return false;

  for (const RefoldModel::CondArm &arm : group.arms)
    if (neutralConditionalArmHasMaterializedTokens(arm, policy.armSpanMode))
      return false;

  recursionStack.push_back(group.id);
  auto popStack = llvm::make_scope_exit([&] { recursionStack.pop_back(); });

  auto insideGroup = [&](uint64_t b, uint64_t e) {
    return group.groupB <= b && b < e && e <= group.groupE;
  };

  auto insideAnyArmBody = [&](uint64_t b, uint64_t e) {
    for (const RefoldModel::CondArm &arm : group.arms)
      if (arm.bodyB <= b && b < e && e <= arm.bodyE)
        return true;
    return false;
  };

  auto insideNeutralNestedConditional = [&](uint64_t b, uint64_t e) {
    for (const auto &nested : model.GetConds()) {
      if (nested.id == group.id)
        continue;
      if (!policy.groupBelongs(nested))
        continue;
      if (nested.groupB < group.groupB || group.groupE < nested.groupE)
        continue;
      if (!(nested.groupB <= b && b < e && e <= nested.groupE))
        continue;
      if (!insideAnyArmBody(nested.groupB, nested.groupE))
        continue;
      if (conditionalGroupIsNeutralIslandImpl(model, nested, group.groupB,
                                              group.groupE, policy,
                                              recursionStack))
        return true;
    }
    return false;
  };

  for (const auto &inc : model.GetIncludes()) {
    if (!policy.includeBelongs(inc) || !insideGroup(inc.siteB, inc.siteE))
      continue;
    if (insideNeutralNestedConditional(inc.siteB, inc.siteE))
      continue;
    if (insideAnyArmBody(inc.siteB, inc.siteE) && policy.includeIsNeutral(inc))
      continue;
    return false;
  }

  // Macro-state directives inside the preserved group are allowed.  Unlike a
  // consumed opaque gap, a preserved complete conditional island carries the
  // directive spelling forward in source order, so active #define/#undef state
  // transitions remain visible to the suffix exactly through the source text
  // that originally produced them.  Nested conditional islands already own
  // their inner directives recursively, so there is no separate directive
  // rejection here.  Pragmas remain fail-closed below because the refold map
  // does not currently model their state domain precisely enough to prove
  // that preserving the surrounding island preserves every relevant compiler
  // state interaction.

  for (const auto &pragma : model.GetPragmas()) {
    if (policy.pragmaBelongs(pragma) &&
        insideGroup(pragma.siteB, pragma.siteE) &&
        !insideNeutralNestedConditional(pragma.siteB, pragma.siteE))
      return false;
  }

  // Macro invocations in #if/#elif control lines are allowed because the
  // complete conditional group is preserved verbatim.  Arm-body macro
  // invocations must either belong to a neutral nested conditional island or
  // independently prove source-neutral zero-token behavior.
  for (const auto &macro : model.GetMacroInvocations()) {
    if (!policy.macroBelongs(macro) || !macro.invB || !macro.invE ||
        !insideGroup(*macro.invB, *macro.invE) ||
        !insideAnyArmBody(*macro.invB, *macro.invE))
      continue;
    if (insideNeutralNestedConditional(*macro.invB, *macro.invE))
      continue;
    if (policy.macroIsNeutral(macro))
      continue;
    return false;
  }

  return true;
}

/// Return true iff \p group discharges the shared neutral conditional-island
/// proof under the caller-provided owner policy.
bool conditionalGroupIsNeutralIsland(
    const RefoldModel &model, const RefoldModel::CondGroup &group,
    uint64_t gapBegin, uint64_t gapEnd,
    const NeutralConditionalIslandPolicy &policy) {
  SmallVector<uint64_t, 8> recursionStack;
  return conditionalGroupIsNeutralIslandImpl(model, group, gapBegin, gapEnd,
                                             policy, recursionStack);
}

//===----------------------------------------------------------------------===//
// Zero-token macro-neutrality dispatch for the TU/header adapter.
//===----------------------------------------------------------------------===//

/// Common source surface used by recursive zero-token macro neutrality proof.
///
/// The surface is either the original TU bytes or the currently materialized
/// header bytes.  The caller-controlled whole-definition-list tiling policy
/// is preserved as explicit data because TU fallback and header
/// materialization intentionally use different explicit admission domains.
/// The neutral-trivia predicate is supplied by the caller as a function
/// pointer so this proof layer does not take a line-control parser
/// dependency merely to classify whitespace/comments.
struct ZeroTokenMacroNeutralityContext {
  const RefoldModel &model;
  const RefoldMacroStateProof &macroStateProof;
  const RefoldPathIdentity &paths;
  StringRef sourceText;
  StringRef sourcePath;
  bool (*isNeutralTrivia)(StringRef);
  bool allowWholeDefinitionListTilingBeforeSubkindCheck = false;
};

bool macroInvocationIsSourceNeutralZeroTokenImpl(
    const ZeroTokenMacroNeutralityContext &context,
    const RefoldModel::MacroInvocation &invocation,
    DenseSet<uint64_t> &visiting) {
  return refoldMacroInvocationIsSourceNeutralZeroToken(
      context.model.GetMacroInvocations(), invocation, visiting,
      [&](const RefoldModel::MacroInvocation &candidate) {
        return context.macroStateProof
            .RecoverMacroDefinitionReplacementListInterval(candidate);
      },
      context.sourceText, context.sourcePath,
      [&](StringRef lhs, StringRef rhs) {
        return context.paths.PathsEqual(lhs, rhs);
      },
      [&](StringRef text) { return context.isNeutralTrivia(text); },
      context.allowWholeDefinitionListTilingBeforeSubkindCheck,
      [&](const RefoldModel::MacroInvocation &child,
          DenseSet<uint64_t> &childVisiting) {
        return macroInvocationIsSourceNeutralZeroTokenImpl(context, child,
                                                           childVisiting);
      });
}

bool macroInvocationIsSourceNeutralZeroToken(
    const ZeroTokenMacroNeutralityContext &context,
    const RefoldModel::MacroInvocation &invocation) {
  DenseSet<uint64_t> visiting;
  return macroInvocationIsSourceNeutralZeroTokenImpl(context, invocation,
                                                     visiting);
}

} // namespace

//===----------------------------------------------------------------------===//
// Public non-adapter entry points.
//===----------------------------------------------------------------------===//

bool sourceTextIsOnlyIgnorableGapTrivia(StringRef text,
                                        const LangOptions &lang) {
  return sourceTextIsOnlyWhitespaceAndCompleteComments(text, lang);
}

/// Return whether one recorded pragma is a `#pragma once`.
///
/// Matched on the producer-recorded directive text after the `#`/`pragma`
/// keywords, so spacing and comment placement do not change the answer.
bool pragmaDirectiveIsPragmaOnce(
    const RefoldModel::PragmaDirective &pragma, const LangOptions &lang) {
  std::vector<PPTok> tokens;
  std::vector<size_t> offsets;
  lexPPTokens(pragma.text.str(), tokens, offsets, lang);

  // `# pragma once` lexes as `#`, `pragma`, `once`; the operator spelling
  // `_Pragma("once")` carries its argument as a string literal instead.
  for (size_t index = 0; index + 1 < tokens.size(); ++index)
    if (tokens[index].spelling == "pragma" && tokens[index + 1].spelling == "once")
      return true;
  return false;
}

void collectPreservedPragmaStateIntervals(
    const RefoldModel &model, StringRef ownerBytes, uint64_t gapBegin,
    uint64_t gapEnd,
    function_ref<bool(const RefoldModel::PragmaDirective &)> pragmaBelongs,
    SmallVectorImpl<BalancedDiagnosticPragmaStateIsland> &out,
    const LangOptions &lang) {
  if (gapBegin >= gapEnd || gapEnd > ownerBytes.size())
    return;

  for (const auto &pragma : model.GetPragmas()) {
    if (!pragmaBelongs(pragma))
      continue;
    if (gapBegin > pragma.siteB || pragma.siteB >= pragma.siteE ||
        pragma.siteE > gapEnd)
      continue;
    if (pragmaDirectiveIsPragmaOnce(pragma, lang))
      continue;

    BalancedDiagnosticPragmaStateIsland interval;
    interval.begin = pragma.siteB;
    interval.end = pragma.siteE;
    interval.id = pragma.id;
    out.push_back(interval);
  }

  // Deterministic order, matching the balanced-island collector.
  llvm::sort(out, [](const BalancedDiagnosticPragmaStateIsland &lhs,
                     const BalancedDiagnosticPragmaStateIsland &rhs) {
    if (lhs.begin != rhs.begin)
      return lhs.begin < rhs.begin;
    if (lhs.end != rhs.end)
      return lhs.end < rhs.end;
    return lhs.id < rhs.id;
  });
}

void collectBalancedDiagnosticPragmaStateIslands(
    const RefoldModel &model, StringRef ownerBytes, uint64_t gapBegin,
    uint64_t gapEnd,
    function_ref<bool(const RefoldModel::PragmaDirective &)> pragmaBelongs,
    SmallVectorImpl<BalancedDiagnosticPragmaStateIsland> &out,
    const LangOptions &lang) {
  if (gapBegin >= gapEnd || gapEnd > ownerBytes.size())
    return;

  SmallVector<const RefoldModel::PragmaDirective *, 8> pragmas;
  for (const auto &pragma : model.GetPragmas()) {
    if (!pragmaBelongs(pragma))
      continue;
    if (gapBegin <= pragma.siteB && pragma.siteB < pragma.siteE &&
        pragma.siteE <= gapEnd)
      pragmas.push_back(&pragma);
  }
  if (pragmas.empty())
    return;

  llvm::sort(pragmas, [](const RefoldModel::PragmaDirective *lhs,
                         const RefoldModel::PragmaDirective *rhs) {
    if (lhs->siteB != rhs->siteB)
      return lhs->siteB < rhs->siteB;
    if (lhs->siteE != rhs->siteE)
      return lhs->siteE < rhs->siteE;
    return lhs->id < rhs->id;
  });

  for (size_t i = 0; i < pragmas.size(); ++i) {
    const RefoldModel::PragmaDirective *first = pragmas[i];
    std::optional<ParsedDiagnosticPragmaStateDirective> firstParsed =
        parseDiagnosticPragmaStateDirective(first->text, lang);
    if (!firstParsed ||
        firstParsed->action != DiagnosticPragmaStateAction::Push) {
      continue;
    }

    unsigned depth = 0;
    uint64_t islandEnd = first->siteB;
    bool valid = true;

    for (size_t j = i; j < pragmas.size(); ++j) {
      const RefoldModel::PragmaDirective *cur = pragmas[j];
      if (cur->siteB < islandEnd ||
          !sourceTextIsOnlyWhitespaceAndCompleteComments(
              ownerBytes.slice(islandEnd, cur->siteB), lang)) {
        valid = false;
        break;
      }

      std::optional<ParsedDiagnosticPragmaStateDirective> parsed =
          parseDiagnosticPragmaStateDirective(cur->text, lang);
      if (!parsed || parsed->namespaceName != firstParsed->namespaceName) {
        valid = false;
        break;
      }

      switch (parsed->action) {
      case DiagnosticPragmaStateAction::Push:
        ++depth;
        break;
      case DiagnosticPragmaStateAction::Pop:
        if (depth == 0) {
          valid = false;
          break;
        }
        --depth;
        break;
      case DiagnosticPragmaStateAction::Setting:
        if (depth == 0) {
          valid = false;
          break;
        }
        break;
      }
      if (!valid)
        break;

      islandEnd = cur->siteE;
      if (depth == 0) {
        out.push_back({first->siteB, islandEnd, first->id});
        i = j;
        break;
      }
    }
  }
}

bool balancedDiagnosticPragmaStateIslandIsCarriedByReplacement(
    StringRef sourceIslandText, StringRef replacement,
    const LangOptions &lang) {
  std::optional<std::string> sourceCanonical =
      canonicalBalancedDiagnosticPragmaStateIslandText(sourceIslandText, lang);
  if (!sourceCanonical)
    return false;

  std::string currentCanonical;
  std::optional<StringRef> currentNamespace;
  unsigned depth = 0;

  auto reset = [&] {
    currentCanonical.clear();
    currentNamespace.reset();
    depth = 0;
  };

  size_t cursor = 0;
  while (cursor < replacement.size()) {
    size_t lineEnd = cursor;
    while (lineEnd < replacement.size() && replacement[lineEnd] != '\n')
      ++lineEnd;
    const size_t next = lineEnd < replacement.size() ? lineEnd + 1 : lineEnd;
    StringRef line = replacement.slice(cursor, next);

    if (!stringutils::lineStartsWithDirectiveKeyword(line, "pragma")) {
      if (!sourceTextIsOnlyWhitespaceAndCompleteComments(line, lang))
        reset();
      cursor = next;
      continue;
    }

    std::optional<ParsedDiagnosticPragmaStateDirective> parsed =
        parseDiagnosticPragmaStateDirective(line, lang);
    std::optional<std::string> directiveCanonical =
        canonicalDiagnosticPragmaStateDirectiveText(line, lang);
    if (!parsed || !directiveCanonical) {
      reset();
      cursor = next;
      continue;
    }

    if ((currentNamespace && *currentNamespace != parsed->namespaceName) ||
        (!currentCanonical.empty() && depth == 0))
      reset();
    currentNamespace = parsed->namespaceName;

    bool validAction = true;
    switch (parsed->action) {
    case DiagnosticPragmaStateAction::Push:
      ++depth;
      break;
    case DiagnosticPragmaStateAction::Pop:
      if (depth == 0) {
        validAction = false;
        break;
      }
      --depth;
      break;
    case DiagnosticPragmaStateAction::Setting:
      if (depth == 0)
        validAction = false;
      break;
    }

    if (!validAction) {
      reset();
      cursor = next;
      continue;
    }

    currentCanonical += *directiveCanonical;
    if (depth == 0) {
      if (currentCanonical == *sourceCanonical)
        return true;
      reset();
    }

    cursor = next;
  }

  return false;
}

//===----------------------------------------------------------------------===//
// TU/header source-neutrality policy adapter.
//===----------------------------------------------------------------------===//

TUSourceNeutralityContext
RefoldSourceNeutralityProof::BuildTUSourceNeutralityContext(
    const RefoldModel &model, const RefoldMacroStateProof &macroStateProof,
    const RefoldPathIdentity &paths, StringRef tuBytes, StringRef tuPath,
    bool (*isNeutralTrivia)(StringRef)) {
  return {model, macroStateProof, paths, tuBytes, tuPath, isNeutralTrivia};
}

HeaderSourceNeutralityContext
RefoldSourceNeutralityProof::BuildHeaderSourceNeutralityContext(
    const RefoldModel &model, const RefoldMacroStateProof &macroStateProof,
    const RefoldPathIdentity &paths, StringRef headerBytes,
    StringRef headerPath, uint64_t includeId,
    bool (*isNeutralTrivia)(StringRef)) {
  return {model,      macroStateProof, paths,    headerBytes,
          headerPath, isNeutralTrivia, includeId};
}

bool RefoldSourceNeutralityProof::MacroInvocationHasMaterializedPPTokens(
    const RefoldModel::MacroInvocation &invocation) {
  return refoldMacroInvocationHasMaterializedPPTokens(invocation);
}

bool RefoldSourceNeutralityProof::MacroInvocationIsSourceNeutralZeroToken(
    const TUSourceNeutralityContext &context,
    const RefoldModel::MacroInvocation &invocation) {
  ZeroTokenMacroNeutralityContext zeroContext{
      context.model,
      context.macroStateProof,
      context.paths,
      context.tuBytes,
      context.tuPath,
      context.isNeutralTrivia,
      /*allowWholeDefinitionListTilingBeforeSubkindCheck=*/false};
  return macroInvocationIsSourceNeutralZeroToken(zeroContext, invocation);
}

bool RefoldSourceNeutralityProof::MacroInvocationIsSourceNeutralZeroToken(
    const HeaderSourceNeutralityContext &context,
    const RefoldModel::MacroInvocation &invocation) {
  ZeroTokenMacroNeutralityContext zeroContext{
      context.model,
      context.macroStateProof,
      context.paths,
      context.headerBytes,
      context.headerPath,
      context.isNeutralTrivia,
      /*allowWholeDefinitionListTilingBeforeSubkindCheck=*/true};
  return macroInvocationIsSourceNeutralZeroToken(zeroContext, invocation);
}

bool RefoldSourceNeutralityProof::ConditionalGroupIsNeutralIsland(
    const TUSourceNeutralityContext &context,
    const RefoldModel::CondGroup &group, uint64_t gapBegin, uint64_t gapEnd,
    const NeutralConditionalIslandContext &islandContext,
    function_ref<bool(const RefoldModel::IncludeItem &)> includeIsNeutral) {
  auto groupBelongs = [&](const RefoldModel::CondGroup &candidate) {
    return context.paths.PathsEqual(candidate.file, context.tuPath) &&
           !candidate.parentIncludeId;
  };
  auto includeBelongs = [&](const RefoldModel::IncludeItem &include) {
    return context.paths.PathsEqual(include.sitePath, context.tuPath);
  };
  auto pragmaBelongs = [&](const RefoldModel::PragmaDirective &pragma) {
    return context.paths.PathsEqual(pragma.sitePath, context.tuPath);
  };
  auto macroBelongs = [&](const RefoldModel::MacroInvocation &macro) {
    return macro.invFile &&
           context.paths.PathsEqual(*macro.invFile, context.tuPath);
  };
  auto macroIsNeutral = [&](const RefoldModel::MacroInvocation &macro) {
    return MacroInvocationIsSourceNeutralZeroToken(context, macro);
  };

  NeutralConditionalIslandPolicy policy{
      context.tuBytes,
      islandContext.requireGroupBeginAtLineStart,
      islandContext.armSpanMode,
      groupBelongs,
      includeBelongs,
      includeIsNeutral,
      pragmaBelongs,
      macroBelongs,
      macroIsNeutral};
  return conditionalGroupIsNeutralIsland(context.model, group, gapBegin, gapEnd,
                                         policy);
}

bool RefoldSourceNeutralityProof::ConditionalGroupIsNeutralIsland(
    const HeaderSourceNeutralityContext &context,
    const RefoldModel::CondGroup &group, uint64_t gapBegin, uint64_t gapEnd,
    const NeutralConditionalIslandContext &islandContext,
    function_ref<bool(const RefoldModel::IncludeItem &)> includeIsNeutral) {
  auto groupBelongs = [&](const RefoldModel::CondGroup &candidate) {
    return context.paths.PathsEqual(candidate.file, context.headerPath) &&
           candidate.parentIncludeId &&
           *candidate.parentIncludeId == context.includeId;
  };
  auto includeBelongs = [&](const RefoldModel::IncludeItem &include) {
    return context.paths.PathsEqual(include.sitePath, context.headerPath) &&
           include.parent && *include.parent == context.includeId;
  };
  auto pragmaBelongs = [&](const RefoldModel::PragmaDirective &pragma) {
    return context.paths.PathsEqual(pragma.sitePath, context.headerPath);
  };
  auto macroBelongs = [&](const RefoldModel::MacroInvocation &macro) {
    return macro.invFile &&
           context.paths.PathsEqual(*macro.invFile, context.headerPath);
  };
  auto macroIsNeutral = [&](const RefoldModel::MacroInvocation &macro) {
    return MacroInvocationIsSourceNeutralZeroToken(context, macro);
  };

  NeutralConditionalIslandPolicy policy{
      context.headerBytes,
      islandContext.requireGroupBeginAtLineStart,
      islandContext.armSpanMode,
      groupBelongs,
      includeBelongs,
      includeIsNeutral,
      pragmaBelongs,
      macroBelongs,
      macroIsNeutral};
  return conditionalGroupIsNeutralIsland(context.model, group, gapBegin, gapEnd,
                                         policy);
}

} // namespace refold
} // namespace clang
