//===--- RefoldMacroGeneratedCalleeReplayEngine.h ---------------*- C++ -*-===//
//
// Higher-order generated-callee replay engine for clang-refold.
//
// Owns the four predicates and builders that admit and construct
// generated-callee chain candidates outside the leaf fallback path:
//
//   * `GeneratedCalleeReplayPreservesEnvelope` — trivial envelope guard.
//   * `GeneratedCalleeReplayIsAdmissible` — pre-build admission gate over
//     the recovered generated-call context.
//   * `BuildGeneratedCalleeReplayCandidate` — main higher-order chain
//     replay candidate construction.
//   * `BuildTupleGeneratedCalleeReplayCandidate` — tuple-aware variant
//     used when the caller forwards a tuple actual through a forwarder
//     callee.
//
// Owns generated-callee replay construction and admission so patch dispatch,
// replay synthesis, and proof stamping remain separate. Calls into
// `RefoldMacroPatchProofCertifier` and into three thin planner-side helpers
// (`BuildInvocationRewriteWithRange`, `ResolveFunctionLikeMacroForReplay`,
// `ResolveFunctionLikeMacroThroughAliasesWithHops`) via explicit
// std::function callbacks supplied by the planner at construction.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDMACROGENERATEDCALLEEREPLAYENGINE_H
#define LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDMACROGENERATEDCALLEEREPLAYENGINE_H

#include "core/RefoldModel.h"
#include "edit/RefoldPatchTypes.h"
#include "macro/RefoldMacroPlannerHelpers.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <utility>

namespace clang {

class LangOptions;

namespace refold {

class RefoldMacroPatchProofCertifier;
class RefoldSourceMapper;

/// Source slot tracked while following a generated-callee chain.
///
/// `text` is the actual spelling at the current replay level.  The root fields
/// identify the original invocation argument that must be edited if the final
/// generated callee solves a different value.
struct GeneratedCalleeSourceSlot {
  /// Current replay-level actual spelling.
  std::string text;
  /// Original root-invocation source spelling that produced this slot.
  std::string rootSourceText;
  /// Root formal index whose actual owns `rootSourceText`.
  uint32_t rootArgIdx = 0;
};

/// Explicit state bundle for higher-order generated-callee replay.
///
/// The engine borrows call-chain storage, root state, source slots, and replay
/// policy flags through this context for one generated-callee replay attempt.
struct GeneratedCalleeReplayContext {
  /// Root invocation whose callee chain is being replayed.
  const RefoldModel::MacroInvocation &invocation;
  /// Complete source spelling of the root invocation.
  llvm::StringRef baseInvocationText;
  /// Formal-content byte ranges inside `baseInvocationText`.
  llvm::ArrayRef<std::pair<size_t, size_t>> invocationArgRanges;
  /// Macro definition at the root of the generated-callee proof.
  const RefoldModel::MacroDirective &rootDefinition;
  /// Whole-cover A-token envelope for the root invocation.
  const std::pair<uint64_t, uint64_t> &wholeCoverATokens;
  /// B-token envelope being realized by generated-callee replay.
  const std::pair<size_t, size_t> &bTokenEnvelope;
  /// Prefix literals accumulated while stepping through generated callees.
  llvm::SmallVectorImpl<std::string> &replayPrefixLiterals;
  /// Per-level suffix literals that must be replayed after the generated call.
  llvm::SmallVectorImpl<llvm::SmallVector<std::string, 8>> &replaySuffixStack;
  /// Current macro definition reached by replay; updated as the chain advances.
  const RefoldModel::MacroDirective *&currentDefinition;
  /// Current actual slots at the replay level reached by `currentDefinition`.
  llvm::SmallVectorImpl<GeneratedCalleeSourceSlot> &currentActuals;
  /// Set when replay actually followed at least one generated call.
  bool &followedGeneratedCall;
  /// Number of generated-call levels consumed by the replay proof.
  uint32_t &generatedCallDepth;
  /// Number of object-like alias hops consumed while resolving callees.
  uint32_t &objectAliasHopCount;
  /// Set when replay passed through stringification-sensitive actuals.
  bool &usesStringification;
  /// Set when replay passed through paste-sensitive actuals.
  bool &usesPaste;
  /// Set when replay passed through variadic forwarding.
  bool &usesVariadicForwarding;
};

/// Explicit root/forwarder state for tuple-generated callee replay.
///
/// Tuple replay edits tuple elements inside one root formal.  The context keeps
/// owner/forwarder identity and object-alias hop counts explicit while local
/// tuple solver state stays inside the replay method.
struct TupleGeneratedCalleeReplayContext {
  /// Root invocation whose tuple actual is being replayed.
  const RefoldModel::MacroInvocation &invocation;
  /// Complete source spelling of the root invocation.
  llvm::StringRef baseInvocationText;
  /// Formal-content byte ranges inside `baseInvocationText`.
  llvm::ArrayRef<std::pair<size_t, size_t>> invocationArgRanges;
  /// Whole-cover A-token envelope for the root invocation.
  const std::pair<uint64_t, uint64_t> &wholeCoverATokens;
  /// B-token envelope being realized by tuple replay.
  const std::pair<size_t, size_t> &bTokenEnvelope;
  /// Macro definition at the tuple replay root.
  const RefoldModel::MacroDirective &rootDefinition;
  /// Forwarder definition that maps the caller tuple into the generated callee.
  const RefoldModel::MacroDirective &forwarderDefinition;
  /// Caller formal index that contains the forwarded tuple actual.
  uint32_t callerArgIdx = 0;
  /// Number of object-like alias hops consumed while resolving the forwarder.
  uint32_t &objectAliasHopCount;
};

/// Builds and validates generated-callee replay candidates.
///
/// Generated-callee replay handles edits whose visible callee surface was
/// produced by macro expansion; the engine admits a candidate only when the
/// generated call can be traced back to a deterministic macro proof root.
class RefoldMacroGeneratedCalleeReplayEngine {
public:
  /// Borrowed inputs needed by the generated-callee replay engine.  All
  /// references must outlive the engine; the planner owns all of them.
  ///
  /// The callbacks delegate to shared planner-side helpers while keeping this
  /// engine free of a back-reference to the planner.
  struct Dependencies {
    const RefoldModel &model;
    const RefoldSourceMapper &sourceMapper;
    const clang::LangOptions &lexLang;
    RefoldMacroPatchProofCertifier &proofCertifier;

