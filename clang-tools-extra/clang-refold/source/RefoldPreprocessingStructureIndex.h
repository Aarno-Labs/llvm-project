//===--- RefoldPreprocessingStructureIndex.h -------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Exact physical-source census for preprocessing structure.
//
// Generic token-derived edits are not entitled to treat the byte envelope
// between mapped tokens as ordinary source.  That envelope can contain
// directives or pragma operators that mutate preprocessing state without
// contributing ordinary A tokens.  This service inventories those protected
// source intervals before later edit-admission code decides whether an edit may
// cross, preserve, or specially rewrite them.
//
// The index is deliberately evidence-only.  A producer binding identifies the
// model record associated with an exact lexical interval, but it does not
// certify that consuming, moving, or reconstructing that interval is safe.
// State-transition discharge remains the responsibility of the owner-state
// proof gateway.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_PREPROCESSING_STRUCTURE_INDEX_H
#define LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_PREPROCESSING_STRUCTURE_INDEX_H

#include "source/RefoldPreprocessingDirectiveScanner.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace clang {

class LangOptions;

namespace refold {

class RefoldMacroStateProof;
class RefoldModel;
class RefoldPathIdentity;

/// Physical preprocessing construct protected by the structure index.
///
/// Conditional-control directives are kept distinct because later structural
/// tiling must preserve their ordered group/arm topology.  `OtherDirective`
/// intentionally covers every lexically valid directive not modeled by a more
/// specific enumerator; unknown directives are protected rather than ignored.
enum class PreprocessingStructureKind {
  ConditionalIf,
  ConditionalIfdef,
  ConditionalIfndef,
  ConditionalElif,
  ConditionalElifdef,
  ConditionalElifndef,
  ConditionalElse,
  ConditionalEndif,
  MacroDefine,
  MacroUndef,
  Include,
  IncludeNext,
  Import,
  Pragma,
  PragmaOperator,
  LineControl,
  ErrorDirective,
  WarningDirective,
  OtherDirective,
};

/// Return a stable diagnostic spelling for a preprocessing-structure kind.
llvm::StringRef toString(PreprocessingStructureKind kind);

/// Producer record class bound to a lexical preprocessing interval.
///
/// Model ids live in several producer arrays and are not assumed to share one
/// namespace.  Carrying the record class prevents a line-control event id from
/// being mistaken for an item id with the same integer value.
enum class PreprocessingStructureModelKind {
  None,
  MacroDirective,
  IncludeDirective,
  PragmaDirective,
  LineControlEvent,
  ConditionalDirective,
};

/// Return a stable diagnostic spelling for a producer binding class.
llvm::StringRef toString(PreprocessingStructureModelKind kind);

/// One exact half-open physical source interval containing preprocessing
/// structure.
///
/// For directive forms, `[begin,end)` starts at the beginning of the complete
/// logical-line prefix containing the directive introducer.  That prefix can
/// include horizontal whitespace, escaped-newline continuations, and comments
/// that preprocessing replaces with whitespace.  The interval ends after the
/// complete logical directive line, including the terminating newline when one
/// is present.  `_Pragma`/`__pragma` use exact directly spelled raw-token
/// ranges; producer `_Pragma` provenance can additionally represent an
/// expansion-derived occurrence.
///
/// `conditionalGroupId` and `conditionalArmId` identify the group/arm opened by
/// a conditional-control directive.  `ownerConditionalArmId` instead names the
/// lexically enclosing producer arm, when that outer arm was bound exactly.
struct PreprocessingStructureInterval {
  PreprocessingStructureKind kind =
      PreprocessingStructureKind::OtherDirective;
  std::string sourcePath;
  std::optional<uint64_t> ownerIncludeId;
  std::optional<uint64_t> ownerConditionalArmId;
  std::optional<uint64_t> conditionalGroupId;
  std::optional<uint64_t> conditionalArmId;
  PreprocessingStructureModelKind modelKind =
      PreprocessingStructureModelKind::None;
  std::optional<uint64_t> modelItemId;

  /// Exact scanner-proven preprocessing spelling inside `[begin,end)`.
  ///
  /// Directive lines begin at the `#`, `%:`, or enabled trigraph introducer and
  /// end immediately before the terminating unspliced physical newline.  A
  /// pragma operator uses its complete raw-token interval.  Leading logical-line
  /// trivia and the terminating newline remain protected by `[begin,end)`, but
  /// an explicitly authorized directive operation need not consume them.
  uint64_t structureSpellingBegin = 0;
  uint64_t structureSpellingEnd = 0;

  /// Exact producer text range bound to this interval, when one exists.
  ///
  /// A producer directive record may omit leading logical-line trivia or the
  /// terminating physical newline while still naming the complete directive
  /// spelling that its specialized planner is authorized to rewrite.  These
  /// coordinates preserve that narrower authority without weakening the
  /// lexical protection interval `[begin,end)` used for ordinary edits.
  std::optional<uint64_t> producerTextBegin;
  std::optional<uint64_t> producerTextEnd;

