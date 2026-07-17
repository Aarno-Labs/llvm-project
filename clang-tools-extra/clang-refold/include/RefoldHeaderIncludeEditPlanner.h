//===--- RefoldHeaderIncludeEditPlanner.h ----------------------*- C++ -*-===//
//
// Header-local include edit planning for clang-refold.
//
// RefoldHeaderIncludeEditPlanner owns the detailed source-envelope,
// line-control, macro-state, and insertion-anchor planning for edits whose
// owner is a materialized header.  RefoldIncludeMaterializer keeps the
// recursive include orchestration and delegates this header-local planning to
// the planner service.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDHEADERINCLUDEEDITPLANNER_H
#define LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDHEADERINCLUDEEDITPLANNER_H

#include "core/RefoldModel.h"
#include "edit/RefoldEditTypes.h"
#include "edit/RefoldPatchTypes.h"
#include "edit/RefoldSourceEnvelopeTiling.h"
#include "line-control/SourceLineDirectiveHelpers.h"
#include "proof/RefoldAcceptedResultTypes.h"
#include "proof/RefoldSidebandReplayProof.h"
#include "source/RefoldToken.h"
#include "util/RefoldPathIdentity.h"

#include "clang/Basic/LangOptions.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace clang {
namespace refold {

class LineDirectiveInserter;
class RefoldLineControlProof;
class RefoldMacroStateProof;
class RefoldOwnerStateProof;
class RefoldProofLattice;
class RefoldSourceMapper;
class RefoldTerminalProofSink;
class RefoldTextEditAssembler;
struct HeaderSourceNeutralityContext;

/// Plans byte edits that are local to one materialized header include.
/// The planner is intentionally run-scoped: it borrows immutable model/source
/// inputs and emits an IncludeTextEditPlan without owning recursive state.
class RefoldHeaderIncludeEditPlanner {
public:
  using IncludeEdits = ::clang::refold::IncludeEdits;
  using IncludePatch = ::clang::refold::IncludePatch;
  using IncludeTextEditPlan = ::clang::refold::IncludeTextEditPlan;
  using IncludeAnchorWitness = ::clang::refold::IncludeAnchorWitness;
  using TextEdit = ::clang::refold::TextEdit;

  /// Constructs a header-local edit planner over the immutable pass inputs.
  /// The referenced services remain owned by the caller and must outlive the
  /// planner invocation.
  RefoldHeaderIncludeEditPlanner(
      const RefoldModel &model, llvm::StringRef aSource,
      llvm::StringRef bSource, llvm::ArrayRef<PPTok> aToks,
      llvm::ArrayRef<PPTok> bToks, llvm::ArrayRef<size_t> bTokOff,
      const std::vector<int64_t> &abTokMapA2B,
      const LineDirectiveInserter &lineDirs,
      const RefoldSourceMapper &sourceMapper, const RefoldPathIdentity &paths,
      const RefoldMacroStateProof &macroStateProof,
      const RefoldLineControlProof &lineControlProof,
      const RefoldOwnerStateProof &ownerStateProof,
      const RefoldProofLattice &proofLattice,
      const RefoldTextEditAssembler &textEditAssembler,
      const RefoldTerminalProofSink &terminalSink,
      llvm::ArrayRef<SidebandPragmaEdit> sidebandPragmaEdits,
      const clang::LangOptions &lexLang);

  /// Selects the recorded header declaration that best owns an include patch.
  /// This is a pure declaration-span chooser shared with the materializer
  /// facade for existing callers.
  static const RefoldModel::HeaderDecl *
  FindHeaderDeclForPatch(const RefoldModel::IncludeItem &inc,
                         const IncludePatch &p);

  /// Computes header-local byte edits for include-scoped patches.
  /// The returned plan preserves the original fail-closed realization reasons
  /// and carries any local line-control candidates produced during planning.
  IncludeTextEditPlan Compute(const IncludeEdits &includeEdits,
                              std::string headerText) const;

  /// Proves a pure insertion anchor at a direct child include boundary.
  /// Ambiguous or interior child-cover positions fail closed by returning none.
  std::optional<uint64_t>
  ComputeChildBoundaryInsertByte(const IncludePatch &p, llvm::StringRef file,
                                 IncludeAnchorWitness *witness = nullptr) const;

private:
  struct HeaderIncludeNeutralityAdapter;

