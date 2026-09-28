//===--- RefoldLineControlProof.h -----------------------------*- C++ -*-===//
//
// Final line-control proof helpers for clang-refold.
//
// This service answers owner-local line-state proof questions for preserved
// location-sensitive builtins and producer-backed `#line` directives.  It is a
// read-only proof object: it borrows the producer model, source/token maps, and
// narrow path/macro/text services, but it does not emit edits or mutate the
// refold result.  Emission remains in the text assembler/materializer.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDLINECONTROLPROOF_H
#define LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDLINECONTROLPROOF_H

#include "line-control/FinalLineControlModel.h"
#include "line-control/LineDirectiveInserter.h"
#include "model/RefoldModel.h"
#include "model/RefoldToken.h"
#include "proof/RefoldProofVocabulary.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"

#include <cstdint>
#include <optional>
#include <vector>

namespace clang {
namespace refold {

class RefoldMacroTopology;
class RefoldPathIdentity;
class RefoldPreprocessingStructureIndexProvider;
struct SourceLineDirectiveGapResume;

/// Earliest preserved line-state observer in an owner suffix.
///
/// The byte offset is expressed in the physical owner source.  The demand says
/// which logical-location components the observer can consume after an edit.
struct LineStateObserverSite {
  uint64_t offset = 0;
  LineStateObserverDemand demand;
};

/// Read-only line-control proof service.
///
/// The service intentionally owns only line-state predicates.  It does not own
/// final `#line` text generation, pruning, or accepted-result theorem auditing;
/// those remain in the line-directive/text-assembly and audit subsystems.
class RefoldLineControlProof {
public:
  /// Construct a proof service over producer line-control and token-map facts.
  RefoldLineControlProof(
      const RefoldModel &model, const RefoldPathIdentity &paths,
      const RefoldMacroTopology &macroTopology,
      const LineDirectiveInserter &lineDirs, llvm::ArrayRef<PPTok> aToks,
      llvm::ArrayRef<PPTok> bToks, const std::vector<int64_t> &abTokMapA2B,
      const std::vector<int64_t> &abTokMapB2A)
      : model_(model), paths_(paths), macroTopology_(macroTopology),
        lineDirs_(lineDirs), aToks_(aToks), bToks_(bToks),
        abTokMapA2B_(abTokMapA2B), abTokMapB2A_(abTokMapB2A) {}

  /// Attach the structure-index provider through which OwnerLineStateAt()
  /// binds lexical line-control directives to producer events.
  ///
  /// The engine builds the provider after this service, so it is attached
  /// once the service graph exists.  Until then every owner line-state query
  /// is unproven.
  void BindStructureIndexes(
      const RefoldPreprocessingStructureIndexProvider &structureIndexes) {
    structureIndexes_ = &structureIndexes;
  }

  /// Return the presumed location Clang assigned at \p offset of one owner's
  /// physical source, computed from producer line-control events alone.
  ///
  /// The result is proven when the owner's structure index accounts for every
  /// lexical line-control directive ending at or before \p offset: each is
  /// bound to a producer event of this owner or lies inside a range the
  /// producer skipped, and each such event of the owner has a site.  The
  /// latest event then gives `(logicalFileAfter, logicalLineAfter +
  /// LB[siteE, offset))`, and with none the location is `(defaultFile,
  /// 1 + LB[0, offset))`, where LB counts physical line breaks as
  /// stringutils::countPhysicalLineBreaks() does.  \p ownerBytes must be the
  /// bytes the producer lexed; a size that differs from the index's is
  /// unproven.
  ///
  /// An unproven result still carries that location, computed from the latest
  /// event whose site is known, and names the last directive that broke the
  /// proof when one has a known offset: a directive straddling \p offset, an
  /// unbound directive outside every skipped range, or an unbound event.
  LineDirectiveLocation OwnerLineStateAt(llvm::StringRef ownerFile,
                                         std::optional<uint64_t> ownerIncludeId,
                                         llvm::StringRef ownerBytes,
                                         uint64_t offset,
                                         llvm::StringRef defaultFile) const;

  /// Return the logical-line start of the owner's line-control directive whose
  /// spelling contains \p offset after its introducer, or nullopt when
  /// \p offset lies inside none.
  ///
  /// A token spelled there, such as a `__LINE__` operand, is expanded before
  /// the directive takes effect, and nothing can be inserted inside the
  /// directive, so a repair aimed at that token belongs at the returned
  /// offset.
  std::optional<uint64_t> LineControlDirectiveLineStartContaining(
      llvm::StringRef ownerFile, std::optional<uint64_t> ownerIncludeId,
      uint64_t offset) const;

  /// Prove that the owner-source gap [\p gapBegin, \p gapEnd) consists of
  /// complete executed line-control directives and lexical trivia, and return
  /// the state a resume directive must re-establish for the source at
  /// \p resumeOffset.
  ///
  /// Every protected interval the owner's structure index finds in the gap
  /// must be a line-control directive wholly inside it and bound to a producer
  /// event, the bytes between them must be lexical trivia, and there must be at
  /// least one.  The file and line are OwnerLineStateAt(\p resumeOffset), which
  /// must be proven.  The line-marker flags replay what the directives from
  /// \p gapBegin on did to the presumed file: `1` for one entered file, then
  /// `3` or `3 4` for the file kind in effect at \p resumeOffset.  A range that
  /// exits a presumed file or enters more than one is refused, as is an event
  /// from a map that predates recording file kinds.
  ///
  /// \p operandMacroIds, when non-null, receives every macro invocation of the
  /// owner spelled inside one of the gap's directives: it contributed no token,
  /// and consuming the directive consumes it.
  std::optional<SourceLineDirectiveGapResume> LineControlGapResume(
      llvm::StringRef ownerFile, std::optional<uint64_t> ownerIncludeId,
      llvm::StringRef ownerBytes, uint64_t gapBegin, uint64_t gapEnd,
      uint64_t resumeOffset,
      llvm::SmallVectorImpl<uint64_t> *operandMacroIds = nullptr) const;

  /// Return the source spelling site that observes a line-state builtin.
  const RefoldModel::MacroInvocation *
  LineStateObservableMacroSite(const RefoldModel::MacroInvocation &macro) const;

  /// Return whether the source prefix before `offset` contains an active
  /// producer-proven line-control directive for the requested owner.
  bool SourcePrefixHasProducerActiveLineControl(
      llvm::StringRef ownerFile, std::optional<uint64_t> ownerIncludeId,
      uint64_t offset) const;

  /// Return whether an executed line-control directive of an include
  /// occurrence, ending by `offset`, provably named the presumed file.
  ///
  /// A directive without a filename operand keeps the presumed file, so one
  /// whose recorded file differs from the file in effect before it named one.
  /// Before the occurrence's first directive that file is the producer's
  /// entered-file spelling.  A directive naming the file already in effect is
  /// not detected; that only keeps a wrapper.  A marker entering or leaving a
  /// presumed file, or an occurrence with no recorded entered-file spelling,
  /// answers false.
  bool SourcePrefixNamesPresumedFile(llvm::StringRef ownerFile,
                                     uint64_t ownerIncludeId,
                                     uint64_t offset) const;

  /// Summarize preserved line-state builtin demand inside an include subtree.
  LineStateObserverDemand
  IncludeSubtreeLineStateObserverDemand(uint64_t includeId) const;

  /// Return whether the child include still needs an entry #line wrapper to
  /// preserve physical blank-line layout before line-state observers.
  bool IncludeEntryLineDirectiveDischargesLayoutBarrier(
      const RefoldModel::IncludeItem &child,
      llvm::StringRef parentOwnerFileForDemand) const;

  /// Compare physical file spellings for line-control proof purposes.
  bool SameLineControlPhysicalFile(llvm::StringRef lhs,
                                   llvm::StringRef rhs) const;

  /// Return the latest active producer-backed line-control end before `offset`.
  std::optional<uint64_t>
  LatestProducerLineControlEndBefore(std::optional<uint64_t> ownerIncludeId,
                                     llvm::StringRef ownerFile,
                                     uint64_t offset) const;

  /// Return whether a line-state builtin invocation survived token-identically
  /// into B and therefore remains a source observer.
  bool LineStateBuiltinInvocationIsPreservedObserver(
      const RefoldModel::MacroInvocation &macro) const;

  /// Return whether demand for `macro` requires producer model evidence rather
  /// than direct lexical discovery in the emitted owner source.
  bool LineStateBuiltinInvocationNeedsModelBackedLineStateDemand(
      const RefoldModel::MacroInvocation &macro) const;

  /// Summarize preserved line-state observer demand at or after `offset` in an
  /// owner file.
  LineStateObserverDemand
  OwnerSuffixLineStateObserverDemand(std::optional<uint64_t> ownerIncludeId,
                                     llvm::StringRef ownerFile,
                                     uint64_t offset) const;

  /// Return the earliest preserved line-state observer site in an owner suffix.
  std::optional<LineStateObserverSite>
  FirstOwnerSuffixLineStateObserverSite(std::optional<uint64_t> ownerIncludeId,
                                        llvm::StringRef ownerFile,
                                        uint64_t offset) const;

  /// Return whether an owner suffix contains any preserved line-state observer.
  bool OwnerSuffixHasLineStateSensitiveBuiltin(
      std::optional<uint64_t> ownerIncludeId, llvm::StringRef ownerFile,
      uint64_t offset) const;

  /// Advance an insertion anchor across contiguous producer-backed source
  /// line-control directives that start at the anchor after whitespace only.
  std::optional<uint64_t> AdvanceInsertionAnchorPastSourceLineControlPrefix(
      llvm::StringRef ownerFile, std::optional<uint64_t> ownerIncludeId,
      llvm::StringRef ownerBytes, uint64_t anchor) const;

  /// Return whether a TU insertion that already advanced past a source
  /// line-control prefix may also defer its local newline resync to the
  /// conditional-group join that closes the innermost top-level cond group
  /// covering the anchor.  The deferral is allowed only when the first
  /// preserved suffix observer demands a line directive and sits at or after
  /// the cond-group end.
  bool TUInsertionCanDeferResyncToConditionalJoin(
      bool advancedOverSourceLineControlPrefix, llvm::StringRef tuPath,
      uint64_t anchor) const;

private:
  const RefoldModel &model_;
  const RefoldPathIdentity &paths_;
  const RefoldMacroTopology &macroTopology_;
  const LineDirectiveInserter &lineDirs_;
  llvm::ArrayRef<PPTok> aToks_;
  llvm::ArrayRef<PPTok> bToks_;
  const std::vector<int64_t> &abTokMapA2B_;
  const std::vector<int64_t> &abTokMapB2A_;
  const RefoldPreprocessingStructureIndexProvider *structureIndexes_ = nullptr;
};

} // namespace refold
} // namespace clang

#endif // LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDLINECONTROLPROOF_H
