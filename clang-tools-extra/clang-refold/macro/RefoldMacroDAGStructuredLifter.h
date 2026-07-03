//===--- RefoldMacroDAGStructuredLifter.h ---------------------*- C++ -*-===//
//
// Structured "lift this hop" engine for the DAG lifting phase.
//
// Owns the certificate types and builders that turn
// formal-rewrite/semantic-interaction evidence (produced by the
// invertibility solver) into a concrete root-targeting `MacroPatch`
// candidate.  Each hop is one invocation-rewrite certificate; the
// chain-builder threads hops together until the root is reached, after
// which the root-formal merger collapses sibling candidates into one
// rewrite plan.
//
// Service methods take a borrowed `RefoldMacroDAGLiftingContext` for
// the shared lifting state (root invocation `m`, invocation-arg
// ranges, subtree validation context, etc.).  Methods that consult
// occurrence consistency also take the canonical `tokenHunksAR` array
// as an explicit parameter, mirroring the invertibility-solver
// convention.
//
// The service has no back-reference to the lifting phase or planner.
// `getMacroInvocationFormalArgContentRanges` is supplied as a
// std::function callback; every other planner-side primitive is
// reached through the `RefoldMacroDAGTextPrimitives` and
// `RefoldMacroDAGInvertibilitySolver` services held in its
// Dependencies.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDMACRODAGSTRUCTUREDLIFTER_H
#define LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDMACRODAGSTRUCTUREDLIFTER_H

#include "core/RefoldModel.h"
#include "macro/RefoldMacroDAGInvertibilitySolver.h"
#include "macro/RefoldMacroDAGLiftingContext.h"
#include "macro/RefoldMacroDAGTextPrimitives.h"
#include "macro/RefoldMacroPasteArgumentBuilder.h"
#include "source/DiffAlgorithms.h"
#include "source/RefoldToken.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
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
class RefoldMacroTopology;
class RefoldSourceMapper;

// ---- Structured-lift certificate types
// ---------------------------------------------

enum class InvocationRewriteCertificateKind {
  NoChange,
  Unique,
  Invalid,
};

enum class InvocationRewriteFailure {
  None,
  ArityChange,
  OccurrenceMismatch,
  MissingInvocationText,
  MissingArgumentRanges,
  PasteMismatch,
};

struct CertifiedFormalRewrite {
  uint32_t argIdx = 0;
  std::string oldText;
  std::string newText;
};

struct InvocationRewriteCertificate {
  InvocationRewriteCertificateKind kind =
      InvocationRewriteCertificateKind::Invalid;
  InvocationRewriteFailure failure = InvocationRewriteFailure::None;
  const RefoldModel::MacroInvocation *inv = nullptr;
  llvm::SmallVector<CertifiedFormalRewrite, 4> rewrites;
  llvm::DenseMap<uint32_t, std::string> replacementByArgIdx;
  llvm::SmallVector<RawFormalValidationCertificate, 4> formalValidations;
  PasteRewriteValidationCertificate pasteValidation;
  bool touchesPaste = false;
  std::string rewrittenInvocationSyntax;
  std::string detail;
};

enum class ParentConstraintDerivationFailure {
  None,
  MissingArgDeps,
  EmptyArgDeps,
  MissingArgRefs,
  TemplateNotCertifiable,
  InversionNotUnique,
  IncompleteDerivation,
};

struct ParentConstraintDerivationCertificate {
  bool valid = false;
  uint32_t childFormal = 0;
  ParentConstraintDerivationFailure failure =
      ParentConstraintDerivationFailure::None;
  llvm::SmallVector<std::pair<uint32_t, ObservedFormalConstraint>, 4>
      derivedConstraints;
  std::string detail;
};

enum class StructuredLiftCertificateKind {
  Unique,
  NeedsLexicalBridge,
  Invalid,
};

enum class StructuredLiftFailureReason {
  None,
  CurrentInvocationInvalid,
  MissingCallerInvocation,
  RootLexicalBridgeRequired,
  ParentConstraintDerivationFailed,
  ParentFormalInvalid,
  ParentInvocationInvalid,
};

