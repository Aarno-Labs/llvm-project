//===--- RefoldMacroPatchPlanner.h -----------------------------*- C++ -*-===//
//
// Macro invocation patch planning service for clang-refold.
//
// This class owns macro patch orchestration while borrowing the exact source,
// token, topology, and proof dependencies it needs from the service graph.
// Domain-specific macro proof primitives live in named helper services such as
// RefoldMacroOccurrenceReplay, RefoldMacroActualLayout,
// RefoldMacroWholeCoverProof, RefoldMacroPasteSpelling, and
// RefoldMacroBoundarySelector.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDMACROPATCHPLANNER_H
#define LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDMACROPATCHPLANNER_H

#include "core/RefoldModel.h"
#include "edit/RefoldPatchTypes.h"
#include "macro/RefoldMacroArgsOnlyTemplateSolver.h"
#include "macro/RefoldMacroDefinitionTapeSolver.h"
#include "macro/RefoldMacroGeneratedCalleeReplayEngine.h"
#include "macro/RefoldMacroGeneratedLeafReplayEngine.h"
#include "macro/RefoldMacroOccurrenceProofValidator.h"
#include "macro/RefoldMacroPasteArgumentBuilder.h"
#include "macro/RefoldMacroPatchProofCertifier.h"
#include "macro/RefoldMacroPlannerHelpers.h"
#include "macro/RefoldMacroReplay.h"
#include "macro/RefoldMacroReplayStabilityValidator.h"
#include "macro/RefoldMacroStandardArgsOnlyPatchBuilder.h"
#include "macro/RefoldMacroSubtreeReplayValidator.h"
#include "macro/RefoldMacroWholeCoverOrchestrator.h"
#include "proof/RefoldAcceptedResultTypes.h"
#include "proof/RefoldOwnerStateTypes.h"
#include "source/DiffAlgorithms.h"
#include "source/RefoldToken.h"
#include "util/RefoldDenseMapInfo.h"

#include "clang/Basic/LangOptions.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace clang {
namespace refold {

class LineDirectiveInserter;
class RefoldBInsertionLedger;
class RefoldArgTextRecovery;
class RefoldMacroStateProof;
class RefoldMacroTopology;
class RefoldOwnerClassifier;
class RefoldOwnerStateProof;
class RefoldPathIdentity;
class RefoldProofLattice;
class RefoldSourceMapper;

/// Orchestrates macro patch planning for one engine object graph.
///
/// The planner coordinates macro replay, args-only/template/DAG/whole-cover
/// phases, reuse admission, state-repair hooks, and proof certification.  The
/// specialized algorithms live in focused macro services owned or borrowed by
/// the planner.
class RefoldMacroPatchPlanner {
  // No friend declarations.  The generated-leaf engine and whole-cover
  // orchestrator reach the planner-side helpers they need through
  // std::function callbacks (installed in their Dependencies bundles) and
  // the small set of public accessors declared below (`Deps`, proof-service
  // getters, spelling/tuple predicates, `RecoverWholeCoverReuseContext`,
  // and the sub-service accessors).
public:
  /// Nested alias for `::clang::refold::MacroPatchReuseAdmissionContext`
  /// (defined in `RefoldMacroPlannerHelpers.h`).  Declared ahead of its first
  /// use so the name has a single, consistent meaning throughout the class
  /// scope (GCC's -Wchanges-meaning otherwise flags the earlier bare uses),
  /// and kept so planner-internal references — including the qualified
  /// `RefoldMacroPatchPlanner::MacroPatchReuseAdmissionContext` spellings in
  /// the .cpp — continue to compile.
  using MacroPatchReuseAdmissionContext =
      ::clang::refold::MacroPatchReuseAdmissionContext;

  /// Explicit object-graph inputs for the macro-planning service.
  ///
  /// The planner borrows source/model/proof services directly.  Macro
  /// arbitration predicates live on the planner itself; the dependency bundle
  /// carries shared source state and proof services through stable non-owning
  /// links installed by the service graph.
  struct Dependencies {
    const RefoldModel *model = nullptr;
    llvm::StringRef bSource;
    llvm::ArrayRef<PPTok> aToks;
    llvm::ArrayRef<PPTok> bToks;
    llvm::ArrayRef<size_t> bTokOff;
    const std::vector<diffutils::Hunk> *abTokHunks = nullptr;
    const RefoldBInsertionLedger *bInsertionLedger = nullptr;
    const RefoldArgTextRecovery *argTextRecovery = nullptr;
    const clang::LangOptions *lexLang = nullptr;
    const LineDirectiveInserter *lineDirs = nullptr;
    const RefoldMacroTopology *macroTopology = nullptr;
    const RefoldPathIdentity *pathIdentity = nullptr;
    const RefoldSourceMapper *sourceMapper = nullptr;
    const RefoldOwnerClassifier *ownerClassifier = nullptr;
    bool strict = false;

    // Borrowed proof services.  They are initialized before the macro planner
    // and outlive it; direct pointers keep the service graph explicit without
    // lazy engine accessors.
    RefoldMacroStateProof *macroStateProof = nullptr;
    RefoldOwnerStateProof *ownerStateProof = nullptr;
    RefoldProofLattice *proofLattice = nullptr;
  };

  /// Recover per-formal argument content ranges for a function-like macro
  /// invocation spelling.  This is a macro-planner primitive because every
  /// caller uses the ranges to prove an invocation-preserving rewrite.
  std::optional<std::vector<std::pair<size_t, size_t>>>
  GetMacroInvocationFormalArgContentRanges(
      const RefoldModel::MacroInvocation &m, llvm::StringRef invText) const;

  /// Compute the A-token interval used for whole-cover replacement of a macro
  /// invocation.  The planner owns this because whole-cover admission, forced
  /// counter stabilization, and fallback reuse all consume the same macro proof
  /// surface.
  std::optional<std::pair<uint64_t, uint64_t>>
  GetWholeCoverATokRange(const RefoldModel::MacroInvocation &m) const;

  /// Nested-name alias for `::clang::refold::ExistingMacroPatchContext`
  /// (defined in `RefoldMacroWholeCoverOrchestrator.h`).  Preserved so
  /// existing external `RefoldMacroPatchPlanner::ExistingMacroPatchContext`
  /// references stay valid.
  using ExistingMacroPatchContext = ::clang::refold::ExistingMacroPatchContext;

  /// Construct the macro-planning service from explicit borrowed dependencies.
  explicit RefoldMacroPatchPlanner(Dependencies deps);

  /// Read-only access to the borrowed dependency bundle.  Sub-services
  /// (e.g. the whole-cover orchestrator's phase services) use this to build
  /// themselves from the same shared model/token/source/proof inputs the
  /// planner was constructed with, without needing friend access.
  const Dependencies &Deps() const { return deps_; }

  /// Read-only access to the macro-patch proof certifier for certification
  /// selected macro candidates and whole-cover proof carriers.
  const RefoldMacroPatchProofCertifier &ProofCertifier() const {
    return proofCertifier_;
  }

  /// Read-only access to the macro-subtree replay-stability validator.  Owns
  /// `SubtreePathHasProvableCalleeClosure`,
  /// `SubtreeReplayDoesNotContradictSiblingSurface`, and
  /// `ClaimedWholeEnvelopeIsReplaySafe`.
  const RefoldMacroSubtreeReplayValidator &SubtreeReplayValidator() const {
    return subtreeReplayValidator_;
  }

  /// Read-only access to the final-admission replay-stability validator.
  /// Owns `MacroCandidateReplayIsStableForFinalSelection` and the four
  /// sub-predicates it conjoins (callsite-syntax, args-only literal-body
  /// replay, root-preserving literal-body replay, header-macro-state
  /// observation).
  const RefoldMacroReplayStabilityValidator &ReplayStabilityValidator() const {
    return replayStabilityValidator_;
  }

  /// Read-only access to the higher-order generated-callee replay engine.
  /// Owns `GeneratedCalleeReplayPreservesEnvelope`,
  /// `GeneratedCalleeReplayIsAdmissible`,
  /// `BuildGeneratedCalleeReplayCandidate`, and
  /// `BuildTupleGeneratedCalleeReplayCandidate`.
  const RefoldMacroGeneratedCalleeReplayEngine &
  GeneratedCalleeReplayEngine() const {
    return generatedCalleeReplayEngine_;
  }