  /// A source interval preserved as a proven-neutral header gap piece.
  struct HeaderPreservedGapPiece {
    /// The kind of construct represented by this preserved piece.
    enum class Kind {
      /// A macro-state directive preserved across the gap.
      MacroStateDirective,

      /// A zero-token conditional group.
      ZeroTokenConditionalGroup,

      /// A zero-token macro invocation.
      ZeroTokenMacroInvocation,

      /// A zero-token child include.
      ZeroTokenChildInclude,

      /// A locally balanced diagnostic pragma state island.
      BalancedPragmaStateIsland
    };

    /// Preservation category for this piece.
    Kind kind = Kind::MacroStateDirective;

    /// Macro directive for `MacroStateDirective`; null otherwise.
    const RefoldModel::MacroDirective *directive = nullptr;

    /// Inclusive begin byte in the header text.
    uint64_t begin = 0;

    /// Exclusive end byte in the header text.
    uint64_t end = 0;

    /// Model id for non-directive pieces, when applicable.
    uint64_t id = 0;
  };

  /// Candidate byte anchor for a pure header insertion.
  /// The accepted path and witness travel with the byte so ranking and commit
  /// use exactly the proof carrier that discharged the anchor.
  struct InsertAnchorCandidate {
    /// Include-preserving proof path represented by this anchor.
    AcceptedPathKind path = AcceptedPathKind::Unknown;

    /// Witness evidence attached to the accepted include candidate.
    IncludeAnchorWitness witness;

    /// Concrete insertion byte in the materialized header text.
    uint64_t anchorByte = 0;
  };

  /// Selected insertion anchor plus its accepted-result carrier.
  /// The carrier is produced before commit so edit certification does not rebuild or
  /// reinterpret the winning proof.
  struct SelectedInsertAnchorCandidate {
    /// Local byte-anchor candidate selected by the proof lattice.
    InsertAnchorCandidate anchor;

    /// Accepted-result carrier attached to the emitted TextEdit.
    AcceptedResultCandidate accepted;
  };

  /// Per-patch state needed while planning a pure header insertion.
  /// This carrier replaces mutation-heavy local captures with explicit inputs
  /// shared by the insertion-anchor helper methods.
  struct HeaderInsertionPlanningState {
    /// Current materialized include being edited.
    const RefoldModel::IncludeItem &include;

    /// Include patch being anchored.
    const IncludePatch &patch;

    /// Best owning declaration for the patch, if one was found.
    const RefoldModel::HeaderDecl *decl = nullptr;

    /// Output plan mutated when an anchor commits or must realize.
    IncludeTextEditPlan &plan;

    /// Entered-file spelling for the current header.
    llvm::StringRef file;

    /// Materialized header bytes used for local edit construction.
    llvm::StringRef headerText;

    /// Already-normalized insertion payload.
    llvm::StringRef materialInsertBytes;

    /// Header byte length.
    uint64_t fileLen = 0;

    /// Lower PP-token bound for this include-local planning window.
    uint64_t ppLo = 0;

    /// Upper PP-token bound for this include-local planning window.
    uint64_t ppHi = 0;

    /// Normalized insertion PP position.
    uint64_t pos = 0;
  };

  /// Source-bearing piece considered for a widened header envelope.
  /// Pieces may be mapped tokens or complete source constructs that own nested
  /// mapped-token evidence.
  struct HeaderSourceEnvelopePiece {
    /// Inclusive begin byte in the materialized header.
    uint64_t begin = 0;

    /// Exclusive end byte in the materialized header.
    uint64_t end = 0;

    /// Inclusive begin PP token covered by this source piece.
    uint64_t ppBegin = 0;

    /// Exclusive end PP token covered by this source piece.
    uint64_t ppEnd = 0;

    /// Stable model id or token index used for deterministic tie-breaking.
    uint64_t id = 0;

    /// Source-piece category; string spelling preserves the old sort order.
    llvm::StringRef kind;
  };

