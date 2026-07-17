//===--- RefoldMacroStandardArgsOnlyInternals.h ----------------*- C++ -*-===//
//
// Private declarations for standard args-only macro patch internals.
//
// This header is an implementation boundary for splitting the large
// `RefoldMacroStandardArgsOnlyPatchBuilder.cpp` translation unit.  It does not
// define a new public service: `RefoldMacroStandardArgsOnlyPatchBuilder` remains
// the only public standard-args entry point.  The types declared here are stable
// private carriers/resolvers used by the builder's occurrence-observation,
// proof-certification, and generated-replay pipeline.  They are exposed only so
// sibling implementation files can share the same explicit state without
// changing replay admission, proof semantics, or candidate ordering.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDMACROSTANDARDARGSONLYINTERNALS_H
#define LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDMACROSTANDARDARGSONLYINTERNALS_H

#include "macro/RefoldMacroStandardArgsOnlyPatchBuilder.h"

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

/// Standard args-only hunk evidence for the formals touched by one seed hunk.
struct TouchedFormalHunkCollection {
  /// Standard and stringify occurrences used for hunk-ownership checks.
  std::vector<RefoldModel::PPArgSpan> occurrences;

  /// Parallel flag for `occurrences`; true entries are stringify observations.
  std::vector<char> occurrenceIsStringify;

  /// Formal-index bitmap derived from the touched occurrences.
  std::vector<char> touchedFormals;

  /// Primary, sibling, and synthetic token hunks that downstream replay must
  /// explain for the touched formals.
  llvm::SmallVector<diffutils::Hunk, 8> tokenHunks;
};

/// Collects every token hunk that must be explained by the touched-formal
/// args-only proof.
///
/// The collector owns only the hunk-coverage obligation for the already built
/// standard/stringify occurrence set.  It is allowed to mutate the returned
/// `touchedFormals` bitmap and `tokenHunks` list; it does not observe or change
/// occurrence-rewrite evidence, tuple-generated-callee replay state, proof
/// carriers, or candidate ordering.  Unsupported ownership cases fail closed by
/// returning `std::nullopt`: the collector never widens outside the current
/// invocation cover and never invents a fallback hunk.  Sibling token hunks are
/// appended in the original diff order, synthetic insertion-frontier envelopes
/// are appended after the seed hunk snapshot in the same order as the former
/// local lambdas, and the existing deterministic sort/unique normalization is
/// preserved as the final collection step.
class TouchedFormalHunkCollector {
public:
  TouchedFormalHunkCollector(
      const RefoldMacroStandardArgsOnlyPatchBuilder::Dependencies &deps,
      const RefoldMacroOccurrenceReplay &occurrenceReplay);

  /// Return the completed touched-formal hunk collection, or nullopt when the
  /// primary hunk or any required sibling/synthetic ownership proof cannot be
  /// established inside this invocation's recorded argument occurrences.
  std::optional<TouchedFormalHunkCollection>
  Collect(TouchedFormalHunkCollection collection,
          const RefoldModel::MacroInvocation &invocation,
          const diffutils::Hunk &primaryHunk,
          size_t invocationArgCount) const;

private:
  bool HunkTouchesFormalOccurrence(
      const diffutils::Hunk &cand, const RefoldModel::PPArgSpan &sp,
      llvm::ArrayRef<RefoldModel::PPArgSpan> occs) const;

  bool HunkTouchesTouchedFormal(const diffutils::Hunk &cand,
                                llvm::ArrayRef<RefoldModel::PPArgSpan> occs,
                                const std::vector<char> &touched) const;

  void MaybeAddSyntheticTouchedFormalEnvelope(
      const RefoldModel::MacroInvocation &invocation,
      const RefoldModel::PPArgSpan &sp, const diffutils::Hunk &anchor,
      llvm::ArrayRef<RefoldModel::PPArgSpan> occs,
      llvm::SmallVector<diffutils::Hunk, 8> &tokenHunks) const;

  const RefoldMacroStandardArgsOnlyPatchBuilder::Dependencies &deps_;
  const RefoldMacroOccurrenceReplay &occurrenceReplay_;
};

/// One old/new observation for a formal occurrence in the args-only replay.
struct OccObservation {
  /// The original spelling contributed by one occurrence of a formal argument.
  ///
  /// Stored by value so extracted observation collection can safely preserve
  /// unstringified old-side spellings after the collector returns.
  std::string oldText;

  /// The rewritten spelling inferred for that same occurrence from the token
  /// hunk set.
  std::string newText;

  /// Optional byte range inside `newText` that corresponds exactly to B-only
  /// insertion payload owned by this occurrence.  When absent, the whole
  /// rewritten argument remains the conservative materialized output range.
  std::optional<std::pair<uint64_t, uint64_t>> materializedNewTextRange;
};

/// Mutable evidence accumulated while observing one touched formal argument.
///
/// The caller fills this carrier in the existing occurrence order and then uses
/// it to decide whether a formal has a uniform replacement or requires the
/// narrower tuple-forwarding proof.  Missing source maps, untracked
/// materialized ranges, delimiter-boundary repairs, and inconsistent
/// observations retain their existing fail-closed behavior; the collector
/// returns this carrier without admitting a candidate or certifying proof state.
struct InvocationOccurrenceObservationSet {
  /// Ordered old/new observations for the current formal.
  llvm::SmallVector<OccObservation, 8> observations;

  /// Uniform replacement if all observations seen so far agree.
  std::optional<std::string> unifiedNewArg;

  /// Union of precise materialized B ranges when every observation tracked one.
  std::optional<std::pair<uint64_t, uint64_t>>
      unifiedMaterializedNewTextRange;

  /// True when at least one observation could not prove a precise materialized
  /// B range, forcing conservative proof certification for that formal.
  bool sawUntrackedMaterializedNewTextRange = false;

  /// True when observations disagree and tuple-forwarding must prove a narrower
  /// slice rewrite before the args-only candidate may be accepted.
  bool needTupleForwarding = false;
};

/// Final per-argument rewrite set handed to the args-only proof certifier.
///
/// This carrier is the boundary between occurrence/rewrite discovery and
/// accepted-candidate construction.  Discovery records exactly the same
/// replacement spellings and materialized output subranges as before, in the
/// same touched-argument order; the certifier only consumes the completed maps.
/// Tuple-forwarding status is recorded for the internal proof boundary but does
/// not alter the current public proof carrier, candidate ranking, or fallback
/// policy.
struct ArgsOnlyFinalArgumentRewriteSet {
  /// Rewritten invocation actual text by formal argument index.
  llvm::DenseMap<uint32_t, std::string> replacementsByArgIdx;

  /// Precise materialized output subrange by formal argument index when proved.
  llvm::DenseMap<uint32_t, std::pair<uint64_t, uint64_t>>
      materializedRangeByArgIdx;

  /// Whether the final spelling for an argument came through tuple forwarding.
  llvm::DenseMap<uint32_t, bool> tupleForwardedByArgIdx;

  /// Record one finalized formal replacement without changing admission policy.
  void Record(uint32_t argIdx, std::string finalNewArg,
              std::optional<std::pair<uint64_t, uint64_t>> materializedRange,
              bool tupleForwarded);

  bool Empty() const;
};

/// Builds the pure paste-only args-only candidate once the caller has proven
/// that no STANDARD or STRINGIFY occurrence evidence is available.
///
/// This private builder owns only the paste-only fallback construction block:
/// it derives per-argument paste edits, requires every paste occurrence for the
/// invocation to agree with the merged replacements, rebuilds the invocation,
/// and certifies the same paste-only proof as the former inline path.  It
/// mutates only the returned `MacroPatch` and the existing proof lattice /
/// certifier state.  It does not decide candidate ordering, does not relax the
/// caller's pure-paste admission guard, and fails closed on every ambiguous
/// segment split, arity-changing comma introduction, conflicting repeated paste
/// occurrence, or failed invocation reconstruction.
class PurePasteOnlyArgsOnlyCandidateBuilder {
public:
  PurePasteOnlyArgsOnlyCandidateBuilder(
      const RefoldMacroStandardArgsOnlyPatchBuilder::Dependencies &deps,
      RefoldMacroPasteArgumentBuilder pasteBuilder);

  /// Return the paste-only args-only candidate for this invocation, or nullopt
  /// under exactly the same rejection conditions as the previous inline
  /// fallback block.
  std::optional<MacroPatch>
  TryBuild(const RefoldModel::MacroInvocation &invocation,
           const diffutils::Hunk &hunk, llvm::StringRef baseInvocationText,
           llvm::ArrayRef<std::pair<size_t, size_t>> invocationArgRanges,
           const InvocationActualRecoveryContext &actualCtx) const;

private:
  const RefoldMacroStandardArgsOnlyPatchBuilder::Dependencies &deps_;
  RefoldMacroPasteArgumentBuilder pasteBuilder_;
};

/// Builds the final standard-args-only invocation patch and certifies its proof.
///
/// This resolver owns only the final proof/candidate construction obligation for
/// the ordinary standard-argument replay path.  It consumes the already-finalized
/// argument replacement map, delegates invocation text rebuilding to the existing
/// planner callback, certifies the same materialized output and whole-expansion
/// B range as before, and assigns the same args-only standard proof summary.
/// It mutates only the returned `MacroPatch` and the existing proof certifier; it
/// does not discover new rewrites, reorder candidates, relax replay validation,
/// or introduce a fallback when the rewrite cannot be rebuilt.
class ArgsOnlyProofCertifier {
public:
  explicit ArgsOnlyProofCertifier(
      const RefoldMacroStandardArgsOnlyPatchBuilder::Dependencies &deps);

  /// Return the accepted patch for the completed rewrite set, or nullopt when
  /// there is no replacement or invocation reconstruction fails exactly as in
  /// the previous inline certification block.
  std::optional<MacroPatch>
  BuildAcceptedCandidate(const RefoldModel::MacroInvocation &invocation,
                         const InvocationActualRecoveryContext &actualCtx,
                         const ArgsOnlyFinalArgumentRewriteSet &rewriteSet)
      const;

private:
  const RefoldMacroStandardArgsOnlyPatchBuilder::Dependencies &deps_;
};

/// Collects the old/new occurrence evidence for one touched formal argument.
///
/// The collector owns the per-formal observation obligation: every recorded
/// standard/stringify occurrence for `argIdx` is visited in the caller-provided
/// order, its B-side envelope is widened with only the already-proven hunk set,
/// stringify and paste inversions are applied exactly as before, and the result
/// carrier records whether all observations collapse to one replacement or need
/// tuple forwarding.  The only mutation is to the returned carrier.  Missing
/// source maps, unsupported stringify inversion, and unprovable materialized
/// ranges retain the existing fail-closed behavior by returning nullopt or by
/// marking the range untracked; this component never admits a candidate and never
/// changes proof certification or candidate ordering.
class InvocationOccurrenceObservationCollector {
public:
  InvocationOccurrenceObservationCollector(
      const RefoldMacroStandardArgsOnlyPatchBuilder::Dependencies &deps,
      const RefoldMacroOccurrenceReplay &occurrenceReplay);

  /// Return the ordered occurrence evidence for `argIdx`, or nullopt when the
  /// occurrence-level rewrite cannot be proven under the existing rules.
  std::optional<InvocationOccurrenceObservationSet>
  Collect(const RefoldModel::MacroInvocation &invocation, uint32_t argIdx,
          llvm::StringRef baseArgText,
          llvm::ArrayRef<RefoldModel::PPArgSpan> occurrences,
          llvm::ArrayRef<char> occurrenceIsStringify,
          llvm::ArrayRef<diffutils::Hunk> tokenHunks,
          const diffutils::Hunk &primaryHunk) const;

private:
  const RefoldMacroStandardArgsOnlyPatchBuilder::Dependencies &deps_;
  const RefoldMacroOccurrenceReplay &occurrenceReplay_;
};

