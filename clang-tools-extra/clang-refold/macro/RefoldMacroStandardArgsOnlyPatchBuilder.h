//===--- RefoldMacroStandardArgsOnlyPatchBuilder.h --------------*- C++ -*-===//
//
// Standard args-only patch builder for clang-refold.
//
// Owns the args-only entry point, `BuildMacroInvocationPatchArgsOnly`.  It
// recovers the invocation's actuals and tries, in candidate order,
// definition-tape replay, the current-level template solver, paste-aware
// replay, and `BuildStandardArgsOnlyPatch`.  The last is the standard
// args-only ranking slot, including the pure paste-only fallback path that
// belongs to that same slot after specialized paste-aware proofs have
// declined.  The builder collects arg-span occurrences (and stringify
// occurrences) for the touched hunk, requires hunk coverage by those spans,
// and derives per-arg replacements from the B-side slices.  Higher-order
// generated-callee, generated-leaf, tuple-forwarded, and pure paste-only paths
// remain implementation details reached through the references supplied in
// `Dependencies`; whole-cover orchestration may invoke the higher-order probe
// through the narrow competition hook declared below.
//
// The builder has no back-reference to `RefoldMacroPatchPlanner`.
// Planner-side helpers that stay on the planner are reached through
// std::function callbacks (`buildInvocationRewriteWithRange`,
// `resolveFunctionLikeMacroThroughAliasesWithHops`,
// `certifyMacroPatchWholeExpansionBRange`); independent services are passed
// by reference.  The planner constructs the builder before any whole-cover
// orchestrator exists, so the orchestrator and its phases borrow it directly.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDMACROSTANDARDARGSONLYPATCHBUILDER_H
#define LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDMACROSTANDARDARGSONLYPATCHBUILDER_H

#include "edit/RefoldPatchTypes.h"
#include "macro/RefoldMacroArgsOnlyTemplateSolver.h"
#include "macro/RefoldMacroPasteArgumentBuilder.h"
#include "macro/RefoldMacroPlannerHelpers.h"
#include "macro/RefoldMacroReplay.h"
#include "model/RefoldModel.h"
#include "model/RefoldToken.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/StringRef.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace clang {

class LangOptions;

namespace refold {

class RefoldArgTextRecovery;
class RefoldBInsertionLedger;
class RefoldMacroDefinitionTapeSolver;
class RefoldMacroGeneratedCalleeReplayEngine;
class RefoldMacroGeneratedLeafReplayEngine;
class RefoldMacroPatchProofCertifier;
class RefoldMacroTopology;
class RefoldMacroPatchProofClassifier;
class RefoldSourceMapper;
class RefoldWitnessTrace;
struct SyntheticEnvelopeCache;

// ArgsOnlyPlanningContext lives in RefoldMacroPlannerHelpers.h and is shared
// by definition-tape, paste-aware, template, and standard args-only replay
// paths.

/// Builds standard args-only macro patches from validated argument layouts.
///
/// The builder materializes the replacement text and proof envelope for the
/// standard args-only ranking slot, including the deterministic pure paste-only
/// fallback that is still certified as an args-only candidate.
class RefoldMacroStandardArgsOnlyPatchBuilder {
public:
  /// Borrowed inputs needed by the standard args-only patch builder.
  ///
  /// All references must outlive the builder; the planner owns or borrows all
  /// of them.
  /// `abTokHunks` is stored as a vector reference, not `ArrayRef`, because the
  /// vector is populated after service construction and must be observed by
  /// later replay attempts.
  struct Dependencies {
    const RefoldModel &model;
    const RefoldSourceMapper &sourceMapper;
    const clang::LangOptions &lexLang;
    const RefoldMacroTopology &macroTopology;
    const RefoldArgTextRecovery &argTextRecovery;
    const RefoldBInsertionLedger &bInsertionLedger;
    llvm::ArrayRef<PPTok> aToks;
    llvm::ArrayRef<PPTok> bToks;
    llvm::StringRef bSource;
    llvm::ArrayRef<size_t> bTokOff;
    const std::vector<diffutils::Hunk> &abTokHunks;
    RefoldMacroPatchProofCertifier &proofCertifier;
    const RefoldMacroPatchProofClassifier &macroPatchProofClassifier;
    const RefoldWitnessTrace &witnessTrace;
    const RefoldMacroGeneratedCalleeReplayEngine &generatedCalleeReplayEngine;
    const RefoldMacroGeneratedLeafReplayEngine &generatedLeafReplayEngine;
    bool strict = false;

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