  /// Per-patch state for DELETE/REPLACE source-envelope widening.
  /// Mutable outputs are references so the helper commits exactly the same
  /// range, witness, gap-preservation, and line-resume state as the old block.
  struct HeaderSourceEnvelopePlanningState {
    /// Include currently being materialized.
    const RefoldModel::IncludeItem &include;

    /// Patch being planned.
    const IncludePatch &patch;

    /// Original patch index used in fail-closed diagnostics.
    size_t patchIndex = 0;

    /// Best owning declaration for the patch, if any.
    const RefoldModel::HeaderDecl *decl = nullptr;

    /// Output plan to mark for include realization on proof failure.
    IncludeTextEditPlan &plan;

    /// Entered-file spelling for the current header.
    llvm::StringRef file;

    /// Materialized header bytes being edited.
    llvm::StringRef headerText;

    /// Replacement payload after insertion/replacement normalization.
    llvm::StringRef materialInsertBytes;

    /// Header source-neutrality context for gap discharge.
    const HeaderSourceNeutralityContext &headerSourceNeutrality;

    /// Header/declaration PP-token planning lower bound.
    uint64_t ppLo = 0;

    /// Header/declaration PP-token planning upper bound.
    uint64_t ppHi = 0;

    /// Include-local A-side cover begin.
    uint64_t coverBegin = 0;

    /// Include-local A-side cover end.
    uint64_t coverEnd = 0;

    /// Normalized A-side material begin.
    uint64_t materialAStart = 0;

    /// Normalized A-side material end.
    uint64_t materialAEnd = 0;

    /// Whether this source-envelope plan belongs to a deletion patch.
    bool isDelete = false;

    /// Current mapped header edit begin, widened on successful proof.
    std::optional<uint64_t> &startByte;

    /// Current mapped header edit end, widened on successful proof.
    std::optional<uint64_t> &endByte;

    /// Witness carrier widened with the accepted source envelope.
    IncludeAnchorWitness &mappedHeaderWitness;

    /// Preserved neutral pieces copied into the final replacement payload.
    llvm::SmallVectorImpl<HeaderPreservedGapPiece> &headerGapPreservations;

    /// Source-line directive resume carried from a proven header gap, if any.
    std::optional<SourceLineDirectiveGapResume>
        &headerSourceLineDirectiveResume;

    /// Set when the mapped range was widened to the full source envelope.
    bool &usedFullHeaderEnvelope;
  };

  /// Active header #define selected for carry after a replacement payload.
  /// The byte interval names the original directive line that will be moved.
  struct HeaderMacroStateCarryCandidate {
    /// Directive being moved after the replacement payload.
    const RefoldModel::MacroDirective *directive = nullptr;

    /// Inclusive begin byte of the original directive line.
    uint64_t begin = 0;

    /// Exclusive end byte of the original directive line.
    uint64_t end = 0;

    /// Macro name recovered from the directive spelling.
    llvm::StringRef name;
  };

  /// Mutable state for carrying active header #defines across a replacement.
  /// The helper updates the byte range, replacement text, and witness in place.
  struct HeaderMacroStateCarryState {
    /// Include currently being materialized.
    const RefoldModel::IncludeItem &include;

    /// Output plan whose staged edits must not overlap carried directives.
    IncludeTextEditPlan &plan;

    /// Entered-file spelling for the current header.
    llvm::StringRef file;

    /// Materialized header bytes being edited.
    llvm::StringRef headerText;

    /// Current mapped header edit begin, widened when carry succeeds.
    std::optional<uint64_t> &startByte;

    /// Current mapped header edit end, widened when carry succeeds.
    std::optional<uint64_t> &endByte;

    /// Replacement payload rebuilt when active definitions are carried.
    std::string &replacement;

    /// Mapped-header witness updated to the widened carried range.
    IncludeAnchorWitness &mappedHeaderWitness;
  };

  /// Result of rebuilding a replacement with carried macro-state directives.
  /// The caller commits it only after every crossed source chunk is admissible.
  struct HeaderMacroStateCarryRewrite {
    /// New inclusive edit begin after carrying directive lines.
    uint64_t startByte = 0;

    /// New exclusive edit end after carrying any physical line tail.
    uint64_t endByte = 0;