/// Validates that tuple-forwarded slice rewrites match every standard
/// occurrence of the same formal in B.
class TupleSliceConsistencyValidator {
public:
  TupleSliceConsistencyValidator(
      const RefoldMacroStandardArgsOnlyPatchBuilder::Dependencies &deps,
      const RefoldMacroOccurrenceReplay &occurrenceReplay);

  bool Validate(uint32_t argIdx,
                llvm::ArrayRef<RefoldModel::PPArgSpan> standardArgSpans,
                llvm::ArrayRef<OccObservation> observations,
                llvm::ArrayRef<diffutils::Hunk> tokenHunks) const;

private:
  const RefoldMacroStandardArgsOnlyPatchBuilder::Dependencies &deps_;
  const RefoldMacroOccurrenceReplay &occurrenceReplay_;
};

/// Rebuilds a caller argument through a structural tuple-forwarding proof.
///
/// This resolver owns only the caller-tuple bridge used after occurrence
/// observation has already shown that one formal either needs tuple forwarding
/// or may benefit from a narrower tuple-element rewrite.  It searches direct
/// children of the current invocation for one unambiguous forwarding witness,
/// delegates generated-callee tuple-element derivation to the generated replay
/// helpers, and mutates only the returned replacement spelling.  Non-applicable
/// shapes return false so the caller can preserve its existing fallback path;
/// ambiguous children, duplicate old-text keys, invalid tuple slices, and
/// conflicting derived rewrites continue to reject fail-closed.
class CallerTupleForwardedRewriteResolver {
public:
  CallerTupleForwardedRewriteResolver(
      const RefoldMacroStandardArgsOnlyPatchBuilder::Dependencies &deps,
      const RefoldModel::MacroInvocation &invocation);

  /// Try to rebuild `baseArgText` as a structural caller-tuple edit.
  bool TryRewrite(uint32_t callerArgIdx, llvm::StringRef baseArgText,
                  llvm::ArrayRef<OccObservation> occObservations,
                  std::string &outNewArg) const;

private:
  static std::optional<llvm::StringRef>
  GetNormalizedArgText(const RefoldModel::MacroInvocation &inv,
                       uint32_t argIdx);

  static std::optional<llvm::StringRef>
  GetInvocationArgText(const RefoldModel::MacroInvocation &inv,
                       uint32_t argIdx);

  const RefoldMacroStandardArgsOnlyPatchBuilder::Dependencies &deps_;
  const RefoldModel::MacroInvocation &invocation_;
};

/// Probes generated replay candidates before ordinary occurrence collection.
///
/// The probe owns the higher-order ranking sequence used before ordinary
/// occurrence collection: direct generated-callee replay, object-selector
/// tuple replay, paste-derived tuple replay, direct tuple-generated-callee
/// replay, recursive tuple-generated-callee replay, and finally generated-leaf
/// fallback.  A miss remains non-terminal and returns
/// nullopt; ambiguity, unsupported shapes, and unprovable B envelopes continue
/// to fail closed through the delegated replay engines rather than introducing a
/// fallback candidate.
class HigherOrderGeneratedReplayProbe {
public:
  explicit HigherOrderGeneratedReplayProbe(
      const RefoldMacroStandardArgsOnlyPatchBuilder::Dependencies &deps);

