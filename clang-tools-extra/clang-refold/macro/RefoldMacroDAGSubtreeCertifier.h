//===--- RefoldMacroDAGSubtreeCertifier.h ---------------------*- C++ -*-===//
//
// Subtree-rewrite certifier for the DAG lifting phase.
//
// Owns the subtree-level certificate types and builders that turn a
// single hop-level lift (produced by `RefoldMacroDAGStructuredLifter`)
// plus its supporting semantic evidence (from
// `RefoldMacroDAGInvertibilitySolver`) into a subtree-wide rewrite
// certificate that `RefoldMacroDAGCandidateValidator` ranks against
// the proof lattice.
//
//   * `BuildSubtreeInteractionSummaryCertificate` — fold per-formal
//     semantic-interaction certificates into a subtree-wide feature
//     summary.
//   * `BuildSubtreeInteractionConsistencyCertificate` — verify that all
//     formal-rewrite certificates agree on wrapper/paste semantics for
//     logically identical formals across multiple hop paths.
//   * `BuildSubtreeDeferredPasteDischargeCertificate` — confirm that
//     every deferred paste obligation produced by the subtree is
//     discharged by a later semantic or ancestor replay witness.
//   * `BuildSubtreeSemanticAdmissibilityCertificate` — gate the subtree
//     on deferred-paste discharge before letting it compete with
//     whole-cover realization.
//   * `BuildSubtreeSemanticCertificate` — compose the full subtree
//     semantic certificate from leaf rewrite, lift chain, root merge,
//     and root rewrite evidence.
//   * `BuildLeafFormalLiftGroups` — partition a leaf's per-formal
//     rewrites into independently-liftable groups so the chain builder
//     is run once per coherent group.
//   * `BuildSubtreeRewriteCertificate` — the main entry point: combine
//     the lift-group results into one subtree rewrite plan with full
//     semantic evidence attached.
//   * `BuildUniformObservedLeafSeedCertificate` — seed the leaf-side
//     rewrite when all observations of a leaf formal agree on the same
//     observed (old,new) pair (the "uniform observed" specialization
//     used when the formal has no structural template).
//
// The service has no back-reference to the lifting phase or planner;
// every needed primitive is reached through the
// `RefoldMacroDAGTextPrimitives` and `RefoldMacroDAGStructuredLifter`
// services held in its Dependencies, plus the per-call lifting context.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDMACRODAGSUBTREECERTIFIER_H
#define LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDMACRODAGSUBTREECERTIFIER_H

#include "core/RefoldModel.h"
#include "macro/RefoldMacroDAGInvertibilitySolver.h"
#include "macro/RefoldMacroDAGLiftingContext.h"
#include "macro/RefoldMacroDAGStructuredLifter.h"
#include "macro/RefoldMacroDAGTextPrimitives.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/ADT/StringSet.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

namespace clang {
namespace refold {

// ---- Subtree-certification certificate types
// ---------------------------------------------

struct OldNewText {
  std::string oldText;
  std::string newText;
};

enum class SubtreeRewriteCertificateKind {
  NoChange,
  Unique,
  Invalid,
};

/// Summarize the semantic interaction modes observed inside a macro subtree.
///
/// This certificate is deliberately coarse-grained: it does not prove a rewrite
/// by itself, but records which sensitive macro semantics appear below this
/// point, such as paste, stringify, raw invocation preservation, preferred
/// child syntax, and mixed interaction modes. Later selection and merge logic
/// can then make decisions using explicit interaction facts instead of
/// re-inspecting every child certificate.
struct SubtreeInteractionSummaryCertificate {
  bool valid = true;
  bool hasPaste = false;
  bool hasStringify = false;
  bool hasWideStringify = false;
  bool hasRawInvocation = false;
  bool hasPreferredChildSyntax = false;
  bool hasMixedInteractions = false;
  bool hasStringifyPaste = false;
  bool hasWideStringifyPaste = false;
  bool hasRawInvocationPaste = false;
  bool hasChildSyntaxPaste = false;
  llvm::SmallVector<SemanticInteractionCertificate, 16> interactions;
  std::string detail;
};

enum class SubtreeSemanticAdmissibilityFailure {
  None,
  MixedSemanticInteractions,
  LexicalBridgeWithStructuredSemantics,
  DeferredPasteNotDischarged,
  PasteWithPassthroughFlatten,
};

/// Decide whether the collected subtree semantics are admissible for a
/// structure-preserving macro rewrite.
///
/// This is the final semantic gate after the subtree has collected lift chains,
/// formal interaction summaries, deferred paste obligations, and root replay
/// information. It does not construct new rewrite evidence; it only checks that
/// the evidence already collected is strong enough to accept the subtree
/// without relying on an ambiguous or lossy macro interpretation.
///
/// The certificate fails closed for three important cases:
///
/// * deferred paste obligations that were never discharged,
/// * mixed semantic interactions that cannot be represented by one structural
///   proof class, and
/// * bridge-sensitive structured semantics that survived a lexical bridge.
struct SubtreeSemanticAdmissibilityCertificate {
  bool valid = true;
  SubtreeSemanticAdmissibilityFailure failure =
      SubtreeSemanticAdmissibilityFailure::None;
  std::string detail;
};

enum class DeferredPasteDischargeFailure {
  None,
  MissingSemanticDischarge,
  MissingAncestorPasteValidation,
};

/// Prove that every deferred paste-validation obligation in the subtree is
/// discharged by a later, semantically stronger witness.
///
/// Some invocation certificates intentionally defer paste validation when the
/// local hop cannot fully validate the paste shape in isolation. This
/// certificate checks that each deferred paste is eventually justified by at
/// least one accepted outer explanation:
///
/// * a semantic discharge from an ancestor formal certificate,
/// * a valid paste replay on an ancestor invocation, or
/// * the accepted root replay, including the lexical-bridge case where
///   bridge-derived root formals exactly match the root certificate.
///
/// If none of those witnesses exists, the deferred paste would become an
/// unproven assumption, so the subtree certificate fails closed.
struct DeferredPasteDischargeCertificate {
  bool valid = true;
  DeferredPasteDischargeFailure failure = DeferredPasteDischargeFailure::None;
  llvm::SmallVector<const InvocationRewriteCertificate *, 4>
      deferredInvocations;
  std::string detail;
};

/// Assemble the complete semantic proof bundle for one accepted macro subtree
/// rewrite.
///
/// This gathers every proof artifact produced while moving from the leaf
/// invocation to the root invocation: invocation certificates, lift-chain
/// steps, parent-constraint derivations, formal/argument/slot rewrite
/// certificates, root-formal merge certificates, raw-formal validations, paste
/// validations, and semantic interaction summaries.
///
/// After collection, the bundle is checked in three stages:
///
/// * all repeated formal-interaction evidence must be consistent,
/// * every deferred paste obligation must be discharged, and
/// * the combined subtree semantics must be admissible.
///
/// The result is fail-closed. If any stage rejects, the returned semantic
/// certificate is invalid and carries that stage's detail string.
struct SubtreeSemanticCertificate {
  bool valid = false;
  bool usesLexicalBridge = false;
  bool touchesPaste = false;
  bool hasWrapperSemantics = false;
  bool hasStringifySemantics = false;
  bool hasWideStringifySemantics = false;
  bool hasPreferredChildSyntax = false;
  bool hasRawInvocationPreservation = false;
  bool hasPassthroughFlatten = false;
  bool hasBridgeSensitiveStructuredSemantics = false;
  bool hasAcceptedRootPlaceholderReplay = false;
  bool rootReplayFlattensOnlyWholeChildArgs = false;
  const RefoldModel::MacroInvocation *acceptedRootReplayInv = nullptr;
  llvm::SmallVector<InvocationRewriteCertificate, 8> invocationCertificates;
  llvm::SmallVector<FormalRewriteCertificate, 16> formalCertificates;
  llvm::SmallVector<ArgSemanticRewriteCertificate, 16> argCertificates;
  llvm::SmallVector<SlotSemanticRewriteCertificate, 32> slotCertificates;
  llvm::SmallVector<SemanticInteractionCertificate, 32> interactionCertificates;
  llvm::SmallVector<FormalInteractionConsistencyCertificate, 16>
      formalInteractionConsistencies;
  llvm::SmallVector<RawFormalValidationCertificate, 16> rawFormalValidations;
  llvm::SmallVector<PasteRewriteValidationCertificate, 8> pasteValidations;
  llvm::SmallVector<ParentConstraintDerivationCertificate, 16>
      parentDerivations;
  llvm::SmallVector<StructuredLiftCertificate, 8> structuredLiftCertificates;
  llvm::SmallVector<LiftChainCertificate, 4> liftChains;
  llvm::SmallVector<RootFormalMergeCertificate, 4> rootMergeCertificates;
  StringSet<> bridgedFormalKeys;
  StringSet<> bridgedInteractionKeys;
  SubtreeInteractionSummaryCertificate interactionSummary;
  SubtreeInteractionConsistencyCertificate interactionConsistency;
  DeferredPasteDischargeCertificate deferredPasteDischarge;
  SubtreeSemanticAdmissibilityCertificate admissibility;
  std::string detail;
};

/// Build a complete rewrite certificate for one edited macro subtree.
///
/// The input `leafEdits` describes edits observed at a leaf invocation. The
/// proof proceeds in four stages:
///
/// * certify the edited leaf invocation,
/// * partition leaf formals into independently liftable groups,
/// * lift each group outward to root formals and merge root rewrites, and
/// * certify the final root invocation plus the collected subtree semantics.
///
/// The returned certificate is `Unique` only if every stage is proven.
/// Otherwise it fails closed with the detail from the first failed proof
/// obligation.
struct SubtreeRewriteCertificate {
  SubtreeRewriteCertificateKind kind = SubtreeRewriteCertificateKind::Invalid;
  const RefoldModel::MacroInvocation *leaf = nullptr;
  const RefoldModel::MacroInvocation *root = nullptr;
  llvm::DenseMap<uint32_t, FormalTextPair> leafFormals;
  llvm::DenseMap<uint32_t, FormalTextPair> rootFormals;
  llvm::SmallVector<uint32_t, 8> deferRootOccurrenceArgIdxs;
  InvocationRewriteCertificate leafCert;
  InvocationRewriteCertificate rootCert;
  llvm::SmallVector<LiftChainCertificate, 4> liftCertificates;
  llvm::SmallVector<RootFormalMergeCertificate, 4> rootMergeCertificates;
  SubtreeSemanticCertificate semantic;
  std::string detail;
};

enum class UniformObservedLeafSeedCertificateKind {
  NoChange,
  Unique,
  Invalid,
};

enum class UniformObservedLeafSeedFailure {
  None,
  EmptyConstraints,
  DivergentConstraints,
};

/// Certify a uniform observed leaf rewrite as the leaf-side seed.
///
/// Nested leaf invocations are recorded in macro-body space, so their
/// invocation text is often placeholder syntax such as `STR1(x)` or `CAT(a,b)`
/// rather than source-spelled actual arguments. When all observed leaf
/// constraints for one formal collapse to the same normalized old/new text,
/// this certificate records that exact uniform observed rewrite and lets the
/// structured lift/root-certificate pipeline continue. It does not accept a
/// root patch by itself.
struct UniformObservedLeafSeedCertificate {
  UniformObservedLeafSeedCertificateKind kind =
      UniformObservedLeafSeedCertificateKind::Invalid;
  UniformObservedLeafSeedFailure failure = UniformObservedLeafSeedFailure::None;
  const RefoldModel::MacroInvocation *inv = nullptr;
  uint32_t argIdx = 0;
  std::string oldText;
  std::string newText;
  std::string detail;
};

// ---- Service class ------------------------------------------------------

/// Subtree-rewrite certifier for DAG lifting.
class RefoldMacroDAGSubtreeCertifier {
public:
  /// Borrowed inputs needed by the subtree certifier.  All references
  /// must outlive the service; the lifting phase owns them.
  struct Dependencies {
    const RefoldMacroDAGTextPrimitives &textPrimitives;
    const RefoldMacroDAGStructuredLifter &structuredLifter;
  };

