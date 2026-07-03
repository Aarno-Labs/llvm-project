//===--- LineControlEditHelpers.h -------------------*- C++ -*-===//
//
// Final line-control candidate bookkeeping helpers.
//
// The line-observer layout, include materializer, and text-edit assembler use
// these routines to build, shift, and deduplicate proof-carrying candidate
// records without owning source-emission or engine orchestration state.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_LINECONTROLEDITHELPERS_H
#define LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_LINECONTROLEDITHELPERS_H

#include "line-control/FinalLineControlModel.h"
#include "util/StringUtils.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringRef.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

namespace clang {
namespace refold {

/// Return true iff a physical source line spells one of the line-control
/// directive forms that can affect the logical file/line state.  This helper is
/// intentionally syntactic and conservative; it is used only to identify the
/// concrete previous directive that caused no-op suppression in the already
/// emitted final stream.
inline bool isPhysicalLineControlDirectiveLine(llvm::StringRef line) {
  line = line.trim();
  if (!line.starts_with("#"))
    return false;

  line = line.drop_front().ltrim();
  if (line.starts_with("line")) {
    line = line.drop_front(4);
    return line.empty() || line.front() == '/' ||
           stringutils::isWs(line.front());
  }

  // Clang/GCC line-marker spelling: `# 42 "file" ...`.
  return !line.empty() && line.front() >= '0' && line.front() <= '9';
}

/// Find the last concrete line-control directive line before an insertion point
/// in an already-emitted output prefix.  The returned range is in final-output
/// coordinates and includes the trailing newline when present.
inline std::optional<std::pair<uint64_t, uint64_t>>
findLastLineControlDirectiveRangeBefore(llvm::StringRef out,
                                        uint64_t insertionOffset) {
  if (insertionOffset > out.size())
    insertionOffset = out.size();

  const uint64_t lookbackLimit =
      insertionOffset > 16 * 1024 ? insertionOffset - 16 * 1024 : 0;
  uint64_t cursor = insertionOffset;
  while (cursor > lookbackLimit) {
    uint64_t contentEnd = cursor;
    uint64_t rangeEnd = cursor;
    if (contentEnd > lookbackLimit && out[contentEnd - 1] == '\n') {
      --contentEnd;
      rangeEnd = cursor;
    }

    uint64_t lineBegin = contentEnd;
    while (lineBegin > lookbackLimit && out[lineBegin - 1] != '\n')
      --lineBegin;

    llvm::StringRef line(out.data() + lineBegin,
                         static_cast<size_t>(contentEnd - lineBegin));
    if (isPhysicalLineControlDirectiveLine(line))
      return std::make_pair(lineBegin, rangeEnd);

    if (lineBegin == 0)
      break;
    cursor = lineBegin - 1;
  }

  return std::nullopt;
}

/// Build one explicit final-line-control pruning candidate for a synthetic
/// directive emitted into an intermediate text buffer.
///
/// The offsets are intentionally relative to the buffer that owns the directive
/// right now.  The edit applicator shifts them exactly once when the buffer is
/// spliced into its parent output.  This keeps candidate generation
/// deterministic despite later edit normalization, include nesting, and
/// pending-resync flushes.
inline FinalLineControlPruneCandidate makeSyntheticLineControlPruneCandidate(
    uint64_t begin, uint64_t end, FinalLineDirective::Origin origin,
    std::optional<FinalLineControlOwnerKey> owner,
    FinalLineControlObligation obligation) {
  return MakeFinalLineControlPruneCandidate(
      begin, end, origin, std::move(owner),
      /*producerProven=*/true, obligation);
}

/// Append source candidates to destination after shifting their offsets by
/// delta.
inline void appendShiftedLineControlPruneCandidates(
    std::vector<FinalLineControlPruneCandidate> &dst,
    llvm::ArrayRef<FinalLineControlPruneCandidate> src, uint64_t delta) {
  dst.reserve(dst.size() + src.size());
  for (FinalLineControlPruneCandidate candidate : src) {
    candidate.finalBegin += delta;
    candidate.finalEnd += delta;
    dst.push_back(std::move(candidate));
  }
}

/// Append final-to-source mappings after shifting their final-output offsets by
/// delta.  Source byte ranges are not changed; only the enclosing final buffer
/// coordinate changes as materialized source text is nested into a parent.
inline void appendShiftedLineControlSourceMappings(
    std::vector<FinalLineControlSourceMapping> &dst,
    llvm::ArrayRef<FinalLineControlSourceMapping> src, uint64_t delta) {
  dst.reserve(dst.size() + src.size());
  for (FinalLineControlSourceMapping mapping : src) {
    mapping.finalBegin += delta;
    mapping.finalEnd += delta;
    dst.push_back(std::move(mapping));
  }
}

inline void deduplicateLineControlSourceMappings(
    std::vector<FinalLineControlSourceMapping> &mappings) {
  CanonicalizeFinalLineControlSourceMappings(mappings);
}

/// Build a pruning candidate for a directive that was inserted into a
/// replacement buffer by LineDirectiveInserter.  The proof is exact: the new
/// text must be the old text plus one complete directive at a single stable
/// byte offset.
inline std::optional<FinalLineControlPruneCandidate>
makeInsertedSyntheticLineControlPruneCandidate(
    llvm::StringRef before, llvm::StringRef after, llvm::StringRef directive,
    FinalLineDirective::Origin origin,
    std::optional<FinalLineControlOwnerKey> owner,
    FinalLineControlObligation obligation) {
  if (directive.empty())
    return std::nullopt;
  if (after.size() != before.size() + directive.size())
    return std::nullopt;

  size_t prefix = 0;
  while (prefix < before.size() && before[prefix] == after[prefix])
    ++prefix;

  if (prefix + directive.size() > after.size())
    return std::nullopt;
  if (after.substr(prefix, directive.size()) != directive)
    return std::nullopt;

  llvm::StringRef afterTail = after.substr(prefix + directive.size());
  llvm::StringRef beforeTail = before.substr(prefix);
  if (afterTail != beforeTail)
    return std::nullopt;

  return makeSyntheticLineControlPruneCandidate(
      static_cast<uint64_t>(prefix),
      static_cast<uint64_t>(prefix + directive.size()), origin,
      std::move(owner), obligation);
}

/// Remove duplicate candidate records after composition/duplicate-edit merging.
///
/// Candidate equality is byte-range based because the final pruner matches the
/// concrete directive bytes.  Preserve compact obligation/removal proofs only
/// when duplicate generation sites agree exactly; conflicting metadata is
/// discarded fail-closed while leaving the operational candidate eligible under
/// the existing authoritative final model.
inline void deduplicateLineControlPruneCandidates(
    std::vector<FinalLineControlPruneCandidate> &candidates) {
  llvm::sort(candidates, [](const FinalLineControlPruneCandidate &lhs,
                            const FinalLineControlPruneCandidate &rhs) {
    if (lhs.finalBegin != rhs.finalBegin)
      return lhs.finalBegin < rhs.finalBegin;
    if (lhs.finalEnd != rhs.finalEnd)
      return lhs.finalEnd < rhs.finalEnd;
    return static_cast<unsigned>(lhs.origin) <
           static_cast<unsigned>(rhs.origin);
  });

  std::vector<FinalLineControlPruneCandidate> deduped;
  deduped.reserve(candidates.size());
  for (FinalLineControlPruneCandidate candidate : candidates) {
    if (!deduped.empty() && deduped.back().finalBegin == candidate.finalBegin &&
        deduped.back().finalEnd == candidate.finalEnd &&
        deduped.back().origin == candidate.origin) {
      FinalLineControlPruneCandidate &merged = deduped.back();
      if (!SameFinalLineControlOwner(merged.physicalOwner,
                                     candidate.physicalOwner))
        merged.physicalOwner = std::nullopt;
      if (!SameFinalLineControlObligationProof(merged.obligationProof,
                                               candidate.obligationProof))
        merged.obligationProof = std::nullopt;
      if (!SameFinalLineControlRemovalProof(merged.removalProof,
                                            candidate.removalProof))
        merged.removalProof = std::nullopt;
      merged.producerProven = merged.producerProven || candidate.producerProven;
      continue;
    }

    deduped.push_back(std::move(candidate));
  }

  candidates = std::move(deduped);
}

} // namespace refold
} // namespace clang

#endif // LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_LINECONTROLEDITHELPERS_H
