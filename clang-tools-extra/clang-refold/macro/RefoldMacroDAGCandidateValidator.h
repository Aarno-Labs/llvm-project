//===--- RefoldMacroDAGCandidateValidator.h -------------------*- C++ -*-===//
//
// Final DAG-candidate validator for the lifting phase.
//
// Consumes the subtree-rewrite certificate produced by
// `RefoldMacroDAGSubtreeCertifier` and turns it into a concrete root
// `MacroPatch` candidate, then validates and merges
// that candidate against any other DAG candidates produced for the
// same root.  The lifting phase's main lift loop calls this service
// once per discovered leaf candidate; the service mutates the borrowed
// acceptance state in `DagCandidateAcceptanceContext` to track the
// best unique-or-merged candidate.
//
// Methods:
//   * `BuildRootPatchConstructionCertificate` — turn a proven root
//     invocation rewrite certificate into byte-range edits inside the
//     root invocation spelling.
//   * `MergeDagCandidateValidationMetadata` — compose two candidate
//     validation summaries for the same root span.
//   * `GetInvocationHeadShape` / `PreservesRootInvocationHead` /
//     `CountPreservedInvocationHeads` — head-shape heuristics for
//     structured-preservation tie-breaking.
//   * `ChoosePreferredStructuredDagCandidate` — tie-breaker between
//     two candidates that both claim the same root span.
//   * `RootDeferredPasteReplayHasOnlyProvenDependentUses` — extra
//     replay-safety check for deferred paste obligations.
//   * `BuildRootProofValidationCertificate` — full root-rewrite proof
//     against B (occurrence, paste, and head-shape consistency).
//   * `ValidateDagCandidateProof` — the gate every candidate passes
//     before being accepted into the acceptance context.
//   * `BuildDagCandidateValidationMetadataFromSubtree` — project a
//     subtree-rewrite certificate into the validation metadata used by
//     the acceptance path.
//   * `AcceptOrMergeDAGCandidatePatch` — main entry: validate, then
//     accept-or-merge, mutating the acceptance context.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDMACRODAGCANDIDATEVALIDATOR_H
#define LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDMACRODAGCANDIDATEVALIDATOR_H

#include "core/RefoldModel.h"
#include "edit/RefoldPatchTypes.h"
#include "macro/RefoldMacroDAGInvertibilitySolver.h"
#include "macro/RefoldMacroDAGLiftingContext.h"
#include "macro/RefoldMacroDAGStructuredLifter.h"
#include "macro/RefoldMacroDAGSubtreeCertifier.h"
#include "macro/RefoldMacroDAGTextPrimitives.h"
#include "macro/RefoldMacroOccurrenceProofValidator.h"
#include "source/RefoldToken.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/ADT/StringSet.h"

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
class RefoldMacroTopology;
class RefoldMacroWholeCoverPlanBuilder;
class RefoldProofLattice;
class RefoldSourceMapper;

// ---- DAG-candidate validation types -----------------------------------------

struct ArgEdit {
  uint64_t begin;
  uint64_t end;
  std::string repl;
};

enum class RootPatchConstructionCertificateKind {
  NoChange,
  Unique,
  Invalid,
};

enum class RootPatchConstructionFailure {
  None,
  ArgIndexOutOfBounds,
  InvalidArgRange,
  OverlappingEdits,
};

/// Construct the concrete source patch for an accepted root invocation rewrite
/// certificate.
///
/// The root invocation rewrite certificate has already proven which root
/// formals should be rewritten. This construction certificate turns those
/// formal rewrites into byte-range edits inside the original root invocation
/// spelling, verifies that the resulting argument edits are in bounds and
/// non-overlapping, and then materializes one `MacroPatch` over the full
/// invocation span.
///
/// This is a construction step, not a semantic proof step: it does not decide
/// whether the rewrite is valid. It only verifies that the accepted
/// root-certificate rewrites can be represented as a single well-formed textual
/// patch.
struct RootPatchConstructionCertificate {
  RootPatchConstructionCertificateKind kind =
      RootPatchConstructionCertificateKind::Invalid;
  RootPatchConstructionFailure failure = RootPatchConstructionFailure::None;
  llvm::SmallVector<ArgEdit, 8> edits;
  std::optional<MacroPatch> patch;
  std::string detail;
};

enum class DagCandidateAcceptanceFailure {
  None,
  DifferentSpan,
  DifferentBaseText,
  MergeConflict,
  MergedRootValidationFailed,
};

struct DagCandidateAcceptanceCertificate {
  bool accepted = false;
  bool merged = false;
  DagCandidateAcceptanceFailure failure = DagCandidateAcceptanceFailure::None;
  std::string detail;
};

/// Validation metadata carried by a DAG candidate after subtree projection.
///
/// The metadata records proof obligations that must remain visible when several
/// candidates for the same root are composed or replay-validated: deferred root
/// occurrence arguments, expected root-formal rewrites, bridge-sensitive
/// semantic signatures, and mixed semantic-interaction hazards. Merge logic may
/// compose metadata only when duplicate evidence agrees and independently
/// produced root-formal rewrites are compatible against the original root
/// argument text.
struct DagCandidateValidationMetadata {
  llvm::SmallVector<uint32_t, 8> deferOccurrenceArgIdxs;
  llvm::DenseMap<uint32_t, FormalTextPair> expectedRootFormals;
  llvm::StringMap<SemanticInteractionSignature> bridgeSensitiveFormalSignatures;
  bool hasExpectedRootFormals = false;
  bool hasBridgeSensitiveStructuredSemantics = false;
  bool hasMixedSemanticInteractions = false;
};

struct DagCandidateAcceptanceContext {
  const MacroSubtreeReplayValidationContext &subtreeValidation;
  std::optional<MacroPatch> &uniquePatch;
  std::optional<std::string> &uniquePatchBaseText;
  DagCandidateValidationMetadata &uniquePatchValidation;
  unsigned &distinctRootPatches;
};

/// Callee spelling and argument count recovered from a macro invocation text
/// fragment.
///
/// This intentionally records only the invocation head shape, not the full
/// argument contents. Callers use it to check whether a rewrite preserves the
/// same outer invocation boundary while allowing the argument text itself to
/// change.
struct InvocationHeadShape {
  std::string callee;
  size_t argCount = 0;
};

/// Full root-rewrite proof against the B stream.
///
/// Replays the proposed root replacement, checks that the invocation head shape
/// remains admissible, and records the replayed root-formal map plus the replay
/// invocation certificate used by final DAG-candidate validation.
struct RootProofValidationCertificate {
  bool valid = false;
  llvm::DenseMap<uint32_t, FormalTextPair> replayRootFormals;
  InvocationRewriteCertificate replayInvocationCertificate;
  std::string detail;
};

// ---- Service class ------------------------------------------------------

/// Final DAG-candidate validator.
class RefoldMacroDAGCandidateValidator {
public:
  /// Borrowed inputs needed by the candidate validator.  All references
  /// must outlive the service; the lifting phase owns them.
  struct Dependencies {
    const RefoldMacroDAGTextPrimitives &textPrimitives;
    const RefoldMacroDAGInvertibilitySolver &invertibilitySolver;
    const RefoldMacroDAGStructuredLifter &structuredLifter;
    const RefoldMacroDAGSubtreeCertifier &subtreeCertifier;
    const RefoldSourceMapper &sourceMapper;
    llvm::ArrayRef<PPTok> bToks;
    const RefoldArgTextRecovery &argTextRecovery;
    const RefoldMacroTopology &macroTopology;
    RefoldProofLattice &proofLattice;
    const clang::LangOptions &lexLang;

    /// Delegates to
    /// `RefoldMacroPatchPlanner::GetMacroInvocationFormalArgContentRanges`.
    std::function<std::optional<std::vector<std::pair<size_t, size_t>>>(
        const RefoldModel::MacroInvocation &, llvm::StringRef)>
        getMacroInvocationFormalArgContentRanges;

    /// Whole-cover plan computation, used as target-PP proof for a DAG
    /// subtree-root repair whose rewritten formal has no direct
    /// PPArgSpan -> B-token envelope of its own.
    const RefoldMacroWholeCoverPlanBuilder &wholeCoverPlanBuilder;
  };

  explicit RefoldMacroDAGCandidateValidator(Dependencies deps);

  /// Turn a proven root invocation rewrite certificate into byte-range edits
  /// inside the root invocation spelling and certify the patch.
  ///
  /// The semantic proof has already established the root-formal rewrite.  This
  /// method is responsible for representability only: translating formal text
  /// changes into non-overlapping argument byte edits and producing one
  /// well-formed MacroPatch over the root invocation span.
  RootPatchConstructionCertificate BuildRootPatchConstructionCertificate(
      const RefoldMacroDAGLiftingContext &ctx,
      const InvocationRewriteCertificate &rootCert,
      llvm::StringRef traceStage) const;

  /// Merge validation metadata from two DAG candidates being composed into one
  /// candidate.
  ///
  /// Returns nullopt when bridge-sensitive semantic signatures, deferred root
  /// occurrence arguments, expected root-formal rewrites, or mixed-interaction
  /// hazards disagree in a way that cannot be represented by one root patch.
  std::optional<DagCandidateValidationMetadata>
  MergeDagCandidateValidationMetadata(
      const RefoldMacroDAGLiftingContext &ctx,
      const DagCandidateValidationMetadata &lhs,
      const DagCandidateValidationMetadata &rhs) const;

  /// Recover the invocation head shape (callee + arg count) for a
  /// piece of refold text.
  std::optional<InvocationHeadShape>
  GetInvocationHeadShape(const RefoldMacroDAGLiftingContext &ctx,
                         llvm::StringRef text) const;

  /// True when `rewrite.oldText` and `rewrite.newText` share the same
  /// callee and arg count at top level.
  bool PreservesRootInvocationHead(const RefoldMacroDAGLiftingContext &ctx,
                                   const FormalTextPair &rewrite) const;

  /// Recursive structural-preservation score for two refold fragments,
  /// counting top-level + nested preserved invocation heads.
  unsigned
  CountPreservedInvocationHeads(const RefoldMacroDAGLiftingContext &ctx,
                                llvm::StringRef oldText,
                                llvm::StringRef newText) const;

  /// Tie-breaker between two candidates that both claim the same root
  /// span.  Returns a positive value to prefer the candidate, negative
  /// to prefer the existing, or zero when the lattice cannot decide.
  int ChoosePreferredStructuredDagCandidate(
      const RefoldMacroDAGLiftingContext &ctx,
      const DagCandidateValidationMetadata &existingValidation,
      const DagCandidateValidationMetadata &candidateValidation) const;

  /// Extra replay-safety check for wrapper-placeholder replay.
  ///
  /// Wrapper-placeholder replay is used only after ordinary paste replay has
  /// failed. In that mode the candidate text proves that one observed paste
  /// chain can be reconstructed, but it does not by itself prove that changing
  /// the root formal is safe for every other descendant use of that formal.
  ///
  /// Reject the deferred wrapper replay unless every descendant output range
  /// controlled by each changed root formal is covered by the current token
  /// diff. Hidden paste-derived macro selectors are rejected because the
  /// current proof does not yet carry selector-safety evidence for them.
  bool RootDeferredPasteReplayHasOnlyProvenDependentUses(
      const RefoldMacroDAGLiftingContext &ctx,
      const llvm::DenseMap<uint32_t, FormalTextPair> &rootFormals,
      llvm::StringRef traceStage, std::string &failureDetail) const;

  /// Full root-rewrite proof against B (occurrence, paste, head-shape
  /// consistency).
  RootProofValidationCertificate BuildRootProofValidationCertificate(
      const RefoldMacroDAGLiftingContext &ctx, llvm::StringRef baseText,
      llvm::StringRef newText, llvm::ArrayRef<uint32_t> deferOccurrenceArgIdxs,
      const llvm::DenseMap<uint32_t, FormalTextPair> *expectedRootFormals,
      llvm::StringRef traceStage) const;

  /// Gate every candidate passes before being accepted into the
  /// acceptance context.
  bool
  ValidateDagCandidateProof(const RefoldMacroDAGLiftingContext &ctx,
                            const DagCandidateAcceptanceContext &accCtx,
                            const DagCandidateValidationMetadata &validation,
                            llvm::StringRef baseText, llvm::StringRef newText,
                            llvm::StringRef traceStage) const;

  /// Convert a proven subtree rewrite certificate into the validation metadata
  /// carried by a DAG candidate.
  ///
  /// The resulting metadata is used later when multiple DAG candidates are
  /// composed or replay-validated at the root. It preserves the expected
  /// root-formal rewrites, deferred root occurrence arguments, and any semantic
  /// hazards that must remain globally visible after candidate construction.
  DagCandidateValidationMetadata BuildDagCandidateValidationMetadataFromSubtree(
      const RefoldMacroDAGLiftingContext &ctx,
      const SubtreeRewriteCertificate &subtreeCert) const;

  /// Main entry: validate, then accept-or-merge the candidate patch.
  /// Mutates `accCtx` (`uniquePatch`, `uniquePatchBaseText`,
  /// `uniquePatchValidation`, `distinctRootPatches`).
  DagCandidateAcceptanceCertificate AcceptOrMergeDAGCandidatePatch(
      const RefoldMacroDAGLiftingContext &ctx,
      DagCandidateAcceptanceContext &accCtx, MacroPatch candPatch,
      llvm::StringRef baseText, llvm::StringRef traceStage,
      const DagCandidateValidationMetadata *candValidation = nullptr) const;

  /// Compact `{key='sig0|sig1|...', ...}` summary used by trace output.
  /// Public because the lifting phase's own trace lines format this map
  /// alongside subtree validation output.
  std::string FormatBridgeSensitiveFormalSignatureMap(
      const llvm::StringMap<SemanticInteractionSignature> &sigs) const;

private:
  Dependencies deps_;
};

} // namespace refold
} // namespace clang

#endif // LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDMACRODAGCANDIDATEVALIDATOR_H
