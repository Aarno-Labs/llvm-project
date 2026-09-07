//===--- RefoldCandidateTypes.h ---------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// Accepted-result candidate carriers for clang-refold.
//
// `ProofSummary` and `AcceptedResultCandidate` -- the records a selector
// actually ranks -- together with the per-dimension witnesses they carry for
// line-control observers, `__COUNTER__` state, generated-callee replay,
// variadic comma elision, and zero-token boundaries, and the selection
// carriers returned by the accepted-result and macro-local selectors.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDCANDIDATETYPES_H
#define LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDCANDIDATETYPES_H

#include "proof/RefoldAcceptancePathTypes.h"
#include "proof/RefoldAnchorWitnessTypes.h"
#include "proof/RefoldCompletenessTypes.h"
#include "proof/RefoldProofDischargeTypes.h"
#include "proof/RefoldTheoremTypes.h"
#include "proof/RefoldTilingWitnessTypes.h"

#include "line-control/FinalLineControlModel.h"
#include "proof/RefoldProofVocabulary.h"

#include "llvm/ADT/StringRef.h"
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

namespace clang {
namespace refold {

using llvm::StringRef;

/// \brief Common proof-summary carrier used during the proof/lattice model.
///
/// The summary packages the construction inventory, local discharge result,
/// lattice law, completeness contract, explicit theorem-domain position, and
/// canonical emitted proof for one accepted artifact.  Compatibility
/// construction fields remain available to construction sites, but
/// theorem-facing code must consume `emittedProof` rather than re-deriving
/// proof authority from AcceptedProofClass or local side bits.
struct ProofSummary {
  /// Final theorem class declared by the builder after construction
  /// provenance has been normalized.  This field, not AcceptedProofClass, is
  /// the summary-local answer to "why is this valid?".
  TheoremProofClass theoremClass = TheoremProofClass::Unknown;

  AcceptedProofClass acceptedClass = AcceptedProofClass::Unknown;
  RealizationMode realizationMode = RealizationMode::Unknown;
  SelectionPreference preference = SelectionPreference::Unknown;
  SurfaceDisposition surfaceDisposition = SurfaceDisposition::None;
  TheoremSelectionTieBreakerKind selectionTieBreaker =
      TheoremSelectionTieBreakerKind::Unknown;
  AcceptancePathInventory inventory;
  CompletenessContract completeness;
  TheoremDomainContract theoremDomain;
  ProofDischargeRecord discharge;
  bool structurePreserving = false;
  uint64_t proofRootMacroId = 0;
  bool hasTUAnchorWitness = false;
  TUAnchorWitness tuAnchorWitness;
  bool hasIncludeAnchorWitness = false;
  IncludeAnchorWitness includeAnchorWitness;
  // shared owner-realization proof carrier.  Realized macro, include, and TU
  // paths now surface through this single theorem-facing witness;
  // owner-specific input metadata remains local to the spelling machinery
  // instead of appearing as separate proof/audit families.
  bool hasOwnerRealizationWitness = false;
  OwnerRealizationWitness ownerRealizationWitness;

  // Durable structural hunk-tiling proof. A candidate produced by either a
  // historical mixed-realizer split or a preserved-structure split carries
  // the whole ordered path here, not merely its emitted token edge.
  bool hasMixedOwnerTilingWitness = false;
  StructuralHunkTilingWitness mixedOwnerTilingWitness;
  /// Exact token-edge selection within `mixedOwnerTilingWitness` for this
  /// accepted artifact.  The full witness proves the original hunk partition;
  /// this binding identifies which emitted segment the current carrier owns so
  /// the final byte-edit audit can check complete segment participation and
  /// preserved-gap disjointness against the actual emitted byte range.
  bool hasMixedOwnerTilingSegmentSelection = false;
  uint32_t mixedOwnerTilingSegmentIndex = 0;

  // State-stabilization witness carried by proof summaries that discharge a
  // suffix/state theorem directly rather than through owner realization.
  // EmittedProof exposes the same witness slot so the remaining MacroPatch
  // mirror fields can be collapsed into one macro-proof carrier.
  bool hasSuffixStabilityWitness = false;
  SuffixStabilityWitness suffixStabilityWitness;

  std::optional<TerminalFallbackWitness> terminalFallbackWitness;

  /// Canonical theorem-facing proof produced by FinalizeProofSummary().
  /// A disengaged optional means the current construction inventory is still
  /// transitional or failed to discharge the theorem obligations.  Keeping
  /// this cached in the summary prevents later selector/emission code from
  /// treating the construction inventory as an independent proof authority.
  std::optional<EmittedProof> emittedProof;

  bool HasCanonicalEmittedProof() const {
    return emittedProof && emittedProof->HasFinalTheoremClass();
  }

  /// True when the summary's primary proof class came from a concrete
  /// accepted-path or patch-proof enum rather than from inferred legacy side
  /// bits. Proof closure requires theorem-facing emitted results to carry
  /// exactly one explicit primary proof class; implicit legacy classification
  /// is therefore allowed only for internal diagnostics and is rejected before
  /// emission.
  bool primaryProofClassExplicit = false;
};

/// \brief Normalized artifact kind carried by an accepted result.
///
/// Accepted macro/include/TU/terminal results are wrapped in one uniform
/// candidate shape before competition. Converted selection sites consume that
/// shape directly; path-specific callers must populate the same proof summary
/// so they can be audited against the global selection law.
#define REFOLD_ACCEPTED_RESULT_CANDIDATE_KIND_LIST(REFOLD_X)                   \
  REFOLD_X(Unknown)                                                            \
  REFOLD_X(MacroPatch)                                                         \
  REFOLD_X(IncludePatch)                                                       \
  REFOLD_X(TUAnchor)                                                           \
  REFOLD_X(TUTextEdit)                                                         \
  REFOLD_X(TerminalOutOfDomain)

enum class AcceptedResultCandidateKind : uint8_t {
#define REFOLD_X(name) name,
  REFOLD_ACCEPTED_RESULT_CANDIDATE_KIND_LIST(REFOLD_X)
#undef REFOLD_X
};

inline StringRef toString(AcceptedResultCandidateKind value) {
  switch (value) {
#define REFOLD_X(name)                                                         \
  case AcceptedResultCandidateKind::name:                                      \
    return #name;
    REFOLD_ACCEPTED_RESULT_CANDIDATE_KIND_LIST(REFOLD_X)
#undef REFOLD_X
  }
  return "Unknown";
}
#undef REFOLD_ACCEPTED_RESULT_CANDIDATE_KIND_LIST

/// \brief Explicit witness state for line-control and builtin-location
/// preservation.
///
/// This carrier moves logical line/file/file-name observer facts into the
/// shared witness vocabulary.  It is deliberately a projection of
/// producer/state-graph facts that already exist on accepted candidates; it
/// does not rescan source text or infer #line semantics from spelling.
struct LineControlObserverWitness {
  bool observesLineNumber = false;
  bool observesFileState = false;
  bool observesFileName = false;

  uint32_t lineControlEventCount = 0;
  uint32_t activeLineControlEventCount = 0;
  uint32_t inactiveLineControlEventCount = 0;
  uint32_t producerProvenLineControlEventCount = 0;
  uint32_t missingOperandLineControlEventCount = 0;
  uint32_t unknownOperandLineControlEventCount = 0;

  uint32_t builtinLocationObservationCount = 0;
  uint32_t builtinLineObservationCount = 0;
  uint32_t builtinFileObservationCount = 0;
  uint32_t builtinFileNameObservationCount = 0;

  uint32_t sourceAuthoredLineDirectiveCount = 0;
  uint32_t producerEmittedLineDirectiveCount = 0;
  uint32_t includeReturnResyncCount = 0;
  uint32_t syntheticResyncCount = 0;

  bool physicalLayoutKnown = false;
  bool physicalLayoutStable = false;
  bool hasSourceLineControlState = false;
  bool hasBuiltinLocationObservers = false;
  bool hasSuffixLineControlDischarge = false;

  std::string stateSignature;
  std::string observerSignature;
  std::string layoutSignature;

  bool Empty() const {
    return !observesLineNumber && !observesFileState && !observesFileName &&
           lineControlEventCount == 0 && builtinLocationObservationCount == 0 &&
           !hasSuffixLineControlDischarge;
  }
};

/// \brief Explicit witness state for `__COUNTER__` preservation.
///
/// Counter sequencing is an ordinary equivalence-key dimension.
/// The witness is a projection of already-proven producer/state facts: it
/// names concrete counter events when available, records whether the accepted
/// repair preserves or materializes the counter suffix state, and keeps the
/// B-side literal value surface separate from ordinary token equivalence.
struct CounterStateWitness {
  bool observesCounter = false;
  bool hasCounterEvents = false;
  bool counterOrderKnown = false;
  bool suffixStateStable = false;
  bool coversAllAffectedObservers = false;
  bool literalizationStable = false;
  bool materializationStable = false;
  bool suffixUnobserved = false;
  bool hasExpectedBValues = false;
  bool hasMissingExpectedBValues = false;

  uint32_t counterConsumptionCount = 0;
  uint32_t counterObservationCount = 0;
  uint32_t counterMutationCount = 0;
  uint32_t preservedSuffixObserverCount = 0;
  uint32_t expectedBValueCount = 0;
  uint32_t missingExpectedBValueCount = 0;

  std::string consumptionSignature;
  std::string orderSignature;
  std::string suffixObserverSignature;
  std::string suffixValueSignature;

  bool Empty() const {
    return !observesCounter && !hasCounterEvents && !suffixStateStable &&
           counterConsumptionCount == 0 && preservedSuffixObserverCount == 0;
  }
};

/// \brief Producer-path evidence for generated-callee / higher-order replay.
///
/// Generated-callee proofs are not ordinary forwarding even when their final
/// emitted source edit is a root invocation argument rewrite.  The proof
/// follows a deterministic generated-call chain, optionally through
/// object-like aliases, then replays the final callee replacement list as a
/// producer-aware transducer.  Decoded string-literal payloads are recorded
/// as comparison evidence only; they are never treated as replacement text
/// unless the path also carries an explicit stringification producer.
struct GeneratedCalleeReplayWitness {
  uint64_t rootMacroId = 0;
  uint64_t finalDirectiveId = 0;
  uint32_t generatedCallDepth = 0;
  uint32_t objectAliasHops = 0;
  bool calleeChainDeterministic = false;
  bool replacementReplayValidated = false;
  bool solvedActualsMappedToRoot = false;
  bool usesForwarding = false;
  bool usesStringification = false;
  bool usesPaste = false;
  bool usesVariadicForwarding = false;
  bool usesObjectAlias = false;
  bool decodedStringLiteralEvidenceOnly = false;
};

/// \brief Producer-path evidence for variadic / comma-elision replay.
///
/// Variadic macro repairs are not ordinary forwarding. The same final token
/// stream can arise by absorbing text into a fixed formal, by making an
/// omitted pack explicit, by deleting or inserting the source-level separating
/// comma, or by activating/deactivating an `__VA_OPT__` payload.  This witness
/// records those distinctions so the common equivalence key never merges
/// missing, empty, comma-elided, and VA_OPT-mediated repairs by source spelling
/// alone.
struct VariadicCommaWitness {
  uint64_t rootMacroId = 0;
  uint32_t variadicFormalIndex = 0;
  bool arityStable = false;
  bool originalMissing = false;
  bool originalExplicitEmpty = false;
  bool originalNonEmpty = false;
  bool resultMissing = false;
  bool resultExplicitEmpty = false;
  bool resultNonEmpty = false;
  bool literalCommaInActual = false;
  bool commaInserted = false;
  bool commaDeleted = false;
  bool gnuCommaElision = false;
  bool vaOptPresent = false;
  bool vaOptOriginallyActive = false;
  bool vaOptResultActive = false;
  bool vaOptCommaIntroduced = false;
  bool vaOptCommaDeleted = false;
  uint32_t vaOptNodeCount = 0;
  uint32_t vaOptIncludedCount = 0;
  std::string producerSignature;
  std::string packStateSignature;
};

/// \brief Producer-path evidence for zero-token insertion and boundary-gap
/// repairs.
///
/// A zero-token repair is not justified by nearest-token guessing.  The
/// anchor must name a producer-proven owner boundary, and the witness records
/// whether layout, preserved observers, and counter state are known stable.
/// Different zero-width anchors remain non-equivalent unless these semantic
/// dimensions match.
struct ZeroTokenBoundaryWitness {
  uint64_t ownerId = 0;
  std::string ownerKind;
  bool hasPPGap = false;
  uint64_t ppGap = 0;
  bool hasSourceAnchor = false;
  uint64_t sourceAnchor = 0;
  bool hasBTokenRange = false;
  uint64_t bTokStart = 0;
  uint64_t bTokEnd = 0;
  bool producerProven = false;
  bool ownerClosed = false;
  bool layoutStable = false;
  bool observersStable = false;
  bool counterStable = false;
  bool fromEmptyActual = false;
  bool fromReplacementGap = false;
  bool fromPairedInsertion = false;
  bool fromTUAnchor = false;
  bool fromIncludeBoundary = false;
  bool fromDirectiveLayoutGap = false;
  std::string boundarySignature;
};

/// \brief Normalized wrapper for a concrete accepted result.
///
/// The carrier stays intentionally small and explicit. It holds the
/// normalized proof summary plus enough artifact-local provenance for the
/// Patch-B selection sites to compare accepted outcomes through the lattice
/// without rebuilding path-specific ordering logic.
struct AcceptedResultCandidate {
  AcceptedResultCandidateKind kind = AcceptedResultCandidateKind::Unknown;
  ProofSummary proofSummary = {};

  // enumeration of the concrete emission surface(s) represented by this
  // candidate. This is inventory only: proof validity still comes from
  // ProofSummary::emittedProof / TheoremProofClass.
  EmissionPathInventory emissionPaths = {};

  // Artifact-local span / owner metadata.
  uint64_t begin = 0;
  uint64_t end = 0;
  bool hasOwnerIncludeId = false;
  uint64_t ownerIncludeId = 0;
  bool hasRootMacroId = false;
  uint64_t rootMacroId = 0;
  bool hasAnchorByte = false;
  uint64_t anchorByte = 0;

  /// Rendering of the selected surface.
  ///
  /// This field is NOT trace-only, despite what it looks like, and the
  /// difference is load-bearing per candidate kind:
  ///
  ///   * a macro or include carrier stores a clipped, whitespace-escaped
  ///     rendering, which is only ever displayed;
  ///   * a direct-TU carrier stores the exact replacement bytes, because the
  ///     emission audit in `RefoldTextEditAssembler` reconstructs the
  ///     carrier's proven source surface from this field, and the macro-state
  ///     repair planner compares it against the edit text it authorized.
  ///
  /// So never clip what a TU carrier puts here, and never compare this field
  /// to decide whether two candidates emit the same repair: for the kinds
  /// that clip, two different surfaces can render identically. Compare
  /// `emittedRepair` for that question.
  bool hasPayloadPreview = false;
  std::string payloadPreview;

  /// Identity of the concrete source repair this candidate will emit, when
  /// the builder knows it.
  ///
  /// This is proof-facing.  The witness resolver compares it to decide
  /// whether two complete proof certificates describe one source repair or
  /// two, and that decision is what lets strict mode treat the disagreement
  /// as proof-certificate ambiguity rather than failing closed.  It must
  /// therefore be exact: the bytes as they will be emitted, not a rendering
  /// of them.
  ///
  /// A builder that does not know the emitted bytes leaves this disengaged,
  /// which denies the candidate a repair identity and fails closed.  That is
  /// the conservative direction and is deliberate -- an unknown repair is not
  /// evidence that two certificates agree.
  std::optional<EmittedRepairIdentity> emittedRepair;

  // witness-equivalence surface for structure-preserving macro actual repair.
  // These fields are copied from MacroPatch only after theexisting macro
  // proof has already validated a whole-envelope replay.  They are proof
  // facts, not selection preferences: the resolver may use them to decide
  // whether two source spellings occupy the same observational class, but the
  // byte spelling itself remains outside the equivalence key.
  bool hasTargetBTokenRange = false;
  uint64_t targetBTokStart = 0;
  uint64_t targetBTokEnd = 0;
  bool hasMacroActualRepairWitness = false;
  bool macroActualWholeEnvelopeReplayValidated = false;
  bool macroActualDefinitionTapeReplayValidated = false;
  bool macroActualArityStable = false;

  // generated-callee / higher-order replay facts.  These are copied from
  // MacroPatchProof only after an existing replay proof has validated the
  // generated callee chain and mapped the solved callee actuals back to the
  // root invocation.  They refine witness equivalence without changing the
  // legacy theorem class or selector ordering.
  bool hasGeneratedCalleeReplayWitness = false;
  GeneratedCalleeReplayWitness generatedCalleeReplayWitness;

  // direct stringification proof facts. These fields are populated only from
  // producer-recorded stringify spans on an already accepted
  // invocation-preserving macro patch. They are equivalence-key facts, not
  // replacement text: decoded string-literal payloads are used only to build
  // a canonical comparison signature for the proven stringification edge.
  bool hasStringificationWitness = false;
  uint64_t stringificationRootMacroId = 0;
  uint32_t stringificationSpanCount = 0;
  uint32_t stringificationArgCount = 0;
  bool stringificationWhitespaceNormalized = false;
  bool stringificationEscapedSpellingStable = false;
  std::string stringificationProducerSignature;
  std::string stringificationCanonicalPayloadSignature;

  // token-paste proof facts. These fields summarize producer-recorded
  // paste-token structure and/or an already validated paste replay. They
  // deliberately distinguish left/right/result paste obligations from
  // ordinary adjacent forwarding.
  bool hasTokenPasteWitness = false;
  uint64_t tokenPasteRootMacroId = 0;
  uint32_t tokenPasteSpanCount = 0;
  uint32_t tokenPasteTokenCount = 0;
  uint32_t tokenPastePartCount = 0;
  uint32_t tokenPasteArgPartCount = 0;
  uint32_t tokenPasteLiteralPartCount = 0;
  bool tokenPasteHasLeftProducer = false;
  bool tokenPasteHasRightProducer = false;
  bool tokenPasteResultValidated = false;
  bool tokenPasteDiagnosticSafe = false;
  std::string tokenPasteProducerSignature;
  std::string tokenPasteResultSignature;

  // variadic / comma-elision proof facts. These fields distinguish the
  // source-level pack state from the PP-token effect: missing variadic tails,
  // explicit empty tails, non-empty forwarded packs, VA_OPT comma
  // materialization, GNU comma elision, and literal commas inside the
  // variadic actual are separate equivalence dimensions.
  bool hasVariadicCommaWitness = false;
  VariadicCommaWitness variadicCommaWitness;

  // zero-token / boundary-gap proof facts.  These facts are producer-anchored
  // ownership evidence for insertions whose A-side source width is zero:
  // exact TU slots, collapsed include boundaries, empty macro actuals,
  // zero-token replacement-list gaps, and paired pure-insertion frontiers.
  // They deliberately track boundary/layout/observer stability as equivalence
  // dimensions rather than treating all zero-width anchors as
  // interchangeable.
  bool hasZeroTokenBoundaryWitness = false;
  ZeroTokenBoundaryWitness zeroTokenBoundaryWitness;

  // line-control / builtin-location observer facts.  These fields are
  // attached after the normal proof summary has been built, and are used only
  // by witness tracing / equivalence-key construction.
  bool hasLineControlObserverWitness = false;
  LineControlObserverWitness lineControlObserverWitness;

  // `__COUNTER__` observer/consumption facts.  These fields are projected
  // from typed CounterEventIdentity and SuffixStabilityWitness records.  They
  // are equivalence facts only; they do not decide whether a patch is
  // admissible or alter selector ordering.
  bool hasCounterStateWitness = false;
  CounterStateWitness counterStateWitness;
};

/// Return the theorem-facing owner-realization evidence carried by one
/// accepted result, when present.
inline std::optional<OwnerRealizationEvidenceKind>
AcceptedResultOwnerRealizationEvidence(
    const AcceptedResultCandidate &candidate) {
  const std::optional<EmittedProof> &emittedProof =
      candidate.proofSummary.emittedProof;
  if (!emittedProof || !emittedProof->ownerRealization)
    return std::nullopt;
  return emittedProof->ownerRealization->evidence;
}

/// Return whether one accepted result is an ordinary token-derived direct-TU
/// carrier.
///
/// The path name alone is insufficient because specialized repair and
/// sideband surfaces reuse the conservative TU path. The canonical
/// owner-realization evidence distinguishes `TUByteSpan` from
/// `TUSpecializedRealization` without adding a parallel path enum.
inline bool
AcceptedResultIsOrdinaryDirectTUCarrier(
    const AcceptedResultCandidate &candidate) {
  const AcceptedPathKind path =
      candidate.proofSummary.inventory.currentPath;
  const std::optional<OwnerRealizationEvidenceKind> evidence =
      AcceptedResultOwnerRealizationEvidence(candidate);
  return candidate.kind == AcceptedResultCandidateKind::TUTextEdit &&
         (path == AcceptedPathKind::TUByteSpanMappedEdit ||
          path == AcceptedPathKind::TUByteSpanConservativeEdit) &&
         evidence && *evidence == OwnerRealizationEvidenceKind::TUByteSpan;
}

/// Return whether one accepted result is an explicit specialized TU
/// realization on the requested construction path.
inline bool AcceptedResultIsSpecializedTUCarrier(
    const AcceptedResultCandidate &candidate, AcceptedPathKind requiredPath) {
  const std::optional<OwnerRealizationEvidenceKind> evidence =
      AcceptedResultOwnerRealizationEvidence(candidate);
  return candidate.kind == AcceptedResultCandidateKind::TUTextEdit &&
         candidate.proofSummary.inventory.currentPath == requiredPath &&
         evidence &&
         *evidence ==
             OwnerRealizationEvidenceKind::TUSpecializedRealization;
}

/// \brief Result returned by the accepted-result selector.
///
/// This carrier is theorem-facing: the selected candidate must already
/// normalize to one final emitted proof class. Selector-only staging objects
/// are deliberately kept out of this type so no internal macro-ranking proof
/// can be mistaken for an emitted accepted artifact.
struct SelectedAcceptedResultCandidate {
  AcceptedResultCandidate candidate;
  size_t index = 0;
};

/// \brief Macro-local selector carrier for internal macro competition.
///
/// Macro candidate ranking sometimes needs a staging proof that is valid only
/// for choosing among macro spellings, such as a nested subtree/call-chain
/// candidate whose remaining obligation is the top-level proof-root rule.
/// That selector proof is not an emitted accepted artifact. When the same
/// concrete macro patch also has an emission-normalized proof, the emitted
/// carrier is stored separately and is the only object allowed to be certified
/// onto MacroPatch::selectedAcceptedCandidate.
struct MacroSelectionCandidate {
  AcceptedResultCandidate selectorCandidate;
  std::optional<AcceptedResultCandidate> emittedCandidate;
  bool selectorOnly = false;
};

/// \brief Result returned by the macro-local selector.
///
/// The index points back to the caller-owned MacroPatch entry. The selected
/// macro carrier may have ranked by a selector-only proof, but callers must
/// certify only emittedCandidate, never selectorCandidate, onto an emitted
/// MacroPatch.
struct SelectedMacroSelectionCandidate {
  MacroSelectionCandidate candidate;
  size_t index = 0;
};

} // namespace refold
} // namespace clang

#endif // LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDCANDIDATETYPES_H
