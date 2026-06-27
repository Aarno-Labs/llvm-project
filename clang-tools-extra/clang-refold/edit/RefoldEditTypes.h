//===--- RefoldEditTypes.h --------------------------------------*- C++ -*-===//
//
// Canonical edit-emission carrier types for clang-refold.
//
// These records are intentionally data-only.  They describe byte edits,
// deferred line-state resync state, and final sidecar materialization mappings
// without depending on RefoldEngine.  Keeping them namespace-scoped lets proof,
// materialization, and text-assembly services exchange edit state through narrow
// typed APIs instead of naming private RefoldEngine nested types.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDEDITTYPES_H
#define LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDEDITTYPES_H

#include "line-control/FinalLineControlModel.h"
#include "proof/RefoldAcceptedResultTypes.h"

#include "llvm/ADT/StringRef.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace clang {
namespace refold {

/// Half-open byte mapping for one edit that survived final TU emission.
///
/// `modifiedPreprocessed*` is expressed in the edited preprocessed stream B
/// passed via `--pp-mod`. `refoldedSource*` is expressed in the final refolded
/// C source emitted to `--out`. All ranges are byte offsets and use the same
/// half-open `[begin,end)` convention as the rest of the engine.
///
/// The two sides are materialization envelopes, not necessarily byte-for-byte
/// textual correspondences. For example, a macro-argument rewrite may map a
/// whole B-side macro expansion envelope to the compact argument text in the
/// preserved source invocation that regenerates that expansion.
struct MaterializedEditMapping {
  uint64_t modifiedPreprocessedBegin = 0;
  uint64_t modifiedPreprocessedEnd = 0;
  uint64_t refoldedSourceBegin = 0;
  uint64_t refoldedSourceEnd = 0;
};

/// Represents a pending line-state resync that must be flushed at the next safe
/// beginning-of-line in copied source text.
///
/// `ownerIncludeId` keeps the deferred query in the same owner domain as the
/// edit that created the drift. Header instances may contain conditionals with
/// the same file spelling but different selected arms, so delayed `#line`
/// recovery must carry the include-instance identity through to the eventual
/// flush point.
struct PendingResync {
  std::string fileSpellingForDir;
  std::optional<uint64_t> ownerIncludeId;

  // True when the pending synthetic resync was emitted for a concrete
  // line-state demand and may therefore enter the fixed-point pruning candidate
  // set. Exact deletion attempts are validated directly rather than consulting a
  // late final-stream liveness scanner.
  bool finalLineControlPruneEligible = false;

  // Logical state proved at the original source offset where the replacement
  // rejoins untouched text. Pending flushes may occur later, after copying a
  // prefix of that untouched text; in that case the state advances by the
  // non-spliced newlines copied between resumeOffset and the flush point.
  std::string resumeFileSpelling;
  size_t resumeLineNo = 0;
  uint64_t resumeOffset = 0;

  // True when the pending correction is dominated by a preserved conditional
  // join rather than by the next textual BOL inside the selected arm. In that
  // case the line-state repair must be emitted by the join pass at the first
  // post-group observer, not before an intervening preprocessor directive in
  // only the selected arm.
  bool deferToConditionalJoin = false;

  explicit PendingResync(llvm::StringRef file,
                         std::optional<uint64_t> ownerInclude = std::nullopt,
                         bool pruneEligible = false,
                         llvm::StringRef resumeFile = llvm::StringRef(),
                         size_t resumeLine = 0, uint64_t resumeAt = 0,
                         bool deferToJoin = false)
      : fileSpellingForDir(file.str()), ownerIncludeId(ownerInclude),
        finalLineControlPruneEligible(pruneEligible),
        resumeFileSpelling(resumeFile.empty() ? file.str() : resumeFile.str()),
        resumeLineNo(resumeLine), resumeOffset(resumeAt),
        deferToConditionalJoin(deferToJoin) {}
};

/// The final text and optional pending line-state resync for an edit.
struct ResyncOutcome {
  std::string text;
  std::optional<PendingResync> pending;
  std::vector<FinalLineControlPruneCandidate> lineControlPruneCandidates;

  ResyncOutcome(std::string t, std::optional<PendingResync> p)
      : text(std::move(t)), pending(std::move(p)) {}

  ResyncOutcome(std::string t, std::optional<PendingResync> p,
                std::vector<FinalLineControlPruneCandidate> candidates)
      : text(std::move(t)), pending(std::move(p)),
        lineControlPruneCandidates(std::move(candidates)) {}
};

/// One byte edit selected for final source emission.
struct TextEdit {
  uint64_t start, end;
  std::string text;
  std::optional<PendingResync> pending;

  // Root macro invocation id to charge as expanded if this edit survives
  // normalization and is applied in the final chosen refold result.
  std::optional<uint64_t> expandedMacroRootId = std::nullopt;

  // Normalized accepted-result carriers for the non-terminal artifacts that
  // were composed into this final emitted edit. This is structural only: it does
  // not change emission semantics, but it makes the proof-bearing source of each
  // emitted artifact explicit at the byte-edit boundary so a later universal
  // proof gate can reason over the actual emitted surface.
  std::vector<std::shared_ptr<const AcceptedResultCandidate>> acceptedResults;

  // Final-output line-control pruning candidates carried by this edit, using
  // byte offsets relative to `text`. Only synthetic directives that a local
  // emitter explicitly proves eligible are listed here; source-authored
  // directives copied through materialized text remain fail-closed until the
  // final model has producer-backed source-line-control evidence.
  std::vector<FinalLineControlPruneCandidate> lineControlPruneCandidates = {};

  // Final-output source mappings carried by replacement text, using byte offsets
  // relative to `text`. These are present only for byte-for-byte source material
  // threaded through a replacement, such as materialized include bodies.
  // Replayed B payloads and synthetic text remain unmapped.
  std::vector<FinalLineControlSourceMapping> lineControlSourceMappings = {};

  // Optional provenance for a TextEdit emitted directly from a single token-level
  // TU hunk. This is deliberately not inferred for macro, include, or
  // synthesized closure edits: the closed-realization resolver may only coalesce
  // edits that still have a one-to-one token-hunk witness.
  bool isDirectTUHunkEdit = false;
  std::optional<uint64_t> directTUHunkIndex = std::nullopt;
  std::optional<uint64_t> directTUHunkAStart = std::nullopt;
  std::optional<uint64_t> directTUHunkAEnd = std::nullopt;
  std::optional<uint64_t> directTUHunkBStart = std::nullopt;
  std::optional<uint64_t> directTUHunkBEnd = std::nullopt;

  // Raw and final TU byte spans for direct TU hunk edits. raw* records the
  // immediate token-map span before lexical widening/spacing repair; final*
  // records the span actually handed to the byte applicator.
  std::optional<uint64_t> directTURawStart = std::nullopt;
  std::optional<uint64_t> directTURawEnd = std::nullopt;
  std::optional<uint64_t> directTUFinalStart = std::nullopt;
  std::optional<uint64_t> directTUFinalEnd = std::nullopt;

  // Optional B-side byte range for the materialized replacement. This is set by
  // the producer of the final edit surface, then consumed only by the optional
  // sidecar mapping writer at the final TU emission boundary.
  std::optional<uint64_t> materializedBByteBegin = std::nullopt;
  std::optional<uint64_t> materializedBByteEnd = std::nullopt;

  // Optional byte range inside `text` that should be reported as the
  // refolded-output side of the materialized edit map. Most edits map to their
  // whole replacement text. Invocation-preserving macro rewrites can replace a
  // full callsite while only the rewritten argument envelope is the source
  // surface corresponding to the B-side materialization witness.
  std::optional<uint64_t> materializedOutputTextBegin = std::nullopt;
  std::optional<uint64_t> materializedOutputTextEnd = std::nullopt;
};

/// Replacement text produced while wrapping a materialized include together
/// with the synthetic line-control candidates that were inserted by the
/// wrapper. Candidate offsets are relative to `text`; the file-level edit
/// applicator translates them to final-output offsets only after all edit
/// normalization and pending-resync flushing has been resolved.
struct LineControlWrappedText {
  std::string text;
  std::vector<FinalLineControlPruneCandidate> lineControlPruneCandidates = {};
  std::vector<FinalLineControlSourceMapping> lineControlSourceMappings = {};
};

/// Result of planning header-local edits for one include.
///
/// When `requiresIncludeRealization` is set, the caller must stop trying to
/// anchor header-local edits and realize the include directly from the edited
/// preprocessed stream B instead.
struct IncludeTextEditPlan {
  std::vector<TextEdit> edits;
  bool requiresIncludeRealization = false;
  std::string realizationReason;
};

} // namespace refold
} // namespace clang

#endif // LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDEDITTYPES_H
