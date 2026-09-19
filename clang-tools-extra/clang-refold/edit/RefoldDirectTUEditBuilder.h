//===--- RefoldDirectTUEditBuilder.h ----------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Construction of the direct translation-unit byte-span edit that realizes one
// token hunk over a TU byte span another proof has already accepted.
//
// RefoldTUAnchorProof proves where the source may be edited; this builder
// constructs the exact replacement that realizes B there; the text-edit
// assembler certifies it; RefoldEngine chooses whether to take this path.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDDIRECTTUEDITBUILDER_H
#define LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDDIRECTTUEDITBUILDER_H

#include "edit/RefoldEditTypes.h"
#include "proof/RefoldTheoremTypes.h"
#include "source/RefoldDiffTypes.h"
#include "clang/Basic/LangOptions.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>

namespace clang {
namespace refold {

class RefoldLineControlProof;
class RefoldLineObserverLayout;
class RefoldModel;
class RefoldPathIdentity;
class RefoldSourceMapper;
class RefoldTextEditAssembler;
class RefoldTUAnchorProof;
class RefoldTUEditPlanner;
struct PPTok;
struct PrintedPragmaCarrier;
struct PrintedPragmaInsertionPlacement;
struct SidebandPragmaEdit;
struct SidebandPragmaLinePairing;
struct TUInsertionAnchorAdjustment;

/// Builds the certified direct-TU byte-span edit for one token hunk.
///
/// Every dependency is borrowed and read only; the builder holds no state of
/// its own, so one instance serves every hunk of a refold run.
class RefoldDirectTUEditBuilder {
public:
  /// Borrowed per-run inputs and services.
  struct Dependencies {
    const RefoldModel &model;
    const RefoldPathIdentity &pathIdentity;
    const clang::LangOptions &lexLang;
    /// Original translation-unit source bytes.
    llvm::StringRef tuSourceBytes;
    /// Edited preprocessed bytes and their tokens.
    llvm::StringRef bSource;
    llvm::ArrayRef<PPTok> bToks;
    llvm::ArrayRef<size_t> bTokOff;
    const RefoldSourceMapper &sourceMapper;
    const RefoldTUAnchorProof &tuAnchorProof;
    const RefoldLineControlProof &lineControlProof;
    const RefoldTUEditPlanner &tuEditPlanner;
    const RefoldLineObserverLayout &lineObserverLayout;
    const RefoldTextEditAssembler &textEditAssembler;
    llvm::ArrayRef<SidebandPragmaEdit> sidebandPragmaEdits;
    llvm::ArrayRef<SidebandPragmaLinePairing> sidebandPragmaLinePairings;
    llvm::ArrayRef<PrintedPragmaCarrier> printedPragmaCarriers;
  };

  explicit RefoldDirectTUEditBuilder(Dependencies deps);

  /// Build the ordinary direct-TU byte-span edit realizing one token hunk over
  /// \p span, the TU byte range a direct-TU proof has already accepted.
  ///
  /// This is the realization half of the direct-TU path: the caller supplies a
  /// span that `RefoldTUAnchorProof::PlanTUByteSpan()` proved, and this turns
  /// it into replacement text -- B token slice, gap and spacing repair,
  /// trailing call-suffix extension, line-control resync -- and certifies the
  /// resulting edit.  Returns std::nullopt when the edit could not be
  /// certified, leaving the caller to escalate.
  ///
  /// It is separate from span planning so that a caller holding a *different*
  /// proved span for the same hunk can reuse the identical realization rather
  /// than restating it.  \p acceptedPath names which proof supplied the span:
  /// `TUByteSpanMappedEdit` when the hunk's tokens map to the TU, or
  /// `TUByteSpanConservativeEdit` for the unresolved-owner fallback.  Only a
  /// conservative edit over a whitespace-only span keeps the span's own bytes,
  /// and only a mapped insertion before a materialized include needs visible
  /// replay text to defer its resync.
  std::optional<TextEdit> Build(const diffutils::Hunk &h, size_t hunkIndex,
                                bool isDel, llvm::StringRef tuPath,
                                llvm::StringRef tuBytes,
                                std::pair<uint64_t, uint64_t> span,
                                AcceptedPathKind acceptedPath) const;

private:
  /// Place a pure TU insertion among the printed pragma lines preserved at
  /// its A gap; see `placeTUInsertionAmongPrintedPragmas`.  Non-insertions
  /// and gaps without such lines report `NotApplicable`.
  PrintedPragmaInsertionPlacement
  PlaceTUInsertionAmongPrintedPragmas(const diffutils::Hunk &h,
                                      uint64_t baseAnchor) const;

  /// Return the B bytes a pure insertion replays: its token envelope, cut
  /// before the first preserved pragma line B prints after it when
  /// \p placement placed it.
  llvm::StringRef
  InsertionEnvelope(const diffutils::Hunk &h,
                    const PrintedPragmaInsertionPlacement &placement) const;

  /// Return the typed anchor adjustment an insertion's final site carries,
  /// or std::nullopt when it still sits on its base anchor.
  static std::optional<TUInsertionAnchorAdjustment>
  InsertionAnchorAdjustment(const PrintedPragmaInsertionPlacement &placement,
                            bool advancedOverSourceLineControlPrefix,
                            uint64_t rawTUStart, uint64_t anchor);

  Dependencies deps_;
};

} // namespace refold
} // namespace clang

#endif // LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDDIRECTTUEDITBUILDER_H
