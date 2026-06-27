//===--- RefoldPatchTypes.h --------------------------------------*- C++ -*-===//
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

struct PasteArgEdit {
  uint32_t argIdx;
  std::string newSeg;
  std::string oldSeg;
  std::optional<uint32_t> argByteBegin;
  std::optional<uint32_t> argByteEnd;

  PasteArgEdit(uint32_t Idx, std::string New, std::string Old,
               std::optional<uint32_t> ArgByteBegin = std::nullopt,
               std::optional<uint32_t> ArgByteEnd = std::nullopt)
      : argIdx(Idx), newSeg(std::move(New)), oldSeg(std::move(Old)),
        argByteBegin(ArgByteBegin), argByteEnd(ArgByteEnd) {}
};

struct MacroPatch {
  uint64_t invStart = 0, invEnd = 0;
  std::string replacement;

  MacroPatch() = default;
  MacroPatch(uint64_t invStart, uint64_t invEnd, std::string replacement,
             uint64_t macroId = 0)
      : invStart(invStart), invEnd(invEnd),
        replacement(std::move(replacement)), macroId(macroId) {}

  // Canonical macro invocation id for this physical callsite patch. This is
  // used only for statistics attribution; the patch itself is still keyed by
  // byte span and owner.
  uint64_t macroId = 0;

  // Proof-lattice summary. The summary is rebuilt from the canonical
  // MacroPatchProof carrier, not from path-local mirror fields.
  ProofSummary proofSummary = {};

  // Canonical MacroPatch-local proof carrier. All macro-local theorem facts
  // live here and are copied into ProofSummary only through
  // ClassifyMacroPatchProof().
  MacroPatchProof proof = {};

  // Selected-result bridge. Macro candidate discovery returns a concrete
  // MacroPatch for replacement-byte ownership, and the final macro selector
  // records the exact AcceptedResultCandidate that won lattice selection here
  // before the patch can be forwarded to emission.
  std::optional<AcceptedResultCandidate> selectedAcceptedCandidate;

  // First-class macro whole-cover realization certificate. These fields record
  // the exact owner cover, containment witness, and B-side token-envelope
  // accounting used to justify realized whole-cover output.
  bool wholeCoverUsedBodyRange = false;
  bool wholeCoverSelfContained = false;
  bool wholeCoverAdjustedLeft = false;
  bool wholeCoverAdjustedRight = false;
  bool wholeCoverClaimsClipped = false;
  uint64_t wholeCoverALo = 0;
  uint64_t wholeCoverAHi = 0;
  uint64_t wholeCoverBRawLo = 0;
  uint64_t wholeCoverBRawHi = 0;
  uint64_t wholeCoverBAdjLo = 0;
  uint64_t wholeCoverBAdjHi = 0;

  // Layer-5 subtree-composition audit metadata. This is instrumentation only
  // and does not participate in admissibility yet.
  bool subtreeCertBacked = false;
  uint64_t subtreeLeafMacroId = 0;
  uint32_t subtreeWitnessCount = 0;
  uint32_t subtreeInvocationCertCount = 0;
  uint32_t subtreeFormalCertCount = 0;
  uint32_t subtreeArgCertCount = 0;
  uint32_t subtreeLiftChainCount = 0;
  uint32_t subtreeLiftStepCount = 0;
  uint32_t subtreeRootMergeCount = 0;
  bool subtreeUsesLexicalBridge = false;
  bool subtreeTouchesPaste = false;
  bool subtreeHasWrapperSemantics = false;
  bool subtreeHasStringifySemantics = false;
  bool subtreeHasWideStringifySemantics = false;
  bool subtreeHasPreferredChildSyntax = false;
  bool subtreeHasRawInvocationPreservation = false;
  bool subtreeHasPassthroughFlatten = false;
  bool subtreeHasBridgeSensitiveStructuredSemantics = false;
  bool subtreeDeferredPasteDischarged = false;
  bool subtreeAdmissible = false;
  uint32_t subtreeExpectedRootFormalCount = 0;
  uint32_t subtreeDeferredRootArgCount = 0;
  uint32_t subtreeBridgeSensitiveFormalCount = 0;
  std::string subtreeExpectedRootFormalSummary;
  std::string subtreeDeferredRootArgSummary;
  std::string subtreeBridgeSensitiveFormalSummary;

  // Direct args-only paste replay proof metadata.
  bool pasteReplayValidated = false;

  // B-token envelope that corresponds to the materialized B-side surface for
  // this physical callsite patch.
  bool hasMaterializedBTokenRange = false;
  uint64_t materializedBTokStart = 0;
  uint64_t materializedBTokEnd = 0;

  // Optional byte range inside `replacement` that is the output-side surface
  // corresponding to the materialized B witness.
  bool hasMaterializedOutputByteRange = false;
  uint64_t materializedOutputByteStart = 0;
  uint64_t materializedOutputByteEnd = 0;

  // Layer-6 mixed-owner decomposition certificate metadata.
  bool ownerCertPresent = false;
  bool ownerMixedWitness = false;
  uint8_t ownerKindCode = 0; // 0=unknown, 1=TU, 2=Include
  uint64_t ownerIncludeIdCert = 0;
  bool ownerHasCondArmCert = false;
  uint64_t ownerCondArmIdCert = 0;
  uint32_t ownerWitnessCount = 0;
};

struct IncludePatch {
  const RefoldModel::IncludeItem *include;
  std::string insertBytes; // exact B bytes
  uint64_t aStart, aEnd;   // A-token interval inside include expansion
  uint64_t bStart, bEnd;   // B-token interval

  // Internal working summary for include-owned patch candidates. Include
  // patches are created before materialization chooses a concrete preserving
  // anchor or realization envelope, so pre-materialization patches must not
  // claim a normalized accepted path yet.
  ProofSummary proofSummary = {};

  /// When present, this insertion was classified as belonging to a specific
  /// selected conditional arm inside the owning include. Include application
  /// must preserve that ownership and never anchor the insertion outside the
  /// certified arm body.
  bool ownerHasCondArmCert = false;
  uint64_t ownerCondArmIdCert = 0;

  /// Some include-local layout repairs are already proved as source-byte edits
  /// before the generic include patch applicator runs. Keep the PP-token A/B
  /// range as the proof envelope, but do not ask the generic mapper to recover a
  /// different byte range from that envelope: the layout theorem has already
  /// chosen the exact header bytes that must be replaced.
  bool hasDirectHeaderByteRange = false;
  uint64_t directHeaderByteBegin = 0;
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
               ownerHasCondArmCert ? std::to_string(ownerCondArmIdCert)
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

struct IncludeEdits {
  const RefoldModel::IncludeItem *include;
  std::vector<IncludePatch> patches;

  explicit IncludeEdits(const RefoldModel::IncludeItem *item) : include(item) {}

  void Add(IncludePatch &&P) { patches.push_back(std::move(P)); }
};

} // namespace refold
} // namespace clang

#endif // LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDPATCHTYPES_H
