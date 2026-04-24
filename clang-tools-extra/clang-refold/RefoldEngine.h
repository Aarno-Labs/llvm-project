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
//   drift. • Boundary padding inserts at most one space locally when needed by
//     maximal-munch rules; internal whitespace is preserved verbatim.
//   • Errors are reported via the Logging subsystem (`fatal()/error()/...`).
//
// Public Surface
// --------------
//   • std::string Refold(...): orchestrates the end-to-end refolding and
//     returns the refolded TU text.
//   • Helper utilities: token/byte mapping, hunk builders, include realization,
//     macro-patch construction, and line-local boundary checks.
//
// Notes
// -----
//   • No RTTI or exceptions required; mirrors LLVM/Clang style.
//   • Paths are compared via canonicalization (see RefoldEngine::PathsEqual()).
//   • All indices are half-open where applicable: tokens [lo,hi), bytes [b,e).
//
// Author:
//   jeikenberry
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDENGINE_H
#define LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDENGINE_H

#include "DiffAlgorithms.h"
#include "LineDirectiveInserter.h"
#include "RefoldModel.h"
#include "StringUtils.h"
#include "clang/Basic/LangOptions.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/FormatVariadic.h"
#include "llvm/Support/JSON.h"

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

using namespace llvm;

namespace clang {
namespace refold {

struct PPTok {
  // Clang token kind (e.g. "identifier", "numeric_constant", "string_literal",
  // "l_paren"). This is sourced from the token dump and is intentionally a
  // simple string.
  std::string kind;
  std::string spelling;
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
/// The engine intentionally does not attempt to model complex preprocessor
/// features that require semantic re-expansion (e.g., token-pasting, variadics,
/// or aggressive escape normalization). In such cases it prefers
/// deterministic expansion of the affected macro instance over speculative
/// rewriting.
///
/// \author jeikenberry
class RefoldEngine {
struct ByteHunk;
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
  /// \returns          The refolded, partially expanded C source.
  static Expected<std::string>
  Refold(const json::Object &rootJson, StringRef aSource, ArrayRef<PPTok> aToks,
         ArrayRef<size_t> aTokOff, StringRef bSource, ArrayRef<PPTok> bToks,
         ArrayRef<size_t> bTokOff, bool noLines, bool strict);

  /// Build lexer language options from the producer-captured language name.
  static clang::LangOptions MakeLexLangOptions(llvm::StringRef langName);

private:
  const RefoldModel model_;
  StringRef aSource_, bSource_;
  ArrayRef<PPTok> aToks_, bToks_;
  ArrayRef<size_t> aTokOff_, bTokOff_;
  LineDirectiveInserter lineDirs_;
  bool strict_;
  LangOptions lexLang_;

  /// \brief Explicit classification for the remaining terminal fallback exits.
  ///
  /// The post-Step-12 engine no longer retries under more expanded tiers. If a
  /// helper still cannot discharge into the declared proof lattice, it requests
  /// the one explicit terminal fallback. Track which exclusion triggered that
  /// fallback so the completeness boundary is stated in terms of named
  /// out-of-domain cases rather than an opaque escape hatch.
  ///
  /// Step 5 formalizes `OwnerUnresolvedNoTUAnchor` as an explicit theorem
  /// boundary rather than a vague implementation leftover. That exclusion means
  /// the edit had no macro/include owner and also lacked the TU witness needed
  /// to stay in-domain: a truthful TU-owned mapped span for non-insertions, or
  /// an exact/provable TU insertion anchor for pure insertions.
  enum class TerminalFallbackKind : uint8_t {
    Unknown,
    OwnerUnresolvedNoTUAnchor,
    IncludeRealizationUnmappableBCoverEnvelope,
    UndischargedEmissionArtifact,
    /// Step 6: strict mode turns any surviving theorem-audit invariant
    /// violation into the one explicit terminal out-of-domain result instead of
    /// merely logging it as a dashboard counter.
    TheoremAuditInvariantViolation,
    MixedExcludedCases,
  };

  /// \brief Compact witness describing why the single-pass engine fell back to B.
  struct TerminalFallbackWitness {
    TerminalFallbackKind kind = TerminalFallbackKind::Unknown;
    uint32_t requestCount = 0;
    bool hasPrimaryReason = false;
    std::string primaryReason;
  };

  struct RefoldStats {
    uint64_t totalIncludes = 0;
    uint64_t expandedIncludes = 0;
    uint64_t totalMacros = 0;
    uint64_t expandedMacros = 0;
    bool terminalFallbackToB = false;
  };

  /// \brief Declared structural-refolding domain used by the theorem audit.
  ///
  /// Step 7 freezes the theorem/completeness target so later work does not
  /// silently move the goalposts. The engine treats an input as inside the
  /// declared structural domain iff every emitted non-terminal artifact can be
  /// represented by a normalized accepted-result carrier such that:
  ///   - the carrier belongs to a declared proof class,
  ///   - the carrier is explicit-proof-backed,
  ///   - the carrier is inside the declared domain,
  ///   - the carrier is locally discharged,
  ///   - final competition is resolved only by the shared lattice, and
  ///   - no unresolved owner / unresolved B-envelope exclusion remains except
  ///     those explicitly tracked as named out-of-domain terminal results.
  ///
  /// Internal staging objects may temporarily lack one of these properties, but
  /// they are not theorem-facing and therefore do not count toward the domain
  /// statement until they are restamped onto a concrete emitted carrier.
  struct TheoremAuditStats {
    uint64_t emittedNonTerminalEdits = 0;
    uint64_t emittedCarriers = 0;
    uint64_t emittedDeclaredClassCarriers = 0;
    uint64_t emittedDischargedCarriers = 0;
    uint64_t emittedSelectorOnlyExceptionCarriers = 0;
    uint64_t emittedTransitionalTheoremCarriers = 0;
    uint64_t emittedUndischargedCarriers = 0;
    uint64_t emittedUnknownClassCarriers = 0;
    uint64_t emittedOutOfDomainCarriers = 0;
    uint64_t selectorCompetitions = 0;
    uint64_t selectorResolutions = 0;
    uint64_t selectorNoSelectable = 0;
    uint64_t selectorUnresolvedCompetitions = 0;
    uint64_t selectorDirectBypasses = 0;
    uint64_t explicitTerminalExclusions = 0;
    uint64_t nonExplicitTerminalExclusions = 0;
    bool theoremSatisfied = true;
    std::string firstViolation;
  };

  RefoldStats lastStats_;
  mutable TheoremAuditStats lastTheoremAudit_;

  // Single-pass terminal-fallback scaffold: if any edit/patch cannot be
  // discharged into the declared proof/lattice outcomes in the current pass,
  // record the reason and fall back to emitting the fully expanded edited
  // preprocessed stream (B). This keeps the engine fail-closed.
  //
  // Note: the fallback is requested from several helper routines that are
  // logically "const" (for example include materialization). Treat the
  // fallback state as diagnostic/side-channel state via `mutable`.
  mutable bool terminalFallbackRequested_ = false;
  mutable std::vector<std::string> terminalFallbackReasons_;
  mutable TerminalFallbackKind terminalFallbackKind_ =
      TerminalFallbackKind::Unknown;
  mutable uint32_t terminalFallbackRequestCount_ = 0;

  /// \brief Record that the current pass must fall back to the explicit
  /// terminal edited-preprocessed-stream result.
  ///
  /// Helpers call this when they cannot discharge an edit into one of the
  /// single-pass proof/lattice outcomes.
  void RequestTerminalFallback(TerminalFallbackKind kind, llvm::StringRef phase,
                               llvm::StringRef detail) const;

  /// \brief Clear per-pass terminal-fallback state.
  ///
  /// This is invoked once before running the single structural pass.
  void ResetTerminalFallbackState() const {
    terminalFallbackRequested_ = false;
    terminalFallbackReasons_.clear();
    terminalFallbackKind_ = TerminalFallbackKind::Unknown;
    terminalFallbackRequestCount_ = 0;
  }

  /// Reset the per-attempt refold statistics to a clean baseline.
  ///
  /// This clears the counters accumulated for the current refolding attempt and
  /// reinitializes the invariant totals from the loaded refold model. The
  /// include total is the size of the recorded include tree. The macro total is
  /// the number of top-level macro invocation roots recorded during
  /// preprocessing, excluding nested expansion nodes that are attributable to an
  /// outer caller via \c callerMacroId.
  void ResetAttemptStats() {
    lastStats_ = RefoldStats{};
    lastStats_.totalIncludes = model_.GetIncludes().size();
    for (const auto &mi : model_.GetMacroInvocations()) {
      if (!mi.callerMacroId)
        ++lastStats_.totalMacros;
    }
  }

  /// Reset the per-run theorem audit counters.
  void ResetTheoremAudit() const { lastTheoremAudit_ = TheoremAuditStats{}; }

  /// Record the first theorem-audit violation encountered in the current run.
  void NoteTheoremAuditViolation(llvm::StringRef detail) const {
    if (lastTheoremAudit_.theoremSatisfied)
      lastTheoremAudit_.firstViolation = detail.str();
    lastTheoremAudit_.theoremSatisfied = false;
  }


  /// Step 6 upgrades the theorem audit from a descriptive dashboard into an
  /// invariant checker. This helper normalizes any surviving counter-based
  /// violations onto the theorem state and, in strict mode, requests the one
  /// explicit terminal fallback instead of allowing a structurally-refolded
  /// result to escape with a violated theorem audit.
  void EnforceTheoremAuditInvariants() const;

  /// Validate that the terminal fallback state itself still classifies as the
  /// one explicit out-of-domain theorem carrier.
  ///
  /// Step 8 closes the final audit hole here: it is not enough for the engine
  /// to request fallback to B; the resulting witness must also restamp onto the
  /// normalized terminal explicit-out-of-domain carrier rather than escaping as
  /// an unclassified fallback side effect.
  void RecordTerminalFallbackTheoremAudit() const;

  /// Build a compact diagnostic string for a strict-mode theorem-audit
  /// fallback.
  std::string BuildTheoremAuditInvariantDetail() const;

  /// Emit a compact theorem-audit summary for the current run.
  ///
  /// The counters summarize the declared-domain statement above rather than an
  /// independent ad hoc checklist: every emitted non-terminal carrier must be
  /// declared, discharged, lattice-selected, and in-domain; any transitional
  /// theorem-facing carrier or unresolved selector competition is a violation;
  /// and named terminal exclusions remain the only acceptable out-of-domain
  /// escape.
  void EmitTheoremAudit() const {
    info("theorem",
         "satisfied={0} emittedEdits={1} carriers={2} declared={3} discharged={4} "
         "selectorOnlyExceptions={5} transitional={6} undischarged={7} unknownClass={8} outOfDomain={9} "
         "selectorCompetitions={10} selectorResolutions={11} selectorNoSelectable={12} selectorUnresolved={13} "
         "selectorDirectBypasses={14} explicitTerminalExclusions={15} nonExplicitTerminalExclusions={16}",
         lastTheoremAudit_.theoremSatisfied ? 1 : 0,
         lastTheoremAudit_.emittedNonTerminalEdits,
         lastTheoremAudit_.emittedCarriers,
         lastTheoremAudit_.emittedDeclaredClassCarriers,
         lastTheoremAudit_.emittedDischargedCarriers,
         lastTheoremAudit_.emittedSelectorOnlyExceptionCarriers,
         lastTheoremAudit_.emittedTransitionalTheoremCarriers,
         lastTheoremAudit_.emittedUndischargedCarriers,
         lastTheoremAudit_.emittedUnknownClassCarriers,
         lastTheoremAudit_.emittedOutOfDomainCarriers,
         lastTheoremAudit_.selectorCompetitions,
         lastTheoremAudit_.selectorResolutions,
         lastTheoremAudit_.selectorNoSelectable,
         lastTheoremAudit_.selectorUnresolvedCompetitions,
         lastTheoremAudit_.selectorDirectBypasses,
         lastTheoremAudit_.explicitTerminalExclusions,
         lastTheoremAudit_.nonExplicitTerminalExclusions);
    if (!lastTheoremAudit_.theoremSatisfied &&
        !lastTheoremAudit_.firstViolation.empty()) {
      info("theorem", "firstViolation={0}",
           stringutils::showWSWithClip(lastTheoremAudit_.firstViolation, 220));
    }
  }

  /// Emit a one-line summary of the final refolding statistics.
  ///
  /// The summary reports how many includes and top-level macro invocations
  /// remained expanded in the chosen refold result, relative to the total
  /// number of includes and root macro invocations recorded in the model. The
  /// line is annotated when refolding terminated by falling back to the fully
  /// expanded B-side text.
  void EmitRefoldStats() const {
    info("stats",
         "includes-expanded={0}/{1} macros-expanded={2}/{3}{4}",
         lastStats_.expandedIncludes, lastStats_.totalIncludes,
         lastStats_.expandedMacros, lastStats_.totalMacros,
         lastStats_.terminalFallbackToB ? " terminal-fallback=B" : "");
  }

  /// Per-gap ownership depth for insertion before PP token k (k in [0..N]).
  /// Computed once per refold run and reused to bound best-effort snapping.
  std::vector<uint32_t> ownerDepthGap_;

  /// Cached token-level hunks for the current refold invocation.
  ///
  /// These hunks are used to deterministically disambiguate A→B token
  /// envelope mapping at *span boundaries* when there are adjacent
  /// pure-insertion hunks (A-span is empty) that should not be absorbed
  /// into a larger mapped envelope (e.g., macro whole-cover replacement).
  std::vector<diffutils::Hunk> abTokHunks_;

  std::optional<std::vector<ByteHunk>> abByteHunks_;

  // ---------------------------------------------------------------------------
  // B token provenance / ownership for pure insertions
  // ---------------------------------------------------------------------------
  //
  // Goal: enforce a global invariant that no B-only insertion segment is
  // emitted twice (e.g., once as a standalone boundary insertion patch and
  // again as part of a macro whole-cover replacement slice).
  //
  // We track token-level pure-insertion hunks (A length 0, B length > 0) and
  // allow exactly one emission site to claim each insertion.

  // Emission ownership for a pure B-only insertion segment.
  enum class BInsertionClaim : uint8_t {
    Unclaimed = 0,
    Standalone = 1 // emitted as a TU/include/arm insertion patch
  };

  // Provenance record for one token-level pure insertion hunk:
  // inserted at A-gap aGap, produced by hunkIndex, spanning B tokens [b0,b1).
  struct BInsertionProv {
    uint64_t aGap = 0;      // insertion position in A (pp token gap)
    uint64_t hunkIndex = 0; // index in abTokHunks_
    size_t b0 = 0;          // [b0,b1) in B token space
    size_t b1 = 0;
    BInsertionClaim claim = BInsertionClaim::Unclaimed;
  };

  // Master table of all tracked pure insertion segments.
  std::vector<BInsertionProv> bInsertions_;

  // Reverse map: B token index -> insertion id in bInsertions_, or -1 if this
  // B token is not part of a tracked pure insertion.
  std::vector<int32_t> bTokToInsertionId_;

  // Reverse map: token diff hunk index -> insertion id in bInsertions_, or -1
  // if the hunk is not a pure insertion.
  std::vector<int32_t> hunkToInsertionId_;

  /// \brief Build provenance indices for token-level pure insertion hunks.
  ///
  /// Scans \p hunks for token-level pure insertions (A-span empty, B-span
  /// non-empty) and populates:
  ///   - \c bInsertions_           : insertion records in B token space
  ///   - \c bTokToInsertionId_     : per-B-token map to insertion id (or -1)
  ///   - \c hunkToInsertionId_     : per-hunk map to insertion id (or -1)
  ///
  /// The resulting structures allow later phases to enforce a global “emit each
  /// B-only segment exactly once” invariant (no double-emission).
  void BuildBInsertionProvenance(ArrayRef<diffutils::Hunk> hunks);

  /// \brief Pre-claim insertions that must be emitted as standalone boundary
  /// patches.
  ///
  /// Runs as an early planning pass (before macro patch construction) so that
  /// later B-slicing codepaths (e.g. macro whole-cover replacement) can clip
  /// away B-only segments that are already committed to standalone emission.
  ///
  /// Macro call-sites take priority: if an insertion lies within a patchable
  /// macro cover, it is left unclaimed so the macro patch may absorb it.
  void PreclaimStandaloneInsertions(StringRef tuPath,
                                    ArrayRef<diffutils::Hunk> hunks);

  /// \brief Claim a pure insertion hunk for a specific emission site.
  ///
  /// Claims are used to prevent double-emission. Attempting to claim an already
  /// claimed insertion with a different claim kind is a hard error.
  ///
  /// \param insId Insertion id in \c bInsertions_.
  /// \param c     Claim kind to record.
  /// \param why   Debug string describing the claimant (for diagnostics).
  void ClaimBInsertion(size_t insId, BInsertionClaim c, llvm::StringRef why);

  /// \brief Partition a B-token interval into segments that are safe to emit.
  ///
  /// Returns a small list of sub-ranges of \c [bTokStart,bTokEnd) with any
  /// B-only insertion hunks claimed as \c Standalone removed. The returned
  /// sub-ranges are ordered and non-overlapping.
  llvm::SmallVector<std::pair<size_t, size_t>, 4>
  ClipBTokenRangeAgainstClaims(size_t bTokStart, size_t bTokEnd) const;

  /// \brief Slice \c [bTokStart,bTokEnd) from B while omitting claimed
  /// insertion segments.
  ///
  /// This concatenates the segments returned by \c ClipBTokenRangeAgainstClaims
  /// and returns the resulting byte sequence.
  std::string SliceBSourceClippedAgainstClaims(size_t bTokStart,
                                               size_t bTokEnd) const;

  // Macro invocation graph (derived from RefoldModel) used for structural
  // queries over recorded callerMacroId relationships.
  DenseMap<uint64_t, SmallVector<const RefoldModel::MacroInvocation *, 4>>
      macroChildrenById_;

  /// Construct an engine from concrete inputs. The instance method `Refold()`
  /// runs the full pipeline using these captured members.
  RefoldEngine(RefoldModel model, StringRef aSource, ArrayRef<PPTok> aToks,
               ArrayRef<size_t> aTokOff, StringRef bSource,
               ArrayRef<PPTok> bToks, ArrayRef<size_t> bTokOff, bool noLines,
               bool strict)
      : model_(std::move(model)), aSource_(aSource), bSource_(bSource),
        aToks_(aToks), bToks_(bToks), aTokOff_(aTokOff), bTokOff_(bTokOff),
        lineDirs_(!noLines, model_.GetPPCwd()), strict_(strict),
        lexLang_(MakeLexLangOptions(model_.GetPPLang())) {
    BuildMacroInvocationGraph();
  }

  // Build derived macro indices once per run.
  // Note: pointers into model_.GetMacroInvocations() remain stable.
  void BuildMacroInvocationGraph() {
    macroChildrenById_.clear();
    for (const auto &mi : model_.GetMacroInvocations()) {
      if (mi.callerMacroId) {
        macroChildrenById_[*mi.callerMacroId].push_back(&mi);
      }
    }
  }

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
  /// The caller reacts to \c RequestTerminalFallback() by selecting the
  /// explicit terminal fallback result.
  std::string RunSinglePassRefold();

  // ---------------------------- Small Data Records ---------------------------

  /// Represents a pending resync that must be flushed at the next safe BOL.
  struct PendingResync {
    std::string fileSpellingForDir;

    explicit PendingResync(llvm::StringRef file)
        : fileSpellingForDir(file.str()) {}
  };

  /// The final text and optional pending state for an edit.
  struct ResyncOutcome {
    std::string text;
    std::optional<PendingResync> pending;

    ResyncOutcome(std::string t, std::optional<PendingResync> p)
        : text(std::move(t)), pending(std::move(p)) {}
  };

  struct AcceptedResultCandidate;

  struct TextEdit {
    uint64_t start, end;
    std::string text;
    std::optional<PendingResync> pending;

    // Root macro invocation id to charge as expanded if this edit survives
    // normalization and is applied in the final chosen refold result.
    std::optional<uint64_t> expandedMacroRootId = std::nullopt;

    // Normalized accepted-result carriers for the non-terminal artifacts that
    // were composed into this final emitted edit. Step 2A is structural only:
    // it does not change emission semantics, but it makes the proof-bearing
    // source of each emitted artifact explicit at the byte-edit boundary so a
    // later universal proof gate can reason over the actual emitted surface.
    std::vector<std::shared_ptr<const AcceptedResultCandidate>>
        acceptedResults;
  };

  // Result of planning header-local edits for one include. When
  // requiresIncludeRealization is set, the caller must stop trying to anchor
  // header-local edits and realize the include directly from the edited
  // preprocessed stream B instead.
  struct IncludeTextEditPlan {
    std::vector<TextEdit> edits;
    bool requiresIncludeRealization = false;
    std::string realizationReason;
  };

  struct PasteArgEdit {
    uint32_t argIdx;
    std::string newSeg;
    std::string oldSeg;

    PasteArgEdit(uint32_t Idx, std::string New, std::string Old)
        : argIdx(Idx), newSeg(std::move(New)), oldSeg(std::move(Old)) {}
  };

  // ---------------------------- Ownership Helpers ----------------------------

  enum class OwnerKind { TU, Include, Unknown };

  static inline StringRef toString(OwnerKind kind) {
    switch (kind) {
    case OwnerKind::TU:
      return "TU";
    case OwnerKind::Include:
      return "Include";
    case OwnerKind::Unknown:
      return "Unknown";
    }
    llvm_unreachable("Invalid owner kind");
  }

  // Grant access to the specific formatter specialization
  template <typename T, typename Enable> friend struct llvm::format_provider;

  struct Owner {
    OwnerKind kind = OwnerKind::Unknown;
    std::optional<uint64_t> includeId; // non-nullopt only when kind == INCLUDE
    std::optional<uint64_t> condArmId; // nullable; non-nullopt when segment is
                                       // in a specific arm

    static Owner TU(std::optional<uint64_t> condArmId = std::nullopt) {
      Owner o;
      o.kind = OwnerKind::TU;
      o.condArmId = condArmId;
      return o;
    }
    static Owner Include(uint64_t includeId,
                         std::optional<uint64_t> condArmId = std::nullopt) {
      Owner o;
      o.kind = OwnerKind::Include;
      o.includeId = includeId;
      o.condArmId = condArmId;
      return o;
    }
    static Owner Unknown() { return Owner(); }
  };

  /// \brief Top-level buckets for the accepted-result proof lattice.
  ///
  /// Step 1 introduces these categories without changing refolding behavior.
  /// They let the engine describe what class of result was accepted without
  /// conflating that classification with candidate ranking or the legacy retry
  /// ladder.
  enum class AcceptedProofClass : uint8_t {
    Unknown,
    InvocationPreserving,
    InvocationRealization,
    IncludePreserving,
    IncludeRealization,
    TUAnchor,
    TUTextualEdit,
  };

  friend inline StringRef toString(AcceptedProofClass kind) {
    switch (kind) {
    case AcceptedProofClass::Unknown:
      return "Unknown";
    case AcceptedProofClass::InvocationPreserving:
      return "InvocationPreserving";
    case AcceptedProofClass::InvocationRealization:
      return "InvocationRealization";
    case AcceptedProofClass::IncludePreserving:
      return "IncludePreserving";
    case AcceptedProofClass::IncludeRealization:
      return "IncludeRealization";
    case AcceptedProofClass::TUAnchor:
      return "TUAnchor";
    case AcceptedProofClass::TUTextualEdit:
      return "TUTextualEdit";
    }
    return "Unknown";
  }

  /// \brief Whether an accepted result preserves original structure or emits
  /// a realized edited surface.
  enum class RealizationMode : uint8_t {
    Unknown,
    PreserveOriginalStructure,
    RealizeEditedSurface,
  };

  friend inline StringRef toString(RealizationMode mode) {
    switch (mode) {
    case RealizationMode::Unknown:
      return "Unknown";
    case RealizationMode::PreserveOriginalStructure:
      return "PreserveOriginalStructure";
    case RealizationMode::RealizeEditedSurface:
      return "RealizeEditedSurface";
    }
    return "Unknown";
  }

  /// \brief Ranking bucket for choosing among multiple valid candidates.
  ///
  /// Preference is tracked separately from proof validity so ordering policy
  /// remains explicit rather than being hidden in construction order.
  enum class SelectionPreference : uint8_t {
    Unknown,
    PreferStructurePreservation,
    PreferSurfaceRealization,
    PreferExactAnchoring,
  };

  friend inline StringRef toString(SelectionPreference preference) {
    switch (preference) {
    case SelectionPreference::Unknown:
      return "Unknown";
    case SelectionPreference::PreferStructurePreservation:
      return "PreferStructurePreservation";
    case SelectionPreference::PreferSurfaceRealization:
      return "PreferSurfaceRealization";
    case SelectionPreference::PreferExactAnchoring:
      return "PreferExactAnchoring";
    }
    return "Unknown";
  }

  /// \brief Surface realization choices kept distinct from proof metadata.
  enum class SurfaceDisposition : uint8_t {
    None,
    RealizeWholeCoverMacros,
    RealizeInlineTouchedIncludesFromB,
    RealizeMaterializedIncludeExpansion,
    RealizeTranslationUnitByteEdit,
    EmitEditedPreprocessedStream,
  };

