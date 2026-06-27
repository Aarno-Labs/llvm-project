//===--- RefoldSourceNeutralityProof.cpp ----------------------*- C++ -*-===//
//
// Source-neutrality proof adapter implementation.
//
// The structural proof algorithms are intentionally left in their low-level
// helper headers.  This file owns only the repeated TU/header policy wiring:
// recursive zero-token macro dispatch, path/owner matching for neutral
// conditional islands, and the explicit policy difference between TU fallback
// and materialized-header replay.
//
//===----------------------------------------------------------------------===//

#include "proof/RefoldSourceNeutralityProof.h"
#include "macro/RefoldMacroStateProof.h"
#include "proof/ZeroTokenMacroNeutrality.h"
#include "util/RefoldPathIdentity.h"

#include "llvm/ADT/DenseSet.h"


using namespace llvm;

namespace clang {
namespace refold {

static bool macroInvocationIsSourceNeutralZeroTokenImpl(
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

static bool macroInvocationIsSourceNeutralZeroToken(
    const ZeroTokenMacroNeutralityContext &context,
    const RefoldModel::MacroInvocation &invocation) {
  DenseSet<uint64_t> visiting;
  return macroInvocationIsSourceNeutralZeroTokenImpl(context, invocation,
                                                    visiting);
}

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
  return {model, macroStateProof, paths, headerBytes, headerPath,
          isNeutralTrivia, includeId};
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
  auto directiveBelongs = [&](const RefoldModel::MacroDirective &directive) {
    return context.paths.PathsEqual(directive.sitePath, context.tuPath);
  };
  auto pragmaBelongs = [&](const RefoldModel::PragmaDirective &pragma) {
    return context.paths.PathsEqual(pragma.sitePath, context.tuPath);
  };
  auto macroBelongs = [&](const RefoldModel::MacroInvocation &macro) {
    return macro.invFile && context.paths.PathsEqual(*macro.invFile,
                                                     context.tuPath);
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
      directiveBelongs,
      pragmaBelongs,
      macroBelongs,
      macroIsNeutral};
  return conditionalGroupIsNeutralIsland(context.model, group, gapBegin,
                                         gapEnd, policy);
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
  auto directiveBelongs = [&](const RefoldModel::MacroDirective &directive) {
    return context.paths.PathsEqual(directive.sitePath, context.headerPath) &&
           directive.ownerIncludeId &&
           *directive.ownerIncludeId == context.includeId;
  };
  auto pragmaBelongs = [&](const RefoldModel::PragmaDirective &pragma) {
    return context.paths.PathsEqual(pragma.sitePath, context.headerPath);
  };
  auto macroBelongs = [&](const RefoldModel::MacroInvocation &macro) {
    return macro.invFile && context.paths.PathsEqual(*macro.invFile,
                                                     context.headerPath);
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
      directiveBelongs,
      pragmaBelongs,
      macroBelongs,
      macroIsNeutral};
  return conditionalGroupIsNeutralIsland(context.model, group, gapBegin,
                                         gapEnd, policy);
}

} // namespace refold
} // namespace clang
