//===--- RefoldEditTypes.h --------------------------------------*- C++ -*-===//
//
// Canonical edit-emission carrier types for clang-refold.
//
// These records are intentionally data-only.  They describe byte edits,
// deferred line-state resync state, and final sidecar materialization mappings
// without depending on RefoldEngine.  Keeping them namespace-scoped lets proof,
// materialization, and text-assembly services exchange edit state through
// narrow typed APIs instead of naming private RefoldEngine nested types.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDEDITTYPES_H
#define LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDEDITTYPES_H

#include "line-control/FinalLineControlModel.h"
#include "proof/RefoldAcceptedResultTypes.h"
#include "source/RefoldPreprocessingStructureIndex.h"

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
  /// Inclusive byte offset in the edited preprocessed stream B.
  uint64_t modifiedPreprocessedBegin = 0;
  /// Exclusive byte offset in the edited preprocessed stream B.
  uint64_t modifiedPreprocessedEnd = 0;
  /// Beginning of the corresponding emitted-source byte envelope.
  uint64_t refoldedSourceBegin = 0;
  /// End of the corresponding emitted-source byte envelope.
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
  /// Filename spelling to emit in the eventual synthetic `#line` directive.
  std::string fileSpellingForDir;
  /// Optional include-instance owner that constrains the deferred flush domain.
  std::optional<uint64_t> ownerIncludeId;

  /// True when the pending synthetic resync was emitted for a concrete
  /// line-state demand and may therefore enter the fixed-point pruning
  /// candidate set. Exact deletion attempts are validated directly rather than
  /// consulting a late final-stream liveness scanner.
  bool finalLineControlPruneEligible = false;

  /// Logical file spelling proved at the original TU/source-byte rejoin offset.
  std::string resumeFileSpelling;
  /// Logical line number proved at the original TU/source-byte rejoin offset.
  size_t resumeLineNo = 0;
  /// Source byte offset where replacement rejoins untouched text.
  ///
  /// Pending flushes may occur later, after copying a prefix of that untouched
  /// text; in that case the logical state advances by the non-spliced newlines
  /// copied between this offset and the flush point.
  uint64_t resumeOffset = 0;

  /// True when the pending correction is dominated by a preserved conditional
  /// join rather than by the next textual BOL inside the selected arm. In that
  /// case the line-state repair must be emitted by the join pass at the first
  /// post-group observer, not before an intervening preprocessor directive in
  /// only the selected arm.
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
  /// Replacement text after local line-control wrapping, if any.
  std::string text;
  /// Deferred line-state repair that must be flushed by a later copy boundary.
  std::optional<PendingResync> pending;
  /// Synthetic final-line-control directives created inside `text`.
  std::vector<FinalLineControlPruneCandidate> lineControlPruneCandidates;

  ResyncOutcome(std::string t, std::optional<PendingResync> p)
      : text(std::move(t)), pending(std::move(p)) {}

  ResyncOutcome(std::string t, std::optional<PendingResync> p,
                std::vector<FinalLineControlPruneCandidate> candidates)
      : text(std::move(t)), pending(std::move(p)),
        lineControlPruneCandidates(std::move(candidates)) {}
};

/// Named proof authority for deliberately changing protected preprocessing
/// structure.
///
/// Ordinary token-derived edits carry no authority.  The exceptional values
/// enumerate the closed set of planners whose own theorem permits consuming or
/// rewriting one exact protected interval. The final emission audit validates
/// the authority class and exact physical source identity below, together with
/// producer metadata whenever the census can bind it; a path name by itself is
/// never sufficient.
enum class ProtectedSourceEditAuthorityKind : uint8_t {
  Unknown,
  MacroStateRepair,
  /// Moves a TU-visible include directive whose included subtree owns the
  /// macro-state transition being repaired.  This authority is narrower than
  /// general include materialization: it may be minted only from the exact
  /// owning-include transition already proved by the macro-state planner.
  IncludeOwnedMacroStateRepair,
  SidebandPragmaEdit,
  IncludeMaterialization,
  IncludeDirectiveRewrite,
  IncludePreservingSourceClosure,
  TUIncludeClosure,
  LineControlRepair,
  /// Re-expresses a physical header's `#pragma once` state as a synthetic macro
  /// guard when that header is inlined into the refolded TU.
  ///
  /// The operation replaces an exact `#pragma once` interval with a `#define` of
  /// the guard macro, and wraps an exact surviving `#include` interval in an
  /// `#ifndef`/`#endif` pair that preserves the original directive spelling.  It
  /// is deliberately narrower than include materialization: it may consume only a
  /// pragma or include directive, never an `#import` (which carries once
  /// semantics this authority does not model) and never a `_Pragma` operator
  /// (which the producer does not record for `once`).  Both therefore fail closed
  /// structurally rather than by convention.
  PragmaOnceGuardRewrite,
};

/// Exact protected preprocessing interval authorized for one emitted edit.
///
/// This is an emission-boundary capability, not a reconstruction hint.  It is
/// created only after a specialized planner has discharged its own semantic
/// proof. The final assembler requires an exact kind/range match with the
/// immutable preprocessing-structure index for the current physical source
/// owner. Producer metadata is retained and rechecked when the physical census
/// can bind it, but the capability does not require such a binding:
/// macro-computed include operands, comments in include operands, and repeated
/// zero-token header occurrences can all have physical spellings or occurrence
/// ownership that intentionally differ from one producer record. Stale or
/// unused capabilities are rejected.
struct ProtectedSourceEditAuthorization {
  ProtectedSourceEditAuthorityKind authority =
      ProtectedSourceEditAuthorityKind::Unknown;
  PreprocessingStructureKind structureKind =
      PreprocessingStructureKind::OtherDirective;
  PreprocessingStructureModelKind modelKind =
      PreprocessingStructureModelKind::None;
  std::optional<uint64_t> modelItemId;
  std::optional<uint64_t> ownerConditionalArmId;
  std::optional<uint64_t> conditionalGroupId;
  std::optional<uint64_t> conditionalArmId;
  uint64_t begin = 0;
  uint64_t end = 0;

  bool IsWellFormed() const {
    // Exact physical owner/kind/range identity is the mandatory capability.
    // Producer metadata is strengthening evidence when available, but it is
    // legitimately absent for scanner-recovered repeated zero-token header
    // occurrences and for physical directive spellings normalized by the
    // producer model.
    if (authority == ProtectedSourceEditAuthorityKind::Unknown || begin >= end)
      return false;
    return true;
  }

  bool operator==(const ProtectedSourceEditAuthorization &other) const {
    return authority == other.authority &&
           structureKind == other.structureKind &&
           modelKind == other.modelKind && modelItemId == other.modelItemId &&
           ownerConditionalArmId == other.ownerConditionalArmId &&
           conditionalGroupId == other.conditionalGroupId &&
           conditionalArmId == other.conditionalArmId && begin == other.begin &&
           end == other.end;
  }
};

/// One byte edit selected for final source emission.
struct TextEdit {
  /// Half-open TU source-byte range replaced by this edit.
  uint64_t start, end;
  /// Replacement bytes to splice at `[start, end)`.
  std::string text;
  /// Deferred line-state resync produced while building `text`, if any.
  std::optional<PendingResync> pending;

  /// Root macro invocation id to charge as expanded if this edit survives
  /// normalization and is applied in the final chosen refold result.
  std::optional<uint64_t> expandedMacroRootId = std::nullopt;

  /// Normalized accepted-result carriers for the non-terminal artifacts that
  /// were composed into this final emitted edit. This is structural only: it
  /// does not change emission semantics, but it makes the proof-bearing source
  /// of each emitted artifact explicit at the byte-edit boundary so a later
  /// universal proof gate can reason over the actual emitted surface.
  std::vector<std::shared_ptr<const AcceptedResultCandidate>> acceptedResults;

  /// Exact capabilities for protected preprocessing intervals intentionally
  /// changed by a specialized directive operation.  Ordinary token-derived
  /// edits leave this vector empty.  Capabilities are preserved and
  /// de-duplicated through edit normalization, then consumed by the final
  /// global emission audit.
  std::vector<ProtectedSourceEditAuthorization>
      protectedSourceAuthorizations;

  /// Final-output line-control pruning candidates carried by this edit, using
  /// byte offsets relative to this edit's replacement `text`. Only synthetic
  /// directives that a local emitter explicitly proves eligible are listed
  /// here; source-authored directives copied through materialized text remain
  /// fail-closed until the final model has producer-backed source-line-control
  /// evidence.
  std::vector<FinalLineControlPruneCandidate> lineControlPruneCandidates = {};

  /// Final-output source mappings carried by replacement text, using byte
  /// offsets relative to this edit's replacement `text`. These are present only
  /// for byte-for-byte source material threaded through a replacement, such as
  /// materialized include bodies. Replayed B payloads and synthetic text remain
  /// unmapped.
  std::vector<FinalLineControlSourceMapping> lineControlSourceMappings = {};