    /// Replacement payload with directives restored after B-derived text.
    std::string replacement;
  };

  /// Builds a TextEdit while applying local or pending #line resync behavior.
  /// This mirrors the materializer facade's edit-assembly policy for
  /// header-local edits planned by this service.
  TextEdit MakeTextEditWithResyncOrPending(
      llvm::StringRef original, uint64_t start, uint64_t end,
      llvm::StringRef replacement, llvm::StringRef fileSpelling,
      std::optional<uint64_t> ownerIncludeId = std::nullopt) const;

  /// Proves that a complete header conditional group is a neutral gap piece.
  /// Selected-arm includes are discharged through the header include neutrality
  /// adapter so nested zero-token subtrees stay instance-specific.
  bool HeaderConditionalGroupIsPreservableGap(
      const HeaderSourceNeutralityContext &headerSourceNeutrality,
      const RefoldModel::IncludeItem &currentInclude, llvm::StringRef file,
      llvm::StringRef headerText, const RefoldModel::CondGroup &group,
      uint64_t gapBegin, uint64_t gapEnd) const;

  /// Proves that a header macro invocation emits no material PP tokens.
  /// This keeps the shared neutrality proof behind a named planner predicate.
  bool HeaderMacroInvocationIsSourceNeutralZeroToken(
      const HeaderSourceNeutralityContext &headerSourceNeutrality,
      const RefoldModel::MacroInvocation &invocation) const;

  /// Proves that a header macro callsite can be preserved as a neutral gap.
  /// The callsite must be owned by the current include and byte-verified before
  /// recursive zero-token neutrality is accepted.
  bool HeaderMacroInvocationIsPreservableGap(
      const HeaderSourceNeutralityContext &headerSourceNeutrality,
      const RefoldModel::IncludeItem &currentInclude, llvm::StringRef file,
      llvm::StringRef headerText, const RefoldModel::MacroInvocation &macro,
      uint64_t gapBegin, uint64_t gapEnd) const;

  /// Returns whether one include instance is nested below another in the model.
  /// The check follows producer parent links rather than path identity.
  bool HeaderIncludeIsDescendantOf(const RefoldModel::IncludeItem &candidate,
                                   const RefoldModel::IncludeItem &root) const;

  /// Proves that a direct child include subtree can be preserved as zero-token.
  /// Descendant includes, macros, conditionals, and macro-state directives must
  /// all be neutral relative to the current replacement text.
  bool HeaderZeroTokenChildIncludeIsPreservableGap(
      const RefoldModel::IncludeItem &currentInclude, llvm::StringRef file,
      llvm::StringRef headerText, const RefoldModel::IncludeItem &child,
      uint64_t gapBegin, uint64_t gapEnd, llvm::StringRef replacement) const;

  /// Proves that a child include is zero-token neutral in this header context.
  /// This is the callback target used by conditional-island neutrality.
  bool HeaderIncludeIsSourceNeutralZeroToken(
      const RefoldModel::IncludeItem &currentInclude, llvm::StringRef file,
      llvm::StringRef headerText, const RefoldModel::IncludeItem &child) const;

  /// Returns whether a PP span lies wholly inside replacement material.
  /// Empty or invalid spans are treated as already covered.
  static bool PPSpanInsideHeaderMaterial(const RefoldModel::PPSpan &span,
                                         uint64_t materialBeginA,
                                         uint64_t materialEndA);

  /// Returns whether an arm is the queried arm or nested beneath it.
  /// Parent-arm links let outer conditional proofs account for nested selected
  /// material reported by innermost ownership queries.
  bool HeaderArmIsSameOrNestedUnder(const RefoldModel::ArmRef &candidate,
                                    uint64_t ancestorArmId) const;

  /// Returns whether a selected arm's effective PP material is fully consumed.
  /// Effective ownership is preferred over direct spans when the model can map
  /// tokens to nested conditional arms.
  bool HeaderSelectedArmEffectiveMaterialInside(const RefoldModel::CondArm &arm,
                                                uint64_t materialBeginA,
                                                uint64_t materialEndA) const;