  /// Read-only access to the generated-leaf fallback replay engine.  Owns
  /// `BuildGeneratedLeafReplayCandidate`.  The engine reaches back into
  /// planner helpers via std::function callbacks installed in its
  /// Dependencies bundle, so no friend access is needed.
  const RefoldMacroGeneratedLeafReplayEngine &
  GeneratedLeafReplayEngine() const {
    return generatedLeafReplayEngine_;
  }

  /// Read-only access to the standard (non-paste) args-only patch builder.
  /// Owns `BuildStandardArgsOnlyPatch`.
  const RefoldMacroStandardArgsOnlyPatchBuilder &
  StandardArgsOnlyPatchBuilder() const {
    return standardArgsOnlyPatchBuilder_;
  }

  /// Build a structure-preserving tuple-terminal patch for roots that forward
  /// one tuple formal into multiple literal child invocations.  This theorem is
  /// narrower than ordinary args-only replay: every edited token in the root
  /// expansion must be explained by producer-recorded child argument,
  /// stringification, or paste evidence that maps back to exact slices of the
  /// same source-spelled root tuple.
  std::optional<MacroPatch> TryBuildTupleSiblingTerminalReplayPatch(
      const RefoldModel::MacroInvocation &m, const diffutils::Hunk &h,
      llvm::StringRef baseInvocationText) const;

  /// Build a structure-preserving invocation patch by rewriting only the
  /// callsite arguments when all touched macro occurrences replay consistently.
  std::optional<MacroPatch>
  BuildMacroInvocationPatchArgsOnly(const RefoldModel::MacroInvocation &m,
                                    const diffutils::Hunk &h,
                                    llvm::StringRef baseInvocationText) const;

  /// Compute the claim-aware B-side replacement surface for whole-cover macro
  /// realization at a callsite.
  std::optional<WholeCoverPlan>
  ComputeWholeCoverPlan(const RefoldModel::MacroInvocation &m) const;

  /// Check whether an existing whole-cover patch still matches the currently
  /// computed whole-cover replacement plan for the same proof root.
  bool WholeCoverPatchMatchesPlan(const MacroPatch &patch,
                                  const WholeCoverPlan &plan,
                                  uint64_t rootMacroId) const;

  /// Normalize a classified hunk owner into the stable owner certificate stored
  /// on macro patches.
  Owner NormalizeHunkOwnerForPatch(llvm::StringRef tuPath,
                                   const diffutils::Hunk &h) const;

  /// Return whether a patch's stored owner certificate matches the supplied
  /// normalized owner witness.
  bool MacroPatchOwnerMatches(const MacroPatch &patch,
                              const Owner &owner) const;

  /// Preserve an owner certificate when deriving a new patch from an accepted
  /// existing patch.  These wrappers are still part of the public planner
  /// service surface because owner-certificate certifying is still
  /// forwarded through the planner while the remaining caller-side seams are
  /// being retired.
  void CarryMacroPatchOwnerCertificate(MacroPatch &dst,
                                       const MacroPatch &src) const;

  /// Certify a normalized owner witness onto a newly constructed macro patch.
  /// The helper intentionally preserves the compact certificate format used by
  /// existing owner-match checks; it does not reclassify proof kind or theorem
  /// class.
  void CertifyMacroPatchOwnerWitness(MacroPatch &patch,
                                     const Owner &owner) const;

  /// Build the final callsite patch for a macro invocation, preferring
  /// args-only preservation and falling back to proven whole-cover realization.
  std::optional<MacroPatch> BuildMacroInvocationPatchWholeCover(
      const RefoldModel::MacroInvocation &m, const diffutils::Hunk &h,
      llvm::StringRef baseInvText,
      const llvm::DenseMap<std::optional<uint64_t>,
                           llvm::DenseMap<uint64_t, MacroPatch>> &patchMap)
      const;