/// Build one proof step that lifts a certified rewrite from `cur` to its
/// caller in the macro-expansion DAG.
///
/// The input `curFormals` describes the rewrite that has already been proven
/// at the current invocation boundary.  The lift tries to invert that rewrite
/// through `cur`'s formal dependencies, derive the equivalent constraints on
/// the parent invocation's formals, and then prove that the parent invocation
/// can be rebuilt with those rewritten formals while preserving the parent's
/// placeholder structure.
///
/// The result is intentionally fail-closed:
///
/// * `Unique` means the hop produced one certified parent rewrite.
/// * `NeedsLexicalBridge` means the structured DAG lift could not be proven at
///   this boundary, so the caller must fall back to a lexical bridge at
///   `nextInv`.
/// * `Invalid`/failure fields record the exact proof obligation that failed.
struct StructuredLiftCertificate {
  StructuredLiftCertificateKind kind = StructuredLiftCertificateKind::Invalid;
  StructuredLiftFailureReason failureReason = StructuredLiftFailureReason::None;
  ParentConstraintDerivationFailure derivationFailure =
      ParentConstraintDerivationFailure::None;
  FormalRewriteFailure parentFormalFailure = FormalRewriteFailure::None;
  InvocationRewriteFailure currentInvocationFailure =
      InvocationRewriteFailure::None;
  InvocationRewriteFailure parentInvocationFailure =
      InvocationRewriteFailure::None;
  const RefoldModel::MacroInvocation *nextInv = nullptr;
  llvm::DenseMap<uint32_t, FormalTextPair> nextFormals;
  llvm::DenseMap<uint32_t, llvm::SmallVector<uint32_t, 2>> parentFormalSources;
  llvm::DenseSet<uint32_t> bridgedNextFormals;
  InvocationRewriteCertificate currentCert;
  llvm::SmallVector<ParentConstraintDerivationCertificate, 4> derivations;
  llvm::SmallVector<FormalRewriteCertificate, 4> parentFormalCertificates;
  InvocationRewriteCertificate parentCert;
  std::string rewrittenChildSyntax;
  std::string detail;
};

enum class LiftChainCertificateKind {
  Unique,
  Invalid,
};

/// Lift a proven leaf-formal rewrite outward through the macro DAG until it
/// reaches the root invocation.
///
/// The leaf rewrite starts as a set of changed formal arguments on `leaf`. Each
/// step either proves a structured parent-formal rewrite directly or records a
/// lexical child bridge where already-certified rewritten child syntax was
/// substituted into the parent argument text.
///
/// The certificate records every hop, whether any lexical bridge was required,
/// and which final root formals came from bridge-derived text. Failure is
/// fail-closed: if any hop cannot be proven or bridged, the returned
/// certificate remains non-unique and carries the failure detail.
struct LiftChainCertificate {
  LiftChainCertificateKind kind = LiftChainCertificateKind::Invalid;
  const RefoldModel::MacroInvocation *leaf = nullptr;
  llvm::SmallVector<uint32_t, 4> leafArgIdxs;
  llvm::DenseMap<uint32_t, FormalTextPair> leafFormals;
  llvm::SmallVector<StructuredLiftCertificate, 4> steps;
  bool usedLexicalBridge = false;
  llvm::DenseSet<uint32_t> bridgedRootArgIdxs;
  llvm::DenseMap<uint32_t, FormalTextPair> rootFormals;
  std::string detail;
};

enum class RootFormalMergeCertificateKind {
  NoChange,
  Unique,
  Invalid,
};

enum class RootFormalMergeFailure {
  None,
  ArgIndexOutOfBounds,
  InvalidArgRange,
  MergeConflict,
};

/// Merge all independently lifted rewrites for one root formal.
///
/// Multiple leaf lift chains can arrive at the same root argument. This
/// certificate verifies that the target root argument exists, recovers its
/// original spelled text from the root invocation span, and then accepts the
/// merge only if all proposed rewrites are mutually compatible with that
/// original argument text.
struct RootFormalMergeCertificate {
  RootFormalMergeCertificateKind kind = RootFormalMergeCertificateKind::Invalid;
  RootFormalMergeFailure failure = RootFormalMergeFailure::None;
  uint32_t argIdx = 0;
  llvm::SmallVector<FormalTextPair, 2> observedRewrites;
  std::string baseArgText;
  std::string mergedArgText;
  std::string detail;
};

// ---- Service class ------------------------------------------------------

/// Structured hop-lifting engine for DAG lifting.
class RefoldMacroDAGStructuredLifter {
public:
  /// Borrowed inputs needed by the structured lifter.  All references
  /// must outlive the service; the lifting phase owns them.
  struct Dependencies {
    const RefoldModel &model;
    const RefoldMacroDAGTextPrimitives &textPrimitives;
    const RefoldMacroDAGInvertibilitySolver &invertibilitySolver;
    const RefoldSourceMapper &sourceMapper;
    llvm::ArrayRef<PPTok> aToks;
    llvm::ArrayRef<PPTok> bToks;
    const clang::LangOptions &lexLang;
    const RefoldArgTextRecovery &argTextRecovery;
    const RefoldMacroTopology &macroTopology;

    /// Delegates to
    /// `RefoldMacroPatchPlanner::GetMacroInvocationFormalArgContentRanges`.
    std::function<std::optional<std::vector<std::pair<size_t, size_t>>>(
        const RefoldModel::MacroInvocation &, llvm::StringRef)>
        getMacroInvocationFormalArgContentRanges;
  };

  explicit RefoldMacroDAGStructuredLifter(Dependencies deps);

  /// Single-invocation rewrite certificate (formal validation + paste
  /// validation + arity).
  InvocationRewriteCertificate BuildInvocationRewriteCertificate(
      const RefoldMacroDAGLiftingContext &ctx,
      const RefoldModel::MacroInvocation &inv,
      const llvm::DenseMap<uint32_t, FormalTextPair> &formals,
      llvm::StringRef traceStage,
      std::optional<llvm::StringRef> callsiteTextOverride = std::nullopt,
      llvm::ArrayRef<std::pair<size_t, size_t>> callsiteArgRangesOverride =
          llvm::ArrayRef<std::pair<size_t, size_t>>(),
      llvm::ArrayRef<uint32_t> deferOccurrenceArgIdxs =
          llvm::ArrayRef<uint32_t>()) const;

  /// Wrapper-placeholder hop variant — allows a wrapper-passthrough
  /// formal to remain unchanged.
  InvocationRewriteCertificate BuildWrapperPlaceholderHopInvocationCertificate(
      const RefoldMacroDAGLiftingContext &ctx,
      const RefoldModel::MacroInvocation &inv,
      const llvm::DenseMap<uint32_t, FormalTextPair> &formals,
      llvm::StringRef traceStage,
      std::optional<llvm::StringRef> callsiteTextOverride = std::nullopt,
      llvm::ArrayRef<std::pair<size_t, size_t>> callsiteArgRangesOverride =
          llvm::ArrayRef<std::pair<size_t, size_t>>(),
      llvm::ArrayRef<uint32_t> deferOccurrenceArgIdxs =
          llvm::ArrayRef<uint32_t>()) const;

  /// Recover an observed root-formal text-pair map from a candidate
  /// root-side callsite replacement.
  std::optional<llvm::DenseMap<uint32_t, FormalTextPair>>
  BuildRootFormalRewriteMapFromCallsiteReplacement(
      const RefoldMacroDAGLiftingContext &ctx, llvm::StringRef baseText,
      llvm::StringRef newText) const;

  /// Rebase a producer-recorded paste-arg group into the observed
  /// concatenated surface for matching.
  std::optional<llvm::SmallVector<std::pair<uint64_t, uint64_t>, 4>>
  TryRebasePasteGroupToObservedSurface(
      const RefoldMacroDAGLiftingContext &ctx,
      const RefoldModel::MacroInvocation *surfaceOwner,
      llvm::ArrayRef<const RefoldModel::PPArgSpan *> group,
      llvm::StringRef observedSurface, llvm::StringRef traceStage) const;

  /// Parent-constraint derivation through a nested paste chain.
  std::optional<ParentConstraintDerivationCertificate>
  TryBuildNestedPasteChainDerivation(const RefoldMacroDAGLiftingContext &ctx,
                                     const RefoldModel::MacroInvocation &cur,
                                     uint32_t curFormal, llvm::StringRef curOld,
                                     llvm::StringRef curNew,
                                     llvm::StringRef traceStage) const;

  /// Parent-constraint derivation through a two-parent delimited
  /// paste shape.
  std::optional<ParentConstraintDerivationCertificate>
  TryBuildTwoParentDelimitedDerivation(const RefoldMacroDAGLiftingContext &ctx,
                                       const RefoldModel::MacroInvocation &cur,
                                       uint32_t curFormal,
                                       llvm::StringRef curOld,
                                       llvm::StringRef curNew,
                                       llvm::StringRef traceStage) const;

  /// Generic parent-constraint derivation: arg-ref inversion first,
  /// then paste/delimiter specializations, then lexical-bridge fallback.
  ParentConstraintDerivationCertificate
  BuildParentConstraintDerivationCertificate(
      const RefoldMacroDAGLiftingContext &ctx,
      const RefoldModel::MacroInvocation &cur, uint32_t curFormal,
      llvm::StringRef curOld, llvm::StringRef curNew,
      llvm::StringRef traceStage) const;

  /// Bridge a certified child invocation rewrite back into the parent
  /// argument that lexically contains the child.
  std::optional<llvm::DenseMap<uint32_t, FormalTextPair>>
  TryLexicalChildBridge(const RefoldMacroDAGLiftingContext &ctx,
                        const RefoldModel::MacroInvocation &parent,
                        const RefoldModel::MacroInvocation &child,
                        const std::string &rewrittenChildSyntax) const;

  /// Try the exact-sibling reroot specialization: a single structured
  /// lift in one step.
  std::optional<StructuredLiftCertificate>
  TryBuildExactSiblingRerootLift(const RefoldMacroDAGLiftingContext &ctx,
                                 const RefoldModel::MacroInvocation &parent,
                                 const RefoldModel::MacroInvocation &cur,
                                 uint32_t curFormal, llvm::StringRef curOld,
                                 llvm::StringRef curNew) const;

  /// Build the structured-lift certificate that turns a single hop
  /// `cur[curFormal]: old -> new` into the corresponding parent-side
  /// invocation rewrite.
  StructuredLiftCertificate BuildStructuredLiftCertificate(
      const RefoldMacroDAGLiftingContext &ctx,
      const RefoldModel::MacroInvocation &cur,
      const llvm::DenseMap<uint32_t, FormalTextPair> &curFormals) const;

  /// Drive multi-hop lifting from a leaf back to the root, producing
  /// per-root-formal text-pair records.
  LiftChainCertificate BuildLiftChainCertificate(
      const RefoldMacroDAGLiftingContext &ctx,
      const RefoldModel::MacroInvocation &leaf,
      const llvm::DenseMap<uint32_t, FormalTextPair> &leafFormals) const;

  /// Collapse multiple sibling observations of the same root formal
  /// into one rewrite, failing closed on conflicts.
  RootFormalMergeCertificate
  BuildRootFormalMergeCertificate(const RefoldMacroDAGLiftingContext &ctx,
                                  uint32_t argIdx,
                                  llvm::ArrayRef<FormalTextPair> rewrites,
                                  llvm::StringRef traceStage) const;

private:
  /// Construct the on-demand paste-argument builder from this service's
  /// own dependencies.
  RefoldMacroPasteArgumentBuilder pasteArgumentBuilder() const;

  Dependencies deps_;
};

} // namespace refold
} // namespace clang

#endif // LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDMACRODAGSTRUCTUREDLIFTER_H
