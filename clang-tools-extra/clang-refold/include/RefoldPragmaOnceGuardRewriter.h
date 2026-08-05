//===--- RefoldPragmaOnceGuardRewriter.h ------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Synthetic once-state for `#pragma once` headers inlined into the refolded TU.
//
// `#pragma once` is a property of a *physical file*: the preprocessor records
// that a file has been entered and suppresses every later inclusion of it.  That
// state is inert in the main file, so once a header's text is copied into the
// refolded translation unit the original directive no longer establishes
// anything.  This service re-expresses the same state as an ordinary macro
// guard, which is the one form that survives being inlined.
//
// The transformation is:
//
//   * each inlined copy of the header is wrapped in
//     `#ifndef __CLANG_REFOLD_ONCE_N` / `#endif`;
//   * every `#pragma once` inside that copy is replaced, in place, by
//     `#define __CLANG_REFOLD_ONCE_N`;
//   * every surviving `#include` of the same physical header is rewritten to
//     `#ifndef __CLANG_REFOLD_ONCE_N` / `#define __CLANG_REFOLD_ONCE_N` /
//     <original directive verbatim> / `#endif`.
//
// clang-refold never modifies a header file.  Every edit produced here is
// emitted into the refolded TU's byte space: either into the *copy* of the header
// text that is being spliced into the TU, or into TU / parent-header bytes.  The
// header on disk keeps its `#pragma once`.
//
// That invariant is why the surviving-include wrapper defines the macro
// *eagerly*, before entering the header, rather than relying on the header to do
// it.  Because the on-disk header is unmodified, a surviving `#include` still
// receives real `#pragma once` semantics from Clang's own once-set, which the
// synthetic macro cannot observe.  When a surviving include precedes an inlined
// copy, the real once-set is populated while the macro is not; without the eager
// define the later `#ifndef` would pass and the body would be emitted twice.
//
// The service is deliberately fail-closed.  Every physical header it cannot
// fully prove carries an explicit `PragmaOnceGuardRejection` rather than a
// silently weaker guard, because emitting an unguarded inlined body beside a
// surviving include duplicates source.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDPRAGMAONCEGUARDREWRITER_H
#define LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDPRAGMAONCEGUARDREWRITER_H

#include "core/RefoldModel.h"
#include "edit/RefoldEditTypes.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/StringRef.h"

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace clang {

class LangOptions;

namespace refold {

class LineDirectiveInserter;
class RefoldLineControlProof;
class RefoldMacroStateProof;
class RefoldPathIdentity;
class RefoldPreprocessingStructureIndex;
class RefoldProofLattice;
class RefoldTerminalProofSink;
class RefoldTextEditAssembler;

/// Return whether \p text is exactly one `#pragma once` directive spelling.
///
/// The grammar accepts optional horizontal whitespace around the introducer and
/// keywords, and treats a complete trailing comment as directive trivia because
/// Clang accepts `#pragma once /* ... */` with identical effect.  An incomplete
/// comment, any additional operand, or any other pragma name is rejected, so a
/// pragma that merely contains the word `once` is never classified here.
///
/// This is the single recognizer for the spelling; the zero-token include
/// closure proof in RefoldExpansionFallbackPlanner uses it as well so the two
/// paths cannot drift apart.
bool refoldTextIsPragmaOnceDirective(llvm::StringRef text);

/// Exact reason a physical header cannot receive synthetic once-state.
///
/// A boolean would lose the evidence needed to explain a fail-closed refold, so
/// the rejection vocabulary is explicit.  Each enumerator names one proof
/// obligation that could not be discharged.
enum class PragmaOnceGuardRejection : uint8_t {
  /// The header is fully proven and may be guarded.
  None,
  /// The header establishes once-state through `_Pragma("once")`, which the
  /// producer records nowhere and which this service does not model.
  PragmaOperatorOnce,
  /// An `#import` edge reaches this header.  `#import` carries once-semantics
  /// with no pragma at all, so the catalog cannot account for it.
  ImportEdge,
  /// An include edge into this header lacks `openedPath`, so physical identity
  /// would fall back to a path *spelling* and two spellings of one file could
  /// mint two guards.
  MissingOpenedPath,
  /// The exact preprocessing-structure census for the header is incomplete, so
  /// the pragma inventory cannot be trusted to be complete.
  IncompleteStructureCensus,
  /// A producer `#pragma` record for this header did not bind to any discovered
  /// lexical interval, or a discovered `once` interval could not be bound.
  UnboundPragmaRecord,
  /// The establishing site does not dominate every guarded site for this header.
  NonDominatingEstablishingSite,
  /// An occurrence of this header was preserved as a source-graph sidecar, which
  /// keeps native `#pragma once` semantics and must not be mixed with an inlined
  /// copy.
  SidecarPreservedOccurrence,
  /// The header includes itself transitively, so a top-of-body `#define` is not
  /// equivalent to a bottom-of-file pragma site.
  SelfIncludingHeader,
  /// The synthetic guard namespace already occurs in the inputs, so a fresh
  /// name cannot be proven collision-free.
  GuardNamespaceCollision,
  /// A lexically discovered include directive bound to no producer record, so
  /// the occurrence count for this header cannot be trusted.
  UnaccountedIncludeDirective,
  /// The guard directives would shift physical lines past a preserved line-state
  /// observer while `#line` repair is unavailable.
  UnrepairableLineDrift,
  /// The include site could not be authorized as a rewritable include directive.
  UnauthorizableIncludeSite,
};

/// How one physical header's once-state is re-expressed in the refolded TU.
enum class PragmaOnceTreatment : uint8_t {
  /// The header has exactly one provable occurrence, so no guard is needed: no
  /// surviving include can re-enter it and no second copy can duplicate it.  The
  /// `#pragma once` is deleted instead, because it is inert in the main file and
  /// otherwise emits `-Wpragma-once-outside-header` for every refolded TU.
  DeletePragma,
  /// The header has more than one occurrence, or its occurrence count could not
  /// be proven, so synthetic guard state is required.
  EmitGuard,
};

/// Return a stable diagnostic spelling for a treatment.
llvm::StringRef toString(PragmaOnceTreatment treatment);

/// Return a stable diagnostic spelling for a guard rejection.
llvm::StringRef toString(PragmaOnceGuardRejection rejection);

/// One exact `#pragma once` occurrence inside a physical header.
///
/// Offsets are physical byte offsets in that header's bytes and use the
/// engine-wide half-open `[begin,end)` convention.  `spelling*` is the
/// scanner-proven directive spelling, which is the range a specialized directive
/// operation is authorized to replace; `[begin,end)` additionally covers leading
/// logical-line trivia and the terminating newline.
struct PragmaOnceSite {
  uint64_t begin = 0;
  uint64_t end = 0;
  uint64_t spellingBegin = 0;
  uint64_t spellingEnd = 0;
  /// Producer `PragmaDirective` id when the lexical interval bound to a record.
  std::optional<uint64_t> modelItemId;
  /// Innermost conditional arm in this file containing the site, when any.
  ///
  /// A site with no enclosing arm is executed whenever the header is entered,
  /// which is what makes a top-of-body `#define` equivalent for a body realized
  /// from the edited preprocessed stream.
  std::optional<uint64_t> enclosingArmId;
};

/// Immutable synthetic-once record for one physical header.
struct PragmaOnceGuard {
  /// Canonical physical path identifying the header.
  std::string physicalHeaderPath;
  /// Synthetic guard macro name, `__CLANG_REFOLD_ONCE_N`.
  std::string macroName;
  /// Every `#pragma once` discovered in the header, in source order.
  llvm::SmallVector<PragmaOnceSite, 2> sites;
  /// True when at least one site has no enclosing conditional arm.
  ///
  /// Only an unconditional site licenses the eager surviving-include wrapper and
  /// the top-of-body `#define` used for a body realized from B.  A conditional
  /// pragma is still exact for an inlined *source* copy, because each `#define`
  /// stays at its original pragma site.
  bool unconditionalPragmaProven = false;
  /// Number of include edges the producer recorded opening this header.
  size_t recordedOccurrences = 0;
  /// How this header's once-state is re-expressed.
  PragmaOnceTreatment treatment = PragmaOnceTreatment::EmitGuard;
  /// Exact reason this header cannot be guarded, or `None`.
  PragmaOnceGuardRejection rejection = PragmaOnceGuardRejection::None;
  /// Deterministic diagnostic detail for `rejection`.
  std::string rejectionDetail;

  /// Return whether this record may be used to emit edits.
  ///
  /// A `DeletePragma` record needs no macro name, so the name is required only
  /// for the guard-emitting treatment.
  bool IsUsable() const {
    if (rejection != PragmaOnceGuardRejection::None || sites.empty())
      return false;
    return treatment == PragmaOnceTreatment::DeletePragma || !macroName.empty();
  }
};

/// Outcome of one guard-edit staging request.
///
/// Staging never partially applies: on rejection the caller must not emit any of
/// the requested edits and must fall back to its own fail-closed path.
struct PragmaOnceGuardEditResult {
  bool proven = false;
  PragmaOnceGuardRejection rejection = PragmaOnceGuardRejection::None;
  std::string detail;

  static PragmaOnceGuardEditResult Proven() { return {true, {}, {}}; }
  static PragmaOnceGuardEditResult Reject(PragmaOnceGuardRejection reason,
                                          std::string detail) {
    return {false, reason, std::move(detail)};
  }
  /// Return a proven result for a header that needs no guard at all.
  static PragmaOnceGuardEditResult NotApplicable() { return {true, {}, {}}; }
};

/// Builds and applies synthetic once-state for `#pragma once` headers.
///
/// The service has a deliberate two-phase contract:
///
///  1. Construction builds the immutable *candidate* catalog: every physical
///     header reachable through an include edge that establishes once-state,
///     together with its guard name and per-header proof state.
///  2. `SetActiveGuardedHeaders()` records which of those headers are actually
///     inlined into the TU in this run, after which the staging methods may be
///     called.
///
/// The two phases are separate because materialization decisions are made during
/// include recursion and cannot be known when the catalog is built.  The active
/// set is therefore an over-approximation, which is sound for an unconditional
/// pragma: guarding occurrences of a header that nothing inlined is inert, since
/// `#ifndef` passes, the define fires, the include proceeds, and no other reader
/// of the macro exists.  Under-approximating would be unsound, so callers must
/// include every header they *might* inline.
class RefoldPragmaOnceGuardRewriter {
public:
  /// Borrowed services required to build the catalog and authorize edits.
  struct Dependencies {
    const RefoldModel &model;
    const RefoldPathIdentity &pathIdentity;
    const RefoldMacroStateProof &macroStateProof;
    const LineDirectiveInserter &lineDirs;
    const RefoldLineControlProof &lineControlProof;
    const RefoldTextEditAssembler &textEditAssembler;
    const RefoldProofLattice &proofLattice;
    RefoldTerminalProofSink &terminalSink;
    const clang::LangOptions &lexLang;
  };

  /// Immutable run inputs scanned while proving guard-name collision-freedom.
  struct GuardNameInputs {
    /// Translation-unit path, excluded from the catalog: a main-file
    /// `#pragma once` establishes nothing and Clang warns on it.
    llvm::StringRef tuPath;
    /// Translation-unit source bytes.
    llvm::StringRef tuBytes;
    /// Original preprocessed stream A.
    llvm::StringRef aSource;
    /// Edited preprocessed stream B.
    llvm::StringRef bSource;
  };

  RefoldPragmaOnceGuardRewriter(Dependencies deps, GuardNameInputs inputs);

  /// Record which physical headers are inlined into the TU in this run.
  ///
  /// Must be called exactly once, before any staging method.  Paths are
  /// canonicalized through the path-identity service, so any spelling of a
  /// guarded header resolves to the same record.
  void SetActiveGuardedHeaders(llvm::ArrayRef<llvm::StringRef> physicalPaths);

  /// Record that an occurrence of \p physicalPath was preserved as a
  /// source-graph sidecar.
  ///

  /// Return the guard for the physical header opened by \p include, if any.
  ///
  /// Returns null when the header establishes no once-state, when it is not in
  /// the active set, or when its proof failed closed.  A null result is not by
  /// itself an error; callers distinguish the fail-closed case with
  /// `FindRejectionForInclude()`.
  const PragmaOnceGuard *
  FindGuardForInclude(const RefoldModel::IncludeItem &include) const;

  /// Return the guard for one canonical physical header path, if any.
  const PragmaOnceGuard *FindGuardForPath(llvm::StringRef physicalPath) const;

  /// Return whether \p physicalPath is an active guarded header.
  bool HeaderRequiresGuard(llvm::StringRef physicalPath) const;

  /// Return the rejection recorded for the header opened by \p include.
  ///
  /// `None` means either that the header needs no guard or that its guard is
  /// usable; callers that must fail closed should consult this after
  /// `FindGuardForInclude()` returns null.
  PragmaOnceGuardRejection
  FindRejectionForInclude(const RefoldModel::IncludeItem &include) const;

  /// Return whether one physical header establishes provable once-state.
  ///
  /// This is a catalog-only query: it does not consider whether the header is
  /// inlined in this run, so it is usable before the active set is recorded.
  bool HeaderEstablishesOnceState(llvm::StringRef physicalPath) const;

  /// Return whether any macro defined inside one physical header's entered
  /// include subtree is invoked from outside that subtree.
  ///
  /// Restoring a header's include guard suppresses every later inclusion of it
  /// *and of everything it includes*.  A body realized from the edited stream
  /// carries the header's expanded declarations and none of its `#define`s, so
  /// that suppression also removes the only remaining source of the subtree's
  /// macro state.  Guard restoration is therefore admissible only when nothing
  /// outside the subtree observes that state.
  ///
  /// The subtree test walks recorded include parents upward from each owner, so
  /// it needs no child index and stays exact for repeated instances of a header.
  bool HeaderMacroStateIsObservedOutside(llvm::StringRef physicalPath) const;

  /// Return whether a header protects itself against re-entry at all, by
  /// `#pragma once` or by a classic `#ifndef` guard the producer named.
  ///
  /// Both kinds create the same hazard once a copy of the header is inlined
  /// into the translation unit: a later `#include` of it must not expand the
  /// content a second time.  They differ only in how the original expressed the
  /// protection, which is not a reason to repair them differently.
  bool HeaderEstablishesReentryProtection(llvm::StringRef physicalPath) const;

  /// Return whether the header opened by \p include transitively includes any of
  /// \p targetPaths, other than by being that header itself.
  ///
  /// This is the re-entry predicate that a guard in TU byte-space cannot
  /// discharge.  Wrapping a surviving `#include "outer.h"` protects `outer.h`,
  /// but if `outer.h` on disk includes an already-inlined once-header, that
  /// nested directive re-enters the real file whose real `#pragma once` never
  /// fired -- because the inlined copy was spliced as text, not included.  The
  /// header is never modified, so the only sound responses are to materialize
  /// this include as well, or to fail closed.
  ///
  /// The closure is computed over producer include records, which include
  /// *skipped* edges: those carry `sitePath` and `openedPath` even though they
  /// were suppressed, and they are exactly the edges that re-enter.
  bool IncludeClosureReentersHeader(const RefoldModel::IncludeItem &include,
                                    llvm::ArrayRef<std::string> targetPaths,
                                    std::string *reenteredPath = nullptr) const;

  /// Return the canonical physical paths of every active, usable guard.
  std::vector<std::string> ActiveGuardedHeaderPaths() const;

  /// Append `#define` directives restoring the include-guard state that a body
  /// realized from the edited preprocessed stream discarded.
  ///
  /// A B realization is tokens, not source, so every directive the header
  /// contained is gone -- including the `#define` of the macro its own
  /// `#ifndef` guard sets.  The content is then present in the TU while the
  /// controlling macro stays undefined, so any later path back to that physical
  /// file re-enters it and emits the content a second time.
  ///
  /// Defining each affected header's real controlling macro reproduces exactly
  /// the skip the original preprocessing performed.  The names come from the
  /// producer (`controlling_macro`), which reads them from HeaderSearch; they
  /// are never guessed from the header text.
  ///
  /// \p enteredSubtreeIncludeIds must be the include instances whose content the
  /// realized body absorbed.  Headers with no recorded controlling macro are
  /// reported through \p unrestoredPath so the caller can fail closed: a
  /// `#pragma once` header has no guard macro to restore and is instead covered
  /// by the synthetic guard, while a header with no protection at all needs
  /// none.
  bool AppendRealizedFromBIncludeGuardRestoration(
      llvm::ArrayRef<uint64_t> enteredSubtreeIncludeIds,
      std::string &realizedBody, std::string *unrestoredPath = nullptr) const;

  /// Return every usable guard, ordered by canonical physical path.
  ///
  /// Deterministic ordering makes the emitted guard numbering reproducible and
  /// keeps diagnostics stable.
  std::vector<const PragmaOnceGuard *> UsableGuards() const;

  /// Stage guard edits for one inlined copy of a header's *source* bytes.
  ///
  /// Appends, to \p edits, the `#ifndef` prologue, one `#define` replacement per
  /// `#pragma once` interval proven in \p headerBytes, and the `#endif` epilogue.
  /// Pragma sites are re-proven against \p headerBytes rather than trusted from
  /// the catalog, because a caller may preseed a materialized body with bytes
  /// that differ from the file on disk.
  ///
  /// This form is exact for a conditional pragma as well: each `#define` remains
  /// at its original site, so a non-firing arm leaves the macro undefined and a
  /// later copy correctly re-emits the body.
  ///
  /// When the header's treatment is `DeletePragma`, only the pragma replacements
  /// are staged and the `#ifndef`/`#endif` pair is omitted.  Replacing just the
  /// directive *spelling* leaves the terminating newline in place, so that form
  /// is line-neutral.
  ///
  /// \p ancestorArmId is the innermost conditional arm enclosing this include's
  /// own site, accumulated across the include ancestry, or nullopt when the
  /// include is unconditional all the way up.  It is required for the dominance
  /// proof: an arm inside the header is not the whole story when the header
  /// itself was included from inside a conditional.
  PragmaOnceGuardEditResult StageMaterializedBodyGuardEdits(
      const RefoldModel::IncludeItem &include, llvm::StringRef headerPath,
      llvm::StringRef headerBytes, std::optional<uint64_t> ancestorArmId,
      std::vector<TextEdit> &edits) const;

  /// Stage guard edits for one inlined body realized from the edited
  /// preprocessed stream B.
  ///
  /// A B realization contains tokens, not header source, so it carries no pragma
  /// site to rewrite and the `#define` must go at the top of the body.  That is
  /// exact only when the pragma is unconditional and the header does not include
  /// itself transitively, because a self-include is the only construct that can
  /// observe once-state between the header start and a bottom-of-file pragma.
  /// Both are proven here; either failure rejects.
  PragmaOnceGuardEditResult
  StageRealizedFromBGuardText(const RefoldModel::IncludeItem &include,
                              std::optional<uint64_t> ancestorArmId,
                              std::string &realizedBody) const;

  /// Stage the wrapper for one surviving `#include` directive of a guarded
  /// header.
  ///
  /// \p ownerBytes is the byte space containing the directive: TU bytes for a
  /// TU-owned site, or the materialized parent copy for a nested site.  The
  /// original directive spelling inside `[siteBegin,siteEnd)` is preserved
  /// verbatim, including comments, delimiters, macro-computed operands, and
  /// `#include_next`.
  ///
  /// Requires an unconditional pragma: the wrapper defines the macro eagerly,
  /// which would mark a conditionally-once header as included even when the
  /// original pragma would not have fired.
  PragmaOnceGuardEditResult StageSurvivingIncludeGuardEdit(
      const RefoldModel::IncludeItem &include, llvm::StringRef ownerPath,
      std::optional<uint64_t> ownerIncludeId, llvm::StringRef ownerBytes,
      uint64_t siteBegin, uint64_t siteEnd,
      std::optional<uint64_t> ancestorArmId,
      std::vector<TextEdit> &edits) const;

private:
  /// Mutable build state for one physical header before the catalog is frozen.
  struct HeaderCandidate {
    PragmaOnceGuard guard;
    /// Include edge ids that opened this physical header, in producer order.
    llvm::SmallVector<uint64_t, 4> includeIds;
    /// True when this header is inlined into the TU in this run.
    bool active = false;
  };

  /// Discover every physical header reached by an include edge that establishes
  /// once-state, recording per-header rejections as they are found.
  void BuildCandidateCatalog();

  /// Prove that the synthetic guard namespace does not already occur in any
  /// input, rejecting every candidate when it does.
  ///
  /// Runs at construction because it does not depend on which headers are
  /// eventually inlined.
  void ProveGuardNamespaceIsFree();

  /// Assign deterministic guard names to the active, guard-emitting candidates.
  ///
  /// Numbering runs after activation and covers only headers that actually emit a
  /// guard, so names are dense from one over exactly the guards that appear in
  /// the output.  A candidate that is never inlined, or whose treatment is
  /// `DeletePragma`, consumes no number and therefore cannot shift the numbering
  /// of an unrelated header.  Iteration is over a `std::map` of canonical paths,
  /// so the assignment does not depend on include discovery order.
  void AssignGuardNames();

  /// Return the canonical physical path for one include edge, or nullopt when
  /// the edge lacks `openedPath`.
  std::optional<std::string>
  CanonicalPhysicalPathForInclude(const RefoldModel::IncludeItem &include) const;

  /// Load one header's on-disk bytes, caching by canonical path.
  ///
  /// Returns nullopt when the file cannot be read; the caller then records an
  /// incomplete-census rejection rather than guessing an inventory.
  std::optional<llvm::StringRef>
  LoadHeaderBytes(llvm::StringRef physicalPath, llvm::StringRef loadPath) const;

  /// Discover the `#pragma once` inventory for one owner occurrence of a header.
  ///
  /// Returns false and sets \p rejection when the structure census is
  /// incomplete, when a `_Pragma` operator establishes once-state, or when a
  /// producer record cannot be reconciled with the lexical inventory.
  bool DiscoverPragmaOnceSites(llvm::StringRef physicalPath,
                               llvm::StringRef sourcePath,
                               std::optional<uint64_t> ownerIncludeId,
                               llvm::StringRef bytes,
                               llvm::SmallVectorImpl<PragmaOnceSite> &sites,
                               PragmaOnceGuardRejection &rejection,
                               std::string &detail) const;

  /// Return the innermost conditional arm containing \p byteOffset for one
  /// owner occurrence, or nullopt when the offset is unconditional there.
  std::optional<uint64_t>
  InnermostEnclosingArm(llvm::StringRef sourcePath,
                        std::optional<uint64_t> ownerIncludeId,
                        uint64_t byteOffset) const;

  /// Return whether \p physicalPath includes itself transitively.
  bool HeaderIncludesItself(llvm::StringRef physicalPath) const;

  /// Prove that every lexically discovered include directive in every physical
  /// source of this run binds to a producer include record.
  ///
  /// The producer only records include edges it actually reached, so an
  /// `#include` sitting in an unreached conditional arm leaves no record at all.
  /// Because the refolded TU preserves conditionals and is expected to remain
  /// configuration-polymorphic, such an include could become reached under a
  /// different `-D` and re-enter a header whose body was inlined unguarded.
  ///
  /// The result is a whole-run property rather than a per-header one: an
  /// unaccounted include in any source could name any header.  Computed once and
  /// memoized; \p detail names the first offending directive.
  bool ProveNoUnaccountedIncludeDirectives(std::string &detail) const;

  /// Return whether the establishing site is executed whenever the header is
  /// entered, considering both the site's own arm and the include ancestry.
  ///
  /// This is the dominance proof required before the macro may be established
  /// eagerly or at the top of a body realized from B.  It intentionally requires
  /// a fully unconditional site rather than implementing the weaker
  /// "establishing site and all guarded sites share one arm" refinement: the
  /// latter needs every guarded site's arm chain, which is not available here.
  bool EstablishingSiteIsUnconditional(
      llvm::ArrayRef<PragmaOnceSite> sites,
      std::optional<uint64_t> ancestorArmId) const;

  /// Return whether guard line drift past \p offset is repairable in the current
  /// line-control mode.
  ///
  /// The prologue and epilogue add whole physical lines, and the surviving-include
  /// wrapper adds three.  With `#line` injection enabled the drift is repaired by
  /// the ordinary resync machinery; with `--no-lines` it is unrepairable, so a
  /// preserved line-state observer in the shifted suffix must fail closed.
  bool GuardLineDriftIsRepairable(std::optional<uint64_t> ownerIncludeId,
                                  llvm::StringRef ownerPath,
                                  uint64_t offset) const;

  /// Return the mutable candidate for one canonical path, or null.
  HeaderCandidate *FindCandidate(llvm::StringRef canonicalPath);
  const HeaderCandidate *FindCandidate(llvm::StringRef canonicalPath) const;

  /// Record a rejection on one candidate without overwriting an earlier one.
  ///
  /// The first rejection is retained so diagnostics name the earliest failed
  /// obligation rather than the last one observed.
  void RejectCandidate(HeaderCandidate &candidate,
                       PragmaOnceGuardRejection reason, std::string detail);

  /// Return whether \p ownerIncludeId names an include instance that lies at or
  /// beneath any instance of the header at \p physicalPath.
  bool IncludeLiesInsideHeader(uint64_t ownerIncludeId,
                               llvm::StringRef physicalPath) const;

  Dependencies deps_;
  GuardNameInputs inputs_;

  /// Candidates keyed by canonical physical path.  `std::map` keeps iteration
  /// deterministic for guard numbering and diagnostics.
  std::map<std::string, HeaderCandidate> candidates_;
  /// True once `SetActiveGuardedHeaders()` has run.
  bool activeSetRecorded_ = false;
  /// On-disk header byte cache keyed by canonical physical path.
  mutable llvm::StringMap<std::optional<std::string>> headerBytesCache_;
  /// Memoized transitive self-inclusion answers keyed by canonical path.
  mutable llvm::StringMap<bool> selfIncludeCache_;
  /// Memoized whole-run unaccounted-include proof, with its diagnostic detail.
  mutable std::optional<std::pair<bool, std::string>> unaccountedIncludeProof_;
};

} // namespace refold
} // namespace clang

#endif // LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDPRAGMAONCEGUARDREWRITER_H