  /// Build the final callsite patch while considering an already-coalesced
  /// same-span patch that the caller found before entering the planner.
  std::optional<MacroPatch> BuildMacroInvocationPatchWholeCover(
      const RefoldModel::MacroInvocation &m, const diffutils::Hunk &h,
      llvm::StringRef baseInvText,
      const llvm::DenseMap<std::optional<uint64_t>,
                           llvm::DenseMap<uint64_t, MacroPatch>> &patchMap,
      ExistingMacroPatchContext existingContext) const;

  /// Return the borrowed owner-state proof service used while certifying
  /// invocation patches that cross observable owner-state boundaries.
  /// Public so the whole-cover orchestrator can certify counter-literal
  /// patches without friend access.
  RefoldOwnerStateProof &GetOwnerStateProof() const;

  /// Return the borrowed proof lattice used for macro-patch proof
  /// construction, ranking, logging, and summary synchronization.  Public so
  /// the whole-cover orchestrator and final candidate selector can build
  /// proof carriers without friend access.
  RefoldProofLattice &GetProofLattice() const;

  /// Compare expected token spellings against an A-token half-open range.
  /// Public so the selector-substitution phase can use it via callback.
  bool TokenSpellingsEqualToA(llvm::ArrayRef<std::string> expected,
                              uint64_t beginTok, uint64_t endTok) const;

  /// Compare expected token spellings against a B-token half-open range.
  bool TokenSpellingsEqualToB(llvm::ArrayRef<std::string> expected,
                              uint64_t beginTok, uint64_t endTok) const;

  /// Return whether an argument spelling is a balanced parenthesized tuple
  /// at top level.  Public so the final candidate selector can detect tuple
  /// collapses through a callback.
  bool IsParenthesizedTuple(llvm::StringRef arg) const;

  /// All-occurrence ignore-paste replacement check using semantic proof.
  /// Public so the DAG lifting phase can reach it through a callback.
  bool MacroArgReplacementMatchesAllOccurrencesInBIgnorePasteSemanticProof(
      const RefoldModel::MacroInvocation &m, uint32_t argIdx,
      llvm::StringRef baseArg, llvm::StringRef newArg,
      llvm::ArrayRef<diffutils::Hunk> tokenHunks) const;

  /// Build the reuse-admission state for whole-cover planning from explicit
  /// caller-owned map/context inputs.  The returned carrier borrows the
  /// owner witness supplied by the caller and does not take ownership of
  /// patch-map storage.  Public so the whole-cover orchestrator can compute
  /// the same reuse context without friend access.
  MacroPatchReuseAdmissionContext RecoverWholeCoverReuseContext(
      const RefoldModel::MacroInvocation &invocation,
      const Owner &currentPatchOwner, uint64_t invocationStart,
      uint64_t invocationEnd,
      const llvm::DenseMap<std::optional<uint64_t>,
                           llvm::DenseMap<uint64_t, MacroPatch>> &patchMap,
      ExistingMacroPatchContext existingContext) const;

private:
  /// Return the borrowed macro-state proof service.  The named accessor
  /// centralizes the invariant that service-graph construction installed a
  /// stable proof service before the planner was created.
  RefoldMacroStateProof &GetMacroStateProof() const;

  RefoldMacroOccurrenceReplay OccurrenceReplay() const;
  RefoldMacroActualLayout ActualLayout() const;

  bool MacroArgReplacementMatchesAllOccurrencesInB(
      const RefoldModel::MacroInvocation &m, uint32_t argIdx,
      llvm::StringRef baseArg, llvm::StringRef newArg,
      llvm::ArrayRef<diffutils::Hunk> tokenHunks) const;

  bool MacroArgReplacementMatchesAllOccurrencesInBIgnorePaste(
      const RefoldModel::MacroInvocation &m, uint32_t argIdx,
      llvm::StringRef baseArg, llvm::StringRef newArg,
      llvm::ArrayRef<diffutils::Hunk> tokenHunks) const;

  /// Local aliases for namespace-scope macro-planning carrier types.
  ///
  /// The aliases keep planner internals readable while the actual carrier
  /// definitions live beside the services that share them.
  ///
  ///   * `InvocationRewriteWithRange` — an invocation spelling produced by
  ///     a formal-rewrite path plus the replacement-relative byte interval
  ///     that materializes B output.
  ///   * `ActualContentRange` / `InvocationActualLayout` — the parsed
  ///     callsite formal-content surface used by args-only replay.
  ///   * `InvocationActualRecoveryContext` — a borrowed view over the
  ///     above layouts so generated-callee/tuple replay helpers can reuse
  ///     caller-owned storage without copying.
  using InvocationRewriteWithRange =
      ::clang::refold::InvocationRewriteWithRange;
  using ActualContentRange = ::clang::refold::ActualContentRange;
  using InvocationActualLayout = ::clang::refold::InvocationActualLayout;
  using InvocationActualRecoveryContext =
      ::clang::refold::InvocationActualRecoveryContext;

  /// Recover the parsed invocation-actual layout needed by args-only replay.
  /// This performs only the admissibility precondition checks and source-range
  /// recovery; candidate construction and ranking remain separate operations.
  std::optional<InvocationActualLayout>
  RecoverInvocationActuals(const RefoldModel::MacroInvocation &invocation,
                           llvm::StringRef baseInvocationText) const;

  /// Nested alias for the args-only planning-context carrier (defined in
  /// `RefoldMacroPlannerHelpers.h`).  The template solver's own carrier
  /// types (`ArgsOnlyTemplateElem`, `CurrentLevelTemplateSurface`,
  /// `ArgsOnlyTemplateReplayContext`) live in
  /// `RefoldMacroArgsOnlyTemplateSolver.h` and are reached directly through
  /// their namespace-scope names.
  using ArgsOnlyPlanningContext = ::clang::refold::ArgsOnlyPlanningContext;

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

  /// Construct the definition-tape replay solver on demand. Lightweight;
  /// borrows source/model/proof state from the planner's dependencies.
  RefoldMacroDefinitionTapeSolver DefinitionTapeSolver() const;

  /// Try paste-aware argument replay.  The result distinguishes a non-terminal
  /// miss from a fail-closed paste-surface rejection so the caller preserves
  /// the same search/fail-closed control flow.
  ArgsOnlyPatchAttempt
  BuildPasteAwareArgsOnlyPatch(const ArgsOnlyPlanningContext &ctx) const;

  /// Nested aliases for the generated-callee replay engine's carrier types
  /// (defined in `RefoldMacroGeneratedCalleeReplayEngine.h`).
  ///
  ///   * `GeneratedCalleeSourceSlot` — a source slot tracked while
  ///     following a generated-callee chain.
  ///   * `GeneratedCalleeReplayContext` — the explicit state bundle used
  ///     during higher-order generated-callee replay; the replay-chain
  ///     vectors and flags remain caller-owned.
  using GeneratedCalleeSourceSlot = ::clang::refold::GeneratedCalleeSourceSlot;
  using GeneratedCalleeReplayContext =
      ::clang::refold::GeneratedCalleeReplayContext;

  /// Local alias for the generated-leaf replay context defined in
  /// `RefoldMacroGeneratedLeafReplayEngine.h`.
  using GeneratedLeafReplayContext =
      ::clang::refold::GeneratedLeafReplayContext;

  /// Local alias for the tuple-generated-callee replay context.
  ///
  /// Tuple replay edits tuple elements inside one root formal; the context
  /// keeps owner/forwarder identity and object-alias hop counts explicit while
  /// local tuple solver state stays inside the replay method.
  using TupleGeneratedCalleeReplayContext =
      ::clang::refold::TupleGeneratedCalleeReplayContext;

  /// Return whether the invocation actual surface recovered by the producer
  /// contains enough bounded source ranges for args-only replay to attempt
  /// source-spelling reconstruction.  This is a fail-closed data-availability
  /// check only; it does not decide token-envelope or stringify/paste policy.
  bool InvocationActualsAreRecoverable(
      const InvocationActualRecoveryContext &ctx) const;

  /// Build a concrete rewritten invocation spelling from explicit per-formal
  /// replacements, preserving the materialized-output byte interval logic used
  /// by args-only source-spelling recovery.
  std::optional<InvocationRewriteWithRange> BuildInvocationRewriteWithRange(
      const InvocationActualRecoveryContext &ctx,
      const llvm::DenseMap<uint32_t, std::string> &replByArgIdx,
      const llvm::DenseMap<uint32_t, std::pair<uint64_t, uint64_t>>
          *materializedRangeByArgIdx = nullptr) const;

  /// Construct the args-only template solver on demand. Lightweight; borrows
  /// source/model/proof state from the planner's dependencies.
  RefoldMacroArgsOnlyTemplateSolver TemplateSolver() const;

  /// Literalize a direct __COUNTER__ invocation when the B-side replacement is
  /// recoverable. This keeps the counter-specific owner-state proof explicit
  /// instead of burying it in the main planner body.
  std::optional<MacroPatch> TryCounterLiteralWholeCoverPatch(
      const RefoldModel::MacroInvocation &invocation,
      const diffutils::Hunk &hunk, uint64_t invocationStart,
      uint64_t invocationEnd) const;

  /// Return whether an existing patch covers the same physical invocation span
  /// and belongs to the same macro patch owner as the current reuse request.
  bool ExistingPatchMatchesReuseSite(const MacroPatchReuseAdmissionContext &ctx,
                                     const MacroPatch &patch) const;

  /// Return whether an existing patch is a structure-preserving callsite patch
  /// for the current invocation being planned.
  bool ExistingPatchPreservesCurrentInvocation(
      const MacroPatchReuseAdmissionContext &ctx,
      const MacroPatch &patch) const;

  /// Recover deterministic same-span reuse candidates from the caller-owned
  /// macro patch map without taking ownership of map storage.
  void CollectExistingMacroPatchReuseFromMap(
      MacroPatchReuseAdmissionContext &ctx,
      const llvm::DenseMap<std::optional<uint64_t>,
                           llvm::DenseMap<uint64_t, MacroPatch>> &patchMap)
      const;

  /// Admit the caller-coalesced same-pass patch context when the patch map did
  /// not already provide a same-span reuse candidate.
  void AdmitCallerExistingMacroPatchContext(
      MacroPatchReuseAdmissionContext &ctx,
      ExistingMacroPatchContext existingContext) const;

  /// Construct the paste-aware argument-builder service on demand. Lightweight;
  /// borrows source/token state from the planner's dependencies.
  RefoldMacroPasteArgumentBuilder PasteArgumentBuilder() const;

  /// Certify the macro patch with the B-token envelope for the invocation's
  /// whole-cover expansion, preserving boundary insertions for proof replay.
  bool
  CertifyMacroPatchWholeExpansionBRange(const RefoldModel::MacroInvocation &m,
                                        MacroPatch &patch) const;

  /// Return whether the A token at the given index has exactly the expected
  /// spelling.
  bool MatchLiteralAToken(uint64_t tok, llvm::StringRef spelling) const;

  /// Construct the subtree-membership / occurrence-proof predicate service
  /// on demand. Lightweight; borrows model and topology from planner deps.
  RefoldMacroOccurrenceProofValidator OccurrenceProofValidator() const;

  /// Resolve a function-like macro name through object-like single-token alias
  /// hops, reporting the number of hops consumed by the replay proof.
  const RefoldModel::MacroDirective *
  ResolveFunctionLikeMacroThroughAliasesWithHops(llvm::StringRef startName,
                                                 uint32_t *aliasHops) const;

  /// Resolve a function-like macro name through replay-safe alias hops when the
  /// hop count is not needed by the caller.
  const RefoldModel::MacroDirective *
  ResolveFunctionLikeMacroForReplay(llvm::StringRef startName) const;

  /// Return whether the named object-like macro expands to a single token that
  /// can be used as a callee alias during replay proof.
  bool IsObjectLikeSingleTokenAlias(llvm::StringRef name) const;

  Dependencies deps_;
  RefoldMacroPatchProofCertifier proofCertifier_;
  RefoldMacroSubtreeReplayValidator subtreeReplayValidator_;
  RefoldMacroReplayStabilityValidator replayStabilityValidator_;
  RefoldMacroGeneratedCalleeReplayEngine generatedCalleeReplayEngine_;
  RefoldMacroGeneratedLeafReplayEngine generatedLeafReplayEngine_;
  RefoldMacroStandardArgsOnlyPatchBuilder standardArgsOnlyPatchBuilder_;
};

} // namespace refold
} // namespace clang

#endif // LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDMACROPATCHPLANNER_H
