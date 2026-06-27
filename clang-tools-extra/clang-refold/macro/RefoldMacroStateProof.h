//===--- RefoldMacroStateProof.h ----------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Macro-state proof helpers.
//
// This service owns the read-only proof primitives for #define/#undef state
// motion and the materialized-header macro-state stabilization planner.  It is
// deliberately narrower than RefoldEngine: callers pass macro-state surfaces and
// already-staged edit intervals explicitly, while owner-state proof/audit remains
// behind the RefoldOwnerStateProof dependency.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDMACROSTATEPROOF_H
#define LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDMACROSTATEPROOF_H

#include "core/RefoldModel.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

namespace clang {
namespace refold {

class RefoldOwnerStateProof;
class RefoldPathIdentity;
class RefoldTokenTextAnalysis;

/// Macro-state observation mode for a moved or preserved #define/#undef.
///
/// Object-like definitions and #undef transitions are observed by any real
/// preprocessing identifier token with the macro name.  Function-like
/// definitions are observed only by NAME followed by `(` as preprocessing
/// tokens.
enum class MacroStateObservationKind {
  IdentifierToken,
  FunctionLikeInvocation,
};

/// Exact source-line interval for a producer-recorded macro-state directive.
///
/// MacroDirective::siteB is anchored at the macro name, not necessarily at the
/// beginning of the physical directive line.  This witness records the validated
/// full-line interval recovered from the recorded directive text.
struct MacroStateDirectiveLineInterval {
  const RefoldModel::MacroDirective *directive = nullptr;
  uint64_t begin = 0;
  uint64_t end = 0;
  llvm::StringRef name;
};

/// File-byte interval for the replacement list of the #define that created a
/// recorded macro invocation.
///
/// The interval is expressed both in directive-text coordinates and in source
/// file coordinates.  Source-neutral macro-gap proofs use this to tile only the
/// replacement-list body while still translating nested callsite byte ranges
/// back through the producer's macro-name source anchor.
struct MacroDefinitionReplacementListInterval {
  const RefoldModel::MacroDirective *directive = nullptr;
  size_t nameTextBegin = 0;
  size_t replacementTextBegin = 0;
  uint64_t fileBase = 0;
  uint64_t fileBegin = 0;
  uint64_t fileEnd = 0;
};

/// Minimal macro-patch surface needed by materialized-header macro-state proof.
///
/// The full MacroPatch carrier is still an edit/planning type.  Macro-state
/// proof only needs the physical invocation interval and B replacement text, so
/// callers adapt MacroPatch into this narrow input instead of giving this
/// service edit-assembly authority.
struct MacroStatePatchReplayInput {
  uint64_t invStart = 0;
  uint64_t invEnd = 0;
  llvm::StringRef replacement;
};

/// Half-open interval for an edit already staged in the materialized header.
///
/// Stabilization must not compose a moved #define line with an unrelated local
/// edit.  Passing only intervals keeps the proof independent of TextEdit.
struct MacroStateStagedEditInterval {
  uint64_t start = 0;
  uint64_t end = 0;
};

/// Replacement interval produced by include-owned macro-state replay
/// stabilization.
///
/// A materialized header macro patch may need to consume earlier #define lines
/// and the callsite line suffix, then re-emit those definitions after the
/// replacement payload so the B-surface replay sees the same macro environment
/// that produced it.
struct StabilizedMaterializedHeaderMacroPatch {
  uint64_t start = 0;
  uint64_t end = 0;
  std::string replacement;
};

/// Read-only macro-state proof service.
///
/// The service centralizes byte-exact directive recovery and raw-token text
/// observation checks.  It does not own edit construction, accepted-result
/// carriers, include recursion, or terminal fallback state.
class RefoldMacroStateProof {
public:
  /// Construct a proof service over producer macro-state and source-text facts.
  RefoldMacroStateProof(const RefoldModel &model,
                        const RefoldPathIdentity &paths,
                        const RefoldTokenTextAnalysis &tokenText,
                        const RefoldOwnerStateProof &ownerStateProof)
      : model_(model), paths_(paths), tokenText_(tokenText),
        ownerStateProof_(ownerStateProof) {}

  /// Classify how `directive` can be observed for `macroName`.
  MacroStateObservationKind MacroStateObservationKindForDirective(
      const RefoldModel::MacroDirective &directive,
      llvm::StringRef macroName) const;

  /// Return the first byte in `text` that observes `directive`, if any.
  ///
  /// `suffix` lets function-like NAME-at-end observations see a following `(`
  /// that lives just past the queried byte range.
  std::optional<size_t> FirstMacroStateObservationOffsetInText(
      const RefoldModel::MacroDirective &directive, llvm::StringRef macroName,
      llvm::StringRef text, llvm::StringRef suffix = llvm::StringRef()) const;

  /// Return whether replacement text would preprocess differently under
  /// `directive`; unnamed directives use `unprovenObserves` fail-closed policy.
  bool ReplacementObservesMacroStateDirective(
      const RefoldModel::MacroDirective &directive,
      llvm::StringRef replacement, bool unprovenObserves) const;

  /// Return whether moving `directive` across `chunk` could change semantics.
  ///
  /// `following` is considered only for function-like invocation observations
  /// split across the end of `chunk`.
  bool SourceChunkObservesMacroStateDirectiveWhenCrossed(
      const RefoldModel::MacroDirective &directive, llvm::StringRef macroName,
      llvm::StringRef chunk,
      llvm::StringRef following = llvm::StringRef()) const;

  /// Recover the validated physical source-line interval for `directive`.
  std::optional<MacroStateDirectiveLineInterval>
  RecoverMacroStateDirectiveLineInterval(
      const RefoldModel::MacroDirective &directive, llvm::StringRef expectedPath,
      llvm::StringRef fileBytes,
      std::optional<uint64_t> requiredOwnerIncludeId) const;

  /// Recover the source interval of the #define replacement list used by
  /// `invocation`.
  std::optional<MacroDefinitionReplacementListInterval>
  RecoverMacroDefinitionReplacementListInterval(
      const RefoldModel::MacroInvocation &invocation) const;

  /// Widen and reorder a materialized-header macro patch when active #define
  /// lines must be carried after the B replacement to preserve replay state.
  std::optional<StabilizedMaterializedHeaderMacroPatch>
  StabilizeMaterializedHeaderMacroPatchReplay(
      MacroStatePatchReplayInput patch, uint64_t patchEnd,
      llvm::StringRef headerPath, uint64_t includeId,
      llvm::StringRef materializedHeaderBytes,
      llvm::ArrayRef<MacroStateStagedEditInterval> stagedEdits) const;

private:
  const RefoldModel &model_;
  const RefoldPathIdentity &paths_;
  const RefoldTokenTextAnalysis &tokenText_;
  const RefoldOwnerStateProof &ownerStateProof_;
};

} // namespace refold
} // namespace clang

#endif // LLVM_CLANG_TOOLS_EXTRA_CLANG_REFOLD_REFOLDMACROSTATEPROOF_H
