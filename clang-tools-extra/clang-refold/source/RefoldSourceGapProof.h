//===--- RefoldSourceGapProof.h -------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Shared exact physical-source gap coverage theorem.
//
// A token hunk, include closure, or owner transition may cross a physical byte
// gap only after every byte in that gap has one deterministic explanation.  The
// explanation is an ordered set of caller-authorized opaque pieces, exact
// preprocessing-structure intervals already contained by those pieces, and
// caller-authorized neutral ranges between the pieces.  This service owns the
// common mechanics:
//
//   * exact lexical-boundary validation;
//   * deterministic piece ordering;
//   * caller-declared nested-piece absorption;
//   * rejection of every other overlap;
//   * use of the shared preprocessing-structure inventory; and
//   * complete left-to-right byte coverage.
//
// It deliberately does not decide whether an include, macro invocation,
// conditional island, directive, or pragma state transition is semantically
// neutral.  Callers prove those path-specific obligations before submitting a
// piece or through the narrow neutral-range policy callback.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDSOURCEGAPPROOF_H
#define LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDSOURCEGAPPROOF_H

#include "source/RefoldPreprocessingStructureIndex.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/STLFunctionalExtras.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace clang {
namespace refold {

/// One caller-authorized opaque physical source interval in a source gap.
///
/// `payloadIndex` is returned to the caller after normalization.  `kindOrder`
/// and `stableId` are deterministic tie-breakers only; they never establish
/// semantic authority.  `nestingClass` identifies this piece's class, while
/// `absorbedNestingClasses` names classes that this piece's independent proof
/// already covers when they are wholly nested inside it.  Class values must be
/// less than 64.
struct SourceGapProofPiece {
  uint64_t begin = 0;
  uint64_t end = 0;
  uint64_t stableId = 0;
  uint32_t kindOrder = 0;
  uint32_t nestingClass = 0;
  uint64_t absorbedNestingClasses = 0;
  size_t payloadIndex = 0;

  /// Return whether this piece's proof subsumes `nested` when the nested byte
  /// interval is wholly contained in this piece.
  bool Absorbs(const SourceGapProofPiece &nested) const {
    return nested.nestingClass < 64 &&
           (absorbedNestingClasses &
            (uint64_t{1} << nested.nestingClass)) != 0;
  }
};

/// Deterministically normalized result of one exact source-gap proof.
struct SourceGapProofResult {
  /// Caller payload indices for the surviving outer pieces, in source order.
  std::vector<size_t> outerPiecePayloadIndices;
  /// Exact protected intervals observed in the queried gap, in index order.
  std::vector<const PreprocessingStructureInterval *> protectedIntervals;
};

/// Return the unique exact lexical interval bound to one producer item.
///
/// Producer array ids are qualified by `modelKind`.  Multiple matching lexical
/// intervals are ambiguous and return std::nullopt rather than selecting by
/// source order.
std::optional<std::pair<uint64_t, uint64_t>>
findSourceGapProducerInterval(
    const RefoldPreprocessingStructureIndex &structureIndex,
    PreprocessingStructureModelKind modelKind, uint64_t modelItemId);

/// Return the exact complete lexical source range of one conditional group.
///
/// The range begins at the scanner's complete opening logical-line prefix and
/// ends after the uniquely bound matching #endif line.  This normalizes old
/// maps whose `group_b` names the directive introducer rather than the leading
/// logical-line trivia without performing any offset search.
std::optional<std::pair<uint64_t, uint64_t>>
findSourceGapConditionalGroupRange(
    const RefoldPreprocessingStructureIndex &structureIndex,
    uint64_t conditionalGroupId);

/// Return the bit corresponding to one protected preprocessing kind.
///
/// The current enum has fewer than 64 members.  The function fails closed by
/// returning zero if that invariant is ever violated by a future extension.
uint64_t sourceGapProtectedStructureKindBit(
    PreprocessingStructureKind kind);

/// Return the mask containing every conditional-control directive kind.
///
/// Callers use this only when a separate conditional-control parser proves the
/// complete uncovered bytes.  The mask does not itself make those bytes
/// neutral or preservable.
uint64_t sourceGapConditionalDirectiveKindMask();

/// Prove that a gap consists only of indexed preprocessing structure and exact
/// lexer trivia.
///
/// Every overlapping protected interval is treated as an opaque preserved
/// source piece.  Nested protected records are absorbed by their containing
/// interval because this theorem proves only physical byte coverage: it does
/// not discard or semantically reinterpret either record.  Partially crossing
/// intervals and nontrivia bytes outside the indexed intervals fail closed.
std::optional<SourceGapProofResult>
proveSourceGapWithIndexedStructureAndTrivia(
    const RefoldPreprocessingStructureIndex &structureIndex,
    uint64_t gapBegin, uint64_t gapEnd, std::string *reason = nullptr);

/// Prove one gap using the index's exact lexer-trivia census.
///
/// Every protected preprocessing interval must be wholly contained by one
/// surviving caller piece.  This is the standard mode for preserved structural
/// gaps and recorded source-neutral islands.  Empty `pieces` are accepted only
/// when the complete gap is indexed lexical trivia.
std::optional<SourceGapProofResult> proveSourceGapWithIndexedTrivia(
    const RefoldPreprocessingStructureIndex &structureIndex,
    uint64_t gapBegin, uint64_t gapEnd,
    llvm::ArrayRef<SourceGapProofPiece> pieces,
    std::string *reason = nullptr);

/// Prove one gap with a caller-specific neutral-range policy.
///
/// This overload is used by the expansion fallback when an inter-piece range
/// may contain a separately proved literal conditional-control sequence.  The
/// shared structure census still remains authoritative: every protected
/// interval not contained by an opaque piece must have its kind bit present in
/// `uncoveredProtectedStructureKinds`, and `proveNeutralRange` must then prove
/// the complete bytes containing it.  `consumeOuterPiece` is invoked in source
/// order between neutral-range calls so stateful scanners can retain exact
/// physical-line context across opaque pieces.
std::optional<SourceGapProofResult> proveSourceGapWithPolicy(
    const RefoldPreprocessingStructureIndex &structureIndex,
    uint64_t gapBegin, uint64_t gapEnd,
    llvm::ArrayRef<SourceGapProofPiece> pieces,
    llvm::function_ref<bool(uint64_t, uint64_t)> proveNeutralRange,
    llvm::function_ref<void(size_t)> consumeOuterPiece,
    uint64_t uncoveredProtectedStructureKinds,
    std::string *reason = nullptr);

} // namespace refold
} // namespace clang

#endif // LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDSOURCEGAPPROOF_H
