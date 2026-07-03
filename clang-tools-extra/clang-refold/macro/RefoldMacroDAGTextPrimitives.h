//===--- RefoldMacroDAGTextPrimitives.h ----------------------*- C++ -*-===//
//
// Foundation text/parsing primitives for the DAG lifting phase.
//
// Owns the small, deterministic text helpers that every higher DAG
// lifting service (invertibility solver, structured lifter, subtree
// certifier, candidate validator) consumes:
//
//   * Invocation-argument text recovery (`GetInvocationArgText`,
//     `GetTrimmedInvocationArgInfo`).
//   * Argument-ref template construction and inverse-template
//     preconditions (`BuildArgRefTemplate`).
//   * Invocation-cover A-side replacement text
//     (`GetInvocationCoverAText`).
//   * Balanced-fragment + token-boundary text predicates
//     (`IsBalancedRefoldFragment`,
//     `IsLikelyTokenBoundaryInRefoldText`).
//   * Top-level literal-match enumeration in refold text
//     (`EnumerateTopLevelLiteralMatchesInRefoldText`).
//   * Per-formal invocation rewriting
//     (`BuildRewrittenInvocationSyntax`).
//   * Equivalent expansion-text surfaces for wrapper matching
//     (`GetExpansionTextCandidates`).
//   * Top-level lexical child-invocation discovery inside one parent
//     argument (`GetTopLevelLexicalChildrenInArg`).
//
// Each method is a small, locally deterministic transformation; the
// service holds no state beyond its borrowed dependencies.
//
// The service has no back-reference to the lifting phase or planner.
// `getMacroInvocationFormalArgContentRanges` is supplied as a
// std::function callback (the planner-side primitive that recovers a
// function-like macro invocation's formal-argument byte ranges).
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDMACRODAGTEXTPRIMITIVES_H
#define LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDMACRODAGTEXTPRIMITIVES_H

#include "core/RefoldModel.h"

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
class RefoldProofLattice;
class RefoldSourceMapper;

/// Argument-ref placeholder rebased into a trimmed parent argument's
/// local byte coordinates.
struct LocalArgRef {
  uint32_t callerParamIndex;
  uint32_t begin;
  uint32_t end;
};

/// Argument-local template recovered from one child invocation argument.
/// `argText` is the trimmed argument surface; `refs` are the
/// caller-parameter placeholder ranges in source order;
/// `distinctCallerParams` lists the unique caller parameter ordinals
/// referenced by `refs`.
struct ArgRefTemplate {
  std::string argText;
  llvm::SmallVector<LocalArgRef, 4> refs;
  llvm::SmallVector<uint32_t, 2> distinctCallerParams;
};

/// Trimmed spelling and absolute byte extent of one invocation
/// argument, derived from the invocation's locally parsed macro-argument
/// slots (not from raw producer source ranges).
struct TrimmedArgInfo {
  std::string text;
  uint64_t absTrimBegin = 0;
  uint64_t absTrimEnd = 0;
};

/// Kind of wrapper-chain match used to align a child invocation against
/// observed parent-argument text.
enum class WrapperChainKind {
  Exact,
  StringLiteral,
  WideStringLiteral,
};

/// Whether the wrapper-chain observation was matched against the child
/// invocation's expansion text or against its raw invocation spelling.
enum class WrapperObservedSource {
  ChildExpansion,
  ChildRawInvocation,
};

/// One wrapper-chain certificate: the observed parent-argument substring
/// together with the logical child input it represents.
struct WrapperChainCertificate {
  WrapperChainKind kind = WrapperChainKind::Exact;
  WrapperObservedSource source = WrapperObservedSource::ChildExpansion;
  std::string observedOldText;
  std::string logicalInputText;
};

/// One lexical-child invocation placeholder discovered inside a trimmed
/// parent argument.  `relBegin`/`relEnd` are byte offsets into that
/// trimmed parent argument; `observedForms` records the equivalent
/// surface texts (expansion, stringification, wide-stringification) used
/// by wrapper-chain reconstruction.
struct LexicalChildPlaceholder {
  uint64_t relBegin = 0;
  uint64_t relEnd = 0;
  const RefoldModel::MacroInvocation *child = nullptr;
  llvm::SmallVector<WrapperChainCertificate, 4> observedForms;
  llvm::SmallVector<std::string, 4> newExpansionCandidates;
  std::string rawInvocationText;
};

/// Foundation text/parsing primitives for DAG lifting.
class RefoldMacroDAGTextPrimitives {
public:
  /// Borrowed inputs needed by the text-primitives service.  All
  /// references must outlive the service; the lifting phase owns them.
  struct Dependencies {
    const RefoldModel &model;
    const RefoldSourceMapper &sourceMapper;
    const clang::LangOptions &lexLang;
    const RefoldArgTextRecovery &argTextRecovery;
    RefoldProofLattice &proofLattice;

    /// Delegates to
    /// `RefoldMacroPatchPlanner::GetMacroInvocationFormalArgContentRanges`.
    std::function<std::optional<std::vector<std::pair<size_t, size_t>>>(
        const RefoldModel::MacroInvocation &, llvm::StringRef)>
        getMacroInvocationFormalArgContentRanges;
  };

  explicit RefoldMacroDAGTextPrimitives(Dependencies deps);

  /// Return the trimmed raw invocation argument text for `argIdx` using
  /// the invocation spelling's formal slots.  Producer source ranges can
  /// describe a larger C syntactic surface (for example `arr[1, 2]`)
  /// even when the preprocessor splits that text across multiple macro
  /// formals, so wrapper reconstruction must use the locally parsed
  /// macro-argument slots here.
  std::optional<llvm::StringRef>
  GetInvocationArgText(const RefoldModel::MacroInvocation &inv,
                       uint32_t argIdx) const;

  /// Build an argument-local template for one child invocation argument
  /// by replacing producer arg-ref byte ranges with caller-parameter
  /// placeholders.  Returns nullopt for malformed metadata or
  /// non-template invocations.
  std::optional<ArgRefTemplate>
  BuildArgRefTemplate(const RefoldModel::MacroInvocation &inv,
                      uint32_t argIdx) const;

  /// Return the trimmed spelling and absolute byte extent of one
  /// invocation argument.  The extent is derived from the invocation's
  /// locally parsed macro-argument slots so commas inside braces or
  /// brackets stay with the formal the preprocessor actually used.
  std::optional<TrimmedArgInfo>
  GetTrimmedInvocationArgInfo(const RefoldModel::MacroInvocation &inv,
                              uint32_t argIdx) const;

  /// Return the comparable A-side expansion text for an invocation
  /// cover.  When a parameterless function-like wrapper has a
  /// producer-recorded body span narrower than its full cover, use that
  /// body payload instead.
  std::optional<std::string>
  GetInvocationCoverAText(const RefoldModel::MacroInvocation &inv) const;

  /// Return true when `pos` is a plausible token boundary inside refold
  /// text.  Rejects cuts through the middle of an identifier-like token.
  bool IsLikelyTokenBoundaryInRefoldText(llvm::StringRef s, size_t pos) const;

  /// Return true when the entire fragment is balanced at top level
  /// according to the lexer-backed cut-point enumerator.
  bool IsBalancedRefoldFragment(llvm::StringRef s) const;

  /// Enumerate top-level, balanced occurrences of `needle` in
  /// `haystack` up to `maxPos`, only emitting matches that begin at a
  /// plausible token boundary.  `emitMatch` receives one `size_t` per
  /// match; returning from `emitMatch` is the only way to stop short.
  void EnumerateTopLevelLiteralMatchesInRefoldText(
      llvm::StringRef haystack, llvm::StringRef needle, size_t maxPos,
      const std::function<void(size_t)> &emitMatch) const;

  /// Rebuild an invocation spelling by replacing selected formal-
  /// argument slots with proven replacement text.  Edits are validated
  /// in invocation-local coordinates first, then applied right-to-left
  /// so original byte ranges remain stable.
  std::optional<std::string> BuildRewrittenInvocationSyntax(
      const RefoldModel::MacroInvocation &inv,
      const llvm::DenseMap<uint32_t, std::string> &replByFormal) const;

  /// Return normalized expansion spellings that can legitimately
  /// represent this invocation when matching wrapper observations.  Each
  /// returned string is trimmed and de-duplicated.  When `fromB` is
  /// true, the base spelling is the lattice's whole-cover B-side
  /// replacement text; otherwise it is the A-side cover text.
  llvm::SmallVector<std::string, 4>
  GetExpansionTextCandidates(const RefoldModel::MacroInvocation &inv,
                             bool fromB) const;

  /// Find direct lexical child macro invocations spelled inside one
  /// trimmed parent argument.  Each returned placeholder records the
  /// child's byte range relative to the trimmed parent argument plus the
  /// observed wrapper forms that can represent the child text during
  /// wrapper-chain reconstruction.
  llvm::SmallVector<LexicalChildPlaceholder, 4>
  GetTopLevelLexicalChildrenInArg(const RefoldModel::MacroInvocation &parent,
                                  uint32_t parentFormal) const;

private:
  Dependencies deps_;
};

} // namespace refold
} // namespace clang

#endif // LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDMACRODAGTEXTPRIMITIVES_H
