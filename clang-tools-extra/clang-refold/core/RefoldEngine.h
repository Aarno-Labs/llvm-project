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
//     extracted proof/planning services; carrier definitions live in focused
//     subsystem headers rather than in RefoldEngine.h.
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
#include "proof/RefoldProofTypes.h"
#include "proof/RefoldSidebandReplayProof.h"
#include "proof/RefoldTerminalProof.h"
#include "proof/RefoldTerminalProofSink.h"
#include "proof/RefoldTheoremAudit.h"
#include "proof/RefoldWitnessTypes.h"
#include "source/DiffAlgorithms.h"
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

using namespace llvm;

namespace clang {
namespace refold {

class RefoldBInsertionLedger;
class RefoldCounterStabilization;
class RefoldExpansionFallbackPlanner;
class RefoldIncludeInsertionPlanner;
class RefoldIncludeMaterializer;
class RefoldLineObserverLayout;
class RefoldMixedOwnerTilingPlanner;
class RefoldMacroPatchPlanner;
class RefoldMacroStateRepairPlanner;
class RefoldMacroStateProof;
class RefoldOwnerClassifier;
class RefoldOwnerStateProof;
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
  static Expected<std::string>
  Refold(const json::Object &rootJson, StringRef aSource, ArrayRef<PPTok> aToks,
         ArrayRef<size_t> aTokOff, StringRef bSource, ArrayRef<PPTok> bToks,
         ArrayRef<size_t> bTokOff, bool noLines, bool strict,
         ProofAuditMode proofAuditMode = ProofAuditMode::Default,
         StringRef finalOutputPath = StringRef(),
         ArrayRef<SidebandPragmaEdit> sidebandPragmaEdits = {},
         std::vector<MaterializedEditMapping> *materializedEditMappings =
             nullptr,
         FinalLineControlValidationCallback finalLineControlValidationCallback =
             FinalLineControlValidationCallback(),
         std::vector<SourceGraphOutput> *sourceGraphOutputs = nullptr);

  /// Build lexer language options from the producer-captured language name.
  static clang::LangOptions MakeLexLangOptions(llvm::StringRef langName);


private:
  const RefoldModel model_;
  StringRef aSource_, bSource_;
  ArrayRef<PPTok> aToks_, bToks_;
  ArrayRef<size_t> aTokOff_, bTokOff_;
  LineDirectiveInserter lineDirs_;

  /// Path identity service shared by the engine and extracted proof/planning
  /// services.  It owns canonical-path caching and include physical/spelling
  /// identity predicates so later services can depend on path proof without
  /// borrowing arbitrary RefoldEngine internals.
  RefoldPathIdentity pathIdentity_;

  bool strict_;
  ProofAuditMode proofAuditMode_;
  LangOptions lexLang_;

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
  /// classification/normalization gate for terminal raw-B fallback.  Extracted
  /// services receive this sink directly instead of calling back into
  /// RefoldEngine just to record a failed terminal proof obligation.
  RefoldTerminalProofSink terminalSink_;

  std::optional<FinalReplaySurface> finalReplaySurface_;
  std::vector<MaterializedEditMapping> *materializedEditMappings_ = nullptr;
  std::vector<SourceGraphOutput> *sourceGraphOutputs_ = nullptr;
  FinalLineControlValidationCallback finalLineControlValidationCallback_;

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

  /// Reset the per-attempt refold statistics to a clean baseline.
  ///
  /// This clears the counters accumulated for the current refolding attempt and
  /// reinitializes the invariant totals from the loaded refold model. The
  /// include total is the size of the recorded include tree. The macro total is
  /// the number of top-level macro invocation roots recorded during
  /// preprocessing, excluding nested expansion nodes that are attributable to
  /// an outer caller via \c callerMacroId.
  void ResetAttemptStats();

  /// Emit a readable theorem-audit summary for the current run.
  ///
  /// The first line answers the operational question: did the emitted result
  /// satisfy the strict theorem audit?  Follow-up debug lines group the dense
  /// counters by proof obligation so a failure can be read without decoding one
  /// very long ledger row.
  void EmitTheoremAudit() const;

  /// Emit a one-line summary of the final refolding statistics.
  ///
  /// The summary reports how many includes and top-level macro invocations
  /// remained expanded in the chosen refold result, relative to the total
  /// number of includes and root macro invocations recorded in the model. The
  /// line is annotated when refolding terminated by falling back to the fully
  /// expanded B-side text.
  void EmitRefoldStats() const;

  /// Per-gap ownership depth for insertion before PP token k (k in [0..N]).
  /// Computed once per refold run and reused to bound best-effort snapping.
  std::vector<uint32_t> ownerDepthGap_;

  /// Cached token-level hunks for the current refold invocation.
  ///
  /// These hunks are used to deterministically disambiguate A→B token envelope
  /// mapping at *span boundaries* when there are adjacent pure-insertion hunks
  /// (A-span is empty) that should not be absorbed into a larger mapped
  /// envelope (e.g., macro whole-cover replacement).
  std::vector<diffutils::Hunk> abTokHunks_;

  /// Persisted mixed-owner tiling witnesses for the current run.
  ///
  /// The mixed-owner normalizer still lowers a proved tiling to ordinary token
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
  /// hunk caches, and it has no access to include, macro, line-control, or proof
  /// orchestration state.  This keeps coordinate projection as an explicit
  /// service instead of another RefoldEngine responsibility.
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
  /// RefoldMixedOwnerTilingPlanner after this initial diff phase.
  std::unique_ptr<RefoldTokenDiffPlanner> tokenDiffPlanner_;

  /// Macro-boundary selector for the narrow insertion-at-cover-edge cases that
  /// may be macro-owned after later whole-cover proof.  Keeping this policy in
  /// a macro-domain service avoids retaining boundary replay helpers on
  /// RefoldEngine.
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
  /// This service stamps proven TU insertion-anchor witnesses into normalized
  /// accepted-result carriers.  It is separate from RefoldProofLattice so the
  /// TU edit planner can build anchor candidates without depending on the
  /// lattice service that will later query TU planning diagnostics.
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
  /// proof queries.  Extracted materialization/assembly services receive this
  /// narrow dependency instead of calling line-control helpers through
  /// RefoldEngine.
  RefoldLineControlProof lineControlProof_;

  /// Owner-state proof service owned by the engine.
  ///
  /// The service owns owner-state delta/graph construction, suffix-observer
  /// queries, transition-gateway checks, and the caches those proofs need. It
  /// receives only immutable inputs plus narrow audit/terminal sinks; it no
  /// longer borrows RefoldEngine or requires friend access.
  std::unique_ptr<RefoldOwnerStateProof> ownerStateProof_;

  /// Mixed-owner token-hunk tiling service owned by the engine graph.
  ///
  /// The planner owns the deterministic DP normalization that may split a
  /// replacement/deletion hunk across provable TU/include/macro owner
  /// boundaries while refreshing the mixed-owner witness ledgers.
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
  /// patch planning.  Macro-specific helper extraction should happen inside the
  /// planner instead of returning macro policy to RefoldEngine.
  std::unique_ptr<RefoldMacroPatchPlanner> macroPatchPlanner_;

  /// Macro-state repair planner owned by the engine object graph.
  ///
  /// This service preserves, moves, or materializes macro-state transitions after
  /// TU edits consume #define/#undef source.  It keeps repair liveness policy out
  /// of RefoldEngine while still mutating the already-staged TU edits explicitly.
  std::unique_ptr<RefoldMacroStateRepairPlanner> macroStateRepairPlanner_;

  /// Include-insertion patch and include-realization envelope planner.
  ///
  /// This service owns the deterministic B-envelope proof and staging patch
  /// construction for include preservation and realization.  Include
  /// materialization and expansion fallback receive it directly as a named
  /// include-edit dependency.
  std::unique_ptr<RefoldIncludeInsertionPlanner> includeInsertionPlanner_;

  /// Final byte-edit assembler owned by the engine.
  ///
  /// The assembler is intentionally a separate object from RefoldEngine: it
  /// owns final splice/application behavior and edit-map stamping while receiving
  /// proof, state, line-control, and terminal dependencies explicitly.
  std::unique_ptr<RefoldTextEditAssembler> textEditAssembler_;

  /// Line-observer layout realization service owned by the engine.
  ///
  /// The service emits TU/header materialization edits and include `#line`
  /// wrappers needed by preserved line-state observers.  It depends on proof
  /// services and the final text-edit assembler, but it owns no orchestration
  /// state from RefoldEngine.  It is declared before the include materializer so
  /// the materializer's borrowed reference remains valid through destruction.
  std::unique_ptr<RefoldLineObserverLayout> lineObserverLayout_;

  /// Include-materialization planner/realizer owned by the engine.
  ///
  /// The materializer owns include subtree realization, header-local include
  /// edit planning, child boundary insertion anchoring, and the include-facing
  /// accepted-result carriers.  It receives explicit services, ledgers, and
  /// narrow orchestration hooks rather than borrowing RefoldEngine.
  std::unique_ptr<RefoldIncludeMaterializer> includeMaterializer_;

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

  /// Allocate the mixed-owner tiling planner that normalizes token hunks after
  /// the initial token-diff pass.
  void InitializeMixedOwnerTilingPlanner();

  /// Allocate and access the translation-unit edit planning service.
  ///
  /// The planner depends on RefoldTUAnchorProof for accepted TU-anchor carrier
  /// construction, not on RefoldProofLattice.  Final TextEdit assembly remains
  /// outside this service.
  void InitializeTUEditPlanner();
  RefoldTUEditPlanner &TUEditPlanner();
  const RefoldTUEditPlanner &TUEditPlanner() const;

  /// Allocate and access the macro patch planner after the proof lattice exists.
  /// Macro-planning orchestration calls this service directly.
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
  /// observer-preserving materialization edits and include wrappers; RefoldEngine
  /// only coordinates when those edits are requested.
  void InitializeLineObserverLayout();
  RefoldLineObserverLayout &LineObserverLayout();
  const RefoldLineObserverLayout &LineObserverLayout() const;

  void InitializeIncludeMaterializer();

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
  RefoldEngine(RefoldModel model, StringRef aSource, ArrayRef<PPTok> aToks,
               ArrayRef<size_t> aTokOff, StringRef bSource,
               ArrayRef<PPTok> bToks, ArrayRef<size_t> bTokOff, bool noLines,
               bool strict, ProofAuditMode proofAuditMode,
               StringRef finalOutputPath,
               ArrayRef<SidebandPragmaEdit> sidebandPragmaEdits,
               std::vector<MaterializedEditMapping> *materializedEditMappings =
                   nullptr,
               FinalLineControlValidationCallback finalLineControlValidationCallback =
                   FinalLineControlValidationCallback(),
               std::vector<SourceGraphOutput> *sourceGraphOutputs = nullptr);

  ~RefoldEngine();

  /// \brief Run the full refolding pipeline for the current inputs.
  ///
  /// This is the main instance entry point. It classifies token hunks, plans
  /// TU/include/macro edits under the structural policy, materializes the
  /// refolded translation unit, and either returns that single-pass result or
  /// falls back to the explicit terminal edited-preprocessed-stream outcome.
  ///
  /// \returns The refolded C/C++ source text for the translation unit.
  std::string Refold();

  /// \brief Run the single structural refold pass.
  ///
  /// The caller reacts to typed requests recorded in RefoldTerminalProofSink by
  /// selecting the explicit terminal fallback result.
  std::string RunSinglePassRefold();

  /// Append source edits for sideband pragma directive text that was present in
  /// the raw `.i` replay surface but removed before token-level diffing.
  /// Returns false after requesting terminal fallback when an edit targets a
  /// non-TU owner or has an invalid source range.
  bool AppendSidebandPragmaSourceEdits(
      StringRef tuPath, StringRef tuBytes,
      RefoldStructuralHunkDispatcher &structuralHunkDispatcher);

  // ---------------------------- Ownership Helpers ----------------------------


private:
  // ------------------------------- Core Helpers ------------------------------

  /// \brief Add boundary padding spaces only when needed to preserve lexical
  /// tokenization.
  ///
  /// Adds at most one space on the left and/or right edge of `text` so that,
  /// when `text` replaces `base[start,end)` (half-open), tokens do not glue
  /// across the replacement boundary. Existing whitespace at either edge of
  /// `text`, or immediate boundary whitespace already present in `base`, counts
  /// as already-separated and suppresses padding on that side. Callers can also
  /// suppress padding explicitly via `allowLeft` / `allowRight` (for example,
  /// when preserving an existing gap).
  ///
  /// #### Behavior
  /// * Find the first and last non-whitespace token in `text`.
  /// * On each allowed side, first check whether the immediate boundary is
  ///   already separated by whitespace in `text` or `base`.
  /// * If not already separated, compare Clang raw-lexing with and without an
  ///   inserted boundary space; add a single space only when omitting it would
  ///   change tokenization across that boundary.
  /// * Never inserts more than one space per side and never modifies `base`.
  ///
  /// Deterministic and local — decisions are based on the actual lexical
  /// boundary, not on broader formatting preferences.
  ///
  /// \param base       The original target string being patched.
  /// \param start      Start index (inclusive) of the slice in `base` to
  ///                   replace.
  /// \param end        End index (exclusive) of the slice in `base` to replace.
  /// \param text       The replacement snippet to be inserted.
  /// \param allowLeft  Whether a left-side pad is permitted.
  /// \param allowRight Whether a right-side pad is permitted.
  /// \returns `text`, possibly prefixed and/or suffixed with a single space to
  ///          preserve lexical separation across the replacement boundary.
  std::string PadAtBoundaries(StringRef base, size_t start, size_t end,
                              std::string text, bool allowLeft,
                              bool allowRight) const;

  bool MaybeConsumeOrdinarySeparatorGapForPunctuation(
      StringRef tuPath, StringRef tuBytes, std::pair<uint64_t, uint64_t> &span,
      StringRef replacement, StringRef tracePrefix) const;

  bool MaybeAdvanceTUInsertionPastSourceLineControlPrefix(
      const diffutils::Hunk &h, StringRef tuPath, StringRef tuBytes,
      std::pair<uint64_t, uint64_t> &span, StringRef tracePrefix) const;

  bool TUInsertionCanDeferResyncToConditionalJoin(
      bool advancedOverSourceLineControlPrefix, StringRef tuPath,
      uint64_t anchor, StringRef tracePrefix) const;

  bool TUInsertionBeforeMaterializedInclude(
      const diffutils::Hunk &h, StringRef tuPath,
      const std::pair<uint64_t, uint64_t> &span,
      bool requireVisibleReplayText) const;

  TextEdit BuildDirectTUHunkTextEdit(
      const diffutils::Hunk &h, uint64_t hunkIndex,
      const std::pair<uint64_t, uint64_t> &span, ResyncOutcome resync,
      StringRef acceptedPayload, uint64_t rawTUStart, uint64_t rawTUEnd,
      std::optional<uint64_t> materializedBByteBegin,
      std::optional<uint64_t> materializedBByteEnd,
      AcceptedPathKind acceptedPath) const;

  // ------------------------- __COUNTER__ stabilization -----------------------

  /// \brief Inject forced __COUNTER__ stabilization patches after normal hunk
  /// attribution.
  ///
  /// __COUNTER__ expansions are time-dependent and can shift when unrelated
  /// edits add/remove counter uses earlier in the stream. After the main hunk
  /// attribution pass, this routine injects additional whole-cover MacroPatches
  /// for each forced occurrence so that all __COUNTER__ sites match the edited
  /// preprocessed stream, even if no diff hunk directly touched the invocation.
  void AddForcedCounterPatches(
      ArrayRef<ForcedMacroPatchRequest> forced,
      RefoldStructuralHunkDispatcher &structuralHunkDispatcher) const;

  bool AuditFinalLineControlAuthorityContract(
      const FinalLineControlAuthorityContract &authority,
      llvm::StringRef role) const;
  bool AuditFinalLineControlRemovalProofPopulation(
      llvm::ArrayRef<FinalLineControlPruneCandidate> candidates,
      llvm::StringRef role) const;

  /// Validate the compact owner-local proof attached to a sideband pragma edit.
  ///
  /// This is the first consolidation gate for the sideband proof system: every
  /// accepted sideband edit must explicitly discharge identity, owner, ordered
  /// anchor, closure, realization, state, location/provenance, and composition
  /// obligations before it is lowered into TU or include materialization edits.
  bool ValidateSidebandPragmaEditProof(const SidebandPragmaEdit &edit,
                                       StringRef stage) const;

  // ------------------------ Low-level Mapping & Utils ------------------------

  /// \brief Resolves the TU/file byte start offset corresponding to a PP
  /// coordinate for a specific file.
  ///
  /// The refold model maintains a PP->(file, byte-range) mapping (e.g.
  /// \c tokmapByPP) that allows code working in PP space to locate the
  /// corresponding region in an owning file (TU or header).
  ///
  /// deliberately makes this helper exact-only.  A PP coordinate that does not
  /// map into \p file is not silently projected to physical EOF.  EOF
  /// insertions are admissible only through an explicit include-anchor proof
  /// (for example a mapped left-neighbor insertion whose zero-width patch is at
  /// the include cover end), or else the caller must realize the include/fall
  /// closed to a wider declared proof class.
  ///
  /// \param file the file path whose mapping is being queried (TU or included
  ///        header)
  /// \param pp the PP token index in the A-side preprocessed token stream to
  ///        resolve
  /// \return the mapped start byte offset in \p file, or `std::nullopt` if the
  ///         PP coordinate has no exact source mapping into \p file
  std::optional<uint64_t> ByteStartForPPInFile(StringRef file,
                                               uint64_t pp) const;

  /// \brief Resolves the TU/file byte end offset corresponding to a PP
  /// coordinate for a specific file.
  ///
  /// Analogous to ByteStartForPPInFile() but returns the mapped end byte offset
  /// (\c e).  This helper is also exact-only: an unmapped PP coordinate must
  /// not manufacture an EOF byte anchor.
  ///
  /// \param file the file path whose mapping is being queried (TU or included
  ///        header)
  /// \param pp the PP token index in the A-side preprocessed token stream to
  ///        resolve
  /// \return the mapped end byte offset in \p file, or `std::nullopt` if the PP
  ///         coordinate has no exact source mapping into \p file
  std::optional<uint64_t> ByteEndForPPInFile(StringRef file,
                                             uint64_t pp) const;

};


} // namespace refold
} // namespace clang


#endif // LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDENGINE_H
