//===--- RefoldSourceNeutralityProof.h ------------------------*- C++ -*-===//
//
// Source-neutrality proof adapter layer for clang-refold.
//
// This service collects the owner-specific wiring needed by source-neutral
// zero-token macro and conditional-island proofs.  The low-level proof
// predicates remain in NeutralSourceIslandProof.h and ZeroTokenMacroNeutrality.h;
// this adapter gives TU and materialized-header callers named context objects so
// they do not each rebuild the same recursive lambda scaffold.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDSOURCENEUTRALITYPROOF_H
#define LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDSOURCENEUTRALITYPROOF_H

#include "core/RefoldModel.h"
#include "proof/NeutralSourceIslandProof.h"

#include "llvm/ADT/STLFunctionalExtras.h"
#include "llvm/ADT/StringRef.h"

#include <cstdint>

namespace clang {
namespace refold {

class RefoldMacroStateProof;
class RefoldPathIdentity;

/// Common source surface used by recursive zero-token macro neutrality proof.
///
/// The surface is either the original TU bytes or the currently materialized
/// header bytes.  The caller-controlled whole-definition-list tiling policy is
/// preserved as explicit data because TU fallback and header materialization
/// intentionally use different historical admission domains.  The neutral-trivia
/// predicate is supplied by the caller as a function pointer so this proof layer
/// does not take a line-control parser dependency merely to classify
/// whitespace/comments.
struct ZeroTokenMacroNeutralityContext {
  const RefoldModel &model;
  const RefoldMacroStateProof &macroStateProof;
  const RefoldPathIdentity &paths;
  llvm::StringRef sourceText;
  llvm::StringRef sourcePath;
  bool (*isNeutralTrivia)(llvm::StringRef);
  bool allowWholeDefinitionListTilingBeforeSubkindCheck = false;
};

/// Source-neutrality context for TU-owned fallback gaps.
struct TUSourceNeutralityContext {
  const RefoldModel &model;
  const RefoldMacroStateProof &macroStateProof;
  const RefoldPathIdentity &paths;
  llvm::StringRef tuBytes;
  llvm::StringRef tuPath;
  bool (*isNeutralTrivia)(llvm::StringRef);
};

/// Source-neutrality context for one materialized include expansion.
struct HeaderSourceNeutralityContext {
  const RefoldModel &model;
  const RefoldMacroStateProof &macroStateProof;
  const RefoldPathIdentity &paths;
  llvm::StringRef headerBytes;
  llvm::StringRef headerPath;
  bool (*isNeutralTrivia)(llvm::StringRef);
  uint64_t includeId = 0;
};

/// Owner-neutral conditional-island policy knobs shared by TU and header uses.
///
/// The remaining owner policy hooks are derived from TUSourceNeutralityContext or
/// HeaderSourceNeutralityContext inside RefoldSourceNeutralityProof so callers do
/// not duplicate the same path/owner matching lambdas.
struct NeutralConditionalIslandContext {
  bool requireGroupBeginAtLineStart = false;
  NeutralConditionalArmSpanMode armSpanMode =
      NeutralConditionalArmSpanMode::SelectedArmsOnly;
};

/// Named adapter service for source-neutral zero-token proof wiring.
struct RefoldSourceNeutralityProof {
  /// Build the context used by TU fallback source-neutrality checks.
  static TUSourceNeutralityContext BuildTUSourceNeutralityContext(
      const RefoldModel &model, const RefoldMacroStateProof &macroStateProof,
      const RefoldPathIdentity &paths, llvm::StringRef tuBytes,
      llvm::StringRef tuPath,
      bool (*isNeutralTrivia)(llvm::StringRef));

  /// Build the context used by materialized-header source-neutrality checks.
  static HeaderSourceNeutralityContext BuildHeaderSourceNeutralityContext(
      const RefoldModel &model, const RefoldMacroStateProof &macroStateProof,
      const RefoldPathIdentity &paths, llvm::StringRef headerBytes,
      llvm::StringRef headerPath, uint64_t includeId,
      bool (*isNeutralTrivia)(llvm::StringRef));

  /// Return true iff the recorded macro invocation emitted material PP tokens.
  static bool MacroInvocationHasMaterializedPPTokens(
      const RefoldModel::MacroInvocation &invocation);

  /// Prove that a TU-spelled macro invocation is recursively zero-token neutral.
  static bool MacroInvocationIsSourceNeutralZeroToken(
      const TUSourceNeutralityContext &context,
      const RefoldModel::MacroInvocation &invocation);

  /// Prove that a header-spelled macro invocation is recursively zero-token
  /// neutral, preserving the header materializer's whole-definition-list tiling
  /// policy.
  static bool MacroInvocationIsSourceNeutralZeroToken(
      const HeaderSourceNeutralityContext &context,
      const RefoldModel::MacroInvocation &invocation);

  /// Prove a TU-owned conditional group under the shared neutral-island proof.
  static bool ConditionalGroupIsNeutralIsland(
      const TUSourceNeutralityContext &context,
      const RefoldModel::CondGroup &group, uint64_t gapBegin, uint64_t gapEnd,
      const NeutralConditionalIslandContext &islandContext,
      llvm::function_ref<bool(const RefoldModel::IncludeItem &)>
          includeIsNeutral);

  /// Prove a materialized-header conditional group under the shared
  /// neutral-island proof.
  static bool ConditionalGroupIsNeutralIsland(
      const HeaderSourceNeutralityContext &context,
      const RefoldModel::CondGroup &group, uint64_t gapBegin, uint64_t gapEnd,
      const NeutralConditionalIslandContext &islandContext,
      llvm::function_ref<bool(const RefoldModel::IncludeItem &)>
          includeIsNeutral);
};

} // namespace refold
} // namespace clang

#endif // LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDSOURCENEUTRALITYPROOF_H
