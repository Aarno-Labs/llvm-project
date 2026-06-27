//===--- NeutralSourceIslandProof.h ---------------*- C++ -*-===//
//
// Private source-neutrality proof helpers shared by RefoldEngine translation
// units after the TailUtilities split.  These helpers prove that preserved
// zero-token source islands, such as balanced diagnostic pragma state islands
// and inactive/empty conditional groups, are observationally neutral instead of
// duplicating that proof in each split implementation file.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_NEUTRALSOURCEISLANDPROOF_H
#define LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_NEUTRALSOURCEISLANDPROOF_H

#include "RefoldModel.h"
#include "RefoldOwnerStateProof.h"
#include "StringUtils.h"

#include "clang/Basic/LangOptions.h"

#include "llvm/ADT/ScopeExit.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/STLFunctionalExtras.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"

#include <cstdint>
#include <optional>
#include <string>

namespace clang {
namespace refold {

/// Controls which conditional arms must have no materialized PP tokens for a
/// preserved conditional island to be source-neutral.
enum class NeutralConditionalArmSpanMode { AllArms, SelectedArmsOnly };

/// One complete locally-neutral diagnostic pragma-state island.
///
/// `#pragma clang/GCC diagnostic push/pop` forms a stack discipline: pushes
/// save the current diagnostic mapping, settings mutate only the top frame, and
/// pops restore the previous mapping.  A fully balanced island that starts and
/// ends at stack depth zero, contains only diagnostic settings while depth is
/// positive, and crosses only trivia has identity net state at its boundaries.
/// Such an island may be carried through a source gap without changing the
/// preprocessing token stream or the diagnostic state observed by preserved
/// suffix source.
struct BalancedDiagnosticPragmaStateIsland {
  uint64_t begin = 0;
  uint64_t end = 0;
  uint64_t id = 0;
};

/// Return true iff a source-gap byte slice is ignorable preprocessing trivia.
///
/// mixed-owner tiling uses this predicate only for bytes that sit
/// between modeled zero-token state owners.  The owner-state proof module owns
/// the shared raw-lexer trivia theorem; this engine-local wrapper preserves the
/// existing call boundary for mixed-owner tiling without duplicating the lexer
/// implementation.
inline bool sourceTextIsOnlyIgnorableGapTrivia(StringRef text,
                                               const LangOptions &lang) {
  return sourceTextIsOnlyWhitespaceAndCompleteComments(text, lang);
}

/// Return the normalized spelling used to compare pragma-state islands across
/// source and B replay surfaces.
inline std::optional<std::string>
canonicalDiagnosticPragmaStateDirectiveText(StringRef text,
                                            const LangOptions &lang) {
  std::optional<ParsedDiagnosticPragmaStateDirective> parsed =
      parseDiagnosticPragmaStateDirective(text, lang);
  if (!parsed)
    return std::nullopt;

  std::string out;
  out += "#pragma ";
  out += parsed->namespaceName.str();
  out += " diagnostic ";
  out += parsed->actionName.str();
  if (parsed->action == DiagnosticPragmaStateAction::Setting) {
    out += " ";
    out += parsed->optionSpelling.str();
  }
  out += "\n";
  return out;
}

/// Canonicalize `text` iff it is exactly one locally balanced diagnostic
/// pragma-state island plus trivia.
///
/// This function is the replay-side counterpart to
/// collectBalancedDiagnosticPragmaStateIslands().  The source collector proves
/// that an island has identity net state; this helper proves that a B replay
/// surface already contains the same state island, so emission must not append
/// the original source island a second time.
inline std::optional<std::string>
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

/// Return true iff `replacement` already contains the same locally balanced
/// diagnostic pragma-state island as `sourceIslandText`.
///
/// Balanced pragma islands are zero-normal-token state artifacts: after
/// sideband normalization, the ordinary B replacement bytes may still contain
/// their raw directive spellings even though the structural token diff does
/// not.  When the B surface carries the island, copying the original source
/// island as a preserved gap would duplicate `#pragma` directives and change
/// validation. The proof is deliberately narrow: only a complete canonical
/// island match suppresses source-gap emission; otherwise the caller preserves
/// the source island or fails through the existing owner proof.
inline bool balancedDiagnosticPragmaStateIslandIsCarriedByReplacement(
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

/// Collect top-level balanced diagnostic pragma-state islands in a source gap.
///
/// This is the pragma/state-effect invariant in mechanical form.  A
/// pragma sequence can be treated as source-neutral only when:
///
///  * every directive belongs to the caller's current owner surface;
///  * every directive parses as `#pragma clang/GCC diagnostic ...`;
///  * all directives in one island use the same diagnostic namespace;
///  * stack depth never goes negative and returns to zero;
///  * settings occur only while a pushed frame is active; and
///  * the bytes crossed between directives are trivia only.
///
/// The island is not deleted by this helper; callers preserve the original
/// source bytes as an explicit gap piece.  The proof is therefore about the net
/// boundary state, not about reconstructing or normalizing pragma spelling.
inline void collectBalancedDiagnosticPragmaStateIslands(
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

/// Owner-specific hooks for the shared neutral conditional-island proof.
///
/// The proof itself is owner-polymorphic: TU gaps, pure include-closure gaps,
/// and header-owned gaps have the same first-order invariant, but differ in how
/// model records are associated with the current owner surface and how neutral
/// include/macro artifacts are discharged.
struct NeutralConditionalIslandPolicy {
  StringRef sourceText;
  bool requireGroupBeginAtLineStart = false;
  NeutralConditionalArmSpanMode armSpanMode =
      NeutralConditionalArmSpanMode::SelectedArmsOnly;

  function_ref<bool(const RefoldModel::CondGroup &)> groupBelongs;
  function_ref<bool(const RefoldModel::IncludeItem &)> includeBelongs;
  function_ref<bool(const RefoldModel::IncludeItem &)> includeIsNeutral;
  function_ref<bool(const RefoldModel::MacroDirective &)> directiveBelongs;
  function_ref<bool(const RefoldModel::PragmaDirective &)> pragmaBelongs;
  function_ref<bool(const RefoldModel::MacroInvocation &)> macroBelongs;
  function_ref<bool(const RefoldModel::MacroInvocation &)> macroIsNeutral;
};

/// Return true iff \p arm has A-side PP material that would make a preserved
/// conditional island source-bearing under \p mode.
inline bool neutralConditionalArmHasMaterializedTokens(
    const RefoldModel::CondArm &arm, NeutralConditionalArmSpanMode mode) {
  if (mode == NeutralConditionalArmSpanMode::SelectedArmsOnly &&
      !arm.selected)
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
inline bool conditionalGroupIsNeutralIslandImpl(
    const RefoldModel &model, const RefoldModel::CondGroup &group,
    uint64_t gapBegin, uint64_t gapEnd,
    const NeutralConditionalIslandPolicy &policy,
    SmallVectorImpl<uint64_t> &recursionStack) {
  if (!policy.groupBelongs(group))
    return false;
  if (group.groupB >= group.groupE ||
      group.groupE > policy.sourceText.size())
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
      if (conditionalGroupIsNeutralIslandImpl(
              model, nested, group.groupB, group.groupE, policy,
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
  // does not currently model their state domain precisely enough to prove that
  // preserving the surrounding island preserves every relevant compiler state
  // interaction.

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
inline bool conditionalGroupIsNeutralIsland(
    const RefoldModel &model, const RefoldModel::CondGroup &group,
    uint64_t gapBegin, uint64_t gapEnd,
    const NeutralConditionalIslandPolicy &policy) {
  SmallVector<uint64_t, 8> recursionStack;
  return conditionalGroupIsNeutralIslandImpl(model, group, gapBegin, gapEnd,
                                             policy, recursionStack);
}


} // namespace refold
} // namespace clang

#endif // LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_NEUTRALSOURCEISLANDPROOF_H
