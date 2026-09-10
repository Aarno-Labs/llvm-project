//===--- RefoldTextEditAssembler.h ------------------------------*- C++ -*-===//
//
// Final byte-edit assembly for clang-refold.
//
// This class owns pending #line resync flushing, accepted-result carrier
// attachment/auditing, sideband replay certifying, materialized edit-map range
// recovery, and final splice/application behavior.  All state, proof services,
// and orchestration callbacks needed for final byte assembly are explicit
// constructor dependencies.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDTEXTEDITASSEMBLER_H
#define LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDTEXTEDITASSEMBLER_H

#include "edit/RefoldEditTypes.h"
#include "edit/RefoldPatchTypes.h"
#include "edit/RefoldTUEditPlanner.h"
#include "line-control/RefoldLineControlProof.h"
#include "macro/RefoldMacroTopology.h"
#include "proof/RefoldAcceptancePathTypes.h"
#include "proof/RefoldCandidateTypes.h"
#include "proof/RefoldCompletenessTypes.h"
#include "proof/RefoldMacroPatchTypes.h"
#include "proof/RefoldOwnerStateTypes.h"
#include "proof/RefoldSidebandReplayProof.h"
#include "proof/RefoldTheoremAudit.h"
#include "proof/RefoldTheoremTypes.h"
#include "proof/RefoldTilingWitnessTypes.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace clang {

class LangOptions;

namespace refold {

class LineDirectiveInserter;
class RefoldMacroStateProof;
class RefoldMacroWholeCoverPlanBuilder;
class RefoldOwnerStateProof;
class RefoldPathIdentity;
class RefoldPreprocessingStructureIndex;
class RefoldProofLattice;
class RefoldSourceMapper;
class RefoldTUEditPlanner;
class RefoldTheoremAudit;

/// Assembles final byte edits and proof carriers after structural planning.
///
/// The assembler owns emission-order checks, pending line-control resync
/// flushing, sideband replay partitioning, materialized range certification,
/// and final accepted-result carrier attachment; it does not choose macro, TU,
/// or include candidates.
class RefoldTextEditAssembler {
public:
  using TextEdit = ::clang::refold::TextEdit;
  using PendingResync = ::clang::refold::PendingResync;
  using ResyncOutcome = ::clang::refold::ResyncOutcome;
  using AcceptedResultCandidate = ::clang::refold::AcceptedResultCandidate;
  using AcceptedResultCandidateKind =
      ::clang::refold::AcceptedResultCandidateKind;
  using MacroPatch = ::clang::refold::MacroPatch;
  using SidebandPragmaEdit = ::clang::refold::SidebandPragmaEdit;
  using LineStateObserverDemand = ::clang::refold::LineStateObserverDemand;
  using LineStateObserverSite = ::clang::refold::LineStateObserverSite;
  using OwnerSourceRange = ::clang::refold::OwnerSourceRange;
  using OwnerStateBoundary = ::clang::refold::OwnerStateBoundary;
  using OwnerStateComponent = ::clang::refold::OwnerStateComponent;
  using StateMutationKind = ::clang::refold::StateMutationKind;
  using SuffixStabilityWitness = ::clang::refold::SuffixStabilityWitness;
  using SuffixStabilityWitnessKind =
      ::clang::refold::SuffixStabilityWitnessKind;
  using AcceptanceSupportKind = ::clang::refold::AcceptanceSupportKind;
  using AcceptedPathKind = ::clang::refold::AcceptedPathKind;
  using CompletenessCoverageKind = ::clang::refold::CompletenessCoverageKind;
  using LegacyPathKind = ::clang::refold::LegacyPathKind;
  using ProofSummary = ::clang::refold::ProofSummary;
  using TheoremDomainKind = ::clang::refold::TheoremDomainKind;
  using TheoremProofClass = ::clang::refold::TheoremProofClass;
  using TerminalFallbackFailureContext =
      ::clang::refold::TerminalFallbackFailureContext;
  using TerminalFallbackFailureReason =
      ::clang::refold::TerminalFallbackFailureReason;
  using TerminalFallbackObligationKind =
      ::clang::refold::TerminalFallbackObligationKind;
  using TerminalFallbackProofFailure =
      ::clang::refold::TerminalFallbackProofFailure;

  /// Exceptional callback bundle for cycle-breaking orchestration queries.
  /// Line-resync deferral is owned by RefoldLineObserverLayout, which depends
  /// on this assembler for emitted layout edits.  Theorem/audit policy is
  /// owned by RefoldTheoremAudit.
  struct Hooks {
    std::function<bool(llvm::StringRef, std::optional<uint64_t>, uint64_t)>
        lineResyncShouldDeferToConditionalJoin;
  };

  RefoldTextEditAssembler(
      const RefoldModel &model, llvm::StringRef bSource,
      llvm::ArrayRef<PPTok> aToks, llvm::ArrayRef<PPTok> bToks,
      llvm::ArrayRef<size_t> bTokOff,
      const std::vector<diffutils::Hunk> &abTokHunks,
      const std::vector<int64_t> &abTokMapA2B,
      const std::vector<int64_t> &abTokMapB2A,
      const RefoldSourceMapper &sourceMapper,
      const RefoldPathIdentity &pathIdentity,
      const RefoldMacroStateProof &macroStateProof,
      const clang::LangOptions &lexLang,
      const RefoldPreprocessingStructureIndex &tuPreprocessingStructureIndex,
      const RefoldProofLattice &proofLattice,
      const RefoldMacroWholeCoverPlanBuilder &wholeCoverPlanBuilder,
      const RefoldOwnerStateProof &ownerStateProof,
      const RefoldMacroTopology &macroTopology,
      const RefoldLineControlProof &lineControlProof,
      const LineDirectiveInserter &lineDirs,
      const RefoldTerminalProofSink &terminalSink,
      const RefoldTUEditPlanner &tuEdits,
      const RefoldTheoremAudit &theoremAuditService,
      const std::vector<SidebandPragmaEdit> &sidebandPragmaEdits,
      const std::vector<MixedOwnerTilingWitness> &mixedOwnerTilingWitnesses,
      TheoremAuditStats &theoremAudit, Hooks hooks)
      : model_(model), bSource_(bSource), aToks_(aToks), bToks_(bToks),
        bTokOff_(bTokOff), abTokHunks_(abTokHunks), abTokMapA2B_(abTokMapA2B),
        abTokMapB2A_(abTokMapB2A), sourceMapper_(sourceMapper),
        pathIdentity_(pathIdentity), macroStateProof_(macroStateProof),
        lexLang_(lexLang),
        tuPreprocessingStructureIndex_(tuPreprocessingStructureIndex),
        proofLattice_(proofLattice),
        wholeCoverPlanBuilder_(wholeCoverPlanBuilder),
        ownerStateProof_(ownerStateProof), macroTopology_(macroTopology),
        lineControlProof_(lineControlProof), lineDirs_(lineDirs),
        terminalSink_(terminalSink), tuEdits_(tuEdits),
        theoremAuditService_(theoremAuditService),
        sidebandPragmaEdits_(sidebandPragmaEdits),
        mixedOwnerTilingWitnesses_(mixedOwnerTilingWitnesses),
        theoremAudit_(theoremAudit), hooks_(std::move(hooks)) {}

  /// Declared out of line so the emission structure-index cache can hold a
  /// forward-declared RefoldPreprocessingStructureIndex.
  ~RefoldTextEditAssembler();

  /// \brief Compute how to preserve __LINE__ after applying replacement to
  /// [start,end) in originalFileText.
  ///
  /// This method detects "line drift" by comparing the newline count in the
  /// original span versus the replacement text. If there is no drift, it
  /// returns (replacement, nullopt).
  ///
  /// If drift is detected, the method computes the logical resume line for the
  /// first character at `end` in the original file, attempts a local resync via
  /// LineDirectiveInserter::MaybeAppendResyncAfterReplacement, and otherwise
  /// returns a PendingResync so the emission layer can flush a `#line`
  /// directive at the next safe BOL.
  ///
  /// Safety note: local injection may fail when inserting a directive would
  /// change token adjacency. In that case, pending resync state is carried only
  /// because a model-recorded suffix __LINE__ observer exists; otherwise no
  /// synthetic directive is produced.
  ResyncOutcome ApplyResyncOrPend(
      llvm::StringRef originalFileText, uint64_t start, uint64_t end,
      llvm::StringRef replacement, llvm::StringRef fileSpellingForDirective,
      std::optional<uint64_t> ownerIncludeId = std::nullopt) const;

