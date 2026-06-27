//===--- RefoldSourceMapper.h -----------------------------------*- C++ -*-===//
//
// Deterministic A/B source-coordinate mapping service for clang-refold.
//
// RefoldSourceMapper owns no source buffers.  It is a lightweight service over
// immutable A/B source text, token streams, token-offset tables, and edit hunk
// caches.  Keeping those dependencies explicit lets callers perform byte/token
// coordinate projection without exposing unrelated orchestration state.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDSOURCEMAPPER_H
#define LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDSOURCEMAPPER_H

#include "core/RefoldModel.h"
#include "source/DiffAlgorithms.h"
#include "source/RefoldToken.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

namespace clang {
namespace refold {

class RefoldSourceMapper {
public:
  RefoldSourceMapper(
      llvm::StringRef aSource, llvm::StringRef bSource,
      llvm::ArrayRef<PPTok> aToks, llvm::ArrayRef<PPTok> bToks,
      llvm::ArrayRef<size_t> aTokOff, llvm::ArrayRef<size_t> bTokOff,
      const std::vector<diffutils::Hunk> &abTokHunks,
      std::optional<std::vector<diffutils::Hunk>> &abByteHunks,
      std::vector<int64_t> &abByteHunkPrefixDelta, bool strict)
      : aSource_(aSource), bSource_(bSource), aToks_(aToks), bToks_(bToks),
        aTokOff_(aTokOff), bTokOff_(bTokOff), abTokHunks_(abTokHunks),
        abByteHunks_(abByteHunks),
        abByteHunkPrefixDelta_(abByteHunkPrefixDelta), strict_(strict) {}

  /// Slice an arbitrary source text by token indices using a token-to-byte
  /// offset table.  The token and byte coordinates are clamped defensively so
  /// callers get an empty slice rather than undefined behavior for malformed
  /// ranges.
  static llvm::StringRef SliceSource(llvm::ArrayRef<size_t> tokOff,
                                     llvm::StringRef source,
                                     uint64_t startTok, uint64_t endTok);

  /// Slice the original A-side preprocessed source by token index.
  llvm::StringRef SliceASource(uint64_t aStartTok, uint64_t aEndTok) const {
    return SliceSource(aTokOff_, aSource_, aStartTok, aEndTok);
  }

  /// Slice the edited B-side preprocessed source by token index.
  llvm::StringRef SliceBSource(uint64_t bStartTok, uint64_t bEndTok) const {
    return SliceSource(bTokOff_, bSource_, bStartTok, bEndTok);
  }

  /// Convert a B-token range into the corresponding half-open B-byte range.
  ///
  /// The conversion is exact and fail-closed: malformed token ranges, missing
  /// sentinel offsets, reversed byte ranges, or byte spans outside the B source
  /// return std::nullopt instead of manufacturing a fallback range.
  std::optional<std::pair<uint64_t, uint64_t>>
  BTokenRangeToByteRange(uint64_t bTokBegin, uint64_t bTokEnd) const;

  /// Determine whether an edit hunk is entirely contained by macro argument
  /// spans, marking every touched occurrence.  Pure insertions require an exact
  /// occurrence owner; replacements/deletions require every consumed A token to
  /// be covered by an argument-like span.
  bool HunkFullyWithinArgSpans(
      const diffutils::Hunk &h,
      llvm::ArrayRef<RefoldModel::PPArgSpan> argSpans,
      llvm::MutableArrayRef<char> touched) const;

  /// Diff the raw A/B source bytes into byte-level hunks using the same
  /// deterministic diff/coalesce path as token hunks.
  std::vector<diffutils::Hunk> BuildByteHunksFromRawText() const;

  /// Build the prefix-summed A->B byte-length delta cache for the current
  /// byte-hunk vector.  The cache is stored in the engine-owned vector supplied
  /// to the mapper constructor.
  void BuildByteHunkPrefixDeltaCache();

  /// Project an A-byte coordinate into B using lower-bound semantics.  Bytes
  /// inside an A-consuming hunk snap to the start of that hunk's B envelope.
  size_t MapAByteToBByteLowerBound(size_t aByte) const;

  /// Project an A-byte coordinate into B using upper-bound semantics.  Boundary
  /// pure insertions at the queried A coordinate are included in the projected
  /// upper bound.
  size_t MapAByteToBByteUpperBound(size_t aByte) const;

  /// Return the greatest B token/sentinel index whose byte offset is <= bByte.
  size_t BTokIndexFloor(size_t bByte) const;

  /// Return the smallest B token/sentinel index whose byte offset is >= bByte.
  size_t BTokIndexCeil(size_t bByte) const;

  /// Map an A-byte range to the corresponding B-token envelope, trimming
  /// B-only byte insertions anchored exactly at a non-empty A-range boundary.
  std::pair<size_t, size_t>
  MapAByteRangeToBTokenEnvelope(size_t aByteBegin, size_t aByteEnd) const;

  /// Map an A-byte range to B tokens while intentionally preserving pure
  /// insertions at the A-range boundaries.
  std::pair<size_t, size_t>
  MapAByteRangeToBTokenEnvelopePreserveBoundaryInsertions(
      size_t aByteBegin, size_t aByteEnd) const;

  /// Map a producer-recorded PP argument span to its B-token envelope.  Strict
  /// mode fails closed when the producer byte span is unavailable.
  std::optional<std::pair<size_t, size_t>>
  MapAToBTokenEnvelopeByPPArgSpan(const RefoldModel::PPArgSpan &sp) const;

  /// Trim pure B-token insertions anchored at the two edges of an A-token cover.
  bool TrimPureBoundaryInsertionsFromTokenEnvelope(
      uint64_t beginTok, uint64_t endTok, std::pair<size_t, size_t> &env) const;

  /// Recover the exact contiguous B-token image of an unchanged A-token range.
  /// This is a narrow proof: replacements, deletions, interior pure insertions,
  /// spelling mismatches, and non-contiguous images all fail closed.
  std::optional<std::pair<size_t, size_t>>
  TryMapUnchangedATokRangeToExactContiguousBImage(uint64_t beginTok,
                                                  uint64_t endTok) const;

  /// Map a non-empty A-token cover to a B-token envelope, excluding pure
  /// insertions anchored exactly at the cover boundaries.
  std::optional<std::pair<size_t, size_t>>
  MapATokRangeAToBTokenEnvelope(uint64_t beginTok, uint64_t endTok) const;

  /// Map an A-token cover while preserving boundary insertions.
  std::optional<std::pair<size_t, size_t>>
  MapATokRangeAToBTokenEnvelopePreserveBoundaryInsertions(
      uint64_t beginTok, uint64_t endTok) const;

  /// Map a whole-cover A-token replacement interval, preserving the full B-side
  /// projected envelope for later claim clipping.
  std::optional<std::pair<size_t, size_t>>
  MapATokRangeAToBTokenEnvelopeWholeCover(uint64_t beginTok,
                                          uint64_t endTok) const;

  /// Variant for whole-cover paths that explicitly trims edge pure insertions
  /// after projection to prevent duplicate emission of B-only boundary payloads.
  std::optional<std::pair<size_t, size_t>>
  MapATokRangeAToBTokenEnvelopeTrimEdgeInsertions(uint64_t beginTok,
                                                  uint64_t endTok) const;

private:
  std::optional<size_t> FindExactOwningArgSpanForPureInsertion(
      uint64_t aPos, llvm::ArrayRef<RefoldModel::PPArgSpan> argSpans) const;

  llvm::StringRef aSource_;
  llvm::StringRef bSource_;
  llvm::ArrayRef<PPTok> aToks_;
  llvm::ArrayRef<PPTok> bToks_;
  llvm::ArrayRef<size_t> aTokOff_;
  llvm::ArrayRef<size_t> bTokOff_;
  const std::vector<diffutils::Hunk> &abTokHunks_;
  std::optional<std::vector<diffutils::Hunk>> &abByteHunks_;
  std::vector<int64_t> &abByteHunkPrefixDelta_;
  bool strict_ = false;
};

} // namespace refold
} // namespace clang

#endif // LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDSOURCEMAPPER_H
