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
// deliberately narrower than RefoldEngine: callers pass macro-state surfaces
// and already-staged edit intervals explicitly, while owner-state proof/audit
// remains behind the RefoldOwnerStateProof dependency.
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
#include <vector>

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

/// One macro name whose binding a preserved directive changes.
///
/// A `#define`/`#undef` carries its producer record in `directive`, which fixes
/// the observation mode exactly: an object-like definition is observed by the
/// bare name, a function-like one only by NAME followed by `(`.  A
/// `#pragma push_macro("M")` or `pop_macro("M")` names a macro without
/// recording the shape of either definition it swaps between, so it leaves
/// `directive` null and takes the conservative identifier-token mode.
struct MacroStateBinding {
  llvm::StringRef name;
  const RefoldModel::MacroDirective *directive = nullptr;
};

/// Exact source-line interval for a producer-recorded macro-state directive.
///
/// MacroDirective::siteB is anchored at the macro name, not necessarily at the
/// beginning of the physical directive line, and it stops at the first physical
/// newline.  This witness records the validated interval for the whole
/// directive spelling: the producer-recorded physical extent when the map
/// carries it, and otherwise the interval recovered from the recorded directive
/// text.
///
/// `[begin,end)` spans the directive from its `#` through the end of its
/// logical line, so it may cover several physical lines when the spelling is
/// backslash-continued.  It is not in general `directive->text.size()` bytes
/// long: the recorded text is a canonical rendering of the parsed definition,
/// not a slice of the source.
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

/// Replacement interval produced by replay stabilization.
///
/// The interval may be wider than the original macro invocation because it can
/// consume earlier #define lines and the invocation's physical-line suffix,
/// then re-emit the carried definitions after the replacement payload.
struct StabilizedMaterializedHeaderMacroPatch {
  uint64_t start = 0;
  uint64_t end = 0;
  std::string replacement;

  /// Exact producer-bound directives moved by the stabilization proof.
  ///
  /// Consumers must authorize only these transitions.  The widened edit range
  /// may contain unrelated preprocessing structure, so range-wide macro-state
  /// authority would exceed the theorem proved by the stabilizer.
  std::vector<MacroStateDirectiveLineInterval> movedTransitions;
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

  /// Return the observation mode for \p directive when it controls \p
  /// macroName.
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

  /// True iff \p replacement can observe a macro-state directive if that
  /// directive is active before the replacement payload.  Malformed producer
  /// proof data is handled by \p unprovenObserves so callers can remain
  /// fail-closed in their own proof domain.
  bool ReplacementObservesMacroStateDirective(
      const RefoldModel::MacroDirective &directive, llvm::StringRef replacement,
      bool unprovenObserves) const;

  /// True iff re-preprocessing \p payload could observe a change to the macro
  /// definitions \p bindings bind.
  ///
  /// This is the placement question for material whose side of a preserved
  /// macro-state directive the alignment does not fix: when the answer is
  /// false, both sides re-preprocess to the same tokens and either is a
  /// realization of the same edit.
  ///
  /// Two obligations are discharged, and neither alone is enough.
  ///
  /// * The payload must not observe any bound name directly, decided per
  ///   binding in that binding's own observation mode.  A binding whose record
  ///   decided that by function-likeness additionally requires every recorded
  ///   `#define` for the name to agree on the shape, since otherwise the mode
  ///   depends on which definition is live -- which is what crossing the
  ///   directive changes.
  /// * No identifier in the payload may *expand* to a bound name.  A payload
  ///   identifier that is itself a live macro does not stand for itself, and
  ///   its replacement list can name a bound macro the payload never spells, so
  ///   the transitive closure over recorded replacement lists must reach none
  ///   of them.  The producer records every `#define` the preprocessor saw, so
  ///   an identifier absent from that index cannot be live.
  ///
  /// An empty binding list reports observed: a placement question is asked only
  /// because something was preserved, so no bindings means the names were never
  /// recovered rather than that nothing is bound.
  bool PayloadObservesMacroStateBindings(
      llvm::ArrayRef<MacroStateBinding> bindings,
      llvm::StringRef payload) const;

  /// Return whether \p directive defines an object-like macro whose whole
  /// replacement list is its own name, as in `#define stderr stderr`.
  ///
  /// The idiom is common in system headers -- glibc spells `stdin`, `stdout`
  /// and `stderr` this way so they are macros as C89/C99 require.  Expanding
  /// such a macro is the identity on the token: the name is not re-expanded
  /// during its own replacement, so the resulting token stream equals the
  /// original one.
  bool MacroDefinitionIsSelfReferentialIdentity(
      const RefoldModel::MacroDirective &directive) const;

  /// Return whether making \p directive active before \p text is provably
  /// unobservable *because* the definition expands to itself.
  ///
  /// An ordinary expansion of a self-referential identity macro cannot change
  /// the token stream, so a payload that merely names the macro is unaffected.
  /// A conditional test is a different matter: `#ifdef X` or `defined(X)` flips
  /// from false to true, and that is a real difference.  Such a test can only
  /// be spelled on a preprocessing-directive logical line, so this admits the
  /// payload exactly when no directive line in it names the macro.
  ///
  /// The directive inventory comes from the shared preprocessing-directive
  /// scanner, which understands logical lines, splices, comments and literals.
  /// An incomplete scan proves nothing and reports observable, so a payload the
  /// scanner cannot fully cover keeps failing closed.
  bool SelfReferentialDefinitionIsUnobservableInText(
      const RefoldModel::MacroDirective &directive, llvm::StringRef macroName,
      llvm::StringRef text) const;

  /// Return whether moving `directive` across `chunk` could change semantics.
  ///
  /// `following` is considered only for function-like invocation observations
  /// split across the end of `chunk`.
  bool SourceChunkObservesMacroStateDirectiveWhenCrossed(
      const RefoldModel::MacroDirective &directive, llvm::StringRef macroName,
      llvm::StringRef chunk,
      llvm::StringRef following = llvm::StringRef()) const;

  /// Recover the complete physical source extent of a recorded #define/#undef
  /// directive in \p fileBytes.
  ///
  /// The helper is the single owner for the repeated proof used by TU carry,
  /// header materialization, include edits, expansion fallback, and replay
  /// stability: the directive must match \p expectedPath, match the requested
  /// include-owner instance, and have a producer-recorded macro name.
  ///
  /// The extent itself comes from whichever evidence the map carries.  A
  /// producer-recorded physical extent is returned as the interval directly,
  /// bounds-checked against \p fileBytes.  Without one, the interval is
  /// reconstructed from the macro-name anchor and admitted only when those file
  /// bytes exactly equal MacroDirective::text, which restricts the legacy path
  /// to directives already spelled the way Clang renders them.
  std::optional<MacroStateDirectiveLineInterval>
  RecoverMacroStateDirectiveLineInterval(
      const RefoldModel::MacroDirective &directive,
      llvm::StringRef expectedPath, llvm::StringRef fileBytes,
      std::optional<uint64_t> requiredOwnerIncludeId) const;

  /// Recover the source interval for the replacement list of the #define used
  /// by \p invocation, if the defining directive and invocation shape are
  /// producer-proven and byte-coordinate translation is well-formed.
  std::optional<MacroDefinitionReplacementListInterval>
  RecoverMacroDefinitionReplacementListInterval(
      const RefoldModel::MacroInvocation &invocation) const;

  /// Stabilize an include-owned macro patch against the macro state at its
  /// final replay position.
  ///
  /// Return value protocol:
  /// * std::nullopt: no active header definition is observed by the B text, so
  ///   the original patch can be staged unchanged;
  /// * non-empty StabilizedMaterializedHeaderMacroPatch: stage the widened,
  ///   replay-stable repair interval returned here; and
  /// * empty {0,0,""}: terminal fallback was requested and materialization of
  ///   this include must stop.
  ///
  /// This keeps candidate selection deterministic: the function either proves a
  /// concrete source rewrite that restores B's macro context, proves that no
  /// rewrite is needed, or records the missing proof obligation.
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