  /// \brief Attach accepted-result metadata to an emitted text edit.
  ///
  /// Copies the normalized accepted-result carrier selected by the proof
  /// lattice onto the concrete `TextEdit` that will be emitted. This preserves
  /// the accepted path, proof-discharge inventory, witnesses, and audit
  /// metadata at the byte-edit boundary so later validation/reporting can
  /// reason about the emitted edit without re-running candidate selection.
  void
  AttachAcceptedResultCarrier(TextEdit &edit,
                              const AcceptedResultCandidate &candidate) const;

  /// Authorize every complete protected interval touched by `[begin,end)` for
  /// one named specialized directive operation.
  ///
  /// The interval census is rebuilt in the exact physical source-owner domain
  /// that will later be assembled.  A narrow specialized operation must contain
  /// the scanner-proven directive spelling for every touched interval; a
  /// source-closure operation must contain the complete lexical interval.  The
  /// final capability is bound to exact physical kind/range identity. Producer
  /// metadata is carried and rechecked whenever the physical census can bind
  /// it, but a missing producer binding does not invalidate an already-proved
  /// physical operation.
  /// Failure returns false; callers must not emit the specialized edit without
  /// this capability.  Candidate planners may set `requestTerminalOnFailure`
  /// to false so their ordinary fallback lattice remains reachable.  The final
  /// emission audit always treats an authorization failure as terminal.
  bool AuthorizeProtectedSourceIntervals(
      TextEdit &edit, ProtectedSourceEditAuthorityKind authority,
      llvm::StringRef sourcePath, std::optional<uint64_t> ownerIncludeId,
      llvm::StringRef sourceBytes, uint64_t begin, uint64_t end,
      llvm::ArrayRef<PreprocessingStructureKind> allowedKinds,
      bool requireProtectedInterval = true,
      bool requestTerminalOnFailure = true) const;

  /// Authorize one exact protected interval already identified by a
  /// specialized semantic proof.
  ///
  /// Unlike `AuthorizeProtectedSourceIntervals`, this routine does not grant
  /// authority to every protected construct touched by the surrounding edit.
  /// It locates exactly one indexed interval matching the supplied physical
  /// transition range, verifies the named authority/kind pair, and records a
  /// capability for only that interval.  This is required by include-owned
  /// macro-state repair: the planner may move the one TU include that embodies
  /// a proved header `#define` or `#undef`, while any unrelated directive in
  /// the widened edit must remain unauthorized and therefore fail closed.
  /// `allowedNestedKinds` names the closed set of independently indexed
  /// constructs that are physically contained by that exact transition and
  /// semantically inseparable from it. The current macro-state theorem uses
  /// this only for `_Pragma` operators inside a complete macro replacement.
  bool AuthorizeExactProtectedSourceInterval(
      TextEdit &edit, ProtectedSourceEditAuthorityKind authority,
      llvm::StringRef sourcePath, std::optional<uint64_t> ownerIncludeId,
      llvm::StringRef sourceBytes, uint64_t intervalBegin,
      uint64_t intervalEnd,
      llvm::ArrayRef<PreprocessingStructureKind> allowedKinds,
      llvm::ArrayRef<PreprocessingStructureKind> allowedNestedKinds = {},
      bool requestTerminalOnFailure = true) const;

  /// Authorize a complete source-closure operation after its independent
  /// source-gap theorem has proved the entire physical byte envelope.
  ///
  /// Only the two named closure authorities are accepted.  The method expands
  /// their closed domain to every indexed preprocessing kind and records one
  /// exact capability per interval; it does not itself prove source closure.
  /// `preservedSourcePieces`, when engaged, is the caller's construct-by-
  /// construct statement of which crossed source ranges its replacement
  /// carries through as preserved source.  Supplying it lets this routine
  /// record every other authorized construct as eliminated, which is what a
  /// later subsumption theorem needs and what the closure authority alone
  /// cannot say.  Leaving it disengaged makes no claim and records nothing.
  bool AuthorizeCompleteProtectedSourceClosure(
      TextEdit &edit, ProtectedSourceEditAuthorityKind authority,
      llvm::StringRef sourcePath, std::optional<uint64_t> ownerIncludeId,
      llvm::StringRef sourceBytes, uint64_t begin, uint64_t end,
      bool requireProtectedInterval = true,
      bool requestTerminalOnFailure = true,
      std::optional<llvm::ArrayRef<std::pair<uint64_t, uint64_t>>>
          preservedSourcePieces = std::nullopt) const;

  /// Return whether an ordinary token-derived edit avoids every protected
  /// preprocessing interval in its exact physical source-owner domain.
  ///
  /// This is the candidate-level form of the final global firewall. It grants
  /// no capability and never interprets an accepted path name as directive
  /// authority. Candidate planners may keep their fallback lattice reachable
  /// by leaving `requestTerminalOnFailure` false.
  bool OrdinaryEditAvoidsProtectedPreprocessingStructure(
      const TextEdit &edit, llvm::StringRef sourcePath,
      std::optional<uint64_t> ownerIncludeId, llvm::StringRef sourceBytes,
      bool requestTerminalOnFailure = false) const;

  /// \brief Audit the complete accepted-proof surface before bytes are emitted.
  ///
  /// This is the last accepted-proof gate before the applicator splices
  /// replacement text into a source file. The audit checks every normalized
  /// edit, every carrier attached to each edit, and the composition law for
  /// multi-carrier edits. `originalFileText` supplies the final physical bytes
  /// needed to distinguish a newline-terminated directive from an end-of-file
  /// directive when auditing a zero-width insertion at the interval end.
  bool AuditAcceptedEditProofs(
      llvm::ArrayRef<TextEdit> edits, llvm::StringRef emissionStage,
      llvm::StringRef emissionOwner = llvm::StringRef(),
      std::optional<uint64_t> ownerIncludeId = std::nullopt,
      llvm::ArrayRef<TextEdit> plannedEdits = {},
      llvm::StringRef originalFileText = llvm::StringRef()) const;

  /// \brief Return whether an emitted non-terminal byte edit is backed only by
  /// emission-discharged normalized accepted-result carriers.
  ///
  /// Proof discharge is the universal gate at the actual emission boundary.
  /// Every non-terminal artifact that survives to a TextEdit must carry at
  /// least one normalized accepted-result candidate, and every such carrier
  /// must already be fully discharged under the proof contract that is
  /// appropriate for emitted source text. Terminal out-of-domain results are
  /// never valid carriers for non-terminal emitted edits.
  bool EmittedTextEditHasDischargedAcceptedResults(
      const TextEdit &edit, llvm::StringRef emissionStage,
      llvm::StringRef emissionOwner = llvm::StringRef()) const;

  /// Verify the preserved-gap theorem against the normalized physical edit set
  /// rather than only against the planner's token carriers.
  ///
  /// The pre-normalization edit set must contain every token segment belonging
  /// to the current physical source owner, the final edit set must retain those
  /// exact segment carriers, and no normalized `TextEdit` may interfere with a
  /// gap recorded as `PreservedInPlace`. A zero-width insertion at the beginning
  /// or inside such an interval can extend or disable the logical directive
  /// without deleting one of its original bytes. An insertion at the physical
  /// end is also rejected when the preserved directive reaches end-of-file
  /// without a terminating newline, because that insertion extends the same
  /// logical directive line. Comparing both edit sets closes
  /// widening, merging, and conservative-closure paths that occur after token
  /// tiling and could otherwise erase the witness they invalidate.  Replacement
  /// witnesses additionally retain the unique A-to-B boundary
  /// projection proof through this final emission audit.
  bool PreservedStructuralGapsRemainOutsideEmittedEdits(
      llvm::ArrayRef<TextEdit> plannedEdits,
      llvm::ArrayRef<TextEdit> emittedEdits, llvm::StringRef emissionStage,
      llvm::StringRef emissionOwner,
      std::optional<uint64_t> ownerIncludeId,
      llvm::StringRef originalFileText) const;

  /// \brief Verify that multiple carriers on one edit compose in source order.
  ///
  /// Individual carrier normalization is not enough for a composite `TextEdit`:
  /// the carriers must either be equivalent witnesses for the same source
  /// surface or form a deterministic, gap-free ordered segment sequence in one
  /// comparable coordinate space. Until future state-gap proof extensions
  /// provide typed state-closed gap witnesses, non-empty inter-segment gaps are
  /// rejected here rather than guessed.
  bool EmittedTextEditHasOrderedAcceptedProofComposition(
      const TextEdit &edit, llvm::StringRef emissionStage,
      llvm::StringRef emissionOwner = llvm::StringRef()) const;

  /// Remove visible sideband replay bytes from an ordinary replay payload when
  /// those bytes are owned by a separate, non-insertion sideband source edit.
  ///
  /// This enforces the replay-partition invariant: B-only sideband insertions
  /// may be carried by an ordinary insertion island, but sideband replacements
  /// and deletions have their own source edit and must not be duplicated by the
  /// ordinary token-envelope replay.
  std::string StripSeparatelyOwnedSidebandReplay(
      llvm::StringRef replayText, std::optional<uint64_t> replayBByteBegin,
      std::optional<uint64_t> replayBByteEnd) const;

  /// Certify a TextEdit with the B-byte range of the materialized surface.
  void CertifyTextEditMaterializedBByteRange(TextEdit &edit, uint64_t begin,
                                             uint64_t end) const;

  /// Certify that a TextEdit realizes no bytes of the edited preprocessed
  /// stream B.
  ///
  /// Use this only where the caller has proved that the emitted replacement is
  /// pure preprocessor state with no B-side payload. The certified edit is
  /// omitted from the materialized edit map instead of contributing an invented
  /// B-byte range; edits that simply reached emission without a certifier keep
  /// failing closed.
  void CertifyTextEditMaterializesNoBPayload(TextEdit &edit) const;

  /// Certify a TextEdit with the B-byte range described by a B-token envelope.
  void CertifyTextEditMaterializedBTokenRange(TextEdit &edit,
                                              uint64_t bTokBegin,
                                              uint64_t bTokEnd) const;

  /// Certify a TextEdit with the materialized ranges witnessed by a sideband
  /// edit proof. The complete sideband proof binds the emitted replacement
  /// payload to its raw-B byte provenance, so TU sideband emission should
  /// certify those two edit-map facts through this single gate rather than as
  /// independent fields.
  void CertifyTextEditMaterializedBReplayProof(
      TextEdit &edit, const SidebandPragmaEdit &sideband) const;

  /// Return the B-byte envelope contributed by sideband pragma edits owned by
  /// one materialized include, if that sideband stream supplies such a witness.
  std::optional<std::pair<uint64_t, uint64_t>>
  SidebandPragmaMaterializedBByteRangeForInclude(uint64_t includeId) const;

  /// Certify a TextEdit with the replacement-text subrange to report on the
  /// refolded-output side of the optional materialized edit map.
  void CertifyTextEditMaterializedOutputTextRange(TextEdit &edit,
                                                  uint64_t begin,
                                                  uint64_t end) const;

  /// Return the replacement-text subrange to report for a final TextEdit.
  std::optional<std::pair<uint64_t, uint64_t>>
  TextEditMaterializedOutputTextRange(const TextEdit &edit) const;

  /// Return the replacement-text subrange to report for a macro patch.
  std::optional<std::pair<uint64_t, uint64_t>>
  MacroPatchMaterializedOutputTextRange(const MacroPatch &patch) const;

  /// Return the B-byte range carried by a final TextEdit, recovering direct TU
  /// hunk ranges from token provenance when the byte range was not
  /// pre-certified.
  std::optional<std::pair<uint64_t, uint64_t>>
  TextEditMaterializedBByteRange(const TextEdit &edit) const;