  explicit RefoldMacroDAGSubtreeCertifier(Dependencies deps);

  /// Fold per-formal semantic-interaction certificates into a subtree-
  /// wide feature summary.
  SubtreeInteractionSummaryCertificate
  BuildSubtreeInteractionSummaryCertificate(
      const RefoldMacroDAGLiftingContext &ctx,
      llvm::ArrayRef<SemanticInteractionCertificate> interactions) const;

  /// Verify that all formal-rewrite certificates agree on wrapper/
  /// paste semantics for logically identical formals across multiple
  /// hop paths.
  SubtreeInteractionConsistencyCertificate
  BuildSubtreeInteractionConsistencyCertificate(
      const RefoldMacroDAGLiftingContext &ctx,
      llvm::ArrayRef<FormalRewriteCertificate> formalCertificates) const;

  /// Confirm that every deferred paste obligation produced by the
  /// subtree is discharged by a later semantic or ancestor replay
  /// witness.
  DeferredPasteDischargeCertificate
  BuildSubtreeDeferredPasteDischargeCertificate(
      const RefoldMacroDAGLiftingContext &ctx,
      const SubtreeSemanticCertificate &semantic,
      const InvocationRewriteCertificate &rootCert) const;

  /// Gate the subtree on deferred-paste discharge before letting it
  /// compete with whole-cover realization.
  SubtreeSemanticAdmissibilityCertificate
  BuildSubtreeSemanticAdmissibilityCertificate(
      const RefoldMacroDAGLiftingContext &ctx,
      const SubtreeSemanticCertificate &semantic) const;

  /// Compose the full subtree semantic certificate from leaf rewrite,
  /// lift chain, root merge, and root rewrite evidence.
  SubtreeSemanticCertificate BuildSubtreeSemanticCertificate(
      const RefoldMacroDAGLiftingContext &ctx,
      const InvocationRewriteCertificate &leafCert,
      llvm::ArrayRef<LiftChainCertificate> liftCertificates,
      llvm::ArrayRef<RootFormalMergeCertificate> rootMergeCertificates,
      const InvocationRewriteCertificate &rootCert) const;

  /// Partition a leaf's per-formal rewrites into independently-liftable
  /// groups so the chain builder is run once per coherent group.
  llvm::SmallVector<llvm::DenseMap<uint32_t, FormalTextPair>, 4>
  BuildLeafFormalLiftGroups(
      const RefoldMacroDAGLiftingContext &ctx,
      const RefoldModel::MacroInvocation &leaf,
      const llvm::DenseMap<uint32_t, FormalTextPair> &leafFormals) const;

  /// Main entry: combine the lift-group results into one subtree
  /// rewrite plan with full semantic evidence attached.
  SubtreeRewriteCertificate BuildSubtreeRewriteCertificate(
      const RefoldMacroDAGLiftingContext &ctx,
      const RefoldModel::MacroInvocation &leaf,
      const llvm::DenseMap<uint32_t, OldNewText> &leafEdits,
      bool deferLeafPasteValidation) const;

  /// Seed the leaf-side rewrite when all observations of a leaf formal
  /// agree on the same observed (old,new) pair.
  UniformObservedLeafSeedCertificate BuildUniformObservedLeafSeedCertificate(
      const RefoldMacroDAGLiftingContext &ctx,
      const RefoldModel::MacroInvocation &inv, uint32_t argIdx,
      llvm::ArrayRef<ObservedFormalConstraint> constraints,
      llvm::StringRef traceStage) const;

private:
  Dependencies deps_;
};

} // namespace refold
} // namespace clang

#endif // LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDMACRODAGSUBTREECERTIFIER_H