  /// Returns the exact source text represented by a preserved gap piece.
  /// Macro directives use their recorded spelling; other pieces slice the
  /// materialized header text when the interval is valid.
  static std::string PreservedGapPieceText(const HeaderPreservedGapPiece &piece,
                                           llvm::StringRef headerText);

  /// Proves that a complete selected conditional group is consumed by a hunk.
  /// The selected arm's effective PP material must lie wholly in the material
  /// interval before the directive group may become a source-envelope piece.
  bool HeaderConditionalGroupIsConsumedSourceEnvelope(
      const RefoldModel::IncludeItem &currentInclude, llvm::StringRef file,
      llvm::StringRef headerText, const RefoldModel::CondGroup &group,
      uint64_t materialBeginA, uint64_t materialEndA) const;

  /// Returns whether a candidate source envelope lies wholly inside one
  /// producer-selected arm of a header conditional group.
  ///
  /// This distinguishes an enclosing conditional wrapper from a source
  /// envelope that actually crosses conditional-control structure. The
  /// caller must still prove every inter-piece source gap before committing
  /// the widened edit.
  bool HeaderSourceEnvelopeIsInsideSelectedConditionalArm(
      const RefoldModel::IncludeItem &currentInclude, llvm::StringRef file,
      const RefoldModel::CondGroup &group, uint64_t sourceBegin,
      uint64_t sourceEnd) const;

  /// Returns whether selected material from a header conditional group overlaps
  /// a hunk's A-side material interval.
  bool HeaderConditionalGroupSelectedMaterialOverlaps(
      const RefoldModel::IncludeItem &currentInclude, llvm::StringRef file,
      const RefoldModel::CondGroup &group, uint64_t materialBeginA,
      uint64_t materialEndA) const;

  /// Returns whether a macro-state directive is owned by an include subtree.
  /// Direct ownership and descendant include ownership are both accepted.
  bool IncludeOwnsDirective(const RefoldModel::MacroDirective &directive,
                            const RefoldModel::IncludeItem &root) const;

  /// Verifies that a recorded macro directive can be recovered from its owner
  /// file spelling.  Failure keeps macro-state preservation fail-closed.
  bool RecordedMacroDirectiveMatchesOwnerFile(
      const RefoldModel::MacroDirective &directive) const;

  /// Collects macro-state directives that may be preserved with an include
  /// subtree.  The replacement text must not observe any collected directive.
  bool CollectMacroStatePreservationsFromIncludeSubtree(
      const RefoldModel::IncludeItem &root, llvm::StringRef replacement,
      llvm::SmallVectorImpl<HeaderPreservedGapPiece> &out) const;

  /// Coalesces adjacent patches that together consume a selected conditional
  /// group.  This preserves the original shortest-run search order.
  bool TryBuildConsumedHeaderConditionalCoalescedPatch(
      const IncludeEdits &includeEdits, llvm::StringRef file,
      llvm::StringRef headerText, size_t idx, IncludePatch &coalesced,
      size_t &skipThrough) const;

  /// Coalesces adjacent patches split around a child include's macro-state
  /// subtree.  The candidate is accepted only when state-preservation proof
  /// pieces can be recovered deterministically.
  bool TryBuildMaterialChildStateCoalescedPatch(
      const IncludeEdits &includeEdits, llvm::StringRef file, size_t idx,
      IncludePatch &coalesced, size_t &skipThrough) const;

  /// Returns whether a source-bearing macro invocation is fully consumed.
  /// Every producer-attributed PP span must be covered by the replacement
  /// material before the callsite can become a source-envelope piece.
  bool HeaderMacroInvocationIsConsumedSourceEnvelope(
      const RefoldModel::IncludeItem &currentInclude, llvm::StringRef file,
      llvm::StringRef headerText, const RefoldModel::MacroInvocation &macro,
      uint64_t materialBeginA, uint64_t materialEndA) const;

  /// Returns whether a header macro invocation overlaps replacement material.
  /// The check is restricted to callsites owned by the current include
  /// instance.
  bool HeaderMacroInvocationOverlapsMaterial(
      const RefoldModel::IncludeItem &currentInclude, llvm::StringRef file,
      const RefoldModel::MacroInvocation &macro, uint64_t materialBeginA,
      uint64_t materialEndA) const;

  /// Returns whether a #line directive can start at an existing header offset.
  /// Beginning-of-line and indent-only prefixes are accepted.
  static bool HeaderLineDirectiveStartsAtPrefix(llvm::StringRef headerText,
                                                uint64_t pos);

  /// Returns whether a #line directive may start after an optional newline.
  /// Backslash-continued physical lines reject the optional-newline form.
  static bool CanStartHeaderLineDirectiveWithOptionalLeadingNewline(
      llvm::StringRef headerText, uint64_t pos);

  /// Checks that widening an edit can safely precede a restored #line
  /// directive. The replacement suffix and copied header suffix must remain
  /// lexically separated when a directive is inserted at `sourceEnd`.
  bool ReplacementHeaderSuffixBoundaryAllowsDirectiveLine(
      llvm::StringRef headerText, llvm::StringRef replacement,
      uint64_t sourceEnd) const;

  /// Orders source-envelope pieces by their preserved source category.
  /// The string comparison preserves the original local-lambda tie-breaker.
  static bool
  HeaderSourceEnvelopePieceKindPrecedes(const HeaderSourceEnvelopePiece &lhs,
                                        const HeaderSourceEnvelopePiece &rhs);

  /// Returns whether an outer source-envelope piece owns a nested piece.
  /// This is the full-envelope ownership rule for tokens nested under complete
  /// child includes, macro invocations, and conditional groups.
  static bool
  HeaderSourceEnvelopePieceContains(const HeaderSourceEnvelopePiece &outer,
                                    const HeaderSourceEnvelopePiece &piece);

  /// Orders preserved gap pieces by preservation category.
  /// This matches the former source-envelope gap tiling callback.
  static bool
  HeaderPreservedGapPieceKindPrecedes(const HeaderPreservedGapPiece &lhs,
                                      const HeaderPreservedGapPiece &rhs);

  /// Returns whether one preserved gap piece owns another proven piece.
  /// Nested zero-token macro calls and complete conditional islands absorb
  /// already-proved child pieces.
  static bool
  HeaderPreservedGapPieceContains(const HeaderPreservedGapPiece &outer,
                                  const HeaderPreservedGapPiece &piece);

  /// Returns whether an uncovered header gap range is trivia.
  /// Only whitespace and complete comments may be accepted without a proof
  /// piece.
  static bool HeaderGapRangeIsTrivia(llvm::StringRef headerText, uint64_t begin,
                                     uint64_t end);

  /// Proves one physical gap between full-envelope source pieces.
  /// The method appends any preserved gap pieces and records source-line resume
  /// state on the planning carrier.
  bool ProveHeaderSourceEnvelopeGap(
      const HeaderSourceEnvelopePlanningState &state,
      const SourceEnvelopeInterval &sourceEnvelope, uint64_t gapBegin,
      uint64_t gapEnd,
      llvm::SmallVectorImpl<HeaderPreservedGapPiece> &preservedPieces) const;

  /// Attempts DELETE/REPLACE full source-envelope widening.
  /// Failure marks the include for explicit realization on the output plan.
  bool TryApplyDeleteReplaceSourceEnvelope(
      const HeaderSourceEnvelopePlanningState &state) const;

  /// Returns whether a definition is the active macro binding at a header byte.
  /// Shadowed or non-define directives are not eligible for carry.
  bool ActiveHeaderDefinitionAtByte(
      const RefoldModel::MacroDirective &definition, llvm::StringRef macroName,
      const RefoldModel::IncludeItem &include, llvm::StringRef file,
      llvm::StringRef headerText, uint64_t offset) const;

  /// Returns whether a header range overlaps an already staged edit.
  /// Macro-state carry only moves directive bytes that are still intact.
  static bool HeaderRangeOverlapsStagedEdit(llvm::ArrayRef<TextEdit> edits,
                                            uint64_t begin, uint64_t end);

  /// Orders macro-state carry candidates by original source order.
  /// Directive ids break ties deterministically for equal begin bytes.
  static bool HeaderMacroStateCarryCandidatePrecedes(
      const HeaderMacroStateCarryCandidate &lhs,
      const HeaderMacroStateCarryCandidate &rhs);

  /// Collects active #define directives that a replacement would observe.
  /// Candidates are filtered to definitions that may cross the replaced bytes.
  void CollectHeaderMacroStateCarryCandidates(
      const HeaderMacroStateCarryState &state,
      llvm::SmallVectorImpl<HeaderMacroStateCarryCandidate> &out) const;

  /// Verifies that multiple carried definitions may cross intervening bytes.
  /// This preserves the original source-order dependency checks between
  /// carries.
  bool HeaderMacroStateCarryCandidatesCanCross(
      const HeaderMacroStateCarryState &state,
      llvm::ArrayRef<HeaderMacroStateCarryCandidate> candidates) const;

  /// Computes the widened replacement tail needed for line-safe carry output.
  /// A carried directive must be restored at a physical line boundary.
  static uint64_t
  HeaderMacroStateCarryReplacementTailEnd(llvm::StringRef headerText,
                                          uint64_t endByte);

  /// Returns whether the widened tail is safe to move before carried
  /// directives. The tail must not observe any carried macro definition.
  bool HeaderMacroStateCarryTailIsNonObserving(
      const HeaderMacroStateCarryState &state,
      llvm::ArrayRef<HeaderMacroStateCarryCandidate> candidates,
      uint64_t replacementTailEnd) const;

  /// Rebuilds the replacement payload with carried definitions restored later.
  /// Returns none when overlapping carry intervals make the rewrite
  /// inadmissible.
  std::optional<HeaderMacroStateCarryRewrite>
  BuildCarriedHeaderMacroStateRewrite(
      const HeaderMacroStateCarryState &state,
      llvm::ArrayRef<HeaderMacroStateCarryCandidate> candidates,
      uint64_t replacementTailEnd) const;

  /// Carries active header #defines after a replacement when proof permits it.
  /// On success, the edit range, replacement text, and witness are widened.
  void TryCarryHeaderMacroStateAfterReplacement(
      const HeaderMacroStateCarryState &state) const;

  /// Checks owner-state proof obligations for a synthetic header #line resume.
  /// The resume must preserve line number, file state, and filename state at
  /// the source-byte boundary where untouched header text resumes.
  void
  CheckHeaderSourceLineResumeStateTransitions(const IncludeEdits &includeEdits,
                                              size_t patchIndex,
                                              uint64_t sourceEnd) const;

  /// Normalizes a duplicated boundary-token insertion across a #line gap.
  /// The adjustment is local to include-owned insertions and fires only when
  /// the A-to-B token map proves the duplicate-token shape.
  void NormalizeDuplicatedBoundaryTokenAcrossLineControlGap(
      llvm::StringRef file, llvm::StringRef headerText, uint64_t ppHi,
      uint64_t &materialAStart, uint64_t &materialBStart,
      uint64_t &materialBEnd, uint64_t &materialInsertPos,
      std::string &materialInsertBytes) const;

  /// Returns whether an insertion anchor satisfies any conditional-arm witness.
  /// Patches without an arm certificate accept any concrete anchor.
  bool AnchorMatchesCondArmCert(const HeaderInsertionPlanningState &state,
                                uint64_t anchorByte) const;

  /// Selects the best proved insertion candidate with the proof lattice.
  /// The returned value carries the exact accepted-result proof for commit.
  std::optional<SelectedInsertAnchorCandidate>
  SelectBestInsertCandidate(llvm::ArrayRef<InsertAnchorCandidate> candidates,
                            const IncludePatch &patch) const;

  /// Commits a selected pure-insertion anchor into the output plan.
  /// Boundary padding, local line resync, and accepted-result certification are
  /// applied in the same operation.
  void
  CommitInsertCandidate(const HeaderInsertionPlanningState &state,
                        const SelectedInsertAnchorCandidate &selected) const;

  /// Returns whether copied header bytes survive after an insertion anchor.
  /// Whitespace-only suffixes do not need local line-state resync.
  static bool HasCopiedHeaderSuffix(llvm::StringRef headerText,
                                    uint64_t fileLen, uint64_t anchorByte);

