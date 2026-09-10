//===--- RefoldMacroDAGInvertibilitySolver.h ------------------*- C++ -*-===//
//
// Semantic-interaction proof engine for the DAG lifting phase.
//
// Owns the medium-grained "is this hop semantically invertible?"
// engine that the structured lifter relies on.  Builders include:
//
//   * `BuildArgRefInvertibilityCertificate` — DFS that checks whether
//     an observed text uniquely realizes an `ArgRefTemplate`.
//   * `BuildArgInvertibilityCertificate` — argument-level literal/slot
//     template inversion against an observed old expansion.
//   * `BuildArgSemanticRewriteCertificate` /
//     `BuildObservedArgRewriteCertificate` — replay the old child-slot
//     decomposition against a new observed text to produce per-slot
//     rewrite decisions and a per-argument semantic certificate.
//   * `BuildRawFormalValidationCertificate` /
//     `BuildPasteRewriteValidationCertificate` — fail-closed local
//     formal/paste validity checks driven by occurrence and paste-token
//     consistency.
//   * `BuildSemanticInteractionCertificate` /
//     `BuildSemanticInteractionSignature` /
//     `BuildFormalInteractionConsistencyCertificate` — semantic-feature
//     classification used by the structured lifter to certify that all
//     observed-rewrite paths for one formal agree on wrapper/paste
//     semantics.
//   * `BuildObservedFormalRewriteCertificate` — full formal-level
//     rewrite certificate built from the above primitives.
//   * `FormatFormalTextPairMap` — compact `{i:'old'->'new', ...}` audit
//     summary used by later layers when comparing expected/realized
//     formal rewrites.
//
// The service has no back-reference to the lifting phase or the
// planner.  `getMacroInvocationFormalArgContentRanges` and
// `macroArgReplacementMatchesAllOccurrencesInBIgnorePasteSemanticProof`
// are supplied as std::function callbacks at construction; the paste-
// argument builder is constructed on demand from the service's own
// dependencies.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDMACRODAGINVERTIBILITYSOLVER_H
#define LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDMACRODAGINVERTIBILITYSOLVER_H

#include "core/RefoldModel.h"
#include "macro/RefoldMacroDAGTextPrimitives.h"
#include "macro/RefoldMacroPasteArgumentBuilder.h"
#include "source/DiffAlgorithms.h"
#include "source/RefoldToken.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace clang {

class LangOptions;

namespace refold {

class RefoldArgTextRecovery;
class RefoldSourceMapper;

// ---- Semantic-interaction certificate types ---------------------------------

enum class ArgRefInvertibilityKind {
  Unique,
  NoMatch,
  Ambiguous,
  Unsupported,

  /// The inversion search was cut off before it had explored every
  /// assignment, so neither `Unique` nor `NoMatch` was established.
  ///
  /// Distinct from `Unsupported`, which denotes a template this solver
  /// cannot express at all. Exhaustion says the question was well posed and
  /// the answer was not paid for; only a template with a repeated caller
  /// parameter can reach it, because a template that references each
  /// parameter once never backtracks. Consumers must treat it exactly as
  /// they treat every other non-`Unique` kind -- it proves nothing -- but
  /// keeping it separate stops a cost cutoff from being read as a proof
  /// that the observed text does not realize the template.
  SearchBudgetExhausted,
};

struct ArgRefInvertibilityCertificate {
  ArgRefInvertibilityKind kind = ArgRefInvertibilityKind::Unsupported;
  llvm::DenseMap<uint32_t, std::string> derivedTextByCallerParam;
};

enum class ArgInvertibilityKind {
  LiteralOnly,
  TemplateWithChildren,
};

struct ArgInvertibilityCertificate {
  ArgInvertibilityKind kind = ArgInvertibilityKind::LiteralOnly;
  std::string rawArgText;
  llvm::SmallVector<std::string, 8> literals;
  llvm::SmallVector<LexicalChildPlaceholder, 4> slots;
  llvm::SmallVector<unsigned, 4> chosenObservedFormIdx;
};

enum class SlotRewriteDecisionKind {
  PreferredChildSyntax,
  PreserveRawInvocation,
  PassthroughFlatten,
};

struct SlotRewriteDecision {
  SlotRewriteDecisionKind kind = SlotRewriteDecisionKind::PassthroughFlatten;
  std::string observedText;
  std::string rebuiltText;

  bool operator==(const SlotRewriteDecision &other) const {
    return kind == other.kind && observedText == other.observedText &&
           rebuiltText == other.rebuiltText;
  }
};

enum class SlotSemanticRewriteCertificateKind {
  Unique,
  Invalid,
};

struct SlotSemanticRewriteCertificate {
  SlotSemanticRewriteCertificateKind kind =
      SlotSemanticRewriteCertificateKind::Invalid;
  SlotRewriteDecision decision;
  WrapperChainKind wrapperKind = WrapperChainKind::Exact;
  WrapperObservedSource wrapperSource = WrapperObservedSource::ChildExpansion;
  std::string logicalInputText;

  bool operator==(const SlotSemanticRewriteCertificate &other) const {
    return kind == other.kind && decision == other.decision &&
           wrapperKind == other.wrapperKind &&
           wrapperSource == other.wrapperSource &&
           logicalInputText == other.logicalInputText;
  }
};

enum class ArgSemanticRewriteCertificateKind {
  NoChange,
  Unique,
  Invalid,
};

enum class ArgSemanticRewriteFailure {
  None,
  MissingStructuralTemplate,
};

struct ArgSemanticRewriteCertificate {
  ArgSemanticRewriteCertificateKind kind =
      ArgSemanticRewriteCertificateKind::Invalid;
  ArgSemanticRewriteFailure failure = ArgSemanticRewriteFailure::None;
  std::string observedNewText;
  std::string rawArgOldText;
  std::string rawArgNewText;
  llvm::SmallVector<SlotRewriteDecision, 8> slotDecisions;
  llvm::SmallVector<SlotSemanticRewriteCertificate, 8> slotCertificates;
  std::string detail;
};

enum class SemanticInteractionKind {
  Plain,
  ChildSyntax,
  RawInvocation,
  Stringify,
  WideStringify,
  Paste,
  ChildSyntaxPaste,
  RawInvocationPaste,
  StringifyPaste,
  WideStringifyPaste,
  Mixed,
};

enum class SemanticInteractionFailure {
  None,
  NonCanonicalLogicalInput,
};

struct SemanticInteractionCertificate {
  bool valid = true;
  SemanticInteractionKind kind = SemanticInteractionKind::Plain;
  SemanticInteractionFailure failure = SemanticInteractionFailure::None;
  const RefoldModel::MacroInvocation *inv = nullptr;
  uint32_t argIdx = 0;
  bool touchesPaste = false;
  bool usesPreferredChildSyntax = false;
  bool usesRawInvocationPreservation = false;
  bool usesPassthroughFlatten = false;
  bool usesStringify = false;
  bool usesWideStringify = false;
  bool usesRawChildInvocationLogicalInput = false;
  llvm::SmallVector<SlotSemanticRewriteCertificate, 8> slotCertificates;
  llvm::SmallVector<std::string, 8> canonicalLogicalInputs;
  std::string detail;
};

struct SemanticInteractionSignature {
  bool touchesPaste = false;
  bool usesPreferredChildSyntax = false;
  bool usesRawInvocationPreservation = false;
  bool usesPassthroughFlatten = false;
  bool usesStringify = false;
  bool usesWideStringify = false;
  bool usesRawChildInvocationLogicalInput = false;
  llvm::SmallVector<std::string, 8> canonicalLogicalInputs;

  bool operator==(const SemanticInteractionSignature &other) const {
    return touchesPaste == other.touchesPaste &&
           usesPreferredChildSyntax == other.usesPreferredChildSyntax &&
           usesRawInvocationPreservation ==
               other.usesRawInvocationPreservation &&
           usesPassthroughFlatten == other.usesPassthroughFlatten &&
           usesStringify == other.usesStringify &&
           usesWideStringify == other.usesWideStringify &&
           usesRawChildInvocationLogicalInput ==
               other.usesRawChildInvocationLogicalInput &&
           canonicalLogicalInputs == other.canonicalLogicalInputs;
  }
};

enum class FormalInteractionConsistencyFailure {
  None,
  DivergentSemanticEvidence,
};

struct FormalInteractionConsistencyCertificate {
  bool valid = true;
  FormalInteractionConsistencyFailure failure =
      FormalInteractionConsistencyFailure::None;
  const RefoldModel::MacroInvocation *inv = nullptr;
  uint32_t argIdx = 0;
  SemanticInteractionSignature signature;
  llvm::SmallVector<SemanticInteractionCertificate, 2> interactions;
  std::string detail;
};

enum class SubtreeInteractionConsistencyFailure {
  None,
  DivergentFormalSemantics,
};

/// Verify that every formal reached through the subtree has consistent
/// semantic-interaction evidence across all lift/root paths.
///
/// A single logical formal may be encountered more than once when multiple
/// child rewrites lift to the same invocation argument. This certificate allows
/// duplicate observations only when their interaction signatures are identical.
/// Divergent signatures mean different paths disagree about whether the formal
/// depends on paste, stringify, raw invocation preservation, preferred child
/// syntax, or another sensitive semantic mode.
struct SubtreeInteractionConsistencyCertificate {
  bool valid = true;
  SubtreeInteractionConsistencyFailure failure =
      SubtreeInteractionConsistencyFailure::None;
  llvm::SmallVector<FormalInteractionConsistencyCertificate, 16>
      formalConsistencies;
  std::string detail;
};

/// Old/new text pair for one formal argument rewrite.
///
/// The pair is used by DAG lifting, subtree certification, and same-root reuse
/// logic to carry the exact spelling before and after a proven formal rewrite.
/// Callers must keep the pair tied to the formal index and invocation boundary
/// that supplied the proof; the carrier intentionally does not encode that
/// context itself.
struct FormalTextPair {
  std::string oldText;
  std::string newText;
};

struct ObservedFormalConstraint {
  std::string oldText;
  std::string newText;
};

enum class FormalRewriteCertificateKind {
  NoChange,
  Unique,
  Invalid,
};

enum class FormalRewriteFailure {
  None,
  MissingArgumentText,
  MissingStructuralTemplate,
  RawRewriteNotCertifiable,
  MergeConflict,
  ArityChange,
  OccurrenceMismatch,
  InteractionConflict,
};

enum class RawFormalValidationFailure {
  None,
  ArityChange,
  OccurrenceMismatch,
};

struct RawFormalValidationCertificate {
  bool valid = false;
  RawFormalValidationFailure failure = RawFormalValidationFailure::None;
  const RefoldModel::MacroInvocation *inv = nullptr;
  uint32_t argIdx = 0;
  std::string oldText;
  std::string newText;
  std::string detail;
};

enum class PasteRewriteValidationFailure {
  None,
  MissingInvocationText,
  MissingArgumentRanges,
  PasteMismatch,
};

struct PasteRewriteValidationCertificate {
  bool required = false;
  bool valid = true;
  bool deferred = false;
  const RefoldModel::MacroInvocation *inv = nullptr;
  llvm::DenseMap<uint32_t, std::string> replacementByArgIdx;
  PasteRewriteValidationFailure failure = PasteRewriteValidationFailure::None;
  std::string detail;
};

struct FormalRewriteCertificate {
  FormalRewriteCertificateKind kind = FormalRewriteCertificateKind::Invalid;
  FormalRewriteFailure failure = FormalRewriteFailure::None;
  const RefoldModel::MacroInvocation *inv = nullptr;
  uint32_t argIdx = 0;
  std::string oldText;
  std::string newText;
  llvm::SmallVector<FormalTextPair, 2> candidateRewrites;
  llvm::SmallVector<ArgSemanticRewriteCertificate, 2> argRewriteCertificates;
  llvm::SmallVector<SemanticInteractionCertificate, 2> interactionCertificates;
  FormalInteractionConsistencyCertificate interactionConsistency;
  RawFormalValidationCertificate validation;
  std::string detail;
};

// ---- Service class ------------------------------------------------------

/// Semantic-interaction proof engine for DAG lifting.
class RefoldMacroDAGInvertibilitySolver {
public:
  /// Borrowed inputs needed by the invertibility solver.  All references
  /// must outlive the service; the lifting phase owns them.
  struct Dependencies {
    const RefoldMacroDAGTextPrimitives &textPrimitives;
    const RefoldSourceMapper &sourceMapper;
    llvm::ArrayRef<PPTok> aToks;
    llvm::ArrayRef<PPTok> bToks;
    const clang::LangOptions &lexLang;
    const RefoldArgTextRecovery &argTextRecovery;

    /// Delegates to
    /// `RefoldMacroPatchPlanner::GetMacroInvocationFormalArgContentRanges`.
    std::function<std::optional<std::vector<std::pair<size_t, size_t>>>(
        const RefoldModel::MacroInvocation &, llvm::StringRef)>
        getMacroInvocationFormalArgContentRanges;

    /// Delegates to
    /// `RefoldMacroPatchPlanner::MacroArgReplacementMatchesAllOccurrencesInBIgnorePasteSemanticProof`.
    std::function<bool(const RefoldModel::MacroInvocation &, uint32_t,
                       llvm::StringRef, llvm::StringRef,
                       llvm::ArrayRef<diffutils::Hunk>)>
        macroArgReplacementMatchesAllOccurrencesInBIgnorePasteSemanticProof;
  };

  explicit RefoldMacroDAGInvertibilitySolver(Dependencies deps);

  /// Compare two caller-parameter index lists as sets.
  bool SameIndexSet(llvm::ArrayRef<uint32_t> a,
                    llvm::ArrayRef<uint32_t> b) const;

  /// Prove whether `observed` uniquely realizes `tpl`.  Returns the
  /// derived per-caller-parameter text on success.
  ArgRefInvertibilityCertificate
  BuildArgRefInvertibilityCertificate(const ArgRefTemplate &tpl,
                                      llvm::StringRef observed) const;

  /// Build a certificate proving how the parent formal's original
  /// argument text produced `observedOld0`.
  std::optional<ArgInvertibilityCertificate>
  BuildArgInvertibilityCertificate(const RefoldModel::MacroInvocation &parent,
                                   uint32_t parentFormal,
                                   llvm::StringRef observedOld0) const;

  /// Build a semantic rewrite certificate for one parent argument by
  /// replaying the old child-slot decomposition against the new
  /// observed text.
  ArgSemanticRewriteCertificate BuildArgSemanticRewriteCertificate(
      const ArgInvertibilityCertificate &cert, llvm::StringRef observedNew0,
      const llvm::DenseMap<uint64_t, std::string> *preferredChildSyntax) const;

  /// Convenience: combine invertibility + semantic rewrite in one call.
  ArgSemanticRewriteCertificate BuildObservedArgRewriteCertificate(
      const RefoldModel::MacroInvocation &parent, uint32_t parentFormal,
      llvm::StringRef observedOld0, llvm::StringRef observedNew0,
      const llvm::DenseMap<uint64_t, std::string> *preferredChildSyntax) const;

  /// Format a `{i:'old'->'new', ...}` audit summary string from a
  /// per-formal text-pair map.
  std::string FormatFormalTextPairMap(
      const llvm::DenseMap<uint32_t, FormalTextPair> &pairs) const;

  /// Fail-closed raw text validation for one proposed formal rewrite.
  /// `tokenHunksAR` is the canonical token-hunk array used by the
  /// occurrence-consistency check (built once per lifting call from
  /// the current hunk).
  RawFormalValidationCertificate BuildRawFormalValidationCertificate(
      const RefoldModel::MacroInvocation &inv, uint32_t argIdx,
      llvm::StringRef oldText0, llvm::StringRef newText0,
      llvm::StringRef traceStage, llvm::ArrayRef<diffutils::Hunk> tokenHunksAR,
      bool skipOccurrenceConsistency = false) const;

  /// Paste-token consistency check for a proposed set of invocation-
  /// argument replacements.  When `callsiteTextOverride` is provided,
  /// `callsiteArgRangesOverride` must also be supplied.
  PasteRewriteValidationCertificate BuildPasteRewriteValidationCertificate(
      const RefoldModel::MacroInvocation &inv,
      const llvm::DenseMap<uint32_t, std::string> &replacementByArgIdx,
      llvm::StringRef traceStage,
      std::optional<llvm::StringRef> callsiteTextOverride = std::nullopt,
      llvm::ArrayRef<std::pair<size_t, size_t>> callsiteArgRangesOverride =
          llvm::ArrayRef<std::pair<size_t, size_t>>()) const;

  /// Summarize the semantic features involved in one argument rewrite.
  SemanticInteractionCertificate BuildSemanticInteractionCertificate(
      const RefoldModel::MacroInvocation &inv, uint32_t argIdx,
      const ArgSemanticRewriteCertificate &argCert,
      llvm::StringRef traceStage) const;

  /// Project a `SemanticInteractionCertificate` down to its
  /// equivalence-key fields so all observed-rewrite paths for one
  /// formal can be compared for agreement.
  SemanticInteractionSignature BuildSemanticInteractionSignature(
      const SemanticInteractionCertificate &interaction) const;

  /// Verify that every semantic-interaction certificate produced for
  /// one formal agrees on wrapper/paste semantics.  Divergent evidence
  /// fails the formal closed.
  FormalInteractionConsistencyCertificate
  BuildFormalInteractionConsistencyCertificate(
      const RefoldModel::MacroInvocation &inv, uint32_t argIdx,
      llvm::ArrayRef<SemanticInteractionCertificate> interactions,
      llvm::StringRef traceStage) const;

  /// Full formal-level rewrite certificate.  Combines invertibility +
  /// semantic-interaction + raw validation + (when present) paste
  /// validation for the supplied observed (old,new) pairs.
  FormalRewriteCertificate BuildObservedFormalRewriteCertificate(
      const RefoldModel::MacroInvocation &inv, uint32_t argIdx,
      llvm::ArrayRef<ObservedFormalConstraint> observedConstraints,
      const llvm::DenseMap<uint64_t, std::string> *preferredChildSyntax,
      llvm::StringRef traceStage,
      llvm::ArrayRef<diffutils::Hunk> tokenHunksAR) const;

  /// Merge candidate replacement texts for one formal, requiring every
  /// candidate to share the same old text and yield a compatible new
  /// replacement.
  std::optional<std::string>
  MergeCompatibleFormalRewrites(llvm::StringRef baseOld0,
                                llvm::ArrayRef<FormalTextPair> rewrites) const;

private:
  /// Construct the on-demand paste-argument builder from this service's
  /// own dependencies.
  RefoldMacroPasteArgumentBuilder pasteArgumentBuilder() const;

  Dependencies deps_;
};

} // namespace refold
} // namespace clang

#endif // LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDMACRODAGINVERTIBILITYSOLVER_H