    /// Certify the materialized B-token envelope on a macro patch from the
    /// invocation's whole A-side cover.  Delegates to
    /// `RefoldMacroPatchPlanner::CertifyMacroPatchWholeExpansionBRange`.
    std::function<bool(const RefoldModel::MacroInvocation &, MacroPatch &)>
        certifyMacroPatchWholeExpansionBRange;
  };

  explicit RefoldMacroStandardArgsOnlyPatchBuilder(Dependencies deps);
  ~RefoldMacroStandardArgsOnlyPatchBuilder();

  /// While alive, lets the builder reuse synthetic insertion-envelope searches
  /// across args-only attempts.
  ///
  /// Those searches read the token-hunk plan, which `Dependencies::abTokHunks`
  /// observes by reference and which is republished between passes.  Open the
  /// scope only where that plan is final and cannot change before the scope
  /// ends; outside a scope nothing is cached.  Scopes do not nest.
  class EnvelopeCacheScope {
  public:
    explicit EnvelopeCacheScope(
        const RefoldMacroStandardArgsOnlyPatchBuilder &builder);
    ~EnvelopeCacheScope();
    EnvelopeCacheScope(const EnvelopeCacheScope &) = delete;
    EnvelopeCacheScope &operator=(const EnvelopeCacheScope &) = delete;

  private:
    const RefoldMacroStandardArgsOnlyPatchBuilder &builder_;
  };

  /// Build a structure-preserving invocation patch by rewriting only the
  /// callsite arguments when all touched macro occurrences replay consistently.
  ///
  /// Tries definition-tape replay, the current-level template solver,
  /// paste-aware replay and `BuildStandardArgsOnlyPatch`, in that order.  The
  /// first admitted candidate is returned; a paste-surface rejection returns
  /// nullopt without trying the standard slot.
  std::optional<MacroPatch>
  BuildMacroInvocationPatchArgsOnly(const RefoldModel::MacroInvocation &m,
                                    const diffutils::Hunk &h,
                                    llvm::StringRef baseInvocationText) const;

  /// Recover the parsed invocation-actual layout needed by args-only replay.
  /// This performs only the admissibility precondition checks and source-range
  /// recovery; candidate construction and ranking remain separate operations.
  std::optional<InvocationActualLayout>
  RecoverInvocationActuals(const RefoldModel::MacroInvocation &invocation,
                           llvm::StringRef baseInvocationText) const;

  /// Run ordinary standard/stringify formal replay after specialized
  /// args-only replay paths did not produce a candidate.  Returns nullopt
  /// for a non-terminal miss.
  std::optional<MacroPatch>
  BuildStandardArgsOnlyPatch(const ArgsOnlyPlanningContext &ctx) const;

  /// Try the existing higher-order generated replay theorem after another
  /// args-only path has already admitted a candidate.
  ///
  /// Definition-tape replay intentionally precedes the standard args-only
  /// builder and can therefore return before the builder's private higher-order
  /// probe runs.  Whole-cover orchestration uses this narrow hook only to let an
  /// invocation-preserving generated-callee proof compete with that earlier
  /// direct candidate.  A miss is non-terminal and does not alter ordinary
  /// args-only or whole-cover fallback behavior.
  std::optional<MacroPatch> TryBuildHigherOrderGeneratedReplay(
      const RefoldModel::MacroInvocation &invocation,
      const diffutils::Hunk &hunk, llvm::StringRef baseInvocationText,
      llvm::ArrayRef<std::pair<size_t, size_t>> invocationArgRanges) const;

private:
  /// Result of one args-only candidate attempt.
  ///
  /// Paste-aware replay needs to distinguish "no candidate, keep trying" from
  /// "the touched paste surface was invalid, fail closed."  The carrier makes
  /// that search/fail-closed control flow explicit for callers.
  struct ArgsOnlyPatchAttempt {
    enum class Disposition : uint8_t { ContinueSearch, Reject, Accepted };