  /// Finds a direct child include whose directive boundary equals an anchor.
  /// The result is used only to decide whether the child will perform its own
  /// materialization wrapper and line-state transition.
  const RefoldModel::IncludeItem *
  ChildIncludeAtBoundaryAnchor(const RefoldModel::IncludeItem &currentInclude,
                               llvm::StringRef file, uint64_t anchorByte) const;

  /// Returns whether sideband replay will visibly materialize an include.
  /// Visible child materialization suppresses the parent's local boundary
  /// resync.
  bool IncludeHasVisibleSidebandWork(uint64_t includeId) const;

  /// Returns whether a pure insertion must locally resume header line state.
  /// Suffix observers, directive boundaries, and directive replay text are all
  /// considered before deciding whether the parent edit needs a pending #line.
  bool InsertionNeedsLocalResync(const HeaderInsertionPlanningState &state,
                                 const InsertAnchorCandidate &candidate,
                                 llvm::StringRef replacement) const;

  /// Finds a selected-conditional begin boundary for an insertion, if valid.
  /// The boundary is accepted only when the insertion is not explicitly owned
  /// by that arm.
  std::optional<uint64_t>
  SelectedArmBeginBoundaryByte(const HeaderInsertionPlanningState &state) const;

  /// Builds a selected-conditional boundary anchor candidate, if available.
  /// The candidate records the selected arm id as witness evidence.
  std::optional<InsertAnchorCandidate> BuildSelectedConditionalBoundaryAnchor(
      const HeaderInsertionPlanningState &state) const;

  /// Builds a direct child-include boundary anchor candidate, if available.
  /// The child-boundary proof names the direct include supplying the anchor.
  std::optional<InsertAnchorCandidate>
  BuildChildBoundaryAnchor(const HeaderInsertionPlanningState &state) const;

  /// Finds the nearest mapped PP token to the right in the same header file.
  /// The search is restricted to the include-local PP planning window.
  std::optional<uint64_t> FindRightNeighborPP(llvm::StringRef file,
                                              uint64_t pos, uint64_t ppLo,
                                              uint64_t ppHi) const;

  /// Finds the nearest mapped PP token to the left in the same header file.
  /// The search is restricted to the include-local PP planning window.
  std::optional<uint64_t> FindLeftNeighborPP(llvm::StringRef file, uint64_t pos,
                                             uint64_t ppLo,
                                             uint64_t ppHi) const;

  /// Appends a right-neighbor insertion anchor when that proof discharges.
  /// Returns false only after recording a fail-closed realization reason.
  bool AppendRightNeighborAnchor(
      const HeaderInsertionPlanningState &state,
      llvm::SmallVectorImpl<InsertAnchorCandidate> &candidates) const;

  /// Appends left-neighbor or declaration-boundary insertion anchors.
  /// Returns false only after recording a fail-closed realization reason.
  bool AppendSecondaryInsertionAnchors(
      const HeaderInsertionPlanningState &state,
      llvm::SmallVectorImpl<InsertAnchorCandidate> &candidates) const;

  /// Plans and commits a pure include-local insertion if any anchor wins.
  /// On failure, the plan is marked for explicit include realization.
  bool PlanPureInsertionPatch(const HeaderInsertionPlanningState &state) const;

  const RefoldModel &model_;
  llvm::StringRef aSource_;
  llvm::StringRef bSource_;
  llvm::ArrayRef<PPTok> aToks_;
  llvm::ArrayRef<PPTok> bToks_;
  llvm::ArrayRef<size_t> bTokOff_;
  const std::vector<int64_t> &abTokMapA2B_;
  const LineDirectiveInserter &lineDirs_;
  const RefoldSourceMapper &sourceMapper_;
  const RefoldPathIdentity &paths_;
  const RefoldMacroStateProof &macroStateProof_;
  const RefoldLineControlProof &lineControlProof_;
  const RefoldOwnerStateProof &ownerStateProof_;
  const RefoldProofLattice &proofLattice_;
  const RefoldTextEditAssembler &textEditAssembler_;
  llvm::ArrayRef<SidebandPragmaEdit> sidebandPragmaEdits_;
  const clang::LangOptions &lexLang_;
};

} // namespace refold
} // namespace clang

#endif // LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDHEADERINCLUDEEDITPLANNER_H
