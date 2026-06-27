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
#include "macro/RefoldMacroReplay.h"
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

class RefoldMacroPatchPlanner {
public:
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

  /// Existing patch already associated with the physical invocation span by
  /// the caller before entering the planner.
  ///
  /// The caller owns the patch map and has already coalesced physical callsite
  /// spans to one canonical patch key.  Passing that exact candidate through
  /// preserves same-pass reuse without making the planner depend on
  /// the engine object or re-discover caller-local map state.
  struct ExistingMacroPatchContext {
    const MacroPatch *patch = nullptr;
    bool isCallsite = false;
  };

  /// Construct the macro-planning service from explicit borrowed dependencies.
  explicit RefoldMacroPatchPlanner(Dependencies deps);

  /// Build a structure-preserving invocation patch by rewriting only the
  /// callsite arguments when all touched macro occurrences replay consistently.
  std::optional<MacroPatch> BuildMacroInvocationPatchArgsOnly(
      const RefoldModel::MacroInvocation &m, const diffutils::Hunk &h,
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
  /// service surface because owner-certificate stamping is still
  /// forwarded through the planner while the remaining caller-side seams are
  /// being retired.
  void CarryMacroPatchOwnerCertificate(MacroPatch &dst,
                                       const MacroPatch &src) const;

  /// Stamp a normalized owner witness onto a newly constructed macro patch.
  /// The helper intentionally preserves the compact certificate format used by
  /// existing owner-match checks; it does not reclassify proof kind or theorem
  /// class.
  void StampMacroPatchOwnerWitness(MacroPatch &patch,
                                   const Owner &owner) const;

  /// Build the final callsite patch for a macro invocation, preferring
  /// args-only preservation and falling back to proven whole-cover realization.
  std::optional<MacroPatch> BuildMacroInvocationPatchWholeCover(
      const RefoldModel::MacroInvocation &m, const diffutils::Hunk &h,
      llvm::StringRef baseInvText,
      const llvm::DenseMap<std::optional<uint64_t>,
                           llvm::DenseMap<uint64_t, MacroPatch>>
          &patchMap) const;

  /// Build the final callsite patch while considering an already-coalesced
  /// same-span patch that the caller found before entering the planner.
  std::optional<MacroPatch> BuildMacroInvocationPatchWholeCover(
      const RefoldModel::MacroInvocation &m, const diffutils::Hunk &h,
      llvm::StringRef baseInvText,
      const llvm::DenseMap<std::optional<uint64_t>,
                           llvm::DenseMap<uint64_t, MacroPatch>>
          &patchMap,
      ExistingMacroPatchContext existingContext) const;

private:
  enum class FinalMacroCandidateOrigin : uint8_t;
  struct FinalMacroCandidate;
  struct FinalMacroCandidateAdmissionContext;
  struct MacroPatchReuseAdmissionContext;
  struct WholeCoverAdmissionContext;
  struct WholeCoverArgsOnlyCandidateContext;
  struct WholeCoverArgsOnlyCandidateResult;
  struct WholeCoverFinalSelectionContext;


  /// Return the borrowed macro-state proof service.  The named accessor
  /// centralizes the invariant that service-graph construction installed a
  /// stable proof service before the planner was created.
  RefoldMacroStateProof &GetMacroStateProof() const;

  /// Return the borrowed owner-state proof service used while stamping
  /// invocation patches that cross observable owner-state boundaries.
  RefoldOwnerStateProof &GetOwnerStateProof() const;

  /// Return the borrowed proof lattice used for macro-patch proof construction,
  /// ranking, logging, and summary synchronization.
  RefoldProofLattice &GetProofLattice() const;

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

  bool MacroArgReplacementMatchesAllOccurrencesInBIgnorePasteSemanticProof(
      const RefoldModel::MacroInvocation &m, uint32_t argIdx,
      llvm::StringRef baseArg, llvm::StringRef newArg,
      llvm::ArrayRef<diffutils::Hunk> tokenHunks) const;

  /// Root-scoped state shared by DAG/subtree replay validation helpers.
  ///
  /// The DAG path proves a descendant rewrite by lifting it through producer
  /// caller edges back to one root invocation.  This carrier names that root
  /// surface and the caller-owned invocation index; it borrows storage only and
  /// does not promote per-call solver state into planner fields.
  struct MacroSubtreeReplayValidationContext {
    const RefoldModel::MacroInvocation &rootInvocation;
    llvm::StringRef rootInvocationText;
    llvm::ArrayRef<std::pair<size_t, size_t>> rootInvocationArgRanges;
    const llvm::DenseMap<uint64_t, const RefoldModel::MacroInvocation *>
        &invocationById;
  };

  /// Invocation spelling produced by a formal-rewrite path together with the
  /// replacement-relative byte interval that materializes B output.
  ///
  /// This is a planner-private carrier for actual-text recovery.  It owns only
  /// the rewritten invocation spelling and the byte interval inside that
  /// spelling; it does not carry proof ranking or candidate-admission state.
  struct InvocationRewriteWithRange {
    std::string text;
    uint64_t materializedOutputByteStart = 0;
    uint64_t materializedOutputByteEnd = 0;
  };

  /// One parsed formal-actual content range inside the invocation spelling.
  ///
  /// The recovery service still exposes ranges as plain byte pairs.  This
  /// carrier names the meaning of those two offsets at planner API boundaries
  /// without changing the underlying storage used by existing ArrayRef-based
  /// helpers.
  struct ActualContentRange {
    size_t begin = 0;
    size_t end = 0;
  };

  /// Complete parsed actual layout for one invocation spelling.
  ///
  /// The layout owns the recovered formal-content ranges for the duration of
  /// one args-only planning attempt.  Existing replay helpers can borrow the
  /// pair-backed view, while the planner can talk about an invocation actual
  /// layout instead of a loose vector of unrelated offsets.
  struct InvocationActualLayout {
    std::vector<std::pair<size_t, size_t>> contentRanges;

    bool empty() const { return contentRanges.empty(); }

    llvm::ArrayRef<std::pair<size_t, size_t>> rangePairs() const {
      return contentRanges;
    }

    ActualContentRange rangeAt(size_t index) const {
      const auto &range = contentRanges[index];
      return ActualContentRange{range.first, range.second};
    }
  };

  /// Borrowed invocation-actual recovery inputs shared by args-only rewrite
  /// helpers.
  ///
  /// The ranges are the already-parsed callsite formal-content surface inside
  /// `baseInvocationText`.  `RecoverInvocationActuals` owns those ranges in an
  /// InvocationActualLayout; this context deliberately borrows an ArrayRef view
  /// so generated-callee and tuple replay helpers can continue to reuse their
  /// caller-owned layouts without copying or changing fallback policy.
  struct InvocationActualRecoveryContext {
    const RefoldModel::MacroInvocation &invocation;
    llvm::StringRef baseInvocationText;
    llvm::ArrayRef<std::pair<size_t, size_t>> invocationArgRanges;
  };

  /// Recover the parsed invocation-actual layout needed by args-only replay.
  /// This performs only the historical availability checks and source-range
  /// recovery; candidate construction and ranking remain separate operations.
  std::optional<InvocationActualLayout>
  RecoverInvocationActuals(const RefoldModel::MacroInvocation &invocation,
                           llvm::StringRef baseInvocationText) const;

  /// Template element over a macro whole-cover: either fixed body tokens or one
  /// standard formal occurrence whose B-side interval must be solved.
  ///
  /// This is the exact carrier previously owned by the local args-only template
  /// solver.  It remains a lightweight value type; moving it into the planner
  /// only gives named replay helpers a stable parameter type.
  struct ArgsOnlyTemplateElem {
    bool isArg = false;
    uint64_t aBegin = 0;
    uint64_t aEnd = 0;
    uint32_t argIdx = 0;
    size_t occurrenceOrdinal = 0;
  };

  /// Current-level replay surface for one macro invocation.
  ///
  /// `standardSpans` are rebased to parsed formal slots.  `[coverBegin,
  /// coverEnd)` is the exact A-token tape covered by those spans and fixed body
  /// spans.  The helper methods below consume this carrier without changing the
  /// exact-cover proof policy.
  struct CurrentLevelTemplateSurface {
    std::vector<RefoldModel::PPArgSpan> standardSpans;
    uint64_t coverBegin = 0;
    uint64_t coverEnd = 0;
  };

  /// Explicit state bundle for args-only template replay.
  ///
  /// Template replay needs the current invocation, its source spelling, and the
  /// parsed formal ranges when recovering actual text and proving split formal
  /// occurrences.  It is intentionally read-only and view-based: candidate
  /// vectors, DFS state, and proof results remain caller-local.
  struct ArgsOnlyTemplateReplayContext {
    const RefoldModel::MacroInvocation &invocation;
    llvm::StringRef baseInvocationText;
    llvm::ArrayRef<std::pair<size_t, size_t>> invocationArgRanges;
  };

  /// Shared read-only state for the args-only planning pipeline.
  ///
  /// `BuildMacroInvocationPatchArgsOnly` owns the recovered layout. Replay
  /// helpers borrow it through this carrier so the entry point can orchestrate
  /// definition-tape replay, paste-aware replay, and ordinary formal replay
  /// without promoting per-call state into planner fields.
  struct ArgsOnlyPlanningContext {
    const RefoldModel::MacroInvocation &invocation;
    const diffutils::Hunk &hunk;
    llvm::StringRef baseInvocationText;
    const InvocationActualLayout &actualLayout;
  };

  /// Result of one args-only candidate attempt.
  ///
  /// Paste-aware replay needs to distinguish "no candidate, keep trying" from
  /// "the touched paste surface was invalid, fail closed."  The old monolithic
  /// function expressed that with local fallthrough versus immediate returns;
  /// this carrier preserves the same control flow explicitly.
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

  /// Try the definition replacement-list replay proof used for empty actuals,
  /// zero-token formal occurrences, and __VA_OPT__ branch flips.  A miss here
  /// is non-terminal, matching the former local lambda fallthrough behavior.
  std::optional<MacroPatch> TryDefinitionTapeReplayArgsOnlyPatch(
      const ArgsOnlyPlanningContext &ctx) const;

  /// Try paste-aware argument replay.  The result distinguishes a non-terminal
  /// miss from a fail-closed paste-surface rejection so the caller preserves the
  /// same search/fail-closed control flow.
  ArgsOnlyPatchAttempt
  BuildPasteAwareArgsOnlyPatch(const ArgsOnlyPlanningContext &ctx) const;

  /// Run ordinary standard/stringify formal replay after specialized
  /// args-only replay paths did not produce a candidate.
  std::optional<MacroPatch>
  BuildStandardArgsOnlyPatch(const ArgsOnlyPlanningContext &ctx) const;


  /// Source slot tracked while following a generated-callee chain.
  ///
  /// `text` is the actual spelling at the current replay level.  The root
  /// fields identify the original invocation argument that must be edited if
  /// the final generated callee solves a different value.
  struct GeneratedCalleeSourceSlot {
    std::string text;
    std::string rootSourceText;
    uint32_t rootArgIdx = 0;
  };

  /// Explicit state bundle for higher-order generated-callee replay.
  ///
  /// The replay-chain vectors and flags remain caller-owned so candidate
  /// ranking and proof stamping keep their existing lifetime and ordering.  The
  /// named context makes that replay state explicit for extraction.
  struct GeneratedCalleeReplayContext {
    const RefoldModel::MacroInvocation &invocation;
    llvm::StringRef baseInvocationText;
    llvm::ArrayRef<std::pair<size_t, size_t>> invocationArgRanges;
    const RefoldModel::MacroDirective &rootDefinition;
    const std::pair<uint64_t, uint64_t> &wholeCoverATokens;
    const std::pair<size_t, size_t> &bTokenEnvelope;
    llvm::SmallVectorImpl<std::string> &replayPrefixLiterals;
    llvm::SmallVectorImpl<llvm::SmallVector<std::string, 8>> &replaySuffixStack;
    const RefoldModel::MacroDirective *&currentDefinition;
    llvm::SmallVectorImpl<GeneratedCalleeSourceSlot> &currentActuals;
    bool &followedGeneratedCall;
    uint32_t &generatedCallDepth;
    uint32_t &objectAliasHopCount;
    bool &usesStringification;
    bool &usesPaste;
    bool &usesVariadicForwarding;
  };

  /// Explicit expansion surface for the generated-leaf replay fallback.
  ///
  /// The leaf fallback reuses the already-proved whole-cover envelope and root
  /// definition while solving scalar or multi-leaf source edits from old/new
  /// expansion text.  It does not add a new fallback path.
  struct GeneratedLeafReplayContext {
    const RefoldModel::MacroInvocation &invocation;
    const diffutils::Hunk &hunk;
    llvm::StringRef baseInvocationText;
    llvm::ArrayRef<std::pair<size_t, size_t>> invocationArgRanges;
    const std::pair<uint64_t, uint64_t> &wholeCoverATokens;
    std::pair<size_t, size_t> &bTokenEnvelope;
    const RefoldModel::MacroDirective &rootDefinition;
    llvm::StringRef oldExpansion;
    llvm::StringRef newExpansion;
  };

  /// Explicit root/forwarder state for tuple-generated callee replay.
  ///
  /// Tuple replay edits tuple elements inside one root formal.  The context
  /// keeps owner/forwarder identity and the object-alias hop counter explicit
  /// while preserving the local tuple solver state inside the replay method.
  struct TupleGeneratedCalleeReplayContext {
    const RefoldModel::MacroInvocation &invocation;
    llvm::StringRef baseInvocationText;
    llvm::ArrayRef<std::pair<size_t, size_t>> invocationArgRanges;
    const std::pair<uint64_t, uint64_t> &wholeCoverATokens;
    const std::pair<size_t, size_t> &bTokenEnvelope;
    const RefoldModel::MacroDirective &rootDefinition;
    const RefoldModel::MacroDirective &forwarderDefinition;
    uint32_t callerArgIdx = 0;
    uint32_t &objectAliasHopCount;
  };

  /// Concrete whole-cover realization candidate before final theorem-lattice
  /// admission.  This carrier keeps the source span and whole-cover plan
  /// together so realization construction is not expressed as scattered local
  /// variables in the final selector.
  struct WholeCoverCandidate {
    const RefoldModel::MacroInvocation &invocation;
    uint64_t invocationStart = 0;
    uint64_t invocationEnd = 0;
    const WholeCoverPlan &plan;
  };

  /// Proof inputs needed to stamp one whole-cover realization candidate.
  /// The proof lattice still performs the actual stamping; this carrier only
  /// names the exact invocation/plan pair that must stay synchronized.
  struct WholeCoverProofInputs {
    const RefoldModel::MacroInvocation &invocation;
    const WholeCoverPlan &plan;
  };

  /// Inputs for direct root args-only whole-cover recovery.
  ///
  /// The request borrows the already-trimmed hunk and the caller-owned reuse
  /// carrier, then computes only the root-level args-only candidate facts. It
  /// deliberately does not run DAG lifting or whole-cover realization.
  struct WholeCoverArgsOnlyCandidateContext {
    const RefoldModel::MacroInvocation &invocation;
    const diffutils::Hunk &effectiveHunk;
    llvm::StringRef baseInvocationText;
    llvm::ArrayRef<RefoldModel::PPArgSpan> argLikeSpans;
    const MacroPatchReuseAdmissionContext &reuseAdmission;
  };

  /// Output of direct root args-only whole-cover recovery.
  ///
  /// `rootHasDirectArgLikeSurface` is intentionally carried forward because
  /// later DAG replay uses the same fact to decide whether unsupported
  /// descendant structure should suppress direct root preservation.
  struct WholeCoverArgsOnlyCandidateResult {
    std::optional<MacroPatch> argsOnlyCandidate;
    bool reuseExistingCallsitePatch = false;
    bool rootHasDirectArgLikeSurface = false;
  };

  /// Final whole-cover arbitration state. The large whole-cover planner still
  /// owns candidate discovery, while replay-stability checks are routed through
  /// the named subtree validation context. This carrier names the already-
  /// discovered candidates so final admission/selection stays separate from
  /// construction without changing ownership or ranking.
  struct WholeCoverFinalSelectionContext {
    const RefoldModel::MacroInvocation &invocation;
    const diffutils::Hunk &effectiveHunk;
    llvm::StringRef baseInvocationText;
    uint64_t invocationStart = 0;
    uint64_t invocationEnd = 0;
    const std::optional<MacroPatch> &argsOnlyCandidate;
    const std::optional<MacroPatch> &dagRootCandidate;
    bool canReuseExistingCallsiteNoOp = false;
    bool canReuseExistingCallsiteSkipWholeCover = false;
    bool canReuseExistingExpanded = false;
    const MacroPatch *existingPatch = nullptr;
    const MacroPatch *existingExpandedPatch = nullptr;
    const std::optional<WholeCoverPlan> &wholeCoverPlan;
    MacroPatchReuseAdmissionContext &reuseAdmissionCtx;
    const MacroSubtreeReplayValidationContext &replayStabilityCtx;
    bool allowNonTopLevelMacroSelectorFailure = false;
  };

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

  /// Stamp the source-side materialized-output byte interval recovered while
  /// rebuilding an invocation spelling.  B-token envelope stamping remains
  /// separate because each replay path proves that envelope through its own
  /// carrier.
  void StampInvocationRewriteMaterializedOutputRange(
      MacroPatch &patch, const InvocationRewriteWithRange &rewrite) const;

  /// Return the standard current-level argument spans used by args-only
  /// template replay, rebased to the parsed formal argument order.  This is the
  /// named exact-cover span-repair helper used by template replay.
  std::optional<std::vector<RefoldModel::PPArgSpan>>
  GetCurrentLevelStandardArgSpans(
      const ArgsOnlyTemplateReplayContext &ctx) const;

  /// Build the exact current-level body/formal replay surface for an invocation
  /// and a parsed formal-range vector.  Recursive template reconstruction uses
  /// this named helper while keeping recursion-local search state local.
  std::optional<CurrentLevelTemplateSurface>
  GetCurrentLevelTemplateSurfaceForInvocation(
      const RefoldModel::MacroInvocation &invocation,
      llvm::ArrayRef<std::pair<size_t, size_t>> formalRanges) const;

  /// Convenience wrapper used when only rebased standard spans are needed.
  std::optional<std::vector<RefoldModel::PPArgSpan>>
  GetCurrentLevelStandardArgSpansForInvocation(
      const RefoldModel::MacroInvocation &invocation,
      llvm::ArrayRef<std::pair<size_t, size_t>> formalRanges) const;

  /// Return the old/new expansion text for one invocation's current-level
  /// template surface, not the broader producer whole-cover.
  std::optional<std::pair<std::string, std::string>>
  GetCurrentLevelExpansionTextForInvocation(
      const RefoldModel::MacroInvocation &invocation) const;

  /// Return whether current-level args-only replay preserves the entire macro
  /// expansion envelope, including fixed body tokens before/after formal slots.
  bool ArgsOnlyTemplateReplayPreservesEnvelope(
      const RefoldModel::MacroInvocation &invocation,
      const CurrentLevelTemplateSurface &surface) const;

  /// Attempts to construct an args-only macro patch from the solved template
  /// replay surface.  The recursive syntax repair and bounded DFS remain local
  /// to this helper so their mutable search state is not promoted to planner
  /// fields.
  std::optional<MacroPatch> TryTemplateSolvedArgsOnlyPatch(
      const ArgsOnlyTemplateReplayContext &ctx) const;

  /// Attach the args-only proof carrier without changing proof kind, theorem
  /// class, or owner certificate state.
  void AttachArgsOnlyProofCarrier(
      const ArgsOnlyTemplateReplayContext &ctx, MacroPatch &patch,
      bool wholeEnvelopeReplayValidated,
      bool definitionTapeReplayValidated = false) const;

  /// Stamp accepted-result metadata for a template-solved args-only patch: the
  /// source-replacement byte interval and the B-token proof envelope.
  void StampArgsOnlyAcceptedCandidate(
      const ArgsOnlyTemplateReplayContext &ctx, MacroPatch &patch,
      const InvocationRewriteWithRange &rewrite,
      std::pair<size_t, size_t> bTokenEnvelope) const;


  /// Return whether the generated-callee replay context still names a non-empty
  /// whole-cover token envelope.  The caller computes the envelope exactly as
  /// before; this helper only makes the proof gate explicit.
  bool GeneratedCalleeReplayPreservesEnvelope(
      const GeneratedCalleeReplayContext &ctx) const;

  /// Return whether the generated-callee chain has advanced to one admissible
  /// final callee with arity-compatible actuals.
  bool GeneratedCalleeReplayIsAdmissible(
      const GeneratedCalleeReplayContext &ctx) const;

  /// Attempts to build the higher-order generated-callee replay candidate from
  /// explicit chain state without changing parent/child eligibility or ranking.
  std::optional<MacroPatch> BuildGeneratedCalleeReplayCandidate(
      const GeneratedCalleeReplayContext &ctx) const;

  /// Attempts to build the generated-leaf fallback candidate from the already
  /// recovered whole-cover expansion surface.
  std::optional<MacroPatch> BuildGeneratedLeafReplayCandidate(
      const GeneratedLeafReplayContext &ctx) const;

  /// Attempts to build the tuple-generated callee replay candidate without
  /// adding fallback paths or changing tuple/callee ranking.
  std::optional<MacroPatch> BuildTupleGeneratedCalleeReplayCandidate(
      const TupleGeneratedCalleeReplayContext &ctx) const;

  /// Add one already-discovered final macro candidate to the caller-owned
  /// candidate vector without changing candidate discovery order.  The helper
  /// performs the same replay-stability gate and selector-carrier construction
  /// used by final macro-candidate admission.
  void AddFinalMacroCandidate(FinalMacroCandidateAdmissionContext &ctx,
                              FinalMacroCandidate candidate) const;

  /// Record which final-admission proof path contributed a candidate.  These
  /// flags are bookkeeping only; proof-lattice ranking remains the selector.
  void NoteFinalMacroCandidateOrigin(
      FinalMacroCandidateAdmissionContext &ctx,
      FinalMacroCandidateOrigin origin) const;

  /// Return whether an already-admitted structure-preserving candidate
  /// dominates a realization candidate under the theorem lattice.  The scan is
  /// intentionally over the caller-owned candidate vector so this preserves the
  /// existing realization-suppression rule exactly.
  bool TheoremLatticeStructureCandidateDominatesRealization(
      const FinalMacroCandidateAdmissionContext &ctx,
      const MacroPatch &realizationPatch) const;

  /// Run the final macro selector over the caller-owned candidate vector.  The
  /// result indexes back into that same vector, so selection does not transfer
  /// candidate ownership or reorder candidates.
  std::optional<SelectedMacroSelectionCandidate>
  SelectPreferredFinalMacroCandidate(
      const FinalMacroCandidateAdmissionContext &ctx) const;

  /// Stamp the emitted accepted-result carrier selected by the final macro
  /// selector.  Selector-only artifacts deliberately remain unstamped so the
  /// final emission gate still fails closed unless a theorem-normalized
  /// emitted carrier exists.
  void StampSelectedFinalMacroCandidate(
      const FinalMacroCandidateAdmissionContext &ctx,
      const SelectedMacroSelectionCandidate &selectedCandidate,
      MacroPatch &selectedPatch) const;

  /// Attach the whole-cover realization proof carrier without changing theorem
  /// class, proof-root id, owner-realization witness state, or whole-cover
  /// envelope metadata.
  void AttachWholeCoverProofCarrier(
      const WholeCoverAdmissionContext &ctx, MacroPatch &patch) const;

  /// Stamp the accepted candidate metadata for a whole-cover realization patch.
  /// This is intentionally a thin named wrapper around the same proof-lattice
  /// stamp used before extraction.
  void StampWholeCoverAcceptedCandidate(
      const WholeCoverAdmissionContext &ctx, MacroPatch &patch) const;

  /// Reused macro patches already carry their proof/accepted-result metadata
  /// from the earlier accepted patch.  This named seam documents that reuse does
  /// not restamp or reclassify the carrier during final macro selection.
  void StampReusedMacroPatchAcceptedCandidate(
      const MacroPatchReuseAdmissionContext &ctx, MacroPatch &patch) const;

  /// Literalize a direct __COUNTER__ invocation when the B-side replacement is
  /// recoverable. This keeps the counter-specific owner-state proof explicit
  /// instead of burying it in the main planner body.
  std::optional<MacroPatch> TryCounterLiteralWholeCoverPatch(
      const RefoldModel::MacroInvocation &invocation, const diffutils::Hunk &hunk,
      uint64_t invocationStart, uint64_t invocationEnd) const;

  /// Build the reuse-admission state for whole-cover planning from explicit
  /// caller-owned map/context inputs.  The returned carrier borrows the owner
  /// witness supplied by the caller and does not take ownership of patch-map
  /// storage.
  MacroPatchReuseAdmissionContext RecoverWholeCoverReuseContext(
      const RefoldModel::MacroInvocation &invocation,
      const Owner &currentPatchOwner, uint64_t invocationStart,
      uint64_t invocationEnd,
      const llvm::DenseMap<std::optional<uint64_t>,
                           llvm::DenseMap<uint64_t, MacroPatch>> &patchMap,
      ExistingMacroPatchContext existingContext) const;

  /// Recover a paired pure-insertion root args-only patch candidate.
  ///
  /// Recovers the root whole-cover args-only pure-insertion case. Two pure
  /// insertion frontiers may synthesize one argument envelope only when the
  /// trimmed envelope is fully contained in exactly one ordinary or stringify
  /// formal occurrence.
  std::optional<MacroPatch> TryPairedPureInsertionRootArgsOnlyPatch(
      const WholeCoverArgsOnlyCandidateContext &ctx) const;

  /// Build the direct root args-only candidate facts used before DAG lifting.
  ///
  /// The method preserves the old arbitration rule: a direct root args-only
  /// candidate is kept for final selection instead of returned immediately, so
  /// DAG-chained replay can still compete to preserve deeper structure.
  WholeCoverArgsOnlyCandidateResult BuildWholeCoverArgsOnlyCandidate(
      const WholeCoverArgsOnlyCandidateContext &ctx) const;

  /// Materialize a whole-cover realization candidate and attach its proof
  /// carrier.  The input plan has already been computed by the whole-cover
  /// proof path; this method only centralizes construction/stamping of the
  /// emitted realization patch.
  MacroPatch MaterializeWholeCoverPatch(
      const WholeCoverCandidate &candidate) const;

  /// Admit and select the final whole-cover-family candidate without changing
  /// discovery order, selector ranking, proof kind, or accepted-result stamping.
  std::optional<MacroPatch>
  SelectWholeCoverPatch(const WholeCoverFinalSelectionContext &ctx) const;

  /// Return whether an existing patch covers the same physical invocation span
  /// and belongs to the same macro patch owner as the current reuse request.
  bool ExistingPatchMatchesReuseSite(
      const MacroPatchReuseAdmissionContext &ctx,
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
                           llvm::DenseMap<uint64_t, MacroPatch>>
          &patchMap) const;

  /// Admit the caller-coalesced same-pass patch context when the patch map did
  /// not already provide a same-span reuse candidate.
  void AdmitCallerExistingMacroPatchContext(
      MacroPatchReuseAdmissionContext &ctx,
      ExistingMacroPatchContext existingContext) const;

  /// Return whether the hunk intersects any token-paste provenance span for
  /// the invocation, forcing paste-aware argument derivation.
  static bool HunkTouchesAnyPasteToken(const RefoldModel::MacroInvocation &m,
                                       const diffutils::Hunk &h);

  /// Derive the single argument rewrite implied by a hunk touching one pasted
  /// token, or fail closed when the pasted segment cannot be isolated.
  std::optional<PasteArgEdit>
  DerivePasteArgEdit(const RefoldModel::MacroInvocation &m,
                     const diffutils::Hunk &h) const;

  /// Derive all argument rewrites required by paste-token edits in the hunk,
  /// preserving the unique-segment proof requirement for each paste site.
  std::optional<std::vector<PasteArgEdit>>
  DerivePasteArgEdits(const RefoldModel::MacroInvocation &m,
                      const diffutils::Hunk &h) const;

  /// Split a pasted A token and its B replacement into per-argument segments
  /// when the fixed non-argument slices give a deterministic segmentation.
  static std::optional<std::vector<std::string>>
  SegmentPastedTokenArgsByFixedSlices(
      llvm::StringRef aTok, llvm::StringRef bTok,
      llvm::ArrayRef<const RefoldModel::PPArgSpan *> spansAsc);

  /// Return the replacement spelling segment corresponding to an old pasted
  /// argument segment inside a rewritten argument spelling.
  static llvm::StringRef DeriveNewPasteSegmentFromSpellingReplacement(
      llvm::StringRef baseArg, llvm::StringRef newArg,
      llvm::StringRef oldSeg);

  /// Verify that the derived argument replacements reproduce every touched
  /// pasted token on the B side before accepting an args-only patch.
  bool PasteArgReplacementsMatchAllPasteTokensInB(
      const RefoldModel::MacroInvocation &m, llvm::StringRef baseInvText,
      llvm::ArrayRef<std::pair<size_t, size_t>> invArgRanges,
      const llvm::DenseMap<uint32_t, std::string> &replByArgIdx) const;

  /// Stamp the macro patch with the B-token envelope for the invocation's
  /// whole-cover expansion, preserving boundary insertions for proof replay.
  bool StampMacroPatchWholeExpansionBRange(
      const RefoldModel::MacroInvocation &m, MacroPatch &patch) const;

  /// Attach the standard args-only macro proof carrier after the callsite
  /// rewrite has replayed against the required invocation envelope.
  void SetArgsOnlyStandardProof(
      MacroPatch &patch, const RefoldModel::MacroInvocation &m,
      bool wholeEnvelopeReplayValidated,
      bool definitionTapeReplayValidated = false) const;

  /// Attach the generated-callee replay proof carrier for a callsite whose
  /// apparent callee is produced by macro expansion or alias replay.
  void StampGeneratedCalleeReplayProof(
      MacroPatch &patch, const RefoldModel::MacroInvocation &m,
      uint64_t finalDirectiveId, uint32_t generatedCallDepth,
      uint32_t objectAliasHops, bool usesStringification, bool usesPaste,
      bool usesVariadicForwarding,
      bool decodedStringLiteralEvidenceOnly = false) const;

  /// Return the macro definition directive selected by the producer for this
  /// invocation, or nullptr when the directive identity is unavailable.
  const RefoldModel::MacroDirective *
  GetDefinitionDirectiveForInvocation(
      const RefoldModel::MacroInvocation &m) const;

  /// Return whether the A token at the given index has exactly the expected
  /// spelling.
  bool MatchLiteralAToken(uint64_t tok, llvm::StringRef spelling) const;

  /// Return whether the current-level invocation belongs to the expansion
  /// subtree rooted at the supplied macro invocation id.
  bool CurrentLevelInvocationIsInSubtreeOf(
      const RefoldModel::MacroInvocation &macro, uint64_t rootId) const;

  /// Return whether a current-level descendant of the root expansion observes
  /// __COUNTER__, requiring counter-stabilization proof before replay.
  bool CurrentLevelSubtreeContainsCounterInvocation(uint64_t rootId) const;

  /// Return the strict descendant distance from `candidate` to the validated
  /// subtree root by following producer-recorded caller macro ids.  The root
  /// itself is not a descendant, and broken ancestry fails closed.
  std::optional<unsigned> CandidateDepthInValidatedSubtree(
      const MacroSubtreeReplayValidationContext &ctx,
      const RefoldModel::MacroInvocation &candidate) const;

  /// Return whether `candidate` is a strict descendant of the validated subtree
  /// root by following producer-recorded caller macro ids.  Broken ancestry
  /// fails closed.
  bool CandidateBelongsToValidatedSubtree(
      const MacroSubtreeReplayValidationContext &ctx,
      const RefoldModel::MacroInvocation &candidate) const;

  /// Return whether every callee edge from the candidate back to the validated
  /// root is either literal or discharged by a whole-formal caller-forwarding
  /// proof. Unsupported generated-callee ancestry remains inadmissible.
  bool SubtreePathHasProvableCalleeClosure(
      const MacroSubtreeReplayValidationContext &ctx,
      const RefoldModel::MacroInvocation &candidate) const;

  /// Return whether a DAG subtree-root replay avoids contradicting sibling
  /// direct stringification/paste observer surfaces in the same root expansion.
  bool SubtreeReplayDoesNotContradictSiblingSurface(
      const MacroSubtreeReplayValidationContext &ctx,
      const MacroPatch &patch) const;

  /// Return whether a whole-envelope claim backed by the validated subtree is
  /// safe to participate in final selection without being treated as an
  /// owner-wide replay proof.
  bool ClaimedWholeEnvelopeIsReplaySafe(
      const MacroSubtreeReplayValidationContext &ctx,
      const MacroPatch &patch) const;

  /// Return whether a preserving callsite replay would observe an active
  /// header-owned macro-state directive that whole-cover/materialized emission
  /// must carry instead.
  bool CallsiteReplayObservesActiveHeaderMacroState(
      const RefoldModel::MacroInvocation &invocation,
      const MacroPatch &patch) const;

  /// Return whether an args-only whole-envelope claim proves that every fixed
  /// replacement-list body surface still replays literally in B.
  bool ArgsOnlyWholeEnvelopeCandidateHasLiteralBodyReplay(
      const RefoldModel::MacroInvocation &invocation,
      const MacroPatch &patch) const;

  /// Return whether a root-preserving candidate leaves fixed root-body tokens
  /// literal after excluding argument-dependent and descendant macro surfaces.
  bool RootPreservingCandidateHasLiteralFixedRootBodyReplay(
      const RefoldModel::MacroInvocation &invocation,
      llvm::StringRef invocationText, const MacroPatch &patch) const;

  /// Apply all final replay-stability gates used before whole-cover-family
  /// candidate admission and proof-lattice selection.
  bool MacroCandidateReplayIsStableForFinalSelection(
      const MacroSubtreeReplayValidationContext &ctx,
      const MacroPatch &patch) const;

  /// Resolve a function-like macro name through object-like single-token alias
  /// hops, reporting the number of hops consumed by the replay proof.
  const RefoldModel::MacroDirective *
  ResolveFunctionLikeMacroThroughAliasesWithHops(
      llvm::StringRef startName, uint32_t *aliasHops) const;

  /// Resolve a function-like macro name through replay-safe alias hops when the
  /// hop count is not needed by the caller.
  const RefoldModel::MacroDirective *
  ResolveFunctionLikeMacroForReplay(llvm::StringRef startName) const;

  /// Return whether the named object-like macro expands to a single token that
  /// can be used as a callee alias during replay proof.
  bool IsObjectLikeSingleTokenAlias(llvm::StringRef name) const;

  /// Compare expected token spellings against an A-token half-open range.
  bool TokenSpellingsEqualToA(llvm::ArrayRef<std::string> expected,
                              uint64_t beginTok, uint64_t endTok) const;

  /// Compare expected token spellings against a B-token half-open range.
  bool TokenSpellingsEqualToB(llvm::ArrayRef<std::string> expected,
                              uint64_t beginTok, uint64_t endTok) const;

  /// Return whether the active hunk overlaps the supplied A-byte span.
  bool HunkTouchesASpan(uint64_t begin, uint64_t end) const;

  /// Return whether the replacement spelling observes directive-local state
  /// such as formal names, stringification, paste, or variadic forwarding.
  bool ReplacementObservesDirective(
      const RefoldModel::MacroDirective &directive, llvm::StringRef macroName,
      llvm::StringRef replacement) const;

  /// Return whether the invocation directly stringifies the selected formal
  /// argument, requiring string-literal-aware replay evidence.
  bool DirectlyStringifiesFormal(const RefoldModel::MacroInvocation &inv,
                                 uint32_t argIdx) const;

  /// Return whether an argument spelling is a balanced parenthesized tuple at
  /// top level.
  bool IsParenthesizedTuple(llvm::StringRef arg) const;

  /// Verify that a structure-preserving callsite patch keeps the formal syntax
  /// stable enough for the macro replay proof to remain valid.
  bool StructurePreservingCallsiteHasStableFormalSyntax(
      const MacroPatch &patch, const RefoldModel::MacroInvocation &m) const;

  Dependencies deps_;
};

} // namespace refold
} // namespace clang

#endif // LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDMACROPATCHPLANNER_H
