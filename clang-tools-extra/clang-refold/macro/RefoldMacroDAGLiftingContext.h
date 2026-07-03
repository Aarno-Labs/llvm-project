//===--- RefoldMacroDAGLiftingContext.h ----------------------*- C++ -*-===//
//
// Per-call carrier for the DAG lifting phase (mirrors the
// `RefoldMacroWholeCoverPlanningContext` pattern).
//
// The DAG-phase sub-services (`RefoldMacroDAGTextPrimitives`,
// `RefoldMacroDAGInvertibilitySolver`,
// `RefoldMacroDAGStructuredLifter`,
// `RefoldMacroDAGSubtreeCertifier`,
// `RefoldMacroDAGCandidateValidator`) each take a const reference to
// this carrier instead of re-expanding the shared per-call state
// (root invocation `m`, invocation-arg ranges, subtree validation
// context, etc.) as a fresh parameter list.
//
// `RefoldMacroDAGLiftingPhase::Run` populates the carrier once at the
// top of the method.  Every field is a borrowed view (StringRef /
// ArrayRef) into caller-owned storage; nothing here owns by value.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDMACRODAGLIFTINGCONTEXT_H
#define LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDMACRODAGLIFTINGCONTEXT_H

#include "core/RefoldModel.h"
#include "macro/RefoldMacroDAGLeafDiscoveryPhase.h"
#include "macro/RefoldMacroOccurrenceProofValidator.h"
#include "source/DiffAlgorithms.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"

#include <cstddef>
#include <cstdint>
#include <utility>

namespace clang {
namespace refold {

/// Per-call carrier for the DAG lifting phase and its sub-services.  All
/// fields are borrowed from caller-owned storage in
/// `RefoldMacroDAGLiftingPhase::Run` (the planning context's locals plus
/// the discovery-phase output); the carrier owns none of them.
struct RefoldMacroDAGLiftingContext {
  /// Root macro invocation being patched.
  const RefoldModel::MacroInvocation &m;
  /// Original A/B token-level hunk for this edit.
  const diffutils::Hunk &h;
  /// `h` after common-edge token trimming.
  const diffutils::Hunk &hEff;
  /// Source spelling of the invocation callsite.
  llvm::StringRef baseInvText;
  /// Argument-like spans for the root invocation (normal arg spans,
  /// stringify spans, and paste spans concatenated).
  llvm::ArrayRef<RefoldModel::PPArgSpan> argLikeSpans;
  /// Whether the args-only phase observed direct argument-like surface
  /// at the root.
  bool rootHasDirectArgLikeSurface = false;
  /// Half-open invocation byte span in the owning file.
  uint64_t invStart = 0;
  uint64_t invEnd = 0;

  /// Discovery-phase outputs republished under stable names.
  llvm::StringRef invSpanText;
  llvm::ArrayRef<std::pair<size_t, size_t>> invArgRanges;
  const llvm::DenseMap<uint64_t, const RefoldModel::MacroInvocation *> &invById;
  const MacroSubtreeReplayValidationContext &subtreeValidationCtx;
  const RefoldModel::MacroInvocation &rootInvocation;
  llvm::StringRef rootInvocationText;
  llvm::ArrayRef<std::pair<size_t, size_t>> rootInvocationArgRanges;

  /// Candidate leaves and split-insertion root candidates produced by the
  /// DAG leaf discovery phase.
  llvm::ArrayRef<DAGLeafCandidate> leafCands;
  llvm::ArrayRef<DAGSplitInsertionRootCandidate> splitInsertionRootCandidates;

  /// Cached per-formal root argument text recovered by the discovery
  /// phase.
  const llvm::DenseMap<uint32_t, llvm::StringRef> &rootArgText;

  /// Canonical single-hunk array built from `h` at the top of the lifting
  /// run.  Raw-formal validation and the later certificates that forward into
  /// it consume this exactly once per lifting call; bundling it into the
  /// carrier avoids threading it through every method signature.
  llvm::ArrayRef<diffutils::Hunk> tokenHunksAR;
};

} // namespace refold
} // namespace clang

#endif // LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDMACRODAGLIFTINGCONTEXT_H
