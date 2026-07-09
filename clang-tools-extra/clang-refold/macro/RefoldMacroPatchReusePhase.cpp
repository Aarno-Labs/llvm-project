//===--- RefoldMacroPatchReusePhase.cpp -------------------------*- C++ -*-===//
//
// Same-root macro-patch reuse, merge, and conflict utilities.
//
// The phase owns the predicates that decide whether a direct candidate, a DAG
// candidate, and an existing callsite/expanded patch can share one root proof
// without contradicting each other's replacement surface.  Dependencies carry
// only the planner services and callbacks needed for formal-range recovery,
// tuple parsing, lexical comparison, and plan matching.
//
//===----------------------------------------------------------------------===//

#include "macro/RefoldMacroPatchReusePhase.h"

#include "macro/RefoldMacroDAGSharedHelpers.h"
#include "macro/RefoldMacroWholeCoverPlanningContext.h"

#include "clang/Basic/SourceLocation.h"
#include "clang/Basic/TokenKinds.h"
#include "clang/Lex/Lexer.h"

#include "llvm/ADT/SmallVector.h"

#include <utility>

using namespace llvm;

namespace clang {
namespace refold {

RefoldMacroPatchReusePhase::RefoldMacroPatchReusePhase(Dependencies deps)
    : deps_(std::move(deps)) {}

bool RefoldMacroPatchReusePhase::ValidateMergedDirectAndDagRootReplacement(
    const RefoldModel::MacroInvocation &m, StringRef baseText,
    StringRef newText) const {
  if (baseText == newText) {
    return true;
  }

  // Re-parse both invocation spellings so the check is anchored to formal
  // argument ranges, not to arbitrary textual diff hunks.
  auto baseRangesOpt =
      deps_.getMacroInvocationFormalArgContentRanges(m, baseText);
  auto newRangesOpt =
      deps_.getMacroInvocationFormalArgContentRanges(m, newText);
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
}

bool RefoldMacroPatchReusePhase::IsConcreteSubtreeWitnessCohort(
    const MacroPatch &patch) const {
  if (!patch.subtree.backed)
    return false;
  if (patch.subtree.usesLexicalBridge)
    return false;
  if (patch.subtree.hasPassthroughFlatten)
    return false;
  if (patch.subtree.deferredRootArgCount != 0)
    return false;
  return true;
}

bool RefoldMacroPatchReusePhase::ConflictingConcreteSubtreeWitnesses(
    const RefoldModel::MacroInvocation &m, const MacroPatch &existing,
    const MacroPatch &candidate) const {
  if (!existing.subtree.backed || !candidate.subtree.backed)
    return false;
  if (existing.proof.proofRootMacroId != m.id ||
      candidate.proof.proofRootMacroId != m.id)
    return false;
  if (!existing.subtree.leafMacroId || !candidate.subtree.leafMacroId)
    return false;
  if (existing.subtree.leafMacroId == candidate.subtree.leafMacroId)
    return false;
  if (!IsConcreteSubtreeWitnessCohort(existing) ||
      !IsConcreteSubtreeWitnessCohort(candidate))
    return false;

  const auto existingFormals = parseExpectedRootFormalSummary(
      existing.subtree.expectedRootFormalSummary);
  const auto candidateFormals = parseExpectedRootFormalSummary(
      candidate.subtree.expectedRootFormalSummary);
  for (const auto &kvLocal : existingFormals) {
    auto it = candidateFormals.find(kvLocal.first);
    if (it == candidateFormals.end())
      continue;
    if (kvLocal.second.oldText != it->second.oldText ||
        kvLocal.second.newText != it->second.newText)
      return true;
  }
  return false;
}

void RefoldMacroPatchReusePhase::MergeCurrentRootWithExistingCallsitePatch(
    RefoldMacroWholeCoverPlanningContext &planningCtx,
    MacroPatch &candidate) const {
  const RefoldModel::MacroInvocation &m = planningCtx.m;
  StringRef baseInvText = planningCtx.baseInvText;
  const MacroPatch *existingPatch = planningCtx.reuseAdmissionCtx.existingPatch;
  const bool existingIsCallsite =
      planningCtx.reuseAdmissionCtx.existingIsCallsite;

  // Only merge against an existing structure-preserving callsite patch
  // that belongs to this same proof root.  Other existing patches are
  // handled by the normal conflict/selection logic outside this helper.
  if (!existingPatch || !existingIsCallsite || baseInvText.empty() ||
      !existingPatch->proof.preservesInvocationStructure ||
      existingPatch->proof.proofRootMacroId != m.id)
    return;
  if (candidate.invRange.begin != existingPatch->invRange.begin ||
      candidate.invRange.end != existingPatch->invRange.end)
    return;
  if (candidate.replacement == existingPatch->replacement)
    return;

  // Concrete subtree witnesses for the same root are not allowed to
  // disagree.  Treat that as an explicit whole-cover trigger instead of
  // merging text and losing witness continuity.
  if (ConflictingConcreteSubtreeWitnesses(m, *existingPatch, candidate)) {
    planningCtx.conflictingConcreteSubtreeWitnessForcesWholeCover = true;
    return;
  }

  SmallVector<StringRef, 2> repls;
  repls.push_back(StringRef(candidate.replacement));
  repls.push_back(StringRef(existingPatch->replacement));
  auto merged = mergeCompatibleStringReplacements(baseInvText,
                                                  ArrayRef<StringRef>(repls));
  if (!merged || !ValidateMergedDirectAndDagRootReplacement(
                     m, baseInvText, StringRef(*merged))) {
    return;
  }

  // The merged replacement has passed both text compatibility and root
  // replay validation, so update only the candidate.  The caller remains
  // responsible for final acceptance/selection of that candidate.
  candidate.replacement = std::move(*merged);
  candidate.materialized.hasOutputByteRange = false;
  if (!candidate.macroId)
    candidate.macroId = existingPatch->macroId;
}

DenseMap<uint32_t, RefoldMacroPatchReusePhase::FormalTextPair>
RefoldMacroPatchReusePhase::parseExpectedRootFormalSummary(
    StringRef summary) const {
  DenseMap<uint32_t, FormalTextPair> out;
  StringRef s = summary.trim();
  if (s.empty() || s == "{}")
    return out;

  // Lex the summary from an artificial buffer.  The raw source location
  // only needs to be stable enough to recover token slices from `lexBuf`.
  const SourceLocation baseLoc = SourceLocation::getFromRawEncoding(1);
  std::string lexBuf = s.str();
  lexBuf.push_back('\0');
  const char *bufStart = lexBuf.data();
  const char *bufEnd = bufStart + s.size();
  Lexer lexer(baseLoc, deps_.lexLang, bufStart, bufStart, bufEnd);

  auto nextNonCommentToken = [&]() {
    Token token;
    while (true) {
      lexer.LexFromRawLexer(token);
      if (!token.is(tok::comment))
        return token;
    }
  };

  // Recover the exact spelling for a token from the artificial lex
  // buffer.  This avoids depending on Token internals beyond location and
  // length.
  auto tokenText = [&](const Token &token) -> StringRef {
    const unsigned offset =
        token.getLocation().getRawEncoding() - baseLoc.getRawEncoding();
    return StringRef(bufStart + offset, token.getLength());
  };

  // Summary payloads are encoded as single-quoted token spellings.
  // Accept only char-constant token kinds so malformed summaries fail
  // closed rather than being partially hand-parsed.
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
    return DenseMap<uint32_t, FormalTextPair>{};

  // Parse entries of the form:
  //
  //   <argIdx> : '<oldText>' -> '<newText>'
  //
  // Any unexpected token rejects the whole summary by returning an empty
  // map.  The caller treats an unparseable summary as unavailable
  // metadata, not as a partially valid witness.
  while (true) {
    token = nextNonCommentToken();
    if (token.is(tok::r_brace) || token.is(tok::eof))
      break;
    if (!token.is(tok::numeric_constant))
      return DenseMap<uint32_t, FormalTextPair>{};

    uint32_t argIdx = 0;
    if (tokenText(token).getAsInteger(10, argIdx))
      return DenseMap<uint32_t, FormalTextPair>{};

    token = nextNonCommentToken();
    if (!token.is(tok::colon))
      return DenseMap<uint32_t, FormalTextPair>{};

    token = nextNonCommentToken();
    auto oldText = parseQuotedPayload(token);
    if (!oldText)
      return DenseMap<uint32_t, FormalTextPair>{};

    token = nextNonCommentToken();
    if (!token.is(tok::arrow))
      return DenseMap<uint32_t, FormalTextPair>{};

    token = nextNonCommentToken();
    auto newText = parseQuotedPayload(token);
    if (!newText)
      return DenseMap<uint32_t, FormalTextPair>{};

    out[argIdx] = FormalTextPair{std::move(*oldText), std::move(*newText)};

    // Entries are comma-separated.  A right brace or EOF ends the
    // compact map; any other separator means the audit summary is
    // malformed.
    token = nextNonCommentToken();
    if (token.is(tok::r_brace) || token.is(tok::eof))
      break;
    if (!token.is(tok::comma))
      return DenseMap<uint32_t, FormalTextPair>{};
  }
  return out;
}

} // namespace refold
} // namespace clang
