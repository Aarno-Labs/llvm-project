//===--- RefoldIncludeReplayProof.h ---------------------------*- C++ -*-===//
//
// Include/include_next replay proof layer for clang-refold.
//
// The proof context is read-only: it consumes immutable model/source inputs
// plus explicit service callbacks for owner-local queries, then returns whether
// an include edge may be preserved, rewritten, or must be materialized.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDINCLUDEREPLAYPROOF_H
#define LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDINCLUDEREPLAYPROOF_H

#include "core/RefoldLog.h"
#include "core/RefoldModel.h"
#include "line-control/LineDirectiveInserter.h"
#include "proof/RefoldProofVocabulary.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/STLFunctionalExtras.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/ErrorHandling.h"

#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>

namespace clang {
namespace refold {

using llvm::ArrayRef;
using llvm::DenseMap;
using llvm::DenseSet;
using llvm::SmallVector;
using llvm::SmallVectorImpl;
using llvm::StringRef;

/// Build the final-source include replay surface used by include replay proof.
///
/// The driver checks the emitted source by preprocessing the final `--out`
/// path, not the producer TU path recorded in the map.  This helper models
/// that final quoted-include lookup surface without mutating RefoldEngine.
std::optional<FinalReplaySurface>
buildFinalReplaySurface(const RefoldModel &model, StringRef finalOutputPath);

/// Immutable data needed by include replay proof.  These references are
/// borrowed from the current refold run and must outlive the short-lived proof
/// context constructed for one materialization operation.
struct IncludeReplayProofInputs {
  const RefoldModel &model;
  StringRef aSource;
  const LineDirectiveInserter &lineDirs;
  const std::optional<FinalReplaySurface> &finalReplaySurface;
};

/// Read-only services used by include replay proof.  The function_ref callables
/// are deliberately non-owning so this layer cannot retain mutable state beyond
/// the caller's materialization scope.
struct IncludeReplayProofServices {
  llvm::function_ref<StringRef(uint64_t Begin, uint64_t End)> SliceASource;

  llvm::function_ref<bool(StringRef CandidatePath,
                          const RefoldModel::IncludeItem &Include)>
      samePhysicalIncludeFile;

  llvm::function_ref<const RefoldModel::MacroInvocation *(
      const RefoldModel::MacroInvocation &Macro)>
      lineStateObservableMacroSite;

  llvm::function_ref<bool(const RefoldModel::MacroInvocation &Macro)>
      lineStateBuiltinInvocationIsPreservedObserver;

  llvm::function_ref<LineStateObserverDemand(uint64_t IncludeId)>
      includeSubtreeLineStateObserverDemand;
};

/// Proves whether source-spelled include edges can be replayed after
/// materialization.
///
/// The context is deliberately read-only and scoped to one include
/// materialization operation.  It distinguishes three facts that must not be
/// conflated: the physical file identity selected by lookup, the
/// producer-entered filename spelling observed by `__FILE__`, and the
/// HeaderSearch cursor needed by descendant `#include_next` replay.  Physical
/// identity alone is not sufficient for include_next proof.
class IncludeReplayProofContext {
public:
  /// Header-name delimiter selected for an ordinary include replay operand.
  enum class OrdinaryIncludeDelimiterKind { Quoted, Angled };

  /// Action chosen for a clean child include after its parent has been
  /// materialized onto a new replay surface.
  enum class CleanChildIncludeReplayAction {
    /// No edit is required; the original child directive remains sound.
    None,
    /// Rewrite the child directive operand/delimiter without materializing the
    /// child include body.
    RewriteOperand,
    /// Materialize the child include body because replay cannot be proven.
    Materialize
  };

  /// Deterministic replay/materialization decision for one clean child include.
  struct CleanChildIncludeReplayPlan {
    CleanChildIncludeReplayAction action = CleanChildIncludeReplayAction::None;
    std::string rewrittenOperand;

    // RewriteOperand plans can preserve an include directive with either
    // header-name delimiter.  Direct relocated #include_next repair may prove
    // an angled ordinary include; quoted child-relocation repairs remain quoted
    // by construction.
    OrdinaryIncludeDelimiterKind rewrittenDelimiterKind =
        OrdinaryIncludeDelimiterKind::Quoted;
    std::string reason;

    // Set when the materialization decision was forced by descendant
    // #include_next state.  The recursive materialization of this child must
    // realize nested include_next directives as concrete text rather than
    // rewriting them as ordinary includes; otherwise the final output would
    // still depend on replaying a search-stack-sensitive directive from a
    // relocated context.
    bool forceMaterializeDescendantIncludeNext = false;
  };

  IncludeReplayProofContext(IncludeReplayProofInputs inputs,
                            IncludeReplayProofServices services)
      : services_(services), model_(inputs.model), aSource_(inputs.aSource),
        lineDirs_(inputs.lineDirs),
        finalReplaySurface_(inputs.finalReplaySurface) {}

  /// Decide whether a clean child include can remain source-spelled after its
  /// parent include was materialized.
  ///
  /// The proof first tries to replay the child directive from the final
  /// materialized-parent surface using the same lookup semantics Clang would
  /// apply.  Ordinary quoted includes may be rewritten to a safe relative
  /// operand when that preserves the producer-selected target; direct
  /// descendant `#include_next` requires modeled search-chain provenance and
  /// otherwise fails closed to materialization.
  CleanChildIncludeReplayPlan PlanCleanChildIncludeReplayFromMaterializedParent(
      const RefoldModel::IncludeItem &child) const;

  /// Return the stable diagnostic name for an ordinary include delimiter.
  static const char *
  OrdinaryIncludeDelimiterName(OrdinaryIncludeDelimiterKind kind) {
    switch (kind) {
    case OrdinaryIncludeDelimiterKind::Quoted:
      return "quote";
    case OrdinaryIncludeDelimiterKind::Angled:
      return "angle";
    }
    llvm_unreachable("Invalid OrdinaryIncludeDelimiterKind");
  }

  /// Return the opening token for a rewritten ordinary include operand.
  static char OrdinaryIncludeDelimiterOpen(OrdinaryIncludeDelimiterKind kind) {
    switch (kind) {
    case OrdinaryIncludeDelimiterKind::Quoted:
      return '"';
    case OrdinaryIncludeDelimiterKind::Angled:
      return '<';
    }
    llvm_unreachable("Invalid OrdinaryIncludeDelimiterKind");
  }

  /// Return the closing token for a rewritten ordinary include operand.
  static char OrdinaryIncludeDelimiterClose(OrdinaryIncludeDelimiterKind kind) {
    switch (kind) {
    case OrdinaryIncludeDelimiterKind::Quoted:
      return '"';
    case OrdinaryIncludeDelimiterKind::Angled:
      return '>';
    }
    llvm_unreachable("Invalid OrdinaryIncludeDelimiterKind");
  }

private:
  /// One candidate result for replaying an ordinary include directive.
  ///
  /// The candidate carries physical identity and the entered filename spellings
  /// separately from lookup provenance. This separation preserves the proof
  /// invariant: a path that names the same file is not enough to prove a
  /// replay when later include_next behavior depends on the producer
  /// search-chain cursor.
  struct IncludeReplayCandidate {
    std::filesystem::path physicalPath;
    std::string enteredFileSpelling;
    std::string enteredFileName;

    // Ordinary include replay has two independent facts to preserve:
    //
    //   * the syntax context that started lookup (quoted vs angled vs direct
    //     source-relative / absolute operand), and
    //   * for search-chain hits, the producer-normalized HeaderSearch entry
    //     that selected the file.
    //
    // Keep the replay candidate tied to the actual search entry kind and index
    // whenever lookup came from pp_ctx.include_search_chain.  Physical-file
    // equality alone is not enough for #include_next proof, which also needs
    // the producer-selected search-chain cursor.
    enum class LookupKind {
      DirectSourceRelative,
      QuoteDir,
      UserI,
      System,
      IdirAfter,
      Framework,
      Builtin,
      AbsoluteOperand,
      Unknown
    };

    LookupKind kind = LookupKind::Unknown;

    // Present only when Kind names a producer/legacy include-search entry.
    // New-schema candidates use the exact pp_ctx.include_search_chain index;
    // legacy argv-reconstructed candidates intentionally leave this empty so
    // future include-next proof cannot mistake argv inference for producer
    // cursor provenance.
    std::optional<uint32_t> searchChainIndex;
  };

  /// Candidate result for replaying a descendant `#include_next`.
  ///
  /// Include-next replay is a proof about where HeaderSearch resumes.  Both the
  /// containing-file cursor and the selected target cursor must come from the
  /// producer search-chain metadata; argv reconstruction and physical identity
  /// matches are intentionally insufficient.
  struct IncludeNextReplayCandidate {
    std::filesystem::path physicalPath;
    std::string enteredFileSpelling;
    std::string enteredFileName;

    // The resume cursor is the first producer search-chain index examined by
    // #include_next replay.  Unlike ordinary includes, include_next never
    // starts from source-relative lookup or from the beginning of the search
    // chain; it resumes immediately after the search entry that selected the
    // containing file.
    uint32_t resumeSearchChainIndex = 0;