  friend inline StringRef toString(SurfaceDisposition disposition) {
    switch (disposition) {
    case SurfaceDisposition::None:
      return "None";
    case SurfaceDisposition::RealizeWholeCoverMacros:
      return "RealizeWholeCoverMacros";
    case SurfaceDisposition::RealizeInlineTouchedIncludesFromB:
      return "RealizeInlineTouchedIncludesFromB";
    case SurfaceDisposition::RealizeMaterializedIncludeExpansion:
      return "RealizeMaterializedIncludeExpansion";
    case SurfaceDisposition::RealizeTranslationUnitByteEdit:
      return "RealizeTranslationUnitByteEdit";
    case SurfaceDisposition::EmitEditedPreprocessedStream:
      return "EmitEditedPreprocessedStream";
    }
    return "None";
  }

  /// \brief Inventory of the currently accepted execution paths.
  ///
  /// Step 2 does not change how the engine refolds code. Instead it names the
  /// concrete path that produced an accepted result today so we can map that
  /// path onto the future proof lattice one class at a time.
  enum class AcceptedPathKind : uint8_t {
    Unknown,
    MacroArgsOnlyStandard,
    MacroArgsOnlyPasteSingle,
    MacroArgsOnlyPasteMulti,
    MacroArgsOnlyPurePasteOnly,
    MacroArgsOnlyPairedPureInsertion,
    MacroDagSubtreeRoot,
    MacroCallChainSuffix,
    MacroCounterLiteral,
    MacroWholeCoverRealization,
    IncludePatchPendingMaterialization,
    IncludeDeleteReplaceMappedHeaderTokens,
    IncludeInsertSelectedConditionalBoundary,
    IncludeInsertChildBoundary,
    IncludeInsertRightNeighborPP,
    IncludeInsertLeftNeighborPP,
    IncludeInsertDeclBoundary,
    IncludeRealizationInlineFromB,
    IncludeMaterializedExpansion,
    TUExactSlotBoundary,
    TUProvableInsertionAnchor,
    TUByteSpanMappedEdit,
    TUByteSpanConservativeEdit,
    TerminalEmitEditedPreprocessedStream,
  };

  friend inline StringRef toString(AcceptedPathKind kind) {
    switch (kind) {
    case AcceptedPathKind::Unknown:
      return "Unknown";
    case AcceptedPathKind::MacroArgsOnlyStandard:
      return "MacroArgsOnlyStandard";
    case AcceptedPathKind::MacroArgsOnlyPasteSingle:
      return "MacroArgsOnlyPasteSingle";
    case AcceptedPathKind::MacroArgsOnlyPasteMulti:
      return "MacroArgsOnlyPasteMulti";
    case AcceptedPathKind::MacroArgsOnlyPurePasteOnly:
      return "MacroArgsOnlyPurePasteOnly";
    case AcceptedPathKind::MacroArgsOnlyPairedPureInsertion:
      return "MacroArgsOnlyPairedPureInsertion";
    case AcceptedPathKind::MacroDagSubtreeRoot:
      return "MacroDagSubtreeRoot";
    case AcceptedPathKind::MacroCallChainSuffix:
      return "MacroCallChainSuffix";
    case AcceptedPathKind::MacroCounterLiteral:
      return "MacroCounterLiteral";
    case AcceptedPathKind::MacroWholeCoverRealization:
      return "MacroWholeCoverRealization";
    case AcceptedPathKind::IncludePatchPendingMaterialization:
      return "IncludePatchPendingMaterialization";
    case AcceptedPathKind::IncludeDeleteReplaceMappedHeaderTokens:
      return "IncludeDeleteReplaceMappedHeaderTokens";
    case AcceptedPathKind::IncludeInsertSelectedConditionalBoundary:
      return "IncludeInsertSelectedConditionalBoundary";
    case AcceptedPathKind::IncludeInsertChildBoundary:
      return "IncludeInsertChildBoundary";
    case AcceptedPathKind::IncludeInsertRightNeighborPP:
      return "IncludeInsertRightNeighborPP";
    case AcceptedPathKind::IncludeInsertLeftNeighborPP:
      return "IncludeInsertLeftNeighborPP";
    case AcceptedPathKind::IncludeInsertDeclBoundary:
      return "IncludeInsertDeclBoundary";
    case AcceptedPathKind::IncludeRealizationInlineFromB:
      return "IncludeRealizationInlineFromB";
    case AcceptedPathKind::IncludeMaterializedExpansion:
      return "IncludeMaterializedExpansion";
    case AcceptedPathKind::TUExactSlotBoundary:
      return "TUExactSlotBoundary";
    case AcceptedPathKind::TUProvableInsertionAnchor:
      return "TUProvableInsertionAnchor";
    case AcceptedPathKind::TUByteSpanMappedEdit:
      return "TUByteSpanMappedEdit";
    case AcceptedPathKind::TUByteSpanConservativeEdit:
      return "TUByteSpanConservativeEdit";
    case AcceptedPathKind::TerminalEmitEditedPreprocessedStream:
      return "TerminalEmitEditedPreprocessedStream";
    }
    return "Unknown";
  }

  /// \brief How mature the current acceptance path is in the migration plan.
  enum class AcceptanceSupportKind : uint8_t {
    Unknown,
    ExplicitProofBacked,
    DeterministicButNotFirstClass,
    ExplicitOutOfDomainClass,
  };

  friend inline StringRef toString(AcceptanceSupportKind support) {
    switch (support) {
    case AcceptanceSupportKind::Unknown:
      return "Unknown";
    case AcceptanceSupportKind::ExplicitProofBacked:
      return "ExplicitProofBacked";
    case AcceptanceSupportKind::DeterministicButNotFirstClass:
      return "DeterministicButNotFirstClass";
    case AcceptanceSupportKind::ExplicitOutOfDomainClass:
      return "ExplicitOutOfDomainClass";
    }
    return "Unknown";
  }

  /// \brief Future proof-class placeholder targeted by a current path.
  enum class FutureProofTarget : uint8_t {
    Unknown,
    MacroStandardArgsOnly,
    MacroPasteSingle,
    MacroPasteMultiFixedAnchor,
    MacroPurePasteOnly,
    MacroPairedPureInsertion,
    MacroDagLift,
    MacroCallChainSuffixPreservation,
    MacroCounterStabilizationRealization,
    MacroRealizationWholeCover,
    IncludePatchByMappedHeaderTokens,
    IncludeConditionalArmCertifiedInsertion,
    IncludeInsertionByChildBoundary,
    IncludeInsertionByRightNeighborPP,
    IncludeInsertionByLeftNeighborPP,
    IncludeInsertionByDeclBoundary,
    IncludeRealizationCover,
    IncludeMaterializedExpansionRealization,
    TUExactSlotAnchor,
    TUProvableInsertionAnchor,
    TUByteSpanTextualEdit,
    EditedPreprocessedStreamFallback,
  };

  friend inline StringRef toString(FutureProofTarget target) {
    switch (target) {
    case FutureProofTarget::Unknown:
      return "Unknown";
    case FutureProofTarget::MacroStandardArgsOnly:
      return "MacroStandardArgsOnly";
    case FutureProofTarget::MacroPasteSingle:
      return "MacroPasteSingle";
    case FutureProofTarget::MacroPasteMultiFixedAnchor:
      return "MacroPasteMultiFixedAnchor";
    case FutureProofTarget::MacroPurePasteOnly:
      return "MacroPurePasteOnly";
    case FutureProofTarget::MacroPairedPureInsertion:
      return "MacroPairedPureInsertion";
    case FutureProofTarget::MacroDagLift:
      return "MacroDagLift";
    case FutureProofTarget::MacroCallChainSuffixPreservation:
      return "MacroCallChainSuffixPreservation";
    case FutureProofTarget::MacroCounterStabilizationRealization:
      return "MacroCounterStabilizationRealization";
    case FutureProofTarget::MacroRealizationWholeCover:
      return "MacroRealizationWholeCover";
    case FutureProofTarget::IncludePatchByMappedHeaderTokens:
      return "IncludePatchByMappedHeaderTokens";
    case FutureProofTarget::IncludeConditionalArmCertifiedInsertion:
      return "IncludeConditionalArmCertifiedInsertion";
    case FutureProofTarget::IncludeInsertionByChildBoundary:
      return "IncludeInsertionByChildBoundary";
    case FutureProofTarget::IncludeInsertionByRightNeighborPP:
      return "IncludeInsertionByRightNeighborPP";
    case FutureProofTarget::IncludeInsertionByLeftNeighborPP:
      return "IncludeInsertionByLeftNeighborPP";
    case FutureProofTarget::IncludeInsertionByDeclBoundary:
      return "IncludeInsertionByDeclBoundary";
    case FutureProofTarget::IncludeRealizationCover:
      return "IncludeRealizationCover";
    case FutureProofTarget::IncludeMaterializedExpansionRealization:
      return "IncludeMaterializedExpansionRealization";
    case FutureProofTarget::TUExactSlotAnchor:
      return "TUExactSlotAnchor";
    case FutureProofTarget::TUProvableInsertionAnchor:
      return "TUProvableInsertionAnchor";
    case FutureProofTarget::TUByteSpanTextualEdit:
      return "TUByteSpanTextualEdit";
    case FutureProofTarget::EditedPreprocessedStreamFallback:
      return "ExplicitOutOfDomainTerminalResult";
    }
    return "Unknown";
  }

  /// \brief Step-2 inventory record that maps a current acceptance path onto
  /// the future proof lattice.
  struct AcceptancePathInventory {
    AcceptedPathKind currentPath = AcceptedPathKind::Unknown;
    AcceptanceSupportKind support = AcceptanceSupportKind::Unknown;
    FutureProofTarget futureTarget = FutureProofTarget::Unknown;
  };

  /// \brief Conflict domain used by the global accepted-result lattice.
  ///
  /// Step 10 does not change how candidates are chosen. It names the owner
  /// domain in which two accepted artifacts may interact so the current global
  /// selection and overlap rules can be described explicitly and audited in one
  /// place.
  enum class LatticeConflictDomain : uint8_t {
    Unknown,
    MacroInvocationRootSpan,
    IncludeOwnerRegion,
    TUAnchorPoint,
    WholeTranslationUnit,
  };

  friend inline StringRef toString(LatticeConflictDomain domain) {
    switch (domain) {
    case LatticeConflictDomain::Unknown:
      return "Unknown";
    case LatticeConflictDomain::MacroInvocationRootSpan:
      return "MacroInvocationRootSpan";
    case LatticeConflictDomain::IncludeOwnerRegion:
      return "IncludeOwnerRegion";
    case LatticeConflictDomain::TUAnchorPoint:
      return "TUAnchorPoint";
    case LatticeConflictDomain::WholeTranslationUnit:
      return "WholeTranslationUnit";
    }
    return "Unknown";
  }

  /// \brief Merge law used when two artifacts in the same lattice domain are
  /// compatible.
  enum class LatticeMergeLaw : uint8_t {
    Unknown,
    DisjointCompose,
    NestedOuterShadowsInner,
    SelectSingleWitness,
    TerminalReplacesAll,
  };

  friend inline StringRef toString(LatticeMergeLaw law) {
    switch (law) {
    case LatticeMergeLaw::Unknown:
      return "Unknown";
    case LatticeMergeLaw::DisjointCompose:
      return "DisjointCompose";
    case LatticeMergeLaw::NestedOuterShadowsInner:
      return "NestedOuterShadowsInner";
    case LatticeMergeLaw::SelectSingleWitness:
      return "SelectSingleWitness";
    case LatticeMergeLaw::TerminalReplacesAll:
      return "TerminalReplacesAll";
    }
    return "Unknown";
  }

  /// \brief Conflict law used when two artifacts in the same lattice domain
  /// are not simultaneously admissible.
  enum class LatticeConflictLaw : uint8_t {
    Unknown,
    RejectPartialOverlap,
    PreferStructurePreservation,
    PreferExactAnchorWitness,
    PreferOwnerPreservingBeforeRealization,
    ExplicitOutOfDomainTerminalResult,
  };

  friend inline StringRef toString(LatticeConflictLaw law) {
    switch (law) {
    case LatticeConflictLaw::Unknown:
      return "Unknown";
    case LatticeConflictLaw::RejectPartialOverlap:
      return "RejectPartialOverlap";
    case LatticeConflictLaw::PreferStructurePreservation:
      return "PreferStructurePreservation";
    case LatticeConflictLaw::PreferExactAnchorWitness:
      return "PreferExactAnchorWitness";
    case LatticeConflictLaw::PreferOwnerPreservingBeforeRealization:
      return "PreferOwnerPreservingBeforeRealization";
    case LatticeConflictLaw::ExplicitOutOfDomainTerminalResult:
      return "ExplicitOutOfDomainTerminalResult";
    }
    return "Unknown";
  }

  /// \brief Normalized Step-10 description of the current global lattice law.
  struct GlobalSelectionLattice {
    LatticeConflictDomain domain = LatticeConflictDomain::Unknown;
    LatticeMergeLaw mergeLaw = LatticeMergeLaw::Unknown;
    LatticeConflictLaw conflictLaw = LatticeConflictLaw::Unknown;
  };

  /// \brief Whether an accepted path currently participates in the declared
  /// completeness set.
  ///
  /// Step 11 does not claim the engine is globally complete yet. Instead it
  /// makes the scope of the completeness claim explicit: accepted paths either
  /// already correspond to a declared proof class, remain transitional while a
  /// class is still being closed, or sit outside the declared class set
  /// entirely (for example an explicit terminal out-of-domain result).
  enum class CompletenessCoverageKind : uint8_t {
    Unknown,
    DeclaredProofClass,
    TransitionalGap,
    ExplicitOutOfDomainClass,
  };

  friend inline StringRef toString(CompletenessCoverageKind kind) {
    switch (kind) {
    case CompletenessCoverageKind::Unknown:
      return "Unknown";
    case CompletenessCoverageKind::DeclaredProofClass:
      return "DeclaredProofClass";
    case CompletenessCoverageKind::TransitionalGap:
      return "TransitionalGap";
    case CompletenessCoverageKind::ExplicitOutOfDomainClass:
      return "ExplicitOutOfDomainClass";
    }
    return "Unknown";
  }

  /// \brief What completeness promise the engine makes for a covered path.
  enum class CompletenessExpectationKind : uint8_t {
    Unknown,
    MustDiscoverDeclaredOrStrongerCompatible,
    NoClaimPendingClassClosure,
    ExplicitlyOutsideDeclaredSet,
  };

  friend inline StringRef toString(CompletenessExpectationKind kind) {
    switch (kind) {
    case CompletenessExpectationKind::Unknown:
      return "Unknown";
    case CompletenessExpectationKind::MustDiscoverDeclaredOrStrongerCompatible:
      return "MustDiscoverDeclaredOrStrongerCompatible";
    case CompletenessExpectationKind::NoClaimPendingClassClosure:
      return "NoClaimPendingClassClosure";
    case CompletenessExpectationKind::ExplicitlyOutsideDeclaredSet:
      return "ExplicitlyOutsideDeclaredSet";
    }
    return "Unknown";
  }

  /// \brief Normalized completeness contract for the declared domain above.
  ///
  /// This contract answers only one question: whether a theorem-facing carrier
  /// already lies in the declared proof-class set, remains an internal-only
  /// transitional gap that must not reach emission, or is explicitly outside
  /// the declared set as a named terminal boundary.
  struct CompletenessContract {
    CompletenessCoverageKind coverage = CompletenessCoverageKind::Unknown;
    CompletenessExpectationKind expectation =
        CompletenessExpectationKind::Unknown;
    FutureProofTarget declaredTarget = FutureProofTarget::Unknown;
    bool countsTowardDeclaredCoverage = false;
    bool hasExplicitExclusion = false;
    TerminalFallbackKind explicitExclusion = TerminalFallbackKind::Unknown;
  };

  /// \brief The summary's position relative to the declared theorem domain.
  ///
  /// This enum is the theorem-domain projection of the same declared-domain
  /// statement: theorem-facing results are either in-domain declared proof
  /// classes or explicit named out-of-domain classes. Transitional states may
  /// still exist internally, but they are not allowed to survive to emission.
  enum class TheoremDomainKind : uint8_t {
    Unknown,
    DeclaredInDomainClass,
    TransitionalGap,
    ExplicitOutOfDomainClass,
  };

  friend inline StringRef toString(TheoremDomainKind kind) {
    switch (kind) {
    case TheoremDomainKind::Unknown:
      return "Unknown";
    case TheoremDomainKind::DeclaredInDomainClass:
      return "DeclaredInDomainClass";
    case TheoremDomainKind::TransitionalGap:
      return "TransitionalGap";
    case TheoremDomainKind::ExplicitOutOfDomainClass:
      return "ExplicitOutOfDomainClass";
    }
    return "Unknown";
  }

  /// \brief Explicit theorem-domain contract derived from the same statement.
  ///
  /// The theorem-domain view must say the same thing as the completeness view:
  /// theorem-facing carriers are either in-domain declared proof classes or
  /// explicit out-of-domain classes. Transitional states may still exist
  /// internally, but step 3 requires them to be non-emitting staging objects.
  struct TheoremDomainContract {
    TheoremDomainKind kind = TheoremDomainKind::Unknown;
    bool inDeclaredDomain = false;
    bool countsTowardCompleteness = false;
    FutureProofTarget declaredTarget = FutureProofTarget::Unknown;
    bool hasExplicitExclusion = false;
    TerminalFallbackKind explicitExclusion = TerminalFallbackKind::Unknown;
  };

  /// \brief Evidence source used to justify an accepted TU anchor.
  ///
  /// Step 7 lifts the deterministic TU anchoring rules into explicit proof
  /// witnesses so accepted TU-owned insertions can explain which anchor source
  /// was used and which non-crossing facts were relied upon.
  enum class TUAnchorEvidenceKind : uint8_t {
    Unknown,
    ExactSlotBoundary,
    ArgLikeBegin,
    ImmediateRightNeighbor,
    ImmediateLeftNeighbor,
    CorroboratedRightNeighbor,
    CorroboratedLeftNeighbor,
  };

  /// \brief Compact Step-7 witness for an accepted TU anchor.
  struct TUAnchorWitness {
    TUAnchorEvidenceKind evidence = TUAnchorEvidenceKind::Unknown;
    bool hasPPGap = false;
    uint64_t ppGap = 0;
    bool hasTUByte = false;
    uint64_t tuByte = 0;

    // Exact structural slot anchor metadata.
    bool exactPPMatch = false;
    uint64_t slotId = 0;
    std::string slotKind;

    // Arg-like begin anchor metadata.
    uint64_t macroId = 0;

    // Neighbor-based anchor metadata.
    bool hasLeftNeighbor = false;
    uint64_t leftNeighborPP = 0;
    bool hasRightNeighbor = false;
    uint64_t rightNeighborPP = 0;

    // Ownership/non-crossing metadata for provable TU insertion anchors.
    bool outsideIncludeCoverage = false;
    bool ownerDepthStable = false;
  };

  /// \brief Evidence source used to justify an accepted include-preserving
  /// anchor or mapped include byte range.
  ///
  /// Step 8 lifts include-preserving materialization paths into explicit local
  /// witnesses so each accepted include patch can explain which deterministic
  /// anchoring or mapping rule was used.
  enum class IncludeAnchorEvidenceKind : uint8_t {
    Unknown,
    MappedHeaderTokens,
    SelectedConditionalBoundary,
    ChildBoundary,
    RightNeighborPP,
    LeftNeighborPP,
    DeclBoundary,
  };

  /// \brief Compact Step-8 witness for an accepted include-preserving path.
  struct IncludeAnchorWitness {
    IncludeAnchorEvidenceKind evidence = IncludeAnchorEvidenceKind::Unknown;

    // Concrete byte placement inside the edited header file. Inserts use a
    // single anchor byte; mapped delete/replace paths use an explicit byte
    // range.
    bool hasAnchorByte = false;
    uint64_t anchorByte = 0;
    bool hasByteRange = false;
    uint64_t startByte = 0;
    uint64_t endByte = 0;

    // PP-token provenance for mapped delete/replace paths and neighbor-based
    // insertion anchors.
    bool hasFirstPP = false;
    uint64_t firstPP = 0;
    bool hasLastPP = false;
    uint64_t lastPP = 0;
    bool hasNeighborPP = false;
    uint64_t neighborPP = 0;

    // Conditional, child-include, and declaration boundary metadata.
    bool hasCondArmId = false;
    uint64_t condArmId = 0;
    bool hasChildIncludeId = false;
    uint64_t childIncludeId = 0;
    bool hasDeclHeaderRange = false;
    uint64_t declHeaderB = 0;
    uint64_t declHeaderE = 0;
  };

  /// \brief Evidence source used to justify an accepted include realization.
  ///
  /// Step 4A makes the include-realization domain boundary explicit. An inline
  /// include realization is in-domain only when the include cover admits either
  /// the canonical A-cover -> B-envelope mapping or one deterministic
  /// consensus-rescue envelope recovered from the existing non-canonical
  /// projections. Any include realization that cannot produce one of those two
  /// witnesses remains outside the declared domain and must terminate via the
  /// explicit terminal fallback instead of manufacturing a weaker proof class.
  enum class IncludeRealizationEvidenceKind : uint8_t {
    Unknown,
    CanonicalBCoverEnvelope,
    ConsensusRescuedBCoverEnvelope,
  };

  /// \brief Compact Step-9 witness for an accepted include realization path.
  struct IncludeRealizationWitness {
    IncludeRealizationEvidenceKind evidence =
        IncludeRealizationEvidenceKind::Unknown;
    bool hasIncludeId = false;
    uint64_t includeId = 0;
    bool hasACover = false;
    uint64_t aCoverBegin = 0;
    uint64_t aCoverEnd = 0;
    bool hasBTokenEnvelope = false;
    uint64_t bTokBegin = 0;
    uint64_t bTokEnd = 0;
  };

  /// \brief Status produced when the engine evaluates a local proof contract.
  ///
  /// The engine records explicit local obligations for every normalized proof
  /// summary. Converted selector sites already use the discharge result as the
  /// participation gate, while the remaining construction paths still mirror
  /// their local facts into the same record so the theorem boundary stays
  /// explicit during migration.
  enum class ProofDischargeStatus : uint8_t {
    Unknown,
    PendingMaterialization,
    Discharged,
    Rejected,
  };

  friend inline StringRef toString(ProofDischargeStatus status) {
    switch (status) {
    case ProofDischargeStatus::Unknown:
      return "Unknown";
    case ProofDischargeStatus::PendingMaterialization:
      return "PendingMaterialization";
    case ProofDischargeStatus::Discharged:
      return "Discharged";
    case ProofDischargeStatus::Rejected:
      return "Rejected";
    }
    return "Unknown";
  }

  /// \brief Named local obligations used by Step 3 proof-discharge records.
  enum class ProofObligationKind : uint8_t {
    Unknown,
    AcceptedPathClassified,
    FutureTargetMapped,
    LegacyValidationRecorded,
    StructureMatchesAcceptedClass,
    ProofRootTracked,
    MacroProofRootResolved,
    MacroProofRootIsTopLevel,
    MacroPasteWitnessPresent,
    MacroPasteWitnessWellFormed,
    MacroPasteFreeSurfaceTracked,
    MacroSubtreeCertificateTracked,
    MacroCallChainWitnessTracked,
    SubtreeAdmissibilityTracked,
    WholeCoverBoundsTracked,
    WholeCoverContainmentTracked,
    WholeCoverBoundaryAccountingTracked,
    IncludePendingMaterializationClassified,
    IncludePatchShapeTracked,
    IncludeAnchorWitnessTracked,
    IncludeAnchorByteTracked,
    IncludeConditionalOwnershipTracked,
    IncludeMappedHeaderRangeTracked,
    IncludeMappedHeaderByteRangeTracked,
    IncludeSelectedConditionalBoundaryWitnessTracked,
    IncludeChildBoundaryWitnessTracked,
    IncludeRightNeighborWitnessTracked,
    IncludeLeftNeighborWitnessTracked,
    IncludeDeclBoundaryWitnessTracked,
    IncludeRealizationWitnessTracked,
    IncludeRealizationIncludeTracked,
    IncludeRealizationCoverTracked,
    IncludeRealizationBEnvelopeTracked,
    TUAnchorPathClassified,
    TUAnchorWitnessTracked,
    TUAnchorPPGapTracked,
    TUAnchorByteTracked,
    TUExactSlotWitnessTracked,
    TUProvableEvidenceTracked,
    TUOutsideIncludeCoverageTracked,
    TUOwnerDepthStableTracked,
    ExplicitOutOfDomainResultTracked,
  };

  friend inline StringRef toString(ProofObligationKind obligation) {
    switch (obligation) {
    case ProofObligationKind::Unknown:
      return "Unknown";
    case ProofObligationKind::AcceptedPathClassified:
      return "AcceptedPathClassified";
    case ProofObligationKind::FutureTargetMapped:
      return "FutureTargetMapped";
    case ProofObligationKind::LegacyValidationRecorded:
      return "LegacyValidationRecorded";
    case ProofObligationKind::StructureMatchesAcceptedClass:
      return "StructureMatchesAcceptedClass";
    case ProofObligationKind::ProofRootTracked:
      return "ProofRootTracked";
    case ProofObligationKind::MacroProofRootResolved:
      return "MacroProofRootResolved";
    case ProofObligationKind::MacroProofRootIsTopLevel:
      return "MacroProofRootIsTopLevel";
    case ProofObligationKind::MacroPasteWitnessPresent:
      return "MacroPasteWitnessPresent";
    case ProofObligationKind::MacroPasteWitnessWellFormed:
      return "MacroPasteWitnessWellFormed";
    case ProofObligationKind::MacroPasteFreeSurfaceTracked:
      return "MacroPasteFreeSurfaceTracked";
    case ProofObligationKind::MacroSubtreeCertificateTracked:
      return "MacroSubtreeCertificateTracked";
    case ProofObligationKind::MacroCallChainWitnessTracked:
      return "MacroCallChainWitnessTracked";
    case ProofObligationKind::SubtreeAdmissibilityTracked:
      return "SubtreeAdmissibilityTracked";
    case ProofObligationKind::WholeCoverBoundsTracked:
      return "WholeCoverBoundsTracked";
    case ProofObligationKind::WholeCoverContainmentTracked:
      return "WholeCoverContainmentTracked";
    case ProofObligationKind::WholeCoverBoundaryAccountingTracked:
      return "WholeCoverBoundaryAccountingTracked";
    case ProofObligationKind::IncludePendingMaterializationClassified:
      return "IncludePendingMaterializationClassified";
    case ProofObligationKind::IncludePatchShapeTracked:
      return "IncludePatchShapeTracked";
    case ProofObligationKind::IncludeAnchorWitnessTracked:
      return "IncludeAnchorWitnessTracked";
    case ProofObligationKind::IncludeAnchorByteTracked:
      return "IncludeAnchorByteTracked";
    case ProofObligationKind::IncludeConditionalOwnershipTracked:
      return "IncludeConditionalOwnershipTracked";
    case ProofObligationKind::IncludeMappedHeaderRangeTracked:
      return "IncludeMappedHeaderRangeTracked";
    case ProofObligationKind::IncludeMappedHeaderByteRangeTracked:
      return "IncludeMappedHeaderByteRangeTracked";
    case ProofObligationKind::IncludeSelectedConditionalBoundaryWitnessTracked:
      return "IncludeSelectedConditionalBoundaryWitnessTracked";
    case ProofObligationKind::IncludeChildBoundaryWitnessTracked:
      return "IncludeChildBoundaryWitnessTracked";
    case ProofObligationKind::IncludeRightNeighborWitnessTracked:
      return "IncludeRightNeighborWitnessTracked";
    case ProofObligationKind::IncludeLeftNeighborWitnessTracked:
      return "IncludeLeftNeighborWitnessTracked";
    case ProofObligationKind::IncludeDeclBoundaryWitnessTracked:
      return "IncludeDeclBoundaryWitnessTracked";
    case ProofObligationKind::IncludeRealizationWitnessTracked:
      return "IncludeRealizationWitnessTracked";
    case ProofObligationKind::IncludeRealizationIncludeTracked:
      return "IncludeRealizationIncludeTracked";
    case ProofObligationKind::IncludeRealizationCoverTracked:
      return "IncludeRealizationCoverTracked";
    case ProofObligationKind::IncludeRealizationBEnvelopeTracked:
      return "IncludeRealizationBEnvelopeTracked";
    case ProofObligationKind::TUAnchorPathClassified:
      return "TUAnchorPathClassified";
    case ProofObligationKind::TUAnchorWitnessTracked:
      return "TUAnchorWitnessTracked";
    case ProofObligationKind::TUAnchorPPGapTracked:
      return "TUAnchorPPGapTracked";
    case ProofObligationKind::TUAnchorByteTracked:
      return "TUAnchorByteTracked";
    case ProofObligationKind::TUExactSlotWitnessTracked:
      return "TUExactSlotWitnessTracked";
    case ProofObligationKind::TUProvableEvidenceTracked:
      return "TUProvableEvidenceTracked";
    case ProofObligationKind::TUOutsideIncludeCoverageTracked:
      return "TUOutsideIncludeCoverageTracked";
    case ProofObligationKind::TUOwnerDepthStableTracked:
      return "TUOwnerDepthStableTracked";
    case ProofObligationKind::ExplicitOutOfDomainResultTracked:
      return "ExplicitOutOfDomainResultTracked";
    }
    return "Unknown";
  }

  /// \brief Why a local proof contract could not be discharged.
  enum class ProofFailureReason : uint8_t {
    None,
    PendingMaterialization,
    MissingAcceptedPathClassification,
    MissingFutureTargetMapping,
    MissingLegacyValidation,
    StructuralMismatch,
    MissingProofRoot,
    MissingMacroProofRootResolution,
    NonTopLevelMacroProofRoot,
    MissingPasteWitness,
    MalformedPasteWitness,
    UnexpectedPasteSurface,
    MissingSubtreeCertificate,
    MissingCallChainWitness,
    MissingSubtreeAdmissibility,
    MissingWholeCoverBounds,
    MissingWholeCoverContainment,
    MissingWholeCoverBoundaryAccounting,
    MissingIncludePatchShape,
    MissingIncludeAnchorWitness,
    MissingIncludeAnchorByte,
    MissingConditionalOwnership,
    MissingMappedHeaderRange,
    MissingMappedHeaderByteRange,
    MissingIncludeSelectedConditionalBoundaryWitness,
    MissingIncludeChildBoundaryWitness,
    MissingIncludeRightNeighborWitness,
    MissingIncludeLeftNeighborWitness,
    MissingIncludeDeclBoundaryWitness,
    MissingIncludeRealizationWitness,
    MissingIncludeRealizationInclude,
    MissingIncludeRealizationCover,
    MissingIncludeRealizationBEnvelope,
    MissingTUAnchorClassification,
    MissingTUAnchorWitness,
    MissingTUAnchorGap,
    MissingTUAnchorByte,
    MissingTUExactSlotWitness,
    MissingTUProvableAnchorWitness,
    MissingTUOutsideIncludeCoverageProof,
    MissingTUOwnerDepthStability,
    ExplicitOutOfDomainResult,
  };

  friend inline StringRef toString(ProofFailureReason reason) {
    switch (reason) {
    case ProofFailureReason::None:
      return "None";
    case ProofFailureReason::PendingMaterialization:
      return "PendingMaterialization";
    case ProofFailureReason::MissingAcceptedPathClassification:
      return "MissingAcceptedPathClassification";
    case ProofFailureReason::MissingFutureTargetMapping:
      return "MissingFutureTargetMapping";
    case ProofFailureReason::MissingLegacyValidation:
      return "MissingLegacyValidation";
    case ProofFailureReason::StructuralMismatch:
      return "StructuralMismatch";
    case ProofFailureReason::MissingProofRoot:
      return "MissingProofRoot";
    case ProofFailureReason::MissingMacroProofRootResolution:
      return "MissingMacroProofRootResolution";
    case ProofFailureReason::NonTopLevelMacroProofRoot:
      return "NonTopLevelMacroProofRoot";
    case ProofFailureReason::MissingPasteWitness:
      return "MissingPasteWitness";
    case ProofFailureReason::MalformedPasteWitness:
      return "MalformedPasteWitness";
    case ProofFailureReason::UnexpectedPasteSurface:
      return "UnexpectedPasteSurface";
    case ProofFailureReason::MissingSubtreeCertificate:
      return "MissingSubtreeCertificate";
    case ProofFailureReason::MissingCallChainWitness:
      return "MissingCallChainWitness";
    case ProofFailureReason::MissingSubtreeAdmissibility:
      return "MissingSubtreeAdmissibility";
    case ProofFailureReason::MissingWholeCoverBounds:
      return "MissingWholeCoverBounds";
    case ProofFailureReason::MissingWholeCoverContainment:
      return "MissingWholeCoverContainment";
    case ProofFailureReason::MissingWholeCoverBoundaryAccounting:
      return "MissingWholeCoverBoundaryAccounting";
    case ProofFailureReason::MissingIncludePatchShape:
      return "MissingIncludePatchShape";
    case ProofFailureReason::MissingIncludeAnchorWitness:
      return "MissingIncludeAnchorWitness";
    case ProofFailureReason::MissingIncludeAnchorByte:
      return "MissingIncludeAnchorByte";
    case ProofFailureReason::MissingConditionalOwnership:
      return "MissingConditionalOwnership";
    case ProofFailureReason::MissingMappedHeaderRange:
      return "MissingMappedHeaderRange";
    case ProofFailureReason::MissingMappedHeaderByteRange:
      return "MissingMappedHeaderByteRange";
    case ProofFailureReason::MissingIncludeSelectedConditionalBoundaryWitness:
      return "MissingIncludeSelectedConditionalBoundaryWitness";
    case ProofFailureReason::MissingIncludeChildBoundaryWitness:
      return "MissingIncludeChildBoundaryWitness";
    case ProofFailureReason::MissingIncludeRightNeighborWitness:
      return "MissingIncludeRightNeighborWitness";
    case ProofFailureReason::MissingIncludeLeftNeighborWitness:
      return "MissingIncludeLeftNeighborWitness";
    case ProofFailureReason::MissingIncludeDeclBoundaryWitness:
      return "MissingIncludeDeclBoundaryWitness";
    case ProofFailureReason::MissingIncludeRealizationWitness:
      return "MissingIncludeRealizationWitness";
    case ProofFailureReason::MissingIncludeRealizationInclude:
      return "MissingIncludeRealizationInclude";
    case ProofFailureReason::MissingIncludeRealizationCover:
      return "MissingIncludeRealizationCover";
    case ProofFailureReason::MissingIncludeRealizationBEnvelope:
      return "MissingIncludeRealizationBEnvelope";
    case ProofFailureReason::MissingTUAnchorClassification:
      return "MissingTUAnchorClassification";
    case ProofFailureReason::MissingTUAnchorWitness:
      return "MissingTUAnchorWitness";
    case ProofFailureReason::MissingTUAnchorGap:
      return "MissingTUAnchorGap";
    case ProofFailureReason::MissingTUAnchorByte:
      return "MissingTUAnchorByte";
    case ProofFailureReason::MissingTUExactSlotWitness:
      return "MissingTUExactSlotWitness";
    case ProofFailureReason::MissingTUProvableAnchorWitness:
      return "MissingTUProvableAnchorWitness";
    case ProofFailureReason::MissingTUOutsideIncludeCoverageProof:
      return "MissingTUOutsideIncludeCoverageProof";
    case ProofFailureReason::MissingTUOwnerDepthStability:
      return "MissingTUOwnerDepthStability";
    case ProofFailureReason::ExplicitOutOfDomainResult:
      return "ExplicitOutOfDomainResult";
    }
    return "None";
  }

  /// \brief Compact record of Step 3 class-local obligation discharge.
  struct ProofDischargeRecord {
    ProofDischargeStatus status = ProofDischargeStatus::Unknown;
    ProofFailureReason failureReason = ProofFailureReason::None;
    ProofObligationKind failedObligation = ProofObligationKind::Unknown;
    uint16_t obligationsEvaluated = 0;
    uint16_t obligationsSatisfied = 0;
  };

  /// \brief Small Step-3 helper that accumulates class-local obligations.
  ///
  /// This remains a private implementation detail because later steps may
  /// replace the current local-discharge bookkeeping with even stronger proof
  /// objects. Keeping the accumulator nested here lets the out-of-line
  /// implementation reuse the normalized proof types without widening
  /// RefoldEngine's public API.
  struct ProofDischargeAccumulator;

  /// \brief Common proof-summary carrier used during the proof/lattice model.
  ///
  /// The summary packages the accepted-path inventory, local discharge result,
  /// lattice law, completeness contract, and explicit theorem-domain position
  /// for one accepted artifact. Converted selector sites already use the
  /// discharge result and lattice law operationally; the remaining migration
  /// work is to route every competition site through this same carrier.
  struct ProofSummary {
    AcceptedProofClass acceptedClass = AcceptedProofClass::Unknown;
    RealizationMode realizationMode = RealizationMode::Unknown;
    SelectionPreference preference = SelectionPreference::Unknown;
    SurfaceDisposition surfaceDisposition =
        SurfaceDisposition::None;
    AcceptancePathInventory inventory;
    GlobalSelectionLattice lattice;
    CompletenessContract completeness;
    TheoremDomainContract theoremDomain;
    ProofDischargeRecord discharge;
    bool validated = false;
    bool structurePreserving = false;
    uint64_t proofRootMacroId = 0;
    bool hasTUAnchorWitness = false;
    TUAnchorWitness tuAnchorWitness;
    bool hasIncludeAnchorWitness = false;
    IncludeAnchorWitness includeAnchorWitness;
    bool hasIncludeRealizationWitness = false;
    IncludeRealizationWitness includeRealizationWitness;
    bool hasTerminalFallbackWitness = false;
    TerminalFallbackWitness terminalFallbackWitness;
  };

  /// \brief Normalized accepted-result artifact kind used by Patch A/B.
  ///
  /// Patch A introduced the common carrier so already-accepted
  /// macro/include/TU/terminal results could be wrapped in one uniform shape.
  /// Patch B starts using that shape at selected competition sites, while the
  /// rest of the engine still accepts results through path-specific control
  /// flow until later selector-closure steps land.
  enum class AcceptedResultCandidateKind : uint8_t {
    Unknown,
    MacroPatch,
    IncludePatch,
    TUAnchor,
    TUTextEdit,
    TerminalOutOfDomain,
  };

  friend inline StringRef toString(AcceptedResultCandidateKind kind) {
    switch (kind) {
    case AcceptedResultCandidateKind::Unknown:
      return "Unknown";
    case AcceptedResultCandidateKind::MacroPatch:
      return "MacroPatch";
    case AcceptedResultCandidateKind::IncludePatch:
      return "IncludePatch";
    case AcceptedResultCandidateKind::TUAnchor:
      return "TUAnchor";
    case AcceptedResultCandidateKind::TUTextEdit:
      return "TUTextEdit";
    case AcceptedResultCandidateKind::TerminalOutOfDomain:
      return "TerminalOutOfDomain";
    }
    return "Unknown";
  }

  /// \brief Normalized wrapper for a concrete accepted result.
  ///
  /// The carrier stays intentionally small and explicit. It holds the
  /// normalized proof summary plus enough artifact-local provenance for the
  /// converted Patch-B selection sites to compare accepted outcomes through the
  /// lattice without rebuilding path-specific ordering logic.
  struct AcceptedResultCandidate {
    AcceptedResultCandidateKind kind = AcceptedResultCandidateKind::Unknown;
    ProofSummary proofSummary = {};

    // Artifact-local span / owner metadata.
    uint64_t begin = 0;
    uint64_t end = 0;
    bool hasOwnerIncludeId = false;
    uint64_t ownerIncludeId = 0;
    bool hasRootMacroId = false;
    uint64_t rootMacroId = 0;
    bool hasAnchorByte = false;
    uint64_t anchorByte = 0;

    // Human-readable preview of the selected surface. This is tracing-only and
    // never participates in admissibility or ordering.
    bool hasPayloadPreview = false;
    std::string payloadPreview;
  };


  enum class MacroPatchProofKind : uint8_t {
    Unknown,
    CounterLiteral,
    ArgsOnlyPasteMulti,
    ArgsOnlyPasteSingle,
    ArgsOnlyPurePasteOnly,
    ArgsOnlyStandard,
    ArgsOnlyPairedPureInsertion,
    DagSubtreeRoot,
    CallChainSuffix,
    WholeCoverRealization,
  };

  struct MacroPatch {
    uint64_t invStart = 0, invEnd = 0;
    std::string replacement;

    MacroPatch() = default;
    MacroPatch(uint64_t invStart, uint64_t invEnd, std::string replacement,
               uint64_t macroId = 0)
        : invStart(invStart), invEnd(invEnd),
          replacement(std::move(replacement)), macroId(macroId) {}

    // Canonical macro invocation id for this physical callsite patch. This is
    // used only for statistics attribution; the patch itself is still keyed by
    // byte span and owner.
    uint64_t macroId = 0;

    // Step-1/2 proof-lattice migration summary. This mirrors the legacy
    // fields below so later steps can reason about proof class, selection
    // preference, and current acceptance-path inventory without rewriting
    // macro-patch behavior yet. Default-initialize the normalized proof
    // summary so aggregate construction of MacroPatch remains warning-free.
    ProofSummary proofSummary = {};

    // Proof provenance for layer-3 root/callsite admissibility.
    MacroPatchProofKind proofKind = MacroPatchProofKind::Unknown;
    bool proofValidated = false;
    bool structurePreserving = false;
    uint64_t proofRootMacroId = 0;

    // Step-4 first-class macro whole-cover realization certificate. These
    // fields record the exact owner cover, containment witness, and B-side
    // token-envelope accounting used to justify realized whole-cover output.
    bool wholeCoverUsedBodyRange = false;
    bool wholeCoverSelfContained = false;
    bool wholeCoverNestedSelfContained = false;
    bool wholeCoverAdjustedLeft = false;
    bool wholeCoverAdjustedRight = false;
    bool wholeCoverClaimsClipped = false;
    uint64_t wholeCoverALo = 0;
    uint64_t wholeCoverAHi = 0;
    uint64_t wholeCoverBRawLo = 0;
    uint64_t wholeCoverBRawHi = 0;
    uint64_t wholeCoverBAdjLo = 0;
    uint64_t wholeCoverBAdjHi = 0;

    // Layer-5 subtree-composition audit metadata. This is instrumentation only
    // and does not participate in admissibility yet.
    bool subtreeCertBacked = false;
    uint64_t subtreeLeafMacroId = 0;
    uint32_t subtreeWitnessCount = 0;
    uint32_t subtreeInvocationCertCount = 0;
    uint32_t subtreeFormalCertCount = 0;
    uint32_t subtreeArgCertCount = 0;
    uint32_t subtreeLiftChainCount = 0;
    uint32_t subtreeLiftStepCount = 0;
    uint32_t subtreeRootMergeCount = 0;
    bool subtreeUsesLexicalBridge = false;
    bool subtreeTouchesPaste = false;
    bool subtreeHasWrapperSemantics = false;
    bool subtreeHasStringifySemantics = false;
    bool subtreeHasWideStringifySemantics = false;
    bool subtreeHasPreferredChildSyntax = false;
    bool subtreeHasRawInvocationPreservation = false;
    bool subtreeHasPassthroughFlatten = false;
    bool subtreeHasBridgeSensitiveStructuredSemantics = false;
    bool subtreeDeferredPasteDischarged = false;
    bool subtreeAdmissible = false;
    uint32_t subtreeExpectedRootFormalCount = 0;
    uint32_t subtreeDeferredRootArgCount = 0;
    uint32_t subtreeBridgeSensitiveFormalCount = 0;
    std::string subtreeExpectedRootFormalSummary;
    std::string subtreeDeferredRootArgSummary;
    std::string subtreeBridgeSensitiveFormalSummary;

    // Direct args-only paste replay proof metadata.
    //
    // Some accepted paste-preserving args-only patches are justified directly
    // from the producer-side root `paste_tokens` witness stream. Others are
    // justified by the engine's own replay check, which re-applies the
    // rewritten invocation arguments and proves that every pasted token
    // occurrence reconstructs the edited B surface exactly. Record the latter
    // case on the patch so Patch C can treat that deterministic replay as an
    // authoritative proof source at the converted selector sites.
    bool pasteReplayValidated = false;

    // Layer-6 mixed-owner decomposition certificate metadata.
    bool ownerCertPresent = false;
    bool ownerMixedWitness = false;
    uint8_t ownerKindCode = 0; // 0=unknown, 1=TU, 2=Include
    uint64_t ownerIncludeIdCert = 0;
    bool ownerHasCondArmCert = false;
    uint64_t ownerCondArmIdCert = 0;
    uint32_t ownerWitnessCount = 0;
  };

  /// \brief Deterministic whole-cover realization plan for one invocation.
  ///
  /// ComputeWholeCoverPlan() proves the exact A/B token envelope that a
  /// whole-cover realization may use. Step 4 promotes the accepted whole-cover
  /// result into a first-class macro realization proof by copying this plan
  /// onto the accepted MacroPatch.
  struct WholeCoverPlan {
    uint64_t covLoA = 0;
    uint64_t covHiA = 0;
    bool usedBodyRange = false;
    bool selfContained = false;
    bool nestedSelfContained = false;
    size_t rawBTokStart = 0;
    size_t rawBTokEnd = 0;
    size_t bTokStart = 0;
    size_t bTokEnd = 0;
    bool adjustedLeft = false;
    bool adjustedRight = false;
    bool claimsClipped = false;
    std::string clippedText;
  };

  struct IncludePatch {
    const RefoldModel::IncludeItem *include;
    std::string insertBytes; // exact B bytes
    uint64_t aStart, aEnd;   // A-token interval inside include expansion
    uint64_t bStart, bEnd;   // B-token interval

    // Internal working summary for include-owned patch candidates.
    // Include patches are created before materialization chooses a concrete
    // preserving anchor or realization envelope, so pre-materialization
    // patches must not claim a normalized accepted path yet. The patch-level
    // summary therefore stays internal-only until materialization restamps the
    // emitted accepted result onto an explicit witness-backed preserving or
    // realization class.
    ProofSummary proofSummary = {};

    /// When present, this insertion was classified as belonging to a specific
    /// selected conditional arm inside the owning include. Include application
    /// must preserve that ownership and never anchor the insertion outside the
    /// certified arm body.
    bool ownerHasCondArmCert = false;
    uint64_t ownerCondArmIdCert = 0;

    std::string ToString() const {
      // 1. Determine the path (Using StringRef to avoid extra copies)
      StringRef path;
      if (include->resolvedPath && !include->resolvedPath->empty()) {
        path = *include->resolvedPath;
      } else {
        path = stringutils::stripHeaderToken(include->target);
      }

      // 2. Handle the preview truncation
      StringRef preview = insertBytes;
      bool truncated = false;
      if (preview.size() > 80) {
        preview = preview.take_front(80);
        truncated = true;
      }

      // 3. Escape whitespace
      std::string escapedPreview = stringutils::escape(preview);

      return formatv(
                 "IncludePatch{{incId={0}, path={1}, A=[{2},{3}), B=[{4},{5}), "
                 "condArm={6}, insert='{7}{8}'}",
                 include->id, path, aStart, aEnd, bStart, bEnd,
                 ownerHasCondArmCert
                     ? std::to_string(ownerCondArmIdCert)
                     : std::string("(none)"),
                 escapedPreview, (truncated ? "..." : ""))
          .str();
    }
  };

  struct IncludeEdits {
    const RefoldModel::IncludeItem *include;
    std::vector<IncludePatch> patches;

    explicit IncludeEdits(const RefoldModel::IncludeItem *item)
        : include(item) {}

    void Add(IncludePatch &&P) { patches.push_back(std::move(P)); }
  };

  struct ByteHunk {
    uint64_t aStart, aEnd;
    uint64_t bStart, bEnd;

    ByteHunk(uint64_t aStart, uint64_t aEnd, uint64_t bStart, uint64_t bEnd)
      : aStart(aStart), aEnd(aEnd), bStart(bStart), bEnd(bEnd) {}
  };

  // ------------------------------- Core Helpers ------------------------------

  /// \brief Maps preprocessor tokens into a stable lexeme sequence suitable for
  /// diff/LCS alignment.
  ///
  /// This method produces the token sequence consumed by the LCS/Myers
  /// pipeline. For most tokens, the emitted element is simply PPTok::spelling.
  /// For ASCII-whitespace tokens, the emitted element is replaced with a
  /// position-tied sentinel of the form "WS@<offset>".
  ///
  /// The whitespace sentinel prevents the aligner from treating arbitrary
  /// whitespace runs as interchangeable anchors. By tying whitespace lexemes to
  /// their A-side byte position, we ensure whitespace cannot "float" to other
  /// whitespace elsewhere in the file and distort the alignment, which is
  /// especially important around include/conditional boundaries and for
  /// pure insertions.
  ///
  /// \param toks The token list (typically PP tokens) whose spellings form the
  ///             baseline sequence.
  /// \param offs The start-byte offset (in the corresponding source text) for
  ///             each token in \p Toks; must be the same size as \p Toks and
  ///             index-aligned.
  /// \returns A list of lexeme strings of size Toks.size(), containing either
  ///          the original token spelling or a position-tied whitespace sentinel.
  static std::vector<StringRef> MapLexemes(ArrayRef<PPTok> toks,
                                           ArrayRef<size_t> offs);

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

