//===--- RefoldPatchTypes.h --------------------------------------*- C++
//-*-===//
//
// Macro/include patch carrier types for clang-refold.
//
// These value types describe candidate source patches before final emission.
// They intentionally live outside RefoldEngine so proof, planning, and
// materialization services can exchange patch state without friend access or
// private nested RefoldEngine aliases.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDPATCHTYPES_H
#define LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDPATCHTYPES_H

#include "core/RefoldModel.h"
#include "proof/RefoldAcceptedResultTypes.h"
#include "util/StringUtils.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/FormatVariadic.h"

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace clang {
namespace refold {

/// One paste-argument spelling replacement inside a macro actual.
struct PasteArgEdit {
  /// Formal argument index touched by the paste segment.
  uint32_t argIdx;
  /// New source spelling bytes for the argument-local slice.
  std::string newSeg;
  /// Original source spelling bytes that `newSeg` replaces.
  std::string oldSeg;
  /// Inclusive byte offset inside the source-spelled argument, when known.
  std::optional<uint32_t> argByteBegin;
  /// Exclusive byte offset inside the source-spelled argument, when known.
  std::optional<uint32_t> argByteEnd;

  PasteArgEdit(uint32_t idx, std::string newSegText, std::string oldSegText,
               std::optional<uint32_t> argByteBegin = std::nullopt,
               std::optional<uint32_t> argByteEnd = std::nullopt)
      : argIdx(idx), newSeg(std::move(newSegText)),
        oldSeg(std::move(oldSegText)), argByteBegin(argByteBegin),
        argByteEnd(argByteEnd) {}
};

/// Half-open A-token range carried by a patch.
///
/// `[begin, end)` indexes the A-side (original-source) preprocessing token
/// stream. Used by MacroPatch to record the macro invocation interval.
struct ATokenRange {
  /// Inclusive A-token index.
  uint64_t begin = 0;
  /// Exclusive A-token index.
  uint64_t end = 0;
};

/// Conditional-arm provenance.
///
/// When `present`, the patch was classified as belonging to the specific
/// `#if`/`#elif`/`#else` arm identified by `armId`. Patch application must
/// preserve that ownership and never anchor the insertion outside the
/// certified arm body. Shared between MacroPatch (inside OwnerCertificate)
/// and IncludePatch (direct member).
struct ConditionalArmCertificate {
  /// True when `armId` names a proven conditional-arm owner.
  bool present = false;
  /// Producer conditional-arm id for the certified source owner.
  uint64_t armId = 0;
};

/// Mixed-owner decomposition certificate for a macro patch.
///
/// Records the compact owner identity for a macro patch: which owner kind
/// (TU vs. Include) and which include, plus optional conditional-arm
/// ownership and how many independent witnesses agree. When `mixedWitness`
/// is set, multiple witnesses contributed but disagreed; the patch then
/// cannot be treated as a single-owner rewrite.
struct OwnerCertificate {
  /// True when the remaining fields describe a proven owner.
  bool present = false;
  /// True when independent witnesses disagree on one compact owner identity.
  bool mixedWitness = false;
  /// Compact owner kind: 0=unknown, 1=TU, 2=Include.
  uint8_t kindCode = 0;
  /// Producer include id when `kindCode` is Include.
  uint64_t includeId = 0;
  /// Conditional-arm owner nested under the TU/include owner, when proven.
  ConditionalArmCertificate condArm;
  /// Number of independent witnesses contributing to this certificate.
  uint32_t witnessCount = 0;
};

/// Materialized B-side surface provenance for a MacroPatch.
///
/// Carries two optional ranges that pin the physical B input and replacement
/// text region that the materialization witness covers:
///   - `bTokens`: the B-token envelope corresponding to the materialized
///     B-side surface for this physical callsite patch.
///   - `output bytes`: the byte range inside the patch's `replacement` text
///     that corresponds to the materialized B witness. Most patches map their
///     whole replacement; invocation-preserving macro rewrites can replace a
///     full callsite while only the rewritten argument envelope is the surface
///     corresponding to the B materialization.
struct MaterializedSurface {
  /// True when `bTokStart`/`bTokEnd` name a valid B-token interval.
  bool hasBTokenRange = false;
  /// Inclusive B-token index of the materialized surface envelope.
  uint64_t bTokStart = 0;
  /// Exclusive B-token index of the materialized surface envelope.
  uint64_t bTokEnd = 0;
  /// True when `outputByteStart`/`outputByteEnd` name bytes in `replacement`.
  bool hasOutputByteRange = false;
  /// Inclusive byte offset inside the patch replacement text.
  uint64_t outputByteStart = 0;
  /// Exclusive byte offset inside the patch replacement text.
  uint64_t outputByteEnd = 0;
};

/// First-class macro whole-cover realization certificate.
///
/// Records the exact owner cover, containment witness, and B-side
/// token-envelope accounting used to justify realized whole-cover output.
/// Carried by a MacroPatch whose proof.kind is WholeCoverRealization.
struct WholeCoverCertificate {
  /// True when the A-side cover came from definition-body spans.
  bool usedBodyRange = false;
  /// True when the whole-cover envelope is self-contained for replay.
  bool selfContained = false;
  /// True when the left A/B token boundary was adjusted during planning.
  bool adjustedLeft = false;
  /// True when the right A/B token boundary was adjusted during planning.
  bool adjustedRight = false;
  /// True when already-claimed B-only insertion segments were clipped out.
  bool claimsClipped = false;
  /// Inclusive A-token lower bound of the whole-cover envelope.
  uint64_t aLo = 0;
  /// Exclusive A-token upper bound of the whole-cover envelope.
  uint64_t aHi = 0;
  /// Inclusive raw B-token lower bound before claim clipping/adjustment.
  uint64_t bRawLo = 0;
  /// Exclusive raw B-token upper bound before claim clipping/adjustment.
  uint64_t bRawHi = 0;
  /// Inclusive adjusted B-token lower bound used for materialized output.
  uint64_t bAdjLo = 0;
  /// Exclusive adjusted B-token upper bound used for materialized output.
  uint64_t bAdjHi = 0;
};

/// Candidate macro-owned source replacement before final emission.
struct MacroPatch {
  /// A-side token interval for the physical macro invocation being patched.
  ATokenRange invRange = {};
  /// Replacement bytes to emit for `invRange` at the macro callsite.
  std::string replacement;

  MacroPatch() = default;
  MacroPatch(uint64_t invStart, uint64_t invEnd, std::string replacement,
             uint64_t macroId = 0)
      : invRange{invStart, invEnd}, replacement(std::move(replacement)),
        macroId(macroId) {}

  /// Canonical macro invocation id for this physical callsite patch. This is
  /// used only for statistics attribution; the patch itself is still keyed by
  /// byte span and owner.
  uint64_t macroId = 0;

  /// Proof-lattice summary. The summary is rebuilt from the canonical
  /// MacroPatchProof carrier, not from path-local mirror fields.
  ProofSummary proofSummary = {};

  /// Canonical MacroPatch-local proof carrier. All macro-local theorem facts
  /// live here and are copied into ProofSummary only through
  /// ClassifyMacroPatchProof().
  MacroPatchProof proof = {};

  /// Selected-result bridge. Macro candidate discovery returns a concrete
  /// MacroPatch for replacement-byte ownership, and the final macro selector
  /// records the exact AcceptedResultCandidate that won lattice selection here
  /// before the patch can be forwarded to emission.
  std::optional<AcceptedResultCandidate> selectedAcceptedCandidate;

  /// Whole-cover A/B token realization certificate for this patch, when
  /// present.
  WholeCoverCertificate wholeCover = {};

  /// Subtree-composition audit metadata. This is instrumentation only and does
  /// not participate in admissibility yet. The proof carrier's proof.subtree is
  /// the canonical theorem-facing copy; this field is the patch-local
  /// construction record that the proof refresh synthesizes from.
  SubtreeCertificate subtree = {};

  /// Direct args-only paste replay proof metadata.
  bool pasteReplayValidated = false;

  /// Materialized B-token and replacement-text byte provenance, when certified.
  MaterializedSurface materialized = {};

  /// Owner/conditional-arm certificate for the patch surface, when proven.
  OwnerCertificate ownerCert = {};
};

/// Candidate include-owned source replacement before materialization/emission.
struct IncludePatch {
  /// Include instance that owns the candidate patch.
  const RefoldModel::IncludeItem *include;
  /// Exact B-side bytes to insert or replace within the include expansion.
  std::string insertBytes;
  /// A-token interval inside the include expansion: `[aStart, aEnd)`.
  uint64_t aStart, aEnd;
  /// B-token interval represented by `insertBytes`: `[bStart, bEnd)`.
  uint64_t bStart, bEnd;

  /// Internal working summary for include-owned patch candidates. Include
  /// patches are created before materialization chooses a concrete preserving
  /// anchor or A/B token realization envelope, so pre-materialization patches
  /// must not claim a normalized accepted path yet.
  ProofSummary proofSummary = {};

  /// Conditional-arm ownership certificate for this include patch, when known.
  ConditionalArmCertificate condArm = {};

  /// Some include-local layout repairs are already proved as source-byte edits
  /// before the generic include patch applicator runs. Keep the PP-token A/B
  /// range as the proof envelope, but do not ask the generic mapper to recover
  /// a different byte range from that envelope: the layout theorem has already
  /// chosen the exact header bytes that must be replaced.
  /// True when `directHeaderByteBegin` / `directHeaderByteEnd` are valid.
  bool hasDirectHeaderByteRange = false;
  /// Inclusive direct header source-byte replacement offset.
  uint64_t directHeaderByteBegin = 0;
  /// Exclusive direct header source-byte replacement offset.
  uint64_t directHeaderByteEnd = 0;

  std::string ToString() const {
    llvm::StringRef path;
    if (include->resolvedPath && !include->resolvedPath->empty()) {
      path = *include->resolvedPath;
    } else {
      path = stringutils::stripHeaderToken(include->target);
    }

    llvm::StringRef preview = insertBytes;
    bool truncated = false;
    if (preview.size() > 80) {
      preview = preview.take_front(80);
      truncated = true;
    }

    std::string escapedPreview = stringutils::escape(preview);

    return llvm::formatv(
               "IncludePatch{{incId={0}, path={1}, A=[{2},{3}), B=[{4},{5}), "
               "condArm={6}, directBytes={7}, insert='{8}{9}'}",
               include->id, path, aStart, aEnd, bStart, bEnd,
               condArm.present ? std::to_string(condArm.armId)
                               : std::string("(none)"),
               hasDirectHeaderByteRange
                   ? llvm::formatv("[{0},{1})", directHeaderByteBegin,
                                   directHeaderByteEnd)
                         .str()
                   : std::string("(none)"),
               escapedPreview, (truncated ? "..." : ""))
        .str();
  }
};

/// Patch bucket for one include instance.
struct IncludeEdits {
  /// Include instance whose local patches are stored in this bucket.
  const RefoldModel::IncludeItem *include;
  /// Include-local candidate patches in deterministic construction order.
  std::vector<IncludePatch> patches;

  explicit IncludeEdits(const RefoldModel::IncludeItem *item) : include(item) {}

  /// Append one include-local patch in deterministic construction order.
  void Add(IncludePatch &&p) { patches.push_back(std::move(p)); }
};

} // namespace refold
} // namespace clang

#endif // LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDPATCHTYPES_H