  uint64_t begin = 0;
  uint64_t end = 0;

  /// Return whether the interval has a nonempty, well-formed byte range.
  bool IsValid() const {
    if (begin >= end || begin > structureSpellingBegin ||
        structureSpellingBegin >= structureSpellingEnd ||
        structureSpellingEnd > end) {
      return false;
    }
    if (producerTextBegin.has_value() != producerTextEnd.has_value())
      return false;
    if (!producerTextBegin)
      return true;
    return begin <= *producerTextBegin && *producerTextBegin < *producerTextEnd &&
           *producerTextEnd <= end;
  }

  /// Return whether an exact nonempty producer text range is available.
  bool HasExactProducerTextRange() const {
    return producerTextBegin && producerTextEnd &&
           begin <= *producerTextBegin &&
           *producerTextBegin < *producerTextEnd && *producerTextEnd <= end;
  }

  /// Return whether an exact producer record was bound to this interval.
  bool IsProducerBound() const {
    if (modelKind == PreprocessingStructureModelKind::ConditionalDirective)
      return conditionalGroupId.has_value();
    return modelKind != PreprocessingStructureModelKind::None &&
           modelItemId.has_value() && HasExactProducerTextRange();
  }

  /// Return whether this interval overlaps `[queryBegin,queryEnd)`.
  bool Overlaps(uint64_t queryBegin, uint64_t queryEnd) const {
    return queryBegin < end && begin < queryEnd;
  }
};

/// Immutable exact preprocessing-structure inventory for one physical source
/// owner occurrence.
///
/// The same header bytes can be entered by several include instances.  The
/// builder therefore receives the concrete optional include owner and binds
/// producer records only in that owner domain.  Lexically discovered
/// directives remain protected even when old or incomplete producer metadata
/// cannot bind them to a unique record.
class RefoldPreprocessingStructureIndex {
public:
  /// Read-only dependencies required to build one source-owner index.
  struct Dependencies {
    /// Producer model used to bind exact directive records and conditionals.
    const RefoldModel &model;
    /// Path-equivalence service for producer/source spelling comparison.
    const RefoldPathIdentity &paths;
    /// Shared exact #define/#undef physical-line recovery service.
    const RefoldMacroStateProof &macroStateProof;
    /// Language mode used by Clang's raw lexer while recognizing directives.
    const clang::LangOptions &lexLang;
  };

  /// Build an index for `sourcePath` and one concrete source-owner occurrence.
  ///
  /// The source buffer is scanned by the shared Clang-raw-lexer logical-line
  /// service so comments, digraph directive introducers, trigraph mode, raw
  /// literals, and escaped-newline token spellings follow the active language
  /// mode.  Producer records are attached only after their source text and
  /// byte anchors match the scanned
  /// construct exactly.  Dedicated `_Pragma` operator coordinates supplement
  /// that lexical census for expansion-derived occurrences.  Any malformed
  /// producer interval is reported in `GetDiagnostics()` and never
  /// manufactures authority for an out-of-bounds source range.
  static RefoldPreprocessingStructureIndex
  Build(Dependencies deps, llvm::StringRef sourcePath,
        llvm::StringRef sourceBytes,
        std::optional<uint64_t> ownerIncludeId = std::nullopt);

  /// Source path whose physical bytes were indexed.
  llvm::StringRef GetSourcePath() const { return sourcePath_; }

  /// Number of physical source bytes covered by this immutable index.
  uint64_t GetSourceSize() const { return sourceSize_; }

  /// Concrete include occurrence owning the indexed bytes, or nullopt for TU.
  std::optional<uint64_t> GetOwnerIncludeId() const {
    return ownerIncludeId_;
  }

  /// Deterministically source-ordered protected intervals.
  llvm::ArrayRef<PreprocessingStructureInterval> GetIntervals() const {
    return intervals_;
  }

  /// Build-time diagnostics for malformed or ambiguously bound producer facts.
  llvm::ArrayRef<std::string> GetDiagnostics() const { return diagnostics_; }

  /// Return true when no source-scanning or producer-binding inconsistency was
  /// observed while constructing the index.
  ///
  /// Unmodeled but lexically valid non-conditional directives do not make the
  /// protective census incomplete: they remain explicit intervals and are
  /// therefore safe preserve-in-place boundaries.  Conditional controls are
  /// stricter because later state proofs require exact group/arm topology:
  /// every lexical conditional control must bind uniquely to the producer, and
  /// every producer conditional group in this owner must bind to one complete
  /// lexical group.  Any scanner inconsistency or failed producer binding makes
  /// the index incomplete.
  bool IsComplete() const { return diagnostics_.empty(); }

  /// Return whether the exact physical protection census is complete.
  ///
  /// This is intentionally narrower than `IsComplete()`.  An ordinary
  /// producer-binding mismatch does not erase the lexically discovered
  /// directive interval, so it rejects only a span that overlaps that interval.
  /// Scanner/topology failures, non-unique conditional bindings, and an
  /// expansion-derived pragma operator with no exact physical interval are
  /// different: in those cases the protection inventory itself is incomplete.
  ///
  /// The historical direct-TU name below is retained as a compatibility alias;
  /// source-gap proof uses the same scanner/topology completeness theorem for
  /// TU and concrete include-owner indexes.
  bool IsProtectionCensusComplete() const {
    return directTUProtectionDiagnostics_.empty();
  }

  bool IsDirectTUProtectionCensusComplete() const {
    return IsProtectionCensusComplete();
  }

  /// Diagnostics that make every direct TU byte proof unavailable.
  /// `GetDiagnostics()` remains the complete producer + protection diagnostic
  /// stream used by producer-authoritative consumers.
  llvm::ArrayRef<std::string> GetDirectTUProtectionDiagnostics() const {
    return directTUProtectionDiagnostics_;
  }

  /// Return true when at least one protected interval lacks an exact producer
  /// binding.  Later structural replay may use this to distinguish a safe
  /// preserve-in-place boundary from a directive it is authorized to rebuild.
  bool HasUnboundStructure() const;

  /// Return every interval overlapping the requested half-open byte range.
  std::vector<const PreprocessingStructureInterval *>
  FindOverlapping(uint64_t begin, uint64_t end) const;

  /// Return whether any interval overlaps the requested half-open byte range.
  ///
  /// This answers the same question as `!FindOverlapping(begin, end).empty()`
  /// from the same binary-search prologue, returning at the first overlap
  /// instead of materializing the complete result list.
  bool HasOverlapping(uint64_t begin, uint64_t end) const;

  /// Return whether `[begin,end)` is covered by one maximal exact lexical
  /// trivia interval discovered in the same whole-source scan as the directive
  /// census.
  ///
  /// Empty ranges are trivially ignorable.  Nonempty ranges must lie wholly
  /// inside whitespace/comment/splice trivia; beginning or ending inside an
  /// ordinary token, comment, string, or other lexical construct is rejected.
  bool IsRangeLexicallyIgnorable(uint64_t begin, uint64_t end) const;

  /// Return whether `[begin,end)` is exactly one complete raw-lexer token
  /// spelling in the indexed physical source.
  bool IsExactTokenSpellingInterval(uint64_t begin, uint64_t end) const;

  /// Return whether `offset` is not inside a raw token, phase-two splice,
  /// comment, or physical CRLF spelling.
  bool IsExactLexicalBoundary(uint64_t offset) const;

  /// Prove one ordinary internal direct-TU source gap.
  ///
  /// Empty ranges are accepted.  A nonempty range must be covered completely
  /// by exact lexer trivia without cutting an indivisible trivia component, and
  /// it must overlap no protected preprocessing-structure interval.  In
  /// particular, an exact producer-bound `#define` or `#undef` remains
  /// preprocessing state and is never ordinary trivia authority.
  bool ProveOrdinaryDirectTUInternalGap(uint64_t begin, uint64_t end) const;

  /// Collect exact producer-bound macro-state directives inside a byte range.
  ///
  /// This is an evidence-only query.  It reports complete `#define` and
  /// `#undef` intervals wholly contained in `[begin,end)`, but does not prove
  /// that any edit may consume them.  Other overlapping structure is ignored
  /// by this query and must be handled independently by the caller.  A false
  /// return denotes an invalid query, incomplete protection census, nonlexical
  /// boundary, or a partially intersected macro-state interval.
  bool CollectExactMacroStateIntervals(
      uint64_t begin, uint64_t end,
      std::vector<const PreprocessingStructureInterval *> &intervals) const;

private:
  /// Return the first interval index that can still reach byte \p begin.
  ///
  /// Shared binary-search prologue of the overlap queries: every earlier
  /// interval ends at or before \p begin, so none of them can overlap a range
  /// starting there.
  size_t FirstPossibleOverlappingIndex(uint64_t begin) const;

  std::string sourcePath_;
  uint64_t sourceSize_ = 0;
  std::optional<uint64_t> ownerIncludeId_;
  std::vector<PreprocessingStructureInterval> intervals_;
  /// Prefix maximum of `intervals_[0..i].end` for logarithmic overlap lookup.
  std::vector<uint64_t> prefixMaximumIntervalEnds_;
  std::vector<PreprocessingLexicalTokenInterval> lexicalTokenIntervals_;
  std::vector<PreprocessingTriviaInterval> triviaIntervals_;
  std::vector<PreprocessingIndivisibleTriviaInterval>
      indivisibleTriviaIntervals_;
  /// Failures that leave physical preprocessing protection incomplete.
  std::vector<std::string> directTUProtectionDiagnostics_;
  /// Complete lexical and producer-binding diagnostic stream.
  std::vector<std::string> diagnostics_;
};

} // namespace refold
} // namespace clang

#endif // LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_PREPROCESSING_STRUCTURE_INDEX_H
