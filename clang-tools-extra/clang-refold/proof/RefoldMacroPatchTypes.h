//===--- RefoldMacroPatchTypes.h --------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// Macro-patch proof carriers for clang-refold.
//
// The MacroPatch-local proof vocabulary: which macro theorem justified a
// callsite rewrite, and the durable certificates behind the ones that need
// producer evidence -- paste preservation, DAG subtree preservation, call-chain
// suffixes, and recursive tuple-generated-callee replay.  These sit above the
// candidate carriers because a macro patch is normalized into an
// `AcceptedResultCandidate` before it can be selected.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDMACROPATCHTYPES_H
#define LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDMACROPATCHTYPES_H

#include "proof/RefoldAnchorWitnessTypes.h"
#include "proof/RefoldCandidateTypes.h"

#include "core/RefoldModel.h"
#include "proof/RefoldProofVocabulary.h"

#include "llvm/ADT/StringRef.h"
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace clang {
namespace refold {

using llvm::StringRef;

#define REFOLD_MACRO_PATCH_PROOF_KIND_LIST(REFOLD_X)                           \
  REFOLD_X(Unknown)                                                            \
  REFOLD_X(CounterLiteral)                                                     \
  REFOLD_X(ArgsOnlyPasteMulti)                                                 \
  REFOLD_X(ArgsOnlyPasteSingle)                                                \
  REFOLD_X(ArgsOnlyPurePasteOnly)                                              \
  REFOLD_X(ArgsOnlyStandard)                                                   \
  REFOLD_X(ArgsOnlyPairedPureInsertion)                                        \
  REFOLD_X(DirectCalleeSubstitution)                                           \
  REFOLD_X(PasteDerivedCalleeSelector)                                         \
  REFOLD_X(RecursiveTupleGeneratedCalleeReplay)                                \
  REFOLD_X(DagSubtreeRoot)                                                     \
  REFOLD_X(CallChainSuffix)                                                    \
  REFOLD_X(WholeCoverRealization)

enum class MacroPatchProofKind : uint8_t {
#define REFOLD_X(name) name,
  REFOLD_MACRO_PATCH_PROOF_KIND_LIST(REFOLD_X)
#undef REFOLD_X
};

inline StringRef toString(MacroPatchProofKind value) {
  switch (value) {
#define REFOLD_X(name)                                                         \
  case MacroPatchProofKind::name:                                              \
    return #name;
    REFOLD_MACRO_PATCH_PROOF_KIND_LIST(REFOLD_X)
#undef REFOLD_X
  }
  return "Unknown";
}
#undef REFOLD_MACRO_PATCH_PROOF_KIND_LIST

/// \brief Producer/replay evidence for paste-preserving macro proofs.
///
/// Paste-specific proof evidence lives inside the canonical MacroPatchProof
/// carrier. This witness is the durable home for the paste-specific part of
/// the proof: either the producer supplied well-formed paste-token spans, or
/// the engine replayed the pasted surface and proved that the edited output
/// is exactly reconstructed.
struct PasteWitness {
  uint64_t rootMacroId = 0;
  bool requiresProducerPasteSpans = false;
  bool replayValidated = false;
};

/// \brief Durable certificate for DAG/subtree macro preservation proofs.
///
/// These fields intentionally summarize the existing subtree audit metadata.
/// Centralizing them here lets ClassifyMacroPatchProof() consume one proof
/// object instead of a scattered collection of path-local side bits.
struct SubtreeCertificate {
  bool backed = false;
  uint64_t leafMacroId = 0;
  uint32_t witnessCount = 0;
  uint32_t invocationCertCount = 0;
  uint32_t formalCertCount = 0;
  uint32_t argCertCount = 0;
  uint32_t liftChainCount = 0;
  uint32_t liftStepCount = 0;
  uint32_t rootMergeCount = 0;
  bool usesLexicalBridge = false;
  bool touchesPaste = false;
  bool hasWrapperSemantics = false;
  bool hasStringifySemantics = false;
  bool hasWideStringifySemantics = false;
  bool hasPreferredChildSyntax = false;
  bool hasRawInvocationPreservation = false;
  bool hasPassthroughFlatten = false;
  bool hasBridgeSensitiveStructuredSemantics = false;
  bool deferredPasteDischarged = false;
  bool admissible = false;
  uint32_t expectedRootFormalCount = 0;
  uint32_t deferredRootArgCount = 0;
  uint32_t bridgeSensitiveFormalCount = 0;
  std::string expectedRootFormalSummary;
  std::string deferredRootArgSummary;
  std::string bridgeSensitiveFormalSummary;
};

/// \brief Root/callsite evidence for call-chain suffix preservation.
///
/// A call-chain suffix proof is valid only when the emitted patch is tied to
/// the same root macro invocation that owns the suffix slice. Keeping the
/// relationship in a witness object avoids future proof code having to infer
/// that relationship from unrelated MacroPatch scalar fields.
struct CallChainWitness {
  uint64_t rootMacroId = 0;
  uint64_t callsiteMacroId = 0;
};

/// \brief One terminal generated-callee actual bound to a root tuple slice.
///
/// The byte offsets are relative to the source-spelled tuple payload, not the
/// complete invocation and not the edited replacement text.  The recursive
/// tuple-generated-callee theorem requires these slices to be exact, ordered,
/// and non-overlapping before any root tuple edit can be emitted.
struct GeneratedActualRootTupleSlice {
  uint32_t generatedActualIndex = 0;
  uint32_t generatedFormalIndex = 0;
  uint32_t rootTupleFormalIndex = 0;
  uint64_t rootTuplePayloadByteBegin = 0;
  uint64_t rootTuplePayloadByteEnd = 0;
};

/// \brief Durable proof carrier for recursive tuple-generated-callee replay.
///
/// This witness is deliberately narrower than ordinary generated-callee replay.
/// It certifies that a root invocation forwards selector formals and a tuple
/// formal through producer-recorded `caller_macro_id` / `arg_refs` edges until
/// generated function-like terminals consume exact elements of that root tuple.
/// `terminalGeneratedInvocationId` is the canonical terminal anchor for the
/// proof identity; builders may additionally verify repeated or sibling
/// terminals internally, but they may set the uniqueness and validation bits
/// only after every such replay target proves the same tuple edit obligation.
struct RecursiveTupleGeneratedCalleeReplayWitness {
  uint64_t rootInvocationId = 0;
  /// Canonical terminal invocation used as the durable proof anchor.
  uint64_t terminalGeneratedInvocationId = 0;
  /// Definition directive for the canonical terminal invocation.
  uint64_t terminalCalleeDefinitionDirectiveId = 0;

  uint32_t rootCalleeFormalIndex = 0;
  uint32_t rootTupleFormalIndex = 0;

  /// True when the terminal callee selector is itself an exact element of the
  /// same root tuple that supplies the generated actuals.  In that proof shape
  /// there is no separate root selector formal, so `rootCalleeFormalIndex` and
  /// `rootTupleFormalIndex` intentionally name the same root formal.
  bool calleeSelectedFromRootTupleSlice = false;

  std::vector<GeneratedActualRootTupleSlice> actualSlices;

  bool uniquePath = false;
  bool uniqueTupleFormal = false;
  bool uniqueReplaySolution = false;
};

/// \brief Evidence that an invocation-preserving patch replayed the whole
/// macro expansion envelope, not merely one local formal span.
///
/// Some ArgsOnlyStandard builders solve the complete replacement-list replay
/// problem: literals, formals, repeated occurrences, empty slots, VA_OPT, or
/// higher-order generated callees are checked together against the edited
/// B-side envelope. Those paths may legitimately preserve the source
/// invocation even when a naive literal-body comparison would observe changed
/// downstream tokens. Keep that fact in the canonical proof carrier so the
/// final whole-cover arbitration gate can distinguish complete replay proofs
/// from local current-level formal rewrites.
struct WholeEnvelopeReplayWitness {
  uint64_t rootMacroId = 0;
  bool replayValidated = false;

  // True only for the definition-tape solver, which reparses the recorded
  // replacement list and matches literals, formals, empty variadic slots, and
  // __VA_OPT__ branch choices against the edited B envelope as one complete
  // transducer.  That stronger proof can discharge fixed-body tokens that
  // have no stable A->B token map after a variadic tail becomes empty.
  bool definitionTapeReplayValidated = false;
};

/// \brief Canonical MacroPatch-local proof carrier.
///
/// This object is the only MacroPatch-local proof authority.
/// SetMacroPatchProof() installs it, SyncMacroPatchProofSummary() refreshes
/// derived paste/subtree/call-chain witnesses, and ClassifyMacroPatchProof()
/// copies the resulting theorem facts into ProofSummary / EmittedProof.
struct MacroPatchProof {
  MacroPatchProofKind kind = MacroPatchProofKind::Unknown;
  uint64_t proofRootMacroId = 0;
  bool preservesInvocationStructure = false;

  std::optional<OwnerRealizationWitness> ownerRealization;
  std::optional<SuffixStabilityWitness> suffixStability;
  std::optional<PasteWitness> paste;
  std::optional<SubtreeCertificate> subtree;
  std::optional<CallChainWitness> callChain;
  std::optional<GeneratedCalleeReplayWitness> generatedCalleeReplay;
  std::optional<RecursiveTupleGeneratedCalleeReplayWitness>
      recursiveTupleGeneratedCalleeReplay;
  std::optional<WholeEnvelopeReplayWitness> wholeEnvelopeReplay;
  std::optional<VariadicCommaWitness> variadicCommaReplay;
  std::optional<ZeroTokenBoundaryWitness> zeroTokenBoundaryReplay;
  std::optional<CounterStateWitness> counterState;
};

struct WholeCoverPlan {
  uint64_t covLoA = 0;
  uint64_t covHiA = 0;
  bool usedBodyRange = false;
  bool selfContained = false;
  size_t rawBTokStart = 0;
  size_t rawBTokEnd = 0;
  size_t bTokStart = 0;
  size_t bTokEnd = 0;
  bool adjustedLeft = false;
  bool adjustedRight = false;
  bool claimsClipped = false;
  std::string clippedText;
};

enum class OccurrenceSupportMode {
  /// Only metadata attached directly to the current invocation may justify
  /// occurrence-consistency success. Use this for direct args-only patch
  /// construction so descendant/sibling support cannot silently substitute
  /// for missing current-invocation evidence.
  CurrentInvocationOnly,

  /// Descendant/sibling dependency paths may justify occurrence-consistency
  /// success. Use this only inside the semantic/DAG certificate pipeline,
  /// where the caller is already proving carried rewrites through the
  /// invocation graph.
  AllowGraphSupport,
};

// Counter-stabilization carrier records used by proof lattice APIs.
struct ForcedMacroPatchRequest {
  const RefoldModel::MacroInvocation *macro = nullptr;
  uint64_t aStart = 0;
  uint64_t aEnd = 0;
  CounterEventIdentity event;
};

struct CounterOccurrence {
  const RefoldModel::MacroInvocation *macro;
  uint64_t aStart;
  uint64_t aEnd;
  std::optional<uint64_t> ownerIncludeId;
  CounterEventIdentity event;
};

} // namespace refold
} // namespace clang

#endif // LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDMACROPATCHTYPES_H