  /// \brief Computes an "owner depth gap" array used to bias the weighted LCS
  /// anchoring for insertions.
  ///
  /// The refolder frequently chooses insertion positions by aligning PP token
  /// sequences. For insertions (gaps between tokens), we want to prefer
  /// positions that remain inside the correct *owner* region (nested includes
  /// and nested #if/#else arms), rather than being attracted to shallower
  /// boundaries. This method precomputes a depth-like metric per PP gap that
  /// can be used as a weighting term in the LCS cost function.
  ///
  /// **Definition:** for each PP gap `k` (between PP tokens `k-1` and `k`), we
  /// compute:
  ///
  ///     ownerDepthGap[k] = includeDepthLCA(leftInc, rightInc) + condDepthGap
  ///
  /// where:
  /// * **leftInc/rightInc** are the innermost include instance ids owning the
  ///   left and right side of the gap (TU is treated as depth 0).
  /// * **includeDepthLCA** is the include-nesting depth of the least common
  ///   ancestor include of the two sides (e.g. 0 for TU, 1 for direct TU
  ///   children, etc.).
  /// * **condDepthGap** is derived from the conditional-arm ownership of the
  ///   two sides. It is computed as the minimum of the two arm depths so that
  ///   a gap straddling a boundary does not receive an artificially deep score.
  ///
  /// **Indexing:** `numOfAOffs` is (#tokens + 1) because it represents PP
  /// offsets including the sentinel gap after the last token. This method
  /// returns an array sized `N + 1` where `N = numOfAOffs - 1` is the number of
  /// tokens, and indices `k in [0..N]` correspond to the `N+1` possible gaps.
  ///
  /// **Behavioral intent:** larger values indicate "deeper" nesting (more
  /// specific ownership). When used as a bias, insertions are encouraged to
  /// remain within the correct nested include/conditional region instead of
  /// drifting outward to more global boundaries.
  ///
  /// \param numOfAOffs number of PP offsets for the A-stream (#tokens + 1
  ///        including sentinel)
  /// \return an array `ownerDepthGap` indexed by PP gap `k` in `[0..N]`
  std::vector<uint32_t> ComputeOwnerDepthGapsForPP();

  /// \brief Classifies the logical "owner" of a diff hunk using the precomputed
  /// segment map for the translation unit.
  ///
  /// A hunk is given in A-side token coordinates [\p a0, \p a1) and describes
  /// how a region of the preprocessed stream was edited. This method projects
  /// the hunk into TU byte space, finds the smallest matching Segment,
  /// and returns an Owner describing whether the hunk belongs to:
  ///   - the TU file itself (kind = Owner::Kind::TU),
  ///   - a particular include (kind = Owner::Kind::INCLUDE}),
  ///   - a specific conditional arm inside an include
  ///     (kind = Owner::Kind::COND_ARM), or
  ///   - a macro expansion or unknown/ambiguous region.
  ///
  /// \headername Algorithm overview
  /// 1. Use tuByteSpan() to map the hunk's A-side token interval [\p a0, \p a1)
  ///    into a TU byte span [b, e) for the given \p tuPath. Even when the hunk
  ///    ultimately belongs to a header, segments for includes and conditional
  ///    arms are defined in TU byte space via include slots, so this step
  ///    is always performed.
  /// 2. Choose a probe byte offset inside [b, e):
  ///      - for non-empty hunks (\p a0 != \p a1), use the midpoint (b + e) / 2,
  ///      - for pure INSERT hunks (\p a0 == \p a1), use the left edge b so that
  ///        insertions at boundaries can be attached to the appropriate
  ///        "parent" segment.
  /// 3. Collect all Segment objects for the TU whose byte ranges intersect
  ///    [b, e). Sort the hits by segment length and apply additional rules for
  ///    pure INSERT hunks to handle boundary cases (for example, treating an
  ///    insertion at the end of a segment as belonging to that segment).
  /// 4. Map the selected segment to an Owner:
  ///      - if ownerIncludeId is empty, the hunk is classified as a TU edit,
  ///      - otherwise it is classified as an include or conditional-arm edit
  ///        based on ownerIncludeId and ownerCondArmId.
  /// 5. If no segment intersects the TU span, or the hunk falls into a
  ///    macro-expansion / slot boundary that cannot be cleanly attributed to a
  ///    single segment, the owner is returned as Owner::Kind::UNKNOWN. Callers
  ///    must then either (a) expand to a larger structural edit (e.g. realize
  ///    an include/conditional arm) or (b) reject the hunk as not safely
  ///    attributable.
  ///
  /// \param tuPath  absolute path of the TU file currently being refolded.
  /// \param h       the hunk to classify (in A-side token coordinates).
  /// \param aTokOff optional mapping from A-side token indices to TU byte
  ///                offsets; currently unused but reserved for future
  ///                refinements of segment selection.
  /// \returns An Owner describing which entity (TU, include, conditional arm,
  ///          macro, unknown) the hunk logically belongs to.
  Owner ClassifyOwnerWithSegments(StringRef tuPath,
                                  const diffutils::Hunk &h) const;

  /// \brief Returns \c true if the given macro invocation is lexically
  /// contained within a \c #define directive in the same source file.
  ///
  /// This is a safety/eligibility guard used by args-only macro patching. If an
  /// invocation occurs inside the spelling of a macro definition (i.e., within
  /// the byte range of a \c #define directive), treating it as a normal
  /// call-site can produce incorrect or destabilizing rewrites (e.g.,
  /// attempting to patch an invocation that is part of the macro's replacement
  /// list, creating self-referential edits, or breaking idempotence under
  /// re-preprocessing).
  ///
  /// The implementation relies on the producer-emitted directive list in the
  /// \c RefoldModel. It scans \c MacroDirective entries with
  /// \c subkind == "#define" in the same file as the invocation and checks
  /// whether the invocation's byte range \c [m.invB, m.invE) lies fully within
  /// the directive's site byte range \c [d.siteB, d.siteE).
  ///
  /// Note that this is a purely lexical containment test. It does not interpret
  /// macro semantics or nested directive structure; it assumes directive site
  /// ranges are accurate and non-overlapping for a given file.
  ///
  /// \param m Macro invocation metadata (file path and invocation byte range).
  /// \returns \c true if the invocation is fully contained within a \c #define
  ///          directive's site range in the same file; otherwise \c false.
  bool IsInvocationInsideDefineDirective(
      const RefoldModel::MacroInvocation &m) const;

  /// \brief Returns the innermost (smallest-width) patchable macro invocation
  /// that fully covers a given A-span.
  ///
  /// This is identical in spirit to smallestCoveringMacro (choose the smallest
  /// invocation whose preprocessed-token cover interval [coverBegin, coverEnd)
  /// contains [aStart, aEnd)), but with an additional constraint: the selected
  /// macro must have a valid invocation-site span in the original source (i.e.,
  /// inv_b/inv_e and inv_text are present).
  ///
  /// Why this matters: many object-like expansions that appear inside other
  /// macro bodies—especially predefined/builtin macros such as __FILE__ and
  /// __LINE__— may not carry their own patchable invocation-site metadata in
  /// the refold map. When an edit targets tokens produced by such an inner
  /// expansion, the refolder must attribute the edit to the nearest enclosing
  /// macro call that can be rewritten at its call site; this method performs
  /// that "bubble up to a patchable call site" selection deterministically by
  /// choosing the smallest enclosing patchable invocation.
  ///
  /// This is used during hunk classification to decide whether an edit should
  /// be realized as a macro call-site patch (whole-cover or argument patch)
  /// rather than as a TU/include byte patch.
  ///
  /// \param aStart  Inclusive start PP-token index in A.
  /// \param aEnd    Exclusive end PP-token index in A.
  /// \return        The smallest covering patchable macro invocation, or
  ///                nullptr if none cover the span.
  const RefoldModel::MacroInvocation *SmallestCoveringPatchableMacro(
      uint64_t aStart, uint64_t aEnd,
      std::optional<uint64_t> ownerIncludeId = std::nullopt) const;

  /// \brief Determine whether an A-token interval is owned by the translation
  ///        unit (TU).
  ///
  /// Non-empty ranges are TU-owned only when every mapped PP entry in
  /// ` [a0, a1) ` resolves to `tuPath`. Pure insertions (`a0 == a1`) are
  /// TU-owned only when the engine can derive a truthful TU insertion anchor at
  /// that exact PP gap via FindProvableTUInsertionAnchor().
  ///
  /// This function is intentionally fail-closed: any concrete mapping to a
  /// non-TU file, or any empty gap without a provable TU anchor, is treated as
  /// non-TU. This prevents header/include edits from being realized as TU byte
  /// edits.
  ///
  /// \param a0      Inclusive start preprocessed-token index in A.
  /// \param a1      Exclusive end preprocessed-token index in A.
  /// \param tuPath  Absolute canonical path of the TU’s source file.
  /// \returns       `true` if the interval is TU-owned; `false` otherwise.
  bool HunkMapsToTU(uint64_t a0, uint64_t a1, StringRef tuPath) const;

  /// \brief Return a conservative TU byte anchor for a pure insertion at PP
  ///        gap \p pp.
  ///
  /// Determines whether the empty A-side hunk at preprocessing-output gap
  /// \p pp has a \em provable insertion point in the translation unit identified
  /// by \p tuPath. This proof is used for two purposes: deciding whether the
  /// pure insertion is truthfully TU-owned, and materializing the corresponding
  /// zero-width TU span in byte space.
  ///
  /// The check is intentionally fail-closed. It accepts only:
  ///   - exact structural slot anchors recorded by the producer,
  ///   - exact TU-side macro "arg-like begin" anchors for wrapper/deferred
  ///     expansion shapes,
  ///   - immediate mapped TU neighbors when the gap is outside include
  ///     coverage, or
  ///   - in non-strict mode, a bounded whitespace probe whose nearest mapped
  ///     neighbors on both sides agree on TU ownership without crossing an
  ///     owner-depth boundary.
  ///
  /// \param pp The PP-gap index in A-space for the pure insertion being
  ///        classified or materialized.
  /// \param tuPath The canonical path of the translation unit whose byte space
  ///        is being queried for a truthful insertion anchor.
  ///
  /// \returns The TU byte offset of the zero-width insertion anchor when a
  ///          truthful TU proof succeeds; otherwise \c std::nullopt.
  std::optional<uint64_t> FindProvableTUInsertionAnchor(
      uint64_t pp, StringRef tuPath,
      TUAnchorWitness *witness = nullptr) const;

  /// Anchors a *pure insertion* (a PP-gap insertion) to a deterministic,
  /// canonical TU byte boundary representing the *same* preprocessed
  /// coordinate, when possible.
  ///
  /// A PP-gap `ppGap` is a boundary between two adjacent PP tokens (i.e. a
  /// "gap" index). For an insertion that conceptually occurs at that PP
  /// boundary, this method returns the TU byte offset of an **exact**
  /// structural boundary corresponding to that same PP coordinate.
  ///
  /// **Key property:** this method performs *no* "nearest" snapping. If `ppGap`
  /// does not exactly match a known boundary PP coordinate, it returns `std::nullopt`
  /// so callers can fall back to neighbor-based span anchoring. This avoids
  /// regressions where an insertion belonging inside a nested owner
  /// (include/arm) is incorrectly pulled out to a shallower boundary.
  ///
  /// **Boundary sources considered** (each producing a candidate `(pp,b)`
  /// pair):
  /// * **Explicit TU slots** with an emitted `pp` coordinate and a conservative
  ///   "boundary-like" `kind` (file/arm/include boundaries).
  ///
  /// **Directive-line newline adjustment:** some recorded boundary slots may
  /// point at the newline that terminates a preprocessor directive line (e.g.
  /// after `#include`, `#else`, `#endif`). For *insertions* at those
  /// boundaries, anchoring at the newline byte can cause directive
  /// concatenation (e.g. `...;#else`). To preserve directive line integrity,
  /// candidates for selected `kind`s are adjusted to anchor *after* the newline
  /// (handling `\n` and `\r\n`).
  ///
  /// **Exact-match requirement:** candidates are filtered to those whose PP
  /// coordinate equals `ppGap` exactly. If none match, returns `std::nullopt`.
  ///
  /// **Deterministic tie-breaking:** if multiple candidates share the same PP
  /// coordinate, the chosen candidate is the one with the highest priority by
  /// `kind`, then the smallest TU byte offset, then the smallest slot id. This
  /// ensures stable output across runs.
  ///
  /// \param tuPath the TU path whose slots/owners are being consulted (must
  ///               match model file keys)
  /// \param ppGap  the PP gap index (between PP tokens) representing the
  ///               desired insertion coordinate
  /// \returns the TU byte offset of an exact canonical boundary matching
  ///          `ppGap`, or `std::nullopt` if `ppGap` is not exactly on a known boundary
  ///          (caller should fall back)
  std::optional<uint64_t> AnchorToExactSlotBoundaryFromPPGap(
      StringRef tuPath, uint64_t ppGap,
      TUAnchorWitness *witness = nullptr) const;

  /// \brief Finds the ID of the narrowest include range that covers a given PP
  /// index.
  ///
  /// Iterates through all include items in the provided model to find the one
  /// whose range [begin, end) contains the specified preprocessor index \p pp.
  /// If multiple includes cover the index, the one with the smallest span
  /// (shortest length) is selected to ensure the most specific match.
  ///
  /// \param pp  The preprocessor index to search for within the covers.
  ///
  /// \returns The ID of the most specific include item if a match is found;
  ///          otherwise, std::nullopt.
  std::optional<uint64_t> IncludeIdCoveringPPIndex(uint64_t pp) const {
    std::optional<uint64_t> bestId;
    uint64_t bestLen = std::numeric_limits<uint64_t>::max();

    for (const RefoldModel::IncludeItem &inc : model_.GetIncludes()) {
      if (pp >= inc.cover.begin && pp < inc.cover.end) {
        uint64_t len = inc.cover.end - inc.cover.begin;
        if (len < bestLen) {
          bestLen = len;
          bestId = inc.id;
        }
      }
    }

    return bestId;
  }

  /// \brief Compute the TU (translation unit) byte span \c [b,e) that corresponds
  /// to an A-side PP-token interval \c [a0,a1).
  ///
  /// This routine converts a diff hunk expressed in A-token indices into a concrete
  /// byte range in the TU source file. The contract is intentionally conservative:
  /// if the interval cannot be proven to touch the TU (or cannot be safely anchored
  /// into the TU for a pure insertion), the method returns \c std::nullopt rather
  /// than "snapping" across ownership boundaries.
  ///
  /// \par Behavior
  /// 1. **Normalize indices:** if \p a0 > \p a1 the bounds are swapped.
  /// 2. **Pure insertion fast-path:** if \p a0 == \p a1, first try to anchor on an
  ///    *exact* canonical TU slot boundary recorded at the same PP gap via
  ///    AnchorToExactSlotBoundaryFromPPGap(). If present, returns \c {b,b}.
  /// 3. **Direct TU coverage:** for non-empty intervals, scan PP indices in
  ///    \c [a0,a1) and consider only tokmap entries whose \c TokMapEntry::file equals
  ///    \p tuPath. If any exist, returns the minimal enclosing TU byte span
  ///    \c [min(ent.b), max(ent.e)).
  /// 4. **No TU-mapped tokens:**
  ///    - If \p a0 != \p a1 (non-empty interval) and no TU tokens were found,
  ///      returns \c std::nullopt.
  ///    - If \p a0 == \p a1 (pure insertion at PP gap \c pp = a0), defer to
  ///      FindProvableTUInsertionAnchor(). If it succeeds, return \c {b,b};
  ///      otherwise return \c std::nullopt.
  ///
  /// A returned span \c {b,b} denotes a concrete insertion anchor point in the TU.
  ///
  /// \param a0 Inclusive start A-side PP-token index.
  /// \param a1 Exclusive end A-side PP-token index.
  /// \param tuPath Absolute/canonical TU path (must match \c TokMapEntry::file).
  /// \returns A TU byte span \c [b,e) (or \c {b,b} for a pure insertion anchor),
  ///          or \c std::nullopt if no TU span/anchor can be derived safely.
  std::optional<std::pair<uint64_t, uint64_t>>
  TUByteSpan(uint64_t a0, uint64_t a1, StringRef tuPath) const;

  /// \brief Determines whether an insertion hunk lands exactly on an include PP
  /// boundary and, if so, returns the include that should own the boundary
  /// insertion.
  ///
  /// This helper is used to conservatively assign ownership for edits that
  /// occur at an insertion point in the A-side PP token stream. The intent is
  /// to detect insertions that are *exactly* between sibling include regions
  /// and to attribute the insertion to their parent include, rather than
  /// incorrectly placing it inside one of the adjacent children.
  ///
  /// \par Applicability
  /// This policy is only applicable when the hunk has an empty A-span (\c
  /// h.aStart == h.aEnd). (Some diff producers may also guarantee \c h.bStart <
  /// h.bEnd for insertions; this function intentionally does not rely on that
  /// invariant.)
  ///
  /// \par Deterministic boundary-only policy
  /// To avoid heuristic ownership mistakes, this routine does \b not probe for
  /// “nearest” tokmap entries and does \b not snap to nearby PP tokens.
  /// Instead, it only considers includes whose A-domain PP cover interval \c
  /// [coverBegin, coverEnd) touches the insertion position exactly:
  ///
  /// - An include is considered immediately to the left if \c coverEnd == aPos.
  /// - An include is considered immediately to the right if \c coverBegin ==
  ///   aPos.
  /// - If multiple includes touch the boundary, the smallest-width include
  ///   (most-nested) is chosen independently on each side.
  ///
  /// The selected left/right include ids (or TU if absent) are then used to
  /// compute the least common ancestor via \c
  /// RefoldModel::LeastCommonAncestorInclude(leftId, rightId).
  ///
  /// \par Outcome
  /// - If both sides are TU (no exact boundary touch), return nullptr (policy
  ///   does not apply).
  /// - If the least common ancestor is absent, return nullptr (boundary meets
  ///   at TU).
  /// - Otherwise, return the ancestor \c IncludeItem, causing the caller to
  ///   treat the insertion as owned by that include level (i.e., between
  ///   sibling includes at the parent scope).
  ///
  /// \param h The diff hunk; only hunks with \c h.aStart == h.aEnd are
  ///          considered.
  /// \return The include that should own the boundary insertion, or nullptr
  ///         if the boundary policy does not apply.
  const RefoldModel::IncludeItem *
  BoundaryParentIncludeForPureInsertion(const diffutils::Hunk &h) const;

  /// \brief Build a fully specified IncludePatch for a pure INSERT hunk,
  ///        carrying the exact bytes from B.
  ///
  /// Constructs an `IncludePatch` that records the A-token interval
  /// (`h.aStart..h.aEnd`), the B-token interval (`h.bStart..h.bEnd`),
  /// and the literal bytes from **B** computed via `bTokOff_`. The slice is
  /// half-open in token space and byte-accurate, making it suitable for
  /// deterministic, line-local application.
  ///
  /// \param inc       The include item that owns this hunk.
  /// \param h         The diff hunk describing the A/B token intervals.
  /// \returns         A new `IncludePatch` containing the exact insertion
  ///                  payload from **B**.
  IncludePatch BuildIncludeInsertionPatch(const RefoldModel::IncludeItem &inc,
                                          const diffutils::Hunk &h) const;

  /// \brief Checks whether an args-only rewrite of a single macro parameter is
  /// consistent with all **observable** occurrences of that parameter in the
  /// edited preprocessed stream (B), for the given `MacroInvocation`.
  ///
  /// This is the primary safety gate used by args-only macro refolding. It
  /// validates the proposed replacement against:
  /// * STANDARD argument occurrences (non-paste expansions) in B
  /// * STRINGIFY occurrences in B (only when `strict` mode is enabled)
  /// * PASTE occurrences in B when they can be mapped soundly near the edit
  ///   site
  ///
  /// Paste-span validation can be sensitive to local A→B mapping quality, so it
  /// is intentionally conservative: if paste verification cannot be performed
  /// safely, the method prefers to return `true` (do not block args-only)
  /// unless an actual contradiction is detected.
  ///
  /// This wrapper enables paste-span validation (when applicable). For callers
  /// that validate paste-token correctness as a group (e.g., multi-span paste
  /// edits), see `MacroArgReplacementMatchesAllOccurrencesInBIgnorePaste`.
  ///
  /// \param m macro invocation being patched
  /// \param argIdx zero-based macro parameter index being rewritten
  /// \param baseArg original argument spelling in the invocation (used to
  ///                classify paste behavior)
  /// \param newArg proposed new argument spelling for `argIdx`
  /// \param tokenHunks token-level edit hunks on the current file (used to
  ///                   widen B-envelopes at insertion boundaries)
  /// \returns `true` if all verifiable occurrences of `argIdx` in B are
  ///          consistent with the rewrite; `false` if any required occurrence
  ///          contradicts the rewrite.
  enum class OccurrenceSupportMode {
    /// Only metadata attached directly to the current invocation may justify
    /// occurrence-consistency success. Use this for direct args-only patch
    /// construction so descendant/sibling support cannot silently substitute
    /// for missing current-invocation evidence.
    CurrentInvocationOnly,

    /// Descendant/sibling dependency paths may justify occurrence-consistency
    /// success. Use this only inside the semantic/DAG certificate pipeline,
    /// where the caller is already proving carried rewrites through the
    /// invocation graph.
    AllowGraphSupport,
  };

  bool MacroArgReplacementMatchesAllOccurrencesInB(
      const RefoldModel::MacroInvocation &m, uint32_t argIdx, StringRef baseArg,
      StringRef newArg, ArrayRef<diffutils::Hunk> tokenHunks) const {
    return MacroArgReplacementMatchesAllOccurrencesInBImpl(
        m, argIdx, baseArg, newArg, tokenHunks, /*checkPasteSpans*/ true,
        OccurrenceSupportMode::CurrentInvocationOnly);
  }

  /// \brief Variant of `MacroArgReplacementMatchesAllOccurrencesInB` that
  /// **intentionally skips** per-span paste validation.
  ///
  /// This is used when the caller performs pasted-token validation at a higher
  /// level (e.g., validating that a set of arg replacements reconstructs the
  /// entire pasted token exactly via `PasteArgReplacementsMatchAllPasteTokensInB`).
  /// In that situation, re-validating each paste span in isolation can cause
  /// false negatives, because:
  /// * Multiple arguments may contribute to a single pasted token.
  /// * One edit hunk may modify multiple pasted segments simultaneously.
  /// * Local A->B mapping can be partial away from the edit site.
  ///
  /// STANDARD occurrences and (in `strict` mode) STRINGIFY occurrences are
  /// still enforced.
  ///
  /// \param m macro invocation being patched
  /// \param argIdx zero-based macro parameter index being rewritten
  /// \param baseArg original argument spelling in the invocation (used to
  ///                classify paste behavior)
  /// \param newArg proposed new argument spelling for `argIdx`
  /// \param tokenHunks token-level edit hunks on the current file (used to
  ///                   widen B-envelopes at insertion boundaries)
  /// \returns `true` if all verifiable non-paste occurrences of `argIdx` in B
  ///          are consistent with the rewrite; `false` if any required
  ///          occurrence contradicts it.
  bool MacroArgReplacementMatchesAllOccurrencesInBIgnorePaste(
      const RefoldModel::MacroInvocation &m, uint32_t argIdx, StringRef baseArg,
      StringRef newArg, ArrayRef<diffutils::Hunk> tokenHunks) const {
    return MacroArgReplacementMatchesAllOccurrencesInBImpl(
        m, argIdx, baseArg, newArg, tokenHunks, /*checkPasteSpans*/ false,
        OccurrenceSupportMode::CurrentInvocationOnly);
  }

  /// \brief Semantic-proof variant of
  /// `MacroArgReplacementMatchesAllOccurrencesInB`.
  ///
  /// This preserves the existing DAG/subtree/root-proof behavior: when the
  /// current invocation has no direct occurrence metadata for a formal, the
  /// consumer may still accept the rewrite if a descendant/sibling dependency
  /// path proves that the formal participates in occurrence-bearing structure
  /// elsewhere in the invocation graph.
  bool MacroArgReplacementMatchesAllOccurrencesInBSemanticProof(
      const RefoldModel::MacroInvocation &m, uint32_t argIdx, StringRef baseArg,
      StringRef newArg, ArrayRef<diffutils::Hunk> tokenHunks) const {
    return MacroArgReplacementMatchesAllOccurrencesInBImpl(
        m, argIdx, baseArg, newArg, tokenHunks, /*checkPasteSpans*/ true,
        OccurrenceSupportMode::AllowGraphSupport);
  }

  /// \brief Semantic-proof variant of
  /// `MacroArgReplacementMatchesAllOccurrencesInBIgnorePaste`.
  bool MacroArgReplacementMatchesAllOccurrencesInBIgnorePasteSemanticProof(
      const RefoldModel::MacroInvocation &m, uint32_t argIdx, StringRef baseArg,
      StringRef newArg, ArrayRef<diffutils::Hunk> tokenHunks) const {
    return MacroArgReplacementMatchesAllOccurrencesInBImpl(
        m, argIdx, baseArg, newArg, tokenHunks, /*checkPasteSpans*/ false,
        OccurrenceSupportMode::AllowGraphSupport);
  }