    // The producer search-chain entry that replay selected.  This must be an
    // actual pp_ctx.include_search_chain index, not a legacy argv-derived
    // approximation, because descendant #include_next proof is a proof about
    // HeaderSearch cursor state.
    uint32_t selectedSearchChainIndex = 0;
    IncludeLookupKind selectedKind = IncludeLookupKind::Unknown;
  };

  /// Proof obligation for one preserved descendant `#include_next`.
  ///
  /// Each obligation records the directive operand plus the producer cursor
  /// facts needed to replay lookup from the containing file.  Missing cursor
  /// provenance remains explicit so later proof checks fail closed instead of
  /// accepting physical-file equality as an include-next proof.
  struct IncludeNextObligation {
    uint64_t includeNextId = 0;

    // Present only for new-schema maps whose producer could identify the
    // include edge that contains this #include_next directive.  Old maps and
    // producer-unknown cases remain explicit obligations, but later proof code
    // must fail them closed because there is no containing-file cursor to
    // replay.
    std::optional<uint64_t> containingIncludeId;

    // Header operand with the surrounding quotes/angles stripped.  An empty
    // value means the directive target was not replay-parseable by the same
    // conservative operand rules used by ordinary include replay; the
    // include-next proof must treat that as unproven rather than guessing.
    std::string operand;

    // Producer resume/selection facts for this directive.  When metadata is
    // absent, this remains the default unknown provenance object so the
    // include-next proof can distinguish a collected obligation from a proven
    // replay obligation.
    RefoldModel::IncludeNextProvenance producer;
  };

  /// Clean-child replay demand accumulated from preserved observers.
  ///
  /// These flags and payloads describe every observable property that ordinary
  /// include replay must preserve before a materialized child subtree can be
  /// kept as a clean include.  The include-next obligations are intentionally
  /// part of the demand because they require search-chain cursor proof, not
  /// merely target physical identity.
  struct CleanChildIncludeReplayDemand {
    bool observesLine = false;
    bool observesFile = false;
    bool observesFileName = false;
    bool observesBaseFile = false;
    bool observesIncludeLevel = false;

    // Descendant #include_next directives observe HeaderSearch cursor state,
    // not just child physical identity.  Clean child replay may preserve the
    // child include only after every obligation here is discharged against the
    // replayed containing-file cursor and selected target.  Missing producer
    // provenance, an unparseable operand, an unknown search-chain entry, or any
    // physical/spelling mismatch keeps materialization fail-closed.
    SmallVector<IncludeNextObligation, 4> includeNextObligations;

    // Exact producer-observed payloads of preserved file-spelling observers in
    // this include subtree.  These come from the macro expansion tokens in the
    // original preprocessed stream, not from IncludeItem::resolvedPath: current
    // maps may store an absolute physical-ish path in resolvedPath even when
    // Clang exposed a direct source-relative spelling such as
    // "./headers/child.h" through __FILE__.
    SmallVector<std::string, 4> fileSpellingPayloads;
    SmallVector<std::string, 4> fileNamePayloads;

    // Producer-side entered spelling for the child edge itself.  New maps fill
    // this from IncludeItem::enteredFileSpelling.  Legacy maps fill it later
    // from recovered observer payloads, a producer-replay reconstruction, or
    // resolved_path, in that order.  The child edge must be re-entered with the
    // same spelling before descendant quoted lookup and file observers can be
    // considered stable.
    std::optional<std::string> producerChildFileSpelling;

    // Producer-side __FILE_NAME__ spelling for the child edge.  Prefer the
    // producer-emitted entered_file_name when present because it was computed
    // with Clang's own processPathToFileName() logic; otherwise derive a
    // compatibility basename from the selected spelling witness.
    std::optional<std::string> producerChildFileName;

    // A preserved file-spelling observer was present, but its exact expansion
    // payload could not be recovered from the producer token stream.  Include
    // replay cannot prove that observer family in that case, so the clean child
    // must fail closed to materialization/line-state repair.
    bool hasUnprovenFileSpellingObserver = false;

    bool observesFileSpelling() const {
      return observesFile || observesFileName ||
             hasUnprovenFileSpellingObserver;
    }

    bool requiresReplayCandidateProof() const {
      return observesLine || observesFileSpelling() || observesBaseFile ||
             observesIncludeLevel || !includeNextObligations.empty();
    }
  };

  /// One modeled directory entry in the producer's include search chain.
  struct IncludeReplaySearchDir {
    std::filesystem::path lookupPath;
    std::string enteredSpellingPrefix;
    IncludeReplayCandidate::LookupKind kind =
        IncludeReplayCandidate::LookupKind::Unknown;
    std::optional<uint32_t> searchChainIndex;

    // True for producer search-chain entries that this consumer deliberately
    // does not model as ordinary directories.  Such entries are not skipped:
    // if lookup reaches one before finding the requested header, the replay
    // result is unknown because the unmodeled entry may have selected or
    // shadowed the target.
    bool isUnsupportedBarrier = false;
  };

  /// Successful replay result for an ordinary include lookup.
  struct OrdinaryIncludeReplayResult {
    std::filesystem::path physicalPath;
    std::string enteredFileSpelling;
    std::string enteredFileName;
    IncludeReplayCandidate::LookupKind lookupKind =
        IncludeReplayCandidate::LookupKind::Unknown;

    // Present only for results selected by a modeled producer search-chain
    // entry.  Source-relative and absolute-operand hits have no HeaderSearch
    // cursor; legacy argv-reconstructed hits intentionally leave this empty.
    std::optional<uint32_t> searchChainIndex;
  };

  /// Failure class for ordinary include replay.
  enum class OrdinaryIncludeReplayFailureKind {
    /// Lookup succeeded or no failure has been recorded.
    None,
    /// Modeled lookup could not resolve the operand.
    Unresolved,
    /// Lookup reached an unsupported producer search-chain barrier.
    UnknownSearchChainEntry
  };

  /// File-spelling observer demand for a preserved include subtree.
  struct FileObserverDemand {
    bool observesFile = false;
    bool observesFileName = false;
  };

  /// Proof bits produced by evaluating one include replay candidate.
  struct IncludeReplayProofResult {
    bool samePhysicalFile = false;

    // `__FILE__` and `__FILE_NAME__` are related, but they are distinct
    // observer contracts.  Keep the proof bits separate so a candidate that has
    // the right basename but the wrong entered spelling cannot satisfy a
    // `__FILE__` demand, and vice versa.
    bool sameEnteredFileSpelling = true;
    bool sameEnteredFileName = true;

    bool sameIncludeNextStack = true;

    bool proves(const CleanChildIncludeReplayDemand &demand) const {
      return samePhysicalFile &&
             (!demand.observesFile || sameEnteredFileSpelling) &&
             (!demand.observesFileName || sameEnteredFileName) &&
             !demand.hasUnprovenFileSpellingObserver &&
             (demand.includeNextObligations.empty() || sameIncludeNextStack);
    }
  };

  /// Rewritten quoted ordinary include candidate plus its replay proof carrier.
  struct QuotedChildIncludeRewriteCandidate {
    std::string operand;
    IncludeReplayCandidate replay;
  };

  /// Ordinary include spelling considered as a direct `#include_next` rewrite.
  struct DirectIncludeNextOrdinaryRewriteCandidate {
    std::string operand;
    OrdinaryIncludeDelimiterKind delimiterKind =
        OrdinaryIncludeDelimiterKind::Quoted;

    // Candidate generation must remain broader than acceptance.  Keep
    // unresolved hypotheses in the list so trace mode can explain why each
    // spelling failed, but require replay to be present before any proof can
    // accept and emit the operand.
    std::optional<IncludeReplayCandidate> replay;
    OrdinaryIncludeReplayFailureKind replayFailure =
        OrdinaryIncludeReplayFailureKind::Unresolved;

    std::string origin;
  };

  /// Final-source directory surface from which clean child includes are
  /// replayed.
  struct IncludeReplaySurface {
    std::filesystem::path sourceDirectoryPath;
    std::string sourceDirectorySpelling;
  };

  /// Resolve a replay proof path against the producer preprocessor working
  /// directory when the spelling is not already absolute.
  std::filesystem::path AbsolutePathInPPCwd(StringRef path) const;

  /// Canonicalize a candidate lookup path for proof comparisons.
  ///
  /// The method resolves relative spellings through the producer preprocessor
  /// cwd first, matching the path interpretation used by the replay evaluator.
  std::optional<std::filesystem::path>
  CanonicalizeForLookupProof(StringRef path) const;

  /// Build the quoted-include replay surface for an entered source file
  /// spelling, preserving its source-directory spelling for __FILE__ proof.
  std::optional<IncludeReplaySurface>
  IncludeReplaySurfaceForFile(StringRef fileSpelling) const;

  /// Build the final emitted-source replay surface, if final replay proof has
  /// a modeled output file/directory surface available.
  std::optional<IncludeReplaySurface> FinalOutputIncludeReplaySurface() const;

  /// Return a safe relative include operand from base to target when canonical
  /// containment proves the operand cannot escape the replay surface.
  std::optional<std::string>
  StableContainedRelativeOperand(const std::filesystem::path &target,
                                 const std::filesystem::path &base) const;

  /// Producer include-search directories split by quoted/angled eligibility.
  struct RecordedIncludeSearchDirs {
    // Direct source-relative lookup is handled before these lists.  The quoted
    // list then contains the exact directories that quoted include lookup may
    // search, while the angled list contains only entries valid for angled
    // lookup.  New maps populate both lists from pp_ctx.include_search_chain,
    // preserving the producer's effective HeaderSearch order and entry index.
    SmallVector<IncludeReplaySearchDir, 48> quotedLookupDirs;
    SmallVector<IncludeReplaySearchDir, 32> angledLookupDirs;
  };

  /// Build one modeled ordinary include-search directory from producer path and
  /// spelling metadata, rejecting incomplete entries that cannot prove both
  /// physical lookup and entered-file spelling.
  std::optional<IncludeReplaySearchDir> MakeIncludeReplaySearchDir(
      StringRef physicalPath, StringRef enteredSpellingPrefix,
      IncludeReplayCandidate::LookupKind kind,
      std::optional<uint32_t> searchChainIndex = std::nullopt) const;

  /// Append a legacy argv-reconstructed search directory when its single token
  /// can safely serve as both the physical directory and entered spelling.
  void
  AppendLegacySearchDirIfSafe(SmallVectorImpl<IncludeReplaySearchDir> &dirs,
                              StringRef path,
                              IncludeReplayCandidate::LookupKind kind) const;

  /// Install an ordinary-include lookup barrier for producer search entries
  /// whose selection semantics cannot be replayed as a concrete directory.
  static void AppendUnsupportedOrdinarySearchEntryBarrier(
      RecordedIncludeSearchDirs &dirs,
      const RefoldModel::IncludeSearchEntry &entry);

  /// Append a producer HeaderSearch entry as a modeled replay directory, or as
  /// a barrier if the entry cannot prove ordinary directory lookup semantics.
  void AppendProducerSearchEntryIfSafe(
      RecordedIncludeSearchDirs &dirs,
      const RefoldModel::IncludeSearchEntry &entry) const;

  /// Reconstruct one joined or separate include-directory argv option.
  ///
  /// On success, this consumes the following argv element only for the separate
  /// spelling form, preserving the legacy parser's exact cursor behavior.
  bool TryConsumeJoinedOrSeparateIncludeArg(
      StringRef arg, ArrayRef<std::string> argv, size_t &index,
      StringRef joinedPrefix, SmallVectorImpl<IncludeReplaySearchDir> &out,
      IncludeReplayCandidate::LookupKind kind) const;

  /// Build replay search directories from the producer-normalized HeaderSearch
  /// chain, preserving ordinary lookup order and unsupported-entry barriers.
  RecordedIncludeSearchDirs ComputeProducerIncludeSearchDirs() const;

  /// Build replay search directories from legacy preprocessor argv data for
  /// maps that do not carry a normalized include-search chain.
  RecordedIncludeSearchDirs ComputeLegacyArgvIncludeSearchDirs() const;

  /// Select the best available source for recorded include-search directories.
  RecordedIncludeSearchDirs ComputeRecordedIncludeSearchDirs() const;

  /// Return the lazily cached recorded include-search directories for this
  /// proof context, preserving the old one-context cache behavior exactly.
  const RecordedIncludeSearchDirs &RecordedIncludeSearchDirsForReplay() const;

  /// Return the entered-file spelling produced by direct source-relative
  /// quoted lookup from a modeled replay surface.
  static std::string
  DirectSourceRelativeEnteredFileSpelling(const IncludeReplaySurface &surface,
                                          StringRef operand);

  /// Replay an absolute ordinary include operand as a candidate if the exact
  /// physical path exists.
  static std::optional<IncludeReplayCandidate>
  ComputeAbsoluteIncludeReplayCandidate(StringRef operand);

  /// Replay an ordinary include operand through one modeled include-search
  /// directory. Unsupported barriers deliberately produce no candidate.
  static std::optional<IncludeReplayCandidate>
  CandidateFromSearchDir(const IncludeReplaySearchDir &dir, StringRef operand);

  /// Convert a replay candidate into the ordinary include result payload used
  /// by the lookup evaluator.
  static OrdinaryIncludeReplayResult
  OrdinaryIncludeResultFromCandidate(const IncludeReplayCandidate &candidate);

  /// Convert an ordinary include replay result back into a reusable candidate
  /// carrier for later proof checks.
  static IncludeReplayCandidate IncludeReplayCandidateFromOrdinaryResult(
      const OrdinaryIncludeReplayResult &result);

  /// Replay one ordinary include spelling from the supplied surface/search
  /// model and report the first deterministic failure class when requested.
  std::optional<OrdinaryIncludeReplayResult> ComputeOrdinaryIncludeReplayResult(
      StringRef operand, OrdinaryIncludeDelimiterKind delimiterKind,
      const IncludeReplaySurface *quotedSurface,
      OrdinaryIncludeReplayFailureKind *failureKind) const;

  /// Replay a quoted ordinary include operand from a specific including-file
  /// surface and return it as a proof candidate.
  std::optional<IncludeReplayCandidate>
  ComputeQuotedIncludeReplayCandidateOnSurface(
      StringRef operand, const IncludeReplaySurface &surface) const;

  /// Replay a quoted ordinary include operand from the final emitted-source
  /// surface.
  std::optional<IncludeReplayCandidate>
  ComputeQuotedIncludeReplayCandidate(StringRef operand) const;

  /// Replay an angled ordinary include operand through the recorded angled
  /// include-search model.
  std::optional<IncludeReplayCandidate>
  ComputeAngledIncludeReplayCandidate(StringRef operand) const;

  /// Reconstruct the producer-side ordinary include candidate for an include
  /// edge using the same replay evaluator used for final-source proof.
  std::optional<IncludeReplayCandidate> ComputeProducerIncludeReplayCandidate(
      const RefoldModel::IncludeItem &include) const;

  /// Replay a `#include_next` operand from a producer-proven containing-file
  /// search-chain cursor.  The surface parameter documents the containing-file
  /// replay surface used by callers; include-next lookup itself resumes through
  /// the producer HeaderSearch chain rather than source-relative probing.
  std::optional<IncludeNextReplayCandidate> ComputeIncludeNextReplayCandidate(
      StringRef operand, const IncludeReplaySurface &surface,
      const RefoldModel::IncludeLookupProvenance &containingFile) const;

  /// Convert a replayed include-next selection into the ordinary replay
  /// candidate shape used by downstream proof gates.
  static std::optional<IncludeReplayCandidate>
  IncludeNextReplayAsOrdinaryCandidate(
      const IncludeNextReplayCandidate &selected);

  /// Convert a replay candidate lookup kind back to producer lookup
  /// provenance vocabulary without exposing the private replay carrier.
  static IncludeLookupKind ReplayCandidateLookupKindAsProducerKind(
      IncludeReplayCandidate::LookupKind kind);

  /// Build producer lookup provenance from a replay candidate when the
  /// candidate carries all cursor facts needed for later include-next proof.
  static std::optional<RefoldModel::IncludeLookupProvenance>
  ReplayCandidateLookupProvenance(const IncludeReplayCandidate &candidate);

  /// Return whether a replay candidate reproduces the producer lookup choice
  /// for an include edge, including exact search-chain index equality when the
  /// producer selected from HeaderSearch.
  bool IncludeReplayCandidateMatchesProducerLookup(
      const IncludeReplayCandidate &candidate,
      const RefoldModel::IncludeItem &include) const;

  /// Return whether every producer search-chain entry crossed by a direct
  /// `#include_next` rewrite is replayable as ordinary directory lookup.
  bool DirectIncludeNextSelectionUsesOnlyReplayableDirectories(
      const RefoldModel::IncludeItem &include) const;

  /// Return whether an ordinary include candidate is a complete proof for
  /// replacing a direct `#include_next`, preserving either the selected search
  /// cursor or proving that direct target naming is sufficient for the demand.
  bool DirectIncludeNextOrdinaryRewriteMatchesProducerSelection(
      const IncludeReplayCandidate &candidate,
      const RefoldModel::IncludeItem &include,
      const CleanChildIncludeReplayDemand &demand) const;

  /// Resolver for replaying preserved child-subtree includes during
  /// descendant include-next proof.
  ///
  /// The resolver owns the recursive replay memo, active recursion set, and
  /// failed-id set for one proof attempt.  Keeping that mutable state in this
  /// short-lived object preserves the old recursive-lambda semantics without
  /// leaking replay decisions across independent child proof evaluations.
  class PreservedChildIncludeReplayResolver {
  public:
    PreservedChildIncludeReplayResolver(
        const IncludeReplayProofContext &context,
        const RefoldModel::IncludeItem &rootChild,
        const IncludeReplayCandidate &rootCandidate);

    /// Replay an include edge inside the preserved child subtree.
    std::optional<IncludeReplayCandidate> Replay(uint64_t includeId);

  private:
    /// Mark an include id as unprovable for this resolver invocation.
    std::optional<IncludeReplayCandidate> Fail(uint64_t includeId);

    /// Replay an ordinary descendant #include from its replayed parent surface.
    std::optional<IncludeReplayCandidate>
    ReplayOrdinaryInclude(const RefoldModel::IncludeItem &include);

    /// Replay a descendant #include_next from a producer-proven cursor.
    std::optional<IncludeReplayCandidate>
    ReplayIncludeNext(const RefoldModel::IncludeItem &include);

    const IncludeReplayProofContext &context_;
    const RefoldModel::IncludeItem &rootChild_;
    DenseMap<uint64_t, IncludeReplayCandidate> replayMemo_;
    DenseSet<uint64_t> replayFailed_;
    DenseSet<uint64_t> replayActive_;
  };

  /// Prove every preserved descendant #include_next obligation against the
  /// replayed child subtree.  This check requires producer search-chain cursor
  /// provenance; matching the selected physical file alone is insufficient.
  bool IncludeNextObligationsAreProven(
      const IncludeReplayCandidate &childCandidate,
      const RefoldModel::IncludeItem &child,
      const CleanChildIncludeReplayDemand &demand) const;

  /// Evaluate whether one replay candidate preserves the child include edge
  /// under the active observer and descendant include-next proof demand.
  IncludeReplayProofResult EvaluateIncludeReplayCandidate(
      const IncludeReplayCandidate &candidate,
      const RefoldModel::IncludeItem &child,
      const CleanChildIncludeReplayDemand &demand) const;

  /// Append one quoted-child rewrite hypothesis after replaying it from the
  /// final source surface and deduplicating by operand spelling.
  void AppendQuotedChildIncludeRewriteCandidate(
      SmallVectorImpl<QuotedChildIncludeRewriteCandidate> &candidates,
      StringRef operand) const;

  /// Generate deterministic quoted-include rewrite hypotheses for a clean
  /// ordinary child include whose materialized parent changed its surface.
  SmallVector<QuotedChildIncludeRewriteCandidate, 8>
  GenerateQuotedChildIncludeRewriteCandidates(
      const RefoldModel::IncludeItem &child) const;

  /// Replay a direct #include_next ordinary-rewrite operand through the same
  /// ordinary include evaluator used by accepted final-source includes.
  std::optional<IncludeReplayCandidate>
  ReplayDirectIncludeNextOrdinaryRewriteCandidate(
      StringRef operand, OrdinaryIncludeDelimiterKind delimiterKind,
      OrdinaryIncludeReplayFailureKind *failureKind) const;

  /// Append one direct #include_next ordinary-rewrite hypothesis.  Failed
  /// replay attempts are intentionally retained so rejection tracing can report
  /// the deterministic replay failure reason for that spelling.
  void AppendDirectIncludeNextOrdinaryRewriteCandidate(
      SmallVectorImpl<DirectIncludeNextOrdinaryRewriteCandidate> &candidates,
      StringRef operand, OrdinaryIncludeDelimiterKind delimiterKind,
      StringRef origin) const;

  /// Append a direct #include_next ordinary-rewrite operand with the producer's
  /// original delimiter first, then the alternate ordinary delimiter.
  void AppendDirectIncludeNextWithPreferredDelimiters(
      SmallVectorImpl<DirectIncludeNextOrdinaryRewriteCandidate> &candidates,
      StringRef operand, StringRef origin, bool preferAngledDelimiter) const;

  /// Append a direct #include_next candidate derived from one recorded search
  /// directory, deduplicating physical directory spellings before operand
  /// synthesis. Unsupported entries remain replay barriers in the evaluator and
  /// are not used as operand sources here.
  void AppendSearchDirectoryRelativeDirectIncludeNextCandidate(
      SmallVectorImpl<DirectIncludeNextOrdinaryRewriteCandidate> &candidates,
      SmallVectorImpl<std::string> &seenSearchDirectories,
      const IncludeReplaySearchDir &dir,
      const std::filesystem::path &producerPhysicalPath,
      bool preferAngledDelimiter) const;

  /// Generate deterministic ordinary-include rewrite hypotheses for a direct
  /// #include_next whose parent has been materialized.
  SmallVector<DirectIncludeNextOrdinaryRewriteCandidate, 16>
  GenerateDirectIncludeNextOrdinaryRewriteCandidates(
      const RefoldModel::IncludeItem &child) const;

  /// Return the stable rejection vocabulary for one direct #include_next
  /// ordinary-rewrite hypothesis.  This is private static because it depends
  /// only on private replay/proof carrier facts, not context state.
  static const char *DirectIncludeNextOrdinaryRewriteRejectReason(
      const DirectIncludeNextOrdinaryRewriteCandidate &rewrite,
      const RefoldModel::IncludeItem &child,
      const CleanChildIncludeReplayDemand &demand,
      const IncludeReplayProofResult *proof, bool selectedTargetProven);

  /// Emit the single trace record for one direct #include_next ordinary-rewrite
  /// hypothesis, keeping the candidate spelling, producer facts, proof bits,
  /// and final decision reason in one stable format.
  static void TraceDirectIncludeNextOrdinaryRewriteCandidate(
      const DirectIncludeNextOrdinaryRewriteCandidate &rewrite,
      const RefoldModel::IncludeItem &child,
      const CleanChildIncludeReplayDemand &demand,
      const IncludeReplayProofResult *proof, bool selectedTargetProven,
      StringRef decisionReason);

  /// Force recursive realization of descendant include-next directives when a
  /// clean child subtree containing include-next obligations is materialized.
  static void MarkMaterializationIfIncludeNextObligationsRemain(
      CleanChildIncludeReplayPlan &plan,
      const CleanChildIncludeReplayDemand &demand);

  /// Plan replay for an angled ordinary child include from the final surface.
  CleanChildIncludeReplayPlan PlanAngledOrdinaryChildReplay(
      const RefoldModel::IncludeItem &child,
      const CleanChildIncludeReplayDemand &demand) const;

  /// Plan ordinary-include rewrite candidates for a direct #include_next whose
  /// original HeaderSearch cursor cannot be preserved after parent
  /// materialization.
  CleanChildIncludeReplayPlan PlanDirectIncludeNextOrdinaryRewrite(
      const RefoldModel::IncludeItem &child,
      const CleanChildIncludeReplayDemand &demand) const;

  /// Plan replay or deterministic operand repair for a quoted ordinary child
  /// include.
  CleanChildIncludeReplayPlan PlanQuotedOrdinaryChildReplay(
      const RefoldModel::IncludeItem &child,
      const CleanChildIncludeReplayDemand &demand) const;

  /// Return whether an include id belongs to an include subtree rooted at root.
  bool IncludeIdIsDescendantOrSelf(uint64_t owner, uint64_t root) const;

  /// Recover the producer-side spelling payload emitted by a file observer.
  std::optional<std::string> ProducerObservedFileSpellingPayload(
      const RefoldModel::MacroInvocation &macro) const;

  /// Return the preserved observable macro owner inside an include subtree.
  std::optional<uint64_t> ObservableMacroOwnerInIncludeSubtree(
      const RefoldModel::MacroInvocation &macro, uint64_t includeId) const;

  /// Build the replay demand imposed by preserved observers in the subtree.
  CleanChildIncludeReplayDemand
  BuildCleanChildIncludeReplayDemand(uint64_t includeId) const;

  /// Return whether a subtree contains preserved file-spelling observers.
  FileObserverDemand IncludeSubtreeFileObserverDemand(uint64_t includeId) const;

  /// Convert producer lookup provenance to the ordinary directory replay kinds
  /// modeled by include replay proof.  Keeping this as a private static helper
  /// avoids exposing the private IncludeReplayCandidate carrier outside this
  /// proof context while still sharing the conversion across ordinary include
  /// and include-next replay checks.
  static std::optional<IncludeReplayCandidate::LookupKind>
  ReplayableDirectoryLookupKind(IncludeLookupKind kind);

  /// Return the stable trace spelling for an ordinary include replay failure.
  /// This remains private because the failure enum is an implementation detail
  /// of the clean-child replay proof.
  static const char *
  OrdinaryIncludeReplayFailureReasonName(OrdinaryIncludeReplayFailureKind kind);

  /// Return the stable trace spelling for an ordinary include replay candidate
  /// lookup kind without exposing the private replay-candidate carrier.
  static const char *
  IncludeReplayLookupKindName(IncludeReplayCandidate::LookupKind kind);

  /// Lazily constructed per-context view of modeled include search dirs.
  mutable std::optional<RecordedIncludeSearchDirs>
      recordedIncludeSearchDirsCache_;

  IncludeReplayProofServices services_;
  const RefoldModel &model_;
  StringRef aSource_;
  const LineDirectiveInserter &lineDirs_;
  const std::optional<FinalReplaySurface> &finalReplaySurface_;
};

} // namespace refold
} // namespace clang

#endif // LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDINCLUDEREPLAYPROOF_H
