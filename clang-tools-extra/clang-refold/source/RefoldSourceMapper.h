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
#include "util/RefoldPathIdentity.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

namespace clang {
namespace refold {

/// Projects between A/B token, byte, and source-coordinate domains.
///
/// The mapper is deliberately read-only with respect to source buffers and
/// token streams.  It translates half-open token covers into source byte
/// slices, projects A-side byte coordinates through the raw byte diff into B,
/// and recovers B-token envelopes for later proof and patch construction.  All
/// public mapping APIs either return a precise range in the named coordinate
/// domain or fail closed with `std::nullopt`; they must not manufacture EOF
/// anchors or widen ownership when producer source facts are missing.
class RefoldSourceMapper {
public:
  RefoldSourceMapper(const RefoldModel &model, const RefoldPathIdentity &paths,
                     llvm::StringRef aSource, llvm::StringRef bSource,
                     llvm::ArrayRef<PPTok> aToks, llvm::ArrayRef<PPTok> bToks,
                     llvm::ArrayRef<size_t> aTokOff,
                     llvm::ArrayRef<size_t> bTokOff,
                     const std::vector<diffutils::Hunk> &abTokHunks,
                     std::optional<std::vector<diffutils::Hunk>> &abByteHunks,
                     std::vector<int64_t> &abByteHunkPrefixDelta, bool strict)
      : model_(model), paths_(paths), aSource_(aSource), bSource_(bSource),
        aToks_(aToks), bToks_(bToks), aTokOff_(aTokOff), bTokOff_(bTokOff),
        abTokHunks_(abTokHunks), abByteHunks_(abByteHunks),
        abByteHunkPrefixDelta_(abByteHunkPrefixDelta), strict_(strict) {}

  /// Slice a source text by token indices using a token-to-byte offset table.
  ///
  /// Converts the half-open token interval `[startTok,endTok)` into byte
  /// offsets using `tokOff` and returns the corresponding `StringRef` of
  /// `source`. `tokOff[i]` is the starting byte offset of token `i`; the table
  /// is expected to include a final sentinel whose value is `source.size()`.
  /// Token indices and derived byte offsets are clamped to the valid
  /// table/source ranges so malformed input yields an empty or clipped slice
  /// instead of undefined behavior.
  static llvm::StringRef SliceSource(llvm::ArrayRef<size_t> tokOff,
                                     llvm::StringRef source, uint64_t startTok,
                                     uint64_t endTok);

  /// Slice the original A-side preprocessed source by token index.
  ///
  /// Uses the same token-to-byte offset semantics as `SliceSource`.
  llvm::StringRef SliceASource(uint64_t aStartTok, uint64_t aEndTok) const {
    return SliceSource(aTokOff_, aSource_, aStartTok, aEndTok);
  }

  /// Slice the edited B-side preprocessed source by token index.
  ///
  /// Uses the same token-to-byte offset semantics as `SliceSource`.
  llvm::StringRef SliceBSource(uint64_t bStartTok, uint64_t bEndTok) const {
    return SliceSource(bTokOff_, bSource_, bStartTok, bEndTok);
  }

  /// Convert a B-token range into the corresponding half-open B-byte range.
  ///
  /// The conversion is exact and fail-closed: malformed token ranges, missing
  /// sentinel offsets, reversed byte ranges, or byte spans outside the B source
  /// return `std::nullopt` instead of manufacturing a fallback range.  The
  /// returned range names bytes in the edited preprocessed stream B, not owner
  /// source-file bytes.
  std::optional<std::pair<uint64_t, uint64_t>>
  BTokenRangeToByteRange(uint64_t bTokBegin, uint64_t bTokEnd) const;

  /// Determine whether an edit hunk is entirely contained by macro argument
  /// spans.
  ///
  /// This is the safety gate for surgical macro patching: an edit may touch
  /// only tokens corresponding to macro parameters, never macro-body
  /// boilerplate such as operators, punctuation, or literal constants.  Pure
  /// insertions have no consumed A token, so they require an exact
  /// insertion-site occurrence owner. Replacements and deletions require every
  /// consumed A token in
  /// `[h.aStart,h.aEnd)` to be covered by at least one argument-like span.
  ///
  /// As a side effect, `touched[i]` is set when the hunk touches
  /// `argSpans[i]`.  Multiple spans may be marked for one token; paste and
  /// stringify metadata can describe overlapping formal-derived surfaces that
  /// share a PP token but differ by byte subrange.
  bool HunkFullyWithinArgSpans(const diffutils::Hunk &h,
                               llvm::ArrayRef<RefoldModel::PPArgSpan> argSpans,
                               llvm::MutableArrayRef<char> touched) const;

  /// Diff the raw A/B source bytes into byte-level hunks.
  ///
  /// Builds byte hunks once using the same deterministic diff/coalesce path as
  /// token hunks.  Raw byte hunks are the authoritative basis for projecting PP
  /// byte spans from A to B, avoiding ambiguity from token-level LCS anchoring.
  /// This is especially important for insert-only edits, where token-only LCS
  /// can choose any stable equivalent insertion point.
  std::vector<diffutils::Hunk> BuildByteHunksFromRawText() const;

  /// Build the prefix-summed A->B byte-length delta cache for byte hunks.
  ///
  /// Prefix deltas let later A-byte-to-B-byte projection queries account for
  /// the cumulative length drift introduced by every preceding byte hunk.  The
  /// cache is stored in the engine-owned vector supplied to the mapper
  /// constructor.
  void BuildByteHunkPrefixDeltaCache();

  /// Project an A-byte coordinate into B using lower-bound semantics.
  ///
  /// This resolves where a coordinate in the original preprocessed stream A
  /// lands in the edited stream B.  If the coordinate falls inside a deleted or
  /// replaced range, the projection snaps to the beginning of that hunk's
  /// B-side envelope; otherwise it applies the cumulative prefix delta from all
  /// preceding byte hunks.
  size_t MapAByteToBByteLowerBound(size_t aByte) const;

  /// Project an A-byte coordinate into B using upper-bound semantics.
  ///
  /// This is the inclusive counterpart to the lower-bound mapper.  Pure
  /// insertions anchored exactly at the queried A boundary are included in the
  /// projected B offset.  Bytes inside a replaced or deleted A range snap to
  /// the end of that hunk's B-side envelope.
  size_t MapAByteToBByteUpperBound(size_t aByte) const;

  /// Return the greatest B token/sentinel index whose byte offset is <= bByte.
  ///
  /// `bTokOff_` contains one offset per B token plus a final sentinel at the
  /// end of the stream.  This floor search identifies the token containing a
  /// byte offset, or the most recent token/sentinel starting before it.
  size_t BTokIndexFloor(size_t bByte) const;

  /// Return the smallest B token/sentinel index whose byte offset is >= bByte.
  ///
  /// `bTokOff_` contains one offset per B token plus a final sentinel at the
  /// end of the stream.  This ceiling search is used to recover the exclusive
  /// token-boundary of a byte-level B envelope.
  size_t BTokIndexCeil(size_t bByte) const;

  /// Map an A-byte range to the corresponding B-token envelope.
  ///
  /// Projects the half-open A-byte interval into B-byte space and then converts
  /// that B-byte envelope into a half-open B-token range.  For non-empty A
  /// ranges, pure B insertions anchored exactly at the begin/end boundaries are
  /// trimmed out because they are not part of the A range's image and must be
  /// discharged by their own insertion owner.  Zero-width ranges remain valid
  /// insertion anchors.
  std::pair<size_t, size_t>
  MapAByteRangeToBTokenEnvelope(size_t aByteBegin, size_t aByteEnd) const;

  /// Map an A-byte range to B tokens while preserving boundary insertions.
  ///
  /// This variant intentionally keeps B-side insertions that occur exactly at
  /// the projected A-range boundaries.  Callers use it only when the boundary
  /// inserted tokens belong to the range being materialized rather than to
  /// surrounding untouched text.
  std::pair<size_t, size_t>
  MapAByteRangeToBTokenEnvelopePreserveBoundaryInsertions(
      size_t aByteBegin, size_t aByteEnd) const;

  /// Map a producer-recorded PP argument span to its B-token envelope.
  ///
  /// The primary path uses the precise producer-recorded preprocessed byte span
  /// stored on the `PPArgSpan`.  If that span is missing, strict mode fails
  /// closed rather than silently downgrading to a consumer-side approximation.
  /// Non-strict runs may approximate from the A-token offset table and then use
  /// the ordinary A-byte-to-B-token envelope mapper.
  std::optional<std::pair<size_t, size_t>>
  MapAToBTokenEnvelopeByPPArgSpan(const RefoldModel::PPArgSpan &sp) const;

  /// Trim pure B-token insertions anchored at the edges of an A-token cover.
  ///
  /// Only token-level pure insertions at the exact begin/end A gaps are edge
  /// material.  Replacements and deletions consume covered A tokens and remain
  /// part of the owner envelope.  Insertions outside the current B envelope are
  /// ignored; if trimming both edges crosses, the envelope collapses to empty.
  bool TrimPureBoundaryInsertionsFromTokenEnvelope(
      uint64_t beginTok, uint64_t endTok, std::pair<size_t, size_t> &env) const;

  /// Recover the exact contiguous B-token image of an unchanged A-token range.
  ///
  /// This narrow proof exists for cases where the byte mapper collapses a
  /// surviving single-token image onto adjacent insertion boundaries.  It fails
  /// closed unless no covered A token is consumed by a replacement/deletion
  /// hunk, no pure insertion appears strictly inside the requested A cover,
  /// every projected B token has identical spelling, and the projected B tokens
  /// form one contiguous image.  Boundary insertions may locate surviving
  /// tokens but are not included in the returned envelope.
  std::optional<std::pair<size_t, size_t>>
  TryMapUnchangedATokRangeToExactContiguousBImage(uint64_t beginTok,
                                                  uint64_t endTok) const;

  /// Map a non-empty A-token cover to a B-token envelope.
  ///
  /// Converts the A-token cover into an A-byte range, projects that range
  /// through the raw byte mapper, then finishes the projection in token space.
  /// Pure B-token insertions anchored exactly at the left or right A-token
  /// boundary are not part of this non-empty cover's image; they must be
  /// handled by their own insertion owner or by the explicit preserve-boundary
  /// variant.
  std::optional<std::pair<size_t, size_t>>
  MapATokRangeAToBTokenEnvelope(uint64_t beginTok, uint64_t endTok) const;

  /// Map an A-token cover while preserving boundary insertions.
  ///
  /// Converts the A-token cover into an A-byte range and then uses the
  /// boundary-preserving envelope mapper so B-side insertions at the projected
  /// begin/end boundaries stay attached to the mapped token envelope.
  std::optional<std::pair<size_t, size_t>>
  MapATokRangeAToBTokenEnvelopePreserveBoundaryInsertions(
      uint64_t beginTok, uint64_t endTok) const;

  /// Map a whole-cover A-token replacement interval to its B-token envelope.
  ///
  /// Whole-cover mapping intentionally preserves pure-insertion payloads
  /// anchored exactly at the A-range boundaries.  Whole-cover macro replacement
  /// wants the full B-side image of the cover; later claim clipping is
  /// responsible for preventing duplicate emission of standalone boundary
  /// insertions.
  std::optional<std::pair<size_t, size_t>>
  MapATokRangeAToBTokenEnvelopeWholeCover(uint64_t beginTok,
                                          uint64_t endTok) const;

  /// Map an A-token cover and trim edge pure insertions from the B envelope.
  ///
  /// Byte-level envelope mapping can legally absorb a pure-insertion hunk
  /// anchored exactly at `beginTok` or `endTok`.  For whole-cover macro
  /// replacement those edge insertions are outside the macro cover and may be
  /// applied separately; including them here would duplicate the insertion
  /// material.  This helper enforces the deterministic no-edge-absorption rule
  /// for coverage intervals. It should not be used for argument-content spans
  /// where boundary insertion material may be semantically part of the
  /// argument.
  std::optional<std::pair<size_t, size_t>>
  MapATokRangeAToBTokenEnvelopeTrimEdgeInsertions(uint64_t beginTok,
                                                  uint64_t endTok) const;

  /// Resolve the source-file byte start offset for one A-side PP coordinate.
  ///
  /// The producer model records PP-to-`(file,byte-range)` facts.  This helper
  /// is deliberately exact-only: a PP coordinate that has no tokmap entry, or
  /// whose tokmap entry names a different file, returns `std::nullopt` instead
  /// of projecting to a synthetic EOF anchor.  EOF insertions are admissible
  /// only through explicit include-anchor proof or a wider declared proof
  /// class.  Path equality is checked via `RefoldPathIdentity`, not raw string
  /// compare.
  std::optional<uint64_t> ByteStartForPPInFile(llvm::StringRef file,
                                               uint64_t pp) const;

  /// Resolve the source-file byte end offset for one A-side PP coordinate.
  ///
  /// Analogous to `ByteStartForPPInFile()`, but returns the mapped exclusive
  /// byte end.  This helper is exact-only; an unmapped PP coordinate must not
  /// manufacture an EOF byte anchor.
  std::optional<uint64_t> ByteEndForPPInFile(llvm::StringRef file,
                                             uint64_t pp) const;

private:
  /// Return the exact A-side occurrence that owns the pure-insertion gap.
  ///
  /// A pure insertion gap may be owned by an occurrence when it lies strictly
  /// inside that occurrence, sits on the separator token immediately before a
  /// right-hand occurrence, or matches an occurrence end and no right-hand
  /// separator owner takes precedence.  The returned index names the uniquely
  /// owning entry in `argSpans`; ambiguous or unowned gaps fail closed.
  std::optional<size_t> FindExactOwningArgSpanForPureInsertion(
      uint64_t aPos, llvm::ArrayRef<RefoldModel::PPArgSpan> argSpans) const;

  const RefoldModel &model_;
  const RefoldPathIdentity &paths_;
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
