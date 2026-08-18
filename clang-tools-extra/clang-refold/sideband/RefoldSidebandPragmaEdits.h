//===--- RefoldSidebandPragmaEdits.h ---------------------------*- C++ -*-===//
//
// Sideband pragma normalization and source-edit construction for clang-refold.
//
// Raw `-E -P` output can contain preserved `#pragma` directive lines. Those
// lines are directive sideband: they are printed in the replay surface but are
// not ordinary preprocessor tokens in the producer's refold-map token count.
// This module collects sideband pragma lines from the raw replay surfaces,
// annotates them with deterministic normal-token-gap anchors, filters them out
// of the ordinary token stream so the structural diff stays small, and builds
// source-edit witnesses that bind sideband differences to explicit byte
// witnesses for the proof lattice to consume.
//
// Internal canonicalization, JSON map collection, and edit-building helpers
// live in the corresponding `.cpp` because they are not consumed outside this
// module.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDSIDEBANDPRAGMAEDITS_H
#define LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDSIDEBANDPRAGMAEDITS_H

#include "proof/RefoldSidebandReplayProof.h"
#include "source/DiffAlgorithms.h"
#include "source/RefoldToken.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/JSON.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace clang {
namespace refold {

class RefoldModel;
class RefoldPathIdentity;
class RefoldProofLattice;
class RefoldStructuralHunkDispatcher;
class RefoldTUEditPlanner;
class RefoldTerminalProofSink;
class RefoldTextEditAssembler;

/// One preserved `#pragma` directive line recovered from a raw `-E -P` replay
/// surface.
///
/// Sideband pragma lines are zero-normal-token artifacts: they appear in the
/// replay text but do not contribute ordinary preprocessor tokens to the
/// producer-recorded token count.  Keeping them in a separate carrier lets the
/// structural diff treat them as zero-token source artifacts rather than
/// forcing whole-file fallback.
///
/// `text` is the verbatim slice of the replay surface (including any trailing
/// newline) and is the authoritative edit range.  `canonicalText` is the
/// normalized identity used for matching across the A and B replay surfaces.
/// `begin`/`end` are absolute byte offsets in the replay surface.
/// `normalTokenGap` is the deterministic structural anchor that records where
/// this directive appeared relative to ordinary tokens after other sideband
/// pragmas are ignored.
struct SidebandPragmaLine {
  std::string text;
  std::string canonicalText;
  uint64_t begin = 0;
  uint64_t end = 0;
  uint64_t normalTokenGap = 0;
};

/// Collect every physical `#pragma` line preserved in a raw `-E -P` replay
/// surface.
///
/// The returned vector is in ascending byte order with `begin` / `end` set to
/// the line slice (including the trailing newline when present) and
/// `canonicalText` already normalized for cross-surface matching.
std::vector<SidebandPragmaLine>
collectSidebandPragmaLines(llvm::StringRef bytes);

/// Certify each sideband pragma line with the gap in the normal, non-sideband
/// token stream at which the directive appears.
///
/// The normal-token gap is a deterministic structural anchor: identical pragma
/// texts at different positions get distinct gaps after other sideband pragmas
/// are skipped, so deletion of two textually-identical directives can be
/// distinguished by their structural neighbors.
void annotateSidebandPragmaTokenGaps(std::vector<SidebandPragmaLine> &lines,
                                     llvm::ArrayRef<PPTok> toks,
                                     llvm::ArrayRef<std::size_t> tokOff);

/// Remove every token whose byte offset lies inside a sideband pragma line.
///
/// This shrinks the ordinary token stream consumed by the structural diff so
/// that sideband pragmas remain visible to the source-edit pipeline without
/// inflating the producer-recorded token count.
void filterSidebandPragmaTokens(llvm::ArrayRef<SidebandPragmaLine> lines,
                                std::vector<PPTok> &toks,
                                std::vector<std::size_t> &tokOff,
                                std::size_t sourceSize);

/// Build proof-witness-bearing source edits for the sideband pragmas that
/// differ between the A and B replay surfaces.
///
/// The function consults the refold map JSON object to recover the
/// producer-recorded site for each pragma, validates that the edit can be
/// expressed as an owner-local source change, and emits one
/// `SidebandPragmaEdit` per accepted difference.  The return value is `true`
/// when every difference was discharged through this proof path; the function
/// fails closed when any sideband shape escapes the owner-local source-edit
/// domain.
bool buildSidebandPragmaSourceEdits(
    const llvm::json::Object &rootJson, llvm::StringRef refoldMapPath,
    llvm::ArrayRef<SidebandPragmaLine> aLines,
    llvm::ArrayRef<SidebandPragmaLine> bLines, llvm::ArrayRef<PPTok> rawAToks,
    llvm::ArrayRef<std::size_t> rawATokOff, llvm::StringRef bBytes,
    llvm::ArrayRef<PPTok> rawBToks, llvm::ArrayRef<std::size_t> rawBTokOff,
    std::vector<SidebandPragmaEdit> &edits);

/// Validate each sideband pragma edit's proof carrier and append a source edit
/// for every TU-owned sideband to the structural hunk dispatcher.
///
/// Sideband pragma directive text appeared in the raw `.i` replay surface but
/// was removed before token-level diffing.  Each accepted edit is validated
/// through the shared sideband proof gate, classified as TU-owned vs.
/// include-owned, and lowered into a TextEdit that the dispatcher applies on
/// the TU emission path.  Header-owned sidebands without a unique include
/// owner, and any sideband whose source range falls outside the TU bytes,
/// request terminal fallback and cause the function to return false.
bool appendSidebandPragmaSourceEdits(
    llvm::ArrayRef<SidebandPragmaEdit> sidebandPragmaEdits,
    const RefoldModel &model, llvm::StringRef tuPath, llvm::StringRef tuBytes,
    const RefoldPathIdentity &pathIdentity,
    const RefoldTextEditAssembler &textEditAssembler,
    const RefoldProofLattice &proofLattice,
    const RefoldTerminalProofSink &terminalSink,
    RefoldStructuralHunkDispatcher &structuralHunkDispatcher);

/// Return whether a zero-width TU insertion lands at the include-boundary just
/// before a materialized include that owns a sideband pragma edit.
///
/// Such insertions are routed through the include-materialization path rather
/// than getting a local newline resync at the TU emission site, because the
/// sideband replay surface will reappear at the materialized header boundary.
/// When `requireVisibleReplayText` is true, the helper only matches sidebands
/// whose replay text is actually emitted at the boundary.
bool tuInsertionBeforeMaterializedInclude(
    const RefoldTUEditPlanner &planner, const RefoldModel &model,
    llvm::ArrayRef<SidebandPragmaEdit> sidebandPragmaEdits,
    const diffutils::Hunk &h, llvm::StringRef tuPath,
    const std::pair<uint64_t, uint64_t> &span, bool requireVisibleReplayText);

} // namespace refold
} // namespace clang

#endif // LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDSIDEBANDPRAGMAEDITS_H
