//===--- RefoldMacroArgsOnlyTemplateSolver.h ------------------*- C++ -*-===//
//
// Recursive current-level template-surface recovery and template-solved
// args-only patch construction for clang-refold.
//
// This service owns the central macro replay path used when an edit can be
// expressed by rewriting only the macro invocation's argument text.  It
// reconstructs the macro expansion as fixed body tokens plus formal-occurrence
// slots, then runs a bounded DFS that proves a B-side token interval for each
// formal whose syntactic spelling can be inverted to a source edit.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDMACROARGSONLYTEMPLATESOLVER_H
#define LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDMACROARGSONLYTEMPLATESOLVER_H

#include "core/RefoldModel.h"
#include "edit/RefoldPatchTypes.h"
#include "source/DiffAlgorithms.h"
#include "source/RefoldToken.h"

#include "clang/Basic/LangOptions.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace clang {
namespace refold {

class RefoldMacroTopology;
class RefoldProofLattice;
class RefoldSourceMapper;

/// Template element over a macro whole-cover: either fixed body tokens or one
/// standard formal occurrence whose B-side interval must be solved.
struct ArgsOnlyTemplateElem {
  bool isArg = false;
  uint64_t aBegin = 0;
  uint64_t aEnd = 0;
  uint32_t argIdx = 0;
  size_t occurrenceOrdinal = 0;
};

/// Current-level replay surface for one macro invocation.
///
/// `standardSpans` are rebased to parsed formal slots.  `[coverBegin,
/// coverEnd)` is the exact A-token tape covered by those spans and fixed body
/// spans.  Consumed by template-replay helpers without changing the
/// exact-cover proof policy.
struct CurrentLevelTemplateSurface {
  std::vector<RefoldModel::PPArgSpan> standardSpans;
  uint64_t coverBegin = 0;
  uint64_t coverEnd = 0;
};

/// Explicit state bundle for args-only template replay.
///
/// Template replay needs the current invocation, its source spelling, and the
/// parsed formal ranges when recovering actual text and proving split formal
/// occurrences.  It is intentionally read-only and view-based: candidate
/// vectors, DFS state, and proof results remain caller-local.
struct ArgsOnlyTemplateReplayContext {
  const RefoldModel::MacroInvocation &invocation;
  llvm::StringRef baseInvocationText;
  llvm::ArrayRef<std::pair<size_t, size_t>> invocationArgRanges;
};

/// Solves args-only macro replay candidates from recovered invocation layouts.
///
/// The solver validates that argument substitutions preserve the macro
/// envelope, constructs the args-only replacement, and returns a candidate only
/// when the replay proof can be certified without widening the macro domain.
class RefoldMacroArgsOnlyTemplateSolver {
public:
  struct Dependencies {
    const RefoldModel *model = nullptr;
    llvm::ArrayRef<PPTok> aToks;
    llvm::ArrayRef<PPTok> bToks;
    llvm::ArrayRef<size_t> bTokOff;
    const RefoldSourceMapper *sourceMapper = nullptr;
    const RefoldMacroTopology *macroTopology = nullptr;
    RefoldProofLattice *proofLattice = nullptr;
    const clang::LangOptions *lexLang = nullptr;
    bool strict = false;
  };

  explicit RefoldMacroArgsOnlyTemplateSolver(Dependencies deps)
      : deps_(std::move(deps)) {}

  /// Build the exact current-level body/formal replay surface for an
  /// invocation and a parsed formal-range vector.
  std::optional<CurrentLevelTemplateSurface>
  GetCurrentLevelTemplateSurfaceForInvocation(
      const RefoldModel::MacroInvocation &invocation,
      llvm::ArrayRef<std::pair<size_t, size_t>> formalRanges) const;

  /// Return the standard current-level argument spans used by args-only
  /// template replay.
  std::optional<std::vector<RefoldModel::PPArgSpan>>
  GetCurrentLevelStandardArgSpans(
      const ArgsOnlyTemplateReplayContext &ctx) const;

  /// Convenience wrapper used when only rebased standard spans are needed.
  std::optional<std::vector<RefoldModel::PPArgSpan>>
  GetCurrentLevelStandardArgSpansForInvocation(
      const RefoldModel::MacroInvocation &invocation,
      llvm::ArrayRef<std::pair<size_t, size_t>> formalRanges) const;

  /// Return the old/new expansion text for one invocation's current-level
  /// template surface, not the broader producer whole-cover.
  std::optional<std::pair<std::string, std::string>>
  GetCurrentLevelExpansionTextForInvocation(
      const RefoldModel::MacroInvocation &invocation) const;

  /// Return whether current-level args-only replay preserves the entire macro
  /// expansion envelope, including fixed body tokens before/after formal slots.
  bool ArgsOnlyTemplateReplayPreservesEnvelope(
      const RefoldModel::MacroInvocation &invocation,
      const CurrentLevelTemplateSurface &surface) const;

  /// Attempt to construct an args-only macro patch from the solved template
  /// replay surface.  The recursive syntax repair and bounded DFS remain local
  /// to this helper so their mutable search state is not promoted to planner
  /// fields.
  std::optional<MacroPatch> TryTemplateSolvedArgsOnlyPatch(
      const ArgsOnlyTemplateReplayContext &ctx) const;

private:
  Dependencies deps_;
};

} // namespace refold
} // namespace clang

#endif // LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDMACROARGSONLYTEMPLATESOLVER_H