  /// Return the B-byte range for a macro patch, using its certified envelope
  /// when present and falling back to the macro's mapped expansion cover
  /// otherwise.
  std::optional<std::pair<uint64_t, uint64_t>>
  MacroPatchMaterializedBByteRange(const MacroPatch &patch) const;

  /// \brief Apply a set of TextEdits to originalFileText, producing the final
  /// refolded text for a single file while preserving __LINE__ transparency via
  /// pending resync.
  ///
  /// Core responsibilities:
  /// * normalize edits by de-duplicating exact-span edits, with the last one
  ///   winning;
  /// * order edits by increasing start/end and enforce non-overlap;
  /// * stream output in order as original slices plus replacement text; and
  /// * when an edit carries a PendingResync, defer `#line` emission until the
  ///   next safe BOL in subsequent unchanged original text, using
  ///   AppendOriginalSliceWithPending().
  ///
  /// Pending semantics: if multiple edits produce pending drift and an earlier
  /// pending resync could not be flushed yet, the most recent PendingResync
  /// wins. EOF behavior: if a pending resync remains at end-of-file, it is
  /// dropped as harmless because there is no subsequent original code whose
  /// __LINE__ needs correction.
  std::string ApplyTextEditsWithPendingResync(
      llvm::StringRef originalFileText, llvm::ArrayRef<TextEdit> edits,
      llvm::DenseSet<uint64_t> *appliedExpandedMacroRootIds = nullptr,
      llvm::StringRef emissionOwner = llvm::StringRef(),
      std::optional<uint64_t> ownerIncludeId = std::nullopt,
      std::vector<MaterializedEditMapping> *materializedEditMappings = nullptr,
      std::vector<FinalLineControlPruneCandidate> *lineControlPruneCandidates =
          nullptr,
      std::vector<FinalLineControlSourceMapping> *lineControlSourceMappings =
          nullptr) const;

  /// \brief Append an unchanged slice of the original file original[from:to)
  /// into out, while attempting to flush a previously deferred PendingResync at
  /// the earliest safe point.
  ///
  /// A pending resync represents a required logical `#line` correction that
  /// could not be emitted inside a prior replacement without risking token
  /// adjacency changes. This method flushes that directive when it becomes safe
  /// to do so while streaming unchanged original content.
  ///
  /// If no safe flush point exists within [from,to), the pending resync is
  /// returned unchanged so it can be attempted again on the next original
  /// slice.
  std::optional<PendingResync> AppendOriginalSliceWithPending(
      llvm::SmallVectorImpl<char> &out, llvm::StringRef original, uint64_t from,
      uint64_t to, std::optional<PendingResync> pending,
      llvm::StringRef emissionOwner = llvm::StringRef(),
      std::optional<uint64_t> ownerIncludeId = std::nullopt,
      std::vector<FinalLineControlPruneCandidate> *lineControlPruneCandidates =
          nullptr,
      std::vector<FinalLineControlSourceMapping> *lineControlSourceMappings =
          nullptr) const;

  /// Build a TextEdit for a direct TU hunk by routing through the TU edit
  /// planner's `BuildDirectTUHunkEditPlan` and certifying the resulting edit
  /// with materialized-byte and accepted-result carriers.
  ///
  /// This service is the single home for assembling proof-certified direct TU
  /// hunk edits; orchestration sites just supply the hunk + span + payload
  /// inputs and consume the resulting `TextEdit`.  A failed final TU-span
  /// revalidation returns `std::nullopt` so orchestration can continue to its
  /// declared fallback path.
  std::optional<TextEdit> BuildDirectTUHunkTextEdit(
      const diffutils::Hunk &h, uint64_t hunkIndex,
      const std::pair<uint64_t, uint64_t> &span, ResyncOutcome resync,
      llvm::StringRef acceptedPayload, uint64_t rawTUStart, uint64_t rawTUEnd,
      std::optional<uint64_t> materializedBByteBegin,
      std::optional<uint64_t> materializedBByteEnd,
      AcceptedPathKind acceptedPath,
      std::optional<TUInsertionAnchorAdjustment> insertionAnchorAdjustment =
          std::nullopt) const;

private:
  /// Reject a preserved-gap audit at the common theorem/fallback boundary.
  bool RejectPreservedStructuralGapAudit(llvm::StringRef emissionStage,
                                         llvm::StringRef detail) const;

  /// Return the immutable protected-structure census for the physical source
  /// owner being assembled, reusing the run-wide TU index when possible.
  ///
  /// The census is a pure function of the physical source owner, the owner
  /// include occurrence, and the original file bytes, so the returned reference
  /// names an assembler-owned immutable index that stays valid for the
  /// assembler's lifetime.  Callers must not retain it beyond that.
  const RefoldPreprocessingStructureIndex &GetEmissionStructureIndex(
      llvm::StringRef emissionOwner,
      std::optional<uint64_t> ownerIncludeId,
      llvm::StringRef originalFileText) const;

  /// Final global firewall for ordinary and specialized source edits.
  ///
  /// Path-specific planners remain the primary proof of token/source
  /// ownership.  This independent audit rechecks that direct-TU lexical
  /// widening contains only trivia plus exact capabilities and that no edit
  /// interferes with protected preprocessing structure without one exact,
  /// compatible specialized-operation authorization.
  bool AuditGlobalSourceEditInvariant(
      llvm::ArrayRef<TextEdit> edits, llvm::StringRef emissionStage,
      llvm::StringRef emissionOwner,
      std::optional<uint64_t> ownerIncludeId,
      llvm::StringRef originalFileText) const;

  const RefoldModel &model_;
  llvm::StringRef bSource_;
  llvm::ArrayRef<PPTok> aToks_;
  llvm::ArrayRef<PPTok> bToks_;
  llvm::ArrayRef<size_t> bTokOff_;
  const std::vector<diffutils::Hunk> &abTokHunks_;
  const std::vector<int64_t> &abTokMapA2B_;
  const std::vector<int64_t> &abTokMapB2A_;
  const RefoldSourceMapper &sourceMapper_;
  const RefoldPathIdentity &pathIdentity_;
  const RefoldMacroStateProof &macroStateProof_;
  const clang::LangOptions &lexLang_;
  const RefoldPreprocessingStructureIndex &tuPreprocessingStructureIndex_;
  const RefoldProofLattice &proofLattice_;
  /// Whole-cover plan computation.  Borrowed directly: the builder depends on
  /// neither the proof lattice nor the macro patch planner, so it is
  /// constructed before both and needs no late binding here.
  const RefoldMacroWholeCoverPlanBuilder &wholeCoverPlanBuilder_;
  const RefoldOwnerStateProof &ownerStateProof_;
  const RefoldMacroTopology &macroTopology_;
  const RefoldLineControlProof &lineControlProof_;
  const LineDirectiveInserter &lineDirs_;
  const RefoldTerminalProofSink &terminalSink_;
  const RefoldTUEditPlanner &tuEdits_;
  const RefoldTheoremAudit &theoremAuditService_;
  const std::vector<SidebandPragmaEdit> &sidebandPragmaEdits_;
  /// Durable structural partitions used to resolve validated TU carrier keys
  /// after later edit normalization has discarded path-local bindings.
  const std::vector<MixedOwnerTilingWitness> &mixedOwnerTilingWitnesses_;
  TheoremAuditStats &theoremAudit_;
  Hooks hooks_;

  /// One built emission census plus the source extent it was built from.
  struct EmissionStructureIndexCacheEntry {
    std::unique_ptr<RefoldPreprocessingStructureIndex> index;
    /// Byte size of the original file text the index was built from.  This is
    /// the same discriminator the run-wide TU index reuse check applies, so a
    /// cache hit never substitutes a census built from a different extent.
    size_t sourceSize = 0;
  };

  /// Emission censuses keyed by `(physical source owner, owner include id)`.
  ///
  /// The census is immutable once built and is consulted by every per-edit and
  /// per-interval authorization predicate, so it is built at most once per
  /// source owner occurrence for the life of the assembler.
  mutable std::map<std::pair<std::string, std::optional<uint64_t>>,
                   EmissionStructureIndexCacheEntry>
      emissionStructureIndexCache_;
};

} // namespace refold
} // namespace clang

#endif // LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDTEXTEDITASSEMBLER_H
