//===--- RefoldMacroReplay.h -----------------------------------*- C++ -*-===//
//
// Macro replay, layout, paste-spelling, whole-cover, and boundary-selection
// helpers for clang-refold.
//
// The classes in this header are small macro-domain services extracted from
// RefoldEngine and RefoldMacroPatchPlanner.  They intentionally share one file
// pair so related replay/proof helpers live together without creating a new
// top-level file for every narrow macro primitive.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_MACRO_REFOLDMACROREPLAY_H
#define LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_MACRO_REFOLDMACROREPLAY_H

#include "core/RefoldModel.h"
#include "proof/RefoldAcceptedResultTypes.h"
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
class RefoldSourceMapper;

class RefoldMacroOccurrenceReplay {
public:
  /// Borrowed state required to replay macro formal occurrences against B.
  /// The object is intentionally lightweight so the macro planner can create a
  /// short-lived view whenever it needs occurrence consistency proof.
  struct Dependencies {
    llvm::ArrayRef<PPTok> aToks;
    llvm::ArrayRef<size_t> bTokOff;
    const RefoldMacroTopology *macroTopology = nullptr;
    const RefoldSourceMapper *sourceMapper = nullptr;
    bool strict = false;
  };

  explicit RefoldMacroOccurrenceReplay(Dependencies deps);

  bool MacroArgReplacementMatchesAllOccurrencesInB(
      const RefoldModel::MacroInvocation &m, uint32_t argIdx,
      llvm::StringRef baseArg, llvm::StringRef newArg,
      llvm::ArrayRef<diffutils::Hunk> tokenHunks) const {
    return MacroArgReplacementMatchesAllOccurrencesInBImpl(
        m, argIdx, baseArg, newArg, tokenHunks, /*checkPasteSpans=*/true,
        OccurrenceSupportMode::CurrentInvocationOnly);
  }

  bool MacroArgReplacementMatchesAllOccurrencesInBIgnorePaste(
      const RefoldModel::MacroInvocation &m, uint32_t argIdx,
      llvm::StringRef baseArg, llvm::StringRef newArg,
      llvm::ArrayRef<diffutils::Hunk> tokenHunks) const {
    return MacroArgReplacementMatchesAllOccurrencesInBImpl(
        m, argIdx, baseArg, newArg, tokenHunks, /*checkPasteSpans=*/false,
        OccurrenceSupportMode::CurrentInvocationOnly);
  }

  bool MacroArgReplacementMatchesAllOccurrencesInBIgnorePasteSemanticProof(
      const RefoldModel::MacroInvocation &m, uint32_t argIdx,
      llvm::StringRef baseArg, llvm::StringRef newArg,
      llvm::ArrayRef<diffutils::Hunk> tokenHunks) const {
    return MacroArgReplacementMatchesAllOccurrencesInBImpl(
        m, argIdx, baseArg, newArg, tokenHunks, /*checkPasteSpans=*/false,
        OccurrenceSupportMode::AllowGraphSupport);
  }

  bool MacroArgReplacementMatchesAllOccurrencesInBImpl(
      const RefoldModel::MacroInvocation &m, uint32_t argIdx,
      llvm::StringRef baseArg, llvm::StringRef newArg,
      llvm::ArrayRef<diffutils::Hunk> tokenHunks, bool checkPasteSpans,
      OccurrenceSupportMode supportMode) const;

  std::optional<std::pair<size_t, size_t>> GetOwnedPureInsertionBRangeForArgSpan(
      const RefoldModel::PPArgSpan &span,
      llvm::ArrayRef<RefoldModel::PPArgSpan> argSpans,
      std::pair<size_t, size_t> mappedEnv, const diffutils::Hunk &h) const;

private:
  Dependencies deps_;
};

class RefoldMacroActualLayout {
public:
  struct Dependencies {
    const clang::LangOptions *lexLang = nullptr;
  };

  explicit RefoldMacroActualLayout(Dependencies deps);

  /// Recover formal-actual content ranges for the supplied invocation text.
  /// The returned vector is indexed by formal parameter number, not by parsed
  /// actual count; variadic surplus and omitted trailing variadic formals are
  /// normalized before the layout is accepted.
  std::optional<std::vector<std::pair<size_t, size_t>>>
  GetMacroInvocationFormalArgContentRanges(
      const RefoldModel::MacroInvocation &m, llvm::StringRef invText) const;

private:
  Dependencies deps_;
};

class RefoldMacroWholeCoverProof {
public:
  static std::optional<std::pair<uint64_t, uint64_t>>
  GetWholeCoverATokRange(const RefoldModel::MacroInvocation &m);

  static bool
  MacroWholeCoverIsSelfContained(const RefoldModel::MacroInvocation &m);
};

class RefoldMacroPasteSpelling {
public:
  static std::string SplicePasteSegmentIntoSpellingArg(
      llvm::StringRef baseArg, llvm::StringRef oldSeg,
      llvm::StringRef newSeg);

  static std::string SplicePasteSegmentIntoSpellingArgExact(
      llvm::StringRef baseArg, uint32_t argByteBegin, uint32_t argByteEnd,
      llvm::StringRef oldSeg, llvm::StringRef newSeg);
};

class RefoldMacroBoundarySelector {
public:
  RefoldMacroBoundarySelector(const RefoldModel &model,
                              const RefoldMacroTopology &macroTopology,
                              const RefoldSourceMapper &sourceMapper,
                              llvm::ArrayRef<PPTok> bToks,
                              const clang::LangOptions &lexLang);

  /// Return a patchable macro whose expansion ends at a pure insertion gap and
  /// whose generated descendant chain contains `__VA_OPT__`.
  ///
  /// Ordinary macro ownership deliberately excludes half-open range boundaries:
  /// a pure insertion at `cover.end` normally belongs to the surrounding TU or
  /// include, not to the macro.  The only exception currently proved here is an
  /// activation of an inactive variadic tail, where the inserted B tokens are
  /// part of the macro's generated replacement-list grammar even though the old
  /// expansion had no A tokens at that position.
  const RefoldModel::MacroInvocation *RightBoundaryVaOptActivationMacro(
      uint64_t aGap,
      std::optional<uint64_t> ownerIncludeId = std::nullopt) const;

  /// Return a patchable macro whose boundary insertion may be part of replacing
  /// a generated callee selector.
  ///
  /// Selector replacement can change only tokens immediately around the old
  /// generated callee expansion, for example `STR(x)` -> `WRAP(x)` changes
  /// `"x"` into `"[" "x" "]"`.  Those prefix/suffix tokens are pure
  /// insertions at the old owner cover boundaries, but they are macro-owned only
  /// if a generated descendant callee came from a caller parameter.
  const RefoldModel::MacroInvocation *BoundaryGeneratedSelectorMacro(
      uint64_t aGap,
      std::optional<uint64_t> ownerIncludeId = std::nullopt) const;

  /// Return a patchable function-like macro at a definition-tape replay
  /// boundary for a pure insertion hunk.
  ///
  /// Boundary insertions are normally owned by the surrounding TU/include
  /// because half-open macro covers do not include the gap itself.  This
  /// selector admits only the narrow cases where the recorded definition tape
  /// exposes an empty formal source slot or a missing token-bearing formal at
  /// the expansion frontier.  The caller must still build and prove the actual
  /// macro rewrite; this method only chooses the candidate owner.
  const RefoldModel::MacroInvocation *BoundaryDefinitionTapeReplayMacro(
      const diffutils::Hunk &hunk,
      std::optional<uint64_t> ownerIncludeId = std::nullopt) const;

private:
  const RefoldModel &model_;
  const RefoldMacroTopology &macroTopology_;
  const RefoldSourceMapper &sourceMapper_;
  llvm::ArrayRef<PPTok> bToks_;
  const clang::LangOptions &lexLang_;
};

} // namespace refold
} // namespace clang

#endif