  /// \brief Core implementation for validating whether a proposed args-only
  /// rewrite of a macro parameter is consistent with the edited preprocessed
  /// stream (B).
  ///
  /// **Goal:** ensure that rewriting parameter `argIdx` from `baseArg` to
  /// `newArg` at the invocation site will not contradict what is observed in B
  /// for the same `MacroInvocation`.
  ///
  /// ### Evidence sources
  /// * **STANDARD spans** (`m.argSpans` with kind `STANDARD`): represent direct
  ///   (non-paste) argument expansions visible in the A stream and alignable
  ///   into B.
  /// * **STRINGIFY spans** (`m.stringifySpans`): represent occurrences produced
  ///   by `#` stringification; checked only when `strict` is enabled.
  /// * **PASTE spans** (`m.pasteSpans`): represent subranges of a pasted token
  ///   (via `##`) attributable to `argIdx`. These are optionally verified.
  ///
  /// ### Conservatism and missing metadata
  /// When `supportMode` is `CurrentInvocationOnly`, missing STANDARD spans only
  /// succeed when the current invocation still carries direct STRINGIFY or
  /// PASTE support for `argIdx`; descendant/sibling support does not count.
  /// When `supportMode` is `AllowGraphSupport`, the semantic proof pipeline may
  /// also use descendant/sibling dependency paths as support.
  ///
  /// ### Stringify rule (strict mode)
  /// When `strict` is enabled and `argIdx` is stringified at least once, every
  /// stringify occurrence must exactly equal `QuoteCString(trimEdgeWS(newArg))`.
  /// If any stringify occurrence disagrees, the method returns `false`.
  ///
  /// ### Paste consumption model for STANDARD spans
  /// When an argument participates in token pasting, STANDARD occurrences may
  /// only expose a prefix/suffix of the argument spelling (the remainder is
  /// consumed into the pasted token). This method infers a coarse consumption
  /// direction using `baseArg` and the A-side pasted subrange, then validates
  /// STANDARD occurrences using equality or prefix/suffix checks as appropriate.
  ///
  /// ### Optional paste-span validation
  /// If `checkPasteSpans` is disabled, paste spans are not validated here
  /// (intended for callers that validate pasted-token correctness holistically).
  /// If enabled, paste spans are only validated when the provided `hintHunk`
  /// appears to touch a paste token, to avoid false negatives caused by
  /// incomplete A->B alignment away from the edit site.
  ///
  /// Paste-span checks validate the *segment* of the pasted token attributable
  /// to `argIdx`, but they explicitly do **not** require the rest of the token
  /// to match between A and B, because multiple arguments may contribute to the
  /// same pasted token and can be edited together.
  ///
  /// \param m macro invocation being patched
  /// \param argIdx zero-based macro parameter index being rewritten
  /// \param baseArg original argument spelling in the invocation (optional)
  /// \param newArg proposed new argument spelling for `argIdx`
  /// \param tokenHunks edit hunks on the B token stream (used to widen
  ///                   B-envelopes at insertion boundaries)
  /// \param checkPasteSpans whether to attempt per-span paste verification
  /// \param supportMode whether no-direct-support cases may consult only the
  ///        current invocation or may also use the invocation graph
  /// \returns `true` if all verifiable occurrences of `argIdx` in B are
  ///          consistent with applying the args-only rewrite; `false` on any
  ///          proven contradiction.
  bool MacroArgReplacementMatchesAllOccurrencesInBImpl(
      const RefoldModel::MacroInvocation &m, uint32_t argIdx, StringRef baseArg,
      StringRef newArg, ArrayRef<diffutils::Hunk> tokenHunks,
      bool checkPasteSpans, OccurrenceSupportMode supportMode) const;

  /// \brief Return the exact A-side occurrence that owns the pure-insertion gap
  ///        at \p aPos.
  ///
  /// Classifies an empty A-side hunk using only exact occurrence structure in
  /// the invocation's PP-token space. A gap may be owned by an occurrence when
  /// it lies strictly inside that occurrence, when it sits on the separator
  /// token immediately before a right-hand occurrence, or when it matches an
  /// occurrence end and no right-hand separator owner takes precedence.
  ///
  /// \param aPos The A-side PP-token gap index of the pure insertion.
  /// \param argSpans The invocation occurrences to test, expressed as half-open
  ///        A-side PP-token spans.
  ///
  /// \returns The index of the uniquely owning occurrence in \p argSpans when
  ///          an exact owner exists; otherwise \c std::nullopt.
  std::optional<size_t> FindExactOwningArgSpanForPureInsertion(
      uint64_t aPos, ArrayRef<RefoldModel::PPArgSpan> argSpans) const;

  /// \brief Return the B-side token range owned by \p span for the pure
  ///        insertion hunk \p h.
  ///
  /// Converts a pure insertion from the raw hunk-local B token range into the
  /// effective occurrence-owned B range used for argument derivation and
  /// cross-occurrence verification. For ordinary interior/begin/end ownership,
  /// the owned range is the raw inserted B range. For the exact
  /// separator-before-right-occurrence case, the owned range is shifted so that
  /// it excludes the shared leading separator and includes the separator that
  /// now precedes the original occurrence in B.
  ///
  /// \param span The occurrence that is being tested as the owner of the pure
  ///        insertion.
  /// \param argSpans All occurrence spans for the same argument, used to
  ///        determine exact separator-before-right-occurrence ownership.
  /// \param mappedEnv The existing mapped B-side token envelope for \p span
  ///        before any pure-insertion extension.
  /// \param h The pure insertion hunk whose owned B range is being computed.
  ///
  /// \returns The half-open B token range owned by \p span when the pure
  ///          insertion is exactly attributable to that occurrence; otherwise
  ///          \c std::nullopt.
  std::optional<std::pair<size_t, size_t>> GetOwnedPureInsertionBRangeForArgSpan(
      const RefoldModel::PPArgSpan &span,
      ArrayRef<RefoldModel::PPArgSpan> argSpans,
      std::pair<size_t, size_t> mappedEnv,
      const diffutils::Hunk &h) const;

  /// \brief Returns `true` if the given hunk touches any token-paste occurrence
  /// recorded for the macro invocation.
  ///
  /// This is used to decide whether a macro edit must be handled by
  /// paste-aware logic (e.g., mapping sub-token edits back into a contributing
  /// argument) rather than treating the edit as a normal argument-splice or
  /// forcing macro realization.
  ///
  /// ### Span conventions
  /// * Replacement/deletion hunks use the half-open interval `[aStart, aEnd)`
  ///   in A-token space.
  /// * Paste spans are treated as half-open intervals `[begin, end)` in
  ///   A-token space.
  ///
  /// Insertion hunks have `aStart == aEnd`. For insertions, we conservatively
  /// treat the hunk as touching a paste span if the insertion point lies on or
  /// within the paste span boundary (`begin <= aStart <= end`). This ensures
  /// that edits inserted exactly at a paste boundary are routed through
  /// paste-aware handling.
  ///
  /// \param m macro invocation metadata (optional)
  /// \param h edit hunk in A-token coordinates (optional)
  /// \returns `true` if the hunk intersects any paste span; otherwise `false`.
  static bool HunkTouchesAnyPasteToken(const RefoldModel::MacroInvocation &m,
                                       const diffutils::Hunk &h);

  /// \brief Attempts to derive a single-argument "paste edit" for a token-pasting
  /// (`##`) macro invocation.
  ///
  /// This is the fast-path used when a hunk touches a `pasteSpans` occurrence.
  /// In this case, the edited region in the preprocessed output may lie
  /// *inside* a single pasted token (e.g., changing `a_b_c` to `a_d_c`), which
  /// cannot be recovered by standard token-to-token argument span matching.
  /// Instead, the refolder tries to attribute the change to exactly one
  /// contributing argument by using the producer-provided `[byteBegin, byteEnd)`
  /// subrange describing each argument's slice within the pasted token.
  ///
  /// ### High-level algorithm
  /// 1. Collect all `pasteSpans` that intersect the hunk in A-token space.
  /// 2. Map the pasted token envelope from A to B and require that it
  ///    corresponds to exactly one B token (the pasted token).
  /// 3. Compute the minimal differing range between the A pasted token text and
  ///    the B pasted token text. If the token texts are identical, the hunk
  ///    cannot be attributed to paste behavior.
  /// 4. Select exactly one candidate paste span whose `[byteBegin, byteEnd)`
  ///    overlaps the differing region. If multiple argument slices overlap, the
  ///    edit is not representable as a single-argument args-only patch.
  /// 5. Verify that all changes are confined to that chosen byte slice (prefix/
  ///    suffix of the pasted token must match exactly), then return the derived
  ///    `PasteArgEdit`.
  ///
  /// Failure is conservative: this method returns `std::nullopt` unless it can
  /// prove that a single argument slice accounts for *all* changes to the
  /// pasted token. Callers should then fall back to normal occurrence-based arg
  /// patching or macro realization.
  ///
  /// Note: this method compares token *text* slices and strips trailing
  /// newlines from the raw token strings before diffing. It assumes that
  /// `byteBegin`/`byteEnd` are offsets into the pasted token spelling (not the
  /// overall PP stream).
  ///
  /// \param m macro invocation metadata containing `pasteSpans`
  /// \param h edit hunk in A-token coordinates
  /// \returns a `PasteArgEdit` describing a single-argument replacement for the
  ///          pasted token, or `std::nullopt` if no safe single-arg derivation
  ///          exists.
  std::optional<PasteArgEdit>
  DerivePasteArgEdit(const RefoldModel::MacroInvocation &m,
                     const diffutils::Hunk &h) const;

  /// \brief Derives one or more *token-paste* argument edits for a single macro
  /// invocation hunk.
  ///
  /// This helper is used by the args-only macro patching path when the edit
  /// touches a pasted token (i.e., a token produced via `##`). In that
  /// situation, the edited surface text in the preprocessed stream may be a
  /// single identifier that is composed of multiple argument contributions
  /// (e.g., `a_b_c`), and the refolder must attribute the change back to one or
  /// more specific macro arguments.
  ///
  /// The method operates on the **A-stream** (unmodified preprocessed output
  /// produced by Clang) and the edited **B-stream** (user-modified preprocessed
  /// output) by mapping the pasted-token A occurrence to a B token envelope via
  /// `MapAToBTokenEnvelopeByPPArgSpan`. It is intentionally conservative: it
  /// returns `std::nullopt` when it cannot produce a deterministic, unambiguous
  /// decomposition.
  ///
  /// ### High-level algorithm
  /// 1. Collect all `PPArgSpan` entries in `m.pasteSpans` that intersect the
  ///    hunk's A-token interval `[h.aStart, h.aEnd)`.
  /// 2. Use the pasted token's A-token interval `[begin, end)` (from the first
  ///    candidate span) and map it to a B token envelope using
  ///    `MapAToBTokenEnvelopeByPPArgSpan`.
  /// 3. Require that the pasted A-span maps to **exactly one** B token. If the
  ///    edit spans multiple B tokens, args-only refolding is not representable
  ///    and the method returns `std::nullopt`.
  /// 4. Slice the raw pasted token text from A and B and normalize by stripping
  ///    trailing newlines.
  /// 5. Deterministically segment the B token into per-argument regions.
  ///
  /// Segmentation is performed by anchoring on the *fixed* (non-span) substrings
  /// between paste spans in the A token. For a contiguous run of adjacent spans
  /// (no fixed internal anchor), the inversion now computes an explicit
  /// certificate: the run is invertible when unchanged neighboring segments pin
  /// the boundary, and conservatively ambiguous once more than one touching
  /// span in the same run would need to change. This supports edits such as
  /// `a##b##c -> foo##b##c` while still rejecting underdetermined rewrites like
  /// `a##b##c -> foo##bar##c`.
  ///
  /// ### Return value
  /// On success, returns a list of `PasteArgEdit` objects, one per affected
  /// argument, where each entry captures:
  /// * `argIdx`: the macro argument index that contributed to the pasted token.
  /// * `oldSeg`: the original A-token segment contributed by that argument.
  /// * `newSeg`: the corresponding replacement segment inferred from the edited
  ///   B token.
  ///
  /// If no argument contributions actually change (i.e., the pasted token text
  /// is identical), or if multiple spans imply edits for the same `argIdx`
  /// within one pasted token (ambiguous args-only representation), this method
  /// returns `std::nullopt`.
  ///
  /// \param m Macro invocation metadata containing paste span information.
  /// \param h Diff hunk in A/B token space being considered for args-only
  ///          refolding.
  /// \returns A list of per-argument paste edits, or `std::nullopt` if the edit
  ///          cannot be represented deterministically as args-only.
  std::optional<std::vector<PasteArgEdit>>
  DerivePasteArgEdits(const RefoldModel::MacroInvocation &m,
                      const diffutils::Hunk &h) const;

  /// \brief Segments the edited pasted-token spelling (`bTok`) into per-argument
  /// substrings by using the original pasted-token spelling (`aTok`) as an
  /// anchor template.
  ///
  /// The input `spansAsc` identifies the argument-contributed regions inside
  /// `aTok`: each `PPArgSpan` provides `[byteBegin, byteEnd)` offsets that
  /// delimit the portion of `aTok` produced by one argument during token
  /// pasting. The remaining characters in `aTok` (the gaps between spans, and
  /// the tail after the final span) are treated as *fixed* slices that must
  /// appear unchanged and in order in `bTok`.
  ///
  /// This method returns a list of segments where `segs[i]` is the substring of
  /// `bTok` corresponding to `spansAsc[i]`. It succeeds only when the
  /// segmentation is lexically deterministic:
  /// * All fixed slices from `aTok` match `bTok` in-order.
  /// * Each span's boundary in `bTok` can be determined by locating the next
  ///   fixed slice that follows it (or by consuming the remaining suffix for
  ///   the final span).
  ///
  /// Length-changing edits are supported (e.g. `a_b_c -> foo_bar_baz`) as long
  /// as fixed slices, or unchanged neighboring span spellings, uniquely pin the
  /// segment boundaries. For a touching run with no fixed internal delimiter,
  /// the inversion accepts at most one edited span in that run; once two
  /// neighboring span contributions both need to move, the boundary becomes
  /// underdetermined and this method returns `std::nullopt`.
  ///
  /// ### Implementation notes
  /// * `spansAsc` must be non-overlapping, sorted by `byteBegin` ascending, and
  ///   within `[0, aTok.length()]`; otherwise `std::nullopt` is returned.
  /// * The search for fixed anchors in `bTok` is memoized by `(idx, posB)` so
  ///   repeated suffix states are solved once while preserving the existing
  ///   left-to-right acceptance order.
  ///
  /// \param aTok The original pasted-token spelling from the unedited
  ///             preprocessed stream (A).
  /// \param bTok The edited pasted-token spelling from the edited preprocessed
  ///             stream (B).
  /// \param spansAsc Argument-contributed spans inside `aTok`, sorted by
  ///                 `byteBegin`.
  /// \returns A list of per-arg segment strings from `bTok` aligned to
  ///          `spansAsc`, or `std::nullopt` if segmentation is not provably
  ///          deterministic.
  static std::optional<std::vector<std::string>>
  SegmentPastedTokenArgsByFixedSlices(
      StringRef aTok, StringRef bTok,
      ArrayRef<const RefoldModel::PPArgSpan *> spansAsc);

  /// \brief Derives the replacement text for a pasted-token sub-segment
  /// (`oldSeg`) implied by an args-only rewrite from `baseArg` to `newArg`.
  ///
  /// In token-paste macros (`##`), an argument may contribute only a prefix or
  /// suffix of its spelling to the final pasted token. A paste-span records the
  /// exact subrange of the pasted token that originated from the argument under
  /// the *original* invocation spelling. When the invocation argument is
  /// rewritten, we need to compute the corresponding sub-segment that should
  /// appear in the pasted token after the rewrite.
  ///
  /// This helper implements a simple, deterministic model:
  /// * If `oldSeg` equals the full trimmed `baseArg`, the entire argument was
  ///   pasted, so the new segment is the full trimmed `newArg`.
  /// * If `baseArg` starts with `oldSeg`, then `oldSeg` is a pasted prefix.
  ///   The suffix remainder of `baseArg` must still appear as a suffix of
  ///   `newArg`, and the returned segment is `newArg` with that suffix removed.
  /// * If `baseArg` ends with `oldSeg`, then `oldSeg` is a pasted suffix.
  ///   The prefix remainder of `baseArg` must still appear as a prefix of
  ///   `newArg`, and the returned segment is `newArg` with that prefix removed.
  ///
  /// If `oldSeg` cannot be classified as the full argument, a prefix, or a
  /// suffix of `baseArg`, or if `newArg` is not structurally compatible with
  /// the inferred classification, this returns an empty `StringRef()`.
  ///
  /// \param baseArg original argument spelling (as written at the invocation
  ///                site)
  /// \param newArg proposed new argument spelling
  /// \param oldSeg sub-segment from the pasted token that originated from
  ///               `baseArg`
  /// \returns the pasted-token replacement segment implied by
  ///          `baseArg -> newArg`, or an empty `StringRef()` if the segment cannot be
  ///          derived soundly.
  static StringRef DeriveNewPasteSegmentFromSpellingReplacement(
      StringRef baseArg, StringRef newArg, StringRef oldSeg);

  /// \brief Validates that a set of candidate argument replacements
  /// (`replByArgIdx`) reproduces the edited spelling of every token produced
  /// via `##` token-pasting for a macro invocation.
  ///
  /// This is a strict safety gate used by args-only macro patching when the
  /// macro body contains token-paste operations. In token-pasting macros, the
  /// preprocessed output often contains *composite* identifiers (or other
  /// tokens) built from multiple argument contributions. When we attempt to
  /// refold by modifying only the invocation spelling (instead of realizing the
  /// macro body), we must ensure that the resulting pasted-token text in the
  /// edited preprocessed stream is exactly reproduced.
  ///
  /// The producer emits `PPArgSpan` entries for paste contributions. Each
  /// `PPArgSpan` in `m.pasteSpans` describes:
  /// * Which invocation argument contributed (`argIdx`).
  /// * The pasted-token occurrence in the A-stream via `[begin, end)` token
  ///   indices.
  /// * The sub-token character slice inside the pasted token via
  ///   `[byteBegin, byteEnd)`.
  ///
  /// This method groups paste spans by pasted-token occurrence, maps each
  /// A occurrence to a corresponding single-token envelope in B using
  /// `MapATokRangeAToBTokenEnvelope`, then simulates rewriting the A pasted
  /// token by applying per-span segment updates derived from invocation
  /// argument replacements. The simulation must match the edited B pasted
  /// token exactly.
  ///
  /// ### Key invariants / constraints
  /// * Each pasted-token occurrence must map to exactly one token in A and B.
  /// * Segment rewrites are applied right-to-left by `byteBegin` to keep
  ///   offsets stable.
  /// * Segment updates are derived deterministically from:
  ///   `baseInvText` + `invArgRanges` (original arg spelling), the candidate
  ///   replacement spelling, and the original pasted-token slice (`oldSeg`).
  /// * If any rewritten pasted token does not equal the B token spelling, this
  ///   returns `false` and the caller must fall back to macro realization
  ///   (expansion).
  ///
  /// \param m Macro invocation metadata (must contain `pasteSpans`).
  /// \param baseInvText Invocation spelling text (e.g., "CONCAT(a,b,c)").
  /// \param invArgRanges Argument content ranges within `baseInvText`.
  /// \param replByArgIdx Candidate replacements per argument index.
  /// \returns `true` if the replacements reproduce every pasted token
  ///          occurrence in B.
  bool PasteArgReplacementsMatchAllPasteTokensInB(
      const RefoldModel::MacroInvocation &m, StringRef baseInvText,
      ArrayRef<std::pair<size_t, size_t>> invArgRanges,
      const DenseMap<uint32_t, std::string> &replByArgIdx) const;

  /// \brief Splices a derived paste-segment edit into a macro argument's
  /// spelling text.
  ///
  /// This helper is used after `derivePasteArgEdit` determines that an edit
  /// inside a token-pasted (`##`) output token can be attributed to a single
  /// argument slice. The goal is to rewrite the corresponding invocation
  /// argument so that re-preprocessing reproduces the edited pasted token in
  /// B_PP, without realizing (expanding) the macro.
  ///
  /// The splice is intentionally conservative: it only applies when `oldSeg`
  /// matches an unambiguous boundary region of the argument spelling (after
  /// trimming edge whitespace):
  /// * Whole-argument replacement: `baseTrim == oldTrim`
  /// * Prefix splice: `baseTrim` starts with `oldTrim`
  /// * Suffix splice: `baseTrim` ends with `oldTrim`
  ///
  /// If `oldTrim` matches both the prefix and suffix (e.g., repeated text) or
  /// matches neither, the splice is rejected and an empty string is returned.
  /// This avoids ambiguous rewrites that could change the meaning of the macro
  /// invocation or fail idempotence under re-preprocessing.
  ///
  /// Whitespace handling: all comparisons are performed on edge-trimmed forms,
  /// and the returned value is constructed from the trimmed base text. Callers
  /// are responsible for re-integrating any desired formatting or argument
  /// quoting policy around the returned string.
  ///
  /// \param baseArg the original invocation argument spelling
  /// \param oldSeg the segment text contributed by the argument in the pasted
  ///               token (A stream)
  /// \param newSeg the replacement segment text observed in the edited pasted
  ///               token (B stream)
  /// \returns the rewritten argument spelling if a safe boundary splice is
  ///          possible; otherwise an empty string.
  static std::string SplicePasteSegmentIntoSpellingArg(StringRef baseArg,
                                                       StringRef oldSeg,
                                                       StringRef newSeg);

  /// \brief Return per-formal argument content ranges for a function-like macro
  /// invocation.
  ///
  /// The returned vector is indexed by **formal parameter index** (not by the
  /// number of "actual" comma-separated arguments in the invocation text). Each
  /// entry is a half-open byte range **`[begin, end)`** into `invText` that
  /// selects the *content* of the corresponding argument (excluding surrounding
  /// whitespace and outer delimiters as produced by the parser).
  ///
  /// ## Preferred source: producer-provided ranges
  ///
  /// If the refold-map contains `m.invArgRanges`, those byte ranges are treated
  /// as authoritative. They are recorded in the **B-domain** (absolute file
  /// offsets), so they are first validated and rebased relative to `m.invB` to
  /// produce indices into `invText`.
  ///
  /// This path is **required for variadic macros**, because multiple "actual"
  /// arguments may correspond to a single formal (e.g. `__VA_ARGS__` or a GNU
  /// named variadic like `args...`). In that case the *last formal* range spans
  /// the entire variadic tail, including separating commas.
  ///
  /// ## Recovery: conservative textual parse
  ///
  /// If some producer-provided ranges are missing/invalid, this routine attempts
  /// to fill them using a conservative parse of the invocation spelling. When
  /// the parsed arity differs from the formal arity:
  ///
  /// * **`actualN == formalN`**: fill missing entries 1:1.
  /// * **`actualN > formalN`**: treat as variadic and merge the remaining tail
  ///   into the last formal range.
  /// * **`actualN < formalN`**: treat trailing formals as empty (e.g. empty
  ///   `__VA_ARGS__`) by producing an empty range at the close-paren.
  ///
  /// Returns `std::nullopt` if the invocation cannot be parsed or if required
  /// anchoring information (e.g. `m.invB`) is unavailable.
  static std::optional<std::vector<std::pair<size_t, size_t>>>
  GetMacroInvocationFormalArgContentRanges(
      const RefoldModel::MacroInvocation &m, StringRef invText);

  /// \brief Attempts to build an *args-only* macro invocation patch for a hunk
  /// attributed to a macro invocation.
  ///
  /// An args-only patch rewrites only the invocation argument text (e.g.,
  /// `MACRO(x,y)`), leaving the macro definition unchanged, such that
  /// re-preprocessing the refolded TU reproduces the edited preprocessed
  /// stream `B_PP`. This is preferred over forcing macro realization because
  /// it preserves the original source structure and avoids pulling expanded
  /// tokens into the TU.
  ///
  /// This method enforces a strict safety contract:
  /// * The invocation must have a concrete TU byte range (`m.invB`/`m.invE`).
  /// * The hunk must be fully contained within one or more producer-recorded
  ///   argument-occurrence spans for this invocation (standard args and
  ///   stringify args).
  /// * For each touched argument, the derived replacement must be consistent
  ///   with *all* of its occurrences in `B_PP` (standard, paste, and stringify,
  ///   subject to strict/non-strict policy).
  /// * If any touched occurrence implies conflicting replacements for the same
  ///   `argIdx`, the patch is rejected.
  ///
  /// ### Paste-aware fast path
  /// If the hunk touches any `pasteSpans` occurrence, the edit may be a
  /// sub-token change inside a single pasted token (e.g., `a_b_c -> a_d_c`).
  /// In that case, the method attempts to derive per-arg segment edits and
  /// splice them into the corresponding invocation arguments via
  /// `SplicePasteSegmentIntoSpellingArg`.
  ///
  /// For multi-span paste edits, per-arg paste-span consistency cannot be
  /// validated in isolation when token lengths may change. Instead, the method
  /// first validates standard + stringify occurrences for each touched argument
  /// (ignoring paste spans), then applies a combined safety gate:
  /// `PasteArgReplacementsMatchAllPasteTokensInB(...)` must reconstruct every
  /// pasted token occurrence exactly as observed in B.
  ///
  /// For single-segment paste edits, the method may validate all occurrences
  /// (including paste spans) directly via
  /// `MacroArgReplacementMatchesAllOccurrencesInB(...)`.
  ///
  /// If paste derivation fails, the method falls through to the standard
  /// (non-paste) policy below.
  ///
  /// ### Standard (non-paste) path
  /// The method collects standard arg spans and stringify spans, requires the
  /// hunk to be covered by those spans, then derives candidate argument
  /// replacements by mapping each touched occurrence to a B token envelope via
  /// `MapAToBTokenEnvelopeByPPArgSpan`. It slices the corresponding edited B
  /// text and interprets it as either:
  /// * Raw token text for standard occurrences, or
  /// * Unstringified argument text for stringify occurrences.
  ///
  /// When an argument participates in token pasting, the method may attempt a
  /// "lift/paste" rewrite: it searches for the A occurrence slice inside the
  /// base argument spelling and replaces only that slice with the edited B
  /// slice.
  ///
  /// On success, returns a `MacroPatch` replacing `[m.invB, m.invE)` in the TU
  /// byte stream with a newly constructed invocation string. On failure,
  /// returns `std::nullopt`, signaling that the caller should fall back to
  /// macro realization/expansion.
  ///
  /// \param m Macro invocation metadata (argument spans, stringify spans,
  ///          paste spans, and TU byte range).
  /// \param h Edit hunk attributed to this macro invocation (A/B token
  ///          coordinates).
  /// \param baseInvocationText The original invocation source text to be
  ///                           rewritten (e.g., `MACRO(...)`).
  /// \returns A `MacroPatch` for args-only invocation rewriting, or
  ///          `std::nullopt` if not provably safe.
  std::optional<MacroPatch> BuildMacroInvocationPatchArgsOnly(
      const RefoldModel::MacroInvocation &m, const diffutils::Hunk &h,
      StringRef baseInvocationText) const;

  /// \brief Slices a source text by token indices using a token-to-byte offset
  /// table.
  ///
  /// This helper converts a half-open token interval `[startTok, endTok)` into
  /// a byte-offset range using `tokOff` and returns the corresponding `StringRef`
  /// of `source`.
  ///
  /// `tokOff` is the token-to-byte offset table:
  /// * `tokOff[i]` is the starting byte offset of token `i`.
  /// * The table is expected to have length `(numTokens + 1)`, where the final
  ///   entry `tokOff[numTokens]` equals `source.size()` (end sentinel).
  ///
  /// For robustness, this method clamps token indices into the valid table range
  /// and clamps derived byte offsets into `[0, source.size()]`.
  ///
  /// \param tokOff   Token-to-byte offset table.
  /// \param source   The source text to slice.
  /// \param startTok Inclusive start token index.
  /// \param endTok   Exclusive end token index.
  /// \returns A `StringRef` of the source covered by tokens `[startTok, endTok)`.
  static StringRef SliceSource(ArrayRef<size_t> tokOff, StringRef source,
                               uint64_t startTok, uint64_t endTok);

  /// Slices the A-side source text by token indices.
  /// \see SliceSource
  StringRef SliceASource(uint64_t aStartTok, uint64_t aEndTok) const {
    return SliceSource(aTokOff_, aSource_, aStartTok, aEndTok);
  }

  /// Slices the B-side source text by token indices.
  /// \see SliceSource
  StringRef SliceBSource(uint64_t bStartTok, uint64_t bEndTok) const {
    return SliceSource(bTokOff_, bSource_, bStartTok, bEndTok);
  }

  /// \brief Attempts to invert a string literal token that was produced by
  /// macro stringification (`#param`) back into a single invocation-site
  /// argument spelling.
  ///
  /// In the preprocessor, `#X` produces a string literal token (e.g.,
  /// `"hello-world"`) even though the invocation argument text was
  /// `hello-world`. During refolding, edits to that produced literal may be
  /// projected back onto the invocation site by "unstringifying" the literal
  /// and using the result as the replacement argument text.
  ///
  /// This routine is intentionally conservative and only supports ordinary and
  /// prefixed string literals that look like one token:
  /// * ` "..." `
  /// * ` L"..." ` (wide)
  /// * ` u"..." ` / ` U"..." ` / ` u8"..." ` (Unicode)
  ///
  /// The body is extracted from the first double-quote through the final
  /// double-quote and is minimally unescaped: `\\` and `\"` are reduced to
  /// `\` and `"`, respectively. Other escape sequences are preserved verbatim
  /// (the backslash and following character are kept) to avoid changing
  /// semantics.
  ///
  /// ### Safety checks
  /// The resulting argument text must be representable as a single macro
  /// argument. If the unescaped body contains a comma, or contains a newline,
  /// this method returns `std::nullopt` to force callers to take a conservative
  /// path (e.g., expand the macro instance) rather than producing an ambiguous
  /// or syntactically invalid invocation such as `FOO(a, b)` when the macro has
  /// only one parameter.
  ///
  /// \param literalTok A token string that should represent a single C/C++
  ///                   string literal.
  /// \returns The corresponding invocation-site argument spelling, or
  ///          `std::nullopt` if the literal is not recognized or cannot be
  ///          safely mapped to a single argument.
  std::optional<std::string>
  UnstringifyLiteralToArgText(StringRef literalTok,
                              bool allowTopLevelComma = false) const;

  /// \brief Determines if a given edit hunk is entirely contained within a set
  /// of macro argument spans.
  ///
  /// This method serves as a safety gate for "surgical" macro patching. It
  /// ensures that an edit (the `Hunk`) only touches the tokens corresponding to
  /// macro parameters and does not overlap with the macro's "boilerplate" body
  /// text (e.g., operators, semicolons, or literal constants defined in the
  /// macro body).
  ///
  /// The containment logic differs based on the type of edit:
  ///
  /// - **Insertions (aStart == aEnd):** Attributed to an argument if the
  ///   insertion point `a0` falls within the *inclusive* range `[begin, end]`
  ///   of an argument span. This allows for prepending or appending to an
  ///   argument's content.
  /// - **Replacements/Deletions (aStart < aEnd):** Every token index in the
  ///   interval `[aStart, aEnd)` must be contained within at least one argument
  ///   span. This ensures no part of the macro's structural body is modified.
  ///
  /// As a side effect, the `touched` array is updated to track which specific
  /// span occurrences from the provided `argSpans` list are affected by the
  /// hunk. Callers that need per-formal state must compress these occurrence
  /// touches using each span's `argIdx`.
  ///
  /// \param h The hunk representing the edit in the original preprocessed (A)
  ///          token stream.
  /// \param argSpans A list of token spans representing the regions where macro
  ///                 parameters appear in the expanded preprocessed output.
  /// \param touched An output array of the same length as `argSpans`, where
  ///                indices will be set to `true` if the hunk overlaps that
  ///                specific argument.
  /// \return `true` if the hunk is fully contained within the provided argument
  ///         spans; `false` if any part of the hunk touches non-argument tokens
  ///         or falls outside the known argument regions.
  bool HunkFullyWithinArgSpans(const diffutils::Hunk &h,
                               ArrayRef<RefoldModel::PPArgSpan> argSpans,
                               MutableArrayRef<char> touched) const;

  /// \brief Converts a list of token-based edit hunks into byte-based hunks.
  ///
  /// This routine projects the token indices in \p tokenHunks back onto the raw
  /// source byte streams (A and B) using the pre-computed token offset arrays.
  ///
  /// The transformation accounts for the sentinel offsets at the end of each
  /// token stream, allowing hunks that point to the end of the file to be
  /// resolved correctly. Input indices are clamped to valid token ranges to
  /// ensure safety against alignment anomalies.
  ///
  /// \returns A vector of \c ByteHunk objects containing the corresponding
  ///          [begin, end) byte offsets in the A and B source buffers.
  std::vector<ByteHunk> BuildByteHunksFromRawText() const;

  /// \brief Projects a byte offset from source A to source B using
  /// lower-bound semantics.
  ///
  /// This mapping resolves where a specific coordinate in the original
  /// preprocessed stream (A) lands in the edited stream (B). If the
  /// coordinate falls within a range that was deleted or replaced, it
  /// snaps to the beginning of the replacement in B.
  ///
  /// \param aByte The byte offset in the original preprocessed stream A.
  /// \returns The corresponding byte offset in the edited stream B.
  size_t MapAByteToBByteLowerBound(size_t aByte) const;

  /// \brief Projects a byte offset from source A to source B using
  /// upper-bound semantics.
  ///
  /// Similar to the lower-bound mapping, this projects coordinates from A
  /// to B, but treats boundaries differently to ensure inclusive ranges.
  /// If aByte falls exactly at the start of an insertion, the mapping
  /// includes that insertion in the resulting B offset. If it falls inside
  /// a replaced range, it snaps to the end of the replacement in B.
  ///
  /// \param aByte The byte offset in the original preprocessed stream A.
  /// \returns The corresponding byte offset in the edited stream B.
  size_t MapAByteToBByteUpperBound(size_t aByte) const;

  /// \brief Finds the largest token index `i` such that `bTokOff_[i] <= bByte`.
  ///
  /// This performs a floor-based binary search on the B-stream token offsets.
  /// It identifies the token that contains the given byte offset or the most
  /// recent token starting before it.
  ///
  /// \param bByte The byte offset in the edited preprocessed stream B.
  /// \returns The index of the token corresponding to the floor of \p bByte.
  size_t BTokIndexFloor(size_t bByte) const;

  /// \brief Finds the smallest token index `i` such that `bTokOff_[i] >= bByte`.
  ///
  /// This performs a ceiling-based binary search on the B-stream token offsets.
  /// It is typically used to find the exclusive end-boundary of a token range
  /// corresponding to a byte-level span.
  ///
  /// \param bByte The byte offset in the edited preprocessed stream B.
  /// \returns The index of the token corresponding to the ceiling of \p bByte.
  size_t BTokIndexCeil(size_t bByte) const;

  /// \brief Maps a byte range in source A to a token envelope in source B.
  ///
  /// This is the core mapping routine that projects a character-level span
  /// from the original preprocessed stream onto a discrete range of tokens
  /// in the edited stream. It accounts for edit hunks to find the correct
  /// B-space byte offsets before performing a binary search to find the
  /// containing tokens.
  ///
  /// \param aByteBegin The starting byte offset in source A.
  /// \param aByteEnd The ending byte offset (exclusive) in source A.
  /// \returns A pair representing the [begin, end) token indices in source B.
  std::pair<size_t, size_t>
  MapAByteRangeToBTokenEnvelope(size_t aByteBegin, size_t aByteEnd) const;
  std::pair<size_t, size_t>
  MapAByteRangeToBTokenEnvelopePreserveBoundaryInsertions(size_t aByteBegin,
                                                          size_t aByteEnd) const;

  /// \brief Maps a macro argument span to its corresponding B-token envelope.
  ///
  /// This function prioritizes high-precision preprocessor byte offsets stored
  /// in the \p PPArgSpan. If those offsets are missing or invalid, it falls
  /// back to using the token-index-based offsets from the consumer stream.
  ///
  /// \param sp The macro argument span metadata from the RefoldModel.
  /// \returns The B-token range if mapping is successful, std::nullopt
  /// otherwise.
  std::optional<std::pair<size_t, size_t>>
  MapAToBTokenEnvelopeByPPArgSpan(const RefoldModel::PPArgSpan &sp) const;

  /// \brief Maps a token range in source A to a token envelope in source B.
  ///
  /// Converts a token-index-based range from the original stream into a
  /// byte-offset range, then projects that range into the edited stream's
  /// token space. This is used when identifying the B-side impact of edits
  /// defined by A-side token boundaries.
  ///
  /// \param beginTok The starting token index in source A.
  /// \param endTok The ending token index (exclusive) in source A.
  /// \returns The B-token range if the input is valid, std::nullopt otherwise.
  std::optional<std::pair<size_t, size_t>>
  MapATokRangeAToBTokenEnvelope(uint64_t beginTok, uint64_t endTok) const;
  std::optional<std::pair<size_t, size_t>>
  MapATokRangeAToBTokenEnvelopePreserveBoundaryInsertions(uint64_t beginTok,
                                                          uint64_t endTok) const;

  /// \brief Map an A-token range to its B-token envelope for whole-cover
  /// replacement.
  ///
  /// Unlike \c MapATokRangeAToBTokenEnvelope, this preserves pure-insertion
  /// payloads anchored exactly at the A-range boundaries. Whole-cover macro
  /// replacement wants the full B-side image of the cover, and later claim
  /// clipping is responsible for preventing duplicate emission of standalone
  /// boundary insertions.
  std::optional<std::pair<size_t, size_t>>
  MapATokRangeAToBTokenEnvelopeWholeCover(uint64_t beginTok,
                                          uint64_t endTok) const;

  /// \brief Variant of `MapATokRangeAToBTokenEnvelope` that trims *edge* pure
  /// insertion hunks from the resulting B-token envelope.
  ///
  /// Rationale:
  ///   Byte-level envelope mapping can legally absorb a pure-insertion hunk
  ///   anchored exactly at `beginTok` or `endTok` into the mapped B envelope for
  ///   the interior A span. For whole-cover macro replacement, those edge
  ///   insertions are outside the macro cover and are applied separately; if
  ///   they are also included in the whole-cover slice, the insertion material
  ///   is duplicated. This method enforces a deterministic "no absorption of
  ///   edge insertions" rule by trimming any token-level pure-insertion hunks
  ///   anchored at the requested A-span boundaries.
  ///
  /// This method is intended for *whole-cover* mapping where the A-span is a
  /// coverage interval (e.g. a macro cover). It should not be used for mapping
  /// argument-content spans where an insertion at the span boundary may be
  /// semantically part of the argument.
  std::optional<std::pair<size_t, size_t>>
  MapATokRangeAToBTokenEnvelopeTrimEdgeInsertions(uint64_t beginTok,
                                                  uint64_t endTok) const;

  /// \brief Resolve an include-realization B-token envelope from an A-cover.
  ///
  /// The canonical include-realization path uses
  /// \c MapATokRangeAToBTokenEnvelope to recover the exact B-side token
  /// envelope for the include cover. Step 4A strengthens that witness search by
  /// also consulting the existing deterministic rescue projections when the
  /// canonical mapping is unavailable.
  ///
  /// To stay fail-closed, this helper only accepts a rescue envelope when the
  /// available non-canonical projections agree on one non-empty B-token range.
  /// The returned evidence kind therefore states the full class predicate for
  /// inline include realization: the witness must be either canonical or a
  /// deterministic consensus rescue. Any unresolved or conflicting B-envelope
  /// recovery remains an explicit out-of-domain terminal case rather than a
  /// guessed realization.
  std::optional<std::pair<size_t, size_t>>
  ResolveIncludeRealizationBTokenEnvelope(
      uint64_t beginTok, uint64_t endTok,
      IncludeRealizationEvidenceKind *evidenceKind = nullptr) const;

  /// \brief Parses the raw text of a function-like macro invocation to identify
  /// the byte ranges of its individual arguments.
  ///
  /// This method performs a shallow, brace-aware scan of the invocation text
  /// starting from the opening parenthesis. It correctly handles nested
  /// parentheses, brackets, and braces, ensuring that commas within nested
  /// expressions (like function calls or initializer lists) do not prematurely
  /// terminate an argument.
  ///
  /// The parser is also string- and character-literal aware; it skips over
  /// escaped characters and delimiters within quotes to avoid misinterpreting
  /// structural C characters as macro argument separators.
  ///
  /// \param invText The full source text of the macro invocation
  ///                (e.g., "MY_MACRO(a, f(b, c))").
  /// \return A list of `[start, end]` byte ranges for each argument, with
  ///         leading and trailing whitespace trimmed; returns `std::nullopt` if
  ///         the text is malformed or the closing parenthesis is missing.
  static std::optional<std::vector<std::pair<size_t, size_t>>>
  ParseMacroInvocationArgContentRanges(StringRef invText);

  // ------------------------- __COUNTER__ stabilization -----------------------

  /// \brief Determine which macro invocations must be forced to remain expanded
  /// in order to stabilize edited __COUNTER__ semantics.
  ///
  /// Policy:
  ///   If any expanded __COUNTER__ occurrence is edited in B (relative to its
  ///   A-side expansion token(s)), then that occurrence and every subsequent
  ///   __COUNTER__ occurrence in PP-token order must be emitted as a literal
  ///   expansion (whole-cover), even if unchanged. This preserves the edited
  ///   preprocessed semantics because replacing a __COUNTER__ site with a
  ///   literal stops the counter from incrementing, which would otherwise shift
  ///   all later __COUNTER__ values.
  ///
  /// The producer emits a MacroInvocation item for each __COUNTER__ expansion
  /// with A-token coverage (cover.begin/cover.end). We use the A→B alignment
  /// map \p a2b to detect the first edited occurrence.
  ///
  /// Important: a __COUNTER__ occurrence may be spelled inside a macro
  /// definition's replacement list (e.g. "#define PRINT(...) __COUNTER__"). In
  /// that case, patching the __COUNTER__ invocation span would illegally mutate
  /// the macro definition. Instead, we must force expansion of the *smallest
  /// patchable macro callsite* that covers the __COUNTER__ expansion token(s)
  /// (e.g. the PRINT(...) call), and then force all later occurrences
  /// similarly.
  ///
  /// This function therefore returns the set of covering, patchable macro
  /// invocations that should be whole-cover expanded.
  struct ForcedMacroPatchRequest {
    const RefoldModel::MacroInvocation *macro;
    uint64_t aStart;
    uint64_t aEnd;
  };

  /// \brief Compute forced expansion patches needed to stabilize __COUNTER__
  /// semantics.
  ///
  /// If any expanded __COUNTER__ occurrence is edited in B, then that
  /// occurrence and every subsequent __COUNTER__ occurrence (in PP-token order)
  /// must be emitted as a literal expansion, even if unchanged. This preserves
  /// the edited preprocessed semantics because replacing a __COUNTER__ site
  /// with a literal stops the counter from incrementing, which would otherwise
  /// shift all later
  /// __COUNTER__ values.
  ///
  /// Important: a __COUNTER__ occurrence may be spelled inside a macro
  /// definition's replacement list (e.g. "#define PRINT(...) __COUNTER__"). In
  /// that case, patching the __COUNTER__ invocation span would illegally mutate
  /// the macro definition. Instead, we force expansion of the smallest
  /// patchable macro callsite that covers the __COUNTER__ expansion token(s)
  /// (e.g. the PRINT(...) callsite), and then force all later occurrences
  /// similarly.
  ///
  /// The producer may record multiple body occurrences for a single invocation
  /// spelling (e.g. when a header is included multiple times). We therefore
  /// treat each body-span occurrence separately and filter by the true PP-owner
  /// include.
  SmallVector<ForcedMacroPatchRequest, 32>
  ComputeForcedCounterPatches(StringRef tuPath, ArrayRef<int64_t> a2b) const;

  /// \brief Compute additional forced __COUNTER__ stabilization patches caused
  ///        by macro callsites that already remain expanded after normal hunk
  ///        attribution.
  ///
  /// If a normal macro patch expands a callsite whose expansion contains a
  /// __COUNTER__ occurrence, recompiling the refolded source would stop that
  /// callsite from incrementing the counter. Every later occurrence in PP-token
  /// order must then be forced to remain expanded as well.
  SmallVector<ForcedMacroPatchRequest, 32>
  ComputeForcedCounterPatchesFromExpandedMacros(
      StringRef tuPath,
      const DenseMap<std::optional<uint64_t>, DenseMap<uint64_t, MacroPatch>>
          &macroPatchByOwnerByMacroId) const;

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
      DenseMap<std::optional<uint64_t>, DenseMap<uint64_t, MacroPatch>>
          &macroPatchByOwnerByMacroId) const;

  /// \brief Compute the A-token interval used for whole-cover replacement of
  ///        a macro invocation.
  ///
  /// This usually matches \c m.cover, but some zero-parameter function-like
  /// macros are conservatively widened by the producer when nested in parent
  /// arguments. In those cases, the precise replacement slice is derived from
  /// the bounding box of \c bodySpans.
  std::optional<std::pair<uint64_t, uint64_t>>
  GetWholeCoverATokRange(const RefoldModel::MacroInvocation &m) const;

  /// \brief Return true when the invocation's whole-cover replacement surface
  ///        is self-contained at the callsite.
  ///
  /// Whole-cover replacement is only valid when every A token in the chosen
  /// cover interval is claimed by this invocation through its own body, arg,
  /// stringify, or paste spans. Nested child invocations whose emitted tokens
  /// are interleaved with parent-owned syntax are not self-contained and must
  /// be lifted through an ancestor rather than replaced at the child callsite.
  bool MacroWholeCoverIsSelfContained(
      const RefoldModel::MacroInvocation &m) const;

  /// \brief Compute whole-cover replacement text for a macro invocation.
  ///
  /// Maps the invocation's A-domain PP-token cover interval to the
  /// corresponding B-domain token envelope and returns the B bytes that should
  /// replace the callsite. The mapping is structural and claim-aware (B-only
  /// insertion segments that are committed to standalone emission are clipped
  /// out so they are not double-emitted).
  std::optional<WholeCoverPlan>
  ComputeWholeCoverPlan(const RefoldModel::MacroInvocation &m) const;

  bool WholeCoverPatchMatchesPlan(const MacroPatch &patch,
                                  const WholeCoverPlan &plan,
                                  uint64_t rootMacroId) const;

  /// \brief Normalize the current hunk owner into the certificate shape used
  ///        for structural macro patch continuity.
  Owner NormalizeHunkOwnerForPatch(StringRef tuPath,
                                   const diffutils::Hunk &h) const;

  /// \brief Return whether a macro patch carries the same non-mixed owner
  ///        certificate as the current hunk owner.
  bool MacroPatchOwnerMatches(const MacroPatch &patch,
                              const Owner &owner) const;

  /// \brief Preserve any prior owner certificate from \p src on \p dst.
  void CarryMacroPatchOwnerCertificate(MacroPatch &dst,
                                       const MacroPatch &src) const;

  /// \brief Stamp the current owner witness onto a macro patch.
  void StampMacroPatchOwnerWitness(MacroPatch &patch,
                                   const Owner &owner) const;

  /// \brief Build the normalized proof summary for a macro patch.
  ///
  /// This bridges the macro-specific proof fields onto the normalized proof
  /// summary. Converted selector sites already use the resulting lattice and
  /// discharge metadata operationally; non-converted sites still mirror their
  /// chosen patch through this same summary for auditability.
  ProofSummary ClassifyMacroPatchProof(const MacroPatch &patch) const;

  /// \brief Synchronize the Step-1 proof summary with the legacy macro patch
  /// proof fields.
  ///
  /// During the migration, the legacy fields remain the behaviorally
  /// authoritative source of truth. This helper mirrors them into the generic
  /// proof summary so later steps can switch over incrementally.
  void SyncMacroPatchProofSummary(MacroPatch &patch) const;

  /// \brief Assign a macro patch proof in one place and update the normalized
  /// proof summary at the same time.
  void StampMacroPatchProof(MacroPatch &patch, MacroPatchProofKind kind,
                            bool validated, bool structurePreserving,
                            uint64_t proofRootMacroId) const;

  /// \brief Materialize the explicit Step-4 whole-cover realization proof.
  ///
  /// Whole-cover output is no longer tracked as an anonymous fallback result.
  /// This helper stamps the accepted patch as a first-class invocation
  /// realization proof and copies the exact realization envelope derived by
  /// ComputeWholeCoverPlan() into the patch-local certificate fields.
  void StampMacroWholeCoverRealizationPatch(
      MacroPatch &patch, const WholeCoverPlan &plan,
      uint64_t proofRootMacroId) const;

  /// \brief Classify the current accepted path inventory for a macro patch.
  AcceptancePathInventory
  InventoryMacroPatchAcceptancePath(const MacroPatch &patch) const;

  /// \brief Complete the Step-2 inventory mapping for a named accepted path.
  ///
  /// This centralizes the mapping from "how the engine accepts a result
  /// today" to "which future proof class should replace that path."
  AcceptancePathInventory
  BuildAcceptancePathInventory(AcceptedPathKind currentPath) const;

  /// \brief Build a normalized proof summary for a concrete accepted path.
  ///
  /// Step 3 uses this helper to attach class-local obligation/discharge
  /// metadata to non-macro accepted paths such as include anchors and TU
  /// anchors. Later steps may supply an explicit TU/include witness so the
  /// accepted-path audit can report the exact deterministic anchor or mapped
  /// byte range that was used. When \p patch is null, only obligations that
  /// can be discharged from the path classification itself are evaluated.
  ProofSummary BuildAcceptedPathProofSummary(
      AcceptedPathKind currentPath, const IncludePatch *patch = nullptr,
      const TUAnchorWitness *tuAnchorWitness = nullptr,
      const IncludeAnchorWitness *includeAnchorWitness = nullptr,
      const IncludeRealizationWitness *includeRealizationWitness = nullptr,
      const TerminalFallbackWitness *terminalFallbackWitness = nullptr) const;

  /// \brief Build the internal working proof summary for an include patch.
  ///
  /// Include patches are created before materialization chooses a concrete
  /// preserving anchor or realization envelope. The resulting summary therefore
  /// is only a staging object, so the default summary intentionally avoids
  /// claiming any normalized accepted path. Callers must restamp the emitted
  /// accepted result onto a concrete witness-backed include class once
  /// materialization chooses the final path.
  ProofSummary BuildIncludePatchProofSummary(
      bool realizedSurface,
      AcceptedPathKind currentPath = AcceptedPathKind::Unknown,
      const IncludePatch *patch = nullptr) const;

