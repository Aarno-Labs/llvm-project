//===--- RefoldMacroGeneratedCalleeReplayEngine.h ---------------*- C++ -*-===//
//
// Higher-order generated-callee replay engine for clang-refold.
//
// Owns the generated-callee predicates and builders that admit and construct
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
//   * `BuildPasteTupleGeneratedCalleeReplayCandidate` — paste-derived
//     callee variant used when the root macro forms the callee token with
//     `##` and supplies the generated call arguments through one tuple actual.
//   * `BuildObjectSelectorTupleGeneratedCalleeReplayCandidate` — object-selector
//     variant used when a root `f t` macro changes both selector alias and
//     tuple actuals.
//
// Owns generated-callee replay construction and admission so patch dispatch,
// replay synthesis, and proof certification remain separate. Calls into
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

/// Explicit root state for literal-callee tuple-actual replay.
///
/// This replay handles wrapper definitions shaped like `CALLEE args`, where the
/// callee is fixed by the root replacement list and the single source actual
/// supplies the complete parenthesized generated-callee actual list.  It is the
/// direct analogue of tuple-generated `f t` replay, but without a source tuple
/// element for the callee name.
struct LiteralCalleeTupleActualReplayContext {
  /// Root invocation whose tuple actual is being replayed.
  const RefoldModel::MacroInvocation &invocation;
  /// Complete source spelling of the root invocation.
  llvm::StringRef baseInvocationText;
  /// Formal-content byte ranges inside `baseInvocationText`.
  llvm::ArrayRef<std::pair<size_t, size_t>> invocationArgRanges;
  /// Whole-cover A-token envelope for the root invocation.
  const std::pair<uint64_t, uint64_t> &wholeCoverATokens;
  /// B-token envelope being realized by literal-callee replay.
  const std::pair<size_t, size_t> &bTokenEnvelope;
  /// Macro definition at the tuple replay root.
  const RefoldModel::MacroDirective &rootDefinition;
  /// Fixed function-like callee named by the root replacement list.
  const RefoldModel::MacroDirective &calleeDefinition;
  /// Caller formal index that contains the parenthesized actual list.
  uint32_t callerArgIdx = 0;
  /// Number of object-like alias hops consumed while resolving the callee.
  uint32_t &objectAliasHopCount;
};

/// Explicit root state for paste-derived tuple generated-callee replay.
///
/// This replay handles roots shaped like `a##b t`: the generated callee token
/// is synthesized by a deterministic paste expression over root actuals, while
/// the final callee arguments are supplied by one parenthesized tuple actual.
/// The engine solves the old expansion against the old pasted callee, solves
/// the edited expansion against exactly one visible replacement callee, then
/// maps both the callee-token paste pieces and tuple elements back to root
/// invocation arguments.
struct PasteTupleGeneratedCalleeReplayContext {
  /// Root invocation whose paste-derived generated callee is being replayed.
  const RefoldModel::MacroInvocation &invocation;
  /// Complete source spelling of the root invocation.
  llvm::StringRef baseInvocationText;
  /// Formal-content byte ranges inside `baseInvocationText`.
  llvm::ArrayRef<std::pair<size_t, size_t>> invocationArgRanges;
  /// Whole-cover A-token envelope for the root invocation.
  const std::pair<uint64_t, uint64_t> &wholeCoverATokens;
  /// B-token envelope being realized by paste/tuple replay.
  const std::pair<size_t, size_t> &bTokenEnvelope;
  /// Macro definition at the paste/tuple replay root.
  const RefoldModel::MacroDirective &rootDefinition;
  /// Root formal index containing the parenthesized tuple actual.
  uint32_t tupleArgIdx = 0;
};

/// Explicit root state for paste-derived generated-callee replay.
///
/// This replay handles roots shaped like `a##b(x, y)`: the generated callee
/// token is synthesized by token paste over root actuals, while the generated
/// call's argument list is spelled directly in the root replacement list.  The
/// engine solves the old and edited expansion surfaces through visible
/// function-like macro definitions and maps both the pasted callee spelling and
/// solved generated-call actuals back to source-spelled root arguments.
struct PasteGeneratedCalleeReplayContext {
  /// Root invocation whose paste-derived generated callee is being replayed.
  const RefoldModel::MacroInvocation &invocation;
  /// Complete source spelling of the root invocation.
  llvm::StringRef baseInvocationText;
  /// Formal-content byte ranges inside `baseInvocationText`.
  llvm::ArrayRef<std::pair<size_t, size_t>> invocationArgRanges;
  /// Whole-cover A-token envelope for the root invocation.
  const std::pair<uint64_t, uint64_t> &wholeCoverATokens;
  /// B-token envelope being realized by paste/generated-callee replay.
  const std::pair<size_t, size_t> &bTokenEnvelope;
  /// Macro definition at the paste/generated-callee replay root.
  const RefoldModel::MacroDirective &rootDefinition;
};