    Disposition disposition = Disposition::ContinueSearch;
    std::optional<MacroPatch> patch;

    static ArgsOnlyPatchAttempt ContinueSearchResult() {
      return ArgsOnlyPatchAttempt{};
    }

    static ArgsOnlyPatchAttempt RejectResult() {
      ArgsOnlyPatchAttempt result;
      result.disposition = Disposition::Reject;
      return result;
    }

    static ArgsOnlyPatchAttempt AcceptedResult(MacroPatch patch) {
      ArgsOnlyPatchAttempt result;
      result.disposition = Disposition::Accepted;
      result.patch = std::move(patch);
      return result;
    }
  };

  /// On-demand constructors for the short-lived macro-domain helper services
  /// that the args-only paths query.
  ///
  /// Each helper borrows the same dependency bundle and remains local to the
  /// replay attempt; the builder does not keep additional mutable helper state.
  RefoldMacroArgsOnlyTemplateSolver TemplateSolver() const;
  RefoldMacroOccurrenceReplay OccurrenceReplay() const;
  RefoldMacroPasteArgumentBuilder PasteArgumentBuilder() const;
  RefoldMacroDefinitionTapeSolver DefinitionTapeSolver() const;

  /// Return whether the invocation actual surface recovered by the producer
  /// contains enough bounded source ranges for args-only replay to attempt
  /// source-spelling reconstruction.  This is a fail-closed data-availability
  /// check only; it does not decide token-envelope or stringify/paste policy.
  bool InvocationActualsAreRecoverable(
      const InvocationActualRecoveryContext &ctx) const;

  /// Return whether derived argument replacements reproduce every stringified
  /// operand exactly as the edited stream spells it.
  ///
  /// A parameter used through `#` and through `##` is constrained twice, and
  /// the two constraints are independent.  When only the pasted product was
  /// rewritten -- `parse_mime` becoming `parse_mime_xjtr_0` while the literal
  /// `"mime"` is untouched -- the argument that satisfies the paste necessarily
  /// changes the stringified literal as well.  No argument text reproduces both
  /// operands, so the callsite is not refoldable and must fall back to the
  /// expanded text instead of silently corrupting the string.
  ///
  /// Each stringified operand is checked by decoding what the edited stream
  /// actually spells and comparing it against the derived replacement, so the
  /// obligation is discharged against B rather than against the paste that
  /// produced the replacement.  An operand whose B spelling cannot be recovered
  /// exactly fails closed.
  bool DerivedReplacementsReproduceStringifiedOperands(
      const RefoldModel::MacroInvocation &m,
      const llvm::DenseMap<uint32_t, std::string> &replacementsByArgIdx) const;

  /// Try paste-aware argument replay.  The result distinguishes a non-terminal
  /// miss from a fail-closed paste-surface rejection so the caller preserves
  /// the same search/fail-closed control flow.
  ArgsOnlyPatchAttempt
  BuildPasteAwareArgsOnlyPatch(const ArgsOnlyPlanningContext &ctx) const;

  Dependencies deps_;

  /// Envelope searches cached for the open `EnvelopeCacheScope`, or null.
  mutable std::unique_ptr<SyntheticEnvelopeCache> envelopeCache_;
};

} // namespace refold
} // namespace clang

#endif // LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDMACROSTANDARDARGSONLYPATCHBUILDER_H
