//===--- RefoldMacroGeneratedLeafReplayEngine.h --------------*- C++ -*-===//
//
// Generated-leaf fallback replay for clang-refold.
//
// When the higher-order generated-callee chain replay does not produce a
// candidate, this service attempts a leaf-fallback proof that compares the
// already-recovered whole-cover expansion surface (old vs new) directly,
// inverting restricted stringification and pure spelling rewrites back to
// the invocation arguments.
//
// The engine takes an explicit `Dependencies` struct and holds no
// back-reference to the planner, mirroring the pattern used by
// `RefoldMacroPatchProofCertifier`,
// `RefoldMacroSubtreeReplayValidator`,
// `RefoldMacroReplayStabilityValidator`, and
// `RefoldMacroGeneratedCalleeReplayEngine`.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDMACROGENERATEDLEAFREPLAYENGINE_H
#define LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDMACROGENERATEDLEAFREPLAYENGINE_H

#include "core/RefoldModel.h"
#include "edit/RefoldPatchTypes.h"
#include "macro/RefoldMacroPlannerHelpers.h"
#include "source/DiffAlgorithms.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/StringRef.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <utility>

namespace clang {

class LangOptions;

namespace refold {

class RefoldMacroPatchProofCertifier;
class RefoldSourceMapper;

/// Explicit context for generated-leaf replay.
///
/// The engine borrows the root invocation surface, generated-leaf identity,
/// formal index, and same-pass macro patch context required to build the leaf
/// replay candidate.
struct GeneratedLeafReplayContext {
  /// Root invocation whose generated leaf is being replayed.
  const RefoldModel::MacroInvocation &invocation;
  /// Complete source spelling of the root invocation.
  llvm::StringRef baseInvocationText;
  /// Formal-content byte ranges inside `baseInvocationText`.
  llvm::ArrayRef<std::pair<size_t, size_t>> invocationArgRanges;
  /// Mutable B-token envelope refined by generated-leaf replay.
  std::pair<size_t, size_t> &bTokenEnvelope;
  /// Macro definition that owns the root replacement-list proof.
  const RefoldModel::MacroDirective &rootDefinition;
  /// Old recovered expansion spelling at the generated leaf.
  llvm::StringRef oldExpansion;
  /// New recovered expansion spelling at the generated leaf.
  llvm::StringRef newExpansion;
};

/// Builds and validates generated-leaf replay candidates.
///
/// Generated-leaf replay handles nested generated macro surfaces that can be
/// realized without lifting the entire root subtree, while still preserving
/// the required macro proof envelope.
class RefoldMacroGeneratedLeafReplayEngine {
public:
  /// Borrowed inputs needed by the generated-leaf replay engine.
  ///
  /// All references must outlive the engine; the planner owns all of them.  The
  /// callbacks delegate to shared planner-side helpers while `std::function`
  /// keeps the engine free of a back-reference to the planner.
  struct Dependencies {
    const RefoldModel &model;
    const RefoldSourceMapper &sourceMapper;
    const clang::LangOptions &lexLang;
    RefoldMacroPatchProofCertifier &proofCertifier;

    /// Resolve a function-like macro name through replay-safe alias hops.
    /// Delegates to
    /// `RefoldMacroPatchPlanner::ResolveFunctionLikeMacroForReplay`.
    std::function<const RefoldModel::MacroDirective *(llvm::StringRef)>
        resolveFunctionLikeMacroForReplay;

    /// Return whether the named object-like macro expands to a single token
    /// that can be used as a callee alias during replay proof.  Delegates to
    /// `RefoldMacroPatchPlanner::IsObjectLikeSingleTokenAlias`.
    std::function<bool(llvm::StringRef)> isObjectLikeSingleTokenAlias;

    /// Rebuild the invocation by replacing formal-content ranges right-to-
    /// left in source order while carrying the materialized output range.
    /// Delegates to `RefoldMacroPatchPlanner::BuildInvocationRewriteWithRange`.
    std::function<std::optional<InvocationRewriteWithRange>(
        const InvocationActualRecoveryContext &,
        const llvm::DenseMap<uint32_t, std::string> &,
        const llvm::DenseMap<uint32_t, std::pair<uint64_t, uint64_t>> *)>
        buildInvocationRewriteWithRange;
  };

  explicit RefoldMacroGeneratedLeafReplayEngine(Dependencies deps)
      : deps_(std::move(deps)) {}

  /// Attempt to build the generated-leaf fallback candidate from the already
  /// recovered whole-cover expansion surface. A miss is non-terminal.
  std::optional<MacroPatch> BuildGeneratedLeafReplayCandidate(
      const GeneratedLeafReplayContext &ctx) const;

private:
  Dependencies deps_;
};

} // namespace refold
} // namespace clang

#endif // LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDMACROGENERATEDLEAFREPLAYENGINE_H