  /// \brief Compute the current global lattice law for an accepted summary.
  ///
  /// Step 10 centralizes the merge/conflict policy that already exists across
  /// macro, include, TU-anchor, and terminal-fallback paths. Patch B starts
  /// using that lattice at converted competition sites; later work will still
  /// be needed before every selector in the engine is routed through it.
  GlobalSelectionLattice
  BuildGlobalSelectionLattice(const ProofSummary &summary) const;

  /// \brief Compute the completeness contract for an accepted summary.
  ///
  /// This is the machine-readable form of the declared-domain statement above:
  /// a theorem-facing summary either belongs to a declared proof class, is an
  /// internal-only transitional state that must not survive to emission, or is
  /// explicitly outside the declared set as a named terminal boundary.
  CompletenessContract
  BuildCompletenessContract(const ProofSummary &summary) const;

  /// \brief Compute the explicit theorem-domain contract for an accepted summary.
  ///
  /// This must stay definitionally aligned with the completeness contract and
  /// theorem audit: theorem-facing summaries are in-domain only when they are
  /// declared, explicit-proof-backed, locally discharged, lattice-resolved, and
  /// free of unresolved owner / envelope exclusions other than named terminal
  /// out-of-domain results.
  TheoremDomainContract
  BuildTheoremDomainContract(const ProofSummary &summary) const;

  /// \brief Return whether the normalized lattice prefers \p lhs over \p rhs.
  ///
  /// Patch B uses this comparator directly at the converted selection sites.
  /// The ordering is deterministic and stable-on-ties so equal summaries can
  /// preserve the existing caller-supplied precedence order.
  bool LatticePrefers(const ProofSummary &lhs,
                      const ProofSummary &rhs) const;

  /// \brief Wrap an already-accepted macro patch in the normalized candidate
  /// carrier introduced by Patch A.
  AcceptedResultCandidate
  BuildAcceptedMacroCandidate(const MacroPatch &patch) const;

  /// \brief Wrap a macro patch in the discharged carrier used at the actual
  /// byte-edit emission boundary.
  ///
  /// Step 2 separates selector admissibility from emitted semantic proof for
  /// nested invocation-preserving macro artifacts. Selector competition still
  /// uses BuildAcceptedMacroCandidate(), which retains the top-level proof-root
  /// obligation for final competition. Once a nested preserving artifact has
  /// already been selected by an internal construction flow and is about to be
  /// emitted as a concrete TextEdit, this helper rebuilds its normalized proof
  /// summary with the emission-specific preserving validator so the byte-edit
  /// boundary sees a fully discharged carrier rather than a selector-only
  /// exception.
  AcceptedResultCandidate
  BuildAcceptedEmittedMacroCandidate(const MacroPatch &patch) const;

  /// \brief Wrap an already-accepted include path in the normalized candidate
  /// carrier introduced by Patch A.
  AcceptedResultCandidate BuildAcceptedIncludeCandidate(
      AcceptedPathKind currentPath, const IncludePatch &patch,
      const IncludeAnchorWitness *includeAnchorWitness = nullptr,
      const IncludeRealizationWitness *includeRealizationWitness = nullptr)
      const;

  /// \brief Wrap an emitted include realization in the normalized candidate
  /// carrier.
  AcceptedResultCandidate BuildAcceptedIncludeRealizationCandidate(
      AcceptedPathKind currentPath, const RefoldModel::IncludeItem &include,
      const IncludeRealizationWitness *includeRealizationWitness = nullptr)
      const;

  /// \brief Wrap an already-accepted TU anchor in the normalized candidate
  /// carrier introduced by Patch A.
  AcceptedResultCandidate BuildAcceptedTUAnchorCandidate(
      AcceptedPathKind currentPath, const TUAnchorWitness &witness) const;

  /// \brief Wrap a direct TU byte-span edit in the normalized candidate
  /// carrier.
  AcceptedResultCandidate BuildAcceptedTUTextEditCandidate(
      AcceptedPathKind currentPath, uint64_t begin, uint64_t end,
      StringRef payloadPreview = StringRef()) const;

  /// \brief Wrap the explicit terminal out-of-domain result in the normalized
  /// candidate carrier introduced by Patch A.
  AcceptedResultCandidate
  BuildAcceptedTerminalCandidate(const TerminalFallbackWitness &witness) const;

  /// \brief Return whether a normalized accepted result is admissible for
  /// lattice-based selection at the converted selector sites.
  ///
  /// Patch C makes proof discharge the participation gate for the converted
  /// selector sites. A non-terminal candidate may participate only when its
  /// declared proof class discharged successfully; the explicit terminal
  /// out-of-domain result remains selectable only as the named terminal
  /// rejection class.
  bool IsSelectableAcceptedResultCandidate(
      const AcceptedResultCandidate &candidate) const;

  /// \brief Return whether \p lhs outranks \p rhs under the normalized
  /// candidate ordering used by Patch B.
  ///
  /// This lifts the proof-summary lattice comparison to the accepted-result
  /// carrier and then applies deterministic artifact-local tie-breakers. When
  /// two candidates are still indistinguishable after those tie-breakers, the
  /// caller's original enumeration order is preserved.
  bool AcceptedResultCandidatePrefers(
      const AcceptedResultCandidate &lhs,
      const AcceptedResultCandidate &rhs) const;

  /// \brief Return whether \p candidate failed only the nested-macro
  /// top-level selector rule.
  ///
  /// Step 1 removes the last direct macro-selector bypass by letting nested
  /// DAG/callsite preservation artifacts flow through the explicit candidate
  /// selector instead of returning them directly. Those intermediate nested
  /// artifacts are still rejected by the theorem-facing discharge rule that
  /// requires a top-level proof root, so this helper isolates the one
  /// narrowly-scoped selector-only failure that the non-top-level macro
  /// construction site may admit locally while preserving the existing final
  /// top-level gating everywhere else.
  bool AcceptedResultCandidateHasOnlyNonTopLevelMacroSelectorFailure(
      const AcceptedResultCandidate &candidate) const;

  /// \brief Return the index of the strongest selectable accepted candidate.
  ///
  /// The returned index always refers to the original \p candidates order so
  /// callers can keep artifact ownership outside the selector while still
  /// centralizing the lattice-based comparison logic. When
  /// \p allowNonTopLevelMacroSelectorFailure is true, the selector also admits
  /// nested macro-preserving artifacts whose only failed obligation is the
  /// top-level proof-root rule; this is reserved for the internal non-top-
  /// level macro construction site that still needs those artifacts as
  /// intermediate results.
  std::optional<size_t> SelectPreferredAcceptedResultCandidateIndex(
      ArrayRef<AcceptedResultCandidate> candidates,
      bool allowNonTopLevelMacroSelectorFailure = false) const;

  /// \brief Return whether \p m carries usable producer-side paste witnesses.
  ///
  /// Step 5 serialized the producer's exact `##` decomposition into the model.
  /// Patch C may discharge a paste-preserving args-only class from either of
  /// two deterministic proof sources: a usable producer-side root witness
  /// stream, checked here, or an explicit direct replay check recorded on the
  /// accepted patch. This helper validates only the producer-side witness
  /// source.
  ///
  /// The producer records only the argument-derived fragments of each pasted
  /// token; literal glue bytes from the macro body (for example the `_` in
  /// `X##_##Y`) may appear as gaps between recorded parts. The helper is
  /// therefore strict about ordered, non-overlapping half-open ranges that
  /// stay within the final spelling, but it does not require the recorded
  /// parts to form a contiguous partition of the pasted token. It also accepts
  /// the parent-level case where `pasteSpans` are only propagated child-paste
  /// contributors inside a standard occurrence of the same formal, because
  /// those spans are validated by the args-only replay proof rather than by a
  /// direct parent-level `paste_tokens` decomposition.
  bool MacroInvocationHasWellFormedPasteWitnesses(
      const RefoldModel::MacroInvocation &m) const;

  /// \brief Validate the current local proof contract for an accepted class.
  ///
  /// These validators lift the existing deterministic checks into named local
  /// obligations. Converted selector sites already use their discharge result
  /// operationally; remaining sites still mirror their facts into the same
  /// proof contract until they are moved onto the normalized selector path.
  ProofDischargeRecord
  ValidateInvocationPreservingProof(const MacroPatch &patch) const;

  /// \brief Validate the emitted semantic proof contract for a preserving
  /// macro patch.
  ///
  /// Step 2 removes the byte-edit boundary's selector-only exception by
  /// rebuilding emitted nested preserving macro carriers under this validator.
  /// It discharges the same semantic obligations as
  /// ValidateInvocationPreservingProof(), but it does not re-impose the
  /// top-level proof-root selector rule that is only relevant while competing
  /// for final selection.
  ProofDischargeRecord
  ValidateEmittedInvocationPreservingProof(const MacroPatch &patch) const;
  ProofDischargeRecord
  ValidateInvocationRealizationProof(const MacroPatch &patch) const;
  ProofDischargeRecord ValidateIncludePreservingProof(
      AcceptedPathKind currentPath, const IncludePatch *patch,
      const IncludeAnchorWitness *witness = nullptr) const;
  /// \brief Return whether an include-realization witness is inside the
  /// declared Step-4A realization domain.
  ///
  /// Inline include realization is first-class only when the witness records a
  /// canonical B-cover envelope or a deterministic consensus-rescued B-cover
  /// envelope. Unknown evidence kinds remain outside the declared theorem
  /// domain and must surface only through the explicit terminal fallback.
  bool IsAcceptedIncludeRealizationEvidenceKind(
      IncludeRealizationEvidenceKind kind) const;
  ProofDischargeRecord ValidateIncludeRealizationProof(
      AcceptedPathKind currentPath, const IncludePatch *patch,
      const IncludeRealizationWitness *witness = nullptr) const;
  ProofDischargeRecord ValidateTUAnchorProof(
      AcceptedPathKind currentPath,
      const TUAnchorWitness *witness = nullptr) const;

  /// \brief Formatters for the normalized Step-1/2/3 proof metadata.
  StringRef FormatTUAnchorEvidenceKind(TUAnchorEvidenceKind kind) const;
  StringRef FormatIncludeAnchorEvidenceKind(IncludeAnchorEvidenceKind kind) const;
  StringRef FormatIncludeRealizationEvidenceKind(
      IncludeRealizationEvidenceKind kind) const;

  /// \brief Format a concrete TU anchor witness for tracing.
  ///
  /// Step 7 records the exact deterministic TU evidence used to justify a
  /// successful TU anchor. This formatter keeps that witness readable in the
  /// same audit stream as the normalized proof/discharge metadata.
  std::string FormatTUAnchorWitness(const TUAnchorWitness &witness) const;

  /// \brief Format a concrete include anchor witness for tracing.
  std::string FormatIncludeAnchorWitness(const IncludeAnchorWitness &witness) const;

  /// \brief Format a concrete include-realization witness for tracing.
  ///
  /// Step 9 records the exact deterministic include cover and mapped B-token
  /// envelope used when the engine realizes an include directly from the
  /// modified preprocessed surface. This formatter keeps that witness readable
  /// in the same audit stream as the normalized proof/discharge metadata.
  std::string
  FormatIncludeRealizationWitness(
      const IncludeRealizationWitness &witness) const;

  /// \brief Build the explicit terminal-fallback witness for the current pass.
  TerminalFallbackWitness BuildTerminalFallbackWitness() const;

  /// \brief Return whether the hunk lies on the explicit unresolved-owner
  /// / no-TU-anchor theorem boundary.
  ///
  /// Step 5 chooses the conservative domain-wall interpretation for the last
  /// ownership gap. This predicate does not search for any new witness. It only
  /// re-states the evidence that has already been exhausted:
  ///
  /// * no resolved macro/TU/include owner remained,
  /// * no exact include-boundary owner exists for a pure insertion,
  /// * no truthful TU-owned mapped span exists for the A interval, and
  /// * for pure insertions, no exact/provable TU insertion anchor exists.
  ///
  /// When all of those facts hold, the edit is explicitly outside the declared
  /// structural refolding domain and must terminate via
  /// `OwnerUnresolvedNoTUAnchor`.
  bool IsOwnerUnresolvedNoTUAnchorOutOfDomain(
      const diffutils::Hunk &h, StringRef tuPath, const Owner &owner,
      bool mapsToTU) const;

  /// \brief Build a detailed terminal-fallback reason for the unresolved-owner
  /// / no-TU-anchor domain wall.
  ///
  /// This helper records the deterministic owner and TU-anchor searches that
  /// were already exhausted before the engine concluded that no declared
  /// macro/include/TU proof class could own the edit. It does not guess a new
  /// owner or widen admissibility.
  std::string BuildOwnerUnresolvedNoTUAnchorDetail(
      size_t hunkIndex, const diffutils::Hunk &h, StringRef tuPath,
      const Owner &owner, bool mapsToTU) const;

  /// \brief Format the explicit terminal-fallback witness for tracing.
  std::string
  FormatTerminalFallbackWitness(const TerminalFallbackWitness &witness) const;

  /// \brief Format a patch proof kind for tracing.
  StringRef FormatMacroPatchProofKind(MacroPatchProofKind kind) const;

  /// \brief Format the Step-2 acceptance-path inventory for tracing.
  std::string
  FormatAcceptancePathInventory(const AcceptancePathInventory &inventory) const;

  /// \brief Format a Step-3 proof-discharge record for tracing.
  std::string
  FormatProofDischargeRecord(const ProofDischargeRecord &record) const;

  /// \brief Format the normalized Step-10 lattice law for tracing.
  std::string
  FormatGlobalSelectionLattice(const GlobalSelectionLattice &lattice) const;

  /// \brief Format the normalized Step-11 completeness contract for tracing.
  std::string
  FormatCompletenessContract(const CompletenessContract &contract) const;

  /// \brief Format the normalized theorem-domain contract for tracing.
  std::string
  FormatTheoremDomainContract(const TheoremDomainContract &contract) const;

  /// \brief Format the normalized accepted-path audit for tracing.
  ///
  /// TU anchor paths do not materialize through IncludePatch, so Step 7 allows
  /// callers to provide an explicit TU witness directly when formatting an
  /// accepted-path audit entry.
  std::string FormatAcceptedPathAudit(
      AcceptedPathKind currentPath, const IncludePatch *patch = nullptr,
      const TUAnchorWitness *tuAnchorWitness = nullptr,
      const IncludeAnchorWitness *includeAnchorWitness = nullptr,
      const IncludeRealizationWitness *includeRealizationWitness = nullptr,
      const TerminalFallbackWitness *terminalFallbackWitness = nullptr) const;

  /// \brief Format a normalized accepted-result candidate for tracing.
  std::string
  FormatAcceptedResultCandidate(const AcceptedResultCandidate &candidate) const;

  /// \brief Format patch provenance and subtree-composition audit metadata.
  std::string FormatMacroPatchAudit(const MacroPatch &patch) const;

  std::optional<std::string>
  BuildWholeCoverReplacementText(const RefoldModel::MacroInvocation &m) const;

  /// \brief Build a macro callsite patch for an invocation using only
  /// span-driven evidence.
  ///
  /// This routine produces the replacement text for a macro invocation at its
  /// callsite byte span \c [m.invB, m.invE) in the owning file. The strategy
  /// is intentionally deterministic and avoids heuristic “best-looking slice"
  /// selection.
  ///
  /// The patching strategy is:
  ///
  /// 1. **Do-not-downgrade guard:** If an existing patch for this invocation
  ///    already exists in \p patchMap and its replacement no longer resembles a
  ///    callsite invocation (i.e. the macro has effectively been realized/
  ///    expanded), preserve and return that patch unchanged.
  ///
  /// 2. **Args-only rewrite (preferred when safe):** If the diff hunk \p h is
  ///    fully contained within the invocation's argument-like spans (normal
  ///    arguments, stringify spans, and paste spans), attempt a surgical patch
  ///    using \c BuildMacroInvocationPatchArgsOnly. This preserves the original
  ///    callsite formatting and whitespace. Args-only rewriting is only
  ///    attempted when the provided invocation text still matches the expected
  ///    callsite prefix for \p m.
  ///
  /// 3. **Whole-cover fallback:** If args-only patching is not applicable or
  ///    fails, replace the entire invocation by mapping the A-domain macro cover
  ///    interval \c [coverBegin, coverEnd) to a B-domain token envelope via \c
  ///    MapATokRangeAToBTokenEnvelope and slicing the corresponding region from
  ///    the edited preprocessed output. The B-envelope is then tightened when
  ///    possible by re-aligning the first/last token to the exact A-boundary
  ///    tokens (only if the expected token exists as an immediately adjacent
  ///    neighbor in B). This corrects common alignment drops of low-information
  ///    punctuation (e.g. leading '(') without scanning or heuristics.
  ///
  /// \param m Metadata for the macro invocation, including callsite byte span
  ///          and A-domain cover.
  /// \param h The diff hunk associated with this macro's region.
  /// \param baseInvText The original callsite invocation text from the source
  ///                    file.
  /// \param patchMap Previously computed macro patches indexed by owner id then
  ///                 macro id; used only to preserve existing non-callsite
  ///                 replacements for this invocation.
  /// \returns A \c MacroPatch for the callsite span if a deterministic
  ///          replacement can be built; otherwise \c std::nullopt (e.g. invalid
  ///          invocation span, invalid cover, or failure to compute a valid
  ///          B-token envelope).
  std::optional<MacroPatch> BuildMacroInvocationPatchWholeCover(
      const RefoldModel::MacroInvocation &m, const diffutils::Hunk &h,
      StringRef baseInvText,
      const DenseMap<std::optional<uint64_t>, DenseMap<uint64_t, MacroPatch>>
          &patchMap) const;

  /// \brief Return whether a nested macro may use whole-cover fallback at its
  ///        own callsite.
  ///
  /// Whole-cover replacement of a nested child macro is only safe when the
  /// child's cover interval is entirely accounted for by tokens owned by that
  /// child (or its descendants). If the cover includes caller-owned tokens,
  /// replacing just the child invocation with the child cover would splice a
  /// partial expanded surface into the parent callsite.
  ///
  /// Top-level macros are always eligible. Nested macros are eligible only when
  /// every token in \c m.cover is covered by authoritative subtree-owned spans
  /// (body/arg/stringify/paste, with a spans fallback only for nodes that lack
  /// detailed span metadata).
  bool NestedWholeCoverIsSelfContained(
      const RefoldModel::MacroInvocation &m) const;

  /// \brief Validates that a raw source span contains a valid macro callsite
  /// prefix matching the invocation metadata.
  ///
  /// This check ensures that the text at the projected invocation coordinate
  /// starts with the correct macro name and, for function-like macros, is
  /// immediately followed by an opening parenthesis. This provides a safety
  /// proof that the refold mapping correctly aligned with the actual source
  /// code callsite before attempting an args-only rewrite.
  ///
  /// \param invSpanText The raw text extracted from the source file at the
  ///                    projected invocation span.
  /// \param m The macro invocation metadata containing the expected name
  ///          and subkind (object-like vs function-like).
  /// \returns True if the text contains a valid prefix for the macro call.
  bool InvocationSpanMatchesCallsitePrefix(
      StringRef invSpanText, const RefoldModel::MacroInvocation &m) const;

  /// \brief Look up a macro invocation record by its stable refold-model id.
  ///
  /// The refold metadata stores each macro invocation with a unique id and may
  /// reference that id from nested invocations, patches, and statistics logic.
  /// This helper performs a linear scan over the recorded invocation list and
  /// returns the matching metadata node when present.
  ///
  /// \param macroId Model-assigned macro invocation id to resolve.
  /// \returns The matching \c MacroInvocation, or \c nullptr if the id is not
  ///          present in the loaded model.
  const RefoldModel::MacroInvocation *
  FindMacroInvocationById(uint64_t macroId) const {
    for (const auto &mi : model_.GetMacroInvocations()) {
      if (mi.id == macroId)
        return &mi;
    }
    return nullptr;
  }

  /// \brief Return the top-level macro invocation that owns \p macroId.
  ///
  /// Macro invocations in the refold model may form a caller chain via
  /// \c callerMacroId when one macro expansion is nested within another. This
  /// helper follows that chain to the unique root invocation representing the
  /// user-visible callsite in source.
  ///
  /// \param macroId Model-assigned macro invocation id whose root should be
  ///        computed.
  /// \returns The id of the outermost invocation in the caller chain. If
  ///          \p macroId is unknown, returns \p macroId unchanged.
  uint64_t GetRootMacroId(uint64_t macroId) const;

  /// \brief Return whether a macro patch leaves its owning root callsite
  ///        expanded in the final refolded source.
  ///
  /// A patch is considered to remain expanded when its replacement text does
  /// not reconstruct the original macro call spelling at the root callsite.
  /// This predicate is used only for statistics attribution: nested/internal
  /// macro work is charged back to the root user-visible invocation exactly
  /// once.
  ///
  /// \param patch Macro patch candidate to classify.
  /// \returns True if the patch should count as an expanded macro in the final
  ///          statistics, false if it preserves/reconstructs the macro callsite.
  bool MacroPatchRemainsExpanded(const MacroPatch &patch) const {
    const RefoldModel::MacroInvocation *mi =
        FindMacroInvocationById(patch.macroId);
    if (!mi)
      return true;
    return !InvocationSpanMatchesCallsitePrefix(patch.replacement, *mi);
  }

  /// \brief Applies a deterministic, stable ordering to include-scoped
  /// insertion patches.
  ///
  /// During refolding, multiple insertions can be attributed to the same
  /// include instance. The refold pipeline may discover or enqueue these
  /// patches in an order that depends on iteration order (e.g., map traversal)
  /// or on incidental properties of the diff/LCS reconstruction. This helper
  /// normalizes that by sorting each include's patch list into a canonical
  /// order.
  ///
  /// \par Ordering rule
  /// For each IncludeEdits entry in \p perInclude, this method sorts
  /// IncludeEdits::patches by:
  /// 1. IncludePatch::aStart (primary key; A-side insertion point in token
  /// space)
  /// 2. IncludePatch::bStart (secondary key; B-side position used to derive
  ///    the payload)
  ///
  /// \par Non-goals
  /// - No widening, merging, coalescing, or re-chunking is performed.
  /// - No payload rewriting occurs; the patch text/slices remain exactly as
  ///   computed earlier.
  /// - No ownership decisions are made here; this function only reorders
  ///   already-attributed patches.
  ///
  /// The \c bSource_ and \c bTokOff_ member variables are available for
  /// potential future diagnostics, but are not used by the current ordering
  /// logic.
  ///
  /// \param perInclude Mapping from include id to accumulated edits/patches for
  ///                   that include; each non-empty patch list is sorted in
  ///                   place.
  void
  OrderIncludeInsertions(DenseMap<uint64_t, IncludeEdits> &perInclude) const {
    for (auto &it : perInclude) {
      IncludeEdits &ie = it.second;
      if (ie.patches.empty())
        continue;

      // Use llvm::sort for a highly optimized stable sort.
      // We sort by aStart primarily and bStart secondarily.
      llvm::sort(ie.patches,
                 [](const IncludePatch &lhs, const IncludePatch &rhs) {
                   if (lhs.aStart != rhs.aStart)
                     return lhs.aStart < rhs.aStart;
                   return lhs.bStart < rhs.bStart;
                 });
    }
  }

  /// \brief Fully materialize the bytes for a single `#include` instance and
  ///        cache the result.
  ///
  /// Materialization is **depth-first** and **deterministic**. Starting from
  /// the include’s raw header bytes (preseeded in `includeExpansion` or read
  /// from disk), this method:
  ///
  /// 1. **Applies macro patches owned by this include** — each `MacroPatch`
  ///    replaces the invocation byte range `[invStart, invEnd)` in the
  ///    include’s own byte space.
  /// 2. **Applies A/B include patches** — insert/delete/replace edits derived
  ///    from the B text are applied within the header using
  ///    `applyIncludeInsertions()`.
  /// 3. **Recursively expands child includes that have work** — for each direct
  ///    child whose subtree contains edits or macro patches (as determined by
  ///    `hasDescendantWork()`), first materialize the child, then replace the
  ///    child’s directive site `[siteB, siteE)` (recorded in the parent’s byte
  ///    space) with the child’s fully materialized bytes.
  ///
  /// Edits collected for the current header are applied
  /// **highest-offset-first** to keep indices stable. The final materialized
  /// bytes are memoized into `includeExpansion` under `includeId`. If an entry
  /// already exists, the method returns immediately (idempotent).
  ///
  /// #### Coordinate Spaces
  /// * **siteB / siteE:** byte offsets in the includer’s file where the child
  ///   directive appears.
  /// * **Macro patches:** byte offsets in the owning include’s file.
  /// * **Coverage (coverBegin / coverEnd):** A-token indices; used to decide
  ///   ownership, not for byte replacement.
  ///
  /// \param includeId           Unique ID of the include to materialize.
  /// \param perInclude          Map of include ID → include-level A/B edits
  ///                            (insert/delete/replace).
  /// \param macroPatchesByOwner Map of owner ID → macro patches whose
  ///                            invocation ranges are expressed in that owner’s
  ///                            byte space. Include-owned patches are applied
  ///                            directly here; TU-owned patches are handled by
  ///                            the TU materialization path.
  /// \param children            Map of parent include ID → direct child include
  ///                            items (with `siteB` / `siteE` in the parent).
  /// \param includeExpansion    Cache/output: include ID → fully materialized
  ///                            header bytes; may be preseeded with raw header
  ///                            text.
  /// \param appliedExpandedMacroRootIds Optional output set that, when non-null,
  ///                            records root macro invocation ids for macro
  ///                            patches that remain expanded after being applied
  ///                            while materializing this include subtree.
  void MaterializeIncludeExpansion(
      uint64_t includeId, const DenseMap<uint64_t, IncludeEdits> &perInclude,
      const DenseMap<std::optional<uint64_t>, std::vector<MacroPatch>>
          &macroPatchesByOwner,
      const DenseMap<uint64_t, std::vector<const RefoldModel::IncludeItem *>>
          &children,
      DenseMap<uint64_t, std::string> &includeExpansion,
      DenseMap<uint64_t, AcceptedResultCandidate>
          &includeExpansionAcceptedResults,
      DenseSet<uint64_t> *appliedExpandedMacroRootIds = nullptr) const;

  // Realize an include directly from the edited preprocessed stream B. This is
  // the single-pass include-realization path used when local include
  // preservation cannot discharge a deterministic anchored edit plan.
  std::optional<std::string>
  BuildInlineIncludeRealizationFromB(
      const RefoldModel::IncludeItem &inc, llvm::StringRef reason,
      AcceptedResultCandidate *acceptedCandidate = nullptr) const;

  /// \brief Selects the most appropriate HeaderDecl within an IncludeItem to
  /// serve as the declaration-level anchor for an include-scoped patch.
  ///
  /// Include edits are often more robust when anchored to a concrete
  /// declaration region rather than to the entire header. The refold model may
  /// provide a set of HeaderDecl entries for each include, each with a PP-space
  /// span (HeaderDecl::ppSpan) describing the declaration's extent in the
  /// preprocessed token/index space. This method chooses a single "best"
  /// declaration to which a patch should attach.
  ///
  /// \par Selection strategy
  ///
  /// The method considers the patch's A-side PP-token interval [\p p.aStart, \p
  /// p.aEnd) and applies the following rules:
  /// 1. **Prefer full coverage:** choose a declaration whose span fully
  /// contains the
  ///    patch interval.
  /// 2. **Otherwise allow overlap:** if no declaration fully contains the
  /// interval,
  ///    choose a declaration whose span merely overlaps the interval.
  /// 3. **Most specific wins:** if multiple candidates satisfy a rule, pick the
  /// one
  ///    with the smallest span length (ppSpan.end - ppSpan.begin) to obtain the
  ///    most specific (tightest) anchor.
  ///
  /// \par Pure insertions
  ///
  /// If the patch is a pure insertion (\p p.aStart == \p p.aEnd), the insertion
  /// is treated as a single attachment point at \c aLo = \p p.aStart. A
  /// declaration is considered a candidate if \c aLo lies within the
  /// declaration's span. Insertions anchored exactly at a declaration end are
  /// treated as belonging to that declaration (i.e., end-inclusive for this
  /// special case).
  ///
  /// \par Return value
  ///
  /// If a full-cover candidate exists, it is returned. Otherwise, the best
  /// overlap candidate is returned. If no declaration is relevant (e.g., the
  /// patch falls in a header preamble region not covered by any recorded
  /// HeaderDecl, or the edit is in macro-only glue that has no associated decl
  /// span), this method returns \c nullptr.
  ///
  /// \param inc the include instance containing declaration metadata
  ///        (IncludeItem::decls)
  /// \param p the include-scoped patch whose A-side interval is used for
  ///        anchor selection
  /// \return the best-matching HeaderDecl to anchor this patch, or \c nullptr
  ///         if none is applicable
  static const RefoldModel::HeaderDecl *
  FindHeaderDeclForPatch(const RefoldModel::IncludeItem &inc,
                         const IncludePatch &p);

  /// \brief Computes (but does not apply) the header-local `TextEdit`s implied
  /// by `ie`.
  ///
  /// `IncludePatch` coordinates are expressed in A-side preprocessed (PP) token
  /// indices. This routine projects those token ranges onto `headerText` using
  /// the refold map token→(file, byte-span) mapping in
  /// `RefoldModel::tokmapByPP`.
  ///
  /// The returned edits use byte offsets into `headerText` and are sorted in
  /// descending `start` order so a caller can apply them without re-mapping
  /// subsequent offsets.
  ///
  /// ### Patch classification
  ///
  /// - **INSERT**: `aStart == aEnd` and `bStart < bEnd`. Inserts
  ///   `IncludePatch::insertBytes` at a deterministic byte anchor inside this
  ///   header.
  /// - **DELETE**: `aStart < aEnd` and `bStart == bEnd`. Deletes the mapped
  ///   A-range (replacement is `""`).
  /// - **REPLACE**: `aStart < aEnd` and `bStart < bEnd`. Replaces the mapped
  ///   A-range with `IncludePatch::insertBytes`.
  ///
  /// ### Scope and clamping
  ///
  /// - The include instance contributes a PP “cover” window `[coverBegin,
  ///   coverEnd)` describing which PP indices belong to this header instance.
  /// - A patch may be associated with an owning `HeaderDecl` via
  ///   `findHeaderDeclForPatch(...)`.
  /// - **DELETE/REPLACE** are restricted to the owning declaration when
  /// present:
  ///   the patch’s effective PP window is intersected with `decl.ppSpan` and
  ///   the resulting byte range is clamped to `[decl.headerB, decl.headerE)`.
  /// - **INSERT** intentionally operates at include scope (not decl scope) so
  ///   that an insertion on a declaration boundary can anchor to the first
  ///   token of the following declaration instead of being forced “back inside”
  ///   the previous one.
  ///
  /// ### Deterministic anchoring for INSERT
  ///
  /// For an INSERT at A-position `pos = aStart`, the insertion byte offset is
  /// chosen in the following priority order within the effective PP window:
  /// 1. **Right neighbor**: smallest `pp >= pos` mapping to this header’s file;
  ///    insert immediately *before* that token (use its byte-start).
  /// 2. **Left neighbor**: greatest `pp < pos` mapping to this file; insert
  ///    immediately *after* that token (use its byte-end).
  /// 3. **Owning decl end**: if an owning decl exists, anchor at
  ///    `decl.headerE`.
  /// 4. **Child-include boundary fallback**: attempt to synthesize a stable
  ///    anchor from child `#include` sites (via
  ///    `computeChildBoundaryInsertByte(...)`). When this path is taken, the
  ///    inserted text may be padded with `padAtBoundaries(...)`. If no anchor
  ///    can be found, the patch is skipped.
  ///
  /// ### Mapping for DELETE/REPLACE
  ///
  /// For non-empty A-ranges, the method locates all PP indices in the patch’s
  /// (possibly intersected) effective window that map into this header file.
  /// The resulting byte interval is `[byteStart(firstPP), byteEnd(lastPP))`. If
  /// no PP tokens map into this header, the patch is skipped.
  ///
  /// ### Formatting policy
  ///
  /// Replacement bytes are applied *literally*. This routine does not perform
  /// whitespace normalization, token reformatting, or “prefix surgery”;
  /// refold-map slices are treated as ground truth.
  ///
  /// \param M Refold model providing PP token→(file, byte range) mappings and
  ///          include/decl metadata.
  /// \param ie Per-include edits: the include instance plus a set of
  ///           `IncludePatch`es.
  /// \param headerText Current text of the header corresponding to
  ///                   `ie.include`.
  /// \returns Header-local `TextEdit`s (byte offsets into `headerText`), sorted
  ///          by descending `start`.
  IncludeTextEditPlan ComputeIncludeTextEdits(const IncludeEdits &ie,
                                              std::string headerText) const;

  /// \brief Computes a deterministic insertion byte offset for a header-scoped
  /// *pure INSERT* when the normal token-based anchoring mechanisms provide no
  /// usable neighbor.
  ///
  /// This is a fallback used only in degenerate header cases where:
  /// - The patch is a pure insertion (`p.aStart == p.aEnd`)
  /// - There are no mapped tokmap neighbors in `file` near `p.aStart` to anchor
  ///   on
  /// - There is no suitable `HeaderDecl` span to provide a declaration-based
  ///   anchor
  ///
  /// In such cases, we attempt to anchor relative to the literal `#include`
  /// sites that appear inside the same header file.
  ///
  /// ### Approach
  ///
  /// Let `owner` be the include instance whose header text is being edited
  /// (`p.include`). We scan the model’s includes to find *direct children* of
  /// `owner` whose `IncludeItem::sitePath` equals `file`. Each such child
  /// represents an include directive that is physically written in this header
  /// and has a PP cover window `[kid.coverBegin, kid.coverEnd)` and a site byte
  /// range `[kid.siteB, kid.siteE)` in `file`.
  ///
  /// For the insertion PP position `pos = p.aStart`, we consider only
  /// "between-children" positions:
  /// - If `pos` lies *strictly inside* a child’s cover window
  ///   (`kid.coverBegin < pos && pos < kid.coverEnd`), this fallback does not
  ///   apply and returns `std::nullopt` (the insertion should have been owned by that
  ///   child include).
  /// - Boundary positions are allowed (`pos == kid.coverBegin` or
  ///   `pos == kid.coverEnd`) and are treated as being "between" children.
  ///
  /// ### Anchor selection
  ///
  /// Among eligible children in `file`:
  /// - `left` is the child with the greatest `coverEnd` such that
  ///   `coverEnd <= pos`
  /// - `right` is the child with the smallest `coverBegin` such that
  ///   `coverBegin >= pos`
  ///
  /// The insertion anchor is then chosen deterministically:
  /// 1. If `right` exists, insert *before* the right child’s `#include` site
  ///    by returning `right.siteB`.
  /// 2. Else if `left` exists, insert *after* the left child’s `#include` site
  ///    by returning `left.siteE`.
  /// 3. Otherwise return `std::nullopt` to indicate that no sane anchor could be derived.
  ///
  /// This method does not validate that the returned site offsets are within
  /// the current header text bounds; callers should ensure the returned byte
  /// offset is usable in the current editing context.
  ///
  /// \param M The refold model providing include hierarchy, PP cover windows,
  ///          and include-site byte ranges.
  /// \param p The include-scoped patch (expected to be a pure insertion) whose
  ///          PP insertion position is `p.aStart`.
  /// \param file The header file path whose text is being edited; only child
  ///             includes whose `sitePath` equals `file` are considered as
  ///             anchors.
  /// \param witness Optional Step-8 witness sink that records which child
  ///               boundary was chosen when the fallback succeeds.
  /// \return A byte offset within `file` at which the insertion should be
  /// applied,
  ///         or `std::nullopt` if this fallback does not apply or no stable anchor can be
  ///         found.
  std::optional<uint64_t> ComputeChildBoundaryInsertByte(
      const IncludePatch &p, StringRef file,
      IncludeAnchorWitness *witness = nullptr) const;

  /// \brief Creates a TextEdit for [start,end) in original that preserves
  /// __LINE__ transparency.
  ///
  /// If the replacement changes the newline count relative to the removed span,
  /// this method will attempt to keep line accounting correct by either:
  /// * injecting a local #line directive inside the replacement (when safe), or
  /// * deferring the correction by attaching a PendingResync to the returned
  ///   edit, to be flushed later by the file-emission layer.
  ///
  /// The returned edit always stores the original span boundaries unchanged,
  /// and stores either:
  /// * text = injectedReplacement, pending = nullopt on successful local
  ///   injection, or
  /// * text = replacement, pending = PendingResync(fileSpelling) when
  ///   deferring.
  ///
  /// \param original The pre-edit file contents the edit coordinates refer to.
  /// \param start Start offset (inclusive) in original.
  /// \param end End offset (exclusive) in original.
  /// \param replacement Replacement text to emit for [start,end).
  /// \param fileSpelling The producer-provided spelled path to use in any
  ///        emitted #line directive.
  /// \return A TextEdit representing the change and (optionally) a pending
  ///         resync to flush later.
  TextEdit MakeTextEditWithResyncOrPending(StringRef original, uint64_t start,
                                           uint64_t end, StringRef replacement,
                                           StringRef fileSpelling) const {
    ResyncOutcome o =
        ApplyResyncOrPend(original, start, end, replacement, fileSpelling);
    return TextEdit{start, end, std::move(o.text), std::move(o.pending),
                    std::nullopt, {}};
  }

  /// \brief Computes how to preserve __LINE__ after applying replacement to
  /// [start,end) in originalFileText.
  ///
  /// This method detects "line drift" by comparing the newline count in the
  /// original span versus the replacement text. If there is no drift, it
  /// returns (replacement, nullopt).
  ///
  /// If drift is detected, the method:
  /// 1. Computes the logical resume line for the first character at `end` in
  ///    the original file,
  /// 2. Attempts a local resync by calling
  ///    LineDirectiveInserter::maybeAppendResyncAfterReplacement,
  /// 3. If local injection succeeds, returns (injectedReplacement, nullopt),
  /// 4. Otherwise, returns (replacement, PendingResync(fileSpelling)) so the
  ///    emission layer can flush a #line directive at the next safe BOL.
  ///
  /// Safety note: local injection may fail when inserting a directive would
  /// change token adjacency (e.g., when the replacement ends mid-line, or when
  /// no safe BOL exists in/around the replacement).
  ///
  /// \param originalFileText Pre-edit file contents the offsets refer to.
  /// \param start Start offset (inclusive) in originalFileText.
  /// \param end End offset (exclusive) in originalFileText.
  /// \param replacement Replacement text.
  /// \param fileSpellingForDirective Path used in any injected #line.
  /// \return A ResyncOutcome containing the emitted text and optional pending
  ///         state.
  ResyncOutcome ApplyResyncOrPend(StringRef originalFileText, uint64_t start,
                                  uint64_t end, StringRef replacement,
                                  StringRef fileSpellingForDirective) const;

  /// \brief Applies a set of TextEdits to originalFileText, producing the final
  /// refolded text for a single file (TU or header), while preserving __LINE__
  /// transparency via "pending resync".
  ///
  /// ### Core responsibilities
  /// * Normalizes edits by de-duplicating exact-span edits (same [start,end)):
  ///   the last one wins.
  /// * Orders edits by increasing start (then end) and enforces non-overlap.
  /// * Streams output in order: original slices + replacement text.
  /// * When an edit carries a PendingResync, defers the #line emission until
  /// the
  ///   next safe BOL in the subsequent unchanged original text, using
  ///   AppendOriginalSliceWithPending.
  ///
  /// **Pending semantics:** if multiple edits produce pending drift and earlier
  /// pending could not be flushed yet, the "last drift wins" policy applies
  /// (the most recent PendingResync overwrites the previous one).
  ///
  /// **EOF behavior:** if a pending resync remains at end-of-file, it is
  /// dropped as harmless because there is no subsequent original code whose
  /// __LINE__ needs correction.
  ///
  /// \param originalFileText The pre-edit file contents the edits are expressed
  ///        against.
  /// \param edits Non-overlapping edits expressed in offsets of
  ///        originalFileText.
  /// \param appliedExpandedMacroRootIds Optional output set that, when non-null,
  ///        records root macro invocation ids for edits whose final applied
  ///        replacement leaves the corresponding macro expanded in the emitted
  ///        text.
  /// \return The edited file text with any necessary #line directives emitted
  ///         (locally or deferred).
  std::string ApplyTextEditsWithPendingResync(
      StringRef originalFileText, ArrayRef<TextEdit> edits,
      DenseSet<uint64_t> *appliedExpandedMacroRootIds = nullptr,
      StringRef emissionOwner = StringRef()) const;

  /// \brief Return whether an emitted non-terminal byte edit is backed only by
  /// emission-discharged normalized accepted-result carriers.
  ///
  /// Step 2 makes proof discharge the universal gate at the actual emission
  /// boundary. Every non-terminal artifact that survives to a TextEdit must
  /// carry at least one normalized accepted-result candidate, and every such
  /// carrier must already be fully discharged under the proof contract that is
  /// appropriate for emitted source text. Terminal out-of-domain results are
  /// never valid carriers for non-terminal emitted edits.
  bool EmittedTextEditHasDischargedAcceptedResults(
      const TextEdit &edit, StringRef emissionPhase,
      StringRef emissionOwner = StringRef()) const;

  /// \brief Copy one normalized accepted-result carrier onto an emitted edit.
  void AttachAcceptedResultCarrier(
      TextEdit &edit, const AcceptedResultCandidate &candidate) const;

  /// \brief Appends an unchanged slice of the original file original[from:to)
  /// into out, while attempting to flush a previously-deferred PendingResync at
  /// the earliest safe point.
  ///
  /// A pending resync represents a required logical #line correction that could
  /// not be emitted inside a prior replacement without risking token adjacency
  /// changes. This method flushes that directive when it becomes safe to do so
  /// while streaming unchanged original content.
  ///
  /// ### Flush rules
  /// 1. **Immediate flush at slice begin:** if the output is currently at BOL
  ///    and `from` is a BOL in `original`, emit the directive before appending
  ///    the slice.
  /// 2. **Otherwise:** scan forward in `original[from:to)` for the first
  /// newline
  ///    boundary that is safe (i.e., not a preprocessor line-splice such as
  ///    `\\\n`). After copying through that newline, emit the directive at the
  ///    following BOL.
  /// 3. **No-op suppression:** even when a flush location is found, the
  ///    directive is only appended if
  ///    LineDirectiveInserter::shouldEmitLineDirective indicates it would
  ///    change logical state (i.e., it is not already in the same file/line
  ///    context).
  ///
  /// If no safe flush point exists within [from,to), the pending resync is
  /// returned unchanged so it can be attempted again on the next original
  /// slice.
  ///
  /// \param out Destination output buffer for the final file emission.
  /// \param original The original file contents we are streaming from.
  /// \param from Start offset (inclusive) of the unchanged slice.
  /// \param to End offset (exclusive) of the unchanged slice.
  /// \param pending A pending resync to flush.
  /// \return std::nullopt if the pending resync was flushed; otherwise the
  ///         still-pending resync.
  std::optional<PendingResync>
  AppendOriginalSliceWithPending(SmallVectorImpl<char> &out, StringRef original,
                                 uint64_t from, uint64_t to,
                                 std::optional<PendingResync> pending) const;

  // ------------------------ Low-level Mapping & Utils ------------------------

  /// \brief Resolves the TU/file byte start offset corresponding to a PP
  /// coordinate for a specific file.
  ///
  /// The refold model maintains a PP->(file, byte-range) mapping (e.g.
  /// \c tokmapByPP) that allows code working in PP space to locate the
  /// corresponding region in an owning file (TU or header).
  ///
  /// If \p pp maps to an entry whose \c file matches the requested \p file,
  /// this returns the mapped begin byte offset (\c b). Otherwise, this returns
  /// \p fileLen when \p fallbackToEOF is enabled, or `std::nullopt` when fallback is
  /// disabled.
  ///
  /// The \p fallbackToEOF behavior is intended for boundary cases where we want
  /// a deterministic anchoring point even when a PP position does not directly
  /// map into the requested file (for example, appending at end-of-file in a
  /// file-owned segment).
  ///
  /// \param file the file path whose mapping is being queried (TU or included
  ///        header)
  /// \param pp the PP token index in the A-side preprocessed token stream to resolve
  /// \param fallbackToEOF if true, return \p fileLen when \p pp does not map
  ///        into \p file
  /// \param fileLen the length of \p file in bytes (used only when
  ///        \p fallbackToEOF is true)
  /// \return the mapped start byte offset in \p file, or \p fileLen when
  ///         falling back to EOF, or `std::nullopt` if unmapped and fallback is disabled
  std::optional<uint64_t> ByteStartForPPInFile(StringRef file, uint64_t pp,
                                               bool fallbackToEOF,
                                               size_t fileLen) const {
    auto it = model_.GetTokmapByPP().find(pp);
    if (it != model_.GetTokmapByPP().end() && PathsEqual(it->second.file, file))
      return it->second.b;
    return fallbackToEOF ? std::optional<uint64_t>(fileLen) : std::nullopt;
  }

  /// \brief Resolves the TU/file byte end offset corresponding to a PP
  /// coordinate for a specific file.
  ///
  /// Analogous to ByteStartForPPInFile() but returns the mapped end byte offset
  /// (\c e). If \p pp maps to an entry whose \c file matches the requested \p
  /// file, this returns \c e. Otherwise, this returns \p fileLen when \p
  /// fallbackToEOF is enabled, or `std::nullopt` when fallback is disabled.
  ///
  /// \param file the file path whose mapping is being queried (TU or included
  ///        header)
  /// \param pp the PP token index in the A-side preprocessed token stream to resolve
  /// \param fallbackToEOF if true, return \p fileLen when \p pp does not map
  ///        into \p file
  /// \param fileLen the length of \p file in bytes (used only when
  ///        \p fallbackToEOF is true)
  /// \return the mapped end byte offset in \p file, or \p fileLen when falling
  ///         back to EOF, or `std::nullopt` if unmapped and fallback is disabled
  std::optional<uint64_t> ByteEndForPPInFile(StringRef file, uint64_t pp,
                                             bool fallbackToEOF,
                                             size_t fileLen) const {
    auto it = model_.GetTokmapByPP().find(pp);
    if (it != model_.GetTokmapByPP().end() && PathsEqual(it->second.file, file))
      return it->second.e;
    return fallbackToEOF ? std::optional<uint64_t>(fileLen) : std::nullopt;
  }

  /// \brief Compare two paths for equality after weak canonicalization.
  ///
  /// If either path is empty, returns string equality directly. Otherwise both
  /// paths are weakly canonicalized (`std::filesystem::weakly_canonical`) and
  /// compared. Canonicalization failures are fatal.
  ///
  /// \param a First path.
  /// \param b Second path.
  /// \returns `true` if canonical paths are equal; `false` otherwise.
  static bool PathsEqual(StringRef a, StringRef b);

  // ---------------------- Diagnostics & Debug Utilities ----------------------

  /// \brief Emits a detailed TRACE log line describing how a given hunk maps
  /// into an include's token/byte space.
  ///
  /// This method is purely diagnostic. It computes:
  /// - A-side byte interval for the hunk using aTokOff (token start offsets in
  ///   aSource)
  /// - B-side byte interval for the hunk using bTokOff (token start offsets in
  ///   bSource)
  /// - Clipped source slices for both sides (escaped and whitespace-visualized)
  ///   and logs a single structured line under "include/patch".
  ///
  /// The log payload is designed to make it straightforward to validate that
  /// include-owned edits are using the correct token indices, byte ranges, and
  /// local context, especially for boundary insertions and for hunks that are
  /// fully inside header-owned regions.
  ///
  /// \param tag a short tag describing the call site / phase (e.g., "before",
  ///        "after", "emit")
  /// \param inc the include item that is expected to own the hunk
  /// \param h the diff hunk being examined (A token range and B token range)
  void DebugIncludePatch(StringRef tag, const RefoldModel::IncludeItem &inc,
                         const diffutils::Hunk &h) const;

  /// \brief Serializes a single macro argument span into a diagnostic string
  /// format.
  ///
  /// The output string follows the pattern:
  /// `{kind=K, A=[begin,end), argIdx=I, [occ=STRINGIFY], [byte=[bBegin,bEnd)]}`
  ///
  /// \param sp The span metadata to serialize.
  /// \param isStringifyOcc Whether this occurrence was produced by a `#`
  ///                       operator.
  /// \returns A formatted string representation of the span.
  static std::string PPArgSpanToString(const RefoldModel::PPArgSpan &sp,
                                       bool isStringifyOcc);

  /// \brief Serializes a list of macro argument spans into a comma-separated
  /// bracketed list.
  ///
  /// \param spans The list of spans to serialize.
  /// \param isStringify A parallel array indicating which spans are
  ///                    stringification occurrences.
  /// \returns A formatted string representation such as `[{...}, {...}]`.
  static std::string
  PPArgSpanListToString(ArrayRef<RefoldModel::PPArgSpan> spans,
                        ArrayRef<char> isStringify);

};


} // namespace refold
} // namespace clang

namespace llvm {
using namespace clang::refold;

template <> struct format_provider<RefoldEngine::OwnerKind> {
  static void format(const RefoldEngine::OwnerKind &kind, raw_ostream &os,
                     StringRef style) {

    switch (kind) {
    case RefoldEngine::OwnerKind::TU:
      os << "TU";
      break;
    case RefoldEngine::OwnerKind::Include:
      os << "Include";
      break;
    case RefoldEngine::OwnerKind::Unknown:
      os << "Unknown";
      break;
    }
  }
};

template <> struct DenseMapInfo<std::optional<uint64_t>> {
  static inline std::optional<uint64_t> getEmptyKey() {
    return std::optional<uint64_t>(~0ULL);
  }
  static inline std::optional<uint64_t> getTombstoneKey() {
    return std::optional<uint64_t>(~1ULL);
  }
  static unsigned getHashValue(const std::optional<uint64_t> &val) {
    // If val has a value, hash it and cast to unsigned.
    // If not (std::nullopt), return a constant (like 0).
    return val ? static_cast<unsigned>(llvm::hash_value(*val)) : 0u;
  }
  static bool isEqual(const std::optional<uint64_t> &lhs,
                      const std::optional<uint64_t> &rhs) {
    return lhs == rhs;
  }
};
} // namespace llvm

#endif // LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDENGINE_H