  /// True when this edit has one-to-one provenance from a single token-level TU
  /// hunk.
  bool isDirectTUHunkEdit = false;
  /// Token-hunk index for direct TU hunk provenance, when present.
  std::optional<uint64_t> directTUHunkIndex = std::nullopt;
  /// Beginning of the A-token interval for direct TU hunk provenance.
  std::optional<uint64_t> directTUHunkAStart = std::nullopt;
  /// End of the A-token interval for direct TU hunk provenance.
  std::optional<uint64_t> directTUHunkAEnd = std::nullopt;
  /// Beginning of the B-token interval for direct TU hunk provenance.
  std::optional<uint64_t> directTUHunkBStart = std::nullopt;
  /// End of the B-token interval for direct TU hunk provenance.
  std::optional<uint64_t> directTUHunkBEnd = std::nullopt;

  /// Raw TU byte span before lexical widening/spacing repair.
  std::optional<uint64_t> directTURawStart = std::nullopt;
  /// End of the raw TU byte span before lexical widening/spacing repair.
  std::optional<uint64_t> directTURawEnd = std::nullopt;
  /// Final TU byte span handed to the byte applicator.
  std::optional<uint64_t> directTUFinalStart = std::nullopt;
  /// End of the final TU byte span handed to the byte applicator.
  std::optional<uint64_t> directTUFinalEnd = std::nullopt;

  /// Inclusive byte offset in the edited preprocessed stream B for the
  /// materialized replacement. This is set by the producer of the final edit
  /// surface, then consumed only by the optional sidecar mapping writer at the
  /// final TU emission boundary.
  std::optional<uint64_t> materializedBByteBegin = std::nullopt;
  /// Exclusive byte offset in the edited preprocessed stream B for the
  /// materialized replacement.
  std::optional<uint64_t> materializedBByteEnd = std::nullopt;

  /// Inclusive byte offset inside this edit's replacement `text` to report as
  /// the refolded-output side of the materialized edit map.
  std::optional<uint64_t> materializedOutputTextBegin = std::nullopt;
  /// Exclusive byte offset inside this edit's replacement `text` to report as
  /// the refolded-output side of the materialized edit map.
  std::optional<uint64_t> materializedOutputTextEnd = std::nullopt;

  /// True when a producer of this edit proved that its replacement realizes no
  /// bytes of the edited preprocessed stream B at all.
  ///
  /// This is a certified fact, not the absence of one. An edit whose payload is
  /// pure preprocessor state — for example the once-guard body staged for an
  /// include occurrence the producer records as contributing zero A tokens —
  /// has no B-side materialization envelope to report, and reporting an
  /// invented one would be a fabricated correspondence. Such an edit is omitted
  /// from the materialized edit map exactly like a copied original slice, while
  /// an edit that merely never reached a certifier keeps
  /// `materializedBByteBegin`/`End` unset and still fails closed.
  bool materializesNoBPayload = false;
};

/// Replacement text produced while wrapping a materialized include together
/// with the synthetic line-control candidates that were inserted by the
/// wrapper. Candidate offsets are relative to `text`; the file-level edit
/// applicator translates them to final-output offsets only after all edit
/// normalization and pending-resync flushing has been resolved.
struct LineControlWrappedText {
  /// Wrapped replacement bytes, including any synthetic line-control text.
  std::string text;
  /// Synthetic directives inside `text`, with offsets relative to `text`.
  std::vector<FinalLineControlPruneCandidate> lineControlPruneCandidates = {};
  /// Source-line mappings inside `text`, with offsets relative to `text`.
  std::vector<FinalLineControlSourceMapping> lineControlSourceMappings = {};
};

/// Result of planning header-local edits for one include.
///
/// When `requiresIncludeRealization` is set, the caller must stop trying to
/// anchor header-local edits and realize the include directly from the edited
/// preprocessed stream B instead.
struct IncludeTextEditPlan {
  /// Header-local edits that can be applied without realizing the include.
  std::vector<TextEdit> edits;
  /// True when header-local anchoring failed closed and include realization is
  /// required instead.
  bool requiresIncludeRealization = false;
  /// Deterministic diagnostic reason explaining the realization requirement.
  std::string realizationReason;
};

} // namespace refold
} // namespace clang

#endif // LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDEDITTYPES_H
