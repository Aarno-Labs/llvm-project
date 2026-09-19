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
//   • refoldTranslationUnit(...): the run controller (RefoldRunController.cpp).
//     It builds one RefoldEngine per pass, walks the narrowing and alignment
//     ladders across passes, and reads each finished pass only through the
//     engine's post-pass queries.
//   • RefoldEngine: one refold pass, fixed at construction by a
//     RefoldPassConfig.  Its services are built by BuildServiceGraph() in
//     RefoldServiceGraphBuilder.cpp.
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

#include "edit/RefoldEditTypes.h"
#include "edit/RefoldFinalAssemblyVerifier.h"
#include "edit/RefoldPatchTypes.h"
#include "line-control/FinalLineControlModel.h"
#include "line-control/LineDirectiveInserter.h"
#include "line-control/RefoldLineControlProof.h"
#include "macro/RefoldArgTextRecovery.h"
#include "macro/RefoldMacroReplay.h"
#include "macro/RefoldMacroTopology.h"
#include "model/RefoldModel.h"
#include "model/RefoldPathIdentity.h"
#include "proof/RefoldOwnerStateTypes.h"
#include "proof/RefoldProofVocabulary.h"
#include "proof/RefoldSidebandReplayProof.h"
#include "proof/RefoldTheoremAudit.h"
#include "proof/RefoldTilingWitnessTypes.h"
#include "proof/RefoldWitnessTrace.h"
#include "source/DiffAlgorithms.h"
#include "source/RefoldAlignmentSemanticResolver.h"
#include "source/RefoldSourceMapper.h"
#include "source/RefoldTokenTextAnalysis.h"
#include "support/RefoldDenseMapInfo.h"

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

struct AlignmentCertificationMemo;
struct OwnerStateGraphMemo;
class RefoldBInsertionLedger;
class RefoldCounterStabilization;
class RefoldExpansionFallbackPlanner;
class RefoldIncludeInsertionPlanner;
class RefoldIncludeMaterializer;
class RefoldPragmaOnceGuardRewriter;
class RefoldLineObserverLayout;
class RefoldStructuralHunkTilingPlanner;
class RefoldMacroPatchPlanner;
class RefoldMacroWholeCoverPlanBuilder;
class RefoldMacroStateRepairPlanner;
class RefoldMacroStateProof;
class RefoldOwnerClassifier;
class RefoldOwnerStateProof;
class RefoldPreprocessingStructureIndex;
class RefoldPreprocessingStructureIndexProvider;
class RefoldProofServices;
class RefoldProofSummaryBuilder;
class RefoldStructuralHunkDispatcher;
class RefoldTextEditAssembler;
class RefoldTextEditCertifier;
class RefoldTokenDiffPlanner;
class RefoldTUAnchorProof;
class RefoldTUEditPlanner;
struct PrintedPragmaInsertionPlacement;
struct TUInsertionAnchorAdjustment;