  /// Return the first higher-order generated replay candidate in theorem order,
  /// or nullopt when no generated replay proof applies.
  std::optional<MacroPatch>
  TryBuild(const RefoldModel::MacroInvocation &invocation,
           const diffutils::Hunk &hunk, llvm::StringRef baseInvocationText,
           llvm::ArrayRef<std::pair<size_t, size_t>> invocationArgRanges) const;

private:
  const RefoldModel::MacroDirective *
  FindRootDefinition(const RefoldModel::MacroInvocation &invocation) const;

  std::optional<std::pair<size_t, size_t>>
  MapWholeCoverBEnvelope(
      const std::pair<uint64_t, uint64_t> &wholeCoverATokens) const;

  std::optional<MacroPatch> TryBuildGeneratedCalleeReplay(
      const RefoldModel::MacroInvocation &invocation,
      llvm::StringRef baseInvocationText,
      llvm::ArrayRef<std::pair<size_t, size_t>> invocationArgRanges) const;

  std::optional<MacroPatch> TryBuildGeneratedLeafReplay(
      const RefoldModel::MacroInvocation &invocation,
      const diffutils::Hunk &hunk, llvm::StringRef baseInvocationText,
      llvm::ArrayRef<std::pair<size_t, size_t>> invocationArgRanges) const;

  std::optional<MacroPatch> TryBuildRecursiveTupleGeneratedCalleeReplay(
      const RefoldModel::MacroInvocation &invocation,
      llvm::StringRef baseInvocationText,
      llvm::ArrayRef<std::pair<size_t, size_t>> invocationArgRanges) const;

  std::optional<MacroPatch> TryBuildTupleGeneratedCalleeReplay(
      const RefoldModel::MacroInvocation &invocation,
      llvm::StringRef baseInvocationText,
      llvm::ArrayRef<std::pair<size_t, size_t>> invocationArgRanges) const;

  std::optional<MacroPatch> TryBuildPasteTupleGeneratedCalleeReplay(
      const RefoldModel::MacroInvocation &invocation,
      llvm::StringRef baseInvocationText,
      llvm::ArrayRef<std::pair<size_t, size_t>> invocationArgRanges) const;

  std::optional<MacroPatch> TryBuildPasteGeneratedCalleeReplay(
      const RefoldModel::MacroInvocation &invocation,
      llvm::StringRef baseInvocationText,
      llvm::ArrayRef<std::pair<size_t, size_t>> invocationArgRanges) const;

  std::optional<MacroPatch> TryBuildObjectSelectorTupleGeneratedCalleeReplay(
      const RefoldModel::MacroInvocation &invocation,
      llvm::StringRef baseInvocationText,
      llvm::ArrayRef<std::pair<size_t, size_t>> invocationArgRanges) const;

  std::optional<MacroPatch> TryBuildFunctionSelectorTupleGeneratedCalleeReplay(
      const RefoldModel::MacroInvocation &invocation,
      llvm::StringRef baseInvocationText,
      llvm::ArrayRef<std::pair<size_t, size_t>> invocationArgRanges) const;

  const RefoldMacroStandardArgsOnlyPatchBuilder::Dependencies &deps_;
};

/// Validates repaired definition-tape formal occurrences against every B-side
/// materialization before the args-only rewrite is admitted.
class ReplayedFormalOccurrenceValidator {
public:
  ReplayedFormalOccurrenceValidator(
      const RefoldMacroStandardArgsOnlyPatchBuilder::Dependencies &deps,
      const RefoldMacroOccurrenceReplay &occurrenceReplay);

  /// Return whether `newArg` matches every repaired STANDARD occurrence.
  bool Validate(uint32_t argIdx, llvm::StringRef newArg,
                llvm::ArrayRef<RefoldModel::PPArgSpan> standardArgSpans,
                llvm::ArrayRef<diffutils::Hunk> tokenHunks) const;

private:
  const RefoldMacroStandardArgsOnlyPatchBuilder::Dependencies &deps_;
  const RefoldMacroOccurrenceReplay &occurrenceReplay_;
};

} // namespace refold
} // namespace clang

#endif // LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDMACROSTANDARDARGSONLYINTERNALS_H
