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

#include "edit/RefoldEditTypes.h"
#include "edit/RefoldPatchTypes.h"
#include "line-control/FinalLineControlModel.h"
#include "model/RefoldModel.h"
#include "model/RefoldToken.h"

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
class RefoldAcceptedCandidateBuilder;
class RefoldTextEditCertifier;

/// Concrete line-observer layout realization service.
///
/// The service owns edit construction for proven line-state observer demands.
/// It consumes proof answers from RefoldLineControlProof and owner-state
/// witness builders, then emits TU/include-local repairs through the text-edit
/// and include materialization vocabulary.
class RefoldLineObserverLayout {
public:
  /// Construct a layout realization service over immutable producer/model
  /// facts and the proof/edit services it composes.
  RefoldLineObserverLayout(
      const RefoldModel &model, llvm::StringRef bSource,
      llvm::ArrayRef<PPTok> aToks, llvm::ArrayRef<PPTok> bToks,
      llvm::ArrayRef<size_t> bTokOff, const std::vector<int64_t> &abTokMapA2B,
      const std::vector<int64_t> &abTokMapB2A, const RefoldPathIdentity &paths,
      const RefoldLineControlProof &lineControlProof,
      const RefoldOwnerStateProof &ownerStateProof,
      const RefoldAcceptedCandidateBuilder &acceptedCandidateBuilder,
      const RefoldTextEditCertifier &textEditCertifier,
      const LineDirectiveInserter &lineDirs)
      : model_(model), bSource_(bSource), aToks_(aToks), bToks_(bToks),
        bTokOff_(bTokOff), abTokMapA2B_(abTokMapA2B), abTokMapB2A_(abTokMapB2A),
        paths_(paths), lineControlProof_(lineControlProof),
        ownerStateProof_(ownerStateProof),
        acceptedCandidateBuilder_(acceptedCandidateBuilder),
        textEditCertifier_(textEditCertifier), lineDirs_(lineDirs) {}

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
  const RefoldAcceptedCandidateBuilder &acceptedCandidateBuilder_;
  const RefoldTextEditCertifier &textEditCertifier_;
  const LineDirectiveInserter &lineDirs_;
};

} // namespace refold
} // namespace clang

#endif // LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDLINEOBSERVERLAYOUT_H
