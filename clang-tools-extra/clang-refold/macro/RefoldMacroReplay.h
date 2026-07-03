//===--- RefoldMacroReplay.h -----------------------------------*- C++ -*-===//
//
// Macro replay, layout, paste-spelling, whole-cover, and boundary-selection
// helpers for clang-refold.
//
// The classes in this header are small macro-domain replay services used by
// RefoldMacroPatchPlanner and its phase helpers.  They intentionally share one
// file pair so related replay/proof helpers live together without creating a
// new top-level file for every narrow macro primitive.
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

/// Provides shared occurrence-level macro replay helpers.
///
/// The service recovers invocation occurrence surfaces and validates replay
/// inputs that are independent of a specific macro-patch construction phase.
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

  /// Return whether a proposed replacement for one formal agrees with every
  /// directly modeled occurrence of that formal in B, including paste spans.
  bool MacroArgReplacementMatchesAllOccurrencesInB(
      const RefoldModel::MacroInvocation &m, uint32_t argIdx,
      llvm::StringRef baseArg, llvm::StringRef newArg,
      llvm::ArrayRef<diffutils::Hunk> tokenHunks) const {
    return MacroArgReplacementMatchesAllOccurrencesInBImpl(
        m, argIdx, baseArg, newArg, tokenHunks, /*checkPasteSpans=*/true,
        OccurrenceSupportMode::CurrentInvocationOnly);
  }

  /// Variant of `MacroArgReplacementMatchesAllOccurrencesInB` that
  /// intentionally skips per-span paste validation.
  ///
  /// This is used when the caller performs pasted-token validation at a higher
  /// level, such as validating that a set of argument replacements reconstructs
  /// the entire pasted token exactly.  STANDARD occurrences and, in strict
  /// mode, STRINGIFY occurrences are still enforced.
  bool MacroArgReplacementMatchesAllOccurrencesInBIgnorePaste(
      const RefoldModel::MacroInvocation &m, uint32_t argIdx,
      llvm::StringRef baseArg, llvm::StringRef newArg,
      llvm::ArrayRef<diffutils::Hunk> tokenHunks) const {
    return MacroArgReplacementMatchesAllOccurrencesInBImpl(
        m, argIdx, baseArg, newArg, tokenHunks, /*checkPasteSpans=*/false,
        OccurrenceSupportMode::CurrentInvocationOnly);
  }

  /// Variant used by DAG semantic proof paths that can justify a replacement
  /// through graph-supported occurrence evidence rather than only the current
  /// invocation's direct occurrence spans.
  bool MacroArgReplacementMatchesAllOccurrencesInBIgnorePasteSemanticProof(
      const RefoldModel::MacroInvocation &m, uint32_t argIdx,
      llvm::StringRef baseArg, llvm::StringRef newArg,
      llvm::ArrayRef<diffutils::Hunk> tokenHunks) const {
    return MacroArgReplacementMatchesAllOccurrencesInBImpl(
        m, argIdx, baseArg, newArg, tokenHunks, /*checkPasteSpans=*/false,
        OccurrenceSupportMode::AllowGraphSupport);
  }

  /// Core implementation for validating whether a proposed args-only rewrite
  /// of a macro parameter is consistent with the edited preprocessed stream B.
  ///
  /// Evidence comes from STANDARD spans, strict-mode STRINGIFY spans, and
  /// optionally PASTE spans.  The method is conservative: if an occurrence
  /// cannot be mapped or compared deterministically in the requested support
  /// mode, the candidate is rejected rather than guessed.
  bool MacroArgReplacementMatchesAllOccurrencesInBImpl(
      const RefoldModel::MacroInvocation &m, uint32_t argIdx,
      llvm::StringRef baseArg, llvm::StringRef newArg,
      llvm::ArrayRef<diffutils::Hunk> tokenHunks, bool checkPasteSpans,
      OccurrenceSupportMode supportMode) const;

  /// Return the B-side token range owned by \p span for the pure insertion hunk
  /// \p h.
  ///
  /// Converts a pure insertion from the raw hunk-local B token range into the
  /// effective occurrence-owned B range used for argument derivation and
  /// cross-occurrence verification.  For ordinary interior/begin/end ownership,
  /// the owned range is the raw inserted B range.  For the exact
  /// separator-before-right-occurrence case, the owned range is shifted so that
  /// it excludes the shared leading separator and includes the separator that
  /// now precedes the original occurrence in B.
  std::optional<std::pair<size_t, size_t>>
  GetOwnedPureInsertionBRangeForArgSpan(
      const RefoldModel::PPArgSpan &span,
      llvm::ArrayRef<RefoldModel::PPArgSpan> argSpans,
      std::pair<size_t, size_t> mappedEnv, const diffutils::Hunk &h) const;

private:
  Dependencies deps_;
};

/// Recovers source-spelled actual-argument layouts for function-like macro
/// invocations.  The layout service preserves caller spelling and delimiter
/// ranges needed by args-only and whole-cover replay.
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

/// Validates whole-cover macro replay proof surfaces.
///
/// This helper checks whether a replacement covers the complete invocation
/// envelope and whether that envelope can be certified as a macro realization.
class RefoldMacroWholeCoverProof {
public:
  /// Compute the A-token interval used for whole-cover replacement of a macro
  /// invocation.
  ///
  /// This usually matches \c m.cover, but some zero-parameter function-like
  /// macros are conservatively widened by the producer when nested in parent
  /// arguments. In those cases, the precise replacement slice is derived from
  /// the bounding box of \c bodySpans.
  static std::optional<std::pair<uint64_t, uint64_t>>
  GetWholeCoverATokRange(const RefoldModel::MacroInvocation &m);

  /// Return true when the invocation's whole-cover replacement surface is
  /// self-contained at the callsite.
  ///
  /// Whole-cover replacement is only valid when every A token in the chosen
  /// cover interval is claimed by this invocation through its own body, arg,
  /// stringify, or paste spans. Nested child invocations whose emitted tokens
  /// are interleaved with parent-owned syntax are not self-contained and must
  /// be lifted through an ancestor rather than replaced at the child callsite.
  static bool
  MacroWholeCoverIsSelfContained(const RefoldModel::MacroInvocation &m);
};

/// Builds and validates paste-spelling replay surfaces.
///
/// Paste spelling helpers splice argument-derived text into macro body glue
/// without assuming that producer paste spans form a contiguous partition of
/// the final token spelling.
class RefoldMacroPasteSpelling {
public:
  /// Splice a derived paste-segment edit into a macro argument's spelling text.
  ///
  /// This helper is used after paste-edit derivation determines that an edit
  /// inside a token-pasted (`##`) output token can be attributed to a single
  /// argument slice.  The splice is conservative: it only applies to an
  /// unambiguous whole-argument, prefix, or suffix occurrence after trimming
  /// edge whitespace.
  static std::string SplicePasteSegmentIntoSpellingArg(llvm::StringRef baseArg,
                                                       llvm::StringRef oldSeg,
                                                       llvm::StringRef newSeg);

  /// Splice a replay-derived paste edit through an exact producer slice.
  ///
  /// The producer has already recorded the byte range inside the original
  /// invocation argument that supplied the paste part, so the consumer only
  /// verifies that the range still spells `oldSeg` and then replaces exactly
  /// that range with `newSeg`.
  static std::string SplicePasteSegmentIntoSpellingArgExact(
      llvm::StringRef baseArg, uint32_t argByteBegin, uint32_t argByteEnd,
      llvm::StringRef oldSeg, llvm::StringRef newSeg);
};

/// Selects macro replay boundaries from token and source envelopes.
///
/// The selector answers boundary questions used by args-only, generated, and
/// whole-cover replay paths without owning candidate ranking or proof stamping.
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
  /// insertions at the old owner cover boundaries, but they are macro-owned
  /// only if a generated descendant callee came from a caller parameter.
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
