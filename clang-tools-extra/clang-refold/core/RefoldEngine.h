//===--- RefoldEngine.h -----------------------------------------*- C++ -*-===//
//
// This component implements the deterministic “refolding” engine that projects
// edits made to a raw preprocessed stream (B) back onto the original, partially
// expanded translation unit (TU) described by the refold map.
//
// Overview
// --------
// RefoldEngine consumes:
//   • A: original preprocessed bytes and tokens
//   • B: edited preprocessed bytes and tokens
//   • M: RefoldModel (parsed from the JSON refold map)
//
// It aligns A↔B token streams, derives edit hunks, classifies each hunk as
// TU-owned / include-owned / macro-invocation–owned, and materializes a new
// TU that incorporates edits while preserving original structure and semantics.
//
// Strict-Domain Theorem Contract
// ------------------------------
// clang-refold's theorem-facing contract is intentionally narrower than “all
// possible C preprocessor edits.”  It is complete for finite deterministic
// owner-closed edit tilings whose state-transition summaries compose and whose
// preserved suffix observers see preprocessing state equivalent to the edited
// preprocessed stream B.
//
// In that domain, the engine must emit source S' such that preprocessing S'
// produces exactly B.  It enforces the contract by requiring every emitted
// accepted edit to normalize to one final TheoremProofClass and every rejected
// or raw-B result to carry a declared TerminalFallbackProofFailure.
//
// A hunk is in-domain only when its proof discharges all of the following:
//   • the modified preprocessed stream can be partitioned into a finite,
//     deterministic sequence of source-repair tiles;
//   • every tile has at least one producer-proven source witness;
//   • each witness is owner-closed and consumes a closed source interval plus
//     exactly the A-token envelope it produced;
//   • each owner emits, preserves, or realizes exactly the B-token envelope
//     assigned to it;
//   • all macro producer semantics used by the witness are modeled explicitly
//     (forwarding, stringification, paste, variadic comma behavior, directive
//     materialization, builtin/counter materialization, etc.);
//   • suffix macro/include/conditional/line/counter state is known or proven
//     equivalent for all preserved observers;
//   • neighboring tiles compose without crossing unowned directive, include,
//     macro, or source boundaries; and
//   • no upstream source-state mutation is reverse-solved from a downstream
//     expansion unless the directive itself lies inside the proven edited
//     source interval.
//
// In-domain does not mean unique inverse recovery. Multiple source spellings
// may be valid when they occupy one observational equivalence class; multiple
// non-equivalent classes fail closed instead of guessing author intent.
//
// Inputs outside this contract are not completeness failures.  They must be
// represented by an explicit failed proof obligation, materialized as a closed
// owner realization when possible, or rejected/fallen back in a way that
// preserves token soundness rather than emitting a speculative partial
// refolding.  The terminology in comments, theorem-audit logs, proof enums,
// and tests is expected to match this contract.
//
// Responsibilities
// ----------------
//   • Compute LCS-based A→B anchors and contiguous edit hunks.
//   • Attribute hunks to includes or macro call sites using M’s coverage data.
//   • Normalize and coalesce include insertions (line-local, boundary safe).
//   • Realize include expansions bottom-up, applying macro patches in-owner.
//   • Apply TU-level replacements with boundary hygiene (no token gluing).
//
// Determinism & Policy
// --------------------
//   • All iteration and sorting are stable; edits apply high→low to avoid
//     byte-offset drift.
//   • Boundary padding inserts at most one space locally when needed by
//     maximal-munch rules; internal whitespace is preserved verbatim.
//   • Errors are reported via the Logging subsystem (`fatal()/error()/...`).
//
// Public Surface
// --------------
//   • Refold(...): constructs a run-scoped engine and returns the refolded TU
//     text.
//   • Refold(): runs the single engine instance.
//   • The remaining declarations are orchestration seams between the engine and
//     proof/planning services; carrier definitions live in focused subsystem
//     headers rather than in RefoldEngine.h.
//
// Notes
// -----
//   • No RTTI or exceptions required; mirrors LLVM/Clang style.
//   • Paths are compared via the RefoldPathIdentity canonicalization service.
//   • All indices are half-open where applicable: tokens [lo,hi), bytes [b,e).
//
// Author:
//   jeikenberry
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDENGINE_H
#define LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDENGINE_H

#include "core/RefoldFinalAssemblyVerifier.h"
#include "core/RefoldModel.h"
#include "edit/RefoldEditTypes.h"
#include "edit/RefoldPatchTypes.h"
#include "include/RefoldSourceGraphProof.h"
#include "line-control/FinalLineControlModel.h"
#include "line-control/LineDirectiveInserter.h"
#include "line-control/RefoldLineControlProof.h"
#include "macro/RefoldArgTextRecovery.h"
#include "macro/RefoldMacroReplay.h"
#include "macro/RefoldMacroTopology.h"
#include "proof/RefoldAcceptedResultTypes.h"
#include "proof/RefoldOwnerStateTypes.h"
#include "proof/RefoldProofVocabulary.h"
#include "proof/RefoldSidebandReplayProof.h"
#include "proof/RefoldTheoremAudit.h"
#include "source/DiffAlgorithms.h"
#include "source/RefoldAlignmentSemanticResolver.h"
#include "source/RefoldSourceMapper.h"
#include "source/RefoldTokenTextAnalysis.h"
#include "util/RefoldDenseMapInfo.h"
#include "util/RefoldPathIdentity.h"

#include "clang/Basic/LangOptions.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/JSON.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace llvm {
class MemoryBuffer;
}

using namespace llvm;

namespace clang {
namespace refold {

class RefoldBInsertionLedger;
class RefoldCounterStabilization;
class RefoldExpansionFallbackPlanner;
class RefoldIncludeInsertionPlanner;
class RefoldIncludeMaterializer;
class RefoldPragmaOnceGuardRewriter;
class RefoldLineObserverLayout;
class RefoldMixedOwnerTilingPlanner;
class RefoldMacroPatchPlanner;
class RefoldMacroStateRepairPlanner;
class RefoldMacroStateProof;
class RefoldOwnerClassifier;
class RefoldOwnerStateProof;
class RefoldPreprocessingStructureIndex;
class RefoldPreprocessingStructureIndexProvider;
class RefoldProofLattice;
class RefoldStructuralHunkDispatcher;
class RefoldTextEditAssembler;
class RefoldTokenDiffPlanner;
class RefoldTUAnchorProof;
class RefoldTUEditPlanner;

/// \brief Deterministic refolder that projects edits made to raw preprocessed
/// C back onto the original translation unit (TU) without re-running the
/// preprocessor.
///
/// Inputs:
///   - A: original raw preprocessed text (e.g., clang -E -P output).
///   - B: edited raw preprocessed text.
///   - M: refold map JSON produced by a modified Clang preprocessor
///     (--refold-map=...). The map records provenance for includes,
///     conditionals, and macro expansions in PP-token coordinates, plus TU
///     file/byte ranges for patch sites.
///
/// Goal:
///
/// Reconstruct a TU that preserves the original preprocessor structure as much
/// as possible while still realizing the user's edits. Only constructs whose
/// expanded bytes were edited in B are forced to materialize in the output; all
/// others remain in their original form.
///
/// Policy and determinism:
///   - Token alignment uses an owner-aware LCS. Each PP-token gap in A is
///     assigned an ownerDepthGap derived from include depth and conditional
///     depth; LCS tie-breaks prefer edits to land at shallower ownership
///     boundaries (e.g., TU over deep includes).
///   - All selection among multiple valid owners is stable: smallest-covering
///     constructs win, ties break by id, and text edits apply right-to-left to
///     avoid offset drift.
///   - The engine fails fast when provenance is insufficient; it does not
///     guess.
///
/// Macro handling:
///
/// Macro edits are expressed as call-site patches:
///   - Whole-cover patches: replace an invocation span with edited expansion
///     bytes.
///   - Argument patches: when an edit is confined to invocation arguments,
///     rewrite the invocation text in-place (keeping the macro call in the
///     output).
///
/// Some expansions (notably builtin/object-like macros such as __FILE__ or
/// __LINE__) may appear inside another macro's replacement list and lack an
/// invocation-site span in M. Such edits are attributed to the nearest
/// enclosing patchable macro invocation (one with inv_b/inv_e/inv_text) so the
/// outer call site becomes the unit of change.
///
/// Stringification (#param):
///
/// Stringified arguments expand to string literal tokens in B and often do not
/// produce arg_spans in M. The engine recovers a mapping by parsing the active
/// #define preceding the invocation and locating #param occurrences in the
/// replacement list. If a string-literal edit can be safely inverted to a
/// single invocation argument (no commas/newlines; simple escape handling),
/// the invocation is rewritten; otherwise the invocation is expanded via a
/// whole-cover patch.
///
/// High-level pipeline:
///  1. Load M and the TU source.
///  2. Lex A and B into comparable PP-token sequences and build token-to-byte
///     offset tables.
///  3. Compute ownerDepthGap and an A->B token mapping via weighted LCS.
///  4. Derive edit hunks from the A->B mapping.
///  5. Classify each hunk deterministically as TU-owned, include-owned, or
///     macro-owned and build per-owner patch plans (including nested macro
///     patch baking).
///  6. Realize includes bottom-up along the recorded conditional arms and
///     apply patches to produce the final refolded TU.
///
/// Unsupported / out of scope:
///
/// The engine intentionally refuses macro-preserving rewrites unless the
/// producer map supplies enough local proof for the relevant feature (including
/// paste, stringify, variadic forwarding, and wrapper chains). When such proof
/// is unavailable, it prefers deterministic expansion of the affected macro
/// instance over speculative rewriting.
///
/// \author jeikenberry
class RefoldEngine {
public:
  /// \brief Perform the end-to-end refolding process for a translation unit.
  ///
  /// This method takes the original preprocessed text *A* (e.g. `test.c.i`),
  /// the edited preprocessed text *B* (e.g. `test.c.i.mod`), and the refold
  /// map JSON produced by the modified Clang preprocessor. It re-lexes the
  /// edited text, aligns A↔B token streams, projects edits back through the
  /// preprocessor structure (macros, includes, and conditional arms) using
  /// the refold map, and emits a partially expanded C source file that
  /// preserves the original semantics while incorporating edits
  /// deterministically.
  ///
  /// #### Determinism
  /// * All decisions are derived from explicit byte/token spans in the refold
  ///   map; no heuristics cross physical newlines.
  /// * Ambiguities are treated as hard errors with precise diagnostics.
  ///
  /// #### Behavioral Guarantees
  /// * Macro edits are applied at the *invocation site*, never at the
  ///   definition.
  /// * `#include` / `#include_next` edits are realized *in place* within the
  ///   included file and recursively folded back.
  /// * Conditional blocks are re-emitted with the correct selected arm, and
  ///   edits land in the corresponding arm.
  /// * Token boundary hygiene prevents “glued” tokens; spaces are inserted only
  ///   when required by maximal-munch rules.
  ///
  /// \param root       Parsed refold map JSON (immutable model root).
  /// \param aSource    Original preprocessed text A (e.g., `test.c.i`).
  /// \param aToks      Tokens of A.
  /// \param aTokOff    Byte offsets for A tokens (size = |A| + 1).
  /// \param bSource    Edited preprocessed text B (e.g., `test.c.i.mod`).
  /// \param bToks      Tokens of B.
  /// \param bTokOff    Byte offsets for B tokens (size = |B| + 1).
  /// \param noLines    If true, then do not inject #line.
  /// \param strict     If true, then make stringified args significant.
  /// \param finalOutputPath
  ///                  Path of the source file that will be emitted to `--out`
  ///                  and later passed to `--check`. Include replay proofs use
  ///                  this path, not the producer TU path, as their direct
  ///                  quoted-lookup surface.
  /// \returns          The refolded, partially expanded C source.
  static Expected<std::string> Refold(
      const json::Object &rootJson, StringRef aSource, ArrayRef<PPTok> aToks,
      ArrayRef<size_t> aTokOff, StringRef bSource, ArrayRef<PPTok> bToks,
      ArrayRef<size_t> bTokOff, bool noLines, bool strict,
      ProofAuditMode proofAuditMode = ProofAuditMode::Default,
      StringRef finalOutputPath = StringRef(),
      ArrayRef<SidebandPragmaEdit> sidebandPragmaEdits = {},
      std::vector<MaterializedEditMapping> *materializedEditMappings = nullptr,
      FinalLineControlValidationCallback finalLineControlValidationCallback =
          FinalLineControlValidationCallback(),
      std::vector<SourceGraphOutput> *sourceGraphOutputs = nullptr,
      bool auditRepair = false);

private:
  const RefoldModel model_;
  StringRef aSource_, bSource_;
  ArrayRef<PPTok> aToks_, bToks_;
  ArrayRef<size_t> aTokOff_, bTokOff_;
  bool noLines_ = false;
  std::string finalOutputPath_;
  LineDirectiveInserter lineDirs_;

  /// Path identity service shared by the engine and proof/planning services.
  ///
  /// It owns canonical-path caching and include physical/spelling identity
  /// predicates so later services can depend on path proof without borrowing
  /// arbitrary RefoldEngine internals.
  RefoldPathIdentity pathIdentity_;

  bool strict_;
  ProofAuditMode proofAuditMode_;
  LangOptions lexLang_;

  /// Exact physical bytes of the producer-recorded translation unit.
  ///
  /// Direct TU byte-span planning must validate every mapped token interval and
  /// every internal source gap against the original physical file rather than
  /// against preprocessed stream A.  The bytes are loaded once and retained for
  /// the lifetime of all planning/proof services.
  std::string tuSourceBytes_;

  /// Deferred physical-source load failure, if the initial snapshot was
  /// unavailable.
  ///
  /// Service construction retains an incomplete fail-closed index so an
  /// already-detected A/model inconsistency can still take the existing raw-B
  /// terminal path without requiring the TU file.  Normal structural refolding
  /// reports this error when it first requires the physical source buffer.
  std::optional<std::string> tuSourceLoadError_;

  /// Exact lexical inventory of preprocessing structure in `tuSourceBytes_`.
  ///
  /// This index is built before TU edit planning and is shared by every direct
  /// TU byte-span query.  Protection-census incompleteness rejects every direct
  /// span; ordinary producer-binding diagnostics remain local because their
  /// lexical intervals still reject any overlapping realization.
  std::unique_ptr<RefoldPreprocessingStructureIndex>
      preprocessingStructureIndex_;

  /// Shared exact structure-index provider for physical source occurrences.
  ///
  /// The provider returns `preprocessingStructureIndex_` for the TU and lazily
  /// builds header indexes per canonical path and concrete include occurrence.
  std::unique_ptr<RefoldPreprocessingStructureIndexProvider>
      preprocessingStructureIndexProvider_;

  /// Macro-argument text recovery service.
  ///
  /// This owns inverse stringification and fallback invocation-actual lexing.
  /// Keeping it as a focused service prevents proof/planning code from calling
  /// back into RefoldEngine for purely lexical text recovery.
  RefoldArgTextRecovery argTextRecovery_;

  /// Lexical text-analysis service for macro-state and builtin-observer
  /// predicates.  It owns raw-lexer based identifier/function-invocation scans
  /// so proof services can reason about arbitrary replacement text without
  /// calling back into RefoldEngine.
  RefoldTokenTextAnalysis tokenTextAnalysis_;

  /// Terminal fallback request sink for the current refold attempt.
  ///
  /// The sink owns the mutable ordered request ledger and the centralized
  /// classification/normalization gate for terminal raw-B fallback.  Subsystem
  /// services receive this sink directly instead of calling back into
  /// RefoldEngine just to record a failed terminal proof obligation.
  RefoldTerminalProofSink terminalSink_;

  std::optional<FinalReplaySurface> finalReplaySurface_;
  std::vector<MaterializedEditMapping> *materializedEditMappings_ = nullptr;
  std::vector<SourceGraphOutput> *sourceGraphOutputs_ = nullptr;
  FinalLineControlValidationCallback finalLineControlValidationCallback_;

  /// Closing assembly check for this run.  Absent when the edited stream could
  /// not be preprocessed, and for candidate simulations, which are not judged.
  std::optional<RefoldFinalAssemblyVerifier> finalAssemblyVerifier_;

  /// Root macro invocations whose callsite must not be preserved.
  ///
  /// The closing assembly check names the owner of a divergence; adding it here
  /// makes the next attempt refuse every candidate that would keep that
  /// callsite, so its expansion realization is taken instead.  The set only
  /// grows within a run, which is what makes the retry loop terminate: each
  /// round either verifies or removes one more owner from contention, and the
  /// ladder ends at the whole translation unit.
  llvm::DenseSet<uint64_t> ownersMustExpand_;

  /// Final-output byte ranges for synthetic `#line` directives that the local
  /// emitters have explicitly made eligible for the final fixed-point pruner.
  /// The vector is cleared at the start of each top-level refold attempt and is
  /// passed to the final pruner only after structural emission has selected the
  /// actual output stream.
  mutable std::vector<FinalLineControlPruneCandidate>
      finalLineControlPruneCandidates_;

  /// Final-output byte ranges that were copied byte-for-byte from a physical
  /// source owner.  The final line-control scanner uses these mappings to bind
  /// final `#line` directive bytes back to producer-recorded LineControlEvent
  /// records without guessing through materialized B replay or synthetic text.
  mutable std::vector<FinalLineControlSourceMapping>
      finalLineControlSourceMappings_;

  /// Source edits for sideband pragma directive lines that were removed from
  /// the lexed A/B token streams before diffing. These are applied as ordinary
  /// TU byte edits only when their source path is the emitted TU; non-TU
  /// occurrences are rejected by the structural pass because this single-file
  /// output cannot directly edit an arbitrary header.
  std::vector<SidebandPragmaEdit> sidebandPragmaEdits_;

  RefoldStats lastStats_;
  mutable TheoremAuditStats lastTheoremAudit_;
  std::unique_ptr<RefoldTheoremAudit> theoremAudit_;

  /// Per-gap ownership depth for insertion before PP token k (k in [0..N]).
  /// Computed once per refold run and reused to bound best-effort snapping.
  std::vector<uint32_t> ownerDepthGap_;

  /// Theorem authority parallel to the selected production A-to-B token map.
  std::vector<diffutils::LcsAnchorProof> abTokAnchorProofs_;

  /// Durable semantic-equivalence witnesses emitted by the alignment resolver.
  std::vector<AlignmentSemanticResolutionWitness>
      alignmentSemanticResolutionWitnesses_;

  /// Internal exact alignment override used by an isolated semantic simulation.
  std::optional<AlignmentSelectionOverride> alignmentSelectionOverride_;

  /// False only for isolated simulations, preventing recursive resolution.
  bool alignmentSemanticResolverEnabled_ = true;

  /// Dynamic theorem boundary for semantic alignment. Isolated candidates set
  /// this from their alignment override; the production engine sets it only
  /// after one complete semantic equivalence class is committed. Proof/audit
  /// services borrow this flag so candidate and production realization use the
  /// same strict witness domain without changing unrelated planner policy.
  bool alignmentSemanticTheoremActive_ = false;

  /// Exact outer-run TU byte snapshot supplied to isolated simulations.
  /// Production engines leave this empty and load the physical source once;
  /// simulations reuse that immutable snapshot instead of rereading the file.
  std::optional<llvm::StringRef> tuSourceBytesOverride_;

  /// Canonical staged source-edit topology captured after final-emission
  /// lowering and repair have completed.
  std::string alignmentSimulationStagedTopologyKey_;
  bool alignmentSimulationStagedTopologyComplete_ = true;
  std::string alignmentSimulationStagedTopologyFailure_;
  AlignmentSemanticPreservationFootprint
      alignmentSimulationPreservationFootprint_;

  /// Ordered planning phase for the structural token-hunk pipeline.
  ///
  /// Structural tiling must finish before insertion provenance or owner
  /// dispatch observes the hunk sequence.  Keeping this phase explicit turns
  /// that ordering from a call-site convention into a checked engine
  /// invariant: no downstream service may accidentally consume the initial,
  /// pre-tiling token diff.
  enum class StructuralHunkPlanningPhase : uint8_t {
    NotStarted,
    InitialTokenDiffBuilt,
    StructuralTilingComplete,
    InsertionLedgerReady,
  };

  StructuralHunkPlanningPhase structuralHunkPlanningPhase_ =
      StructuralHunkPlanningPhase::NotStarted;

  /// Cached token-level hunks for the current refold invocation.
  ///
  /// After `PlanTokenDiff()` returns, this cache is required to equal the
  /// normalized hunk vector returned to structural dispatch.  These hunks are
  /// used to deterministically disambiguate A→B token envelope
  /// mapping at *span boundaries* when there are adjacent pure-insertion hunks
  /// (A-span is empty) that should not be absorbed into a larger mapped
  /// envelope (e.g., macro whole-cover replacement).
  std::vector<diffutils::Hunk> abTokHunks_;

  /// Persisted structural-hunk tiling witnesses for the current run.
  ///
  /// The structural normalizer still lowers a proved tiling to ordinary token
  /// hunks so existing macro/include/TU classifiers can operate unchanged.
  /// These side tables preserve the theorem proof that created those hunks and
  /// map each emitted token segment back to the full ordered tiling path.
  std::vector<MixedOwnerTilingWitness> mixedOwnerTilingWitnesses_;
  std::vector<MixedOwnerTilingSegmentBinding> mixedOwnerTilingSegmentBindings_;

  /// Cached token-level A/B maps for the current refold invocation.
  ///
  /// Final TU emission uses these as proof inputs when it has to decide whether
  /// an otherwise uncomposable cluster of direct TU hunk edits can be replaced
  /// by one closed source/B-token realization. The closure proof requires that
  /// every matched A token physically consumed by the source interval maps into
  /// the candidate B interval, and every matched B token emitted by that
  /// replacement maps back into the same source interval.
  std::vector<int64_t> abTokMapA2B_;
  std::vector<int64_t> abTokMapB2A_;

  std::optional<std::vector<diffutils::Hunk>> abByteHunks_;

  /// \brief Prefix-summed A->B byte-length delta for \c abByteHunks_.
  ///
  /// Entry \c i stores the cumulative `(bLen - aLen)` contributed by the first
  /// \c i byte hunks. This keeps repeated A-byte→B-byte coordinate projection
  /// at `O(log H)` after the initial binary search instead of re-walking all
  /// preceding hunks on every lookup.
  std::vector<int64_t> abByteHunkPrefixDelta_;

  /// Source/byte/token coordinate mapper over this refold run's A/B streams.
  ///
  /// The mapper is constructed from explicit source/token inputs plus mutable
  /// hunk caches, and it has no access to include, macro, line-control, or
  /// proof orchestration state.  This keeps coordinate projection as an
  /// explicit service instead of another RefoldEngine responsibility.
  RefoldSourceMapper sourceMapper_;

  /// Macro graph/topology service over producer-recorded invocations.
  ///
  /// The service owns macro ID lookup, caller/child traversal, #define
  /// containment indexing, conditional-arm containment checks, and __COUNTER__
  /// event identity construction.  Friend services receive this narrow oracle
  /// instead of calling macro-topology helpers through RefoldEngine.
  RefoldMacroTopology macroTopology_;

  /// Initial A/B token-diff planning service owned by the engine graph.
  ///
  /// The planner owns lexeme mapping, LCS provenance/profile construction,
  /// initial token-hunk derivation, token-map cache population, and raw
  /// byte-hunk cache construction. Mixed-owner partitioning is handled by
  /// RefoldMixedOwnerTilingPlanner after the initial token-diff plan is built.
  std::unique_ptr<RefoldTokenDiffPlanner> tokenDiffPlanner_;

  /// Macro-boundary selector for narrow A/B token insertion-at-cover-edge cases
  /// that may be macro-owned after later whole-cover proof.  Keeping this
  /// policy in a macro-domain service avoids retaining boundary replay helpers
  /// on RefoldEngine.
  RefoldMacroBoundarySelector macroBoundarySelector_;

  /// Owner/TU classification service shared by macro, counter, and proof
  /// consumers.
  ///
  /// The classifier owns ownership decisions and uses RefoldTUEditPlanner as a
  /// named dependency for TU anchor/span queries.
  std::unique_ptr<RefoldOwnerClassifier> ownerClassifier_;

  /// B-insertion provenance and claim ledger for this refold run.
  ///
  /// The ledger owns pure B-token insertion provenance, standalone preclaims,
  /// and claim-aware B slicing.  Macro and edit services consume this named
  /// service instead of borrowing raw insertion state from RefoldEngine.
  std::unique_ptr<RefoldBInsertionLedger> bInsertionLedger_;

  /// Translation-unit edit planning service.
  ///
  /// This owns TU insertion-anchor, TU byte-span, direct TU edit-plan, and
  /// trailing-call-suffix extension policy.  Final TextEdit assembly and legacy
  /// orchestration wrappers remain outside this service.
  std::unique_ptr<RefoldTUEditPlanner> tuEditPlanner_;

  /// TU-anchor accepted-result proof builder.
  ///
  /// This service certifies proven TU insertion-anchor witnesses into
  /// normalized accepted-result carriers.  It is separate from
  /// RefoldProofLattice so the TU edit planner can build anchor candidates
  /// without depending on the lattice service that will later query TU planning
  /// diagnostics.
  std::unique_ptr<RefoldTUAnchorProof> tuAnchorProof_;

  /// Counter-stabilization planner over producer macro topology.
  ///
  /// This service owns the policy that forces a suffix of counter-bearing macro
  /// callsites to remain expanded after an edited or already-expanded
  /// `__COUNTER__` occurrence would otherwise shift replay-time counter
  /// sequencing.  It receives explicit topology and owner-classification
  /// services; selected-patch expandedness is now a macro-topology query rather
  /// than a counter-specific hook.
  std::unique_ptr<RefoldCounterStabilization> counterStabilization_;

  /// Line-control proof service over producer line-control metadata.
  ///
  /// The service owns read-only line-state observer and producer-backed #line
  /// proof queries.  Materialization and assembly services receive this narrow
  /// dependency instead of calling line-control helpers through RefoldEngine.
  RefoldLineControlProof lineControlProof_;

  /// Owner-state proof service owned by the engine.
  ///
  /// The service owns owner-state delta/graph construction, suffix-observer
  /// queries, transition-gateway checks, and the caches those proofs need. It
  /// receives only immutable inputs plus narrow audit/terminal sinks; it no
  /// longer borrows RefoldEngine or requires friend access.
  std::unique_ptr<RefoldOwnerStateProof> ownerStateProof_;

  /// Structural token-hunk tiling service owned by the engine graph.
  ///
  /// The planner owns deterministic normalization that may split a hunk across
  /// provable owner boundaries or, for delete-only hunks, around exact
  /// preprocessing structure preserved in place.  It refreshes the durable
  /// structural witness ledgers before any owner-sensitive service executes.
  std::unique_ptr<RefoldMixedOwnerTilingPlanner> mixedOwnerTilingPlanner_;

  /// Macro-state proof service owned by the engine.
  ///
  /// The service owns read-only #define/#undef observation and directive-line
  /// recovery proof.  It receives owner-state proof explicitly for the one
  /// materialized-header stabilization path that records macro-state movement
  /// witnesses, instead of reaching back through RefoldEngine.
  std::unique_ptr<RefoldMacroStateProof> macroStateProof_;

  /// Accepted-result proof lattice owned by the engine.
  ///
  /// The lattice owns theorem/proof-summary classification, witness resolver
  /// decisions, accepted-result ranking, and proof carrier construction.  It
  /// receives explicit services, ledgers, and narrow proof hooks instead of
  /// borrowing RefoldEngine or requiring friend access.
  std::unique_ptr<RefoldProofLattice> proofLattice_;

  /// Macro patch planner owned by the engine object graph.
  ///
  /// This service is the ownership boundary for args-only and whole-cover macro
  /// patch planning.  Macro-specific helper logic belongs inside the macro
  /// subsystem instead of returning macro policy to RefoldEngine.
  std::unique_ptr<RefoldMacroPatchPlanner> macroPatchPlanner_;

  /// Macro-state repair planner owned by the engine object graph.
  ///
  /// This service preserves, moves, or materializes macro-state transitions
  /// after TU edits consume #define/#undef source.  It keeps repair liveness
  /// policy out of RefoldEngine while still mutating the already-staged TU
  /// edits explicitly.
  std::unique_ptr<RefoldMacroStateRepairPlanner> macroStateRepairPlanner_;

  /// Include-insertion patch and include-realization A/B token envelope
  /// planner.
  ///
  /// This service owns the deterministic B-envelope proof and staging patch
  /// construction for include preservation and realization.  Include
  /// materialization and expansion fallback receive it directly as a named
  /// include-edit dependency.
  std::unique_ptr<RefoldIncludeInsertionPlanner> includeInsertionPlanner_;

  /// Final byte-edit assembler owned by the engine.
  ///
  /// The assembler is intentionally a separate object from RefoldEngine: it
  /// owns final splice/application behavior and edit-map certifying while
  /// receiving proof, state, line-control, and terminal dependencies
  /// explicitly.
  std::unique_ptr<RefoldTextEditAssembler> textEditAssembler_;

  /// Line-observer layout realization service owned by the engine.
  ///
  /// The service emits TU/header materialization edits and include `#line`
  /// wrappers needed by preserved line-state observers.  It depends on proof
  /// services and the final text-edit assembler, but it owns no orchestration
  /// state from RefoldEngine.  It is declared before the include materializer
  /// so the materializer's borrowed reference remains valid through
  /// destruction.
  std::unique_ptr<RefoldLineObserverLayout> lineObserverLayout_;

  /// Include-materialization planner/realizer owned by the engine.
  ///
  /// The materializer owns include subtree realization, header-local include
  /// edit planning, child boundary insertion anchoring, and the include-facing
  /// accepted-result carriers.  It receives explicit services, ledgers, and
  /// narrow orchestration hooks rather than borrowing RefoldEngine.
  std::unique_ptr<RefoldIncludeMaterializer> includeMaterializer_;

  /// Synthetic `#pragma once` guard catalog owned by the engine.
  ///
  /// The rewriter re-expresses a physical header's once-state as macro state
  /// when that header's text is inlined into the refolded TU, where the original
  /// pragma is inert.  It is mutable because the set of headers actually inlined
  /// is only known after include-materialization scheduling.
  std::unique_ptr<RefoldPragmaOnceGuardRewriter> pragmaOnceGuardRewriter_;

  /// Expansion-fallback planner owned by the engine.
  ///
  /// The planner owns the explicit TU include-closure and terminal
  /// out-of-domain fallback surfaces.  It receives immutable inputs, proof
  /// services, mutable ledgers, and narrow audit/orchestration hooks explicitly
  /// instead of borrowing RefoldEngine or requiring friend access.
  std::unique_ptr<RefoldExpansionFallbackPlanner> expansionFallbackPlanner_;

  /// Allocate and access the centralized theorem/audit service.
  void InitializeTheoremAudit();
  RefoldTheoremAudit &TheoremAudit() const;

  /// Allocate the owner-state proof service after the engine's immutable model
  /// and source/token inputs have been constructed.  This stays out-of-line so
  /// RefoldEngine.h does not need the service's full definition.
  void InitializeOwnerStateProof();

  /// Return the owned owner-state proof service.
  RefoldOwnerStateProof &OwnerStateProof();
  const RefoldOwnerStateProof &OwnerStateProof() const;

  /// Allocate the macro-state proof service after owner-state proof exists.
  void InitializeMacroStateProof();

  /// Return the owned macro-state proof service.
  RefoldMacroStateProof &MacroStateProof();
  const RefoldMacroStateProof &MacroStateProof() const;

  /// Allocate the proof lattice after all borrowed engine members have been
  /// constructed.  This stays out-of-line so RefoldEngine.h does not need to
  /// include the lattice's full definition.
  void InitializeProofLattice();

  /// Return the owned proof-lattice service.
  RefoldProofLattice &ProofLattice();
  const RefoldProofLattice &ProofLattice() const;

  /// Allocate and access the owner/TU classification service.
  ///
  /// The service is initialized after RefoldTUEditPlanner so owner
  /// classification can use the planner by constructor reference.
  void InitializeOwnerClassifier();
  RefoldOwnerClassifier &OwnerClassifier();
  const RefoldOwnerClassifier &OwnerClassifier() const;

  /// Allocate and access the B-insertion claim ledger.
  void InitializeBInsertionLedger();
  RefoldBInsertionLedger &BInsertionLedger();
  const RefoldBInsertionLedger &BInsertionLedger() const;

  /// Allocate and access the TU-anchor proof builder.
  void InitializeTUAnchorProof();
  RefoldTUAnchorProof &TUAnchorProof();
  const RefoldTUAnchorProof &TUAnchorProof() const;

  /// Allocate the initial A/B token-diff planning service.
  void InitializeTokenDiffPlanner();

  /// Allocate the generalized structural tiler that normalizes token hunks
  /// after the initial token-diff pass.
  void InitializeMixedOwnerTilingPlanner();

  /// Allocate and access the translation-unit edit planning service.
  ///
  /// The planner depends on RefoldTUAnchorProof for accepted TU-anchor carrier
  /// construction, not on RefoldProofLattice.  Final TextEdit assembly remains
  /// outside this service.
  void InitializeTUEditPlanner();
  RefoldTUEditPlanner &TUEditPlanner();
  const RefoldTUEditPlanner &TUEditPlanner() const;

  /// Load the physical TU bytes and build the exact preprocessing-structure
  /// census consumed by direct TU byte-span planning.
  void InitializePreprocessingStructureIndex();

  /// Allocate the shared occurrence-local preprocessing-structure provider.
  void InitializePreprocessingStructureIndexProvider();

  /// Allocate and access the macro patch planner after the proof lattice
  /// exists. Macro-planning orchestration calls this service directly.
  void InitializeMacroPatchPlanner();
  RefoldMacroPatchPlanner &MacroPatchPlanner();
  const RefoldMacroPatchPlanner &MacroPatchPlanner() const;

  /// Allocate and access the macro-state repair planner after macro patch
  /// planning and final text-edit assembly services exist.
  void InitializeMacroStateRepairPlanner();
  RefoldMacroStateRepairPlanner &MacroStateRepairPlanner();
  const RefoldMacroStateRepairPlanner &MacroStateRepairPlanner() const;

  /// Allocate and access the counter-stabilization planner.
  void InitializeCounterStabilization();
  RefoldCounterStabilization &CounterStabilization();
  const RefoldCounterStabilization &CounterStabilization() const;

  /// Allocate and access the include-insertion planner after the proof lattice
  /// exists.  This keeps include patch construction and include-realization
  /// B-envelope proof out of RefoldEngine while preserving construction order.
  void InitializeIncludeInsertionPlanner();
  RefoldIncludeInsertionPlanner &IncludeInsertionPlanner();
  const RefoldIncludeInsertionPlanner &IncludeInsertionPlanner() const;

  /// Allocate and access the line-observer layout realization service after the
  /// final text-edit assembler exists.  The layout service emits concrete
  /// observer-preserving materialization edits and include wrappers;
  /// RefoldEngine only coordinates when those edits are requested.
  void InitializeLineObserverLayout();
  RefoldLineObserverLayout &LineObserverLayout();
  const RefoldLineObserverLayout &LineObserverLayout() const;

  /// Allocate the include materializer after include insertion, line-observer
  /// layout, proof, and final text-edit services are available.  The
  /// materializer owns recursive include realization; RefoldEngine owns only
  /// construction order and final orchestration.
  /// Retract hunk edges out of macro expansions the hunk only partially owns.
  ///
  /// A realizer reconstructs source by re-emitting a callsite, so an expansion
  /// must be wholly inside a hunk or wholly outside it.  When an edge lands
  /// strictly inside one, the source projection widens to the complete
  /// invocation spelling while the replacement carries only the fragment of
  /// expanded tokens inside the hunk, and the difference is dropped silently:
  /// `return NULL;` became `return );` when an alignment boundary fell one
  /// token inside `NULL`'s `((void*)0)` expansion.
  ///
  /// The edge is walked outward one token at a time, and only across tokens
  /// that are identical on both sides -- restoring a match the certifier left
  /// unforced because a repeated spelling made it ambiguous.  This keeps the
  /// repair local: the expansion rejoins the untouched region beside the hunk
  /// and every other hunk in the translation unit is unaffected.  An edge that
  /// cannot be walked out is left alone for the ordinary realizer lattice
  /// rather than escalating the whole translation unit to raw B.
  void RetractHunkEdgesOutOfPartiallyOwnedMacroExpansions(
      std::vector<diffutils::Hunk> &hunks) const;

  void InitializeIncludeMaterializer();

  /// Allocate and access the synthetic `#pragma once` guard rewriter.
  void InitializePragmaOnceGuardRewriter();
  RefoldPragmaOnceGuardRewriter &PragmaOnceGuardRewriter();
  const RefoldPragmaOnceGuardRewriter &PragmaOnceGuardRewriter() const;

  /// Allocate the final text-edit assembler after all borrowed engine
  /// members have been constructed.  This stays out-of-line so RefoldEngine.h
  /// does not need to include the assembler's full definition.
  void InitializeTextEditAssembler();

  /// Allocate the expansion-fallback planner after the source mapper and
  /// downstream materialization/assembly services are available.
  void InitializeExpansionFallbackPlanner();

  /// Construct an engine from concrete inputs. The instance method `Refold()`
  /// runs the full pipeline using these captured members.
  ///
  /// This constructor is intentionally defined out-of-line: `RefoldEngine` owns
  /// `std::unique_ptr` subsystem services, and libc++ must see each service's
  /// complete type when generating constructor cleanup paths.
  RefoldEngine(
      RefoldModel model, StringRef aSource, ArrayRef<PPTok> aToks,
      ArrayRef<size_t> aTokOff, StringRef bSource, ArrayRef<PPTok> bToks,
      ArrayRef<size_t> bTokOff, bool noLines, bool strict,
      ProofAuditMode proofAuditMode, StringRef finalOutputPath,
      ArrayRef<SidebandPragmaEdit> sidebandPragmaEdits,
      std::vector<MaterializedEditMapping> *materializedEditMappings = nullptr,
      FinalLineControlValidationCallback finalLineControlValidationCallback =
          FinalLineControlValidationCallback(),
      std::vector<SourceGraphOutput> *sourceGraphOutputs = nullptr,
      std::optional<AlignmentSelectionOverride> alignmentSelectionOverride =
          std::nullopt,
      bool alignmentSemanticResolverEnabled = true,
      std::optional<llvm::StringRef> tuSourceBytesOverride = std::nullopt);

  ~RefoldEngine();

  /// \brief Run the full refolding pipeline for the current inputs.
  ///
  /// This is the main instance entry point. It classifies token hunks, plans
  /// TU/include/macro edits under the structural policy, materializes the
  /// refolded translation unit, and either returns that structural result or
  /// falls back to the explicit terminal edited-preprocessed-stream outcome.
  ///
  /// \returns The refolded C/C++ source text for the translation unit.
  std::string Refold();

  /// Validate that the producer-recorded A token count still matches the
  /// re-lexed A stream for the current refold attempt.
  ///
  /// A mismatch means the structural pass cannot discharge token provenance, so
  /// this stage records the existing terminal fallback request instead of
  /// continuing with stale token ownership facts.
  bool ValidateTokenCount();

  /// Load the physical translation-unit source bytes for structural refolding.
  ///
  /// The returned memory buffer must outlive all later pipeline stages because
  /// those stages borrow `StringRef` views into the TU bytes while planning and
  /// materializing edits.
  std::unique_ptr<llvm::MemoryBuffer> LoadTUSource(StringRef tuPath);

  /// Build and normalize the A/B token diff used by structural hunk dispatch.
  ///
  /// This stage refreshes the token-diff planner's run-local caches, applies
  /// generalized structural tiling, verifies that the normalized hunk cache and
  /// durable segment ledgers agree, and only then records pure-insertion
  /// provenance.  No macro/include/TU owner dispatch may consume the sequence
  /// before this method completes.
  /// Resolve non-forced core-optimal anchors through isolated full structural
  /// simulations and one exact semantic equivalence class.
  void ResolveSemanticAlignment(
      llvm::ArrayRef<llvm::StringRef> aLexemes,
      llvm::ArrayRef<llvm::StringRef> bLexemes,
      llvm::ArrayRef<diffutils::LcsAGapProvenance> aGapProvenance,
      llvm::ArrayRef<diffutils::LcsBGapProvenance> bGapProvenance,
      diffutils::CertifiedLcsResult &alignment);

  /// Run one candidate map through a fresh, non-recursive structural engine.
  AlignmentSemanticSimulationResult SimulateSemanticAlignmentCandidate(
      const AlignmentSelectionOverride &selection) const;


  /// Reject an isolated run whose durable structural witnesses are incomplete.
  bool AlignmentSimulationProofComplete(std::string &failure) const;

  std::vector<diffutils::Hunk> PlanTokenDiff(StringRef tuPath);

  /// Emit trace diagnostics for each B-token envelope selected by the token
  /// diff and owner-aware tiling stages.
  ///
  /// The diagnostics validate token-to-byte envelope shape and never become a
  /// proof source for accepting a hunk.
  void TraceStructuralHunkEnvelopes(ArrayRef<diffutils::Hunk> hunks);

  /// Stage source edits for sideband pragma lines before ordinary hunk dispatch.
  ///
  /// Sideband edits have already been stripped from the normal token streams,
  /// so staging them first lets later TU/include/macro realization avoid
  /// replaying the same B bytes through ordinary structural hunks.
  bool StageSidebandEdits(
      StringRef tuPath, StringRef tuBytes,
      RefoldStructuralHunkDispatcher &structuralHunkDispatcher);

  /// Classify each structural hunk and stage the selected TU/include/macro edit.
  ///
  /// This preserves the existing dispatch order: macro call-site proofs are
  /// tried first, include ownership is honored next, truthful TU byte-span edits
  /// follow, and unresolved ownership records the explicit terminal fallback
  /// obligation instead of manufacturing a weaker success class.
  bool DispatchStructuralHunks(
      StringRef tuPath, StringRef tuBytes, ArrayRef<diffutils::Hunk> hunks,
      RefoldStructuralHunkDispatcher &structuralHunkDispatcher);

  /// Complete macro-state repair, include materialization, and final TU emission
  /// after all structural hunks have been dispatched.
  ///
  /// This stage owns only final orchestration. The macro-state, include
  /// scheduler, and final-emission services continue to own their respective
  /// proof obligations and fail-closed mechanics.
  std::string FinalizeStructuralResult(
      StringRef tuPath, StringRef tuBytes, ArrayRef<diffutils::Hunk> hunks,
      RefoldStructuralHunkDispatcher &structuralHunkDispatcher);

  /// \brief Run the structural refold pass.
  ///
  /// The caller reacts to typed requests recorded in RefoldTerminalProofSink by
  /// selecting the explicit terminal fallback result.
  /// Prove that the assembled source re-expands every preserved `__LINE__`
  /// observer to the value the edited stream carries, and request terminal
  /// fallback when it does not.
  ///
  /// The observer is located by the preprocessor rather than by any byte
  /// mapping: the assembled source is preprocessed through the producer's
  /// recorded context and its token stream compared to B.  Only positions that
  /// B produced from a `__LINE__` expansion are enforced, so this closes the
  /// newline-drift ordering hole without becoming a general re-check.
  bool AuditPreservedLineObserversInFinalOutput(llvm::StringRef finalSource);

  /// Report whether the accepted assembly replays the edited stream.
  ///
  /// Observation only for now: the verdict is logged and nothing acts on it, so
  /// the check can be measured against the corpus before any refold depends on
  /// it.  Alignment candidate simulations re-enter the engine and are skipped,
  /// because judging an assembly that is about to be discarded says nothing
  /// about the result that is kept.


  /// Identify the smallest macro invocation that owns a diverging
  /// edited-stream token, so an unsound region can be narrowed instead of
  /// condemning the whole translation unit.
  ///
  /// The verifier reports its mismatch in *preprocessed* edited-stream token
  /// numbering, while the producer's maps are expressed in the edited stream as
  /// written.  Those coincide only when re-preprocessing the edited stream is a
  /// no-op, which is the ordinary case -- it is already `-E -P` output -- but is
  /// not guaranteed: a stream carrying comments loses them on replay.  The
  /// correspondence is therefore checked rather than assumed, and an
  /// unverifiable one yields no owner, leaving the caller to escalate.
  ///
  /// The *smallest* covering invocation is returned.  Expanding it costs the
  /// least source structure, and it is the first rung of a ladder: a caller
  /// that expands it and still finds the assembly unsound escalates outward to
  /// the enclosing invocation, and finally to the translation unit.
  std::optional<uint64_t>
  FindSmallestMacroOwnerForEditedToken(std::size_t editedTokenIndex) const;

  std::string RunRefoldPass();
};

} // namespace refold
} // namespace clang

#endif // LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDENGINE_H
