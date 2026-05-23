//===--- FinalLineControlModel.h -------------------------------*- C++ -*-===//
//
// Final-stream line-control proof scaffolding for clang-refold.
//
// This module provides the stable data carriers used by the final-stream
// pruner: directives, preserved observers, physical layout obligations,
// logical line/file simulation over the final output stream, observer/layout
// liveness summaries, and the fail-closed fixed-point pruning boundary.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_FINALLINECONTROLMODEL_H
#define LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_FINALLINECONTROLMODEL_H

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace clang {
namespace refold {

/// Physical source-owner identity associated with a final-stream line-control
/// fact.
///
/// The final minimizer must not conflate the spelling emitted in a `#line`
/// directive with the owner key used for refold-map/model lookup.  The emitted
/// spelling is logical preprocessor state; this key is the physical source
/// domain that supplied the fact.
struct FinalLineControlOwnerKey {
  std::string physicalFile;
  std::optional<uint64_t> ownerIncludeId = std::nullopt;

  FinalLineControlOwnerKey() = default;
  FinalLineControlOwnerKey(std::string physicalFile,
                           std::optional<uint64_t> ownerIncludeId)
      : physicalFile(std::move(physicalFile)),
        ownerIncludeId(ownerIncludeId) {}

  std::string ToString() const;
};


/// Byte-accurate mapping from a final-output slice back to the physical source
/// bytes it copied verbatim.
///
/// This is intentionally separate from materialized B edit mappings.  It is
/// used only by the final line-control scanner to prove that a `#line` directive
/// in the final stream corresponds to a producer-recorded source directive at a
/// specific physical source byte range.  Synthetic directives and replayed B
/// payloads do not receive source mappings unless a caller can prove such a
/// byte-for-byte source correspondence.
struct FinalLineControlSourceMapping {
  uint64_t finalBegin = 0;
  uint64_t finalEnd = 0;
  std::string physicalFile;
  uint64_t sourceBegin = 0;
  uint64_t sourceEnd = 0;
  std::optional<uint64_t> ownerIncludeId = std::nullopt;

  std::string ToString() const;
};


/// Canonicalize final-to-source mapping records in deterministic final-byte
/// order.
///
/// Adjacent records are merged only when both the final byte range and the
/// physical source byte range are contiguous in the same owner domain.  This
/// preserves exact byte-for-byte provenance across internal copy-slice
/// boundaries without inventing provenance across synthetic or materialized
/// gaps.
void CanonicalizeFinalLineControlSourceMappings(
    std::vector<FinalLineControlSourceMapping> &mappings);

/// Adjust final-to-source mapping records after deleting a final-output byte
/// range.
///
/// Mappings after the deletion are shifted left.  Mappings strictly containing
/// the deleted range are split into surviving prefix/suffix records, so a
/// removed source-authored #line does not destroy provenance for neighboring
/// copied bytes.  Any mapping portion overlapping the deleted bytes is discarded
/// fail-closed.
void AdjustFinalLineControlSourceMappingsAfterDeletion(
    std::vector<FinalLineControlSourceMapping> &mappings, uint64_t removedBegin,
    uint64_t removedEnd);

/// Producer-recorded active source line-control event, lowered into the final
/// scanner's standalone model types.
///
/// These records are trusted only when `producerProven` is true and an exact
/// final-to-source byte mapping proves that the final directive bytes correspond
/// to the recorded physical source directive site.  They let the scanner use
/// Clang's already-evaluated `#line` effect for macro-expanded operands without
/// re-parsing expressions or evaluating preprocessor conditionals.
struct FinalLineControlProducerEvent {
  uint64_t id = 0;
  std::string physicalFile;
  std::optional<uint64_t> siteBegin = std::nullopt;
  std::optional<uint64_t> siteEnd = std::nullopt;
  bool active = false;
  bool producerProven = false;
  uint64_t logicalLineAfter = 0;
  std::string logicalFileAfter;
  std::optional<uint64_t> ownerIncludeId = std::nullopt;
  std::string text;

  std::string ToString() const;
};

/// Expected value observed by a preserved logical-location builtin.
///
/// Later chunks will populate it from model-backed preserved `__LINE__`,
/// `__FILE__`, and `__FILE_NAME__` evidence rather than inferring intent from
/// final source spelling.
struct FinalExpectedLineValue {
  enum class Kind : uint8_t { Unknown, Unsigned, String };

  Kind kind = Kind::Unknown;
  uint64_t unsignedValue = 0;
  std::string stringValue;

  static FinalExpectedLineValue Unknown() { return FinalExpectedLineValue(); }

  static FinalExpectedLineValue Unsigned(uint64_t value) {
    FinalExpectedLineValue out;
    out.kind = Kind::Unsigned;
    out.unsignedValue = value;
    return out;
  }

  static FinalExpectedLineValue String(std::string value) {
    FinalExpectedLineValue out;
    out.kind = Kind::String;
    out.stringValue = std::move(value);
    return out;
  }

  std::string ToString() const;
};

llvm::StringRef toString(FinalExpectedLineValue::Kind kind);

/// Whether the final-stream scanner can prove that a line-control directive is
/// executed by the final source stream.
///
/// Chunk 2 deliberately does not evaluate conditional expressions.  Literal
/// top-level line-control directives are known-active.  Literal directives that
/// appear inside an unresolved conditional region are activity-unknown unless a
/// later producer/model event proves them.
enum class FinalLineDirectiveActivity : uint8_t {
  Unknown,
  KnownInactive,
  KnownActive,
};

llvm::StringRef toString(FinalLineDirectiveActivity activity);

/// Where the logical state associated with a final directive came from.
///
/// `LiteralFinalSource` means the scanner parsed a direct literal line-control
/// spelling in the final emitted source.  It does not imply producer/model
/// proof about a source-authored directive under arbitrary conditional control.
enum class FinalLineDirectiveSemanticSource : uint8_t {
  Unknown,
  LiteralFinalSource,
  ProducerModel,
};

llvm::StringRef toString(FinalLineDirectiveSemanticSource source);

/// The scanner's conservative classification of one final physical source line.
enum class FinalPhysicalLineKind : uint8_t {
  Blank,
  CommentOnly,
  Ordinary,
  OtherDirective,
  ConditionalDirective,
  LineDirective,
  Unknown,
};

llvm::StringRef toString(FinalPhysicalLineKind kind);

/// Logical preprocessor line/file state at a point in the final source stream.
///
/// `logicalLine` is meaningful only when `lineKnown` is true.  `logicalFile` is
/// meaningful only when `fileKnown` is true.  The initial file is intentionally
/// unknown in this chunk because the final pruner must ultimately receive it
/// from the engine/model boundary rather than guess it from the emitted bytes.
struct FinalLogicalState {
  bool lineKnown = true;
  uint64_t logicalLine = 1;
  bool fileKnown = false;
  std::string logicalFile;

  static FinalLogicalState Initial() { return FinalLogicalState(); }

  void AdvancePhysicalLine() {
    if (lineKnown)
      ++logicalLine;
  }

  void ApplyKnownLineDirective(uint64_t nextLogicalLine,
                               std::optional<std::string> nextLogicalFile) {
    lineKnown = true;
    logicalLine = nextLogicalLine;
    if (nextLogicalFile) {
      fileKnown = true;
      logicalFile = std::move(*nextLogicalFile);
    }
  }

  void MarkLineUnknown() { lineKnown = false; }
  void MarkFileUnknown() { fileKnown = false; }
  void MarkUnknown() {
    MarkLineUnknown();
    MarkFileUnknown();
  }

  std::string ToString() const;
};

/// One final physical source line with the scanner's before/after logical
/// state.  This is trace-only infrastructure for the final-stream pruner.
struct FinalPhysicalLine {
  uint64_t finalBegin = 0;
  uint64_t finalEnd = 0;
  uint64_t physicalLine = 1;
  FinalPhysicalLineKind kind = FinalPhysicalLineKind::Unknown;
  FinalLogicalState stateBefore;
  FinalLogicalState stateAfter;
  uint32_t conditionalDepthBefore = 0;
  uint32_t conditionalDepthAfter = 0;
  std::optional<size_t> directiveIndex = std::nullopt;

  /// Directive identifier spelling for preprocessing-directive physical lines.
  ///
  /// For continuation lines this is the identifier from the directive that owns
  /// the continuation.  The layout proof uses this only to distinguish
  /// directive lines that are known zero-token material, such as `#define`,
  /// from directives whose -E -P effect is not locally modeled, such as
  /// `#include` or `#pragma`.
  std::string directiveName;

  std::string rawText;

  std::string ToString() const;
};

/// One `#line` / line-control directive that exists in the final emitted C
/// stream.
///
/// Offsets are final-output byte offsets using the engine-wide half-open
/// convention.  `line` and `file` are optional because the final scanner can
/// only fill them for literal/proven line-control events.  Unknown line-control
/// stays fail-closed and is never made removable by this chunk.
struct FinalLineDirective {
  enum class Origin : uint8_t {
    PreservedSource,
    SyntheticIncludeEntry,
    SyntheticIncludeReturn,
    SyntheticNewlineResync,
    SyntheticSourceLineResume,
    SyntheticTUPrologue,
    SyntheticLayoutBarrier,
    Unknown,
  };

  uint64_t finalBegin = 0;
  uint64_t finalEnd = 0;
  Origin origin = Origin::Unknown;
  std::optional<uint64_t> line = std::nullopt;
  std::optional<std::string> file = std::nullopt;
  std::optional<FinalLineControlOwnerKey> physicalOwner = std::nullopt;
  bool producerProven = false;
  std::optional<uint64_t> producerEventId = std::nullopt;
  bool removable = false;
  FinalLineDirectiveActivity activity = FinalLineDirectiveActivity::Unknown;
  FinalLineDirectiveSemanticSource semanticSource =
      FinalLineDirectiveSemanticSource::Unknown;
  bool semanticsKnown = false;
  std::string rawText;

  FinalLineDirective() = default;

  FinalLineDirective(uint64_t finalBegin, uint64_t finalEnd, Origin origin,
                     std::string rawText = std::string())
      : finalBegin(finalBegin), finalEnd(finalEnd), origin(origin),
        rawText(std::move(rawText)) {}

  std::string ToString() const;
};

llvm::StringRef toString(FinalLineDirective::Origin origin);

/// Whether a final-stream observer candidate is known to be executed.
///
/// Chunk 3 records direct source-spelled observer candidates passively.  It does
/// not evaluate conditional expressions; candidates inside unresolved
/// conditional regions are therefore activity-unknown until producer/model
/// evidence proves the selected arm.
enum class FinalObserverActivity : uint8_t {
  Unknown,
  KnownInactive,
  KnownActive,
};

llvm::StringRef toString(FinalObserverActivity activity);

/// Where a final-stream observer candidate came from.
///
/// `LexicalFinalSource` means the final scanner found a direct builtin token in
/// ordinary final source text.  It deliberately excludes preprocessing
/// directives, so macro replacement-list builtins are not mis-modeled as
/// definition-site observations; those require later producer-model extraction
/// at the actual expansion site.
enum class FinalObserverSemanticSource : uint8_t {
  Unknown,
  LexicalFinalSource,
  ProducerModel,
};

llvm::StringRef toString(FinalObserverSemanticSource source);

/// A preserved final-stream builtin occurrence whose observed value constrains
/// line-control pruning.
struct FinalObserver {
  enum class Kind : uint8_t { Line, File, FileName };

  uint64_t finalOffset = 0;
  uint64_t finalEnd = 0;
  uint64_t physicalLine = 1;
  Kind kind = Kind::Line;
  FinalExpectedLineValue expected;
  std::optional<FinalLineControlOwnerKey> physicalOwner = std::nullopt;
  FinalObserverActivity activity = FinalObserverActivity::Unknown;
  FinalObserverSemanticSource semanticSource =
      FinalObserverSemanticSource::Unknown;
  bool producerProven = false;
  bool spellingPreserved = false;
  FinalLogicalState stateBefore;
  std::string rawText;

  std::string ToString() const;
};

llvm::StringRef toString(FinalObserver::Kind kind);

/// Producer/model-backed final-stream observer at a macro expansion site.
///
/// The producer records predefined builtin macro invocations such as
/// `__LINE__`, `__FILE__`, and `__FILE_NAME__` in macro-expansion space.  When
/// such a builtin is reached through wrapper macros, the final source may
/// preserve the direct builtin spelling, an intermediate wrapper, or the
/// outermost callsite.  Each record describes one observable expansion site in
/// physical source bytes.  The final scanner adds the observer only when the
/// final emitted bytes map exactly back to that site, so materialized or
/// rewritten expansions do not become false liveness witnesses.
struct FinalLineControlProducerObserver {
  uint64_t id = 0;
  FinalObserver::Kind kind = FinalObserver::Kind::Line;
  std::string physicalFile;
  uint64_t sourceBegin = 0;
  uint64_t sourceEnd = 0;
  std::optional<uint64_t> ownerIncludeId = std::nullopt;
  bool active = false;
  bool producerProven = false;
  FinalExpectedLineValue expected;
  std::string text;

  std::string ToString() const;
};

/// Logical-location component controlled by a final-stream line-control
/// directive and observed by preserved location builtins.
///
/// Observer liveness is intentionally component-wise: a directive can be dead
/// for `__LINE__` while still live for `__FILE__` / `__FILE_NAME__`, or vice
/// versa.  The final pruner must therefore reason about line, file, and
/// basename/file-name state independently.
enum class FinalLineObserverComponent : uint8_t {
  Line,
  File,
  FileName,
};

llvm::StringRef toString(FinalLineObserverComponent component);

/// Observer-only liveness proof summary for one final-stream line-control
/// directive.
///
/// This is not the final pruning verdict.  Chunk 4 proves whether a directive
/// is live with respect to preserved logical-location observers and whether it
/// is observer-dead after component dominance.  A later layout pass must still
/// decide whether an observer-dead directive is required as an `-E -P` physical
/// layout barrier before any emitted bytes may be removed.
struct FinalLineDirectiveObserverLiveness {
  size_t directiveIndex = 0;
  uint64_t finalBegin = 0;
  uint64_t finalEnd = 0;

  bool lineLive = false;
  bool fileLive = false;
  bool fileNameLive = false;

  bool lineUnknownDependence = false;
  bool fileUnknownDependence = false;
  bool fileNameUnknownDependence = false;

  /// True iff the directive has no proven observer liveness and no unknown
  /// observer/dominance dependency.  This says only "dead with respect to
  /// observer obligations"; it deliberately does not say layout-dead.
  bool observerDead = false;

  /// True iff observer analysis would permit dropping this directive once a
  /// separate layout proof also proves that no physical-layout obligation keeps
  /// it live.
  bool removableIfLayoutDead = false;

  std::vector<std::string> reasons;

  bool HasObserverLiveComponent() const {
    return lineLive || fileLive || fileNameLive;
  }

  bool HasUnknownDependence() const {
    return lineUnknownDependence || fileUnknownDependence ||
           fileNameUnknownDependence;
  }

  std::string ToString() const;
};

class FinalLineControlModel;

/// Compute deterministic observer/dominance liveness for the final-stream
/// directives already present in \p model.
///
/// This pass never rewrites the output.  It only proves component liveness or
/// observer-deadness so later chunks can combine it with layout liveness before
/// performing final fixed-point pruning.
std::vector<FinalLineDirectiveObserverLiveness>
ComputeFinalObserverLiveness(const FinalLineControlModel &model);

/// A final-stream physical-layout obligation that may keep a `#line` directive
/// live even when no preserved logical-location builtin observes it.
struct FinalLayoutObligation {
  enum class Kind : uint8_t {
    ZeroTokenPrefixBarrier,
    ZeroTokenGapBarrier,
    FirstVisibleTokenAlignment,
    BlankLinePreservation,
  };

  uint64_t finalOffset = 0;
  Kind kind = Kind::ZeroTokenPrefixBarrier;

  /// The directive currently discharging this obligation in the final stream,
  /// when the scanner can identify one deterministically.  Unknown or
  /// ambiguous layout dependencies remain fail-closed in the layout-liveness
  /// pass instead of being attached to a guessed directive.
  std::optional<size_t> directiveIndex = std::nullopt;

  std::optional<FinalLineControlOwnerKey> physicalOwner = std::nullopt;
  std::string reason;

  std::string ToString() const;
};

llvm::StringRef toString(FinalLayoutObligation::Kind kind);

/// Layout-only liveness proof summary for one final-stream line-control
/// directive.
///
/// This is intentionally separate from observer liveness.  A directive that is
/// dead for `__LINE__` / `__FILE__` / `__FILE_NAME__` may still be needed as a
/// physical-layout barrier under `-E -P`, especially around zero-token prefix
/// material, directive/comment-only gaps, and first visible token alignment.
struct FinalLineDirectiveLayoutLiveness {
  size_t directiveIndex = 0;
  uint64_t finalBegin = 0;
  uint64_t finalEnd = 0;

  bool zeroTokenPrefixBarrierLive = false;
  bool zeroTokenGapBarrierLive = false;
  bool firstVisibleTokenAlignmentLive = false;
  bool blankLinePreservationLive = false;

  /// True when final-stream layout may depend on this directive, but the
  /// current scanner cannot prove the obligation precisely.  Future pruning
  /// must treat this as live/fail-closed.
  bool layoutUnknownDependence = false;

  /// True iff no final layout obligation and no unknown layout dependency keeps
  /// this directive live.  This is not a pruning verdict by itself; the fixed
  /// point pruner must combine it with observer liveness.
  bool layoutDead = false;

  /// True iff layout analysis would permit dropping this directive once the
  /// observer proof also says it is observer-dead.
  bool removableIfObserverDead = false;

  std::vector<std::string> reasons;

  bool HasLayoutLiveComponent() const {
    return zeroTokenPrefixBarrierLive || zeroTokenGapBarrierLive ||
           firstVisibleTokenAlignmentLive || blankLinePreservationLive;
  }

  std::string ToString() const;
};

/// Compute deterministic layout liveness for final-stream line-control
/// directives.
///
/// This pass never rewrites output.  It identifies the line directives that
/// currently discharge final-stream zero-token layout obligations and marks
/// uncertain dependencies fail-closed so later pruning cannot remove through an
/// unproved `-E -P` blank-line interaction.
std::vector<FinalLineDirectiveLayoutLiveness>
ComputeFinalLayoutLiveness(const FinalLineControlModel &model);



/// A final-stream line-control directive that a conservative emitter has
/// explicitly made eligible for fixed-point pruning.
///
/// Scanning a literal `#line` directive proves its syntax/state, but not that
/// deleting it is safe in the presence of producer-only macro expansion
/// observers.  This candidate record is therefore the explicit bridge from
/// candidate-generation code to the physical pruning pass.  Candidates match
/// scanned directives by exact final byte range.
struct FinalLineControlPruneCandidate {
  uint64_t finalBegin = 0;
  uint64_t finalEnd = 0;
  FinalLineDirective::Origin origin = FinalLineDirective::Origin::Unknown;
  std::optional<FinalLineControlOwnerKey> physicalOwner = std::nullopt;
  bool producerProven = false;
  std::string reason;

  std::string ToString() const;
};

/// One deterministic fixed-point pruning decision for a final-stream
/// line-control directive.
///
/// Decisions are trace/proof artifacts for the pruning pass.  A directive is
/// physically removed only when it is an explicit removable candidate and both
/// observer and layout liveness prove it dead.  Directives that are merely
/// scanned from the final source stream remain fail-closed unless some emitter
/// or later candidate-generation step has marked them removable.
struct FinalLineControlPruneDecision {
  uint32_t iteration = 0;
  size_t directiveIndex = 0;
  uint64_t finalBegin = 0;
  uint64_t finalEnd = 0;

  bool explicitCandidate = false;
  bool observerDead = false;
  bool layoutDead = false;
  bool verificationAttempted = false;
  bool verificationRejected = false;
  bool removed = false;

  std::vector<std::string> reasons;

  std::string ToString() const;
};

/// Result of deterministic final-stream fixed-point line-control pruning.
struct FinalLineControlPruneResult {
  std::string output;
  uint32_t iterations = 0;
  bool changed = false;
  std::vector<FinalLineControlPruneDecision> decisions;
};

/// Optional executable oracle for a proposed final-stream #line deletion.
///
/// The callback compares the current accepted emitted source against a
/// candidate source with one directive removed, normally by preprocessing both
/// through the same `clang -E -P` context.  Returning false rejects the
/// deletion fail-closed; `reason` should describe preprocessing failure,
/// unavailable context, or an output mismatch for trace diagnostics.
using FinalLineControlValidationCallback = std::function<bool(
    llvm::StringRef currentOutput, llvm::StringRef candidateOutput,
    std::string &reason)>;

/// Run deterministic fixed-point pruning over final-stream line-control
/// directives.
///
/// This is the first physical rewrite boundary for the final minimizer, but it
/// is intentionally fail-closed: a directive is eligible only when it has been
/// explicitly marked as a removable candidate and the current fixed-point
/// iteration proves it both observer-dead and layout-dead.  The pass removes at
/// most one directive per iteration, in stable final-offset order, then rescans
/// the composed source.  This implements the canonical dominance law that
/// earlier dominated directives are removed before later directives are
/// reconsidered.
FinalLineControlPruneResult
PruneFinalLineControlDirectives(
    llvm::StringRef finalSource,
    llvm::ArrayRef<FinalLineControlPruneCandidate> removableCandidates =
        llvm::ArrayRef<FinalLineControlPruneCandidate>(),
    llvm::ArrayRef<FinalLineControlSourceMapping> sourceMappings =
        llvm::ArrayRef<FinalLineControlSourceMapping>(),
    llvm::ArrayRef<FinalLineControlProducerEvent> producerEvents =
        llvm::ArrayRef<FinalLineControlProducerEvent>(),
    llvm::ArrayRef<FinalLineControlProducerObserver> producerObservers =
        llvm::ArrayRef<FinalLineControlProducerObserver>(),
    FinalLineControlValidationCallback validationCallback =
        FinalLineControlValidationCallback());

/// Emit deterministic trace diagnostics for fixed-point pruning decisions.
void TraceFinalLineControlPruneResult(
    const FinalLineControlPruneResult &result, llvm::StringRef phase);

/// Passive final-stream line-control fact collection.
///
/// This container is deliberately behavior-free: adding facts never changes the
/// emitted source.  The fixed-point pruner consumes this model to prove
/// liveness; Chunks 1-5 use it for trace visibility and as the shared data
/// boundary, while Chunk 6 rewrites only directives that are explicit removable
/// candidates.  Observer-liveness summaries are computed from this model but
/// are not cached here so the model remains a direct representation of the
/// scanned final stream.
class FinalLineControlModel {
public:
  size_t AddDirective(FinalLineDirective directive) {
    directives_.push_back(std::move(directive));
    return directives_.size() - 1;
  }

  void AddObserver(FinalObserver observer) {
    observers_.push_back(std::move(observer));
  }

  void AddLayoutObligation(FinalLayoutObligation obligation) {
    layoutObligations_.push_back(std::move(obligation));
  }

  void AddPhysicalLine(FinalPhysicalLine line) {
    physicalLines_.push_back(std::move(line));
  }

  void AddSourceMapping(FinalLineControlSourceMapping mapping) {
    sourceMappings_.push_back(std::move(mapping));
  }

  llvm::ArrayRef<FinalLineDirective> Directives() const { return directives_; }
  llvm::ArrayRef<FinalObserver> Observers() const { return observers_; }
  llvm::ArrayRef<FinalLayoutObligation> LayoutObligations() const {
    return layoutObligations_;
  }
  llvm::ArrayRef<FinalPhysicalLine> PhysicalLines() const {
    return physicalLines_;
  }
  llvm::ArrayRef<FinalLineControlSourceMapping> SourceMappings() const {
    return sourceMappings_;
  }

  bool Empty() const {
    return directives_.empty() && observers_.empty() &&
           layoutObligations_.empty() && sourceMappings_.empty();
  }

private:
  std::vector<FinalLineDirective> directives_;
  std::vector<FinalObserver> observers_;
  std::vector<FinalLayoutObligation> layoutObligations_;
  std::vector<FinalPhysicalLine> physicalLines_;
  std::vector<FinalLineControlSourceMapping> sourceMappings_;
};

/// Build a passive final-stream model by scanning \p finalSource linearly.
///
/// Literal top-level line-control directives are parsed and simulated.  Direct
/// source-spelled `__LINE__`, `__FILE__`, and `__FILE_NAME__` tokens in ordinary
/// final source lines are recorded as observer candidates.  Unknown
/// line-control, conditional observer activity, and model-only macro expansion
/// observers are kept fail-closed for later chunks.  This function does not
/// prune or rewrite the output.
FinalLineControlModel CollectFinalLineControlModel(
    llvm::StringRef finalSource,
    llvm::ArrayRef<FinalLineControlPruneCandidate> removableCandidates =
        llvm::ArrayRef<FinalLineControlPruneCandidate>(),
    llvm::ArrayRef<FinalLineControlSourceMapping> sourceMappings =
        llvm::ArrayRef<FinalLineControlSourceMapping>(),
    llvm::ArrayRef<FinalLineControlProducerEvent> producerEvents =
        llvm::ArrayRef<FinalLineControlProducerEvent>(),
    llvm::ArrayRef<FinalLineControlProducerObserver> producerObservers =
        llvm::ArrayRef<FinalLineControlProducerObserver>());

/// Backward-compatible name from Chunk 1.  It now returns the Chunk 3 model,
/// including physical-line scan records, conservative logical-state
/// simulation, and passive observer candidates, but still has no behavioral
/// effect on emitted source.
FinalLineControlModel CollectPassiveFinalLineControlModel(
    llvm::StringRef finalSource,
    llvm::ArrayRef<FinalLineControlSourceMapping> sourceMappings =
        llvm::ArrayRef<FinalLineControlSourceMapping>(),
    llvm::ArrayRef<FinalLineControlProducerEvent> producerEvents =
        llvm::ArrayRef<FinalLineControlProducerEvent>(),
    llvm::ArrayRef<FinalLineControlProducerObserver> producerObservers =
        llvm::ArrayRef<FinalLineControlProducerObserver>());

/// Emit deterministic trace diagnostics for the final-stream line-control
/// facts collected so far.
void TraceFinalLineControlModel(const FinalLineControlModel &model,
                                llvm::StringRef phase);

} // namespace refold
} // namespace clang

#endif // LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_FINALLINECONTROLMODEL_H