    /// Resolve a function-like macro name through replay-safe alias hops
    /// when the hop count is not needed.  Delegates to
    /// `RefoldMacroPatchPlanner::ResolveFunctionLikeMacroForReplay`.
    std::function<const RefoldModel::MacroDirective *(llvm::StringRef)>
        resolveFunctionLikeMacroForReplay;

    /// Resolve a function-like macro name through replay-safe alias hops,
    /// reporting the number of hops consumed by the replay proof.  Delegates
    /// to
    /// `RefoldMacroPatchPlanner::ResolveFunctionLikeMacroThroughAliasesWithHops`.
    std::function<const RefoldModel::MacroDirective *(llvm::StringRef,
                                                      uint32_t *)>
        resolveFunctionLikeMacroThroughAliasesWithHops;

    /// Rebuild the invocation by replacing formal-content ranges right-to-
    /// left in source order while carrying the materialized output range.
    /// Delegates to `RefoldMacroPatchPlanner::BuildInvocationRewriteWithRange`.
    std::function<std::optional<InvocationRewriteWithRange>(
        const InvocationActualRecoveryContext &,
        const llvm::DenseMap<uint32_t, std::string> &,
        const llvm::DenseMap<uint32_t, std::pair<uint64_t, uint64_t>> *)>
        buildInvocationRewriteWithRange;
  };

  explicit RefoldMacroGeneratedCalleeReplayEngine(Dependencies deps);

  /// Trivial envelope guard ahead of higher-order replay.
  bool GeneratedCalleeReplayPreservesEnvelope(
      const GeneratedCalleeReplayContext &ctx) const;

  /// Pre-build admission gate over the recovered generated-call context.
  bool GeneratedCalleeReplayIsAdmissible(
      const GeneratedCalleeReplayContext &ctx) const;

  /// Build the higher-order generated-callee chain replay candidate.
  /// Returns nullopt for a non-terminal miss.
  std::optional<MacroPatch> BuildGeneratedCalleeReplayCandidate(
      const GeneratedCalleeReplayContext &ctx) const;

  /// Build the tuple-aware generated-callee replay candidate.  Returns
  /// nullopt for a non-terminal miss.
  std::optional<MacroPatch> BuildTupleGeneratedCalleeReplayCandidate(
      const TupleGeneratedCalleeReplayContext &ctx) const;

private:
  Dependencies deps_;
};

} // namespace refold
} // namespace clang

#endif // LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDMACROGENERATEDCALLEEREPLAYENGINE_H
