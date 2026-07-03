//===--- RefoldLineObserverLayout.h ----------------------------*- C++ -*-===//
//
// Line-observer layout realization for clang-refold.
//
// RefoldLineObserverLayout owns the concrete source-layout repairs required
// when preserved line-state observers such as `__LINE__` would otherwise see a
// different physical/logical line after refolding.  It is deliberately separate
// from RefoldLineControlProof: the proof service answers whether line-control
// demand exists, while this service emits the TU/include edits and synthetic
// wrappers that realize those proven demands.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDLINEOBSERVERLAYOUT_H
#define LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDLINEOBSERVERLAYOUT_H

#include "core/RefoldModel.h"
#include "edit/RefoldEditTypes.h"
#include "edit/RefoldPatchTypes.h"
#include "line-control/FinalLineControlModel.h"
#include "source/RefoldToken.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/StringRef.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace clang {
namespace refold {

class LineDirectiveInserter;
class RefoldLineControlProof;
class RefoldOwnerStateProof;
class RefoldPathIdentity;
class RefoldProofLattice;
class RefoldTextEditAssembler;

/// Concrete line-observer layout realization service.
///
/// The service owns edit construction for proven line-state observer demands.
/// It consumes proof answers from RefoldLineControlProof and owner-state
/// witness builders, then emits TU/include-local repairs through the text-edit
/// and include materialization vocabulary.
class RefoldLineObserverLayout {
public:
  /// Return true iff a raw source span starts with the recorded macro callsite
  /// spelling.  This is a token-boundary safety check for source-preserving
  /// macro rewrite paths; it is static because it depends only on the source
  /// slice and producer macro metadata.
  static bool
  InvocationSpanMatchesCallsitePrefix(llvm::StringRef invSpanText,
                                      const RefoldModel::MacroInvocation &m);

  /// Construct a layout realization service over immutable producer/model
  /// facts and the proof/edit services it composes.
  RefoldLineObserverLayout(const RefoldModel &model, llvm::StringRef bSource,
                           llvm::ArrayRef<PPTok> aToks,
                           llvm::ArrayRef<PPTok> bToks,
                           llvm::ArrayRef<size_t> bTokOff,
                           const std::vector<int64_t> &abTokMapA2B,
                           const std::vector<int64_t> &abTokMapB2A,
                           const RefoldPathIdentity &paths,
                           const RefoldLineControlProof &lineControlProof,
                           const RefoldOwnerStateProof &ownerStateProof,
                           const RefoldProofLattice &proofLattice,
                           const RefoldTextEditAssembler &textEditAssembler,
                           const LineDirectiveInserter &lineDirs)
      : model_(model), bSource_(bSource), aToks_(aToks), bToks_(bToks),
        bTokOff_(bTokOff), abTokMapA2B_(abTokMapA2B), abTokMapB2A_(abTokMapB2A),
        paths_(paths), lineControlProof_(lineControlProof),
        ownerStateProof_(ownerStateProof), proofLattice_(proofLattice),
        textEditAssembler_(textEditAssembler), lineDirs_(lineDirs) {}

  /// Append TU-local materialization edits for preserved source-spelled
  /// `__LINE__` observers whose B-side layout cannot be represented by legal
  /// `#line` placement alone.
  bool AppendTURealizationEdits(llvm::StringRef tuPath, llvm::StringRef tuBytes,
                                std::vector<TextEdit> &tuEdits) const;

  /// Append include-local materialization patches for the same line-observer
  /// layout theorem, but in header-owner token coordinates.
  bool AppendIncludeRealizationEdits(
      llvm::DenseMap<uint64_t, IncludeEdits> &perInclude) const;

  /// Return true iff a return/resync repair emitted inside a conditional arm
  /// should instead be dominated by a post-join line-control repair.
  bool
  LineResyncShouldDeferToConditionalJoin(llvm::StringRef ownerFile,
                                         std::optional<uint64_t> ownerIncludeId,
                                         uint64_t resumeOffset) const;

  /// Wrap materialized include text with the entry/return `#line` directives
  /// proven necessary for child and parent line-state observers.
  LineControlWrappedText WrapIncludeExpansionForMaterialization(
      const RefoldModel::IncludeItem &child, llvm::StringRef parentFileSpelling,
      llvm::StringRef parentOwnerFileForDemand,
      std::optional<uint64_t> parentOwnerIncludeId, uint64_t parentResumeOffset,
      size_t childEntryLineNo, size_t parentResumeLineNo,
      llvm::StringRef childBody,
      llvm::ArrayRef<FinalLineControlPruneCandidate>
          childBodyLineControlCandidates,
      llvm::ArrayRef<FinalLineControlSourceMapping>
          childBodyLineControlSourceMappings,
      bool allowUnobservableLineDirectiveSuppression) const;

private:
  const RefoldOwnerStateProof &OwnerStateProof() const {
    return ownerStateProof_;
  }

  const RefoldModel &model_;
  llvm::StringRef bSource_;
  llvm::ArrayRef<PPTok> aToks_;
  llvm::ArrayRef<PPTok> bToks_;
  llvm::ArrayRef<size_t> bTokOff_;
  const std::vector<int64_t> &abTokMapA2B_;
  const std::vector<int64_t> &abTokMapB2A_;
  const RefoldPathIdentity &paths_;
  const RefoldLineControlProof &lineControlProof_;
  const RefoldOwnerStateProof &ownerStateProof_;
  const RefoldProofLattice &proofLattice_;
  const RefoldTextEditAssembler &textEditAssembler_;
  const LineDirectiveInserter &lineDirs_;
};

} // namespace refold
} // namespace clang

#endif // LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDLINEOBSERVERLAYOUT_H
