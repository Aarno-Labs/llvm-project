//===--- RefoldMacroStandardArgsOnlyPatchBuilder.h --------------*- C++ -*-===//
//
// Standard args-only patch builder for clang-refold.
//
// Owns `BuildStandardArgsOnlyPatch`: the entry point for the standard
// args-only ranking slot, including the pure paste-only fallback path that
// belongs to that same slot after specialized paste-aware proofs have
// declined.  The builder collects arg-span occurrences (and stringify
// occurrences) for the touched hunk, requires hunk coverage by those spans,
// and derives per-arg replacements from the B-side slices.  Higher-order
// generated-callee, generated-leaf, tuple-forwarded, and pure paste-only paths
// remain private implementation details reached through the references
// supplied in `Dependencies`.
//
// The builder has no back-reference to `RefoldMacroPatchPlanner`.
// Planner-side helpers that stay on the planner are reached through
// std::function callbacks (`buildInvocationRewriteWithRange`,
// `resolveFunctionLikeMacroThroughAliasesWithHops`); independent services are
// passed by reference.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDMACROSTANDARDARGSONLYPATCHBUILDER_H
#define LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDMACROSTANDARDARGSONLYPATCHBUILDER_H

#include "core/RefoldModel.h"
#include "edit/RefoldPatchTypes.h"
#include "macro/RefoldMacroArgsOnlyTemplateSolver.h"
#include "macro/RefoldMacroPasteArgumentBuilder.h"
#include "macro/RefoldMacroPlannerHelpers.h"
#include "macro/RefoldMacroReplay.h"
#include "source/RefoldToken.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/StringRef.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace clang {

class LangOptions;

namespace refold {

class RefoldArgTextRecovery;
class RefoldBInsertionLedger;
class RefoldMacroGeneratedCalleeReplayEngine;
class RefoldMacroGeneratedLeafReplayEngine;
class RefoldMacroPatchProofCertifier;
class RefoldMacroTopology;
class RefoldProofLattice;
class RefoldSourceMapper;

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
  /// All references must outlive the builder; the planner owns all of them.
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
    RefoldProofLattice &proofLattice;
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

  /// Run ordinary standard/stringify formal replay after specialized
  /// args-only replay paths did not produce a candidate.  Returns nullopt
  /// for a non-terminal miss.
  std::optional<MacroPatch>
  BuildStandardArgsOnlyPatch(const ArgsOnlyPlanningContext &ctx) const;

  /// Try the higher-order generated-callee replay theorem independently of
  /// args-only hunk admission.  Whole-cover planning uses this when the edited
  /// token is body-owned by the root expansion, but the producer graph still
  /// proves a source-preserving generated-callee rewrite.
  std::optional<MacroPatch> TryBuildHigherOrderGeneratedReplay(
      const RefoldModel::MacroInvocation &invocation,
      const diffutils::Hunk &hunk, llvm::StringRef baseInvocationText,
      llvm::ArrayRef<std::pair<size_t, size_t>> invocationArgRanges) const;

private:
  /// On-demand constructors for the short-lived macro-domain helper services
  /// that the standard args-only body queries.
  ///
  /// Each helper borrows the same dependency bundle and remains local to the
  /// replay attempt; the builder does not keep additional mutable helper state.
  RefoldMacroArgsOnlyTemplateSolver TemplateSolver() const;
  RefoldMacroOccurrenceReplay OccurrenceReplay() const;
  RefoldMacroPasteArgumentBuilder PasteArgumentBuilder() const;

  Dependencies deps_;
};

} // namespace refold
} // namespace clang

#endif // LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDMACROSTANDARDARGSONLYPATCHBUILDER_H