/// Explicit root state for object-selector tuple generated-callee replay.
///
/// This replay handles roots shaped like `f t` when `f` is a source-spelled
/// object-like selector macro that resolves to the function-like generated
/// callee and `t` is one parenthesized tuple actual.  Unlike the paste-derived
/// variant, the selector rewrite preserves the object-selector abstraction:
/// an old object alias such as `SELECT_ADD` may be rewritten only to another
/// object alias that resolves to the uniquely solved edited callee.
struct ObjectSelectorTupleGeneratedCalleeReplayContext {
  /// Root invocation whose object-selector generated callee is replayed.
  const RefoldModel::MacroInvocation &invocation;
  /// Complete source spelling of the root invocation.
  llvm::StringRef baseInvocationText;
  /// Formal-content byte ranges inside `baseInvocationText`.
  llvm::ArrayRef<std::pair<size_t, size_t>> invocationArgRanges;
  /// Whole-cover A-token envelope for the root invocation.
  const std::pair<uint64_t, uint64_t> &wholeCoverATokens;
  /// B-token envelope being realized by object-selector/tuple replay.
  const std::pair<size_t, size_t> &bTokenEnvelope;
  /// Macro definition at the object-selector/tuple replay root.
  const RefoldModel::MacroDirective &rootDefinition;
  /// Root formal index containing the selector actual.
  uint32_t selectorArgIdx = 0;
  /// Root formal index containing the parenthesized tuple actual.
  uint32_t tupleArgIdx = 0;
};

/// Request for solving one already-composed terminal generated callee.
///
/// The recursive tuple-generated-callee resolver uses this narrow engine entry
/// point after it has independently proven the producer ancestry path and
/// terminal-actual source slices.  The generated-callee engine still owns the
/// replacement-list parser, old/new expansion replay, stringification/paste
/// handling, and unique-solution policy.
struct TerminalGeneratedCalleeReplayRequest {
  /// Terminal generated function-like macro definition to replay.
  const RefoldModel::MacroDirective &terminalDefinition;
  /// Old terminal actual spellings in terminal callee formal order.
  llvm::ArrayRef<std::string> oldActuals;
  /// A-token replay surface for the terminal generated invocation.
  ///
  /// For direct recursive tuple replay this is usually the root whole-cover
  /// envelope.  For nested generated callees it is the terminal callee's own
  /// producer-recorded expansion surface, excluding enclosing generated-macro
  /// wrapper tokens that are not part of the terminal replacement list.
  const std::pair<uint64_t, uint64_t> &wholeCoverATokens;
  /// B-token envelope corresponding to `wholeCoverATokens`.
  const std::pair<size_t, size_t> &bTokenEnvelope;
};

/// Unique old/new replay solution for one terminal generated callee.
struct TerminalGeneratedCalleeReplaySolution {
  /// Solved old actuals in terminal callee formal order.
  llvm::SmallVector<std::string, 8> oldSolvedActuals;
  /// Solved edited actuals in terminal callee formal order.
  llvm::SmallVector<std::string, 8> newSolvedActuals;
  /// True when the accepted terminal replay inverted stringification.
  bool usesStringification = false;
  /// True when the accepted terminal replay inverted token paste.
  bool usesPaste = false;
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

  /// Build a literal-callee tuple-actual replay candidate.  Returns nullopt for
  /// a non-terminal miss.
  std::optional<MacroPatch> BuildLiteralCalleeTupleActualReplayCandidate(
      const LiteralCalleeTupleActualReplayContext &ctx) const;

  /// Build the paste-derived tuple generated-callee replay candidate. Returns
  /// nullopt for a non-terminal miss.
  std::optional<MacroPatch> BuildPasteTupleGeneratedCalleeReplayCandidate(
      const PasteTupleGeneratedCalleeReplayContext &ctx) const;

  /// Build the paste-derived generated-callee replay candidate. Returns nullopt
  /// for a non-terminal miss.
  std::optional<MacroPatch> BuildPasteGeneratedCalleeReplayCandidate(
      const PasteGeneratedCalleeReplayContext &ctx) const;

  /// Build the object-selector tuple generated-callee replay candidate. Returns
  /// nullopt for a non-terminal miss.
  std::optional<MacroPatch>
  BuildObjectSelectorTupleGeneratedCalleeReplayCandidate(
      const ObjectSelectorTupleGeneratedCalleeReplayContext &ctx) const;

  /// Solve an already-composed terminal generated-callee replay.
  ///
  /// This method exposes only the existing generated-callee solver boundary:
  /// callers provide the terminal definition and old actuals, while the engine
  /// parses the terminal replacement tape and requires unique old/new replay
  /// solutions.  It constructs no patch and performs no root tuple edits.
  std::optional<TerminalGeneratedCalleeReplaySolution>
  SolveTerminalGeneratedCalleeReplay(
      const TerminalGeneratedCalleeReplayRequest &ctx) const;

private:
  Dependencies deps_;
};

} // namespace refold
} // namespace clang

#endif // LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDMACROGENERATEDCALLEEREPLAYENGINE_H