/// Per-pass policy and borrowed run state, fixed when a pass is built.
///
/// Every nested pass one run builds -- a production attempt, the alignment
/// resolution probe, a candidate-map simulation -- is described completely by
/// one of these, so no caller assigns a pass member after construction.  Which
/// run memos a pass borrows is itself behavior: a candidate simulation borrows
/// only the raw-byte-hunk and owner-state-graph memos.  Each memo's type
/// documents why one answer serves every pass that borrows it.
struct RefoldPassConfig {
  /// Which of one run's nested passes this is.  Logging only: no proof reads
  /// it and it enters no equivalence key.
  std::string role = "production attempt";
  bool noLines = false;
  bool strict = false;
  ProofAuditMode proofAuditMode = ProofAuditMode::Default;
  StringRef finalOutputPath;
  ArrayRef<SidebandPragmaEdit> sidebandPragmaEdits;
  /// Where each A sideband pragma line survived in B; see
  /// `SidebandPragmaLinePairing`.
  ArrayRef<SidebandPragmaLinePairing> sidebandPragmaLinePairings;
  std::vector<MaterializedEditMapping> *materializedEditMappings = nullptr;
  FinalLineControlValidationCallback finalLineControlValidationCallback;
  /// Closing assembly check; absent for probes and candidate simulations.
  std::optional<RefoldFinalAssemblyVerifier> finalAssemblyVerifier;
  ArrayRef<std::string> verifyIncludeDirs;
  /// Root invocations the run has ruled out preserving.
  llvm::DenseSet<uint64_t> ownersMustExpand;
  /// Whether this pass resolves alignment ambiguity at all.
  bool resolveAlignmentAmbiguity = false;
  /// Exact alignment to plan from, for a candidate simulation or a ladder
  /// repair; absent otherwise.
  std::optional<AlignmentSelectionOverride> alignmentSelectionOverride;
  /// False only for candidate simulations, preventing recursive resolution.
  bool alignmentSemanticResolverEnabled = true;
  /// The outer run's TU bytes, reused by candidate simulations.
  std::optional<StringRef> tuSourceBytesOverride;
  AlignmentSemanticResolutionMemo *alignmentResolutionMemo = nullptr;
  AlignmentCertificationMemo *alignmentCertificationMemo = nullptr;
  RawByteHunkMemo *rawByteHunkMemo = nullptr;
  OwnerStateGraphMemo *ownerStateGraphMemo = nullptr;
};

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
  /// Build one refold pass over the run's inputs.  \p config fixes every
  /// per-pass policy and borrowed run memo; nothing is assigned after
  /// construction.
  ///
  /// Defined out-of-line: `RefoldEngine` owns `std::unique_ptr` subsystem
  /// services, and libc++ must see each service's complete type when
  /// generating constructor cleanup paths.
  RefoldEngine(RefoldModel model, StringRef aSource, ArrayRef<PPTok> aToks,
               ArrayRef<size_t> aTokOff, StringRef bSource,
               ArrayRef<PPTok> bToks, ArrayRef<size_t> bTokOff,
               RefoldPassConfig config);

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

  /// Publish this run's alignment-resolution theorem into
  /// `alignmentResolutionMemo_` without planning or emitting anything.
  ///
  /// The narrowing loop calls this when an attempt's evidence says alignment
  /// ambiguity is what limited it.  Resolution either commits a window, in
  /// which case the loop re-plans against the recorded answer, or commits none,
  /// in which case the resolved attempt would retain exactly the core-forced
  /// anchors the finished attempt already planned from and would therefore
  /// reproduce its output byte for byte.  Asking here costs the alignment work
  /// that attempt would have started with; running the attempt to find out
  /// costs a complete pass.
  void ProbeAlignmentResolution();

  //===--------------------------------------------------------------------===//
  // Post-pass queries.  The run controller reads a finished pass only through
  // these; it never touches pass state directly.
  //===--------------------------------------------------------------------===//

  /// The ordered terminal-fallback requests this pass recorded.
  const RefoldTerminalProofSink &TerminalSink() const { return terminalSink_; }

  /// A ranges of this pass's certified alignment windows, in window order.
  llvm::ArrayRef<std::pair<uint64_t, uint64_t>>
  CertificationWindowARanges() const {
    return certificationWindowARanges_;
  }

  /// Return whether this attempt showed that alignment ambiguity is what
  /// limited it, so the next attempt should resolve and re-plan.
  ///
  /// Two kinds of evidence count, matching the two ways a run falls short:
  ///
  ///   * it gave up, and a terminal request localized the failure to a range of
  ///     A tokens -- the coordinates the resolver actually moves; or
  ///   * it emitted source but expanded a macro root or materialized an
  ///     include, which is the preservation loss forced-only anchors cause and
  ///     resolution repairs.
  ///
  /// A terminal request that names no A-token range is not evidence.  Such a
  /// failure is a fact about producer or preprocessor state, and it recurs
  /// unchanged under every alignment, so simulating candidates against it buys
  /// a complete refold of the translation unit per candidate and no proof.
  /// That is an attribution rule, not a budget: a request that does name tokens
  /// is honoured however expensive its window turns out to be.
  bool AlignmentResolutionIsDemanded() const;

  /// Return whether one certified window's A range can carry alignment
  /// ambiguity, given the forced map this attempt retained.
  ///
  /// This restates the resolver's `WindowCarriesAmbiguity()` theorem against
  /// `alignmentForcedMap_` and `certificationWindowARanges_`, which hold
  /// exactly the certified windows the resolver would consider.
  bool CertificationWindowCarriesAmbiguity(
      const std::pair<uint64_t, uint64_t> &aRange) const;

  /// Return whether any certified window can carry alignment ambiguity.
  ///
  /// When none can, resolution would pass over every window and commit nothing,
  /// so the alignment it publishes is the core-forced map the attempt already
  /// planned from.  Answering this costs one linear scan of the forced map; the
  /// alternative is a complete resolution pass that reaches the same
  /// conclusion.
  bool AnyCertificationWindowCarriesAmbiguity() const;

  /// Narrow a replacement hunk whose B payload is reproduced by tokens at its
  /// own edges, when exactly one narrowing preserves source line numbering.
  ///
  /// Suppressing a non-forced anchor widens a hunk: the tokens the anchor would
  /// have matched fall inside it, and the hunk becomes a replacement whose B
  /// payload is spelled identically to A tokens at its own edges. Every way of
  /// re-anchoring those payload tokens is an equally optimal alignment; they
  /// differ only in where the surviving deletion lands.
  ///
  /// The narrowings are separated by what they do to newlines. A deletion that
  /// removes one renumbers every line after it, and `__LINE__`, `#line` and the
  /// other location observers are semantic, so that is a real change to the
  /// surviving source rather than a matter of layout. A deletion that removes
  /// none cannot move any observer. This also subsumes the preprocessing
  /// firewall for this shape: a directive occupies its own logical line, so a
  /// range crossing one necessarily takes the newlines bounding it.
  ///
  /// Returns the sole newline-preserving narrowing. When none or several
  /// qualify it returns nullopt and the ladder proceeds unchanged, so the
  /// repair never chooses between equals. Runs only after an attempt has
  /// requested the terminal carrier.
  std::optional<AlignmentSelectionOverride> BuildLineAlignedHunkNarrowing(
      const diffutils::CertifiedLcsResult &coreAlignment) const;

  /// How this run's terminal-fallback requests resolved against the narrowing
  /// ladder.  Every request lands in exactly one bucket: it named a region the
  /// ladder can still give up, it named no region at all, or every region it
  /// named is already given up and has no enclosing region left to widen to.
  struct TerminalRequestNarrowingCensus {
    uint64_t requests = 0;
    uint64_t unattributed = 0;
    uint64_t exhausted = 0;

    /// Return whether every request named a region the ladder can still give
    /// up.
    bool EveryRequestNarrowable() const {
      return unattributed == 0 && exhausted == 0;
    }
  };

  /// Collect every region named by this run's terminal-fallback requests that
  /// has not already been given up.
  ///
  /// The returned census says which requests named no such region, and why.
  /// It is reported for diagnostics only: the caller narrows the regions that
  /// *were* named regardless, because the alternative to a partial refold is a
  /// verbatim copy of the whole translation unit, not a marginally larger one.
  ///
  /// A terminal request records the region whose proof failed when the site
  /// knows it.  Recovering that region is what lets the ladder expand one
  /// macro or one include instead of emitting the edited stream for the whole
  /// translation unit.  Requests naming no region yield nothing, which leaves
  /// the terminal carrier in place.
  TerminalRequestNarrowingCensus AppendNarrowableOwnersForTerminalRequests(
      const llvm::DenseSet<uint64_t> &alreadyExpanded,
      llvm::SmallVectorImpl<uint64_t> &owners) const;

  /// Identify the smallest region that owns a diverging edited-stream token,
  /// so an unsound region can be narrowed instead of condemning the whole
  /// translation unit.
  ///
  /// A region is a macro invocation or an include instance, named by producer
  /// id.  Both live in one id space, so the result identifies an owner without
  /// saying which kind it is -- which is the point: the caller records it in a
  /// single set, and each subsystem's existing expansion path acts on the
  /// owners belonging to it.  Narrowing adds no expansion machinery of its own.
  ///
  /// The verifier reports its mismatch in *preprocessed* edited-stream token
  /// numbering, while the producer's maps are expressed in the edited stream as
  /// written.  Those coincide only when re-preprocessing the edited stream is a
  /// no-op, which is the ordinary case -- it is already `-E -P` output -- but
  /// is not guaranteed: a stream carrying comments loses them on replay.  The
  /// correspondence is therefore checked rather than assumed, and an
  /// unverifiable one yields no owner, leaving the caller to escalate.
  ///
  /// The *smallest* covering region is returned.  Expanding it costs the least
  /// source structure, and it is the first rung of a ladder walked by
  /// `FindEnclosingOwner()`.
  std::optional<uint64_t>
  FindSmallestOwnerForEditedToken(std::size_t editedTokenIndex) const;

  /// Return the region that encloses \p ownerId, for widening a narrowing step
  /// that did not suffice.
  ///
  /// Ancestry is producer-recorded, never inferred from source overlap: an
  /// invocation widens to its caller and then to the include instance owning
  /// its callsite, and an include widens to the include that entered it.  No
  /// enclosing region means the translation unit is all that is left.
  std::optional<uint64_t> FindEnclosingOwner(uint64_t ownerId) const;

  /// Return a human-readable description of one owner, for diagnostics.
  std::string DescribeOwner(uint64_t ownerId) const;

  /// Record a terminal request for a closing-check divergence at edited token
  /// \p mismatchTokenIndex that no region is left to narrow.
  void RecordUnnarrowableDivergence(std::size_t mismatchTokenIndex) const;

  /// Build this pass's post-structural fallback result from its requests.
  std::string ResolvePostStructuralFallback();

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

  /// Proof-trace service for this pass.  Built ahead of the terminal sink,
  /// which traces every request through it; `RefoldProofServices` borrows it.
  RefoldWitnessTrace witnessTrace_;

  /// Terminal fallback request sink for the current refold attempt.
  ///
  /// The sink owns the mutable ordered request ledger and the centralized
  /// classification/normalization gate for terminal raw-B fallback.  Subsystem
  /// services receive this sink directly instead of calling back into
  /// RefoldEngine just to record a failed terminal proof obligation.
  RefoldTerminalProofSink terminalSink_;

  std::optional<FinalReplaySurface> finalReplaySurface_;
  std::vector<MaterializedEditMapping> *materializedEditMappings_ = nullptr;
  FinalLineControlValidationCallback finalLineControlValidationCallback_;

  /// Closing assembly check for this run.  Absent when the edited stream could
  /// not be preprocessed, and for candidate simulations, which are not judged.
  std::optional<RefoldFinalAssemblyVerifier> finalAssemblyVerifier_;

  /// Caller-declared last-resort include directories for verification replays.
  ///
  /// The line-observer audit re-preprocesses the assembled source just as the
  /// closing assembly check does, so it needs the same declared directories: an
  /// edited stream naming a header the producer never saw is unreadable without
  /// them, and an unreadable replay skips the audit entirely.
  std::vector<std::string> verifyIncludeDirs_;

  /// Root macro invocations whose callsite must not be preserved.
  ///
  /// The closing assembly check names the owner of a divergence; adding it here
  /// makes the next attempt refuse every candidate that would keep that
  /// callsite, so its expansion realization is taken instead.  The set only
  /// grows within a run, which is what makes the retry loop terminate: each
  /// round either verifies or removes one more owner from contention, and the
  /// ladder ends at the whole translation unit.
  llvm::DenseSet<uint64_t> ownersMustExpand_;

  /// Whether this attempt resolves alignment ambiguity at all.
  ///
  /// False for the first attempt of every run, which plans on the core
  /// theorem's forced anchors alone. A candidate map is realized by a complete
  /// refold of the translation unit, so resolution is not run until an attempt
  /// has produced evidence that ambiguity is what limited it -- see
  /// `AlignmentResolutionIsDemanded()`. When it is set, *every* window that can
  /// carry ambiguity is resolved, because a partially resolved alignment is a
  /// different alignment rather than a weaker one.
  bool resolveAlignmentAmbiguity_ = false;

  /// This run's recorded alignment-resolution theorem, owned by the narrowing
  /// loop and shared by every attempt it builds.
  ///
  /// Null for a candidate simulation, which is handed its alignment and never
  /// resolves, and for any engine built outside that loop.  See
  /// `AlignmentSemanticResolutionMemo` for why one answer serves every attempt.
  AlignmentSemanticResolutionMemo *alignmentResolutionMemo_ = nullptr;

  /// This run's certified core alignment, owned by the narrowing loop and
  /// shared by every attempt it builds.
  ///
  /// Null for a candidate simulation, which is handed its alignment through
  /// `alignmentSelectionOverride_` and never certifies one.  See
  /// `AlignmentCertificationMemo`.
  AlignmentCertificationMemo *alignmentCertificationMemo_ = nullptr;

  /// This run's raw A/B byte hunks, owned by the narrowing loop and shared by
  /// every attempt it builds.
  ///
  /// Unlike the two memos above this one is also handed to candidate
  /// simulations: a simulation is given a different alignment, but it reads the
  /// same A and B buffers, so the byte diff it would build is the same one.
  /// See `RawByteHunkMemo`.
  RawByteHunkMemo *rawByteHunkMemo_ = nullptr;

  /// This run's owner-state graph, owned by the narrowing loop and shared by
  /// every attempt it builds.
  ///
  /// Like the byte-hunk memo above this one is also handed to candidate
  /// simulations: a simulation is given a different alignment, but it censuses
  /// the same producer owners over the same A stream, so the graph it would
  /// build is the same one.  See `OwnerStateGraphMemo`.
  OwnerStateGraphMemo *ownerStateGraphMemo_ = nullptr;

  /// Whether this engine exists only to publish the alignment-resolution
  /// theorem.
  ///
  /// A probe answers one question -- does resolution commit any window -- and
  /// stops as soon as token-diff planning has answered it.  It plans no edits,
  /// assembles nothing, and emits nothing; its whole output is the memo.
  bool stopAfterAlignmentResolution_ = false;

  /// Which of one run's nested passes this engine is, for logging alone.
  ///
  /// One invocation plans the translation unit many times over: once per
  /// production attempt, once for the resolution probe, and once for every
  /// enumerated candidate alignment map the probe realizes.  Each of those is a
  /// complete pass that logs the same per-pass lines, so without a role they
  /// are indistinguishable from a loop repeating itself.  This is diagnostic
  /// state only: no proof reads it, and it enters no equivalence key.
  std::string passRole_ = "production attempt";

  /// Number of candidate alignment maps this engine realized as complete
  /// refolds, for logging alone.  Mutable because
  /// `SimulateSemanticAlignmentCandidate()` is const: a simulation reads this
  /// engine and writes only its own nested one, and this counter preserves
  /// that by recording nothing a proof can observe.
  mutable uint64_t alignmentCandidateSimulationCount_ = 0;

  /// The core theorem's forced A-to-B map for this attempt, retained after
  /// token-diff planning.
  ///
  /// A forced anchor is an edge every optimal path takes. That makes it the
  /// exact test for whether a hunk frontier is pinned -- and therefore whether
  /// resolution could place the hunk anywhere else -- which is the evidence
  /// `AlignmentResolutionIsDemanded()` reads.
  std::vector<int64_t> alignmentForcedMap_;

  /// A ranges of this attempt's certified certification windows, in window
  /// order.  Paired with `alignmentForcedMap_` these say which regions of the
  /// stream still admit more than one optimal map.
  std::vector<std::pair<uint64_t, uint64_t>> certificationWindowARanges_;

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
  /// Where each sideband pragma line removed from the A token stream survived
  /// in B.  Structural tiling reads this to place a preserved directive at the
  /// gap B printed it in.
  std::vector<SidebandPragmaLinePairing> sidebandPragmaLinePairings_;
  /// The source carrier of every paired line, derived from the pairings once
  /// the macro topology exists; see `PrintedPragmaCarrier`.
  std::vector<PrintedPragmaCarrier> printedPragmaCarriers_;

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
  std::vector<StructuralHunkTilingWitness> structuralHunkTilingWitnesses_;
  std::vector<StructuralHunkTilingSegmentBinding>
      structuralHunkTilingSegmentBindings_;

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
  /// byte-hunk cache construction. Structural hunk tiling is handled by
  /// RefoldStructuralHunkTilingPlanner after the initial token-diff plan is
  /// built.
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

  /// Whole-cover replacement plan computation for this refold run.
  ///
  /// Constructed after the B-insertion ledger and before every consumer: the
  /// macro patch planner, the text-edit assembler, and counter stabilization
  /// all borrow it directly.  Keeping plan computation out of the planner is
  /// what lets them avoid reaching it through a late-bound callback.
  std::unique_ptr<RefoldMacroWholeCoverPlanBuilder> wholeCoverPlanBuilder_;

  /// Translation-unit edit planning service.
  ///
  /// This owns direct TU edit-plan and trailing-call-suffix extension policy
  /// over the anchors and byte spans RefoldTUAnchorProof proves.  Final
  /// TextEdit assembly and legacy orchestration wrappers remain outside this
  /// service.
  std::unique_ptr<RefoldTUEditPlanner> tuEditPlanner_;

  /// TU insertion-anchor and TU byte-span proofs.
  ///
  /// This service proves anchors and byte spans, and certifies each proven
  /// anchor witness into a normalized accepted-result carrier.  It is separate
  /// from the accepted candidate builder, which depends on it through the
  /// owner-realization proof builder.
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
  std::unique_ptr<RefoldStructuralHunkTilingPlanner>
      structuralHunkTilingPlanner_;

  /// Macro-state proof service owned by the engine.
  ///
  /// The service owns read-only #define/#undef observation and directive-line
  /// recovery proof.  It receives owner-state proof explicitly for the one
  /// materialized-header stabilization path that records macro-state movement
  /// witnesses, instead of reaching back through RefoldEngine.
  std::unique_ptr<RefoldMacroStateProof> macroStateProof_;

  /// Proof-summary construction.  Depends on B alone; built before the theorem
  /// audit, which borrows it.
  std::unique_ptr<RefoldProofSummaryBuilder> proofSummaryBuilder_;

  /// Accepted-result proof services owned by the engine: witness trace and
  /// resolution, ranking, path and macro-patch classification, owner
  /// realization, and accepted-candidate construction.  Feature services
  /// borrow the individual services they use; nothing borrows this object.
  std::unique_ptr<RefoldProofServices> proofServices_;

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

  /// Protected-source authority and materialization certification.  Declared
  /// before every service that borrows it.
  std::unique_ptr<RefoldTextEditCertifier> textEditCertifier_;

  /// Line-observer layout realization service owned by the engine.
  ///
  /// The service emits TU/header materialization edits and include `#line`
  /// wrappers needed by preserved line-state observers, and repairs newline
  /// drift.  It depends on proof services and the text-edit certifier, but it
  /// owns no orchestration state from RefoldEngine.  It is declared before the
  /// assembler and the include materializer so their borrowed references remain
  /// valid through destruction.
  std::unique_ptr<RefoldLineObserverLayout> lineObserverLayout_;

  /// Final byte-edit assembler owned by the engine.
  ///
  /// The assembler is intentionally a separate object from RefoldEngine: it
  /// owns final splice/application behavior and edit-map certifying while
  /// receiving proof, state, line-control, and terminal dependencies
  /// explicitly.
  std::unique_ptr<RefoldTextEditAssembler> textEditAssembler_;

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

  /// Construct every planning, proof and emission service in dependency order.
  /// Each service receives explicit inputs and services; none stores the
  /// engine.  Defined in RefoldServiceGraphBuilder.cpp, the composition root.
  void BuildServiceGraph();

  /// Move hunk edges off macro expansions the hunk only partially owns.
  ///
  /// A realizer reconstructs source by re-emitting a callsite, so an expansion
  /// must be wholly inside a hunk or wholly outside it.  When an edge lands
  /// strictly inside one, no owner can realize the hunk: the source projection
  /// widens to the complete invocation spelling while the replacement carries
  /// only the fragment of expanded tokens inside the hunk.  Either the
  /// difference is dropped silently -- `return NULL;` became `return );` when
  /// an alignment boundary fell one token inside `NULL`'s `((void*)0)`
  /// expansion -- or every owner refuses and the whole translation unit
  /// escalates to raw B.
  ///
  /// The token objective admits such a boundary because it scores lexemes, not
  /// expansions: matching two tokens of `((void*)0)` against a `0` and a `)`
  /// that an edit newly wrote is one match richer than leaving them unmatched,
  /// and can be forced on every optimal path.
  ///
  /// Two kinds of move resolve a split expansion, and this tries them in
  /// preference order.  *Retraction* walks an edge inward across tokens that
  /// are identical on both sides; it keeps the invocation preserved, so it is
  /// preferred, and it is what restores a match the certifier left unforced
  /// because a repeated spelling made it ambiguous.  It comes in two forms.
  /// The edge inside the expansion retracts out of it, giving the expansion
  /// back to the untouched region beside the hunk.  When that edge's tokens
  /// differ, the opposite edge may instead retract up to the expansion's
  /// boundary, giving the tokens outside it back and leaving the hunk contained
  /// in the expansion, where the callsite realizers own it -- the shape an
  /// edit rewriting the start of a macro argument produces, when a repeated
  /// `(` leaves the certifier unable to say which side of the callsite the new
  /// one belongs on.  *Widening* walks the edge outward to the expansion's own
  /// boundary, taking the rest of the expansion into the hunk; it gives up that
  /// one callsite's spelling and is the only move available when no retraction
  /// is, which is what an edit that rewrites the expression around a callsite
  /// produces.
  ///
  /// Widening is sound because an absorbed token pair sits in the untouched run
  /// between two hunks, matched to each other by the selected alignment: moving
  /// such a pair across the edge leaves the edit script producing exactly the
  /// same B.  It is admitted only on the A->B map's own evidence and never
  /// moves an edge into the neighbouring hunk, for reasons the helper
  /// documents.  When the walk arrives at a neighbour that holds part of the
  /// same expansion, the two hunks are merged -- the neighbour's replacement,
  /// the walked run, and this hunk's replacement still produce exactly the same
  /// B -- and the walk continues from the merged edge, so an edit touching an
  /// argument and the tokens past the callsite replaces the whole callsite.
  /// An edge that cannot reach a whole-expansion boundary this way is left
  /// alone for the ordinary realizer lattice, which refuses a partial cover --
  /// so this repair never trades a refusal for a guess.
  void RepairHunkEdgesOutOfPartiallyOwnedMacroExpansions(
      std::vector<diffutils::Hunk> &hunks) const;






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
  /// simulations and one exact semantic equivalence class per certification
  /// window.
  ///
  /// `certificationByteBudget` is the budget the core theorem ran under; the
  /// per-window pair facts recomputed during resolution are charged against
  /// that same budget.
  void ResolveSemanticAlignment(
      llvm::ArrayRef<llvm::StringRef> aLexemes,
      llvm::ArrayRef<llvm::StringRef> bLexemes,
      llvm::ArrayRef<diffutils::LcsAGapProvenance> aGapProvenance,
      llvm::ArrayRef<diffutils::LcsBGapProvenance> bGapProvenance,
      uint64_t certificationByteBudget,
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

  /// Prove that the assembled source re-expands every preserved `__LINE__`
  /// observer to the value the edited stream carries, and request terminal
  /// fallback when it does not.
  ///
  /// The observer is located by the preprocessor rather than by any byte
  /// mapping: the assembled source is preprocessed through the producer's
  /// recorded context and its token stream compared to B.  Only positions that
  /// B produced from a `__LINE__` expansion are enforced, so this closes the
  /// newline-drift ordering hole without becoming a general re-check.
  ///
  /// Returns false only after recording the terminal request that names the
  /// displaced observer and the region owning it, so the caller performs the
  /// fallback without raising a second request for the same failure.  It also
  /// returns true, auditing nothing, once any earlier check has reached the
  /// seam: the assembly has been replaced by the edited stream at that point,
  /// which replays every observer by construction.
  bool AuditPreservedLineObserversInFinalOutput(llvm::StringRef finalSource);


  /// Name the smallest region owning one *edited stream* token, given an index
  /// already known to address that stream.
  ///
  /// This is the half of `FindSmallestOwnerForEditedToken()` after the
  /// verifier-numbering check, exposed for a caller that indexed the edited
  /// stream itself and so has nothing to reconcile.  Do not call it with an
  /// index reported by the closing verifier: that numbering coincides with the
  /// producer's only when re-preprocessing the edited stream is a no-op, and
  /// `FindSmallestOwnerForEditedToken()` exists to check it.
  std::optional<uint64_t> FindSmallestOwnerForBToken(uint64_t bToken) const;





  /// Return the smallest region covering one *original*-stream token.
  ///
  /// This is the half of the search that does not depend on the edited stream,
  /// so a terminal-fallback request that recorded an A-token range can reuse it
  /// without a verifier having run.
  std::optional<uint64_t> FindSmallestOwnerForAToken(uint64_t aToken) const;

  /// Append the minimal regions covering every token of `[aBegin, aEnd)`.
  ///
  /// A failing range routinely spans several regions, and naming only the one
  /// owning its first token cannot repair a divergence inside a later one:
  /// widening escalates through that first region's *ancestry*, which need
  /// never reach a sibling include holding the rest of the range.
  ///
  /// The result is the minimal antichain -- each token's narrowest owner,
  /// deduplicated -- and deliberately not the transitive closure.  Every
  /// enclosing region also covers these tokens, so including them would give up
  /// a whole header to repair a hunk one invocation wide.  Escalating to an
  /// enclosing region stays the caller's separate widening step.
  ///
  /// Owners are appended in ascending producer id, and ids already present in
  /// \p owners are not repeated, so the result does not depend on model order.
  void AppendMinimalOwnersCoveringATokenRange(
      uint64_t aBegin, uint64_t aEnd,
      llvm::SmallVectorImpl<uint64_t> &owners) const;



  /// Build the ordinary direct-TU byte-span edit realizing one token hunk over
  /// \p span, the TU byte range a direct-TU proof has already accepted.
  ///
  /// This is the realization half of the direct-TU path: the caller supplies a
  /// span that `RefoldTUAnchorProof::PlanTUByteSpan()` proved, and this turns
  /// it into replacement text -- B token slice, gap and spacing repair,
  /// trailing call-suffix extension, line-control resync -- and certifies the
  /// resulting edit.  Returns std::nullopt when the edit could not be
  /// certified, leaving the caller to escalate.
  ///
  /// It is separate from span planning so that a caller holding a *different*
  /// proved span for the same hunk can reuse the identical realization rather
  /// than restating it.
  std::optional<TextEdit>
  BuildDirectTUByteSpanEditForHunk(const diffutils::Hunk &h, size_t hunkIndex,
                                   bool isDel, StringRef tuPath,
                                   StringRef tuBytes,
                                   std::pair<uint64_t, uint64_t> span);

  /// Place a pure TU insertion among the printed pragma lines preserved at
  /// its A gap; see `placeTUInsertionAmongPrintedPragmas`.  Non-insertions
  /// and gaps without such lines report `NotApplicable`.
  PrintedPragmaInsertionPlacement PlaceTUInsertionAmongPrintedPragmas(
      const diffutils::Hunk &h, StringRef tuPath, uint64_t baseAnchor) const;

  /// Return the B bytes a pure insertion replays: its token envelope, cut
  /// before the first preserved pragma line B prints after it when
  /// \p placement placed it.
  StringRef
  InsertionEnvelope(const diffutils::Hunk &h,
                    const PrintedPragmaInsertionPlacement &placement) const;

  /// Return the typed anchor adjustment an insertion's final site carries,
  /// or std::nullopt when it still sits on its base anchor.
  static std::optional<TUInsertionAnchorAdjustment>
  InsertionAnchorAdjustment(const PrintedPragmaInsertionPlacement &placement,
                            bool advancedOverSourceLineControlPrefix,
                            uint64_t rawTUStart, uint64_t anchor);

  /// \brief Run the structural refold pass.
  ///
  /// The caller reacts to typed requests recorded in RefoldTerminalProofSink by
  /// selecting the explicit terminal fallback result.
  std::string RunRefoldPass();
};

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
Expected<std::string> refoldTranslationUnit(
    const json::Object &rootJson, StringRef aSource, ArrayRef<PPTok> aToks,
    ArrayRef<size_t> aTokOff, StringRef bSource, ArrayRef<PPTok> bToks,
    ArrayRef<size_t> bTokOff, bool noLines, bool strict,
    ProofAuditMode proofAuditMode = ProofAuditMode::Default,
    StringRef finalOutputPath = StringRef(),
    ArrayRef<SidebandPragmaEdit> sidebandPragmaEdits = {},
    ArrayRef<SidebandPragmaLinePairing> sidebandPragmaLinePairings = {},
    std::vector<MaterializedEditMapping> *materializedEditMappings = nullptr,
    FinalLineControlValidationCallback finalLineControlValidationCallback =
        FinalLineControlValidationCallback(),
    OutputVerificationMode verifyMode = OutputVerificationMode::Off,
    llvm::ArrayRef<std::string> verifyIncludeDirs = {});

} // namespace refold
} // namespace clang

#endif // LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDENGINE_H
