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

#include "core/RefoldModel.h"
#include "line-control/FinalLineControlModel.h"
#include "line-control/LineDirectiveInserter.h"
#include "proof/RefoldProofVocabulary.h"
#include "source/RefoldSourceMapper.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"

#include <cstdint>
#include <optional>
#include <vector>

namespace clang {
namespace refold {

class RefoldMacroTopology;
class RefoldPathIdentity;
class RefoldTokenTextAnalysis;

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
      const RefoldModel &model, const RefoldSourceMapper &sourceMapper,
      const RefoldPathIdentity &paths, const RefoldTokenTextAnalysis &tokenText,
      const RefoldMacroTopology &macroTopology,
      const LineDirectiveInserter &lineDirs, llvm::ArrayRef<PPTok> aToks,
      llvm::ArrayRef<PPTok> bToks, const std::vector<int64_t> &abTokMapA2B,
      const std::vector<int64_t> &abTokMapB2A)
      : model_(model), paths_(paths), macroTopology_(macroTopology),
        lineDirs_(lineDirs), aToks_(aToks), bToks_(bToks),
        abTokMapA2B_(abTokMapA2B), abTokMapB2A_(abTokMapB2A) {}

  /// Return the source spelling site that observes a line-state builtin.
  const RefoldModel::MacroInvocation *
  LineStateObservableMacroSite(const RefoldModel::MacroInvocation &macro) const;

  /// Return whether the source prefix before `offset` contains an active
  /// producer-proven line-control directive for the requested owner.
  bool SourcePrefixHasProducerActiveLineControl(
      llvm::StringRef ownerFile, std::optional<uint64_t> ownerIncludeId,
      uint64_t offset) const;

  /// Summarize preserved line-state builtin demand inside an include subtree.
  LineStateObserverDemand
  IncludeSubtreeLineStateObserverDemand(uint64_t includeId) const;

  /// Return whether an include subtree contains any preserved line-state
  /// observer that can consume synthetic line-control repair.
  bool IncludeSubtreeHasLineStateSensitiveBuiltin(uint64_t includeId) const;

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

  /// Compute the logical location at `locationOffset` from the latest eligible
  /// producer-backed line-control event ending no later than `eventEndLimit`.
  std::optional<LineDirectiveLocation> ProducerBackedLineControlLocationAt(
      llvm::StringRef ownerBytes, llvm::StringRef ownerFile,
      std::optional<uint64_t> ownerIncludeId, uint64_t eventEndLimit,
      uint64_t locationOffset) const;

  /// Compute the logical location at an owner byte offset, preferring
  /// producer-proven line-control state when available.
  LineDirectiveLocation LogicalLocationAtOwnerOffset(
      llvm::StringRef ownerBytes, llvm::StringRef ownerFile,
      std::optional<uint64_t> ownerIncludeId, uint64_t offset) const;

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
};

} // namespace refold
} // namespace clang

#endif // LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDLINECONTROLPROOF_H
